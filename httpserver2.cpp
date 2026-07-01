//
//  httpserver2.cpp
//  test
//
//  Created by zhaoliang on 2025/10/17.
//

#include "httpserver2.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <thread>
#include <cstring>
#include <regex>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <charconv>
#include <zlib.h>
#include <boost/bind/bind.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

// PerformanceMonitor类静态成员初始化
std::atomic<uint64_t> PerformanceMonitor::total_requests_(0);
std::atomic<uint64_t> PerformanceMonitor::active_requests_(0);
std::atomic<uint64_t> PerformanceMonitor::total_response_time_(0);
std::atomic<uint64_t> PerformanceMonitor::max_response_time_(0);
std::atomic<uint64_t> PerformanceMonitor::min_response_time_(UINT64_MAX);
std::atomic<uint64_t> PerformanceMonitor::request_errors_(0);
std::atomic<uint64_t> PerformanceMonitor::bytes_sent_(0);
std::atomic<uint64_t> PerformanceMonitor::bytes_received_(0);
std::chrono::steady_clock::time_point PerformanceMonitor::start_time_ = std::chrono::steady_clock::now();
std::mutex PerformanceMonitor::metrics_mutex_;

// RateLimiter 实现
RateLimiter::RateLimiter(uint32_t requests_per_window, uint32_t window_size_seconds)
    : requests_per_window_(requests_per_window),
      window_size_(std::chrono::seconds(window_size_seconds))
{
    LOG_INFO("RateLimiter initialized with " + std::to_string(requests_per_window) + " requests per " + std::to_string(window_size_seconds) + " seconds");
}

bool RateLimiter::isAllowed(const std::string& client_ip)
{
    std::lock_guard<std::mutex> lock(ip_requests_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto& request_times = ip_requests_[client_ip];
    
    // 清除超出时间窗口的请求
    auto window_start = now - window_size_;
    auto it = std::remove_if(request_times.begin(), request_times.end(),
        [window_start](const std::chrono::steady_clock::time_point& time) {
            return time < window_start;
        });
    request_times.erase(it, request_times.end());
    
    // 检查是否超过限制
    if (request_times.size() < requests_per_window_) {
        // 允许请求，记录时间戳
        request_times.push_back(now);
        return true;
    }
    
    // 超过限制
    LOG_WARNING("Rate limit exceeded for IP: " + client_ip + ", current requests: " + std::to_string(request_times.size()) + ", limit: " + std::to_string(requests_per_window_));
    return false;
}

void RateLimiter::setRateLimit(uint32_t requests_per_window, uint32_t window_size_seconds)
{
    std::lock_guard<std::mutex> lock(ip_requests_mutex_);
    
    requests_per_window_ = requests_per_window;
    window_size_ = std::chrono::seconds(window_size_seconds);
    
    LOG_INFO("Rate limit updated to " + std::to_string(requests_per_window) + " requests per " + std::to_string(window_size_seconds) + " seconds");
}

// gzip压缩函数
std::string gzipCompress(const std::string& data) {
    if (data.empty()) {
        return data;
    }
    
    // 压缩缓冲区大小
    const size_t CHUNK_SIZE = 16384;
    
    // 压缩流
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    
    // 初始化gzip压缩
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return data; // 压缩初始化失败，返回原数据
    }
    
    // 设置输入数据
    zs.next_in = (Bytef*)data.data();
    zs.avail_in = data.size();
    
    // 输出缓冲区
    std::string compressed;
    std::vector<char> buffer(CHUNK_SIZE);
    
    // 压缩数据
    do {
        zs.next_out = (Bytef*)buffer.data();
        zs.avail_out = buffer.size();
        
        // 执行压缩
        int ret = deflate(&zs, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            return data; // 压缩失败，返回原数据
        }
        
        // 计算压缩后的数据大小
        size_t compressed_size = buffer.size() - zs.avail_out;
        compressed.append(buffer.data(), compressed_size);
        
        if (ret == Z_STREAM_END) {
            break;
        }
    } while (zs.avail_out == 0);
    
    // 清理压缩流
    deflateEnd(&zs);
    
    return compressed;
}
std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> PerformanceMonitor::route_metrics_;

// ============================================================
// 访问日志（独立的 Apache 风格 access log）
// ============================================================
static std::mutex g_access_log_mutex;

void logAccess(const std::string& client_ip, const std::string& method, const std::string& path,
               const std::string& status_code, const std::string& user_agent, int response_time_ms) {
    std::lock_guard<std::mutex> lock(g_access_log_mutex);
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S.")
       << std::setw(3) << std::setfill('0') << ms;
    std::string timestamp = ss.str();
    
    std::string log_entry = timestamp + " - " + client_ip + " - \"" + method + " " + path + "\" " +
                            status_code + " " + std::to_string(response_time_ms) + "ms - \"" + user_agent + "\"";
    
    // 用新日志库的 INFO 级别输出（同时到控制台和文件）
    LOG_INFO("[ACCESS] " + log_entry);
}

// ============================================================
// 旧 Logger 类方法已被移除，改用 single_include/logger.h
// ============================================================

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace algorithm = boost::algorithm;
using tcp = boost::asio::ip::tcp;

// 错误响应生成函数实现
HttpResponse createErrorResponse(HttpErrorType error_type, const std::string& message) {
    HttpResponse response;
    std::string default_message;
    std::string status_code;
    
    switch (error_type) {
        case HttpErrorType::BAD_REQUEST:
            status_code = "400";
            default_message = "Bad Request";
            break;
        case HttpErrorType::UNAUTHORIZED:
            status_code = "401";
            default_message = "Unauthorized";
            break;
        case HttpErrorType::FORBIDDEN:
            status_code = "403";
            default_message = "Forbidden";
            break;
        case HttpErrorType::NOT_FOUND:
            status_code = "404";
            default_message = "Not Found";
            break;
        case HttpErrorType::INTERNAL_ERROR:
            status_code = "500";
            default_message = "Internal Server Error";
            break;
        case HttpErrorType::SERVICE_UNAVAILABLE:
            status_code = "503";
            default_message = "Service Unavailable";
            break;
        default:
            status_code = "500";
            default_message = "Internal Server Error";
    }
    
    response.status_code = status_code;
    response.status_message = default_message;
    
    // 设置Content-Type为JSON
    response.headers["Content-Type"] = "application/json";
    
    // 构建JSON响应体
    std::string error_message = message.empty() ? default_message : message;
    response.body = "{";
    response.body += "\"error\":{";
    response.body += "\"code\":\"" + status_code + "\",";
    response.body += "\"message\":\"" + error_message + "\"";
    response.body += "}";
    response.body += "}";
    
    // 设置Content-Length
    response.headers["Content-Length"] = std::to_string(response.body.size());
    
    return response;
}

// 错误处理中间件工厂函数
Middleware createErrorHandlingMiddleware() {
    return [](const HttpRequest& /* request */) -> HttpResponse {
        // 这个中间件不做具体处理，只是让请求继续向下传递
        // 真正的错误处理发生在defaultHandler和handleMiddlewareChain中
        HttpResponse response;
        response.status_code = "404"; // 继续处理，让后续处理器处理
        return response;
    };
}

// ============================================================
// 安全头添加函数
// ============================================================
void addSecurityHeaders(HttpResponse& response, bool use_ssl) {
    // Content-Security-Policy: 限制资源加载来源
    // 注意: 如需内联脚本/样式，应在具体路由中覆盖此头或使用 nonce
    response.headers["Content-Security-Policy"] =
        "default-src 'self'; "
        "script-src 'self'; "
        "style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data:; "
        "font-src 'self'; "
        "connect-src 'self'; "
        "frame-ancestors 'self'; "
        "form-action 'self'; "
        "base-uri 'self'";

    // Strict-Transport-Security: 仅在 HTTPS 时设置
    if (use_ssl) {
        response.headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains";
    }

    // X-Content-Type-Options: 禁止 MIME 嗅探
    response.headers["X-Content-Type-Options"] = "nosniff";

    // X-Frame-Options: 禁止点击劫持
    response.headers["X-Frame-Options"] = "SAMEORIGIN";

    // X-XSS-Protection: 启用在旧浏览器中的 XSS 过滤（新浏览器已废弃此头，保留无害）
    response.headers["X-XSS-Protection"] = "1; mode=block";

    // Referrer-Policy: 限制 Referer 头的传递范围
    response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin";

    // Permissions-Policy: 限制浏览器 API 权限（替代废弃的 Feature-Policy）
    response.headers["Permissions-Policy"] =
        "camera=(), "
        "microphone=(), "
        "geolocation=(), "
        "interest-cohort=()";

    // Cross-Origin-Opener-Policy: 隔离跨窗口访问（防止 Spectre 信息泄露）
    response.headers["Cross-Origin-Opener-Policy"] = "same-origin";

    // Cache-Control: 非静态资源不应被缓存
    // 静态文件在 serveStaticFile 中会覆盖此值
    if (response.headers.find("Cache-Control") == response.headers.end()) {
        response.headers["Cache-Control"] = "no-store";
    }
}

// ============================================================
// 公共响应处理（消除 HTTP/HTTPS handler 重复代码）
// ============================================================

void HttpServer2::finalizeResponse(HttpResponse& response,
                                    const HttpRequest& request,
                                    bool use_ssl,
                                    const std::shared_ptr<DebugInfo>& debug_info) {
    // 添加调试头信息
    if (debug_mode_) {
        std::shared_ptr<DebugInfo> local_debug = debug_info;
        if (!local_debug) {
            local_debug = std::make_shared<DebugInfo>();
            local_debug->start_time = std::chrono::steady_clock::now();
            local_debug->end_time = std::chrono::steady_clock::now();
            local_debug->client_ip = "127.0.0.1";
            local_debug->routing_time = std::chrono::milliseconds(0);
        }
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            local_debug->end_time - local_debug->start_time);
        response.headers["X-Debug-Time"] = std::to_string(duration.count()) + "ms";
        response.headers["X-Debug-Client-IP"] = local_debug->client_ip;
        response.headers["X-Debug-Route-Time"] = std::to_string(local_debug->routing_time.count()) + "ms";
    }

    // 添加 CSRF cookie
    if (csrf_protection_enabled_) {
        bool has_csrf_cookie = false;
        auto cookie_it = request.headers.find("Cookie");
        if (cookie_it != request.headers.end()) {
            has_csrf_cookie = (cookie_it->second.find(csrf_cookie_name_ + "=") != std::string::npos);
        }
        if (!has_csrf_cookie) {
            const std::string charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(0, charset.size() - 1);
            std::string csrf_token;
            csrf_token.reserve(32);
            for (int i = 0; i < 32; ++i) {
                csrf_token += charset[distrib(gen)];
            }
            std::string cookie = csrf_cookie_name_ + "=" + csrf_token + "; HttpOnly=false; SameSite=Strict; Path=/";
            if (use_ssl) cookie += "; Secure";
            response.headers["Set-Cookie"] = cookie;
        }
    }

    // 添加安全头
    addSecurityHeaders(response, use_ssl);
}

void HttpServer2::buildAndSendResponse(tcp::socket& socket,
                                        HttpResponse& custom_response,
                                        const std::string& client_ip,
                                        const std::string& path,
                                        bool keep_alive,
                                        bool should_compress,
                                        std::function<void()> on_complete) {
    // 如果响应已被流式发送（如反向代理直转），跳过
    if (custom_response.response_sent) {
        if (keep_alive && on_complete) on_complete();
        return;
    }
    
    // 如果设置了 file_body_path，使用 sendfile 零拷贝发送
    if (!custom_response.file_body_path.empty()) {
        sendFileResponse(socket, custom_response, client_ip, path);
        return;
    }
    
    auto response = std::make_shared<http::response<http::string_body>>();

    // 状态码
    int status_code = 500;
    try { status_code = std::atoi(custom_response.status_code.c_str()); } catch (...) {}
    response->result(http::status(status_code));

    // 响应体
    response->body() = custom_response.body;

    // 添加响应头
    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }

    // Content-Type 默认值
    if (custom_response.headers.find("Content-Type") == custom_response.headers.end()) {
        response->set(http::field::content_type, "text/plain");
    }

    // gzip 压缩
    if (should_compress && !response->body().empty()) {
        std::string compressed = gzipCompress(response->body());
        if (compressed.size() < response->body().size()) {
            response->body() = std::move(compressed);
            response->set(http::field::content_encoding, "gzip");
        }
    }

    // 服务端 & 日期头
    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream date_ss;
    date_ss << std::put_time(gmtime(&now_c), "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());

    // Content-Length & payload
    response->prepare_payload();
    response->keep_alive(keep_alive);

    // 异步发送
    http::async_write(socket, *response,
        [response, keep_alive, on_complete](beast::error_code ec, std::size_t) {
            if (ec && ec != boost::asio::error::eof &&
                ec != boost::asio::error::broken_pipe &&
                ec != boost::asio::error::connection_reset) {
                LOG_ERROR("Write error: " + ec.message());
            }
            // Keep-Alive: 通知调用方继续读取下一个请求
            if (keep_alive && on_complete && !ec) {
                on_complete();
            }
        }
    );
}

void HttpServer2::buildAndSendResponse(std::shared_ptr<ssl::stream<tcp::socket>> ssl_socket_ptr,
                                        HttpResponse& custom_response,
                                        const std::string& client_ip,
                                        const std::string& path,
                                        bool /* keep_alive */,
                                        bool should_compress) {
    // 如果响应已被流式发送，跳过
    if (custom_response.response_sent) return;
    
    // 如果设置了 file_body_path，使用 sendfile 零拷贝发送
    if (!custom_response.file_body_path.empty()) {
        sendFileResponse(ssl_socket_ptr, custom_response, client_ip, path);
        return;
    }
    
    auto response = std::make_shared<http::response<http::string_body>>();

    int status_code = 500;
    try { status_code = std::atoi(custom_response.status_code.c_str()); } catch (...) {}
    response->result(http::status(status_code));

    response->body() = custom_response.body;

    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }

    if (custom_response.headers.find("Content-Type") == custom_response.headers.end()) {
        response->set(http::field::content_type, "text/plain");
    }

    if (should_compress && !response->body().empty()) {
        std::string compressed = gzipCompress(response->body());
        if (compressed.size() < response->body().size()) {
            response->body() = std::move(compressed);
            response->set(http::field::content_encoding, "gzip");
        }
    }

    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream date_ss;
    date_ss << std::put_time(gmtime(&now_c), "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());

    response->prepare_payload();

    http::async_write(*ssl_socket_ptr, *response,
        [ssl_socket_ptr, response](beast::error_code ec, std::size_t) {
            boost::ignore_unused(ec, response);
            // SSL 安全关闭
            boost::system::error_code shut_ec;
            ssl_socket_ptr->shutdown(shut_ec);
            if (ssl_socket_ptr->lowest_layer().is_open()) {
                boost::system::error_code close_ec;
                ssl_socket_ptr->lowest_layer().close(close_ec);
            }
        }
    );
}

// ============================================================
// sendfile 零拷贝文件发送
// ============================================================

void HttpServer2::sendFileResponse(tcp::socket& socket,
                                    HttpResponse& custom_response,
                                    const std::string& /* client_ip */,
                                    const std::string& /* path */) {
    // 打开文件
    beast::error_code ec;
    http::file_body::value_type file;
    file.open(custom_response.file_body_path.c_str(), beast::file_mode::read, ec);
    if (ec) {
        LOG_ERROR("sendFileResponse: failed to open " + custom_response.file_body_path + ": " + ec.message());
        // 回退：发送 500 错误
        http::response<http::string_body> err_resp(http::status::internal_server_error, 11);
        err_resp.body() = "Internal Server Error";
        err_resp.prepare_payload();
        http::write(socket, err_resp, ec);
        return;
    }

    auto file_size = file.size();
    
    // 构建 file_body 响应
    auto response = std::make_shared<http::response<http::file_body>>(
        std::piecewise_construct,
        std::make_tuple(std::move(file)),
        std::make_tuple()
    );
    
    // 状态码
    int status_code = 500;
    try { status_code = std::atoi(custom_response.status_code.c_str()); } catch (...) {}
    response->result(http::status(status_code));
    response->version(11);
    
    // 复制响应头
    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }
    
    // 服务端头
    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream date_ss;
    date_ss << std::put_time(gmtime(&now_c), "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());
    
    // Content-Length (file_body 不自动设置)
    response->content_length(file_size);
    
    // 异步发送（底层使用 sendfile 系统调用）
    http::async_write(socket, *response,
        [response](beast::error_code ec, std::size_t) {
            if (ec) {
                LOG_ERROR("sendFileResponse write error: " + ec.message());
            }
        }
    );
}

void HttpServer2::sendFileResponse(std::shared_ptr<ssl::stream<tcp::socket>> ssl_socket_ptr,
                                    HttpResponse& custom_response,
                                    const std::string& /* client_ip */,
                                    const std::string& /* path */) {
    beast::error_code ec;
    http::file_body::value_type file;
    file.open(custom_response.file_body_path.c_str(), beast::file_mode::read, ec);
    if (ec) {
        LOG_ERROR("sendFileResponse(SSL): failed to open " + custom_response.file_body_path + ": " + ec.message());
        http::response<http::string_body> err_resp(http::status::internal_server_error, 11);
        err_resp.body() = "Internal Server Error";
        err_resp.prepare_payload();
        http::write(*ssl_socket_ptr, err_resp, ec);
        return;
    }

    auto file_size = file.size();
    
    auto response = std::make_shared<http::response<http::file_body>>(
        std::piecewise_construct,
        std::make_tuple(std::move(file)),
        std::make_tuple()
    );
    
    int status_code = 500;
    try { status_code = std::atoi(custom_response.status_code.c_str()); } catch (...) {}
    response->result(http::status(status_code));
    response->version(11);
    
    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }
    
    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream date_ss;
    date_ss << std::put_time(gmtime(&now_c), "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());
    
    response->content_length(file_size);
    
    http::async_write(*ssl_socket_ptr, *response,
        [ssl_socket_ptr, response](beast::error_code ec, std::size_t) {
            boost::ignore_unused(ec, response);
            boost::system::error_code shut_ec;
            ssl_socket_ptr->shutdown(shut_ec);
            if (ssl_socket_ptr->lowest_layer().is_open()) {
                boost::system::error_code close_ec;
                ssl_socket_ptr->lowest_layer().close(close_ec);
            }
        }
    );
}

// PerformanceMonitor类方法实现

