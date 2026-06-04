#include <opm/network/rtsp_server.h>

#include <opm/airplay/hap_session.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

namespace opm::network {

RtspServer::RtspServer() = default;
RtspServer::~RtspServer() { stop(); }

bool RtspServer::start(uint16_t port) {
    return tcp_.start(port, [this](socket_t client, const std::string& addr) {
        handle_client(client, addr);
    });
}

void RtspServer::stop() {
    tcp_.stop();
}

void RtspServer::on_method(const std::string& method, RtspHandler handler) {
    handlers_[method] = std::move(handler);
}

void RtspServer::set_default_handler(RtspHandler handler) {
    default_handler_ = std::move(handler);
}

void RtspServer::handle_client(socket_t client, const std::string& addr) {
    std::cout << "[RTSP] Session started with " << addr << "\n";

    // Track this socket so disconnect_ip() can close it.
    std::string ip = addr;
    auto colon = ip.find(':');
    if (colon != std::string::npos) ip = ip.substr(0, colon);
    {
        std::lock_guard lock(clients_mutex_);
        clients_by_ip_.emplace(ip, client);
    }

    ClientCtx ctx;
    ctx.sock = client;

    while (true) {
        RtspRequest req = parse_request(ctx);
        if (req.method.empty()) {
            std::cout << "[RTSP] Client disconnected: " << addr << "\n";
            break;
        }
        req.client_addr = addr;

        std::cout << "[RTSP] " << req.method << " " << req.uri
                  << " CSeq=" << req.cseq
                  << " ver=" << req.version
                  << (ctx.hap ? " [enc]" : "") << "\n";

        RtspResponse resp;

        auto it = handlers_.find(req.method);
        if (it != handlers_.end()) {
            resp = it->second(req);
        } else if (default_handler_) {
            resp = default_handler_(req);
        } else {
            resp.status_code = 405;
            resp.reason = "Method Not Allowed";
        }

        // Send the response plaintext (HAP M4) BEFORE we promote to encrypted,
        // so the response itself stays readable. After it goes out, install
        // the session keys for all subsequent traffic on this socket.
        bool wants_promote =
            !ctx.hap &&
            resp.promote_hap_read_key.size()  == 32 &&
            resp.promote_hap_write_key.size() == 32;

        send_response(ctx, req, resp);

        if (wants_promote) {
            ctx.hap = std::make_unique<opm::airplay::HapSession>(
                resp.promote_hap_read_key, resp.promote_hap_write_key);
            std::cout << "[RTSP] Socket promoted to HAP encrypted framing ("
                      << addr << ")\n";
        }

        if (req.method == "TEARDOWN") {
            break;
        }
    }

    {
        std::lock_guard lock(clients_mutex_);
        auto range = clients_by_ip_.equal_range(ip);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == client) {
                clients_by_ip_.erase(it);
                break;
            }
        }
    }
    TcpServer::close_socket(client);
}

void RtspServer::disconnect_ip(const std::string& ip) {
    std::vector<socket_t> to_close;
    {
        std::lock_guard lock(clients_mutex_);
        auto range = clients_by_ip_.equal_range(ip);
        for (auto it = range.first; it != range.second; ++it)
            to_close.push_back(it->second);
    }
    for (socket_t s : to_close) {
        std::cout << "[RTSP] Forcing disconnect of client " << ip << "\n";
        TcpServer::close_socket(s);
    }
}

RtspRequest RtspServer::parse_request(ClientCtx& ctx) {
    RtspRequest req;

    // Read request line: METHOD URI RTSP/1.0
    std::string request_line = recv_line_ctx(ctx);
    if (request_line.empty()) return req;

    std::istringstream iss(request_line);
    iss >> req.method >> req.uri >> req.version;

    // Read headers
    while (true) {
        std::string line = recv_line_ctx(ctx);
        if (line.empty()) break; // Empty line = end of headers

        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim leading whitespace from value
            while (!val.empty() && val[0] == ' ') val.erase(0, 1);
            req.headers[key] = val;

            if (key == "CSeq") {
                req.cseq = std::stoi(val);
            }
        }
    }

    // Read body if Content-Length is present
    auto cl_it = req.headers.find("Content-Length");
    if (cl_it != req.headers.end()) {
        int content_len = std::stoi(cl_it->second);
        if (content_len > 0 && content_len < 1024 * 1024) { // max 1MB
            req.body.resize(content_len);
            if (!recv_exact_ctx(ctx, req.body.data(), content_len)) {
                // Partial read = treat as disconnect.
                req.method.clear();
                return req;
            }
        }
    }

    return req;
}

