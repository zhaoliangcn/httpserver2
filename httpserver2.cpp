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
#include <system_error>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <cctype>
#include <zlib.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <boost/bind/bind.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include "ws/websocket.h"

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
    
    // 定期回收空条目：旧实现只清理 vector 内容、从不删除 map 条目，
    // 攻击者用大量随机源 IP 即可让该 map 无限增长（内存型 DoS）。
    if (ip_requests_.size() > 10000) {
        for (auto it = ip_requests_.begin(); it != ip_requests_.end(); ) {
            bool stale = true;
            for (const auto& t : it->second) {
                if (t >= now - window_size_) { stale = false; break; }
            }
            if (stale) it = ip_requests_.erase(it);
            else ++it;
        }
    }
    
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
    
    // zlib 的 avail_in 是 uInt（32 位），超过 4GB 会被静默截断，
    // 结果是一个只含前 4GB 数据的"合法"压缩流 —— 必须提前返回。
    if (data.size() > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        LOG_WARNING("gzipCompress: input exceeds 4GB, skipped");
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
    // 上面的 4GB 检查已保证窄化安全，这里显式 cast 让意图明确（否则 MSVC 报 C4267）
    zs.next_in = (Bytef*)data.data();
    zs.avail_in = static_cast<uInt>(data.size());
    
    // 输出缓冲区
    std::string compressed;
    std::vector<char> buffer(CHUNK_SIZE);
    
    // 压缩数据
    do {
        zs.next_out = (Bytef*)buffer.data();
        zs.avail_out = static_cast<uInt>(buffer.size());
        
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
// 时间 / 日志辅助（线程安全版本）
// ============================================================
// std::localtime / std::gmtime 返回指向静态缓冲区的指针，多线程并发调用是数据竞争
// （会产生错乱的时间戳，甚至崩溃）。统一走 *_r / *_s 版本。
static std::tm localTimeSafe(std::time_t t) {
    std::tm tm_buf{};
#if defined(_WIN32) && defined(_MSC_VER)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    return tm_buf;
}

static std::tm gmTimeSafe(std::time_t t) {
    std::tm tm_buf{};
#if defined(_WIN32) && defined(_MSC_VER)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    return tm_buf;
}

// JSON 字符串转义 —— 日志与错误响应都由用户可控内容拼接，
// 不转义会让引号/换行破坏 JSON（日志注入、客户端解析失败）。
static std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 纯文本日志字段清理：把控制字符（含换行）替换掉，防止伪造日志行
static std::string sanitizeLogField(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        unsigned char uc = static_cast<unsigned char>(c);
        out += (uc < 0x20 || uc == 0x7f) ? ' ' : c;
    }
    return out;
}

// 密码学安全随机（CSRF 令牌、盐、WebSocket 会话 ID 等）。
// std::mt19937 的输出可由少量样本推导，不能用于安全令牌。
static std::string secureRandomHex(size_t num_bytes) {
    std::vector<unsigned char> buf(num_bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        // 兜底：RAND_bytes 失败极罕见（熵源异常），退化到随机设备 + 计数器，
        // 至少不返回可预测的空串。
        static std::atomic<uint64_t> fallback_counter{0};
        std::random_device rd;
        for (size_t i = 0; i < num_bytes; ++i) {
            buf[i] = static_cast<unsigned char>((rd() ^ fallback_counter.fetch_add(1)) & 0xff);
        }
        LOG_CRITICAL("RAND_bytes failed, falling back to weak randomness");
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(num_bytes * 2);
    for (unsigned char c : buf) {
        out += hex[c >> 4];
        out += hex[c & 0x0f];
    }
    return out;
}

// ============================================================
// 访问日志（独立的 Apache 风格 access log）
// ============================================================
static std::mutex g_access_log_mutex;
static std::ofstream g_access_log_file;   // 常驻句柄：避免每请求 open/close

void logAccess(const std::string& client_ip, const std::string& method, const std::string& path,
               const std::string& status_code, const std::string& user_agent, int response_time_ms,
               const std::string& request_id) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm_buf = localTimeSafe(time_t_now);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S.")
       << std::setw(3) << std::setfill('0') << ms;
    std::string timestamp = ss.str();

    // JSON 格式结构化日志（所有字段转义）
    std::string json_entry;
    {
        std::stringstream js;
        js << "{\"time\":\"" << timestamp << "\","
           << "\"client_ip\":\"" << jsonEscape(client_ip) << "\","
           << "\"method\":\"" << jsonEscape(method) << "\","
           << "\"path\":\"" << jsonEscape(path) << "\","
           << "\"status\":\"" << jsonEscape(status_code) << "\","
           << "\"duration_ms\":" << response_time_ms << ","
           << "\"user_agent\":\"" << jsonEscape(user_agent) << "\"";
        if (!request_id.empty()) {
            js << ",\"request_id\":\"" << jsonEscape(request_id) << "\"";
        }
        js << "}";
        json_entry = js.str();
    }

    LOG_INFO("[ACCESS] " + json_entry);

    // 纯文本 access.log，句柄复用 + 互斥保护
    std::lock_guard<std::mutex> lock(g_access_log_mutex);
    if (!g_access_log_file.is_open()) {
        g_access_log_file.open("access.log", std::ios::app);
    }
    if (g_access_log_file.is_open()) {
        std::string plain_entry = timestamp + " - " + sanitizeLogField(client_ip) + " - \"" +
                                  sanitizeLogField(method) + " " + sanitizeLogField(path) + "\" " +
                                  sanitizeLogField(status_code) + " " + std::to_string(response_time_ms) +
                                  "ms - \"" + sanitizeLogField(user_agent) + "\"";
        if (!request_id.empty()) {
            plain_entry += " - id:" + sanitizeLogField(request_id);
        }
        g_access_log_file << plain_entry << std::endl;
    }
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
    
    // 构建JSON响应体（message 可能来自异常文本等外部输入，必须转义）
    std::string error_message = message.empty() ? default_message : message;
    response.body = "{";
    response.body += "\"error\":{";
    response.body += "\"code\":\"" + jsonEscape(status_code) + "\",";
    response.body += "\"message\":\"" + jsonEscape(error_message) + "\"";
    response.body += "}";
    response.body += "}";
    
    // 设置Content-Length
    response.headers["Content-Length"] = std::to_string(response.body.size());
    
    return response;
}

// 错误处理中间件工厂函数
Middleware createErrorHandlingMiddleware() {
    // 纯放行中间件：异常捕获已统一收敛到 handleMiddlewareChain（fail-closed）。
    // 旧实现返回 404 来"表示继续"，与真实 404 无法区分。
    return [](HttpRequest& /* request */) -> std::optional<HttpResponse> {
        return std::nullopt;
    };
}

// ============================================================
// 安全头添加函数
// ============================================================
// CORS 头辅助函数（前向声明，实现在后面）
static void addCorsHeaders(HttpResponse& response, const CorsConfig& config, const std::string& request_origin = "");

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
        "geolocation=()";

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
    // 添加请求 ID 头
    if (!request.request_id.empty()) {
        response.headers["X-Request-Id"] = request.request_id;
    }
    
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
            // 使用 CSPRNG 生成（std::mt19937 可预测，不能作为安全令牌）
            std::string csrf_token = generateCsrfToken();
            std::string cookie = csrf_cookie_name_ + "=" + csrf_token + "; HttpOnly=false; SameSite=Strict; Path=/";
            if (use_ssl) cookie += "; Secure";
            // 用 addCookie 而不是 headers["Set-Cookie"]：map 只能存一个，
            // 会覆盖 Session 中间件下发的 Set-Cookie。
            response.addCookie(cookie);
        }
    }

    // 添加安全头
    addSecurityHeaders(response, use_ssl);
    
    // 添加 CORS 头（如果启用）
    if (cors_config_.enabled) {
        std::string origin;
        auto origin_it = request.headers.find("Origin");
        if (origin_it != request.headers.end()) {
            origin = origin_it->second;
        }
        addCorsHeaders(response, cors_config_, origin);
    }
}