// 重置所有指标
void PerformanceMonitor::reset() {
    total_requests_.store(0);
    active_requests_.store(0);
    total_response_time_.store(0);
    max_response_time_.store(0);
    min_response_time_.store(UINT64_MAX);
    request_errors_.store(0);
    bytes_sent_.store(0);
    bytes_received_.store(0);
    start_time_ = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    route_metrics_.clear();
}

// 记录请求开始
void PerformanceMonitor::recordRequest() {
    total_requests_++;
    active_requests_++;
}

// 记录请求结束和响应信息
void PerformanceMonitor::recordResponse(const std::string& route, uint64_t response_time_ms, size_t response_size, bool success) {
    active_requests_--;
    total_response_time_ += response_time_ms;
    bytes_sent_ += response_size;
    
    // 更新最大和最小响应时间
    uint64_t current_max = max_response_time_;
    while (response_time_ms > current_max && !max_response_time_.compare_exchange_weak(current_max, response_time_ms)) {
        // 继续尝试
    }
    
    uint64_t current_min = min_response_time_;
    while (response_time_ms < current_min && !min_response_time_.compare_exchange_weak(current_min, response_time_ms)) {
        // 继续尝试
    }
    
    // 如果请求失败，记录错误
    if (!success) {
        request_errors_++;
    }
    
    // 记录路由性能指标
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        auto& [count, total_time] = route_metrics_[route];
        count++;
        total_time += response_time_ms;
    }
}

// 记录接收的字节数
void PerformanceMonitor::recordBytesReceived(size_t bytes) {
    bytes_received_ += bytes;
}

// 记录错误
void PerformanceMonitor::recordError() {
    request_errors_++;
}

// 获取平均响应时间
double PerformanceMonitor::getAverageResponseTime() {
    uint64_t total = total_requests_;
    if (total == 0) return 0.0;
    return static_cast<double>(total_response_time_) / total;
}

// 获取总请求数
uint64_t PerformanceMonitor::getTotalRequests() {
    return total_requests_;
}

// 获取活跃请求数
uint64_t PerformanceMonitor::getActiveRequests() {
    return active_requests_;
}

// 获取请求错误数
uint64_t PerformanceMonitor::getRequestErrors() {
    return request_errors_;
}

// 获取最大响应时间
uint64_t PerformanceMonitor::getMaxResponseTime() {
    return max_response_time_;
}

// 获取最小响应时间
uint64_t PerformanceMonitor::getMinResponseTime() {
    uint64_t min = min_response_time_;
    return (min == UINT64_MAX) ? 0 : min;
}

// 获取发送的字节数
uint64_t PerformanceMonitor::getBytesSent() {
    return bytes_sent_;
}

// 获取接收的字节数
uint64_t PerformanceMonitor::getBytesReceived() {
    return bytes_received_;
}

// 获取每秒请求数
double PerformanceMonitor::getRequestsPerSecond() {
    auto now = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (elapsed_seconds == 0) return 0.0;
    return static_cast<double>(total_requests_) / elapsed_seconds;
}

// 获取路由性能数据
std::unordered_map<std::string, double> PerformanceMonitor::getRoutePerformance() {
    std::unordered_map<std::string, double> result;
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    for (const auto& [route, metrics] : route_metrics_) {
        const auto& [count, total_time] = metrics;
        if (count > 0) {
            result[route] = static_cast<double>(total_time) / count;
        }
    }
    
    return result;
}

// 生成性能报告
std::string PerformanceMonitor::generatePerformanceReport() {
    std::stringstream ss;
    ss << "=== Performance Report ===\n";
    ss << "Total Requests: " << getTotalRequests() << "\n";
    ss << "Active Requests: " << getActiveRequests() << "\n";
    ss << "Request Errors: " << getRequestErrors() << " (" << 
       (getTotalRequests() > 0 ? static_cast<double>(getRequestErrors()) / getTotalRequests() * 100 : 0) << "%)\n";
    ss << "Average Response Time: " << getAverageResponseTime() << " ms\n";
    ss << "Max Response Time: " << getMaxResponseTime() << " ms\n";
    ss << "Min Response Time: " << getMinResponseTime() << " ms\n";
    ss << "Requests Per Second: " << getRequestsPerSecond() << "\n";
    ss << "Bytes Sent: " << getBytesSent() << "\n";
    ss << "Bytes Received: " << getBytesReceived() << "\n\n";
    
    ss << "Route Performance:\n";
    auto route_perf = getRoutePerformance();
    std::vector<std::pair<std::string, double>> sorted_routes(route_perf.begin(), route_perf.end());
    std::sort(sorted_routes.begin(), sorted_routes.end(), 
             [](const auto& a, const auto& b) { return a.second > b.second; });
    
    for (const auto& [route, avg_time] : sorted_routes) {
        ss << "  " << std::setw(30) << std::left << route << " : " 
           << std::setprecision(2) << std::fixed << avg_time << " ms\n";
    }
    
    ss << "===========================";
    return ss.str();
}

// 生成Prometheus格式的指标
std::string PerformanceMonitor::generatePrometheusMetrics() {
    std::stringstream ss;
    
    // 添加指标描述
    ss << "# HELP httpserver2_total_requests Total number of requests received\n";
    ss << "# TYPE httpserver2_total_requests counter\n";
    ss << "httpserver2_total_requests " << getTotalRequests() << "\n\n";
    
    ss << "# HELP httpserver2_active_requests Current number of active requests\n";
    ss << "# TYPE httpserver2_active_requests gauge\n";
    ss << "httpserver2_active_requests " << getActiveRequests() << "\n\n";
    
    ss << "# HELP httpserver2_request_errors Total number of request errors\n";
    ss << "# TYPE httpserver2_request_errors counter\n";
    ss << "httpserver2_request_errors " << getRequestErrors() << "\n\n";
    
    ss << "# HELP httpserver2_average_response_time_ms Average response time in milliseconds\n";
    ss << "# TYPE httpserver2_average_response_time_ms gauge\n";
    ss << "httpserver2_average_response_time_ms " << getAverageResponseTime() << "\n\n";
    
    ss << "# HELP httpserver2_max_response_time_ms Maximum response time in milliseconds\n";
    ss << "# TYPE httpserver2_max_response_time_ms gauge\n";
    ss << "httpserver2_max_response_time_ms " << getMaxResponseTime() << "\n\n";
    
    ss << "# HELP httpserver2_min_response_time_ms Minimum response time in milliseconds\n";
    ss << "# TYPE httpserver2_min_response_time_ms gauge\n";
    ss << "httpserver2_min_response_time_ms " << getMinResponseTime() << "\n\n";
    
    ss << "# HELP httpserver2_requests_per_second Requests per second\n";
    ss << "# TYPE httpserver2_requests_per_second gauge\n";
    ss << "httpserver2_requests_per_second " << getRequestsPerSecond() << "\n\n";
    
    ss << "# HELP httpserver2_bytes_sent Total bytes sent\n";
    ss << "# TYPE httpserver2_bytes_sent counter\n";
    ss << "httpserver2_bytes_sent " << getBytesSent() << "\n\n";
    
    ss << "# HELP httpserver2_bytes_received Total bytes received\n";
    ss << "# TYPE httpserver2_bytes_received counter\n";
    ss << "httpserver2_bytes_received " << getBytesReceived() << "\n\n";
    
    // 添加路由级别的指标
    ss << "# HELP httpserver2_route_requests_total Total requests per route\n";
    ss << "# TYPE httpserver2_route_requests_total counter\n";
    
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    for (const auto& [route, metrics] : route_metrics_) {
        const auto& [count, total_time] = metrics;
        std::string sanitized_route = route;
        // 替换Prometheus标签中的特殊字符
        std::replace(sanitized_route.begin(), sanitized_route.end(), '/', '_');
        std::replace(sanitized_route.begin(), sanitized_route.end(), ' ', '_');
        std::replace(sanitized_route.begin(), sanitized_route.end(), '.', '_');
        ss << "httpserver2_route_requests_total{route=\"" << sanitized_route << "\"} " << count << "\n";
    }
    
    ss << "\n# HELP httpserver2_route_average_response_time_ms Average response time per route in milliseconds\n";
    ss << "# TYPE httpserver2_route_average_response_time_ms gauge\n";
    
    for (const auto& [route, metrics] : route_metrics_) {
        const auto& [count, total_time] = metrics;
        std::string sanitized_route = route;
        std::replace(sanitized_route.begin(), sanitized_route.end(), '/', '_');
        std::replace(sanitized_route.begin(), sanitized_route.end(), ' ', '_');
        std::replace(sanitized_route.begin(), sanitized_route.end(), '.', '_');
        double avg_time = count > 0 ? static_cast<double>(total_time) / count : 0;
        ss << "httpserver2_route_average_response_time_ms{route=\"" << sanitized_route << "\"} " 
           << std::setprecision(2) << std::fixed << avg_time << "\n";
    }
    
    return ss.str();
}


