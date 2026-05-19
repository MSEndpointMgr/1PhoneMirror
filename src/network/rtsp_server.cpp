#include <opm/network/rtsp_server.h>
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

    while (true) {
        RtspRequest req = parse_request(client);
        if (req.method.empty()) {
            std::cout << "[RTSP] Client disconnected: " << addr << "\n";
            break;
        }
        req.client_addr = addr;

        std::cout << "[RTSP] " << req.method << " " << req.uri
                  << " CSeq=" << req.cseq << "\n";

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

        send_response(client, req, resp);

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

RtspRequest RtspServer::parse_request(socket_t client) {
    RtspRequest req;

    // Read request line: METHOD URI RTSP/1.0
    std::string request_line = TcpServer::recv_line(client);
    if (request_line.empty()) return req;

    std::istringstream iss(request_line);
    iss >> req.method >> req.uri >> req.version;

    // Read headers
    while (true) {
        std::string line = TcpServer::recv_line(client);
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
            TcpServer::recv_exact(client, req.body.data(), content_len);
        }
    }

    return req;
}

void RtspServer::send_response(socket_t client, const RtspRequest& req,
                                const RtspResponse& resp) {
    std::ostringstream oss;
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

    std::string header_str = oss.str();
    TcpServer::send_string(client, header_str);

    if (!resp.body.empty()) {
        TcpServer::send_all(client, resp.body.data(),
                            static_cast<int>(resp.body.size()));
    }
}

} // namespace opm::network
