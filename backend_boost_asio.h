// backend_boost_asio.h — Boost.Asio + Boost.Beast 后端实现

#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include <thread>
#include <iostream>
#include <sstream>
#include <memory>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

namespace backend {

// ============================================================
// 类型定义
// ============================================================
struct Context {
    net::io_context ioc;
};

struct TCPSocket {
    tcp::socket sock;
    TCPSocket(Context& ctx) : sock(ctx.ioc) {}
};

// ============================================================
// 连接
// ============================================================
bool connect(TCPSocket& s, const std::string& host, int port, int /* timeout_sec */ = 10) {
    try {
        tcp::resolver resolver(s.sock.get_executor());
        auto endpoints = resolver.resolve(host, std::to_string(port));
        net::connect(s.sock, endpoints);
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================
// 发送
// ============================================================
bool send(TCPSocket& s, const std::string& data) {
    try {
        net::write(s.sock, net::buffer(data));
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================
// 接收 HTTP 响应
// ============================================================
struct Response {
    bool success = false;
    int status_code = 0;
    size_t body_size = 0;
    bool keep_alive = false;
};

Response recv_http(TCPSocket& s) {
    Response r;
    try {
        beast::flat_buffer buf;
        http::response<http::string_body> response;
        http::read(s.sock, buf, response);

        r.success = true;
        r.status_code = response.result_int();
        r.body_size = response.body().size();
        r.keep_alive = response.keep_alive();
    } catch (...) {
        // r.success stays false
    }
    return r;
}

// ============================================================
// 关闭
// ============================================================
void close(TCPSocket& s) {
    try {
        boost::system::error_code ec;
        s.sock.shutdown(tcp::socket::shutdown_both, ec);
        s.sock.close(ec);
    } catch (...) {}
}

} // namespace backend