void RtspServer::send_response(ClientCtx& ctx, const RtspRequest& req,
                                const RtspResponse& resp) {
    std::ostringstream oss;
    // Always respond with RTSP/1.0 regardless of what the client sent.
    // The reference ap2-receiver does this: parse_request() replaces RTSP
    // with HTTP for Python's parser, then forces protocol_version="RTSP/1.0"
    // for ALL responses. Modern iPadOS sends HTTP/1.1 for pair-setup but
    // expects RTSP/1.0 back (AirPlay is RTSP-based).
    oss << "RTSP/1.0 " << resp.status_code << " " << resp.reason << "\r\n";
    oss << "CSeq: " << req.cseq << "\r\n";
    oss << "Server: AirTunes/220.68\r\n";

    for (const auto& [key, val] : resp.headers) {
        oss << key << ": " << val << "\r\n";
    }

    if (!resp.body.empty()) {
        oss << "Content-Length: " << resp.body.size() << "\r\n";
    }

    oss << "\r\n";

    std::string hdr_str = oss.str();

    // Diagnostic: log full response for pair-setup
    if (req.uri.find("/pair-setup") != std::string::npos ||
        req.uri.find("/pair-verify") != std::string::npos) {
        std::cerr << "[RTSP] >>> response to " << req.uri << " (" << hdr_str.size()
                  << "B hdr + " << resp.body.size() << "B body):\n"
                  << hdr_str;
    }

    send_string_ctx(ctx, hdr_str);

    if (!resp.body.empty()) {
        send_bytes_ctx(ctx, resp.body.data(), resp.body.size());
    }
}

// ----------------------------------------------------------------------------
// Per-client session-aware I/O. Plaintext path is a trivial pass-through to
// the static TcpServer helpers; encrypted path goes through HapSession with
// a plaintext buffer that absorbs whole frames.
// ----------------------------------------------------------------------------

bool RtspServer::pump_one_frame(ClientCtx& ctx) {
    if (!ctx.hap) return false;
    uint8_t hdr[2];
    if (TcpServer::recv_exact(ctx.sock, hdr, 2) != 2) return false;
    size_t plain_len = static_cast<size_t>(hdr[0]) |
                       (static_cast<size_t>(hdr[1]) << 8);
    if (plain_len == 0 || plain_len > opm::airplay::HapSession::kMaxFrame) {
        std::cerr << "[RTSP] HAP frame length out of range: " << plain_len << "\n";
        return false;
    }
    std::vector<uint8_t> wire(2 + plain_len + 16);
    wire[0] = hdr[0];
    wire[1] = hdr[1];
    if (TcpServer::recv_exact(ctx.sock, wire.data() + 2,
                              static_cast<int>(plain_len + 16))
        != static_cast<int>(plain_len + 16)) {
        return false;
    }
    auto pt = ctx.hap->decrypt_frame(wire);
    if (pt.empty()) {
        std::cerr << "[RTSP] HAP frame auth failed; tearing down session\n";
        return false;
    }
    // Compact any consumed bytes, then append the new plaintext.
    if (ctx.plain_off > 0) {
        ctx.plain_buf.erase(ctx.plain_buf.begin(),
                            ctx.plain_buf.begin() + ctx.plain_off);
        ctx.plain_off = 0;
    }
    ctx.plain_buf.insert(ctx.plain_buf.end(), pt.begin(), pt.end());
    return true;
}

bool RtspServer::recv_exact_ctx(ClientCtx& ctx, uint8_t* buf, int len) {
    if (!ctx.hap) {
        return TcpServer::recv_exact(ctx.sock, buf, len) == len;
    }
    int remaining = len;
    while (remaining > 0) {
        size_t avail = ctx.plain_buf.size() - ctx.plain_off;
        if (avail == 0) {
            if (!pump_one_frame(ctx)) return false;
            continue;
        }
        size_t take = std::min<size_t>(avail, static_cast<size_t>(remaining));
        std::memcpy(buf, ctx.plain_buf.data() + ctx.plain_off, take);
        buf       += take;
        ctx.plain_off += take;
        remaining -= static_cast<int>(take);
    }
    return true;
}

std::string RtspServer::recv_line_ctx(ClientCtx& ctx) {
    if (!ctx.hap) {
        return TcpServer::recv_line(ctx.sock);
    }
    std::string line;
    while (true) {
        // Need at least one byte to scan for \n.
        if (ctx.plain_off >= ctx.plain_buf.size()) {
            if (!pump_one_frame(ctx)) {
                return {};
            }
        }
        // Scan available plaintext for '\n'.
        size_t start = ctx.plain_off;
        size_t end   = ctx.plain_buf.size();
        size_t i = start;
        while (i < end && ctx.plain_buf[i] != '\n') ++i;
        line.append(reinterpret_cast<const char*>(ctx.plain_buf.data() + start),
                    i - start);
        if (i < end) {
            ctx.plain_off = i + 1;
            // Trim trailing \r.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        ctx.plain_off = end;
        // Need more bytes.
    }
}

bool RtspServer::send_bytes_ctx(ClientCtx& ctx, const uint8_t* buf, size_t len) {
    if (!ctx.hap) {
        return TcpServer::send_all(ctx.sock, buf, static_cast<int>(len));
    }
    // Fragment into HAP frames (max kMaxFrame plaintext per frame).
    size_t off = 0;
    while (off < len) {
        size_t chunk = std::min<size_t>(
            len - off, opm::airplay::HapSession::kMaxFrame);
        auto wire = ctx.hap->encrypt_frame(buf + off, chunk);
        if (wire.empty()) {
            std::cerr << "[RTSP] HAP frame encrypt failed\n";
            return false;
        }
        if (!TcpServer::send_all(ctx.sock, wire.data(),
                                 static_cast<int>(wire.size()))) {
            return false;
        }
        off += chunk;
    }
    return true;
}

} // namespace opm::network