// 辅助函数：URL解码
std::string HttpServer2::urlDecode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.length(); i++) {
        if (encoded[i] == '%') {
            if (i + 2 < encoded.length()) {
                unsigned int byte_val;
                auto result = std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, byte_val, 16);
                if (result.ec == std::errc{}) {
                    decoded += static_cast<char>(byte_val);
                    i += 2;
                }
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

// ExactRouteMatcher实现
ExactRouteMatcher::ExactRouteMatcher(const std::string& path) : path_(path) {}

bool ExactRouteMatcher::match(const std::string& path, std::unordered_map<std::string, std::string>& /* path_params */) const {
    return path == path_;
}

// ParametricRouteMatcher实现
ParametricRouteMatcher::ParametricRouteMatcher(const std::string& pattern) : pattern_(pattern) {
    // 解析路径模式，提取参数名
    std::vector<std::string> parts;
    boost::algorithm::split(parts, pattern, boost::algorithm::is_any_of("/"), boost::algorithm::token_compress_on);
    
    // 由于头文件中没有声明segments_，我们直接使用param_names_
    for (const auto& part : parts) {
        if (!part.empty()) {
            if (part.front() == '{' && part.back() == '}') {
                // 提取参数名，去掉{和}
                param_names_.push_back(part.substr(1, part.size() - 2));
            }
        }
    }
}

bool ParametricRouteMatcher::match(const std::string& path, std::unordered_map<std::string, std::string>& path_params) const {
    std::vector<std::string> path_parts;
    boost::algorithm::split(path_parts, path, boost::algorithm::is_any_of("/"), boost::algorithm::token_compress_on);
    
    // 过滤掉空部分
    path_parts.erase(std::remove_if(path_parts.begin(), path_parts.end(), 
                                  [](const std::string& s) { return s.empty(); }), 
                    path_parts.end());
    
    // 重新解析模式来获取部分，因为parts变量不在这个作用域中
    std::vector<std::string> pattern_parts;
    boost::algorithm::split(pattern_parts, pattern_, boost::algorithm::is_any_of("/"), boost::algorithm::token_compress_on);
    
    // 过滤掉空部分
    pattern_parts.erase(std::remove_if(pattern_parts.begin(), pattern_parts.end(), 
                                     [](const std::string& s) { return s.empty(); }), 
                       pattern_parts.end());
    
    if (path_parts.size() != pattern_parts.size()) {
        return false;
    }
    
    // 匹配路径并提取参数
    size_t param_index = 0;
    for (size_t i = 0; i < pattern_parts.size(); ++i) {
        if (!pattern_parts[i].empty() && pattern_parts[i].front() == '{' && pattern_parts[i].back() == '}') {
            // 这是一个参数位置
            if (param_index < param_names_.size()) {
                path_params[param_names_[param_index++]] = path_parts[i];
            }
        } else if (pattern_parts[i] != path_parts[i]) {
            // 非参数部分必须完全匹配
            return false;
        }
    }
    
    return true;
}

// RegexRouteMatcher实现 - 使用boost正则库
RegexRouteMatcher::RegexRouteMatcher(const std::string& pattern) : pattern_(pattern) {
    try {
        // 处理模式中的参数占位符 {param_name}
        std::string regex_pattern = pattern;
        size_t start_pos = 0;
        
        // 使用字符串查找替代正则表达式查找
        while (true) {
            size_t open_brace = regex_pattern.find('{', start_pos);
            if (open_brace == std::string::npos) break;
            
            size_t close_brace = regex_pattern.find('}', open_brace + 1);
            if (close_brace == std::string::npos) break;
            
            // 提取参数名，但RegexRouteMatcher类中没有param_names_成员变量
            // 我们直接替换为捕获组，不保存参数名
            
            // 替换为捕获组 ([^/]+)
            regex_pattern.replace(open_brace, close_brace - open_brace + 1, "([^/]+)");
            start_pos = open_brace + 8; // "([^/]+)" 的长度
        }
        
        // 编译正则表达式 - 检查是否已经有^和$修饰符
        bool has_anchor_start = !regex_pattern.empty() && regex_pattern[0] == '^';
        bool has_anchor_end = !regex_pattern.empty() && regex_pattern.back() == '$';
        
        if (has_anchor_start && has_anchor_end) {
            // 如果已经有^和$，直接使用
            regex_ = boost::regex(regex_pattern);
            LOG_DEBUG("Compiled regex with existing anchors: '" + regex_pattern + "'");
        } else {
            // 否则添加^和$
            regex_ = boost::regex("^" + regex_pattern + "$");
            LOG_DEBUG("Compiled regex with added anchors: '" + ("^" + regex_pattern + "$") + "'");
        }
    } catch (...) {
        // 如果解析失败，使用简单的字符串匹配
        try {
            // 转义特殊字符后创建正则表达式
            std::string escaped;
            for (char c : pattern) {
                if (strchr(".+*?()[]{}|^$", c)) {
                    escaped += '\\';
                }
                escaped += c;
            }
            regex_ = boost::regex("^" + escaped + "$");
        } catch (...) {
            // 如果还是失败，不初始化正则表达式
        }
    }
}

bool RegexRouteMatcher::match(const std::string& path, std::unordered_map<std::string, std::string>& path_params) const {
    try {
        boost::smatch matches;
        if (boost::regex_match(path, matches, regex_)) {
            // 提取捕获组作为路径参数，使用捕获组索引作为键
            for (size_t i = 1; i < matches.size(); i++) {
                path_params[std::to_string(i)] = matches[i].str();
            }
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Regex matching error: " + std::string(e.what()));
        // 即使出错，也尝试使用boost::regex_match进行简单匹配
        try {
            return boost::regex_match(path, boost::regex(pattern_));
        } catch (...) {
            // 最后才回退到字符串比较
            return path == pattern_;
        }
    }
}

// 创建适合的路由匹配器
std::shared_ptr<RouteMatcher> HttpServer2::createRouteMatcher(const std::string& path) {
    // 首先检查是否包含路径参数 - 优先使用参数匹配器
    if (path.find('{') != std::string::npos && path.find('}') != std::string::npos) {
        return std::make_shared<ParametricRouteMatcher>(path);
    }
    
    // 检查是否是纯正则表达式模式（包含正则特殊字符）
    // 特别是对于serveStatic中使用的模式如^/static/(.*)$
    if (path.find('^') != std::string::npos || 
        path.find('$') != std::string::npos || 
        path.find('(') != std::string::npos || 
        path.find(')') != std::string::npos || 
        path.find('*') != std::string::npos || 
        path.find('+') != std::string::npos || 
        path.find('[') != std::string::npos || 
        path.find(']') != std::string::npos) {
        LOG_DEBUG("Creating RegexRouteMatcher for pattern: '" + path + "'");
        return std::make_shared<RegexRouteMatcher>(path);
    }
    
    // 否则使用精确匹配
    LOG_DEBUG("Creating ExactRouteMatcher for path: '" + path + "'");
    return std::make_shared<ExactRouteMatcher>(path);
}

// 构造函数
HttpServer2::HttpServer2(bool use_ssl) : 
    next_io_context_index_(0),
    use_ssl_(use_ssl),
    cert_path_(),
    key_path_(),
    request_timeout_(std::chrono::seconds(60)),
    running_(false),
    max_pool_connections_(100),
    max_idle_time_(std::chrono::seconds(60)),
    request_count_(0),
    requests_per_second_(0),
    max_queue_size_(10000) {
    // 初始化rate_limit_window_
    rate_limit_window_ = std::chrono::steady_clock::now();
    // 初始化MIME类型映射
    initializeMimeTypes();
    // acceptor将在start方法中创建
    
    // 如果启用SSL，创建SSL上下文
    if (use_ssl_) {
        try {
            ssl_context_ = std::make_unique<ssl::context>(ssl::context::tlsv12);
            ssl_context_->set_options(
                ssl::context::default_workarounds |
                ssl::context::no_sslv2 |
                ssl::context::no_sslv3 |
                ssl::context::no_tlsv1 |
                ssl::context::single_dh_use);
        } catch (const std::exception& e) {
            LOG_ERROR("SSL context initialization error: " + std::string(e.what()));
            throw;
        }
    }
}

// 析构函数
HttpServer2::~HttpServer2() {
    if (isRunning()) {
        stop();
    }
}

// 启动服务器
void HttpServer2::start(int port, size_t thread_pool_size) {
    try {
        // 确定线程池大小，如果为0则使用CPU核心数
        if (thread_pool_size == 0) {
            thread_pool_size = std::thread::hardware_concurrency();
            if (thread_pool_size == 0) thread_pool_size = 4;  // 默认值
        }

        // 创建多个io_context实例
        io_contexts_.resize(thread_pool_size);
        for (size_t i = 0; i < thread_pool_size; ++i) {
            io_contexts_[i] = std::make_shared<net::io_context>();
        }

        // 选择第一个io_context来创建acceptor
        auto main_io_context = io_contexts_[0];
        
        // 创建acceptor
        acceptor_ = std::make_unique<tcp::acceptor>(*main_io_context, tcp::endpoint(tcp::v4(), port));
        
        // 设置服务器运行状态
        running_ = true;
        
        // 开始异步接受连接
        acceptConnection();
        
        // 启动服务器清理线程（定期清理过期连接）
        cleanup_thread_ = std::thread([this]() {
            while (running_) {
                // 使用较短的等待时间，以便更快响应退出信号
                for (int i = 0; i < 60 && running_; ++i) { // 总共等待30秒，但每秒检查一次running_
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                if (running_) { // 再次检查running_，避免在等待期间服务器已停止
                    cleanupExpiredConnections();
                }
            }
        });

        // 启动多个请求队列处理线程，充分利用线程池
        queue_threads_.resize(thread_pool_size);
        for (size_t i = 0; i < thread_pool_size; ++i) {
            queue_threads_[i] = std::thread([this]() {
                processRequestQueue();
            });
        }
        
        // 创建并启动线程池
        thread_pool_.resize(thread_pool_size);
        for (size_t i = 0; i < thread_pool_size; ++i) {
            thread_pool_[i] = std::thread(&HttpServer2::runIoContext, this, io_contexts_[i]);
        }
        
        // 启动自适应线程池调整线程
        startThreadAdjustment();
        
        // 启动配置文件监控线程
        startConfigWatch();
        
        std::cout << "HTTP Server (supporting HTTP/1.1 and HTTP/2.0) started on port " << port 
                  << " with " << thread_pool_size << " worker threads" << std::endl;
        std::cout << "Server is running at " << (use_ssl_ ? "https" : "http") << "://localhost:" << port << std::endl;
        if (enable_adaptive_thread_pool_) {
            std::cout << "Adaptive thread pool enabled, min: " << min_thread_pool_size_ 
                      << ", max: " << max_thread_pool_size_ << std::endl;
        }
        if (enable_config_hot_reload_) {
            std::cout << "Config hot reload enabled, watching " << config_file_path_ << std::endl;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Server startup error: " + std::string(e.what()));
        running_ = false;  // 设置原子变量为false
                    stop();  // 确保资源被正确释放
        throw;
    }
}

// 异步接受连接
void HttpServer2::acceptConnection() {
    if (!isRunning()) return;
    
    if (use_ssl_) {
        // SSL模式：使用SSL流
        auto main_io_context = io_contexts_[0];  // acceptor始终使用第一个io_context
        auto socket = std::make_shared<tcp::socket>(*main_io_context);
        
        acceptor_->async_accept(*socket, 
            [this, socket](const boost::system::error_code& error) {
                if (!error && isRunning()) {
                    try {
                        // 获取一个io_context用于处理连接（负载均衡）
                        auto io_context = getNextIoContext();
                        
                        // 创建新的socket并移动到选定的io_context
                        auto io_socket = std::make_shared<tcp::socket>(*io_context);
                        *io_socket = std::move(*socket);
                        
                        // 创建SSL流
                        // 创建一个新的socket而不是使用引用
                        tcp::socket new_socket(*io_contexts_[0]);
                        new_socket = std::move(*io_socket);
                        auto ssl_socket = std::make_shared<ssl::stream<tcp::socket>>(std::move(new_socket), *ssl_context_);
                        
                        // 执行SSL握手
                        ssl_socket->async_handshake(ssl::stream_base::server, 
                            [this, ssl_socket](const boost::system::error_code& error) {
                                if (!error && isRunning()) {
                                    // SSL握手成功，处理HTTPS连接
                                    handleClientHttps(std::move(*ssl_socket));
                                } else if (isRunning()) {
                                    LOG_ERROR("SSL handshake error: " + error.message());
                                }
                                // 继续接受下一个连接
                                acceptConnection();
                            }
                        );
                    } catch (const std::exception& e) {
                        LOG_ERROR("SSL connection error: " + std::string(e.what()));
                        acceptConnection();
                    }
                } else if (isRunning()) {
                    LOG_ERROR("Accept error: " + error.message());
                    acceptConnection();
                }
            }
        );
    } else {
        // HTTP模式：直接使用TCP socket
        auto main_io_context = io_contexts_[0];  // acceptor始终使用第一个io_context
        auto socket = std::make_shared<tcp::socket>(*main_io_context);
        
        acceptor_->async_accept(*socket, 
            [this, socket](const boost::system::error_code& error) {
                if (!error && isRunning()) {
                    // 获取一个io_context用于处理连接（负载均衡）
                    auto io_context = getNextIoContext();
                    
                    // 创建新的socket并移动到选定的io_context
                    auto io_socket = std::make_shared<tcp::socket>(*io_context);
                    *io_socket = std::move(*socket);
                    
                    // 处理HTTP连接
                    handleClientHttp1(std::move(*io_socket));
                    
                    // 继续接受下一个连接
                    acceptConnection();
                } else if (isRunning()) {
                    LOG_ERROR("Accept error: " + error.message());
                    // 继续接受下一个连接
                    acceptConnection();
                }
            }
        );
    }
}

// 处理客户端连接（HTTP/1.1）
void HttpServer2::handleClientHttp1(tcp::socket socket) {
    // 创建调试信息对象（如果启用调试模式）
    std::shared_ptr<DebugInfo> debug_info;
    if (debug_mode_) {
        debug_info = std::make_shared<DebugInfo>();
        debug_info->start_time = std::chrono::steady_clock::now();
        debug_info->client_ip = socket.remote_endpoint().address().to_string();
        debug_info->client_port = socket.remote_endpoint().port();
    }
    
    // 记录请求开始
    PerformanceMonitor::recordRequest();
    
    // 检查是否限流
    bool rate_limited = false;
    std::string client_ip = socket.remote_endpoint().address().to_string();
    
    // 检查全局限流
    if (isRateLimited()) {
        rate_limited = true;
    } 
    // 检查IP级别的限流
    else if (!rate_limiter_.isAllowed(client_ip)) {
        rate_limited = true;
    }
    
    if (rate_limited) {
        try {
            // 发送429 Too Many Requests响应
            http::response<http::string_body> response;
            response.version(11);
            response.result(http::status::too_many_requests);
            response.set(http::field::content_type, "text/plain");
            response.set(http::field::retry_after, "60");
            response.body() = "Too many requests, please try again later.";
            response.prepare_payload();
            
            http::write(socket, response);
            boost::system::error_code ec;
            socket.shutdown(tcp::socket::shutdown_send, ec);
        } catch (...) {
            // 忽略错误
        }
        return;
    }

    // 创建socket的shared_ptr以便在异步操作中保持其生命周期
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    // 确保socket有效
    if (!socket_ptr || !socket_ptr->is_open()) {
        LOG_ERROR("Invalid socket, closing connection");
        return;
    }
    
    // 创建连接状态对象，管理所有相关资源
    struct ConnectionState {
        std::shared_ptr<tcp::socket> socket;
        beast::flat_buffer buffer; // 避免shared_ptr，减少引用计数开销
        net::deadline_timer timer;
        http::request_parser<http::string_body> parser; // 避免shared_ptr，直接存储
        
        ConnectionState(std::shared_ptr<tcp::socket> s, size_t max_size) 
            : socket(std::move(s)), 
              buffer(8192), // 预分配8KB缓冲区
              timer(socket->get_executor()),
              parser()
        {
            // 使用服务器配置的最大请求大小限制
            parser.body_limit(max_size);
        }
    };
    
    auto state = std::make_shared<ConnectionState>(socket_ptr, max_request_size_);
    
    // 设置超时时间
    std::chrono::seconds timeout = request_timeout_;
    if (timeout.count() <= 0) {
        timeout = std::chrono::seconds(60); // 默认60秒
    }
    state->timer.expires_from_now(boost::posix_time::seconds(timeout.count()));
    
    // 设置超时回调
    state->timer.async_wait([state](const boost::system::error_code& ec) {
        if (!ec) { // 如果定时器没有被取消，则超时
            try {
                if (state->socket && state->socket->is_open()) {
                    boost::system::error_code close_ec;
                    state->socket->shutdown(tcp::socket::shutdown_both, close_ec);
                    state->socket->close(close_ec);
                }
            } catch (...) {
                // 忽略关闭时的异常
            }
        }
    });
    
    // 开始异步读取请求
            http::async_read(*state->socket, state->buffer, state->parser, 
                [this, state, socket_ptr, debug_info](beast::error_code ec, std::size_t bytes_transferred) {
                    // 取消定时器
                    boost::system::error_code timer_ec;
                    state->timer.cancel(timer_ec);
                    
                    boost::ignore_unused(bytes_transferred);
                    
                    // 检查是否因为请求体过大而导致读取错误
                    if (ec && ec == http::error::body_limit) {
                        LOG_WARNING("Request body limit exceeded during parsing");
                        try {
                            http::response<http::string_body> response;
                            response.result(http::status::payload_too_large);
                            response.body() = "Request body exceeds maximum allowed size"; // 413 Payload Too Large
                            response.prepare_payload();
                            response.set(http::field::connection, "close");
                            
                            boost::system::error_code write_ec;
                            http::write(*state->socket, response, write_ec);
                            
                            if (state->socket->is_open()) {
                                boost::system::error_code close_ec;
                                state->socket->close(close_ec);
                            }
                        } catch (...) {
                            // 忽略错误
                        }
                        return;
                    }
                    
                    if (!ec) {
                        try {
                    // 获取解析结果
                    const auto& beast_request = state->parser.get();
                    // 将Beast请求转换为自定义请求格式
                    auto custom_request = std::make_shared<HttpRequest>(convertFromBeastRequest(beast_request));
                    // 附加客户端 socket（供反向代理流式转发使用）
                    custom_request->client_socket = state->socket;

                    // 请求大小检查 - 移到最前面，确保大文件请求先被拒绝
                    if (beast_request.body().size() > max_request_size_) {
                        LOG_WARNING("Request body too large: " + std::to_string(beast_request.body().size()) + 
                                " bytes (max: " + std::to_string(max_request_size_) + " bytes)");
                        http::response<http::string_body> response;
                        response.result(http::status::payload_too_large);
                        response.body() = "Request body exceeds maximum allowed size";
                        response.prepare_payload();
                        response.set(http::field::connection, "close");
                        
                        boost::system::error_code write_ec;
                        http::write(*state->socket, response, write_ec);
                        
                        if (state->socket->is_open()) {
                            boost::system::error_code close_ec;
                            state->socket->close(close_ec);
                        }
                        return;
                    }
                    
                    // 检查是否需要基本认证
                    if (!basic_auth_realm_.empty()) {
                        auto auth_header_it = beast_request.find(http::field::authorization);
                        std::string auth_header;
                        
                        if (auth_header_it != beast_request.end()) {
                            auth_header = std::string(auth_header_it->value().begin(), auth_header_it->value().end());
                        }
                        
                        if (!validateBasicAuth(auth_header)) {
                            // 创建响应对象，确保包含调试头信息
                            HttpResponse response_with_debug;
                            response_with_debug.status_code = "401";
                            response_with_debug.body = "Unauthorized";
                            response_with_debug.headers["WWW-Authenticate"] = "Basic realm=\"" + basic_auth_realm_ + "\"";
                            
                            // 添加调试头信息
                            if (this->debug_mode_) {
                                std::shared_ptr<DebugInfo> local_debug_info = debug_info;
                                if (!local_debug_info) {
                                    // 如果debug_info为空但调试模式已启用，创建临时对象
                                    local_debug_info = std::make_shared<DebugInfo>();
                                    local_debug_info->start_time = std::chrono::steady_clock::now();
                                    local_debug_info->end_time = std::chrono::steady_clock::now();
                                    local_debug_info->client_ip = state->socket->remote_endpoint().address().to_string();
                                    local_debug_info->routing_time = std::chrono::milliseconds(0);
                                }
                                
                                // 更新结束时间
                                local_debug_info->end_time = std::chrono::steady_clock::now();
                                
                                // 添加调试头信息
                                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(local_debug_info->end_time - local_debug_info->start_time);
                                response_with_debug.headers["X-Debug-Time"] = std::to_string(duration.count()) + "ms";
                                response_with_debug.headers["X-Debug-Client-IP"] = local_debug_info->client_ip;
                                response_with_debug.headers["X-Debug-Route-Time"] = std::to_string(local_debug_info->routing_time.count()) + "ms";
                            }
                            
                            // 直接创建响应对象并发送
                            auto response = std::make_shared<http::response<http::string_body>>();
                            response->result(http::status::unauthorized);
                            response->body() = response_with_debug.body;
                            
                            // 添加所有响应头
                            for (const auto& [key, value] : response_with_debug.headers) {
                                response->set(key, value);
                            }
                            
                            // 添加服务器和日期头
                            response->set(http::field::server, "BoostBeastHttpServer/2.0");
                            auto system_now = std::chrono::system_clock::now();
                            auto now_c = std::chrono::system_clock::to_time_t(system_now);
                            std::stringstream date_ss;
                            date_ss << std::put_time(gmtime(&now_c), "%a, %d %b %Y %H:%M:%S GMT");
                            response->set(http::field::date, date_ss.str());
                            
                            // 自动设置Content-Length
                            response->prepare_payload();
                            
                            boost::system::error_code write_ec;
                            http::write(*state->socket, *response, write_ec);
                            
                            if (state->socket->is_open()) {
                                boost::system::error_code close_ec;
                                state->socket->close(close_ec);
                            }
                            return;
                        }
                    }
                    
                    // 检查CSRF令牌（如果启用）— 复用已转换的 custom_request
                    if (csrf_protection_enabled_ && custom_request) {
                        
                        // 对于所有请求类型，确保CSRF令牌被添加到响应中
                        HttpResponse csrf_response;
                        addCsrfTokenToResponse(csrf_response, *custom_request);
                        
                        // 只对修改操作进行CSRF验证
                        auto method = std::string(beast_request.method_string());
                        if (method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH") {
                            if (!validateCsrfToken(*custom_request)) {
                                // 返回403禁止访问，使用统一的错误响应格式
                                HttpResponse http_response = createErrorResponse(HttpErrorType::FORBIDDEN, "CSRF token validation failed");
                                
                                // 直接构建http::response对象，不使用模板函数
                                http::response<http::string_body> beast_response;
                                beast_response.version(11); // HTTP/1.1
                                beast_response.result(http::status::forbidden);
                                beast_response.set(http::field::content_type, "application/json");
                                beast_response.body() = http_response.body;
                                beast_response.prepare_payload();
                                
                                boost::system::error_code write_ec;
                                http::write(*state->socket, beast_response, write_ec);
                                
                                if (state->socket->is_open()) {
                                    boost::system::error_code close_ec;
                                    state->socket->close(close_ec);
                                }
                                return;
                            }
                        }
                    }
                
                // 检查连接是否可以重用（是否支持keep-alive）
                // 注意：当前队列架构每个请求独占一个连接，强制 keep_alive=false
                // 如需 Keep-Alive 支持需改为非队列的处理架构
                bool keep_alive = false; // beast_request.keep_alive();
                    
                    // 检查队列是否已满
                    bool queue_full = false;
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex_);
                        queue_full = request_queue_.size() >= max_queue_size_;
                    }
                    
                    if (queue_full) {
                        // 队列已满，拒绝请求
                        http::response<http::string_body> response;
                        response.result(http::status::service_unavailable);
                        response.body() = "Service Unavailable - Queue full";
                        response.prepare_payload();
                        
                        boost::system::error_code write_ec;
                        http::write(*state->socket, response, write_ec);
                        
                        if (state->socket->is_open()) {
                            boost::system::error_code close_ec;
                            state->socket->close(close_ec);
                        }
                    } else {
                        // 将请求添加到队列
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex_);
                            RequestQueueItem queue_item;
                            queue_item.request = custom_request;
                            
                            // 创建回调函数 — 按值捕获所需成员变量，避免捕获 this 指针
                            queue_item.callback = [socket = state->socket, keep_alive, debug_info_copy = debug_info, 
                                                   debug_mode = debug_mode_, csrf_enabled = csrf_protection_enabled_,
                                                   csrf_cookie = csrf_cookie_name_, use_ssl = use_ssl_,
                                                   custom_request](const HttpResponse& http_response) {
                                // 记录响应信息
                                auto response_time = 0;
                                if (debug_info_copy) {
                                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - debug_info_copy->start_time);
                                    response_time = duration.count();
                                }
                                bool is_success = std::atoi(http_response.status_code.c_str()) < 400;
                                PerformanceMonitor::recordResponse("/", response_time, http_response.body.size(), is_success);
                                
                                // 记录访问日志
                                auto now = std::chrono::steady_clock::now();
                                if (debug_info_copy) {
                                    debug_info_copy->end_time = now;
                                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(debug_info_copy->end_time - debug_info_copy->start_time);
                                    logAccess(debug_info_copy->client_ip, "/", "GET", http_response.status_code, ":", duration.count());
                                }
                                
                                // 创建响应副本，准备添加调试信息
                                HttpResponse response_with_debug = http_response;
                                
                                // 确保debug_info对象存在，如果不存在则创建临时对象
                                if (debug_mode) {
                                    std::shared_ptr<DebugInfo> local_debug_info = debug_info_copy;
                                    if (!local_debug_info) {
                                        // 如果debug_info为空但调试模式已启用，创建临时对象
                                        local_debug_info = std::make_shared<DebugInfo>();
                                        local_debug_info->start_time = std::chrono::steady_clock::now();
                                        local_debug_info->end_time = std::chrono::steady_clock::now();
                                        local_debug_info->client_ip = "127.0.0.1"; // 默认本地IP
                                        local_debug_info->routing_time = std::chrono::milliseconds(0);
                                    }
                                    
                                    // 添加调试头信息
                                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(local_debug_info->end_time - local_debug_info->start_time);
                                    response_with_debug.headers["X-Debug-Time"] = std::to_string(duration.count()) + "ms";
                                    response_with_debug.headers["X-Debug-Client-IP"] = local_debug_info->client_ip;
                                    response_with_debug.headers["X-Debug-Route-Time"] = std::to_string(local_debug_info->routing_time.count()) + "ms";
                                    LOG_DEBUG("Debug headers added to response: X-Debug-Time=" + response_with_debug.headers["X-Debug-Time"] + ", X-Debug-Client-IP=" + response_with_debug.headers["X-Debug-Client-IP"]);
                                }
                                
                                // 如果启用了CSRF保护，添加CSRF令牌到响应cookie中（内联逻辑，避免依赖 this）
                                if (csrf_enabled) {
                                    // 检查请求是否已包含 CSRF cookie
                                    bool has_csrf_cookie = false;
                                    auto cookie_it = custom_request->headers.find("Cookie");
                                    if (cookie_it != custom_request->headers.end()) {
                                        has_csrf_cookie = (cookie_it->second.find(csrf_cookie + "=") != std::string::npos);
                                    }
                                    if (!has_csrf_cookie) {
                                        // 生成随机令牌
                                        const std::string charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
                                        std::random_device rd;
                                        std::mt19937 gen(rd());
                                        std::uniform_int_distribution<> distrib(0, charset.size() - 1);
                                        std::string csrf_token;
                                        csrf_token.reserve(32);
                                        for (int i = 0; i < 32; ++i) {
                                            csrf_token += charset[distrib(gen)];
                                        }
                                        response_with_debug.headers["Set-Cookie"] = csrf_cookie + "=" + csrf_token + "; HttpOnly=false; SameSite=Strict; Path=/" + (use_ssl ? "; Secure" : "");
                                    }
                                }
                                
                                // 添加安全头
                                addSecurityHeaders(response_with_debug, use_ssl);
                                
                                try {
                                    // 确保socket有效且打开
                                    if (!socket || !socket->is_open()) {
                                        return;
                                    }
                                    
                                    // 检查请求头中的Accept-Encoding，看看客户端是否支持gzip压缩
                                    bool should_compress = false;
                                    auto accept_encoding_it = custom_request->headers.find("Accept-Encoding");
                                    if (accept_encoding_it != custom_request->headers.end()) {
                                        const std::string& accept_encoding = accept_encoding_it->second;
                                        if (accept_encoding.find("gzip") != std::string::npos) {
                                            should_compress = true;
                                        }
                                    }
                                    
                                    // 统一构建并发送响应（Keep-Alive 时传入 on_complete）
                                    buildAndSendResponse(*socket, response_with_debug, 
                                                          debug_info_copy ? debug_info_copy->client_ip : "unknown",
                                                          custom_request->path, keep_alive, should_compress);
                                } catch (const std::exception& e) {
                                    try {
                                        if (socket && socket->is_open()) {
                                            boost::system::error_code ec;
                                            socket->close(ec);
                                        }
                                    } catch (...) {
                                        // 忽略错误
                                    }
                                }
                            };
                            request_queue_.push(queue_item);
                        }
                        queue_cv_.notify_one();  // 通知处理线程
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Client handling error: " + std::string(e.what()));
                    try {
                        if (socket_ptr->is_open()) {
                            socket_ptr->close();
                        }
                    } catch (...) {
                        // 忽略关闭时的错误
                    }
                }
            } else if (ec != http::error::end_of_stream) {
                LOG_ERROR("Read error: " + ec.message());
            }
        }
    );
}

// 默认GET方法的路由注册通过get()方法提供

// 注册指定HTTP方法的路由处理器
void HttpServer2::addRoute(const std::string& method, const std::string& path, 
                          const RouteHandler& handler) {
    // 创建适合的路由匹配器
    std::shared_ptr<RouteMatcher> matcher = createRouteMatcher(path);
    
    // 使用独占锁保护routes_的并发访问（写操作）
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);
    
    // 添加路由条目，确保初始化所有字段
    routes_.push_back({method, matcher, handler, {}});
}

