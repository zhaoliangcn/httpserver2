#include "http2_handler.h"
#include <nghttp2/nghttp2.h>
#include <vector>
#include <cstring>
// ALPN 设置与错误码读取需要 OpenSSL 的显式头文件。
// boost/asio/ssl.hpp 会间接引入 ssl.h，但 err.h 不在其中。
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../single_include/logger.h"

#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL

#define LOG_DEBUG(msg)    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())
#define LOG_INFO(msg)     log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())
#define LOG_WARNING(msg)  log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())
#define LOG_ERROR(msg)    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())

namespace http2 { namespace http2_support {

// ============================================================
// nghttp2 回调函数
// ============================================================
static ssize_t s_send_callback(nghttp2_session* /*session*/,
                               const uint8_t* data, size_t length,
                               int /*flags*/, void* user_data) {
    auto* handler = static_cast<Http2Session*>(user_data);
    return handler->sendData(data, length);
}

static int s_on_header_callback(nghttp2_session* /*session*/,
                                const nghttp2_frame* /*frame*/,
                                const uint8_t* /*name*/, size_t /*namelen*/,
                                const uint8_t* /*value*/, size_t /*valuelen*/,
                                uint8_t /*flags*/,
                                void* /*user_data*/) {
    return 0;
}

static int s_on_frame_recv_callback(nghttp2_session* /*session*/,
                                    const nghttp2_frame* /*frame*/,
                                    void* /*user_data*/) {
    return 0;
}

static int s_on_data_chunk_recv_callback(nghttp2_session* /*session*/,
                                         uint8_t /*flags*/, int32_t /*stream_id*/,
                                         const uint8_t* /*data*/, size_t /*len*/,
                                         void* /*user_data*/) {
    return 0;
}

static int s_on_stream_close_callback(nghttp2_session* /*session*/,
                                      int32_t /*stream_id*/,
                                      uint32_t /*error_code*/,
                                      void* /*user_data*/) {
    return 0;
}

// ============================================================
// Http2Session 实现
// ============================================================
Http2Session::~Http2Session() {
    if (session_) {
        nghttp2_session_del(session_);
    }
}

bool Http2Session::initServer() {
    if (session_) return true;
    
    nghttp2_session_callbacks* callbacks = nullptr;
    nghttp2_session_callbacks_new(&callbacks);
    if (!callbacks) {
        LOG_ERROR("Failed to create nghttp2 callbacks");
        return false;
    }
    
    nghttp2_session_callbacks_set_send_callback(callbacks, s_send_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, s_on_header_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, s_on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, s_on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, s_on_stream_close_callback);
    
    nghttp2_option* options = nullptr;
    nghttp2_option_new(&options);
    
    int rv = nghttp2_session_server_new2(&session_, callbacks, this, options);
    
    if (options) nghttp2_option_del(options);
    nghttp2_session_callbacks_del(callbacks);
    
    if (rv != 0) {
        LOG_ERROR("Failed to create nghttp2 session: " + std::string(nghttp2_strerror(rv)));
        return false;
    }
    
    initialized_ = true;
    LOG_INFO("HTTP/2 session initialized");
    return true;
}

ssize_t Http2Session::sendData(const uint8_t* data, size_t length) {
    if (send_callback_) {
        return send_callback_(data, length);
    }
    return 0;
}

ssize_t Http2Session::processData(const uint8_t* data, size_t length) {
    if (!session_) return -1;

    // nghttp2_session_mem_recv 返回已消费的字节数（ssize_t），负值是错误码。
    // 之前接进 int 会在单次接收超过 2GB 时静默截断。
    ssize_t rv = nghttp2_session_mem_recv(session_, data, length);
    if (rv < 0) {
        LOG_ERROR("HTTP/2 data processing error: " + std::string(nghttp2_strerror(static_cast<int>(rv))));
    }
    return rv;
}

int Http2Session::submitResponse(int32_t stream_id,
                                  const std::vector<std::pair<std::string, std::string>>& headers,
                                  const std::string& body) {
    if (!session_) return -1;

    std::vector<nghttp2_nv> nva;
    nva.reserve(headers.size());

    for (const auto& [name, value] : headers) {
        nghttp2_nv hdr;
        hdr.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.c_str()));
        hdr.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.c_str()));
        hdr.namelen = name.size();
        hdr.valuelen = value.size();
        hdr.flags = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(hdr);
    }

    // nghttp2_submit_response 的最后一个参数是 data_provider。这里传
    // provider，由它在 END_STREAM 之前把 body 拉出来发走。
    //
    // 关键：body 非空时不能直接用 nghttp2_submit_response(..., nullptr)，
    // 那样响应头一发完就带 END_STREAM，body 永远发不出去——这正是本函数
    // 之前丢掉 body 的原因。
    nghttp2_data_provider provider;
    provider.source.ptr = this;
    provider.read_callback = s_read_body_callback;

    pending_body_ = body;
    pending_body_pos_ = 0;

    int rv = nghttp2_submit_response(session_, stream_id,
                                      nva.data(), nva.size(),
                                      body.empty() ? nullptr : &provider);
    if (rv != 0) {
        LOG_ERROR("HTTP/2 submit response error: " + std::string(nghttp2_strerror(rv)));
        pending_body_.clear();
        pending_body_pos_ = 0;
    }

    return rv;
}