// 判断 MIME 是否值得 gzip（对已压缩格式再压只是浪费 CPU）
static bool isCompressibleContentType(const std::string& content_type) {
    std::string base = content_type.substr(0, content_type.find(';'));
    // 去首尾空白 + 转小写
    size_t b = base.find_first_not_of(" \t");
    size_t e = base.find_last_not_of(" \t");
    if (b == std::string::npos) return false;
    base = base.substr(b, e - b + 1);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (base.rfind("text/", 0) == 0) return true;
    return base == "application/json" ||
           base == "application/javascript" ||
           base == "application/xml" ||
           base == "application/xhtml+xml" ||
           base == "application/rss+xml" ||
           base == "application/atom+xml" ||
           base == "image/svg+xml";
}

// 状态码解析 + 合法性校验：atoi 不会抛异常，且空串会得到 0，
// 直接 http::status(0) 会写出 "HTTP/1.1 0" 这种非法状态行。
static int parseStatusCode(const std::string& code_str) {
    if (code_str.empty()) return 200;
    int code = 0;
    auto res = std::from_chars(code_str.data(), code_str.data() + code_str.size(), code);
    if (res.ec != std::errc{} || code < 100 || code > 599) return 500;
    return code;
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
        if (on_complete) on_complete();
        return;
    }
    
    // 如果设置了 file_body_path，使用 sendfile 零拷贝发送
    if (!custom_response.file_body_path.empty()) {
        sendFileResponse(socket, custom_response, client_ip, path);
        // 修复：sendfile 分支此前直接 return，on_complete 不执行 →
        // active_connections_ 只增不减，优雅关闭永远等满超时。
        if (on_complete) on_complete();
        return;
    }
    
    auto response = std::make_shared<http::response<http::string_body>>();

    // 状态码（越界/空值回退）
    const int status_int = parseStatusCode(custom_response.status_code);
    response->result(http::status(status_int));

    // 响应体
    response->body() = custom_response.body;

    // 添加响应头
    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }
    // 多值 Set-Cookie（Session / CSRF / 代理上游可能同时下发）
    for (const auto& cookie : custom_response.set_cookies) {
        response->insert(http::field::set_cookie, cookie);
    }

    // Content-Type 默认值
    if (custom_response.headers.find("Content-Type") == custom_response.headers.end()) {
        response->set(http::field::content_type, "text/plain");
    }

    // gzip 压缩
    // 关键：上游/处理器可能已经设置了 Content-Encoding（例如反向代理原样透传
    // 上游的 gzip 响应），此时再次压缩会产出 gzip(gzip(body)) 却仍标注单层编码，
    // 客户端必然解码失败。
    const bool already_encoded =
        custom_response.headers.find("Content-Encoding") != custom_response.headers.end();
    const std::string content_type =
        custom_response.headers.count("Content-Type") ? custom_response.headers.at("Content-Type") : "text/plain";

    if (should_compress && !already_encoded && isCompressibleContentType(content_type) &&
        status_int != 206 && status_int != 304 &&
        !response->body().empty()) {
        std::string compressed = gzipCompress(response->body());
        if (compressed.size() < response->body().size()) {
            response->body() = std::move(compressed);
            response->set(http::field::content_encoding, "gzip");
        }
        // 只要响应可能随 Accept-Encoding 变化，就必须声明 Vary，
        // 否则共享缓存会把 gzip 版本发给不支持压缩的客户端。
        response->set(http::field::vary, "Accept-Encoding");
    }

    // 服务端 & 日期头
    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf = gmTimeSafe(now_c);
    std::stringstream date_ss;
    date_ss << std::put_time(&tm_buf, "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());

    // Content-Length & payload
    response->prepare_payload();
    response->keep_alive(keep_alive);

    // 异步发送
    http::async_write(socket, *response,
        [response, on_complete](beast::error_code ec, std::size_t) {
            if (ec && ec != boost::asio::error::eof &&
                ec != boost::asio::error::broken_pipe &&
                ec != boost::asio::error::connection_reset) {
                LOG_ERROR("Write error: " + ec.message());
            }
            // 无论是否 keep-alive 都必须回调：调用方在此递减活跃连接计数。
            // 旧实现只在 keep-alive 且无错误时回调，导致所有 Connection: close
            // 请求的计数只增不减（优雅关闭永远等满 30 秒）。
            if (on_complete) {
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
                                        bool should_compress,
                                        std::function<void()> on_complete) {
    // 如果响应已被流式发送，跳过
    if (custom_response.response_sent) {
        if (on_complete) on_complete();
        return;
    }
    
    // 如果设置了 file_body_path，使用 sendfile 零拷贝发送
    if (!custom_response.file_body_path.empty()) {
        sendFileResponse(ssl_socket_ptr, custom_response, client_ip, path);
        if (on_complete) on_complete();
        return;
    }
    
    auto response = std::make_shared<http::response<http::string_body>>();

    const int status_int = parseStatusCode(custom_response.status_code);
    response->result(http::status(status_int));

    response->body() = custom_response.body;

    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }
    for (const auto& cookie : custom_response.set_cookies) {
        response->insert(http::field::set_cookie, cookie);
    }

    if (custom_response.headers.find("Content-Type") == custom_response.headers.end()) {
        response->set(http::field::content_type, "text/plain");
    }

    const bool already_encoded =
        custom_response.headers.find("Content-Encoding") != custom_response.headers.end();
    const std::string content_type =
        custom_response.headers.count("Content-Type") ? custom_response.headers.at("Content-Type") : "text/plain";

    if (should_compress && !already_encoded && isCompressibleContentType(content_type) &&
        status_int != 206 && status_int != 304 &&
        !response->body().empty()) {
        std::string compressed = gzipCompress(response->body());
        if (compressed.size() < response->body().size()) {
            response->body() = std::move(compressed);
            response->set(http::field::content_encoding, "gzip");
        }
        response->set(http::field::vary, "Accept-Encoding");
    }

    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf = gmTimeSafe(now_c);
    std::stringstream date_ss;
    date_ss << std::put_time(&tm_buf, "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());

    response->prepare_payload();

    http::async_write(*ssl_socket_ptr, *response,
        [ssl_socket_ptr, response, on_complete](beast::error_code ec, std::size_t) {
            boost::ignore_unused(ec, response);
            // SSL 安全关闭
            boost::system::error_code shut_ec;
            ssl_socket_ptr->shutdown(shut_ec);
            if (ssl_socket_ptr->lowest_layer().is_open()) {
                boost::system::error_code close_ec;
                ssl_socket_ptr->lowest_layer().close(close_ec);
            }
            if (on_complete) {
                on_complete();
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
    response->result(http::status(parseStatusCode(custom_response.status_code)));
    response->version(11);
    
    // 复制响应头
    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }
    
    // 服务端头
    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf = gmTimeSafe(now_c);
    std::stringstream date_ss;
    date_ss << std::put_time(&tm_buf, "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());
    
    // Content-Length (file_body 不自动设置)
    response->content_length(file_size);
    
    // 同步发送（确保完整文件发送完成后再返回）
    beast::error_code write_ec;
    http::write(socket, *response, write_ec);
    if (write_ec) {
        LOG_ERROR("sendFileResponse write error: " + write_ec.message());
    }
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
    
    response->result(http::status(parseStatusCode(custom_response.status_code)));
    response->version(11);
    
    for (const auto& [key, value] : custom_response.headers) {
        response->set(key, value);
    }
    
    response->set(http::field::server, "BoostBeastHttpServer/2.0");
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf = gmTimeSafe(now_c);
    std::stringstream date_ss;
    date_ss << std::put_time(&tm_buf, "%a, %d %b %Y %H:%M:%S GMT");
    response->set(http::field::date, date_ss.str());
    
    response->content_length(file_size);
    
    // 同步发送（确保完整文件发送完成后再返回）
    beast::error_code write_ec;
    http::write(*ssl_socket_ptr, *response, write_ec);
    if (write_ec) {
        LOG_ERROR("sendFileResponse(SSL) write error: " + write_ec.message());
    }
    // 关闭 SSL 连接
    boost::system::error_code shut_ec;
    ssl_socket_ptr->shutdown(shut_ec);
    if (ssl_socket_ptr->lowest_layer().is_open()) {
        boost::system::error_code close_ec;
        ssl_socket_ptr->lowest_layer().close(close_ec);
    }
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
    double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
                                 now - start_time_).count();
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
                if (result.ec == std::errc{} && result.ptr == encoded.data() + i + 3) {
                    decoded += static_cast<char>(byte_val);
                    i += 2;
                    continue;
                }
            }
            // 非法转义（如 "%zz"、结尾孤立的 "%"）保留原字符，
            // 旧实现会把这个 '%' 直接吞掉，导致 key 被悄悄改写。
            decoded += '%';
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
            
            const std::string inner = regex_pattern.substr(open_brace + 1, close_brace - open_brace - 1);
            const bool is_identifier = !inner.empty() &&
                std::all_of(inner.begin(), inner.end(), [](unsigned char c) {
                    return std::isalnum(c) != 0 || c == '_';
                });
            // {id} → 匹配任意一段；{[0-9]+} → 把花括号里的内容当正则片段
            // （README 里写的 /api/users/{[0-9]+} 正是后者，旧实现一律换成
            //  ([^/]+)，导致"正则路由"其实匹配任意段）
            const std::string replacement = is_identifier ? std::string("([^/]+)")
                                                          : std::string("(") + inner + ")";
            regex_pattern.replace(open_brace, close_brace - open_brace + 1, replacement);
            start_pos = open_brace + replacement.size(); // 旧实现硬编码 +8，替换串变长会漏字符
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
            // matches.size() 返回 boost::smatch::size_type（有符号），用同样的
            // 类型遍历，避免 size_t / ptrdiff_t 混比产生的告警与潜在下溢。
            // boost::smatch::size_type 是 ptrdiff_t（有符号），std::to_string
            // 没有对应的重载，转换到 long long 才是无歧义的。
            for (boost::smatch::size_type i = 1; i < matches.size(); i++) {
                path_params[std::to_string(static_cast<long long>(i))] = matches[i].str();
            }
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        // 注意：不要在这里现构造 boost::regex —— 异常路径上会变成"每请求编译一次正则"。
        // regex_ 才是唯一可信的匹配器；匹配过程出错按不匹配处理（fail-closed）。
        LOG_ERROR("Regex matching error: " + std::string(e.what()));
        return false;
    }
}

// 创建适合的路由匹配器
std::shared_ptr<RouteMatcher> HttpServer2::createRouteMatcher(const std::string& path) {
    // 含 {..}：花括号内是标识符 → 参数路由；花括号内是正则片段 → 正则路由
    // （支持 README 中 /api/users/{[0-9]+} 的写法）
    if (path.find('{') != std::string::npos && path.find('}') != std::string::npos) {
        const size_t open = path.find('{');
        const size_t close = path.find('}', open);
        if (close != std::string::npos) {
            const std::string inner = path.substr(open + 1, close - open - 1);
            const bool is_identifier = !inner.empty() &&
                std::all_of(inner.begin(), inner.end(), [](unsigned char c) {
                    return std::isalnum(c) != 0 || c == '_';
                });
            if (is_identifier) {
                LOG_DEBUG("Creating ParametricRouteMatcher for pattern: '" + path + "'");
                return std::make_shared<ParametricRouteMatcher>(path);
            }
        }
        LOG_DEBUG("Creating RegexRouteMatcher for brace-regex pattern: '" + path + "'");
        return std::make_shared<RegexRouteMatcher>(path);
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
        // 允许 stop() 后重新 start()
        stopped_.store(false);

        // 端口合法性检查。
        // tcp::endpoint 的端口类型是 uint16_t，直接把 int 传进去会静默截断：
        // start(70000) 会变成 4464，start(-1) 会变成 65535 —— 监听的端口与
        // 调用方以为的完全不符，且没有任何提示。这里提前拦下。
        if (port <= 0 || port > 65535) {
            throw std::invalid_argument(
                "Invalid port " + std::to_string(port) + ": must be in range 1-65535");
        }

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
        // 端口类型是 net::ip::port_type（uint_least16_t），不是 tcp::port_type
        acceptor_ = std::make_unique<tcp::acceptor>(
            *main_io_context,
            tcp::endpoint(tcp::v4(), static_cast<net::ip::port_type>(port)));
        
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
        
        // 注意：这里只对外提供 HTTP/1.1。HTTP/2 模块已编译进库但 acceptor 未接入，
        // 打印 HTTP/2 会让使用者以为可以用 ALPN / h2 访问。
        std::cout << "HTTP Server (HTTP/1.1) started on port " << port
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
        // 不要先把 running_ 置 false 再 stop()：那样 stop() 会因"未运行"提前返回，
        // 已经创建的工作线程不会 join，析构时 std::terminate。
        stop();
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

// ============================================================
// 统一的请求处理模板（消除 HTTP/HTTPS 重复代码）
// ============================================================
template <typename StreamType>
void HttpServer2::handleClient(StreamType stream, bool use_ssl) {
    active_connections_++;
    
    // 获取最低层 socket（SSL 流需要 .lowest_layer()）
    auto getLowestLayer = [](auto& s) -> auto& {
        if constexpr (std::is_same_v<std::decay_t<decltype(s)>, ssl::stream<tcp::socket>>) {
            return s.lowest_layer();
        } else {
            return s;
        }
    };
    
    auto& lowest_layer = getLowestLayer(stream);
    
    // remote_endpoint() 在连接已断开时会抛异常；而异常从 async 完成回调里逃出
    // io_context::run() 会直接终结该 worker 线程（容量永久减少）。统一用 error_code 版本。
    boost::system::error_code ep_ec;
    const auto remote_ep = lowest_layer.remote_endpoint(ep_ec);
    if (ep_ec) {
        LOG_DEBUG("Connection closed before request handling: " + ep_ec.message());
        active_connections_--;
        return;
    }
    const std::string client_ip = remote_ep.address().to_string();

    // 创建调试信息对象
    std::shared_ptr<DebugInfo> debug_info;
    if (debug_mode_) {
        debug_info = std::make_shared<DebugInfo>();
        debug_info->start_time = std::chrono::steady_clock::now();
        debug_info->client_ip = client_ip;
        debug_info->client_port = remote_ep.port();
    }
    
    PerformanceMonitor::recordRequest();
    
    // 检查限流 — 全局 RPS 与 IP 级窗口限流互相独立，任一超限即拒绝。
    // 旧实现只在 requests_per_second_ > 0 时才检查 rate_limiter_，
    // 导致"只配置 per-IP 限流"完全不生效。
    const bool global_limited = (requests_per_second_.load() > 0) && isRateLimited();
    const bool ip_limited = ip_rate_limit_enabled_.load() && !rate_limiter_.isAllowed(client_ip);
    if (global_limited || ip_limited) {
        try {
            http::response<http::string_body> response;
            response.version(11);
            response.result(http::status::too_many_requests);
            response.set(http::field::content_type, "text/plain");
            response.set(http::field::retry_after, "60");
            response.body() = "Too many requests, please try again later.";
            response.prepare_payload();
            http::write(stream, response);
            lowest_layer.shutdown(tcp::socket::shutdown_send);
        } catch (...) {}
        active_connections_--;
        return;
    }
    
    // 创建 shared_ptr 管理流生命周期
    auto stream_ptr = std::make_shared<StreamType>(std::move(stream));
    auto buffer = std::make_shared<beast::flat_buffer>(8192);
    auto parser = std::make_shared<http::request_parser<http::string_body>>();
    parser->body_limit(max_request_size_);
    
    auto timer = std::make_shared<net::deadline_timer>(*io_contexts_[0]);
    std::chrono::seconds timeout = request_timeout_;
    if (timeout.count() <= 0) timeout = std::chrono::seconds(60);
    timer->expires_from_now(boost::posix_time::seconds(timeout.count()));
    
    timer->async_wait([stream_ptr](const boost::system::error_code& ec) {
        if (!ec) {
            try {
                auto& ll = [&]() -> auto& {
                    if constexpr (std::is_same_v<StreamType, ssl::stream<tcp::socket>>) {
                        return stream_ptr->lowest_layer();
                    } else {
                        return *stream_ptr;
                    }
                }();
                if (ll.is_open()) {
                    boost::system::error_code close_ec;
                    ll.shutdown(tcp::socket::shutdown_both, close_ec);
                    ll.close(close_ec);
                }
            } catch (...) {}
        }
    });
    
    // 异步读取请求
    // 捕获 client_ip：WebSocket 升级分支要用它构造会话（按值拷贝，每个连接一份）。
    http::async_read(*stream_ptr, *buffer, *parser,
        [this, stream_ptr, buffer, parser, timer, debug_info, use_ssl, client_ip](beast::error_code ec, std::size_t bytes_transferred) {
            boost::system::error_code timer_ec;
            timer->cancel(timer_ec);
            boost::ignore_unused(bytes_transferred);
            
            auto& ll = [&]() -> auto& {
                if constexpr (std::is_same_v<StreamType, ssl::stream<tcp::socket>>) {
                    return stream_ptr->lowest_layer();
                } else {
                    return *stream_ptr;
                }
            }();
            
            if (ec && ec == http::error::body_limit) {
                LOG_WARNING("Request body limit exceeded");
                try {
                    http::response<http::string_body> r;
                    r.result(http::status::payload_too_large);
                    r.body() = "Request body exceeds maximum allowed size";
                    r.prepare_payload(); r.set(http::field::connection, "close");
                    http::write(*stream_ptr, r);
                    if (ll.is_open()) ll.close();
                } catch (...) {}
                active_connections_--;
                return;
            }
            if (ec) {
                if (ec != http::error::end_of_stream) LOG_ERROR("Read error: " + ec.message());
                active_connections_--;
                return;
            }
            
            try {
                const auto& beast_request = parser->get();
                // 统一的计时起点：性能指标与访问日志都基于它，
                // 不再依赖 debug_mode_（旧实现非 debug 下响应时间恒为 0）。
                const auto request_start = std::chrono::steady_clock::now();
                
                // 请求大小检查
                if (beast_request.body().size() > max_request_size_) {
                    LOG_WARNING("Request body too large: " + std::to_string(beast_request.body().size()));
                    http::response<http::string_body> r;
                    r.result(http::status::payload_too_large);
                    r.body() = "Request body exceeds maximum allowed size";
                    r.prepare_payload(); r.set(http::field::connection, "close");
                    http::write(*stream_ptr, r);
                    if (ll.is_open()) ll.close();
                    active_connections_--;
                    return;
                }
                
                // 基本认证
                if (!basic_auth_realm_.empty()) {
                    auto auth_it = beast_request.find(http::field::authorization);
                    std::string auth_h = (auth_it != beast_request.end()) ? std::string(auth_it->value()) : "";
                    if (!validateBasicAuth(auth_h)) {
                        http::response<http::string_body> r;
                        r.result(http::status::unauthorized);
                        r.set(http::field::www_authenticate, "Basic realm=\"" + basic_auth_realm_ + "\"");
                        r.body() = "Unauthorized"; r.prepare_payload();
                        http::write(*stream_ptr, r);
                        if (ll.is_open()) ll.close();
                        active_connections_--;
                        return;
                    }
                }
                
                // 转换请求
                HttpRequest custom_request = convertFromBeastRequest(beast_request);
                static std::atomic<uint64_t> req_counter{0};
                uint64_t seq = req_counter++;
                auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::stringstream rid_ss; rid_ss << std::hex << now_us << "-" << std::hex << seq;
                custom_request.request_id = rid_ss.str();

                // 记录 TCP 对端地址：handler 里取不到 socket，审计/限流需要来源 IP。
                // 用 lowest_layer() 统一处理 HTTP 与 HTTPS（ssl_stream 也能拿到底层 socket）。
                {
                    boost::system::error_code peer_ec;
                    const auto endpoint = stream_ptr->lowest_layer().remote_endpoint(peer_ec);
                    if (!peer_ec) {
                        custom_request.remote_ip = endpoint.address().to_string();
                    }
                }

                // 反向代理流式转发 / WebSocket 代理需要直接写回客户端 socket。
                // 旧实现从未给该字段赋值 —— 于是 can_stream 恒 false（代理永远走缓冲）
                // 且 WS 代理恒返回 400。这里在明文 HTTP 下把 socket 交给请求对象。
                // （TLS 下不暴露底层 socket：代理需要走缓冲路径。）
                if constexpr (std::is_same_v<StreamType, tcp::socket>) {
                    custom_request.client_socket = stream_ptr;
                }
                
                // WebSocket 升级检测（仅对非 SSL 连接编译）
                if constexpr (std::is_same_v<StreamType, tcp::socket>) {
                    auto upgrade_it = beast_request.find(http::field::upgrade);
                    if (upgrade_it != beast_request.end()) {
                        std::string upgrade_val(upgrade_it->value());
                        if (upgrade_val == "websocket" || upgrade_val == "WebSocket") {
                            auto ws_it = ws_handlers_.find(custom_request.path);
                            if (ws_it != ws_handlers_.end()) {
                                std::unordered_map<std::string, std::string> query_params;
                                auto target = beast_request.target();
                                std::string target_str(target.data(), target.size());
                                auto qpos = target_str.find('?');
                                if (qpos != std::string::npos) {
                                    parseQueryParams(target_str.substr(qpos + 1), query_params);
                                }
                                namespace beast_ws = boost::beast::websocket;
                                auto ws_stream = std::make_shared<beast_ws::stream<tcp::socket>>(std::move(*stream_ptr));
                                try {
                                    ws_stream->accept(beast_request);
                                    // 会话 ID 用 CSPRNG：MT19937 输出可预测，且会话 ID 可被枚举
                                    std::string ws_id = secureRandomHex(16);
                                    auto ws_session = std::make_shared<http2::ws::WebSocketSession>(
                                                                            ws_stream, ws_id, custom_request.headers, query_params,
                                                                            client_ip, *ws_it->second);
                                    http2::ws::g_ws_manager.add(ws_session);
                                    ws_session->start();
                                    // 连接已移交给 WebSocket 会话管理，不再计入 HTTP 活跃连接。
                                    // 旧实现此处直接 return，计数器只增不减。
                                    active_connections_--;
                                    return;
                                } catch (const std::exception& e) {
                                    LOG_ERROR("WebSocket upgrade failed: " + std::string(e.what()));
                                    if (ll.is_open()) ll.close();
                                    active_connections_--;
                                    return;
                                }
                            }
                        }
                    }
                }
                
                // CSRF 检查
                if (csrf_protection_enabled_) {
                    HttpResponse csrf_resp; addCsrfTokenToResponse(csrf_resp, custom_request);
                    std::string method = std::string(beast_request.method_string());
                    if (method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH") {
                        if (!validateCsrfToken(custom_request)) {
                            HttpResponse http_resp = createErrorResponse(HttpErrorType::FORBIDDEN, "CSRF token validation failed");
                            http::response<http::string_body> r;
                            r.version(11); r.result(http::status::forbidden);
                            r.set(http::field::content_type, "application/json");
                            r.body() = http_resp.body; r.prepare_payload();
                            http::write(*stream_ptr, r);
                            if (ll.is_open()) ll.close();
                            active_connections_--;
                            return;
                        }
                    }
                }
                
                // 路由查找
                // Host 头单独传给 findRoute 做虚拟主机匹配，不混进 path_params
                // （旧实现把 Host 塞进参数表，静态文件子路径处理器按"第一个参数"
                //   取相对路径时会取到 Host 值）。
                const std::string host_header =
                    custom_request.headers.count("Host") ? custom_request.headers.at("Host") : "";

                auto route_start = std::chrono::steady_clock::now();
                std::unordered_map<std::string, std::string> path_params;
                auto route_handler = findRoute(custom_request.method, host_header,
                                               custom_request.path, path_params);
                custom_request.path_params = std::move(path_params);
                
                // 处理请求（中间件链 + 路由处理器）
                // 中间件返回 nullopt = 放行；返回响应 = 已处理。
                // 旧实现「响应是 404 就再调一次 handler」会让 handler 执行两次，
                // 且第二次完全绕过中间件链（鉴权可被跳过）。
                HttpResponse custom_response;
                if (route_handler) {
                    custom_response = handleMiddlewareChain(custom_request, global_middlewares_, 0, route_handler->handler);
                } else {
                    custom_response = defaultHandler(custom_request);
                }
                
                // 合并中间件挂起的响应头 / Cookie（用户处理器已显式设置的优先）
                for (const auto& [k, v] : custom_request.pending_response_headers) {
                    if (custom_response.headers.find(k) == custom_response.headers.end()) {
                        custom_response.headers[k] = v;
                    }
                }
                for (const auto& cookie : custom_request.pending_response_cookies) {
                    custom_response.addCookie(cookie);
                }
                
                if (debug_info) {
                    debug_info->routing_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - route_start);
                }
                
                // 性能记录（与 debug 模式解耦）
                const int response_time = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_start).count());
                PerformanceMonitor::recordResponse(custom_request.path, response_time,
                    custom_response.body.size(), parseStatusCode(custom_response.status_code) < 400);
                PerformanceMonitor::recordBytesReceived(beast_request.body().size());
                
                // 调试头
                if (debug_mode_ && debug_info) {
                    debug_info->end_time = std::chrono::steady_clock::now();
                    auto td = std::chrono::duration_cast<std::chrono::milliseconds>(debug_info->end_time - debug_info->start_time);
                    custom_response.headers["X-Debug-Time"] = std::to_string(td.count()) + "ms";
                    custom_response.headers["X-Debug-Client-IP"] = debug_info->client_ip;
                    custom_response.headers["X-Debug-Route-Time"] = std::to_string(debug_info->routing_time.count()) + "ms";
                }
                
                // 访问日志：无论是否 debug 都记录（README 承诺的访问日志不应只在调试模式产出）。
                // 同时修正了原来的参数顺序错误（method / path 之前是反的）。
                logAccess(custom_request.remote_ip.empty() ? client_ip : custom_request.remote_ip,
                          custom_request.method, custom_request.path,
                          custom_response.status_code,
                          custom_request.headers.count("User-Agent") ? custom_request.headers.at("User-Agent") : "-",
                          response_time, custom_request.request_id);
                
                // 最终处理（CSRF cookie + 安全头 + 调试头 + CORS）
                finalizeResponse(custom_response, custom_request, use_ssl, debug_info);
                
                // gzip 检查
                bool should_compress = false;
                auto ae_it = beast_request.find(http::field::accept_encoding);
                if (ae_it != beast_request.end()) {
                    if (std::string(ae_it->value()).find("gzip") != std::string::npos)
                        should_compress = true;
                }
                
                bool keep_alive = beast_request.keep_alive();
                
                // 发送响应
                if constexpr (std::is_same_v<StreamType, ssl::stream<tcp::socket>>) {
                    buildAndSendResponse(stream_ptr, custom_response,
                        debug_info ? debug_info->client_ip : "unknown",
                        custom_request.path, false, should_compress,
                        [this]() { active_connections_--; });
                } else {
                    // 统一回调：先递减活跃连接数；keep-alive 且连接仍可用时继续读下一个请求。
                    // 不再区分 keep_alive 用两个 lambda —— 旧实现里非 keep-alive 分支
                    // 的 lambda 从来不会被调用，计数只增不减。
                    auto on_complete = std::function<void()>([this, stream_ptr, keep_alive]() {
                        active_connections_--;
                        if (keep_alive && stream_ptr && stream_ptr->is_open() && !shutdown_in_progress_) {
                            try {
                                handleClient(std::move(*stream_ptr), false);
                            } catch (const std::exception& e) {
                                LOG_ERROR("Keep-Alive restart error: " + std::string(e.what()));
                                // 注意：这里不能再减一次（外层已减），
                                // 重复递减会让 size_t 下溢成天文数字。
                            }
                        }
                    });
                    
                    buildAndSendResponse(*stream_ptr, custom_response,
                        debug_info ? debug_info->client_ip : "unknown",
                        custom_request.path, keep_alive, should_compress, on_complete);
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Client handling error: " + std::string(e.what()));
                try { if (ll.is_open()) ll.close(); } catch (...) {}
                active_connections_--;
            }
        }
    );
}

// 处理客户端连接（HTTP/1.1）— 委托给模板函数
void HttpServer2::handleClientHttp1(tcp::socket socket) {
    handleClient(std::move(socket), false);
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

// 虚拟主机路由注册
void HttpServer2::addRoute(const std::string& method, const std::string& host,
                          const std::string& path, const RouteHandler& handler) {
    std::shared_ptr<RouteMatcher> matcher = createRouteMatcher(path);
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);
    routes_.push_back({method, matcher, handler, {}, host});
}

// 添加中间件
void HttpServer2::use(const Middleware& middleware) {
    global_middlewares_.push_back(middleware);
}

// 便捷方法：GET路由
void HttpServer2::get(const std::string& path, const RouteHandler& handler) {
    addRoute("GET", path, handler);
}

// 虚拟主机 GET 路由
void HttpServer2::get(const std::string& host, const std::string& path, const RouteHandler& handler) {
    addRoute("GET", host, path, handler);
}

// 便捷方法：POST路由
void HttpServer2::post(const std::string& path, const RouteHandler& handler) {
    addRoute("POST", path, handler);
}

// 虚拟主机 POST 路由
void HttpServer2::post(const std::string& host, const std::string& path, const RouteHandler& handler) {
    addRoute("POST", host, path, handler);
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

// 检查是否超出全局限流（1 秒滑动窗口）
bool HttpServer2::isRateLimited() {
    const size_t limit = requests_per_second_.load();
    if (limit == 0) return false;  // 不限流
    
    // rate_limit_window_ 是普通 time_point，会被多个 io 线程并发读写，
    // 必须加锁（旧实现存在数据竞争，可能反复重置窗口）。
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(1);
    
    // 如果过了一秒钟，重置计数器
    if (now - rate_limit_window_ > window) {
        rate_limit_window_ = now;
        request_count_.store(1);
        return false;
    }
    
    // 增加请求计数并检查是否超过限制
    size_t count = request_count_.fetch_add(1) + 1;
    return count > limit;
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
                        const std::string host = item.request->headers.count("Host")
                            ? item.request->headers.at("Host") : "";
                        auto route_handler = findRoute(item.request->method, host,
                                                       item.request->path, item.request->path_params);
                        
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

                    // 合并中间件挂起的响应头 / Cookie。
                    // 与 handleClient 主路径保持一致：Session / CSRF / 缓存标记
                    // / X-Request-ID 这些中间件都通过 pending_* 通道下发，
                    // 漏掉这一步会让它们在这条路径上凭空消失。
                    if (item.request) {
                        for (const auto& [k, v] : item.request->pending_response_headers) {
                            if (response.headers.find(k) == response.headers.end()) {
                                response.headers[k] = v;
                            }
                        }
                        for (const auto& cookie : item.request->pending_response_cookies) {
                            response.addCookie(cookie);
                        }
                    }

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

// 设置全局 QPS 限流（0 表示不限）
void HttpServer2::setGlobalRateLimit(size_t requests_per_second, size_t max_queue_size) {
    requests_per_second_.store(requests_per_second);
    max_queue_size_ = max_queue_size;
    LOG_INFO("Global rate limit settings updated: " + (requests_per_second == 0 ? "unlimited" : std::to_string(requests_per_second) + " req/s") + 
             ", max_queue_size=" + std::to_string(max_queue_size));
}

// 停止服务器
void HttpServer2::stop() {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    
    // 幂等：已经完整回收过就直接返回
    if (stopped_.exchange(true)) {
        return;
    }
    
    // 注意：这里不能因为 running_==false 就提前返回。
    // start() 失败时线程可能已经创建，提前返回会留下 joinable 的 std::thread，
    // 导致 HttpServer2 析构时 std::terminate。
    running_ = false;
    shutdown_in_progress_ = true;
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
    
    // 等待活跃连接完成（最多等30秒）
    if (active_connections_ > 0) {
        std::cout << "Waiting for " << active_connections_ << " active connections to complete..." << std::endl;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (active_connections_ > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (active_connections_ > 0) {
            std::cout << "Graceful shutdown timeout: " << active_connections_ << " connections still active, forcing shutdown." << std::endl;
        } else {
            std::cout << "All active connections completed." << std::endl;
        }
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
    
    // 设置HTTP版本（req.version() 已经是 11 / 10，不能再拼一次 ".1"，
    // 旧实现会得到 "HTTP/11.1"）
    request.version = "HTTP/" + std::to_string(req.version() / 10) + "." +
                      std::to_string(req.version() % 10);
    
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
            // 结果必须写进 json_body：Validator 读的是 json_body，
            // 旧实现只写 form_data，导致所有 JSON 字段校验都被静默跳过。
            parseJsonBody(request.body, request.json_body);
            // 同时保留写进 form_data 的旧行为，避免已按 form_data 取值的处理器失效
            request.form_data.insert(request.json_body.begin(), request.json_body.end());
        }
    }
    
    // 使用移动语义而不是复制
    return request; // 移除std::move以允许复制省略
}

// 查找匹配的路由
// host 由调用方单独传入（虚拟主机匹配），不再混进 params。
std::optional<Route> HttpServer2::findRoute(const std::string& method, const std::string& host,
                                            const std::string& path,
                                            std::unordered_map<std::string, std::string>& params) {
    // 读锁覆盖整个查找过程（旧实现在加锁前先读了一次 routes_.empty()，属于数据竞争）
    std::shared_lock<std::shared_mutex> lock(routes_mutex_);
    
    for (const auto& route : routes_) {
        // 首先检查HTTP方法是否匹配
        if (route.method != "*" && route.method != method) {
            continue;
        }
        
        // 虚拟主机限制
        if (!route.host.empty() && host != route.host) {
            continue;
        }
        
        // 用临时容器接收参数：匹配失败的路由不会污染最终 path_params
        // （旧的复用同一个 params 会让失败匹配残留部分参数）
        std::unordered_map<std::string, std::string> matched;
        if (route.matcher->match(path, matched)) {
            params.insert(matched.begin(), matched.end());
            return route;
        }
    }
    
    // 如果没有找到匹配的路由，返回空
    return std::nullopt;
}



// 处理中间件链
// 契约：中间件返回 std::nullopt → 放行；返回 HttpResponse → 已处理，中断链。
HttpResponse HttpServer2::handleMiddlewareChain(HttpRequest& request, const std::vector<Middleware>& middlewares, size_t index, const RouteHandler& final_handler) {
    for (size_t i = index; i < middlewares.size(); ++i) {
        if (!middlewares[i]) continue;
        try {
            if (auto handled = middlewares[i](request)) {
                return *handled;
            }
        } catch (const std::exception& e) {
            // fail-closed：中间件抛异常时绝不能当作"放行"，
            // 否则一个抛异常的鉴权中间件就等于后门。
            LOG_ERROR("Middleware exception at index " + std::to_string(i) + ": " + std::string(e.what()));
            PerformanceMonitor::recordError();
            return createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Middleware error");
        } catch (...) {
            LOG_ERROR("Unknown middleware exception at index " + std::to_string(i));
            PerformanceMonitor::recordError();
            return createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Middleware error");
        }
    }
    
    // 所有中间件都放行，执行最终路由处理器
    if (!final_handler) {
        return defaultHandler(request);
    }
    
    try {
        return final_handler(request);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception in route handler for " + request.method + " " + request.path + ": " + std::string(e.what()));
        PerformanceMonitor::recordError();
        return createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Internal Server Error");
    } catch (...) {
        LOG_ERROR("Unknown exception in route handler for " + request.method + " " + request.path);
        PerformanceMonitor::recordError();
        return createErrorResponse(HttpErrorType::INTERNAL_ERROR, "Internal Server Error");
    }
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
    handleClient(std::move(ssl_socket), true);
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
            // 用 generic_string()：它在所有平台都用 '/' 作为分隔符。
            // 旧实现手工给 base 末尾补 '/'，而 Windows 的 string() 用反斜杠，
            // 前缀比较永远不成立 —— Windows 上所有静态文件都会被判 403。
            std::string canonical_base_dir_str =
                std::filesystem::canonical(base_dir).generic_string();
            const std::string canonical_path_str =
                std::filesystem::canonical(file_path).generic_string();
            
            LOG_DEBUG("serveStaticFile - Canonical path: " + canonical_path_str);
            LOG_DEBUG("serveStaticFile - Canonical base directory: " + canonical_base_dir_str);
            
            // 确保目录路径末尾有斜杠，避免部分匹配问题
            if (!canonical_base_dir_str.empty() && canonical_base_dir_str.back() != '/') {
                canonical_base_dir_str += '/';
            }
            
            // 检查文件是否在允许的目录内
            if (canonical_path_str.compare(0, canonical_base_dir_str.length(), canonical_base_dir_str) != 0) {
                LOG_WARNING("serveStaticFile - Security violation: file outside allowed directory");
                response.status_code = "403";
                response.status_message = "Forbidden";
                response.headers["Content-Type"] = "text/plain";
                response.body = "Access denied: file outside allowed directory";
                return response;
            }
            LOG_DEBUG("[serveStaticFile] Security check passed");
        } catch (const std::exception& e) {
            // fail-closed：无法确认路径归属时必须拒绝。
            // 旧实现只是打日志然后继续，等于把包含性检查静默跳过。
            LOG_ERROR("serveStaticFile - canonical path check failed: " + std::string(e.what()));
            response.status_code = "403";
            response.status_message = "Forbidden";
            response.headers["Content-Type"] = "text/plain";
            response.body = "Access denied: unable to verify path";
            return response;
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
        std::tm last_modified_tm = gmTimeSafe(last_modified_c);
        std::stringstream last_modified_ss;
        last_modified_ss << std::put_time(&last_modified_tm, "%a, %d %b %Y %H:%M:%S GMT");
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
            if (range_it != request.headers.end() && file_size > 0) {
                const std::string& range_hdr = range_it->second;
                bool invalid_range = false;
                // 仅支持 bytes=start-end 格式
                if (range_hdr.size() > 6 && range_hdr.compare(0, 6, "bytes=") == 0) {
                    std::string range_val = range_hdr.substr(6);
                    size_t dash = range_val.find('-');
                    if (dash != std::string::npos) {
                        const std::string start_str = range_val.substr(0, dash);
                        const std::string end_str = range_val.substr(dash + 1);
                        // 用 from_chars 而不是 stoull：后者遇到 "abc" 会抛异常，
                        // 让一个畸形 Range 头变成 500。
                        auto parse_u64 = [](const std::string& s, uint64_t& out) {
                            if (s.empty()) return false;
                            auto r = std::from_chars(s.data(), s.data() + s.size(), out);
                            return r.ec == std::errc{} && r.ptr == s.data() + s.size();
                        };
                        bool ok = true;
                        if (!start_str.empty()) ok = parse_u64(start_str, range_start);
                        if (ok && !end_str.empty()) ok = parse_u64(end_str, range_end);
                        if (ok) {
                            if (end_str.empty() || range_end >= file_size) range_end = file_size - 1;
                            if (range_start <= range_end && range_start < file_size) {
                                has_range = true;
                            } else {
                                invalid_range = true;
                            }
                        } else {
                            invalid_range = true;
                        }
                    } else {
                        invalid_range = true;
                    }
                } else {
                    invalid_range = true;
                }
                
                if (invalid_range && !has_range) {
                    // RFC 7233：无法满足的 Range 返回 416 + Content-Range: bytes */size
                    response.status_code = "416";
                    response.status_message = "Range Not Satisfiable";
                    response.headers["Content-Range"] = "bytes */" + std::to_string(file_size);
                    response.headers["Content-Type"] = "text/plain";
                    response.body = "Range Not Satisfiable";
                    return response;
                }
            }
            
            // 过大的 Range 退回整文件发送：避免 `bytes=0-9999999999` 触发 GB 级
            // 一次性内存分配（原实现直接 vector<char>(range_len)）。
            if (has_range && (range_end - range_start + 1) > (32ull * 1024 * 1024)) {
                LOG_WARNING("serveStaticFile - Range request too large, fallback to full file");
                has_range = false;
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
              // RegexRouteMatcher 用捕获组序号作参数名（^/static/(.*)$ → "1"）。
              // 不能用 begin()：unordered_map 迭代顺序不确定，而且此前 path_params
              // 里还混入过 Host 头，会把 Host 当成文件名去查磁盘。
              auto capture = request.path_params.find("1");
              relative_path = (capture != request.path_params.end()) ? capture->second : std::string();
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
    //
    // 不能直接用 std::stoi：一是它对非数字字符串抛 std::invalid_argument，
    // 而 parseUrl 没有异常契约，会把异常抛到调用方；二是 port 是 uint16_t，
    // 赋 70000 这类越界值会被静默截断成 4464，请求打到意外端口。
    // 这里用 from_chars 严格解析，越界或非法一律按默认端口处理。
    size_t port_start = parts.host.find(":");
    if (port_start != std::string::npos) {
        const std::string port_str = parts.host.substr(port_start + 1);
        unsigned long parsed_port = 0;
        const char* begin = port_str.data();
        const char* end = begin + port_str.size();
        const auto result = std::from_chars(begin, end, parsed_port);
        if (result.ec == std::errc() && result.ptr == end &&
            parsed_port > 0 && parsed_port <= 65535) {
            parts.port = static_cast<uint16_t>(parsed_port);
        } else {
            LOG_WARNING("Invalid port in URL: '" + port_str + "', using default port");
        }
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
static std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 逐跳（hop-by-hop）头：只对单条连接有意义，代理不得原样转发。
// 旧实现把上游的 Transfer-Encoding / Connection 直接抄给客户端，
// 配上被去分块（de-chunked）的 body，会让客户端解析失败。
static bool isHopByHopHeader(const std::string& name) {
    static const char* kHopByHop[] = {
        "connection", "keep-alive", "proxy-authenticate",
        "proxy-authorization", "te", "trailer", "transfer-encoding", "upgrade"
    };
    const std::string n = toLowerAscii(name);
    for (const char* h : kHopByHop) {
        if (n == h) return true;
    }
    return false;
}

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
    auto proxy_handler = [this, backends, request_headers, response_headers, counter](const HttpRequest& request) -> HttpResponse {
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
                    
                    // 复制请求头（跳过 Host 与逐跳头；Content-Length 由 prepare_payload 重算）
                    for (const auto& [key, value] : request.headers) {
                        if (toLowerAscii(key) == "host" || isHopByHopHeader(key) ||
                            toLowerAscii(key) == "content-length") {
                            continue;
                        }
                        proxy_request.set(key, value);
                    }
                    
                    // 添加/修改请求头
                    for (const auto& [key, value] : request_headers) {
                        proxy_request.set(key, value);
                    }
                    
                    // 设置请求体
                    proxy_request.body() = request.body;
                    
                    // 准备请求
                    proxy_request.prepare_payload();
                    
                    // 明确要求上游在响应后关闭连接：
                    // 否则 HTTP/1.1 默认 keep-alive，流式转发会在读完响应体后
                    // 继续阻塞在 read_some 上直到 5 秒超时（每个请求都卡）。
                    proxy_request.keep_alive(false);
                    
                    // 发送请求
                    http::write(stream, proxy_request);
                    
                    // 检查是否可流式转发：必须显式开启（默认关闭）+ 有客户端 socket + 非 SSL。
                    // 流式路径是同步阻塞写，且依赖对上游响应分帧的正确处理，
                    // 默认走下方"全量缓冲"路径以保证正确性。
                    const bool can_stream = proxy_streaming_enabled_.load() &&
                                            request.client_socket && request.client_socket->is_open();
                    
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
                            const std::string name = std::string(f.name_string());
                            const std::string lower = toLowerAscii(name);
                            // 逐跳头不转发；但 Transfer-Encoding / Content-Length 必须保留，
                            // 因为这里 body 是原始字节透传（含 chunk 帧），
                            // 去掉会让客户端按错误的边界解析。
                            if (lower == "connection" || lower == "keep-alive" ||
                                lower == "upgrade" || lower == "trailer" ||
                                lower == "te" || lower == "proxy-authenticate" ||
                                lower == "proxy-authorization") {
                                continue;
                            }
                            resp_header_ss << name << ": " << f.value() << "\r\n";
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
                        uint64_t total_bytes = 0;
                        
                        // read_header 可能把 body 的首段字节一起读进 header_buf，
                        // 直接丢弃会造成响应体截断 —— 先把残留字节转发出去。
                        if (header_buf.size() > 0) {
                            const char* leftover = static_cast<const char*>(header_buf.data().data());
                            boost::asio::write(*request.client_socket,
                                boost::asio::buffer(leftover, header_buf.size()));
                            total_bytes += header_buf.size();
                        }
                        
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
                    
                    // 复制响应头：跳过逐跳头与 Content-Length（交给 prepare_payload 重算），
                    // Set-Cookie 用多值容器保存，避免 map 覆盖后只下发一条。
                    for (const auto& field : proxy_response) {
                        const std::string name = std::string(field.name_string());
                        const std::string lower = toLowerAscii(name);
                        if (isHopByHopHeader(lower) || lower == "content-length") {
                            continue;
                        }
                        if (lower == "set-cookie") {
                            response.addCookie(std::string(field.value()));
                            continue;
                        }
                        response.headers[name] = std::string(field.value());
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
// 反向代理流式转发开关
// ============================================================
void HttpServer2::setProxyStreaming(bool enable) {
    proxy_streaming_enabled_.store(enable);
    if (enable) {
        LOG_WARNING("Reverse proxy streaming enabled: responses are written to the client "
                    "synchronously inside the io_context thread");
    } else {
        LOG_INFO("Reverse proxy streaming disabled (buffered mode)");
    }
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

// ============================================================
// 原生 WebSocket 端点注册
// ============================================================
void HttpServer2::addWebSocket(const std::string& path, const http2::ws::WebSocketHandler& handler) {
    ws_handlers_[path] = std::make_shared<http2::ws::WebSocketHandler>(handler);
    LOG_INFO("WebSocket endpoint registered: " + path);
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

// 设置 IP 级窗口限流（每 window_size 秒内允许 requests_per_window 次）
void HttpServer2::setRateLimit(uint32_t requests_per_window, uint32_t window_size_seconds) {
    if (window_size_seconds == 0) {
        LOG_WARNING("setRateLimit: window_size_seconds must be > 0, ignored");
        return;
    }
    
    rate_limiter_.setRateLimit(requests_per_window, window_size_seconds);
    ip_rate_limit_enabled_.store(requests_per_window > 0);
    
    // 不再顺带推导全局 RPS：旧实现用 requests_per_window / window_size_seconds
    // 做整除，100 次/60 秒会被算成 1 次/秒（比配置严 100 倍）。
    // 需要全局限流请显式调用 setGlobalRateLimit()。
    LOG_INFO("IP rate limit set to " + std::to_string(requests_per_window) + " requests per " +
             std::to_string(window_size_seconds) + "s");
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
            // 分片睡眠：直接 sleep(interval) 会让 stop() 的 join 最多干等一整个 interval
            auto waited = std::chrono::milliseconds(0);
            while (running_ && waited < thread_adjust_interval_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                waited += std::chrono::milliseconds(200);
            }
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
            // 分片睡眠，避免 stop() 被 config_check_interval_（默认 30s）拖住
            auto waited = std::chrono::milliseconds(0);
            while (running_ && waited < config_check_interval_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                waited += std::chrono::milliseconds(200);
            }
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

// PBKDF2-HMAC-SHA256 派生口令哈希（输出十六进制）
// 单轮 SHA-256(salt+password) 的离线爆破成本极低，不适合存口令。
static std::string pbkdf2Hex(const std::string& password, const std::string& salt_hex,
                             int iterations, int out_len = 32) {
    // salt 以十六进制字符串保存，这里还原成字节
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string salt;
    salt.reserve(salt_hex.size() / 2);
    for (size_t i = 0; i + 1 < salt_hex.size(); i += 2) {
        int hi = hexval(salt_hex[i]);
        int lo = hexval(salt_hex[i + 1]);
        if (hi < 0 || lo < 0) return "";
        salt += static_cast<char>((hi << 4) | lo);
    }
    
    std::vector<unsigned char> out(static_cast<size_t>(out_len));
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          out_len, out.data()) != 1) {
        return "";
    }
    
    static const char* hex = "0123456789abcdef";
    std::string res;
    res.reserve(out.size() * 2);
    for (unsigned char c : out) {
        res += hex[c >> 4];
        res += hex[c & 0x0f];
    }
    return res;
}

void HttpServer2::addUser(const std::string& username, const std::string& password) {
    // 128 位随机盐
    const std::string salt = secureRandomHex(16);
    const int iterations = 100000;
    const std::string password_hash = pbkdf2Hex(password, salt, iterations, 32);
    if (password_hash.empty()) {
        LOG_ERROR("addUser: PBKDF2 failed for user " + username);
        return;
    }
    
    // salt 字段里编码迭代次数，方便以后调参而不影响已存在的用户
    std::lock_guard<std::mutex> lock(basic_auth_mutex_);
    basic_auth_users_[username] = {std::to_string(iterations) + "$" + salt, password_hash};
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
        
        // 在锁内只做查表并拷贝，PBKDF2 计算放到锁外，
        // 否则多请求会在这把锁上串行化（每次 10 万轮迭代）。
        std::string stored_salt;
        std::string stored_hash;
        {
            std::lock_guard<std::mutex> lock(basic_auth_mutex_);
            auto it = basic_auth_users_.find(username);
            if (it == basic_auth_users_.end()) {
                return false;
            }
            stored_salt = it->second.first;
            stored_hash = it->second.second;
        }
        
        // stored_salt 格式："<iterations>$<hexsalt>"；兼容旧格式（纯盐）
        int iterations = 100000;
        std::string salt = stored_salt;
        auto dollar = stored_salt.find('$');
        if (dollar != std::string::npos) {
            iterations = std::atoi(stored_salt.substr(0, dollar).c_str());
            salt = stored_salt.substr(dollar + 1);
        }
        if (iterations <= 0) iterations = 100000;
        
        const std::string computed = pbkdf2Hex(password, salt, iterations, 32);
        if (computed.empty() || computed.size() != stored_hash.size()) {
            return false;
        }
        // 恒定时间比较
        return CRYPTO_memcmp(computed.data(), stored_hash.data(), computed.size()) == 0;
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

        // idx 最多 63，落在 int 范围内；显式转换消除窄化告警
        val = (val << 6) + static_cast<int>(idx);
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
    // 用 EVP 而非已废弃的 SHA256_Init/Update/Final：
    // OpenSSL 3.0 起那组低阶 API 被标记 deprecated，未来版本会移除。
    //
    // 注意：这不是用于口令存储的口令哈希（口令另有 PBKDF2 路径），
    // 这里只用于 ETag 等非机密场景的内容摘要。
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        LOG_ERROR("calculateSha256: EVP_MD_CTX_new failed");
        return std::string();
    }

    std::string result;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        LOG_ERROR("calculateSha256: digest computation failed");
    } else {
        std::stringstream ss;
        for (unsigned int i = 0; i < hash_len; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        result = ss.str();
    }

    EVP_MD_CTX_free(ctx);
    return result;
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
    // 128 位 CSPRNG 令牌（32 个十六进制字符）。
    // 旧实现用 std::mt19937：状态可从少量输出推导，CSRF 令牌不能可预测。
    return secureRandomHex(16);
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
        response.addCookie(cookie);
        LOG_DEBUG("Added CSRF token to response cookie");
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
    
    // 验证令牌是否匹配（恒定时间比较，避免逐字节时序猜测）
    bool isValid = !header_token.empty() && !cookie_token.empty() &&
                   header_token.size() == cookie_token.size() &&
                   CRYPTO_memcmp(header_token.data(), cookie_token.data(), header_token.size()) == 0;
    
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

// ============================================================
// CORS 支持
// ============================================================
void HttpServer2::enableCors(const CorsConfig& config) {
    cors_config_ = config;
    LOG_INFO("CORS enabled: origins=" + config.allowed_origins + 
             ", methods=" + config.allowed_methods + 
             ", credentials=" + (config.allow_credentials ? "true" : "false"));
}

void HttpServer2::setCorsOrigin(const std::string& origin) {
    cors_config_.enabled = true;
    cors_config_.allowed_origins = origin;
    LOG_INFO("CORS origin set to: " + origin);
}

void HttpServer2::setCorsCredentials(bool allow) {
    cors_config_.allow_credentials = allow;
}

// 添加 CORS 头到响应
static void addCorsHeaders(HttpResponse& response, const CorsConfig& config, const std::string& request_origin) {
    if (!config.enabled) return;
    
    // 处理通配符或特定 origin
    if (config.allowed_origins == "*") {
        response.headers["Access-Control-Allow-Origin"] = "*";
    } else if (!request_origin.empty()) {
        // 检查请求的 origin 是否在允许列表中
        std::string origins = config.allowed_origins;
        std::vector<std::string> origin_list;
        size_t pos = 0;
        while ((pos = origins.find(',')) != std::string::npos) {
            std::string origin = origins.substr(0, pos);
            origin.erase(0, origin.find_first_not_of(" \t"));
            origin.erase(origin.find_last_not_of(" \t") + 1);
            origin_list.push_back(origin);
            origins.erase(0, pos + 1);
        }
        origins.erase(0, origins.find_first_not_of(" \t"));
        origins.erase(origins.find_last_not_of(" \t") + 1);
        if (!origins.empty()) origin_list.push_back(origins);
        
        for (const auto& allowed : origin_list) {
            if (allowed == request_origin || allowed == "*") {
                response.headers["Access-Control-Allow-Origin"] = request_origin;
                break;
            }
        }
    }
    
    response.headers["Access-Control-Allow-Methods"] = config.allowed_methods;
    response.headers["Access-Control-Allow-Headers"] = config.allowed_headers;
    response.headers["Access-Control-Max-Age"] = std::to_string(config.max_age_seconds);
    
    if (config.allow_credentials) {
        response.headers["Access-Control-Allow-Credentials"] = "true";
    }
    
    if (!config.exposed_headers.empty()) {
        std::string exposed;
        for (size_t i = 0; i < config.exposed_headers.size(); ++i) {
            if (i > 0) exposed += ", ";
            exposed += config.exposed_headers[i];
        }
        response.headers["Access-Control-Expose-Headers"] = exposed;
    }
}
            