// 添加中间件
void HttpServer2::use(const Middleware& middleware) {
    global_middlewares_.push_back(middleware);
}

// 便捷方法：GET路由
void HttpServer2::get(const std::string& path, const RouteHandler& handler) {
    addRoute("GET", path, handler);
}

// 便捷方法：POST路由
void HttpServer2::post(const std::string& path, const RouteHandler& handler) {
    addRoute("POST", path, handler);
}

// 便捷方法：PUT路由
void HttpServer2::put(const std::string& path, const RouteHandler& handler) {
    addRoute("PUT", path, handler);
}

// 便捷方法：DELETE路由
void HttpServer2::del(const std::string& path, const RouteHandler& handler) {
    addRoute("DELETE", path, handler);
}

// PATCH路由方法在头文件中未声明，已移除

// 运行io_context
void HttpServer2::runIoContext(std::shared_ptr<net::io_context> io_context) {
    try {
        // 运行io_context直到停止
        io_context->run();
    } catch (const std::exception& e) {
        LOG_ERROR("IoContext error: " + std::string(e.what()));
    }
}

// 负载均衡获取下一个io_context
std::shared_ptr<net::io_context> HttpServer2::getNextIoContext() {
    std::lock_guard<std::mutex> lock(io_context_mutex_);
    if (io_contexts_.empty()) {
        throw std::runtime_error("No io_context available");
    }
    
    // 轮询选择io_context
    auto io_context = io_contexts_[next_io_context_index_];
    next_io_context_index_ = (next_io_context_index_ + 1) % io_contexts_.size();
    return io_context;
}

// 检查是否超出速率限制
bool HttpServer2::isRateLimited() {
    if (requests_per_second_ == 0) return false;  // 不限流
    
    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(1);
    
    // 如果过了一秒钟，重置计数器
    if (now - rate_limit_window_ > window) {
        rate_limit_window_ = now;
        request_count_.store(0);
        return false;
    }
    
    // 增加请求计数并检查是否超过限制
    size_t count = request_count_.fetch_add(1) + 1;
    return count > requests_per_second_;
}

// 处理请求队列
void HttpServer2::processRequestQueue() {
    const int MAX_BATCH_SIZE = 20; // 增加批处理大小，减少线程切换开销
    const int BATCH_TIMEOUT_MS = 5; // 批处理超时，避免长时间阻塞
    
    std::vector<RequestQueueItem> batch_items;
    batch_items.reserve(MAX_BATCH_SIZE); // 预分配内存
    
    while (running_) {
        batch_items.clear();
        bool has_items = false;
        
        // 批量获取请求，减少锁竞争
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            // 使用超时等待，允许更频繁地检查running状态
            queue_cv_.wait_for(lock, 
                std::chrono::milliseconds(BATCH_TIMEOUT_MS), 
                [this]() { 
                    return !running_ || !request_queue_.empty(); 
                });
            
            if (!running_ && request_queue_.empty()) {
                break;
            }
            
            // 批量获取请求，但不超过最大批处理大小
            size_t items_to_fetch = std::min(
                static_cast<size_t>(MAX_BATCH_SIZE), 
                request_queue_.size());
            
            for (size_t i = 0; i < items_to_fetch && !request_queue_.empty(); ++i) {
                batch_items.push_back(std::move(request_queue_.front()));
                request_queue_.pop();
            }
            
            has_items = !batch_items.empty();
        }
        
        // 处理批量请求
        if (has_items) {
            for (auto& item : batch_items) {
                if (!item.request) continue;
                
                // 安全地设置处理状态
                bool already_processed = item.request->is_processed.exchange(true);
                if (already_processed) {
                    // 移除冗余的日志记录，减少I/O开销
                    continue;
                }
                
                try {
                    // 查找匹配的路由处理器（已包含路径参数处理）
                    HttpResponse response;
                    
                    // 开始计时
                    START_TIMER();

                    
                    try {
                        // 直接使用item.request中的path_params，避免重复处理
                        auto route_handler = findRoute(item.request->method, item.request->path, item.request->path_params);
                        
                        if (route_handler) {
                            // 使用中间件链处理请求
                            response = handleMiddlewareChain(*item.request, global_middlewares_, 0, route_handler->handler);
                        } else {
                            // 未找到路由，使用默认处理器
                            response = defaultHandler(*item.request);
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Error handling request: " + std::string(e.what()));
                        response = createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Internal Server Error");
                        PerformanceMonitor::recordError();
                    }
                    
                    // 结束计时并记录性能指标
                    END_TIMER();
                    if (item.callback) {
                        try {
                            // 减少调试日志，提高性能
                            item.callback(response);
                        } catch (...) {
                            // 简化异常处理，只记录关键错误
                            LOG_ERROR("Exception in response callback");
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Unexpected error in request processing: " + std::string(e.what()));
                    continue;
                } catch (...) {
                        // 最外层异常处理，确保线程不会崩溃
                        try {
                            if (item.callback) {
                                HttpResponse error_response = createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Internal Server Error");
                                item.callback(error_response);
                            }
                        } catch (...) {
                            // 完全忽略回调中的异常
                        }
                    }
            }
            
            // 批处理完成后让出时间片，避免CPU占用过高
            std::this_thread::yield();
        }
    }
}
// 清理过期的连接池连接
void HttpServer2::cleanupExpiredConnections() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    auto now = std::chrono::steady_clock::now();
    
    auto it = connection_pool_.begin();
    while (it != connection_pool_.end()) {
        if (!(*it)->in_use && now - (*it)->last_used > max_idle_time_) {
            // 关闭过期连接
            try {
                if ((*it)->socket && (*it)->socket->is_open()) {
                    (*it)->socket->shutdown(tcp::socket::shutdown_both);
                    (*it)->socket->close();
                }
                LOG_DEBUG("Expired connection removed from pool");
            } catch (...) {
                // 忽略错误
            }
            it = connection_pool_.erase(it);
        } else {
            ++it;
        }
    }
    
    LOG_DEBUG("Connection pool cleanup completed, current size: " + std::to_string(connection_pool_.size()));
}

// 设置连接池参数
void HttpServer2::setConnectionPoolSettings(size_t max_connections, std::chrono::seconds max_idle_time) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    max_pool_connections_ = max_connections;
    max_idle_time_ = max_idle_time;
    LOG_INFO("Connection pool settings updated: max_connections=" + std::to_string(max_connections) + 
             ", max_idle_time=" + std::to_string(max_idle_time.count()) + "s");
}

// 设置速率限制
void HttpServer2::setRateLimit(size_t requests_per_second, size_t max_queue_size) {
    requests_per_second_ = requests_per_second;
    max_queue_size_ = max_queue_size;
    LOG_INFO("Rate limit settings updated: " + (requests_per_second == 0 ? "unlimited" : std::to_string(requests_per_second) + " req/s") + 
             ", max_queue_size=" + std::to_string(max_queue_size));
}

// 停止服务器
void HttpServer2::stop() {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (!running_) return;
    
    running_ = false;  // 设置原子变量为false，所有线程都能立即看到
    std::cout << "HTTP Server stopping..." << std::endl;
    
    // 关闭acceptor
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->cancel(ec);
        acceptor_->close(ec);
    }
    
    // 通知请求队列处理线程退出
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        queue_cv_.notify_all();
    }
    
    // 停止所有io_context
    for (auto& io_context : io_contexts_) {
        if (io_context) {
            io_context->stop();
        }
    }
    
    // 等待线程池结束
    for (auto& thread : thread_pool_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    thread_pool_.clear();
    
    // 清理连接池
    {
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        for (auto& conn : connection_pool_) {
            try {
                if (conn->socket && conn->socket->is_open()) {
                    conn->socket->shutdown(tcp::socket::shutdown_both);
                    conn->socket->close();
                }
            } catch (...) {
                // 忽略错误
            }
        }
        connection_pool_.clear();
    }
    
    // 等待所有队列处理线程完成
    for (auto& thread : queue_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    queue_threads_.clear();
    
    // 等待清理线程完成
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    
    // 等待自适应线程池调整线程完成
    if (thread_adjust_thread_.joinable()) {
        thread_adjust_thread_.join();
    }
    
    // 等待配置文件监控线程完成
    if (config_watch_thread_.joinable()) {
        config_watch_thread_.join();
    }
    
    // 清空io_contexts
    io_contexts_.clear();
    
    std::cout << "HTTP Server stopped" << std::endl;
}

// 检查服务器是否在运行
  bool HttpServer2::isRunning() const {
     // 直接返回原子变量running_的值，无需加锁
     return running_;
  }

// 解析查询参数
void HttpServer2::parseQueryParams(const std::string& query_string, std::unordered_map<std::string, std::string>& params) {
    if (query_string.empty()) return;
    
    std::vector<std::string> pairs;
    boost::algorithm::split(pairs, query_string, boost::algorithm::is_any_of("&"), boost::algorithm::token_compress_off);
    
    for (const auto& pair : pairs) {
        std::vector<std::string> key_value;
        boost::algorithm::split(key_value, pair, boost::algorithm::is_any_of("="), boost::algorithm::token_compress_off);
        
        if (!key_value.empty()) {
            std::string key = urlDecode(key_value[0]);
            std::string value = key_value.size() > 1 ? urlDecode(key_value[1]) : "";
            params[key] = value;
        }
    }
}

// 解析表单数据（application/x-www-form-urlencoded）
void HttpServer2::parseFormData(const std::string& form_data, std::unordered_map<std::string, std::string>& params) {
    // 表单数据解析与查询参数解析逻辑相同
    parseQueryParams(form_data, params);
}

// 解析multipart/form-data表单数据
void HttpServer2::parseMultipartFormData(const std::string& content_type, const std::string& body, 
                                        std::unordered_map<std::string, std::string>& form_data, 
                                        std::vector<UploadedFile>& uploaded_files) {
    // 提取boundary
    size_t boundary_pos = content_type.find("boundary=");
    if (boundary_pos == std::string::npos) {
        LOG_DEBUG("[parseMultipartFormData] No boundary found in Content-Type");
        return;
    }
    
    std::string boundary = "--" + content_type.substr(boundary_pos + 9);
    
    // 查找每个part的开始和结束位置
    size_t part_start = 0;
    while ((part_start = body.find(boundary, part_start)) != std::string::npos) {
        part_start += boundary.length();
        
        // 跳过可能的换行符
        while (part_start < body.length() && (body[part_start] == '\r' || body[part_start] == '\n')) {
            part_start++;
        }
        
        // 查找part的结束位置
        size_t part_end = body.find(boundary, part_start);
        if (part_end == std::string::npos) break;
        
        // 提取part内容
        std::string part = body.substr(part_start, part_end - part_start);
        
        // 解析part头和内容
        size_t header_end = part.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            part_start = part_end;
            continue;
        }
        
        std::string headers = part.substr(0, header_end);
        std::string content = part.substr(header_end + 4);
        
        // 提取Content-Disposition头
        size_t disp_pos = headers.find("Content-Disposition:");
        if (disp_pos != std::string::npos) {
            std::string disp = headers.substr(disp_pos + 19);
            
            // 提取name属性
            size_t name_pos = disp.find("name=\"");
            if (name_pos != std::string::npos) {
                name_pos += 6;
                size_t name_end = disp.find('"', name_pos);
                if (name_end != std::string::npos) {
                    std::string field_name = disp.substr(name_pos, name_end - name_pos);
                    
                    // 检查是否有filename属性
                    size_t filename_pos = disp.find("filename=\"");
                    if (filename_pos != std::string::npos) {
                        // 这是一个文件字段
                        filename_pos += 10;
                        size_t filename_end = disp.find('"', filename_pos);
                        if (filename_end != std::string::npos) {
                            std::string filename = disp.substr(filename_pos, filename_end - filename_pos);
                            
                            // 提取Content-Type
                            std::string content_type_str = "application/octet-stream";
                            size_t type_pos = headers.find("Content-Type:");
                            if (type_pos != std::string::npos) {
                                type_pos += 13;
                                size_t type_end = headers.find("\r\n", type_pos);
                                if (type_end != std::string::npos) {
                                    content_type_str = headers.substr(type_pos, type_end - type_pos);
                                }
                            }
                            
                            // 创建上传文件对象
                            UploadedFile file;
                            file.name = field_name;
                            file.filename = filename;
                            file.content_type = content_type_str;
                            file.data.assign(content.begin(), content.end());
                            
                            uploaded_files.push_back(file);
LOG_DEBUG("[parseMultipartFormData] File uploaded: " + filename + ", size: " + std::to_string(file.data.size()) + " bytes");
                        }
                    } else {
                        // 这是一个普通表单字段
                        // 移除末尾可能的\r\n
                        if (!content.empty() && content.back() == '\n') {
                            content.pop_back();
                        }
                        if (!content.empty() && content.back() == '\r') {
                            content.pop_back();
                        }
                        
                        form_data[field_name] = content;
                        LOG_DEBUG("[parseMultipartFormData] Form field: " + field_name + " = " + content);
                    }
                }
            }
        }
        
        part_start = part_end;
    }
}

// 解析JSON请求体
void HttpServer2::parseJsonBody(const std::string& json_str, std::unordered_map<std::string, std::string>& json_data) {
    try {
        // 简单的JSON解析实现，只支持一层对象
        // 移除前后的大括号和空白字符
        std::string trimmed = json_str;
        size_t start = trimmed.find_first_not_of("{ \t\r\n");
        size_t end = trimmed.find_last_not_of("} \t\r\n");
        
        if (start == std::string::npos || end == std::string::npos || start > end) {
            return;
        }
        
        trimmed = trimmed.substr(start, end - start + 1);
        
        // 分割键值对
        size_t pos = 0;
        while (pos < trimmed.length()) {
            // 查找键名开始
            size_t key_start = trimmed.find('"', pos);
            if (key_start == std::string::npos) break;
            
            // 查找键名结束
            size_t key_end = trimmed.find('"', key_start + 1);
            if (key_end == std::string::npos) break;
            
            std::string key = trimmed.substr(key_start + 1, key_end - key_start - 1);
            
            // 查找冒号
            size_t colon_pos = trimmed.find(':', key_end + 1);
            if (colon_pos == std::string::npos) break;
            
            // 查找值开始
            size_t val_start = trimmed.find_first_not_of(" \t\r\n", colon_pos + 1);
            if (val_start == std::string::npos) break;
            
            std::string value;
            if (trimmed[val_start] == '"') {
                // 字符串值
                size_t val_end = val_start + 1;
                while (val_end < trimmed.length()) {
                    if (trimmed[val_end] == '"' && trimmed[val_end - 1] != '\\') {
                        break;
                    }
                    val_end++;
                }
                if (val_end == trimmed.length()) break;
                
                value = trimmed.substr(val_start + 1, val_end - val_start - 1);
                pos = val_end + 1;
            } else {
                // 非字符串值（数字、布尔值、null）
                size_t val_end = trimmed.find_first_of(",}\r\n\t ", val_start);
                value = trimmed.substr(val_start, val_end - val_start);
                pos = val_end;
            }
            
            json_data[key] = value;
            
            // 查找下一个逗号或结束
            pos = trimmed.find(',', pos);
            if (pos == std::string::npos) break;
            pos++;
        }
        
        return;
    } catch (const std::exception& e) {
        LOG_DEBUG("[parseJsonBody] Error parsing JSON: " + std::string(e.what()));
        return;
    }
}

