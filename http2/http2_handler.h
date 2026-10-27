#ifndef HTTPSERVER2_HTTP2_H
#define HTTPSERVER2_HTTP2_H

#include <memory>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
// Windows 没有 ssize_t，而 nghttp2 的旧版回调 typedef（nghttp2_send_callback 等）
// 用的就是 ssize_t。UCRT 的 <BaseTsd.h> 提供 SSIZE_T（即 LONG_PTR，与 64 位
// 平台的 ssize_t 同宽），补一个兼容别名即可，不必改动 nghttp2 头文件本身。
#  include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#include <nghttp2/nghttp2.h>
#include "../httpserver2.h"

namespace http2 { namespace http2_support {

// HTTP/2 会话
class Http2Session {
private:
    nghttp2_session* session_ = nullptr;
    // 用 ssize_t 而非 int：nghttp2 的 send callback 约定返回写入字节数，
    // 负值表示错误（NGHTTP2_ERR_CALLBACK_FAILURE）。用 int 会在返回值
    // 超过 2GB 时溢出，且负值语义与 nghttp2 的约定对不上。
    std::function<ssize_t(const uint8_t*, size_t)> send_callback_;
    bool initialized_ = false;

    // 待发送的响应体。
    // nghttp2 的 nghttp2_submit_data 通过 data_provider 回调按需拉取数据，
    // 没有「传一段 buffer 就发完」的简化接口，所以 body 必须由会话持有，
    // 在 read_callback 里分片交出。
    std::string pending_body_;
    size_t pending_body_pos_ = 0;

    // 从 pending_body_ 中读取下一片数据供 nghttp2 发送
    static ssize_t s_read_body_callback(nghttp2_session* session,
                                         int32_t stream_id,
                                         uint8_t* buf, size_t length,
                                         uint32_t* data_flags,
                                         nghttp2_data_source* source,
                                         void* user_data);

public:
    Http2Session() = default;
    ~Http2Session();

    // 初始化 HTTP/2 会话（服务器端）
    bool initServer();

    // 处理接收到的数据
    // 返回 nghttp2_session_mem_recv 的原始结果：已消费的字节数（ssize_t），
    // 负值表示错误码。用 int 会在单次接收超过 2GB 时截断。
    ssize_t processData(const uint8_t* data, size_t length);

    // 发送数据（由回调调用）
    ssize_t sendData(const uint8_t* data, size_t length);

    // 发送数据回调
    void setSendCallback(std::function<ssize_t(const uint8_t*, size_t)> cb) {
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