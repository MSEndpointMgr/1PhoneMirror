#pragma once

#include <opm/network/tcp_server.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace opm::airplay { class HapSession; }

namespace opm::network {

// Simple RTSP server for AirPlay / Miracast control
// Handles RTSP methods: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, SET_PARAMETER, GET_PARAMETER

struct RtspRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    int cseq = 0;
    std::string client_addr; // "ip:port" of the RTSP client
};

struct RtspResponse {
    int status_code = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    // HAP session promotion: if both keys are 32B, RtspServer wraps this
    // socket in ChaCha20-Poly1305 framing immediately AFTER this response
    // is sent (the response itself goes out plaintext). Set by the HAP
    // /pair-verify M4 handler.
    std::vector<uint8_t> promote_hap_read_key;
    std::vector<uint8_t> promote_hap_write_key;
};

using RtspHandler = std::function<RtspResponse(const RtspRequest& req)>;

class RtspServer {
public:
    RtspServer();
    ~RtspServer();

    bool start(uint16_t port);
    void stop();

    // Register handler for specific RTSP method
    void on_method(const std::string& method, RtspHandler handler);

    // Set a catch-all handler
    void set_default_handler(RtspHandler handler);

    // Force-close all RTSP control sockets connected from `ip` (host part).
    void disconnect_ip(const std::string& ip);

private:
    // Per-client read state. Owns the optional HAP encrypted session plus a
    // plaintext queue that absorbs whole decrypted HAP frames so line/exact
    // readers can pull arbitrary byte counts across frame boundaries.
    struct ClientCtx {
        socket_t sock = INVALID_SOCK;
        std::unique_ptr<opm::airplay::HapSession> hap;
        std::vector<uint8_t> plain_buf;
        size_t               plain_off = 0;
    };

    void handle_client(socket_t client, const std::string& addr);
    RtspRequest parse_request(ClientCtx& ctx);
    void send_response(ClientCtx& ctx, const RtspRequest& req, const RtspResponse& resp);

    bool   recv_exact_ctx(ClientCtx& ctx, uint8_t* buf, int len);
    std::string recv_line_ctx(ClientCtx& ctx);
    bool   send_bytes_ctx(ClientCtx& ctx, const uint8_t* buf, size_t len);
    bool   send_string_ctx(ClientCtx& ctx, const std::string& s) {
        return send_bytes_ctx(ctx,
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    bool   pump_one_frame(ClientCtx& ctx);

    TcpServer tcp_;
    std::map<std::string, RtspHandler> handlers_;
    RtspHandler default_handler_;

    std::mutex clients_mutex_;
    std::multimap<std::string, socket_t> clients_by_ip_; // ip -> active socket
};

} // namespace opm::network