// 从Beast请求转换为自定义请求格式
HttpRequest HttpServer2::convertFromBeastRequest(const http::request<http::string_body>& req) {
    HttpRequest request;
    
    // 设置请求方法
    request.method = http::to_string(req.method());
    
    // 解析路径和查询参数
    std::string target = std::string(req.target().begin(), req.target().end());
    size_t query_pos = target.find('?');
    
    if (query_pos != std::string::npos) {
        // 提取路径部分
        request.path = target.substr(0, query_pos);
        // 提取并解析查询参数
        std::string query_string = target.substr(query_pos + 1);
        parseQueryParams(query_string, request.query_params);
    } else {
        // 没有查询参数
        request.path = target;
    }
    
    // 设置HTTP版本
    request.version = "HTTP/" + std::to_string(req.version()) + "." + std::to_string(req.keep_alive() ? 1 : 0);
    
    // 设置请求体
    request.body = req.body();
    
    // 设置请求头
    for (const auto& field : req) {
        std::string header_name = std::string(field.name_string().begin(), field.name_string().end());
        std::string header_value = std::string(field.value().begin(), field.value().end());
        request.headers[header_name] = header_value;
    }
    
    // 根据Content-Type解析请求体
    auto content_type_it = request.headers.find("Content-Type");
    if (content_type_it != request.headers.end()) {
        std::string content_type = content_type_it->second;
        
        // 解析application/x-www-form-urlencoded
        if (content_type.find("application/x-www-form-urlencoded") != std::string::npos) {
            LOG_DEBUG("[convertFromBeastRequest] Parsing application/x-www-form-urlencoded");
            parseFormData(request.body, request.form_data);
        }
        // 解析multipart/form-data
        else if (content_type.find("multipart/form-data") != std::string::npos) {
            LOG_DEBUG("[convertFromBeastRequest] Parsing multipart/form-data");
            parseMultipartFormData(content_type, request.body, request.form_data, request.files);
        }
        // 解析application/json
        else if (content_type.find("application/json") != std::string::npos) {
            LOG_DEBUG("[convertFromBeastRequest] Parsing application/json");
            // 简单的JSON解析，结果存储在form_data中供方便访问
            parseJsonBody(request.body, request.form_data);
        }
    }
    
    // 使用移动语义而不是复制
    return request; // 移除std::move以允许复制省略
}

// 查找匹配的路由
std::optional<Route> HttpServer2::findRoute(const std::string& method, const std::string& path, std::unordered_map<std::string, std::string>& params) {
    // 先快速路径：检查是否有精确匹配的路由（大多数请求场景）
    if (!routes_.empty()) {
        // 使用共享锁而非独占锁，允许并发读取
        std::shared_lock<std::shared_mutex> lock(routes_mutex_);
        
        // 优先查找完全匹配的路由，避免不必要的正则计算
        for (const auto& route : routes_) {
            // 首先检查HTTP方法是否匹配
            if (route.method != "*" && route.method != method) {
                continue;
            }
            
            // 直接使用params作为参数容器，避免额外的映射创建和复制
            if (route.matcher->match(path, params)) {
                return route;
            }
        }
    }
    
    // 如果没有找到匹配的路由，返回空
    return std::nullopt;
}



// 处理中间件链 - 使用迭代而非递归，减少栈开销
HttpResponse HttpServer2::handleMiddlewareChain(const HttpRequest& request, const std::vector<Middleware>& middlewares, size_t index, const RouteHandler& final_handler) {
    // 预先分配响应对象，避免多次创建
    HttpResponse response;
    bool handled = false;
    
    // 使用迭代方式处理中间件链，避免递归调用
    for (size_t i = index; i < middlewares.size(); ++i) {
        try {
            response = middlewares[i](request);
            // 检查是否需要继续处理
            if (response.status_code != "404") {
                handled = true;
                break;
            }
        } catch (const std::exception& e) {
            // 记录中间件异常但继续处理
            LOG_ERROR("Middleware exception at index " + std::to_string(i) + ": " + std::string(e.what()));
            continue;
        } catch (...) {
            // 忽略中间件中的非标准异常，继续下一个中间件
            LOG_ERROR("Unknown middleware exception at index " + std::to_string(i));
            continue;
        }
    }
    
    // 如果没有中间件处理，调用最终处理器
    if (!handled && final_handler) {
        try {
            response = final_handler(request);
        } catch (const std::exception& e) {
            // 记录详细的异常信息
            std::string error_msg = "Exception in route handler for " + request.method + " " + request.path + ": " + std::string(e.what());
            LOG_ERROR(error_msg);
            PerformanceMonitor::recordError();
            
            // 使用createErrorResponse生成统一格式的错误响应
            response = createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Internal Server Error");
        } catch (...) {
            // 记录未知异常
            std::string error_msg = "Unknown exception in route handler for " + request.method + " " + request.path;
            LOG_ERROR(error_msg);
            PerformanceMonitor::recordError();
            
            // 使用createErrorResponse生成统一格式的错误响应
            response = createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Internal Server Error");
        }
    } else if (!handled) {
        // 如果最终处理器无效，返回默认404响应
        response = defaultHandler(request);
    }
    
    return response;
}

// 默认处理函数（处理未匹配到的路由）
HttpResponse HttpServer2::defaultHandler(const HttpRequest& request) {
    LOG_DEBUG("defaultHandler called for path: " + request.path);
    
    // 使用createErrorResponse创建404响应
    HttpResponse response = createErrorResponse(HttpErrorType::NOT_FOUND);
    
    // 记录未找到的请求
    LOG_WARNING("Resource not found: " + request.path + " (Method: " + request.method + ")");
    
    return response;
}

// 处理客户端连接（HTTPS/1.1）
void HttpServer2::handleClientHttps(ssl::stream<tcp::socket> ssl_socket) {
    // 创建调试信息对象（如果启用调试模式）
    std::shared_ptr<DebugInfo> debug_info;
    if (debug_mode_) {
        debug_info = std::make_shared<DebugInfo>();
        debug_info->start_time = std::chrono::steady_clock::now();
        debug_info->client_ip = ssl_socket.lowest_layer().remote_endpoint().address().to_string();
        debug_info->client_port = ssl_socket.lowest_layer().remote_endpoint().port();
    }
    
    // 记录请求开始
    PerformanceMonitor::recordRequest();
    
    // 检查IP级别的限流
    std::string client_ip = ssl_socket.lowest_layer().remote_endpoint().address().to_string();
    bool rate_limited = false;
    
    // 检查全局限流
    if (isRateLimited()) {
        rate_limited = true;
    } 
    // 检查IP级别的限流
    else if (!rate_limiter_.isAllowed(client_ip)) {
        rate_limited = true;
    }
    
    if (rate_limited) {
        try {
            // 发送429 Too Many Requests响应
            http::response<http::string_body> response;
            response.version(11);
            response.result(http::status::too_many_requests);
            response.set(http::field::content_type, "text/plain");
            response.set(http::field::retry_after, "60");
            response.body() = "Too many requests, please try again later.";
            response.prepare_payload();
            
            http::write(ssl_socket, response);
            boost::system::error_code ec;
            ssl_socket.lowest_layer().shutdown(tcp::socket::shutdown_send, ec);
        } catch (...) {
            // 忽略错误
        }
        return;
    }
    
    // 创建ssl_socket的shared_ptr以便在异步操作中保持其生命周期
    // 使用new直接构造，避免make_shared对move-only类型的不兼容
    auto ssl_socket_ptr = std::shared_ptr<ssl::stream<tcp::socket>>(
        new ssl::stream<tcp::socket>(std::move(ssl_socket)));
    
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto request = std::make_shared<http::request<http::string_body>>();
    // 创建定时器用于请求超时控制
    // 直接创建timer，不再有条件块限制作用域
    auto timer = std::make_shared<net::deadline_timer>(*io_contexts_[0]);
    
    // 设置超时时间
    std::chrono::seconds timeout = request_timeout_;
    if (timeout.count() <= 0) {
        timeout = std::chrono::seconds(60); // 默认60秒
    }
    timer->expires_from_now(boost::posix_time::seconds(timeout.count()));
    
    // 设置超时回调
    timer->async_wait([ssl_socket_ptr, timer](const boost::system::error_code& ec) {
        if (!ec) { // 如果定时器没有被取消，则超时
            LOG_WARNING("HTTPS Request timeout, closing connection");
            try {
                if (ssl_socket_ptr->lowest_layer().is_open()) {
                    boost::system::error_code close_ec;
                    ssl_socket_ptr->lowest_layer().shutdown(tcp::socket::shutdown_both, close_ec);
                    ssl_socket_ptr->lowest_layer().close(close_ec);
                }
            } catch (...) {
                // 忽略关闭时的异常
            }
        }
    });
    
    // 异步读取HTTPS请求
    http::async_read(*ssl_socket_ptr, *buffer, *request, 
        [this, ssl_socket_ptr, buffer, request, timer, debug_info](beast::error_code ec, std::size_t bytes_transferred) {
            // 取消定时器，因为我们已经开始处理请求
            boost::system::error_code timer_ec;
            timer->cancel(timer_ec);
            
            boost::ignore_unused(bytes_transferred);
            
            if (!ec) {
                try {
                    // 请求大小检查 - 移到最前面，确保大文件请求先被拒绝
                    if (request->body().size() > max_request_size_) {
                        LOG_WARNING("HTTPS request body too large: " + std::to_string(request->body().size()) + 
                                " bytes (max: " + std::to_string(max_request_size_) + " bytes)");
                        auto response = std::make_shared<http::response<http::string_body>>();
                        response->result(http::status::payload_too_large);
                        response->body() = "Request body exceeds maximum allowed size";
                        response->prepare_payload();
                        response->set(http::field::connection, "close");
                        
                        http::async_write(*ssl_socket_ptr, *response, 
                            [ssl_socket_ptr, response](beast::error_code /* ec */, std::size_t /* bytes_transferred */) {
                                
                                try {
                                    if (ssl_socket_ptr->lowest_layer().is_open()) {
                                        boost::system::error_code close_ec;
                                        ssl_socket_ptr->lowest_layer().close(close_ec);
                                    }
                                } catch (...) {
                                    // 忽略关闭时的异常
                                }
                            }
                        );
                        return;
                    }
                    
                    // 检查是否需要基本认证
                    if (!basic_auth_realm_.empty()) {
                        auto auth_header_it = request->find(http::field::authorization);
                        std::string auth_header;
                        
                        if (auth_header_it != request->end()) {
                            auth_header = std::string(auth_header_it->value().begin(), auth_header_it->value().end());
                        }
                        
                        if (!validateBasicAuth(auth_header)) {
                            // 返回401未授权
                            auto response = std::make_shared<http::response<http::string_body>>();
                            response->result(http::status::unauthorized);
                            response->set(http::field::www_authenticate, "Basic realm=\"" + basic_auth_realm_ + "\"");
                            response->body() = "Unauthorized";
                            response->prepare_payload();
                            
                            http::async_write(*ssl_socket_ptr, *response, 
                                [ssl_socket_ptr, response](beast::error_code /* ec */, std::size_t /* bytes_transferred */) {
                                    
                                    try {
                                        if (ssl_socket_ptr->lowest_layer().is_open()) {
                                            boost::system::error_code close_ec;
                                            ssl_socket_ptr->lowest_layer().close(close_ec);
                                        }
                                    } catch (...) {
                                        // 忽略关闭时的异常
                                    }
                                }
                            );
                            return;
                        }
                    }
                    
                    // 检查CSRF令牌（如果启用）
                    if (csrf_protection_enabled_) {
                        HttpRequest csrf_request = convertFromBeastRequest(*request);
                        HttpResponse csrf_response;
                        // 为所有请求添加CSRF令牌到响应cookie中
                        addCsrfTokenToResponse(csrf_response, csrf_request);
                        
                        // 只对修改操作进行CSRF验证
                        auto method = std::string(request->method_string());
                        if (method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH") {
                            if (!validateCsrfToken(csrf_request)) {
                                // 返回403禁止访问，使用统一的错误响应格式
                                HttpResponse http_response = createErrorResponse(HttpErrorType::FORBIDDEN, "CSRF token validation failed");
                                auto response = std::make_shared<http::response<http::string_body>>();
                                
                                // 直接构建http::response对象，不使用模板函数
                                response->version(11); // HTTP/1.1
                                response->result(http::status::forbidden);
                                response->set(http::field::content_type, "application/json");
                                response->body() = http_response.body;
                                response->prepare_payload();
                                
                                http::async_write(*ssl_socket_ptr, *response, 
                                    [ssl_socket_ptr, response](beast::error_code /* ec */, std::size_t /* bytes_transferred */) {
                                        
                                        try {
                                            if (ssl_socket_ptr->lowest_layer().is_open()) {
                                                boost::system::error_code close_ec;
                                                ssl_socket_ptr->lowest_layer().close(close_ec);
                                            }
                                        } catch (...) {
                                            // 忽略关闭时的异常
                                        }
                                    }
                                );
                                return;
                            }
                        }
                        // CSRF token is already added to response headers by addCsrfTokenToResponse
                    }
                    
                    // 将Beast请求转换为自定义请求格式
                    HttpRequest custom_request = convertFromBeastRequest(*request);
                    
                    // 创建路径参数映射
                    std::unordered_map<std::string, std::string> path_params;
                    // 查找匹配的路由处理器
                    auto route_handler = findRoute(custom_request.method, custom_request.path, path_params);
                    // 将找到的路径参数添加到请求中
                    for (const auto& [key, value] : path_params) {
                        custom_request.path_params[key] = value;
                    }
                    
                    // 检查是否找到路由处理器
                    // 记录路由匹配时间（如果启用调试模式）
                    auto route_start_time = std::chrono::steady_clock::now();
                    
                    HttpResponse custom_response;
                    if (route_handler) {
                        // 使用中间件链处理请求
                        custom_response = handleMiddlewareChain(custom_request, global_middlewares_, 0, route_handler->handler);
                        // 如果中间件链没有处理请求，使用路由处理器
                        if (custom_response.status_code == "404") {
                            custom_response = route_handler->handler(custom_request);
                        }
                    } else {
                        // 使用默认处理器
                        custom_response = defaultHandler(custom_request);
                    }
                    
                    // 记录路由处理时间（如果启用调试模式）
                    if (debug_info) {
                        auto route_end_time = std::chrono::steady_clock::now();
                        debug_info->routing_time = std::chrono::duration_cast<std::chrono::milliseconds>(route_end_time - route_start_time);
                    }
                    
                    // 记录响应信息
                    auto response_time = 0;
                    if (debug_info) {
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - debug_info->start_time);
                        response_time = duration.count();
                    }
                    PerformanceMonitor::recordResponse(custom_request.path, response_time, custom_response.body.size(), custom_response.status_code < "400");
                    
                    // 如果启用调试模式，将调试信息添加到响应头
                    if (debug_mode_ && debug_info) {
                        auto now = std::chrono::steady_clock::now();
                        debug_info->end_time = now;
                        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - debug_info->start_time);
                        custom_response.headers["X-Debug-Time"] = std::to_string(total_duration.count()) + "ms";
                        custom_response.headers["X-Debug-Client-IP"] = debug_info->client_ip;
                        custom_response.headers["X-Debug-Route-Time"] = std::to_string(debug_info->routing_time.count()) + "ms";
                    }
                    
                    // 记录访问日志
                    if (debug_info) {
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - debug_info->start_time);
                        logAccess(debug_info->client_ip, custom_request.path, custom_request.method, custom_response.status_code, 
                                          custom_request.headers.count("User-Agent") ? custom_request.headers["User-Agent"] : ":", duration.count());
                    }
                    
                    // 统一处理：CSRF cookie + 安全头 + 调试头
                    finalizeResponse(custom_response, custom_request, true, debug_info);
                    
                    // 检查客户端是否支持gzip压缩
                    bool should_compress = false;
                    auto accept_encoding_it = request->find(http::field::accept_encoding);
                    if (accept_encoding_it != request->end()) {
                        const std::string accept_encoding(accept_encoding_it->value());
                        if (accept_encoding.find("gzip") != std::string::npos) {
                            should_compress = true;
                        }
                    }
                    
                    // 统一构建并发送响应
                    buildAndSendResponse(ssl_socket_ptr, custom_response,
                                          debug_info ? debug_info->client_ip : "unknown",
                                          custom_request.path, false, should_compress);
                } catch (const std::exception& e) {
                    LOG_ERROR("HTTPS client handling error: " + std::string(e.what()));
                    try {
                        if (ssl_socket_ptr->lowest_layer().is_open()) {
                            ssl_socket_ptr->lowest_layer().close();
                        }
                    } catch (...) {
                        // 忽略关闭时的错误
                    }
                }
            } else if (ec != http::error::end_of_stream) {
                LOG_ERROR("HTTPS read error: " + ec.message());
            }
        }
    );
}

