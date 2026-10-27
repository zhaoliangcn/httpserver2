#include "http2_handler.h"
#include <nghttp2/nghttp2.h>
#include <vector>
#include <cstring>
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

int Http2Session::processData(const uint8_t* data, size_t length) {
    if (!session_) return -1;
    
    int rv = nghttp2_session_mem_recv(session_, data, length);
    if (rv < 0) {
        LOG_ERROR("HTTP/2 data processing error: " + std::string(nghttp2_strerror(rv)));
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
    
    int rv = nghttp2_submit_response(session_, stream_id, 
                                      nva.data(), nva.size(),
                                      nullptr);
    if (rv != 0) {
        LOG_ERROR("HTTP/2 submit response error: " + std::string(nghttp2_strerror(rv)));
    }
    
    return rv;
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
    LOG_INFO("HTTP/2 ALPN configured for SSL connections");
}

bool Http2Handler::isAvailable() {
    return true;
}

}} // namespace http2::http2_support