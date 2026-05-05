#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
#endif

namespace openmirror::network {

// Callback when a client connects — receives client socket
using OnClientConnect = std::function<void(socket_t client_sock, const std::string& client_addr)>;

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    bool start(uint16_t port, OnClientConnect on_connect);
    void stop();
    bool is_running() const { return running_.load(); }

    static void init_winsock();
    static void cleanup_winsock();

    // Utility: read exactly n bytes
    static int recv_exact(socket_t sock, uint8_t* buf, int len);
    // Utility: read a line (up to \r\n)
    static std::string recv_line(socket_t sock);
    // Utility: send all bytes
    static bool send_all(socket_t sock, const uint8_t* data, int len);
    static bool send_string(socket_t sock, const std::string& str);
    static void close_socket(socket_t sock);

private:
    void accept_loop();

    socket_t listen_sock_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    OnClientConnect on_connect_;
};

} // namespace openmirror::network