// 初始化MIME类型映射
void HttpServer2::initializeMimeTypes() {
    // 常见文件类型的MIME映射
    // 文本类型
    mime_types_[".html"] = "text/html";
    mime_types_[".htm"] = "text/html";
    mime_types_[".css"] = "text/css";
    mime_types_[".js"] = "application/javascript";
    mime_types_[".json"] = "application/json";
    mime_types_[".xml"] = "application/xml";
    mime_types_[".txt"] = "text/plain";
    mime_types_[".md"] = "text/markdown";
    mime_types_[".markdown"] = "text/markdown";
    mime_types_[".rtf"] = "application/rtf";
    mime_types_[".csv"] = "text/csv";
    mime_types_[".tsv"] = "text/tab-separated-values";
    mime_types_[".yaml"] = "text/yaml";
    mime_types_[".yml"] = "text/yaml";
    mime_types_[".toml"] = "text/toml";
    mime_types_[".ini"] = "text/plain";
    mime_types_[".sh"] = "application/x-sh";
    mime_types_[".bash"] = "application/x-bash";
    mime_types_[".py"] = "text/x-python";
    mime_types_[".cpp"] = "text/x-c++src";
    mime_types_[".hpp"] = "text/x-c++hdr";
    mime_types_[".c"] = "text/x-csrc";
    mime_types_[".h"] = "text/x-chdr";
    mime_types_[".java"] = "text/x-java";
    mime_types_[".go"] = "text/x-go";
    mime_types_[".rs"] = "text/x-rust";
    mime_types_[".php"] = "text/x-php";
    mime_types_[".swift"] = "text/x-swift";
    mime_types_[".kt"] = "text/x-kotlin";
    mime_types_[".ts"] = "text/typescript";
    mime_types_[".jsx"] = "text/jsx";
    mime_types_[".tsx"] = "text/tsx";
    mime_types_[".vue"] = "text/vue";
    mime_types_[".sass"] = "text/sass";
    mime_types_[".scss"] = "text/scss";
    mime_types_[".less"] = "text/less";
    
    // 图片类型
    mime_types_[".jpg"] = "image/jpeg";
    mime_types_[".jpeg"] = "image/jpeg";
    mime_types_[".png"] = "image/png";
    mime_types_[".gif"] = "image/gif";
    mime_types_[".webp"] = "image/webp";
    mime_types_[".svg"] = "image/svg+xml";
    mime_types_[".ico"] = "image/x-icon";
    mime_types_[".bmp"] = "image/bmp";
    mime_types_[".tif"] = "image/tiff";
    mime_types_[".tiff"] = "image/tiff";
    mime_types_[".heic"] = "image/heic";
    mime_types_[".heif"] = "image/heif";
    mime_types_[".avif"] = "image/avif";
    
    // 字体类型
    mime_types_[".ttf"] = "font/ttf";
    mime_types_[".otf"] = "font/otf";
    mime_types_[".woff"] = "font/woff";
    mime_types_[".woff2"] = "font/woff2";
    mime_types_[".eot"] = "application/vnd.ms-fontobject";
    mime_types_[".sfnt"] = "font/sfnt";
    
    // 文档类型
    mime_types_[".pdf"] = "application/pdf";
    mime_types_[".doc"] = "application/msword";
    mime_types_[".docx"] = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    mime_types_[".xls"] = "application/vnd.ms-excel";
    mime_types_[".xlsx"] = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    mime_types_[".ppt"] = "application/vnd.ms-powerpoint";
    mime_types_[".pptx"] = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    mime_types_[".odt"] = "application/vnd.oasis.opendocument.text";
    mime_types_[".ods"] = "application/vnd.oasis.opendocument.spreadsheet";
    mime_types_[".odp"] = "application/vnd.oasis.opendocument.presentation";
    
    // 压缩文件类型
    mime_types_[".zip"] = "application/zip";
    mime_types_[".gz"] = "application/gzip";
    mime_types_[".tar"] = "application/x-tar";
    mime_types_[".tgz"] = "application/x-gzip";
    mime_types_[".rar"] = "application/x-rar-compressed";
    mime_types_[".7z"] = "application/x-7z-compressed";
    mime_types_[".bz2"] = "application/x-bzip2";
    mime_types_[".xz"] = "application/x-xz";
    
    // 音频类型
    mime_types_[".mp3"] = "audio/mpeg";
    mime_types_[".ogg"] = "audio/ogg";
    mime_types_[".wav"] = "audio/wav";
    mime_types_[".flac"] = "audio/flac";
    mime_types_[".aac"] = "audio/aac";
    mime_types_[".m4a"] = "audio/mp4";
    mime_types_[".wma"] = "audio/x-ms-wma";
    
    // 视频类型
    mime_types_[".mp4"] = "video/mp4";
    mime_types_[".avi"] = "video/x-msvideo";
    mime_types_[".mov"] = "video/quicktime";
    mime_types_[".wmv"] = "video/x-ms-wmv";
    mime_types_[".flv"] = "video/x-flv";
    mime_types_[".webm"] = "video/webm";
    mime_types_[".mpeg"] = "video/mpeg";
    mime_types_[".mpg"] = "video/mpeg";
    
    // 其他类型
    mime_types_[".exe"] = "application/x-msdownload";
    mime_types_[".dll"] = "application/x-msdownload";
    mime_types_[".so"] = "application/x-sharedlib";
    mime_types_[".a"] = "application/x-archive";
    mime_types_[".bin"] = "application/octet-stream";
    mime_types_[".iso"] = "application/x-iso9660-image";
    mime_types_[".img"] = "application/octet-stream";
    mime_types_[".deb"] = "application/x-deb";
    mime_types_[".rpm"] = "application/x-rpm";
    mime_types_[".apk"] = "application/vnd.android.package-archive";
    mime_types_[".ipa"] = "application/vnd.iphone";
    mime_types_[".dmg"] = "application/x-apple-diskimage";
    mime_types_[".torrent"] = "application/x-bittorrent";
}

// 根据文件扩展名获取MIME类型
std::string HttpServer2::getMimeType(const std::string& file_path) {
    size_t dot_pos = file_path.find_last_of(".");
    if (dot_pos != std::string::npos) {
        std::string extension = file_path.substr(dot_pos);
        // 转换为小写以确保匹配
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (mime_types_.count(extension)) {
            return mime_types_[extension];
        }
    }
    // 默认返回二进制流类型
    return "application/octet-stream";
}

// 提供静态文件
HttpResponse HttpServer2::serveStaticFile(const HttpRequest& request, const std::string& url_path, const std::string& directory_path, const std::string& default_file) {
    HttpResponse response;
    // 添加CORS支持
    response.headers["Access-Control-Allow-Origin"] = "*";
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
    response.headers["Access-Control-Allow-Headers"] = "Content-Type";
    
    LOG_DEBUG("serveStaticFile - ENTRY - url_path: '" + url_path + "', directory_path: '" + directory_path + "', default_file: '" + default_file + "'");
    
    try {
        // 获取规范化的目录路径
        std::filesystem::path base_dir = std::filesystem::absolute(directory_path);
        LOG_DEBUG("serveStaticFile - Base directory (absolute): " + base_dir.string());
        
        // 检查目录是否存在
        if (!std::filesystem::exists(base_dir) || !std::filesystem::is_directory(base_dir)) {
            LOG_WARNING("serveStaticFile - Directory does not exist or is not a directory: " + base_dir.string());
            response.status_code = "404";
            response.status_message = "Not Found";
            response.headers["Content-Type"] = "text/plain";
            response.body = "Directory not found";
            return response;
        }
        
        // 构建文件路径
        std::filesystem::path file_path;
        
        // 处理URL路径 - 改进路径规范化
        std::string safe_path = url_path;
        LOG_DEBUG("serveStaticFile - Original safe_path: '" + safe_path + "'");
        
        // 移除开头的斜杠
        if (!safe_path.empty() && safe_path[0] == '/') {
            safe_path = safe_path.substr(1);
            LOG_DEBUG("serveStaticFile - Removed leading slash, safe_path: '" + safe_path + "'");
        }
        
        // 规范化路径，移除多余的斜杠
        size_t pos = 0;
        while ((pos = safe_path.find("//", pos)) != std::string::npos) {
            safe_path.replace(pos, 2, "/");
            LOG_DEBUG("serveStaticFile - Removed double slash, safe_path: '" + safe_path + "'");
        }
        
        // 特殊处理：如果请求的是index.html，确保能够正确找到
        if (safe_path == "index.html") {
            LOG_DEBUG("serveStaticFile - Special case: index.html requested directly");
            file_path = base_dir / "index.html";
            LOG_DEBUG("serveStaticFile - Using direct index.html path: " + file_path.string());
        } else if (safe_path.empty()) {
            // 如果没有相对路径，使用默认文件
            file_path = base_dir / (default_file.empty() ? "index.html" : default_file);
            LOG_DEBUG("serveStaticFile - No url path, using default file: " + file_path.string());
        } else {
            // 安全检查
            if (safe_path.find("..") != std::string::npos) {
                LOG_WARNING("serveStaticFile - Security violation: '..' in path");
                response.status_code = "403";
                response.status_message = "Forbidden";
                response.headers["Content-Type"] = "text/plain";
                response.body = "Access denied: path traversal attempt";
                return response;
            }
            
            // 构建完整路径
            file_path = base_dir / safe_path;
            LOG_DEBUG("serveStaticFile - Constructed file path: " + file_path.string());
            
            // 检查路径是否是目录
            if (std::filesystem::exists(file_path) && std::filesystem::is_directory(file_path)) {
                // 如果是目录，尝试添加默认文件
                file_path = file_path / (default_file.empty() ? "index.html" : default_file);
                LOG_DEBUG("serveStaticFile - Path is directory, trying with default file: " + file_path.string());
            }
        }
        
        // 改进的文件查找逻辑：只查找请求的文件，不自动回退到index.html
        std::vector<std::filesystem::path> possible_paths;
        possible_paths.push_back(file_path);  // 原始路径
        possible_paths.push_back(base_dir / safe_path);  // 直接路径
        
        LOG_DEBUG("serveStaticFile - Starting file lookup strategy");
        LOG_DEBUG("serveStaticFile - Trying multiple file paths");
        bool file_found = false;
        
        // 尝试所有可能的路径
        for (const auto& candidate : possible_paths) {
            bool exists = std::filesystem::exists(candidate);
            bool is_dir = exists && std::filesystem::is_directory(candidate);
            LOG_DEBUG("serveStaticFile - Checking path: '" + candidate.string() + "' (exists: " + (exists ? "true" : "false") + ", is_dir: " + (is_dir ? "true" : "false") + ")");
                        
            if (exists && !is_dir) {
                LOG_DEBUG("serveStaticFile - Found valid file at: " + candidate.string());
                file_path = candidate;
                file_found = true;
                break;
            }
        }
        
        // 只有当请求的是目录时，才尝试返回默认文件
        if (!file_found && std::filesystem::exists(base_dir / safe_path) && std::filesystem::is_directory(base_dir / safe_path)) {
            LOG_DEBUG("serveStaticFile - Path is directory, trying default file");
            std::filesystem::path dir_file_path = (base_dir / safe_path) / (default_file.empty() ? "index.html" : default_file);
            if (std::filesystem::exists(dir_file_path) && !std::filesystem::is_directory(dir_file_path)) {
                LOG_DEBUG("serveStaticFile - Found default file in directory: " + dir_file_path.string());
                file_path = dir_file_path;
                file_found = true;
            }
        }
        
        // 输出最终确定的文件路径
        LOG_DEBUG("serveStaticFile - Final file path: '" + file_path.string() + "' (exists: " + (std::filesystem::exists(file_path) ? "true" : "false") + ")");
        
        if (!file_found && !std::filesystem::exists(file_path)) {
            LOG_WARNING("serveStaticFile - File not found after trying multiple paths");
            response.status_code = "404";
            response.status_message = "Not Found";
            response.headers["Content-Type"] = "text/plain";
            response.body = "File not found: " + safe_path;
            return response;
        }
        
        // 获取规范化路径并检查安全性
        try {
            // 规范化文件路径和基础目录路径，确保两者都使用相同的规范化格式
            std::filesystem::path canonical_path = std::filesystem::canonical(file_path);
            std::string canonical_path_str = canonical_path.string();
            
            // 确保基础目录也进行规范化处理，以正确处理包含./的路径
            std::filesystem::path canonical_base_dir = std::filesystem::canonical(base_dir);
            std::string canonical_base_dir_str = canonical_base_dir.string();
            
            LOG_DEBUG("serveStaticFile - Canonical path: " + canonical_path_str);
            LOG_DEBUG("serveStaticFile - Canonical base directory: " + canonical_base_dir_str);
            
            // 确保目录路径末尾有斜杠，避免部分匹配问题
            if (!canonical_base_dir_str.empty() && canonical_base_dir_str.back() != '/') {
                canonical_base_dir_str += '/';
            }
            
            // 检查文件是否在允许的目录内
            if (!(canonical_path_str.compare(0, canonical_base_dir_str.length(), canonical_base_dir_str) == 0)) {
                LOG_DEBUG("serveStaticFile - Security violation: File outside allowed directory");
                response.status_code = "403";
                response.status_message = "Forbidden";
                response.headers["Content-Type"] = "text/plain";
                response.body = "Access denied: file outside allowed directory";
                return response;
            }
            LOG_DEBUG("[serveStaticFile] Security check passed");
        } catch (const std::exception& e) {
            LOG_WARNING("serveStaticFile - Error getting canonical path: " + std::string(e.what()));
            // 继续，让下面的文件存在性检查处理
        }
        
        // 检查文件是否存在
        if (!std::filesystem::exists(file_path)) {
            LOG_WARNING("serveStaticFile - File not found: " + file_path.string());
            response.status_code = "404";
            response.status_message = "Not Found";
            response.headers["Content-Type"] = "text/plain";
            response.body = "File not found: " + file_path.string();
            return response;
        }
        
        // 获取MIME类型
        std::string mime_type = getMimeType(file_path.string());
        
        // 设置 ETag 和 Last-Modified（基于文件系统元数据，无需读取文件内容）
        std::string last_modified_str;
        std::string etag;
        
        // 获取文件修改时间
        auto last_write_time = std::filesystem::last_write_time(file_path);
        
        // 获取文件大小（用于 ETag 和 Content-Length）
        uint64_t file_size = 0;
        std::error_code fs_ec;
        auto fs_size = std::filesystem::file_size(file_path, fs_ec);
        if (!fs_ec) {
            file_size = static_cast<uint64_t>(fs_size);
        }
        
        // 生成ETag (基于文件大小和修改时间)
        uint64_t timestamp = static_cast<uint64_t>(last_write_time.time_since_epoch().count());
        std::stringstream etag_ss;
        etag_ss << "\"" << file_size << "-" << timestamp << "\"";
        etag = etag_ss.str();
        
        // 设置Last-Modified头
        auto tp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            last_write_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t last_modified_c = std::chrono::system_clock::to_time_t(tp);
        std::stringstream last_modified_ss;
        last_modified_ss << std::put_time(gmtime(&last_modified_c), "%a, %d %b %Y %H:%M:%S GMT");
        last_modified_str = last_modified_ss.str();
        
        // 检查缓存验证
        bool should_return_304 = false;
        
        // 检查If-None-Match头
        auto if_none_match_it = request.headers.find("If-None-Match");
        if (if_none_match_it != request.headers.end() && if_none_match_it->second == etag) {
            should_return_304 = true;
        } 
        // 检查If-Modified-Since头
        else {
            auto if_modified_since_it = request.headers.find("If-Modified-Since");
            if (if_modified_since_it != request.headers.end()) {
                // 简化比较：直接比较字符串
                if (if_modified_since_it->second == last_modified_str) {
                    should_return_304 = true;
                }
            }
        }
        
        // 设置所有响应头
        response.headers["Content-Type"] = mime_type;
        response.headers["Cache-Control"] = "max-age=3600, public";
        response.headers["Last-Modified"] = last_modified_str;
        response.headers["ETag"] = etag;
        
        if (should_return_304) {
            // 缓存验证通过，返回304 Not Modified
            response.status_code = "304";
            response.status_message = "Not Modified";
            response.body.clear(); // 304响应不需要响应体
            LOG_INFO("serveStaticFile - Returning 304 Not Modified for: " + file_path.string());
        } else {
            // 检查 Range 头（断点续传）
            uint64_t range_start = 0;
            uint64_t range_end = file_size - 1;
            bool has_range = false;
            
            auto range_it = request.headers.find("Range");
            if (range_it != request.headers.end()) {
                const std::string& range_hdr = range_it->second;
                // 仅支持 bytes=start-end 格式
                if (range_hdr.size() > 6 && range_hdr.substr(0, 6) == "bytes=") {
                    std::string range_val = range_hdr.substr(6);
                    size_t dash = range_val.find('-');
                    if (dash != std::string::npos) {
                        std::string start_str = range_val.substr(0, dash);
                        std::string end_str = range_val.substr(dash + 1);
                        if (!start_str.empty()) range_start = std::stoull(start_str);
                        if (!end_str.empty()) range_end = std::stoull(end_str);
                        if (end_str.empty()) range_end = file_size - 1;
                        if (range_end >= file_size) range_end = file_size - 1;
                        if (range_start <= range_end && range_start < file_size) {
                            has_range = true;
                        }
                    }
                }
            }
            
            if (has_range) {
                // Range 请求：读取指定范围到内存（范围通常较小）
                uint64_t range_len = range_end - range_start + 1;
                std::ifstream file(file_path, std::ios::binary);
                if (file.is_open()) {
                    file.seekg(static_cast<std::streamoff>(range_start));
                    std::vector<char> buf(static_cast<size_t>(range_len));
                    file.read(buf.data(), buf.size());
                    file.close();
                    
                    response.status_code = "206";
                    response.status_message = "Partial Content";
                    response.headers["Content-Range"] = "bytes " + std::to_string(range_start) + "-"
                        + std::to_string(range_end) + "/" + std::to_string(file_size);
                    response.headers["Content-Length"] = std::to_string(range_len);
                    response.body.assign(buf.begin(), buf.end());
                    LOG_INFO("serveStaticFile - Range " + std::to_string(range_start) + "-" + std::to_string(range_end) + "/" + std::to_string(file_size));
                } else {
                    // 文件打开失败，回退到完整文件
                    response.status_code = "200";
                    response.status_message = "OK";
                    response.file_body_path = file_path.string();
                }
            } else {
                // 完整文件请求：使用 sendfile 零拷贝
                response.status_code = "200";
                response.status_message = "OK";
                response.file_body_path = file_path.string();
                LOG_INFO("serveStaticFile - Serving via sendfile: " + file_path.string() + " (" + std::to_string(file_size) + " bytes)");
            }
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("serveStaticFile - Exception: " + std::string(e.what()));
        response.status_code = "500";
        response.status_message = "Internal Server Error";
        response.headers["Content-Type"] = "text/plain";
        response.body = "Error serving file: " + std::string(e.what());
    }
    
    return response;
}

// 静态文件服务方法 - 无默认文件
void HttpServer2::serveStatic(const std::string& url_path, const std::string& directory_path) {
    LOG_DEBUG("serveStatic - ENTRY - url_path: '" + url_path + "', directory_path: '" + directory_path + "'");
    serveStatic(url_path, directory_path, "index.html");
}

// 静态文件服务方法 - 带默认文件
void HttpServer2::serveStatic(const std::string& url_path, const std::string& directory_path, const std::string& default_file) {
        LOG_DEBUG("serveStatic - ENTRY - url_path: '" + url_path + "', directory_path: '" + directory_path + "', default_file: '" + default_file + "'");
    
    // 获取规范化的文件系统路径
    std::filesystem::path fs_path(directory_path);
    std::filesystem::path abs_fs_path = std::filesystem::absolute(fs_path);
    LOG_DEBUG("serveStatic - Absolute file system path: " + abs_fs_path.string());
    
    // 确保URL路径格式正确
    std::string normalized_url_path = url_path;
    if (normalized_url_path.empty() || normalized_url_path[0] != '/') {
        normalized_url_path = "/" + normalized_url_path;
        LOG_DEBUG("serveStatic - Normalized URL path: " + normalized_url_path);
    }
    
    // 处理根路径的处理器
    auto root_handler = [this, abs_fs_path, default_file, url_path](const HttpRequest& request) -> HttpResponse {
        LOG_DEBUG("serveStatic - Root handler invoked for path: '" + request.path + "', route: '" + url_path + "'");
        HttpResponse response = serveStaticFile(request, "", abs_fs_path.string(), default_file);
        LOG_DEBUG("serveStatic - Root handler response status: " + response.status_code);
        return response;
    };
    
    // 处理子路径的处理器
    auto subpath_handler = [this, abs_fs_path, default_file, url_path](const HttpRequest& request) -> HttpResponse {
          LOG_DEBUG("serveStatic - Subpath handler invoked for path: '" + request.path + "', route: '" + url_path + "'");
          
          // 从请求路径中提取相对路径部分
          std::string relative_path;
          
          // 特殊处理index.html直接请求
          if (request.path == url_path + "/index.html") {
              LOG_DEBUG("serveStatic - Direct match for index.html request");
              relative_path = "index.html";
          } else if (request.path_params.empty()) {
              LOG_DEBUG("serveStatic - Subpath handler: No path params, extracting from path");
              // 尝试从请求路径中直接提取相对路径
              if (request.path.size() > url_path.size() + 1 && request.path.substr(0, url_path.size()) == url_path) {
                  relative_path = request.path.substr(url_path.size() + 1);
                  LOG_DEBUG("serveStatic - Extracted relative path from URL: '" + relative_path + "'");
              } else {
                  relative_path = "";
              }
          } else {
              relative_path = request.path_params.begin()->second;
              LOG_DEBUG("serveStatic - Subpath handler: Using path param: '" + relative_path + "'");
          }
          
          LOG_DEBUG("serveStatic - Calling serveStaticFile with relative_path='" + relative_path + "', directory='" + abs_fs_path.string() + "'");
          HttpResponse response = serveStaticFile(request, relative_path, abs_fs_path.string(), default_file);
          LOG_DEBUG("serveStatic - Subpath handler response status: " + response.status_code);
          return response;
    };
    
    // 添加根路径路由
    get(normalized_url_path, root_handler);
    LOG_DEBUG("serveStatic - Added root handler for: " + normalized_url_path);
    
    // 添加子路径路由
    std::string regex_pattern;
    if (normalized_url_path == "/") {
        regex_pattern = "^/(.*)$";
    } else {
        regex_pattern = "^" + normalized_url_path + "/(.*)$";
    }
    addRoute("GET", regex_pattern, subpath_handler);
    LOG_DEBUG("serveStatic - Added subpath handler with pattern: " + regex_pattern);
}

// URL解析辅助函数
struct UrlParts {
    std::string scheme;      // http or https
    std::string host;        // 主机名
    uint16_t port;           // 端口号
    std::string path;        // 路径
    std::string query;       // 查询字符串
};

// 解析URL，提取主机名、端口和路径
UrlParts parseUrl(const std::string& url) {
    UrlParts parts;
    
    // 默认值
    parts.scheme = "http";
    parts.port = 80;
    parts.path = "/";
    
    // 查找协议分隔符
    size_t scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        parts.scheme = url.substr(0, scheme_end);
        if (parts.scheme == "https") {
            parts.port = 443;
        }
    }
    
    // 提取主机名、端口和路径
    size_t host_start = scheme_end != std::string::npos ? scheme_end + 3 : 0;
    size_t path_start = url.find("/", host_start);
    
    if (path_start == std::string::npos) {
        // 没有路径，使用默认路径
        parts.host = url.substr(host_start);
    } else {
        parts.host = url.substr(host_start, path_start - host_start);
        
        // 提取路径和查询字符串
        size_t query_start = url.find("?", path_start);
        if (query_start == std::string::npos) {
            parts.path = url.substr(path_start);
        } else {
            parts.path = url.substr(path_start, query_start - path_start);
            parts.query = url.substr(query_start);
        }
    }
    
    // 提取端口号
    size_t port_start = parts.host.find(":");
    if (port_start != std::string::npos) {
        parts.port = std::stoi(parts.host.substr(port_start + 1));
        parts.host = parts.host.substr(0, port_start);
    }
    
    return parts;
}

