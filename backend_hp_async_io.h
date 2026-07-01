// backend_hp_async_io.h — hp-async-io / POSIX raw socket 后端
//
// 由于当前环境 clang 10 的 C++20 标准库不完整
// (<span>/coroutines 缺失)，改用 POSIX raw socket 实现 I/O 操作。
// 语义上属于 "hp-async-io 后端系列"（无 Boost 依赖），
// 可与 hp-async-io 库链接以扩展功能。

#pragma once

#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <future>
#include <memory>
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <cerrno>

namespace backend {

// ============================================================
// 类型定义
// ============================================================
struct Context {};

struct TCPSocket {
    int fd = -1;
    TCPSocket(Context&) {}
    ~TCPSocket() { if (fd >= 0) ::close(fd); }
    TCPSocket(TCPSocket&& other) noexcept : fd(other.fd) { other.fd = -1; }
    TCPSocket& operator=(TCPSocket&& other) noexcept {
        if (fd >= 0) ::close(fd);
        fd = other.fd; other.fd = -1;
        return *this;
    }
    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;
};

// ============================================================
// DNS 解析
// ============================================================
static bool resolve_host(const std::string& host, int port, struct sockaddr_in& addr) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int ret = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (ret != 0 || !res) return false;
    addr = *reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    addr.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

// ============================================================
// 连接
// ============================================================
bool connect(TCPSocket& s, const std::string& host, int port, int timeout_sec = 10) {
    if (s.fd >= 0) { ::close(s.fd); s.fd = -1; }

    struct sockaddr_in addr{};
    if (!resolve_host(host, port, addr)) return false;

    s.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s.fd < 0) return false;

    // Set non-blocking for timeout support
    int flags = fcntl(s.fd, F_GETFL, 0);
    fcntl(s.fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(s.fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == 0) {
        fcntl(s.fd, F_SETFL, flags); // restore blocking
        return true;
    }
    if (errno != EINPROGRESS) {
        ::close(s.fd); s.fd = -1;
        return false;
    }

    // Wait for connection with timeout
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s.fd, &wset);
    struct timeval tv{};
    tv.tv_sec = timeout_sec;
    rc = select(s.fd + 1, nullptr, &wset, nullptr, &tv);
    if (rc <= 0) {
        ::close(s.fd); s.fd = -1;
        return false;
    }

    // Check if connect succeeded
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
        ::close(s.fd); s.fd = -1;
        return false;
    }

    fcntl(s.fd, F_SETFL, flags); // restore blocking
    return true;
}

// ============================================================
// 发送
// ============================================================
bool send(TCPSocket& s, const std::string& data) {
    if (s.fd < 0) return false;
    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t n = ::write(s.fd, ptr, remaining);
        if (n <= 0) return false;
        ptr += n;
        remaining -= n;
    }
    return true;
}

// ============================================================
// HTTP 响应
// ============================================================
struct Response {
    bool success = false;
    int status_code = 0;
    size_t body_size = 0;
    bool keep_alive = false;
};

Response recv_http(TCPSocket& s) {
    Response r;
    if (s.fd < 0) return r;

    try {
        std::string raw;
        char buf[4096];
        bool header_done = false;
        size_t content_length = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (std::chrono::steady_clock::now() < deadline) {
            // Set socket timeout for each read
            struct timeval tv{};
            tv.tv_sec = 1;
            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(s.fd, &rset);
            int rc = select(s.fd + 1, &rset, nullptr, nullptr, &tv);
            if (rc <= 0) break;

            ssize_t n = ::read(s.fd, buf, sizeof(buf));
            if (n <= 0) break;

            raw.append(buf, n);

            if (!header_done) {
                auto hdr_end = raw.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    header_done = true;

                    // Parse status line
                    auto sp1 = raw.find(' ');
                    auto sp2 = raw.find(' ', sp1 + 1);
                    if (sp1 != std::string::npos && sp2 != std::string::npos) {
                        r.status_code = std::stoi(raw.substr(sp1 + 1, sp2 - sp1 - 1));
                    }

                    // Parse Content-Length
                    auto cl = raw.find("Content-Length:");
                    if (cl == std::string::npos) cl = raw.find("content-length:");
                    if (cl != std::string::npos) {
                        auto vs = raw.find_first_of("0123456789", cl + 15);
                        if (vs != std::string::npos) {
                            content_length = std::stoul(raw.substr(vs));
                        }
                    }

                    // Parse Connection
                    auto conn = raw.find("Connection:");
                    if (conn == std::string::npos) conn = raw.find("connection:");
                    if (conn != std::string::npos) {
                        auto v = raw.substr(conn + 11);
                        r.keep_alive = (v.find("keep-alive") != std::string::npos ||
                                        v.find("Keep-Alive") != std::string::npos);
                    }

                    // Check if body already fully received
                    size_t body_received = raw.size() - (hdr_end + 4);
                    if (body_received >= content_length) break;
                }
            } else {
                // Body receiving
                auto hdr_end = raw.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    size_t body_received = raw.size() - (hdr_end + 4);
                    if (body_received >= content_length) break;
                }
            }
        }

        r.success = header_done;
        r.body_size = content_length;
    } catch (...) {}
    return r;
}

// ============================================================
// 关闭
// ============================================================
void close(TCPSocket& s) {
    if (s.fd >= 0) {
        ::shutdown(s.fd, SHUT_RDWR);
        ::close(s.fd);
        s.fd = -1;
    }
}

} // namespace backend
