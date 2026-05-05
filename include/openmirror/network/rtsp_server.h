#pragma once

#include <openmirror/network/tcp_server.h>
#include <functional>
#include <map>
#include <string>

namespace openmirror::network {

// Simple RTSP server for AirPlay / Miracast control
// Handles RTSP methods: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, SET_PARAMETER, GET_PARAMETER

struct RtspRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    int cseq = 0;
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

private:
    void handle_client(socket_t client, const std::string& addr);
    RtspRequest parse_request(socket_t client);
    void send_response(socket_t client, const RtspRequest& req, const RtspResponse& resp);

    TcpServer tcp_;
    std::map<std::string, RtspHandler> handlers_;
    RtspHandler default_handler_;
};

} // namespace openmirror::network