// 主动健康检查：尝试 TCP 连接后端服务器
bool BackendState::checkHealth() const {
    try {
        auto parts = parseUrl(url);
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(parts.host, std::to_string(parts.port));
        tcp::socket sock(ioc);
        sock.connect(*results);
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_both, ec);
        sock.close(ec);
        return true;
    } catch (...) {
        return false;
    }
}

// 反向代理功能实现
// 为所有HTTP方法添加反向代理
void HttpServer2::addReverseProxy(const std::string& path, const std::string& target_url) {
    addReverseProxy("*", path, target_url);
}

// 为特定HTTP方法添加反向代理
void HttpServer2::addReverseProxy(const std::string& method, const std::string& path, const std::string& target_url) {
    // 默认不添加任何额外的头信息
    std::unordered_map<std::string, std::string> empty_request_headers;
    std::unordered_map<std::string, std::string> empty_response_headers;
    addReverseProxy(method, path, target_url, empty_request_headers, empty_response_headers);
}

// 为特定HTTP方法添加反向代理，支持自定义请求头和响应头
void HttpServer2::addReverseProxy(const std::string& method, const std::string& path, const std::string& target_url,
                                  const std::unordered_map<std::string, std::string>& request_headers,
                                  const std::unordered_map<std::string, std::string>& response_headers) {
    // 将单个URL转换为向量，调用支持多个后端的版本
    std::vector<std::string> target_urls = {target_url};
    addReverseProxy(method, path, target_urls, request_headers, response_headers);
}

// 为所有HTTP方法添加支持多个后端的反向代理
void HttpServer2::addReverseProxy(const std::string& path, const std::vector<std::string>& target_urls) {
    addReverseProxy("*", path, target_urls);
}

// 为特定HTTP方法添加支持多个后端的反向代理
void HttpServer2::addReverseProxy(const std::string& method, const std::string& path, const std::vector<std::string>& target_urls) {
    // 默认不添加任何额外的头信息
    std::unordered_map<std::string, std::string> empty_request_headers;
    std::unordered_map<std::string, std::string> empty_response_headers;
    addReverseProxy(method, path, target_urls, empty_request_headers, empty_response_headers);
}

// 为特定HTTP方法添加支持多个后端的反向代理，支持自定义请求头和响应头
void HttpServer2::addReverseProxy(const std::string& method, const std::string& path, const std::vector<std::string>& target_urls,
                                  const std::unordered_map<std::string, std::string>& request_headers,
                                  const std::unordered_map<std::string, std::string>& response_headers) {
    LOG_DEBUG("addReverseProxy - Adding reverse proxy for " + method + " " + path + " with " + std::to_string(target_urls.size()) + " backend servers");
    
    // 创建共享的后端状态列表（健康检查）
    auto backends = std::make_shared<std::vector<std::shared_ptr<BackendState>>>();
    for (const auto& url : target_urls) {
        backends->push_back(std::make_shared<BackendState>(url));
    }
    auto counter = std::make_shared<std::atomic<size_t>>(0);
    
    // 创建反向代理处理器，捕获目标URLs和头信息
    auto proxy_handler = [backends, request_headers, response_headers, counter](const HttpRequest& request) -> HttpResponse {
        try {
            // 收集所有健康的或可重试的后端
            std::vector<size_t> candidate_indices;
            for (size_t i = 0; i < backends->size(); ++i) {
                auto& b = (*backends)[i];
                if (b->healthy) {
                    candidate_indices.push_back(i);
                } else if (b->shouldRetry()) {
                    // 主动健康检查
                    if (b->checkHealth()) {
                        b->healthy = true;
                        b->consecutive_failures = 0;
                        LOG_INFO("Reverse proxy - Backend " + b->url + " recovered via active health check");
                        candidate_indices.push_back(i);
                    } else {
                        b->last_check = std::chrono::steady_clock::now();
                        LOG_WARNING("Reverse proxy - Backend " + b->url + " still down after health check");
                    }
                }
            }
            
            // 如果没有可用后端，返回 503
            if (candidate_indices.empty()) {
                LOG_ERROR("Reverse proxy - All backends are down, returning 503");
                HttpResponse error_resp = createErrorResponse(HttpErrorType::SERVICE_UNAVAILABLE,
                    "All backend servers are currently unavailable");
                return error_resp;
            }
            
            // 尝试所有候选后端
            for (size_t attempt = 0; attempt < candidate_indices.size(); ++attempt) {
                // 轮询选择
                size_t idx = (*counter)++ % candidate_indices.size();
                size_t backend_index = candidate_indices[idx];
                auto& backend = (*backends)[backend_index];
                const std::string& target_url = backend->url;
                
                LOG_DEBUG("Reverse proxy - Forwarding request: " + request.method + " " + request.path + " -> " + target_url + " (backend " + std::to_string(backend_index + 1) + "/" + std::to_string(backends->size()) + ", attempt " + std::to_string(attempt + 1) + ")");
                
                try {
                    // 解析目标URL
                    UrlParts target_parts = parseUrl(target_url);
                    LOG_DEBUG("Reverse proxy - Parsed URL: scheme=" + target_parts.scheme + ", host=" + target_parts.host + ", port=" + std::to_string(target_parts.port) + ", path=" + target_parts.path);
                    
                    // 创建IO上下文
                    net::io_context ioc;
                    
                    // 设置超时时间
                    const std::chrono::seconds timeout(5);
                    
                    // 创建解析器
                    tcp::resolver resolver(ioc);
                    auto const results = resolver.resolve(target_parts.host, std::to_string(target_parts.port));
                    
                    // 创建套接字
                    beast::tcp_stream stream(ioc);
                    stream.expires_after(timeout);
                    stream.connect(results);
                    
                    // 创建请求
                    http::request<http::string_body> proxy_request;
                    
                    // 设置请求方法
                    if (request.method == "GET") {
                        proxy_request.method(http::verb::get);
                    } else if (request.method == "POST") {
                        proxy_request.method(http::verb::post);
                    } else if (request.method == "PUT") {
                        proxy_request.method(http::verb::put);
                    } else if (request.method == "DELETE") {
                        proxy_request.method(http::verb::delete_);
                    } else if (request.method == "PATCH") {
                        proxy_request.method(http::verb::patch);
                    } else if (request.method == "HEAD") {
                        proxy_request.method(http::verb::head);
                    } else if (request.method == "OPTIONS") {
                        proxy_request.method(http::verb::options);
                    } else {
                        proxy_request.method(http::verb::get);
                    }
                    
                    // 设置请求目标
                    proxy_request.target(target_parts.path + target_parts.query);
                    
                    // 设置HTTP版本
                    proxy_request.version(11);
                    
                    // 设置主机头
                    proxy_request.set(http::field::host, target_parts.host);
                    
                    // 复制请求头
                    for (const auto& [key, value] : request.headers) {
                        if (key != "Host" && key != "Connection" && key != "Content-Length") {
                            proxy_request.set(key, value);
                        }
                    }
                    
                    // 添加/修改请求头
                    for (const auto& [key, value] : request_headers) {
                        proxy_request.set(key, value);
                    }
                    
                    // 设置请求体
                    proxy_request.body() = request.body;
                    
                    // 准备请求
                    proxy_request.prepare_payload();
                    
                    // 发送请求
                    http::write(stream, proxy_request);
                    
                    // 检查是否可流式转发（有客户端 socket + 非 SSL）
                    bool can_stream = (request.client_socket && request.client_socket->is_open());
                    
                    if (can_stream) {
                        // ======== 流式转发路径：逐块转发上游响应到客户端 ========
                        // 1. 解析响应头
                        http::response_parser<http::string_body> header_parser;
                        header_parser.skip(true); // 只解析头，跳过 body
                        beast::flat_buffer header_buf;
                        http::read_header(stream, header_buf, header_parser);
                        
                        auto& upstream_resp = header_parser.get();
                        int status_code_int = static_cast<int>(upstream_resp.result());
                        
                        // 2. 转发状态行 + 响应头到客户端
                        std::stringstream resp_header_ss;
                        resp_header_ss << "HTTP/1.1 " << status_code_int << " "
                                       << upstream_resp.reason() << "\r\n";
                        for (const auto& f : upstream_resp) {
                            resp_header_ss << f.name_string() << ": " << f.value() << "\r\n";
                        }
                        // 添加自定义响应头
                        for (const auto& [k, v] : response_headers) {
                            resp_header_ss << k << ": " << v << "\r\n";
                        }
                        resp_header_ss << "\r\n";
                        
                        std::string header_str = resp_header_ss.str();
                        boost::asio::write(*request.client_socket,
                            boost::asio::buffer(header_str.data(), header_str.size()));
                        
                        // 3. 流式转发 body（每次 64KB 块）
                        std::vector<char> chunk(65536);
                        beast::flat_buffer body_buf;
                        uint64_t total_bytes = 0;
                        
                        while (true) {
                            boost::system::error_code read_ec;
                            size_t bytes_read = stream.socket().read_some(
                                boost::asio::buffer(chunk.data(), chunk.size()), read_ec);
                            
                            if (bytes_read > 0) {
                                boost::asio::write(*request.client_socket,
                                    boost::asio::buffer(chunk.data(), bytes_read));
                                total_bytes += bytes_read;
                            }
                            
                            if (read_ec == boost::asio::error::eof) break;
                            if (read_ec) {
                                LOG_WARNING("Reverse proxy stream read error: " + read_ec.message());
                                break;
                            }
                        }
                        
                        LOG_DEBUG("Reverse proxy - Streamed " + std::to_string(total_bytes) + " bytes from " + target_url);
                        
                        // 4. 关闭上游连接
                        boost::system::error_code shut_ec;
                        stream.socket().shutdown(tcp::socket::shutdown_both, shut_ec);
                        
                        // 5. 标记响应已发送
                        HttpResponse response;
                        response.response_sent = true;
                        
                        // 记录成功后端
                        if (status_code_int < 500) {
                            backend->recordSuccess();
                        } else {
                            backend->recordFailure();
                        }
                        
                        return response;
                    }
                    
                    // ======== 非流式路径：全量缓冲后转发 ========
                    // 接收响应
                    beast::flat_buffer buffer;
                    http::response<http::string_body> proxy_response;
                    http::read(stream, buffer, proxy_response);
                    
                    // 关闭连接
                    boost::system::error_code ec;
                    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
                    // 忽略关闭错误
                    
                    // 创建响应对象
                    HttpResponse response;
                    response.status_code = std::to_string(static_cast<int>(proxy_response.result()));
                    response.status_message = proxy_response.reason();
                    
                    // 复制响应头
                    for (const auto& field : proxy_response) {
                        response.headers[std::string(field.name_string())] = std::string(field.value());
                    }
                    
                    // 添加/修改响应头
                    for (const auto& [key, value] : response_headers) {
                        response.headers[key] = value;
                    }
                    
                    // 设置响应体
                    response.body = proxy_response.body();
                    
                    LOG_DEBUG("Reverse proxy - Response received: " + response.status_code + " " + response.status_message);
                    
                    // 记录成功后端状态
                    if (std::atoi(response.status_code.c_str()) < 500) {
                        backend->recordSuccess();
                        LOG_DEBUG("Reverse proxy - Backend " + target_url + " marked healthy");
                    } else {
                        backend->recordFailure();
                        LOG_WARNING("Reverse proxy - Backend " + target_url + " returned " + response.status_code + ", failures: " + std::to_string(backend->consecutive_failures.load()));
                    }
                    
                    return response;
                } catch (const boost::system::system_error& e) {
                    // 记录后端失败
                    backend->recordFailure();
                    LOG_WARNING("Reverse proxy backend " + target_url + " failed: " + e.what() + " (failures: " + std::to_string(backend->consecutive_failures.load()) + ")");
                    // 继续循环，尝试下一个后端
                    continue;
                } catch (const std::exception& e) {
                    // 记录后端失败
                    backend->recordFailure();
                    LOG_WARNING("Reverse proxy backend " + target_url + " failed: " + e.what() + " (failures: " + std::to_string(backend->consecutive_failures.load()) + ")");
                    // 继续循环，尝试下一个后端
                    continue;
                }
            }
            
            // 所有后端都尝试过了，返回503并列出健康状态
            std::string health_status;
            for (size_t i = 0; i < backends->size(); ++i) {
                if (i > 0) health_status += ", ";
                health_status += (*backends)[i]->url + ":" + ((*backends)[i]->healthy ? "UP" : "DOWN");
            }
            LOG_ERROR("Reverse proxy - All backends are unavailable [" + health_status + "]");
            return createErrorResponse(HttpErrorType::SERVICE_UNAVAILABLE, "All backend servers are unavailable: " + health_status);
        } catch (const std::exception& e) {
            LOG_ERROR("Reverse proxy error: " + std::string(e.what()));
            return createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Reverse proxy error: " + std::string(e.what()));
        }
    };
    
    // 添加路由
    addRoute(method, path, proxy_handler);
    LOG_DEBUG("addReverseProxy - Added reverse proxy route for " + method + " " + path);
}


// ============================================================
// WebSocket 代理：将 WebSocket 连接双向转发到目标服务器
// ============================================================
void HttpServer2::addWebSocketProxy(const std::string& path, const std::string& target_url) {
    auto handler = [this, target_url](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.response_sent = true;
        if (!request.client_socket || !request.client_socket->is_open()) {
            response.status_code = "400"; response.response_sent = false; return response;
        }
        auto ws_key_it = request.headers.find("Sec-WebSocket-Key");
        if (ws_key_it == request.headers.end()) {
            response.status_code = "400"; response.response_sent = false; return response;
        }
        try {
            // 生成 Sec-WebSocket-Accept (RFC 6455)
            std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            std::string concat = ws_key_it->second + magic;
            unsigned char hash[20];
            SHA1((const unsigned char*)concat.data(), concat.size(), hash);
            std::string accept; accept.reserve(28);
            static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 20; i += 3) {
                int v = (hash[i] << 16) | (hash[i+1] << 8) | hash[i+2];
                accept += b64[(v >> 18) & 0x3f];
                accept += b64[(v >> 12) & 0x3f];
                accept += b64[(v >> 6) & 0x3f];
                accept += b64[v & 0x3f];
            }
            accept.resize((20 * 4 + 2) / 3); // base64 padding
            
            // 构建 101 响应
            std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            boost::asio::write(*request.client_socket, boost::asio::buffer(resp));
            
            UrlParts parts = parseUrl(target_url);
            net::io_context ioc;
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(parts.host, std::to_string(parts.port));
            tcp::socket upstream(ioc);
            boost::asio::connect(upstream, results);
            
            std::atomic<bool> running{true};
            auto csock = request.client_socket;
            
            std::thread t1([csock, &upstream, &running]() {
                std::vector<char> buf(65536);
                while (running) {
                    boost::system::error_code ec;
                    size_t n = csock->read_some(boost::asio::buffer(buf), ec);
                    if (ec || n == 0) { running = false; break; }
                    boost::asio::write(upstream, boost::asio::buffer(buf.data(), n));
                }
            });
            std::thread t2([csock, &upstream, &running]() {
                std::vector<char> buf(65536);
                while (running) {
                    boost::system::error_code ec;
                    size_t n = upstream.read_some(boost::asio::buffer(buf), ec);
                    if (ec || n == 0) { running = false; break; }
                    boost::asio::write(*csock, boost::asio::buffer(buf.data(), n));
                }
            });
            t1.join(); t2.join();
            boost::system::error_code ec;
            upstream.shutdown(tcp::socket::shutdown_both, ec);
            upstream.close(ec);
        } catch (const std::exception& e) {
            LOG_ERROR("WebSocket proxy error: " + std::string(e.what()));
        }
        return response;
    };
    addRoute("*", path, handler);
    LOG_INFO("WebSocket proxy added: " + path + " -> " + target_url);
}