// 从 pending_body_ 拉取下一片数据交给 nghttp2 发送。
// 返回实际写入 buf 的字节数；返回 0 且置 EOF 表示 body 已发完。
ssize_t Http2Session::s_read_body_callback(nghttp2_session* /*session*/,
                                            int32_t /*stream_id*/,
                                            uint8_t* buf, size_t length,
                                            uint32_t* data_flags,
                                            nghttp2_data_source* /*source*/,
                                            void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self || !buf || length == 0) {
        if (data_flags) *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t remaining = self->pending_body_.size() - self->pending_body_pos_;
    if (remaining == 0) {
        if (data_flags) *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t chunk = remaining < length ? remaining : length;
    std::memcpy(buf, self->pending_body_.data() + self->pending_body_pos_, chunk);
    self->pending_body_pos_ += chunk;

    if (data_flags) {
        // 全部发完时带上 EOF，让 nghttp2 结束该 stream
        *data_flags = (self->pending_body_pos_ >= self->pending_body_.size())
                          ? NGHTTP2_DATA_FLAG_EOF
                          : NGHTTP2_DATA_FLAG_NONE;
    }
    return static_cast<ssize_t>(chunk);
}

int Http2Session::submitGoaway() {
    if (!session_) return -1;
    return nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, 0, 0, nullptr, 0);
}

// ============================================================
// Http2Handler 实现
// ============================================================
void Http2Handler::handleConnection(tcp::socket socket) {
    LOG_INFO("HTTP/2 connection accepted");
    auto session = std::make_shared<Http2Session>();
    if (!session->initServer()) {
        LOG_ERROR("Failed to initialize HTTP/2 session");
        boost::system::error_code ec;
        socket.close(ec);
        return;
    }
    // HTTP/2 connection lifecycle handled by the session
}

void Http2Handler::setupAlpn(ssl::context& ctx) {
    // 通过 TLS 扩展 ALPN 协商出 h2。
    // 之前这个函数只打了一行日志就返回，SSL_CTX 上从未设置过 ALPN 协议，
    // 导致 TLS 握手即使成功也协商不出 h2，客户端只能回落到 http/1.1。
    //
    // ALPN 协议列表是长度前缀的字节串：\x02h2（"h2" 前跟 1 字节长度 2）。
    // 只声明 h2 而不声明 http/1.1，是因为本模块只处理 h2 流。
    static const unsigned char kAlpnProtoList[] = {
        0x02, 'h', '2'
    };

    if (SSL_CTX_set_alpn_protos(ctx.native_handle(),
                                kAlpnProtoList,
                                static_cast<unsigned int>(sizeof(kAlpnProtoList))) != 0) {
        // 返回非 0 表示失败，可用 SSL_CTX_get_error 查具体原因
        unsigned long err = ERR_get_error();
        char buf[256] = {0};
        if (err) {
            ERR_error_string_n(err, buf, sizeof(buf));
        }
        LOG_ERROR("Failed to set ALPN protocols for HTTP/2: " + std::string(buf));
    } else {
        LOG_INFO("HTTP/2 ALPN configured for SSL connections");
    }
}

bool Http2Handler::isAvailable() {
    return true;
}

}} // namespace http2::http2_support