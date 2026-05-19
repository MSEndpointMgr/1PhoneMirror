#pragma once

#include <opm/network/tcp_server.h>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
    void handle_client(socket_t client, const std::string& addr);
    RtspRequest parse_request(socket_t client);
    void send_response(socket_t client, const RtspRequest& req, const RtspResponse& resp);

    TcpServer tcp_;
    std::map<std::string, RtspHandler> handlers_;
    RtspHandler default_handler_;

    std::mutex clients_mutex_;
    std::multimap<std::string, socket_t> clients_by_ip_; // ip -> active socket
};

} // namespace opm::network