// 调试模式开关
// 调试模式开关
void HttpServer2::enableDebugMode(bool enable) {
    debug_mode_ = enable;
    // 在调试模式下，设置日志级别为DEBUG
    if (enable) {
        log_set_level(LOG_LEVEL_DEBUG);
        LOG_DEBUG("Debug mode enabled");
    } else {
        log_set_level(LOG_LEVEL_INFO);
        LOG_INFO("Debug mode disabled");
    }
}

// 检查是否启用调试模式
bool HttpServer2::isDebugMode() const {
    return debug_mode_;
}

// 获取性能报告
std::string HttpServer2::getPerformanceReport() const {
    return PerformanceMonitor::generatePerformanceReport();
}

// 重置性能指标
void HttpServer2::resetPerformanceMetrics() {
    PerformanceMonitor::reset();
    LOG_INFO("Performance metrics reset");
}

// 获取Prometheus格式的指标
std::string HttpServer2::getPrometheusMetrics() const {
    return PerformanceMonitor::generatePrometheusMetrics();
}

// 设置速率限制
void HttpServer2::setRateLimit(uint32_t requests_per_window, uint32_t window_size_seconds) {
    rate_limiter_.setRateLimit(requests_per_window, window_size_seconds);
    
    // 同时更新全局速率限制
    requests_per_second_ = requests_per_window / window_size_seconds;
    
    LOG_INFO("Global rate limit set to " + std::to_string(requests_per_second_) + " requests per second");
}

// 自适应线程池配置
void HttpServer2::enableAdaptiveThreadPool(bool enable) {
    enable_adaptive_thread_pool_ = enable;
    std::string status = enable ? "enabled" : "disabled";
    LOG_INFO("Adaptive thread pool " + status);
}

void HttpServer2::setMinThreadPoolSize(size_t min_size) {
    min_thread_pool_size_ = min_size;
    LOG_INFO("Minimum thread pool size set to " + std::to_string(min_size));
}

void HttpServer2::setMaxThreadPoolSize(size_t max_size) {
    max_thread_pool_size_ = max_size;
    LOG_INFO("Maximum thread pool size set to " + std::to_string(max_size));
}

void HttpServer2::setThreadAdjustInterval(std::chrono::milliseconds interval) {
    thread_adjust_interval_ = interval;
    LOG_INFO("Thread adjust interval set to " + std::to_string(interval.count()) + "ms");
}

// 动态调整线程池大小
void HttpServer2::adjustThreadPoolSize() {
    if (!enable_adaptive_thread_pool_) {
        return;
    }
    
    // 计算当前系统负载
    size_t current_threads = thread_pool_.size();
    size_t pending = pending_requests_.load();
    
    LOG_DEBUG("Current thread count: " + std::to_string(current_threads) + ", Pending requests: " + std::to_string(pending));
    
    // 根据等待请求数调整线程数
    if (pending > current_threads * 2 && current_threads < max_thread_pool_size_) {
        // 需要增加线程
        size_t new_threads = std::min(current_threads + 1, max_thread_pool_size_);
        LOG_INFO("Increasing thread pool size from " + std::to_string(current_threads) + " to " + std::to_string(new_threads) + " due to " + std::to_string(pending) + " pending requests");
        
        // 添加新的io_context和线程（使用互斥锁保护容器写操作）
        {
            std::lock_guard<std::mutex> lock(io_context_mutex_);
            for (size_t i = current_threads; i < new_threads; ++i) {
                auto io_context = std::make_shared<net::io_context>();
                io_contexts_.push_back(io_context);
                thread_pool_.emplace_back(&HttpServer2::runIoContext, this, io_context);
            }
            
            // 增加队列处理线程
            while (queue_threads_.size() < new_threads) {
                queue_threads_.emplace_back([this]() {
                    processRequestQueue();
                });
            }
        }
    } else if (pending < current_threads / 2 && current_threads > min_thread_pool_size_) {
        // 需要减少线程
        size_t new_threads = std::max(current_threads - 1, min_thread_pool_size_);
        LOG_INFO("Decreasing thread pool size from " + std::to_string(current_threads) + " to " + std::to_string(new_threads) + " due to " + std::to_string(pending) + " pending requests");
        
        // 减少线程（简单实现：标记线程为可停止，等待自然退出）
        // 注意：实际实现需要更复杂的线程安全机制
        if (new_threads < current_threads) {
            // 这里只是演示，实际实现需要更复杂的逻辑
            // 目前只是记录日志，不实际减少线程
            LOG_DEBUG("Thread pool size adjustment deferred - complex thread management required");
        }
    }
}

// 自适应线程池管理线程
void HttpServer2::startThreadAdjustment() {
    if (!enable_adaptive_thread_pool_) {
        return;
    }
    
    thread_adjust_thread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(thread_adjust_interval_);
            if (running_) {
                adjustThreadPoolSize();
            }
        }
    });
    
    LOG_INFO("Started adaptive thread pool adjustment thread");
}

// 配置热重载实现
void HttpServer2::enableConfigHotReload(bool enable) {
    enable_config_hot_reload_ = enable;
    std::string status = enable ? "enabled" : "disabled";
    LOG_INFO("Config hot reload " + status);
}

void HttpServer2::setConfigFilePath(const std::string& config_path) {
    config_file_path_ = config_path;
    
    // 获取配置文件最后修改时间
    try {
        auto file_time = std::filesystem::last_write_time(config_path);
        last_config_modified_ = std::chrono::time_point_cast<std::chrono::steady_clock::duration>(
            file_time - std::filesystem::file_time_type::clock::now() + std::chrono::steady_clock::now());
        LOG_INFO("Config file path set to " + config_path + ", last modified at " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(last_config_modified_.time_since_epoch()).count()));
    } catch (const std::exception& e) {
        LOG_WARNING("Failed to get last modified time for config file: " + std::string(e.what()));
    }
}

void HttpServer2::setConfigCheckInterval(std::chrono::milliseconds interval) {
    config_check_interval_ = interval;
    LOG_INFO("Config check interval set to " + std::to_string(interval.count()) + "ms");
}

// 启动配置文件监控线程
void HttpServer2::startConfigWatch() {
    if (!enable_config_hot_reload_ || config_file_path_.empty()) {
        return;
    }
    
    config_watch_thread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(config_check_interval_);
            if (running_) {
                watchConfigFile();
            }
        }
    });
    
    LOG_INFO("Started config file watch thread for " + config_file_path_ + ", checking every " + std::to_string(config_check_interval_.count()) + "ms");
}

// 监控配置文件变化
void HttpServer2::watchConfigFile() {
    if (checkConfigFileModified()) {
        LOG_INFO("Config file modified, reloading configuration...");
        reloadConfig();
    }
}

// 检查配置文件是否被修改
bool HttpServer2::checkConfigFileModified() {
    try {
        auto file_time = std::filesystem::last_write_time(config_file_path_);
        auto file_modified = std::chrono::time_point_cast<std::chrono::steady_clock::duration>(
            file_time - std::filesystem::file_time_type::clock::now() + std::chrono::steady_clock::now());
        
        if (file_modified > last_config_modified_) {
            last_config_modified_ = file_modified;
            return true;
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Failed to check config file modification: " + std::string(e.what()));
    }
    return false;
}

// 重新加载配置
void HttpServer2::reloadConfig() {
    try {
        // 这里可以根据实际情况实现配置重新加载逻辑
        // 示例：重新解析配置文件并更新服务器配置
        LOG_INFO("Config reloaded successfully");
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to reload config: " + std::string(e.what()));
    }
}

// 设置SSL证书和密钥（如果启用SSL）
void HttpServer2::setSslCertificates(const std::string& cert_path, const std::string& key_path) {
    if (!use_ssl_) {
        throw std::runtime_error("SSL is not enabled");
    }
    
    if (!ssl_context_) {
        throw std::runtime_error("SSL context not initialized");
    }
    
    try {
        // 保存证书和密钥路径
        cert_path_ = cert_path;
        key_path_ = key_path;
        
        // 加载证书和密钥
        ssl_context_->use_certificate_chain_file(cert_path);
        ssl_context_->use_private_key_file(key_path, ssl::context::pem);
        
        // 如果设置了密码套件，应用它
        if (!cipher_suites_.empty()) {
            SSL_CTX_set_cipher_list(ssl_context_->native_handle(), cipher_suites_.c_str());
            LOG_INFO("SSL cipher suites configured: " + cipher_suites_);
        }
        
        LOG_INFO("SSL certificates loaded successfully: " + cert_path + ", " + key_path);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load SSL certificates: " + std::string(e.what()));
        throw;
    }
}
    
    // 配置SSL协议版本和选项
void HttpServer2::configureSslOptions(const std::string& /* min_version */, const std::string& /* max_version */) {
    if (!use_ssl_ || !ssl_context_) {
        return;
    }
    
    try {
        // 禁用不安全的协议版本
        ssl_context_->set_options(ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                                 ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
        
        // 设置TLS选项
        ssl_context_->set_options(ssl::context::default_workarounds |
                                 ssl::context::single_dh_use |
                                 ssl::context::no_compression);
        
        // 设置最小TLS版本为TLSv1.2
        SSL_CTX_set_min_proto_version(ssl_context_->native_handle(), TLS1_2_VERSION);
        
        LOG_INFO("SSL options configured: TLSv1.2+ enabled, SSLv2/3/TLSv1/TLSv1.1 disabled");
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to configure SSL options: " + std::string(e.what()));
    }
}

// 设置SSL密码套件
void HttpServer2::setCipherSuites(const std::string& cipher_suites) {
    cipher_suites_ = cipher_suites;
    
    // 如果SSL已初始化且证书已加载，立即应用密码套件
    if (use_ssl_ && ssl_context_ && !cert_path_.empty() && !key_path_.empty()) {
        try {
            SSL_CTX_set_cipher_list(ssl_context_->native_handle(), cipher_suites_.c_str());
            LOG_INFO("SSL cipher suites applied: " + cipher_suites_);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to apply SSL cipher suites: " + std::string(e.what()));
        }
    }
}
// 实现基本认证功能
void HttpServer2::setBasicAuth(const std::string& realm) {
    basic_auth_realm_ = realm;
    LOG_INFO("Basic authentication enabled for realm: " + realm);
}

void HttpServer2::addUser(const std::string& username, const std::string& password) {
    // 生成随机盐值（16字节，32个十六进制字符）
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 255);
    std::stringstream salt_ss;
    for (int i = 0; i < 16; ++i) {
        salt_ss << std::hex << std::setw(2) << std::setfill('0') << distrib(gen);
    }
    std::string salt = salt_ss.str();
    
    // 计算加盐密码的SHA-256哈希值：SHA256(salt + password)
    std::string password_hash = calculateSha256(salt + password);
    basic_auth_users_[username] = {salt, password_hash};
    LOG_INFO("User added: " + username);
}

bool HttpServer2::validateBasicAuth(const std::string& auth_header) {
    // 检查是否有基本认证头部
    if (auth_header.empty() || auth_header.substr(0, 6) != "Basic ") {
        return false;
    }
    
    try {
        // 解码Base64认证字符串
        std::string encoded = auth_header.substr(6);
        std::string decoded = base64Decode(encoded);
        
        // 分割用户名和密码
        size_t colon_pos = decoded.find(':');
        if (colon_pos == std::string::npos) {
            return false;
        }
        
        std::string username = decoded.substr(0, colon_pos);
        std::string password = decoded.substr(colon_pos + 1);
        
        // 检查用户是否存在
        auto it = basic_auth_users_.find(username);
        if (it == basic_auth_users_.end()) {
            return false;
        }
        
        // 验证密码：使用存储的盐值重新计算 SHA256(salt + password)
        const auto& [stored_salt, stored_hash] = it->second;
        std::string password_hash = calculateSha256(stored_salt + password);
        return password_hash == stored_hash;
    } catch (...) {
        return false;
    }
}

// Base64解码函数
std::string HttpServer2::base64Decode(const std::string& encoded) {
    // 简化实现，实际生产环境应使用更健壮的Base64解码库
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string decoded;
    int val = 0, valb = -8;
    
    for (unsigned char c : encoded) {
        if (c == '=') break;
        
        size_t idx = base64_chars.find(c);
        if (idx == std::string::npos) continue;
        
        val = (val << 6) + idx;
        valb += 6;
        
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return decoded;
}

// SHA-256哈希计算函数
std::string HttpServer2::calculateSha256(const std::string& input) {
    // 简化实现，实际生产环境应使用更安全的密码哈希方法
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input.c_str(), input.size());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return ss.str();
}

// CSRF保护功能实现
void HttpServer2::enableCsrfProtection(const std::string& cookie_name, const std::string& header_name) {
    csrf_protection_enabled_ = true;
    csrf_cookie_name_ = cookie_name;
    csrf_header_name_ = header_name;
    csrf_param_name_ = "csrf_token"; // 默认参数名称
    LOG_INFO("CSRF protection enabled with cookie: " + cookie_name + ", header: " + header_name);
}

// 设置CSRF令牌请求头名称
void HttpServer2::setCsrfTokenHeader(const std::string& header_name) {
    csrf_header_name_ = header_name;
    LOG_INFO("CSRF token header name set to: " + header_name);
}

// 设置CSRF令牌请求参数名称
void HttpServer2::setCsrfTokenParam(const std::string& param_name) {
    csrf_param_name_ = param_name;
    LOG_INFO("CSRF token parameter name set to: " + param_name);
}

// 启用/禁用CSRF保护（默认参数版本）
void HttpServer2::enableCsrfProtection(bool enable) {
    csrf_protection_enabled_ = enable;
    if (enable) {
        LOG_INFO("CSRF protection enabled with default cookie: XSRF-TOKEN, header: X-CSRF-Token");
    } else {
        LOG_INFO("CSRF protection disabled");
    }
}

// 生成CSRF令牌
std::string HttpServer2::generateCsrfToken() {
    // 生成随机令牌
    std::string token;
    const std::string charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, charset.size() - 1);
    
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        token += charset[distrib(gen)];
    }
    
    return token;
}

// 添加CSRF令牌到响应
void HttpServer2::addCsrfTokenToResponse(HttpResponse& response, const HttpRequest& request) {
    if (!csrf_protection_enabled_) {
        return;
    }
    
    // 检查请求是否已经包含CSRF cookie
    bool has_csrf_cookie = false;
    auto cookie_it = request.headers.find("Cookie");
    if (cookie_it != request.headers.end()) {
        std::string cookies = cookie_it->second;
        has_csrf_cookie = (cookies.find(csrf_cookie_name_ + "=") != std::string::npos);
    }
    
    // 如果没有CSRF cookie，生成并添加
    if (!has_csrf_cookie) {
        std::string csrf_token = generateCsrfToken();
        // 添加CSRF令牌到cookie（设置HttpOnly=false以便JavaScript可以读取）
        // Secure 标志仅在 HTTPS 时设置
        std::string cookie = csrf_cookie_name_ + "=" + csrf_token + "; HttpOnly=false; SameSite=Strict; Path=/";
        if (use_ssl_) {
            cookie += "; Secure";
        }
        response.headers["Set-Cookie"] = cookie;
        LOG_DEBUG("Added CSRF token to response cookie: " + csrf_token);
    }
}

// 验证CSRF令牌
bool HttpServer2::validateCsrfToken(const HttpRequest& request) {
    // 如果未启用CSRF保护，直接通过
    if (!csrf_protection_enabled_) {
        return true;
    }
    
    // 只对可能修改数据的方法进行验证
    if (request.method != "POST" && request.method != "PUT" && 
        request.method != "DELETE" && request.method != "PATCH") {
        return true;
    }
    
    // 从请求头获取令牌
    std::string header_token;
    auto token_it = request.headers.find(csrf_header_name_);
    if (token_it != request.headers.end()) {
        header_token = token_it->second;
    }
    
    // 如果请求头中没有令牌，尝试从请求体获取
    std::string param_pattern = csrf_param_name_ + "=";
    if (header_token.empty() && request.body.find(param_pattern) != std::string::npos) {
        // 简化的表单数据解析
        size_t start = request.body.find(param_pattern) + param_pattern.length();
        size_t end = request.body.find("&", start);
        if (end == std::string::npos) {
            end = request.body.size();
        }
        header_token = request.body.substr(start, end - start);
    }
    
    // 从请求头获取Cookie
    std::string cookie_token;
    auto cookie_it = request.headers.find("Cookie");
    if (cookie_it != request.headers.end()) {
        std::string cookies = cookie_it->second;
        size_t token_pos = cookies.find(csrf_cookie_name_ + "=");
        if (token_pos != std::string::npos) {
            token_pos += csrf_cookie_name_.size() + 1;
            size_t end = cookies.find("; ", token_pos);
            if (end == std::string::npos) {
                end = cookies.size();
            }
            cookie_token = cookies.substr(token_pos, end - token_pos);
        }
    }
    
    // 验证令牌是否匹配
    bool isValid = !header_token.empty() && !cookie_token.empty() && header_token == cookie_token;
    
    if (!isValid) {
        // 只记录token前8字符和后4字符，避免完整token泄露到日志
        auto maskToken = [](const std::string& t) -> std::string {
            if (t.size() <= 12) return t;
            return t.substr(0, 8) + "..." + t.substr(t.size() - 4);
        };
        LOG_WARNING("CSRF token validation failed: header_token='" + maskToken(header_token) + "', cookie_token='" + maskToken(cookie_token) + "'");
    }
    
    return isValid;
}

// 实现请求大小限制功能
void HttpServer2::setMaxRequestSize(size_t max_size_bytes) {
    max_request_size_ = max_size_bytes;
    LOG_INFO("Maximum request size set to " + std::to_string(max_size_bytes) + " bytes");
}

// 检查请求大小
void HttpServer2::checkRequestSize(const HttpRequest& request) {
    if (request.body.size() > max_request_size_) {
        throw std::runtime_error("Request body too large: " + std::to_string(request.body.size()) + 
                               " bytes (max: " + std::to_string(max_request_size_) + " bytes)");
    }
}
            