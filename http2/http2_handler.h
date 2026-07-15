#ifndef HTTPSERVER2_HTTP2_H
#define HTTPSERVER2_HTTP2_H

#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <nghttp2/nghttp2.h>
#include "../httpserver2.h"

namespace http2 { namespace http2_support {

// HTTP/2 会话
class Http2Session {
private:
    nghttp2_session* session_ = nullptr;
    std::function<int(const uint8_t*, size_t)> send_callback_;
    bool initialized_ = false;

public:
    Http2Session() = default;
    ~Http2Session();
    
    // 初始化 HTTP/2 会话（服务器端）
    bool initServer();
    
    // 处理接收到的数据
    int processData(const uint8_t* data, size_t length);
    
    // 发送数据（由回调调用）
    int sendData(const uint8_t* data, size_t length);
    
    // 发送数据回调
    void setSendCallback(std::function<int(const uint8_t*, size_t)> cb) {
        send_callback_ = std::move(cb);
    }
    
    // 发送响应
    int submitResponse(int32_t stream_id, 
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body);
    
    // 发送 GOAWAY
    int submitGoaway();
    
    // 获取 nghttp2 会话
    nghttp2_session* native() const { return session_; }
    
    bool isInitialized() const { return initialized_; }
};

// HTTP/2 连接处理器
class Http2Handler {
public:
    static void handleConnection(tcp::socket socket);
    static void setupAlpn(ssl::context& ctx);
    static bool isAvailable();
};

}} // namespace http2::http2_support

#endif // HTTPSERVER2_HTTP2_H