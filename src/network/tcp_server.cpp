#include <opm/network/tcp_server.h>
#include <iostream>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace opm::network {

TcpServer::TcpServer() = default;

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::init_winsock() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[TCP] WSAStartup failed\n";
    }
#endif
}

void TcpServer::cleanup_winsock() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void TcpServer::close_socket(socket_t sock) {
    if (sock == INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

bool TcpServer::start(uint16_t port, OnClientConnect on_connect) {
    on_connect_ = std::move(on_connect);

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCK) {
        std::cerr << "[TCP] Failed to create socket\n";
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        std::cerr << "[TCP] Bind failed on port " << port
                  << " (error " << WSAGetLastError() << ")\n";
#else
        std::cerr << "[TCP] Bind failed on port " << port << "\n";
#endif
        close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }

    if (listen(listen_sock_, 4) != 0) {
        std::cerr << "[TCP] Listen failed\n";
        close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&TcpServer::accept_loop, this);

    std::cout << "[TCP] Listening on port " << port << "\n";
    return true;
}

void TcpServer::stop() {
    running_.store(false);
    if (listen_sock_ != INVALID_SOCK) {
        close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Wake any client handler threads still blocked in recv() by shutting
    // down their sockets. We only shutdown (not close) here — the handler
    // owns the socket lifetime and will close it on exit. Then join all of
    // them so no detached thread can outlive this server (and the objects
    // it captures via `this` in its on_connect_ lambda).
    std::vector<std::unique_ptr<ClientCtx>> to_join;
    {
        std::lock_guard lk(clients_mutex_);
        to_join.swap(clients_);
    }
    for (auto& c : to_join) {
        if (c && c->sock != INVALID_SOCK) {
#ifdef _WIN32
            ::shutdown(c->sock, SD_BOTH);
#else
            ::shutdown(c->sock, SHUT_RDWR);
#endif
        }
    }
    for (auto& c : to_join) {
        if (c && c->th.joinable()) c->th.join();
    }
}

void TcpServer::reap_finished_clients_locked() {
    // Caller holds clients_mutex_. Joins and removes any handler threads
    // that have already finished (signalled via the per-client `done` flag).
    auto it = clients_.begin();
    while (it != clients_.end()) {
        auto& c = *it;
        if (c && c->done.load()) {
            if (c->th.joinable()) c->th.join();
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void TcpServer::accept_loop() {
    // Poll with select() so we always notice running_=false on stop().
    // Closing the listen socket from another thread on Windows does NOT
    // reliably wake a blocking accept() — and we hit exactly that hang:
    // shutdown reached rtsp_.stop() and never came back because the
    // accept thread sat forever in accept(). A short select() timeout
    // makes the loop self-tick without any platform tricks.
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (listen_sock_ == INVALID_SOCK) break;
        FD_SET(listen_sock_, &rfds);
        timeval tv{0, 250000}; // 250ms

        int sel = select(static_cast<int>(listen_sock_) + 1,
                         &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        if (!FD_ISSET(listen_sock_, &rfds)) continue;

        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);

        socket_t client = accept(listen_sock_,
            reinterpret_cast<sockaddr*>(&client_addr),
#ifdef _WIN32
            &addr_len
#else
            reinterpret_cast<socklen_t*>(&addr_len)
#endif
        );

        if (client == INVALID_SOCK) {
            if (running_.load()) {
                std::cerr << "[TCP] Accept failed\n";
            }
            continue;
        }

        // Format client address
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string addr = std::string(ip_str) + ":" + std::to_string(ntohs(client_addr.sin_port));

        std::cout << "[TCP] Client connected: " << addr << "\n";

        if (!on_connect_) {
            close_socket(client);
            continue;
        }

        auto ctx = std::make_unique<ClientCtx>();
        ctx->sock = client;
        ClientCtx* raw = ctx.get();
        ctx->th = std::thread([this, raw, addr]() {
            on_connect_(raw->sock, addr);
            raw->done.store(true);
        });

        std::lock_guard lk(clients_mutex_);
        // Reap any previously-finished client threads so the vector does
        // not grow unbounded over the lifetime of the server.
        reap_finished_clients_locked();
        clients_.push_back(std::move(ctx));
    }
}

int TcpServer::recv_exact(socket_t sock, uint8_t* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(sock, reinterpret_cast<char*>(buf + total), len - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}

std::string TcpServer::recv_line(socket_t sock) {
    std::string line;
    char ch;
    while (recv(sock, &ch, 1, 0) == 1) {
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        line += ch;
    }
    return line;
}

bool TcpServer::send_all(socket_t sock, const uint8_t* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, reinterpret_cast<const char*>(data + sent), len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool TcpServer::send_string(socket_t sock, const std::string& str) {
    return send_all(sock, reinterpret_cast<const uint8_t*>(str.data()),
                    static_cast<int>(str.size()));
}

} // namespace opm::network
