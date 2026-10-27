#ifndef HTTPSERVER2_H
#define HTTPSERVER2_H

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <regex>
#include <fstream>
#include <filesystem>
#include <optional>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex> // 引入读写锁支持
#include <queue>
#include <condition_variable>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/regex.hpp>
#include <openssl/sha.h>

// 引入单头文件日志库
#include "single_include/logger.h"

// WebSocket 前向声明
namespace http2 { namespace ws {
    struct WebSocketHandler;
    class WebSocketSession;
    class WebSocketManager;
    extern WebSocketManager g_ws_manager;
}}

// 日志级别枚举（与外部 logger 库对接）
// Windows SDK 的 wingdi.h 里定义了 `#define ERROR 0`，不取消定义的话
// `ERROR = 4` 会被展开成 `0 = 4`。
#ifdef ERROR
#undef ERROR
#endif
enum class LogLevel {
    DEBUG = 1,
    INFO  = 2,
    WARNING = 3,
    ERROR = 4,
    CRITICAL = 5
};

// 便捷日志宏 — 对接 single_include/logger.h 的 C API（printf 风格 + std::string 兼容）
// 所有现有的 LOG_DEBUG(msg) / LOG_INFO(msg) 等调用无需改动
// 先取消 logger.h 定义的 printf 风格宏，再重新定义为 std::string 风格
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
#define LOG_CRITICAL(msg) log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())

// 访问日志（独立的辅助函数，不走通用日志库）
void logAccess(const std::string& client_ip, const std::string& method, const std::string& path,
               const std::string& status_code, const std::string& user_agent, int response_time_ms,
               const std::string& request_id = "");

// 性能监控类
class PerformanceMonitor {
private:
    static std::atomic<uint64_t> total_requests_;
    static std::atomic<uint64_t> active_requests_;
    static std::atomic<uint64_t> total_response_time_;
    static std::atomic<uint64_t> max_response_time_;
    static std::atomic<uint64_t> min_response_time_;
    static std::atomic<uint64_t> request_errors_;
    static std::atomic<uint64_t> bytes_sent_;
    static std::atomic<uint64_t> bytes_received_;
    static std::chrono::steady_clock::time_point start_time_;
    static std::mutex metrics_mutex_;
    static std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> route_metrics_; // 路由 -> {请求数, 总响应时间}
    
public:
    static void reset();
    static void recordRequest();
    static void recordResponse(const std::string& route, uint64_t response_time_ms, size_t response_size, bool success = true);
    static void recordBytesReceived(size_t bytes);
    static void recordError();
    static double getAverageResponseTime();
    static uint64_t getTotalRequests();
    static uint64_t getActiveRequests();
    static uint64_t getRequestErrors();
    static uint64_t getMaxResponseTime();
    static uint64_t getMinResponseTime();
    static uint64_t getBytesSent();
    static uint64_t getBytesReceived();
    static double getRequestsPerSecond();
    static std::unordered_map<std::string, double> getRoutePerformance();
    static std::string generatePerformanceReport();
    static std::string generatePrometheusMetrics();
};

// 速率限制器类，用于防止DDoS攻击
class RateLimiter {
private:
    // 每个IP的请求记录，存储最近请求的时间戳
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> ip_requests_;
    std::mutex ip_requests_mutex_;
    
    // 速率限制参数
    uint32_t requests_per_window_; // 窗口内允许的最大请求数
    std::chrono::seconds window_size_; // 时间窗口大小（秒）
    
public:
    // 构造函数
    RateLimiter(uint32_t requests_per_window = 100, uint32_t window_size_seconds = 60);
    
    // 检查IP是否允许请求
    bool isAllowed(const std::string& client_ip);
    
    // 设置速率限制参数
    void setRateLimit(uint32_t requests_per_window, uint32_t window_size_seconds);
};

// 计时器类 — 替代START_TIMER/END_TIMER宏，避免__LINE__不匹配问题
class ScopedTimer {
    std::chrono::steady_clock::time_point start_;
public:
    ScopedTimer() : start_(std::chrono::steady_clock::now()) {}
    uint64_t elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
    }
};

#define START_TIMER() ScopedTimer ATOMCODE_TEMP_TIMER
#define END_TIMER() ATOMCODE_TEMP_TIMER.elapsedMs()
#define RECORD_METRICS(route, response, success) {\
    auto response_time = END_TIMER();\
    PerformanceMonitor::recordResponse(route, response_time, response.body.size(), success);\
}

// 类型别名（仅在头文件中使用，各.cpp文件有各自独立的别名）
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;

// 反向代理后端状态（健康检查）
struct BackendState {
    std::string url;
    std::atomic<bool> healthy{true};
    std::atomic<int> consecutive_failures{0};
    std::chrono::steady_clock::time_point last_check;
    int max_failures_before_down = 3;
    std::chrono::seconds recovery_interval{30};

    BackendState(const std::string& u) : url(u), last_check(std::chrono::steady_clock::now()) {}
    
    void recordSuccess() {
        healthy = true;
        consecutive_failures = 0;
        last_check = std::chrono::steady_clock::now();
    }
    
    void recordFailure() {
        consecutive_failures++;
        last_check = std::chrono::steady_clock::now();
        if (consecutive_failures >= max_failures_before_down) {
            healthy = false;
        }
    }
    
    bool shouldRetry() const {
        if (healthy) return true;
        auto elapsed = std::chrono::steady_clock::now() - last_check;
        return elapsed >= recovery_interval;
    }
    
    // 主动健康检查：尝试 TCP 连接
    bool checkHealth() const;
};

// 文件上传信息结构
struct UploadedFile {
    std::string name;          // 表单字段名
    std::string filename;      // 上传的文件名
    std::string content_type;  // 文件MIME类型
    std::vector<char> data;    // 文件内容
};

// HTTP请求结构
struct HttpRequest {
    std::string method;        // HTTP方法
    std::string path;          // 请求路径
    std::string version;       // HTTP版本
    std::unordered_map<std::string, std::string> headers;  // 请求头
    std::unordered_map<std::string, std::string> query_params;  // 查询参数
    std::unordered_map<std::string, std::string> form_data;  // 表单数据
    std::vector<UploadedFile> files;  // 上传的文件
    std::string body;          // 请求体
    std::unordered_map<std::string, std::string> path_params;  // 路径参数
    std::unordered_map<std::string, std::string> json_body;  // JSON请求体（解析后）
    std::atomic<bool> is_processed{false};  // 用于并发处理控制
    std::string request_id;     // 唯一请求 ID（用于链路追踪）

    // 中间件 → 最终响应的单向通道。
    // 中间件在 handler 之前执行，此时最终 HttpResponse 还不存在，
    // 需要下发 Cookie / 响应头的中间件（Session、Request-ID 等）把内容写在这里，
    // 框架在发送前合并进最终响应（用户处理器显式设置的头部优先）。
    std::unordered_map<std::string, std::string> pending_response_headers;
    std::vector<std::string> pending_response_cookies;

    // 客户端 TCP 对端地址（如 "127.0.0.1"）。
    // 由框架在收到请求时填入；HTTPS 与 HTTP 都可用。
    // 仅供审计/日志展示，转发头（X-Forwarded-For）可能更接近真实来源。
    std::string remote_ip;
    
    // 客户端 socket（流式响应用，如反向代理直接转发）
    // 仅 HTTP 使用；HTTPS 不流式转发
    std::shared_ptr<tcp::socket> client_socket;
    
    // 默认构造函数
    HttpRequest() = default;
    
    // 移动构造函数
    HttpRequest(HttpRequest&& other) noexcept
        : method(std::move(other.method))
        , path(std::move(other.path))
        , version(std::move(other.version))
        , headers(std::move(other.headers))
        , query_params(std::move(other.query_params))
        , form_data(std::move(other.form_data))
        , files(std::move(other.files))
        , body(std::move(other.body))
        , path_params(std::move(other.path_params))
        , json_body(std::move(other.json_body))
        , is_processed(other.is_processed.load())
        , request_id(std::move(other.request_id))
        , pending_response_headers(std::move(other.pending_response_headers))
        , pending_response_cookies(std::move(other.pending_response_cookies))
        , remote_ip(std::move(other.remote_ip))
        , client_socket(std::move(other.client_socket))
    {}
    
    // 移动赋值运算符
    HttpRequest& operator=(HttpRequest&& other) noexcept {
        if (this != &other) {
            method = std::move(other.method);
            path = std::move(other.path);
            version = std::move(other.version);
            headers = std::move(other.headers);
            query_params = std::move(other.query_params);
            form_data = std::move(other.form_data);
            files = std::move(other.files);
            body = std::move(other.body);
            path_params = std::move(other.path_params);
            json_body = std::move(other.json_body);
            is_processed.store(other.is_processed.load());
            request_id = std::move(other.request_id);
            pending_response_headers = std::move(other.pending_response_headers);
            pending_response_cookies = std::move(other.pending_response_cookies);
            remote_ip = std::move(other.remote_ip);
            client_socket = std::move(other.client_socket);
        }
        return *this;
    }
};

// 连接池中的连接结构体
struct PooledConnection {
    std::shared_ptr<tcp::socket> socket;
    std::chrono::steady_clock::time_point last_used;
    bool in_use = false;
};

// 前向声明
namespace boost { namespace asio { namespace ip { class tcp; } } }

// HTTP错误类型枚举
enum class HttpErrorType {
    BAD_REQUEST,
    UNAUTHORIZED,
    FORBIDDEN,
    NOT_FOUND,
    INTERNAL_ERROR,
    SERVICE_UNAVAILABLE
};

// HTTP响应结构
struct HttpResponse {
    std::string status_code = "200";   // 状态码（默认 200，避免忘记赋值时写出 "HTTP/1.1 0"）
    std::string status_message = "OK"; // 状态消息
    std::unordered_map<std::string, std::string> headers;  // 响应头（单值头）
    // 多值 Set-Cookie：headers 是 map，只能容纳一个 Set-Cookie，
    // 会导致 Session Cookie 与 CSRF Cookie 互相覆盖（以及上游多个 Set-Cookie 丢失）。
    // 这里单独保存，发送时逐条 append。
    std::vector<std::string> set_cookies;
    std::string body;          // 响应体
    std::string file_body_path; // 非空时使用 sendfile 零拷贝发送文件，body 将被忽略
    bool response_sent = false; // true 表示响应已被流式发送（代理场景），无需再次发送

    // 追加一条 Set-Cookie（推荐使用，避免覆盖）
    void addCookie(const std::string& cookie) { set_cookies.push_back(cookie); }
};

// 错误响应生成函数
HttpResponse createErrorResponse(HttpErrorType error_type, const std::string& message = "");

// 安全头添加函数 — 为响应添加现代安全头部（在发送前调用）
// use_ssl: 是否启用 SSL/TLS（控制 HSTS 等 SSL 相关头）
void addSecurityHeaders(HttpResponse& response, bool use_ssl);

// gzip 压缩工具函数
std::string gzipCompress(const std::string& data);

// 请求队列项结构体
struct RequestQueueItem {
    std::shared_ptr<HttpRequest> request;
    std::function<void(const HttpResponse&)> callback;
};

// 中间件函数类型
//
// 契约（2.6 起，破坏性变更）：
//   - 返回 std::nullopt      → 放行，继续执行下一个中间件 / 最终路由处理器
//   - 返回 HttpResponse      → 已处理该请求，中断中间件链并直接返回该响应
//                              （例如 401/403/400、缓存命中）
//
// 旧版本用「返回 404 表示放行」，与真实 404 响应无法区分，且与内置中间件
// （jwt / session / cache / validator 放行时返回 200）互相矛盾，导致一旦启用
// 这些中间件，所有请求都会被判定为"已处理"而返回空 200。故改为显式契约。
//
// 参数为 HttpRequest&：中间件需要把认证结果（如 JWT claims、Session ID）
// 写回请求供下游处理器读取，而旧的 const 引用无法做到。
// 以 `const HttpRequest&` 为参数的 lambda 仍可隐式转换，旧代码无需改写。
typedef std::function<std::optional<HttpResponse>(HttpRequest&)> Middleware;

// 错误处理中间件工厂函数
Middleware createErrorHandlingMiddleware();

// 路由处理器函数类型
typedef std::function<HttpResponse(const HttpRequest&)> RouteHandler;

// 路由匹配器基类
class RouteMatcher {
public:
    virtual ~RouteMatcher() = default;
    virtual bool match(const std::string& path, std::unordered_map<std::string, std::string>& params) const = 0;
};

// 精确路由匹配器
class ExactRouteMatcher : public RouteMatcher {
private:
    std::string path_;

public:
    ExactRouteMatcher(const std::string& path);
    bool match(const std::string& path, std::unordered_map<std::string, std::string>& params) const override;
};

// 参数化路由匹配器
class ParametricRouteMatcher : public RouteMatcher {
private:
    std::string pattern_;
    std::vector<std::string> param_names_;

public:
    ParametricRouteMatcher(const std::string& pattern);
    bool match(const std::string& path, std::unordered_map<std::string, std::string>& params) const override;
};

// 正则路由匹配器
class RegexRouteMatcher : public RouteMatcher {
private:
    std::string pattern_;
    boost::regex regex_;

public:
    RegexRouteMatcher(const std::string& pattern);
    bool match(const std::string& path, std::unordered_map<std::string, std::string>& params) const override;
};

// 路由结构
struct Route {
    std::string method;
    std::shared_ptr<RouteMatcher> matcher;
    RouteHandler handler;
    std::vector<Middleware> middlewares;
    std::string host;  // 可选：限制匹配特定 Host 头（虚拟主机）
};

// CORS 配置结构
struct CorsConfig {
    bool enabled = false;
    std::string allowed_origins = "*";
    std::string allowed_methods = "GET, POST, PUT, DELETE, OPTIONS, PATCH";
    std::string allowed_headers = "Content-Type, Authorization, X-Requested-With, X-CSRF-Token";
    bool allow_credentials = false;
    int max_age_seconds = 86400;
    std::vector<std::string> exposed_headers;
};

// HTTP服务器类
class HttpServer2 {
private:
    // 多io_context和线程池支持
    std::vector<std::shared_ptr<net::io_context>> io_contexts_;
    std::vector<std::thread> thread_pool_;
    size_t next_io_context_index_ = 0;
    std::mutex io_context_mutex_;
    
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::unique_ptr<ssl::context> ssl_context_;
    std::vector<Route> routes_;
    std::shared_mutex routes_mutex_; // 保护routes_的互斥锁，支持读写锁分离
    std::vector<Middleware> global_middlewares_;
    // running_已移至stop_mutex_下方，使用atomic<bool>类型以确保线程可见性
    bool use_ssl_ = false;
    bool debug_mode_ = false;  // 调试模式标志
    std::string cert_path_;
    std::string key_path_;
    std::string cipher_suites_;  // 密码套件配置
    std::unordered_map<std::string, std::pair<std::string, std::string>> basic_auth_users_;  // 用户名 -> {盐, 密码哈希}
    std::mutex basic_auth_mutex_;  // 保护 basic_auth_users_（可能在运行时 addUser）
    std::string basic_auth_realm_;  // 基本认证域（为空则不启用）
    bool csrf_protection_enabled_ = false;
    std::string csrf_token_header_ = "X-CSRF-Token";
    std::string csrf_token_param_ = "csrf_token";
    std::string csrf_cookie_name_ = "XSRF-TOKEN";
    std::string csrf_header_name_ = "X-XSRF-TOKEN";
    std::string csrf_param_name_ = "csrf_token";
    size_t max_request_size_ = 10 * 1024 * 1024;  // 默认10MB
    std::unordered_map<std::string, std::string> mime_types_;
    std::chrono::seconds request_timeout_ = std::chrono::seconds(30);  // 默认请求超时时间
    std::mutex stop_mutex_;  // 用于保护stop操作的互斥锁
    std::atomic<bool> running_ = false;  // 服务器运行状态（原子变量确保线程可见性）
    std::atomic<bool> stopped_{false};   // stop() 是否已完成资源回收（保证幂等）
    std::vector<std::shared_ptr<net::steady_timer>> request_timers_;  // 跟踪所有请求定时器
    std::mutex timers_mutex_;  // 保护定时器列表的互斥锁
    
    // 调试信息收集
    struct DebugInfo {
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
        std::string client_ip;
        uint16_t client_port{0};
        std::string request_method;
        std::string request_path;
        std::chrono::milliseconds routing_time{0};
        std::chrono::milliseconds middleware_time{0};
        std::chrono::milliseconds handler_time{0};
        std::string matched_route;
        bool route_found{false};
    };
    
    // 连接池相关
    std::vector<std::shared_ptr<PooledConnection>> connection_pool_;
    std::mutex pool_mutex_;
    size_t max_pool_connections_ = 100;
    std::chrono::seconds max_idle_time_ = std::chrono::seconds(60);
    
    // 限流相关
    std::atomic<size_t> request_count_ = 0;
    std::chrono::steady_clock::time_point rate_limit_window_;
    std::mutex rate_limit_mutex_;                     // 保护 rate_limit_window_（会被多线程读写）
    std::atomic<size_t> requests_per_second_{0};      // 全局 RPS 上限，0 表示不限
    std::atomic<bool> ip_rate_limit_enabled_{false};  // IP 级限流开关
    size_t max_queue_size_ = 1000;
    RateLimiter rate_limiter_;  // IP级别的速率限制器
    
    // 自适应线程池相关
    bool enable_adaptive_thread_pool_ = true;  // 是否启用自适应线程池
    size_t min_thread_pool_size_ = 2;  // 最小线程数
    size_t max_thread_pool_size_ = std::thread::hardware_concurrency() * 2;  // 最大线程数
    std::chrono::milliseconds thread_adjust_interval_ = std::chrono::milliseconds(5000);  // 线程调整间隔
    std::atomic<size_t> pending_requests_ = 0;  // 等待处理的请求数
    std::thread thread_adjust_thread_;  // 线程调整线程
    
    // 配置热重载相关
    bool enable_config_hot_reload_ = false;  // 是否启用配置热重载
    std::string config_file_path_;  // 配置文件路径
    std::chrono::steady_clock::time_point last_config_modified_;  // 上次配置文件修改时间
    std::thread config_watch_thread_;  // 配置文件监控线程
    std::chrono::milliseconds config_check_interval_ = std::chrono::milliseconds(30000);  // 配置检查间隔（30秒）
    
    // CORS 配置
    CorsConfig cors_config_;

    // 反向代理流式转发（默认关闭，见 setProxyStreaming）
    std::atomic<bool> proxy_streaming_enabled_{false};
    
    // 原生 WebSocket 处理器注册表（使用 shared_ptr 避免不完整类型问题）
    std::unordered_map<std::string, std::shared_ptr<http2::ws::WebSocketHandler>> ws_handlers_;
    
    // 请求队列
    std::queue<RequestQueueItem> request_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    // 活跃连接计数（用于 graceful shutdown）
    std::atomic<size_t> active_connections_{0};
    std::atomic<bool> shutdown_in_progress_{false};
    
    // 线程管理
    std::thread cleanup_thread_;  // 清理线程
    std::vector<std::thread> queue_threads_;  // 请求队列处理线程

    // 辅助函数
    static std::string urlDecode(const std::string& encoded);
    static std::shared_ptr<RouteMatcher> createRouteMatcher(const std::string& pattern);
    void initializeMimeTypes();
    std::string getMimeType(const std::string& file_path);

    // 连接处理函数
    void acceptConnection();
    void handleClientHttp1(tcp::socket socket);
    void handleClientHttps(ssl::stream<tcp::socket> socket);
    
    // 统一的请求处理模板（消除 HTTP/HTTPS 重复代码）
    template <typename StreamType>
    void handleClient(StreamType stream, bool use_ssl);

    // 请求处理函数
    // host 单独传入（虚拟主机匹配），不再混进 path_params —— 否则 Host 值会
    // 混入路径参数，静态文件子路径处理器按"第一个参数"取相对路径时会取到 Host。
    std::optional<Route> findRoute(const std::string& method, const std::string& host,
                                   const std::string& path, std::unordered_map<std::string, std::string>& params);
    HttpResponse handleMiddlewareChain(HttpRequest& request, const std::vector<Middleware>& middlewares,
                                       size_t index, const RouteHandler& final_handler);
    HttpResponse defaultHandler(const HttpRequest& request);

    // 解析函数
    void parseQueryParams(const std::string& query_str, std::unordered_map<std::string, std::string>& params);
    void parseFormData(const std::string& body, std::unordered_map<std::string, std::string>& form_data);
    void parseMultipartFormData(const std::string& body, const std::string& boundary, std::unordered_map<std::string, std::string>& form_data, std::vector<UploadedFile>& files);
    void parseJsonBody(const std::string& body, std::unordered_map<std::string, std::string>& json_body);

    // 请求/响应转换函数
    HttpRequest convertFromBeastRequest(const http::request<http::string_body>& request);
    template <typename Body>
    http::response<Body> convertToBeastResponse(const HttpResponse& response);

    // 请求超时处理
    void setupRequestTimeout(net::steady_timer& timer, std::shared_ptr<void> socket_ptr);
    
    // 线程池和并发优化方法
    std::shared_ptr<net::io_context> getNextIoContext();
    void runIoContext(std::shared_ptr<net::io_context> io_context);
    void cleanupExpiredConnections();
    bool isRateLimited();
    void processRequestQueue();

public:
    HttpServer2(bool use_ssl = false);
    ~HttpServer2();

    // 服务器控制
    void start(int port, size_t thread_pool_size = 0);  // 默认使用CPU核心数
    void stop();
    bool isRunning() const;
    void setRequestTimeout(std::chrono::seconds timeout);
    
    // 全局 QPS 限流（按秒窗口），0 表示不限
    // 注意：不要同时提供 setRateLimit(size_t,size_t) 与 setRateLimit(uint32_t,uint32_t) ——
    // 以 int 字面量调用时两者都是标准转换、同等级，会产生重载二义（编译错误）。
    void setGlobalRateLimit(size_t requests_per_second, size_t max_queue_size = 1000);
    
    // 连接池设置
    void setConnectionPoolSettings(size_t max_connections, std::chrono::seconds max_idle_time);

    // 路由注册
    void addRoute(const std::string& method, const std::string& pattern, const RouteHandler& handler);
    // 虚拟主机路由（匹配特定 Host 头）
    void addRoute(const std::string& method, const std::string& host, const std::string& pattern, const RouteHandler& handler);
    void get(const std::string& pattern, const RouteHandler& handler);
    void post(const std::string& pattern, const RouteHandler& handler);
    void put(const std::string& pattern, const RouteHandler& handler);
    void del(const std::string& pattern, const RouteHandler& handler);
    // 虚拟主机便捷方法
    void get(const std::string& host, const std::string& pattern, const RouteHandler& handler);
    void post(const std::string& host, const std::string& pattern, const RouteHandler& handler);

    // 中间件
    void use(const Middleware& middleware);

    // SSL设置
    void setSslCertificates(const std::string& cert_path, const std::string& key_path);
    
    // 配置SSL协议版本和密码套件
    void configureSslOptions(const std::string& min_version = "TLSv1.2", const std::string& max_version = "TLSv1.3");
    void setCipherSuites(const std::string& cipher_suites);
    
    // 获取SSL证书和密钥路径
    std::string getCertPath() const { return cert_path_; }
    std::string getKeyPath() const { return key_path_; }
    
    // 基本认证方法
    void setBasicAuth(const std::string& realm);
    void addUser(const std::string& username, const std::string& password);
    bool validateBasicAuth(const std::string& auth_header);
    std::string base64Decode(const std::string& encoded);
    std::string calculateSha256(const std::string& input);
    
    // CSRF保护
    void enableCsrfProtection(bool enable = true);
    void enableCsrfProtection(const std::string& cookie_name, const std::string& header_name);
    bool validateCsrfToken(const HttpRequest& request);
    std::string generateCsrfToken();
    void addCsrfTokenToResponse(HttpResponse& response, const HttpRequest& request);
    
    // CSRF保护设置
    void setCsrfTokenHeader(const std::string& header_name);
    void setCsrfTokenParam(const std::string& param_name);
    
    // 请求大小限制
    void setMaxRequestSize(size_t max_size_bytes);
    void checkRequestSize(const HttpRequest& request);

    // CORS 支持
    void enableCors(const CorsConfig& config);
    void setCorsOrigin(const std::string& origin);
    void setCorsCredentials(bool allow);

    // 静态文件服务
    HttpResponse serveStaticFile(const HttpRequest& request, const std::string& url_path, const std::string& directory_path, const std::string& default_file);
    void serveStatic(const std::string& url_path, const std::string& directory_path);
    void serveStatic(const std::string& url_path, const std::string& directory_path, const std::string& default_file);
    
    // 反向代理功能
    void addReverseProxy(const std::string& path, const std::string& target_url);
    void addReverseProxy(const std::string& method, const std::string& path, const std::string& target_url);
    void addReverseProxy(const std::string& method, const std::string& path, const std::string& target_url,
                        const std::unordered_map<std::string, std::string>& request_headers,
                        const std::unordered_map<std::string, std::string>& response_headers);
    
    // 支持多个后端服务的反向代理（带健康检查）
    void addReverseProxy(const std::string& path, const std::vector<std::string>& target_urls);
    void addReverseProxy(const std::string& method, const std::string& path, const std::vector<std::string>& target_urls);
    void addReverseProxy(const std::string& method, const std::string& path, const std::vector<std::string>& target_urls,
                        const std::unordered_map<std::string, std::string>& request_headers,
                        const std::unordered_map<std::string, std::string>& response_headers);

    // WebSocket 代理：将 WebSocket 连接双向转发到目标服务器
    void addWebSocketProxy(const std::string& path, const std::string& target_url);

    // 反向代理流式转发开关（默认关闭）。
    // 开启后代理会把上游响应体按 64KB 块直接写给客户端，避免整包缓冲；
    // 但该路径是同步阻塞写，且需要请求对象持有客户端 socket。
    // 默认关闭以保证正确性与并发安全，确认后端行为可预测后再开启。
    void setProxyStreaming(bool enable);
    
    // 原生 WebSocket 端点注册
    void addWebSocket(const std::string& path, const http2::ws::WebSocketHandler& handler);

    // 调试和监控
    void enableDebugMode(bool enable);
    bool isDebugMode() const;
    std::string getPerformanceReport() const;
    std::string getPrometheusMetrics() const;
    void resetPerformanceMetrics();
    
    // 速率限制
    void setRateLimit(uint32_t requests_per_window, uint32_t window_size_seconds);
    
    // 自适应线程池配置
    void enableAdaptiveThreadPool(bool enable);
    void setMinThreadPoolSize(size_t min_size);
    void setMaxThreadPoolSize(size_t max_size);
    void setThreadAdjustInterval(std::chrono::milliseconds interval);
    
    // 配置热重载
    void enableConfigHotReload(bool enable);
    void setConfigFilePath(const std::string& config_path);
    void setConfigCheckInterval(std::chrono::milliseconds interval);
    
private:
    // 自适应线程池内部方法
    void adjustThreadPoolSize();
    void startThreadAdjustment();
    
    // 配置热重载内部方法
    void startConfigWatch();
    void watchConfigFile();
    bool checkConfigFileModified();
    void reloadConfig();
    
    // 公共响应处理（消除 HTTP/HTTPS handler 重复代码）
    void finalizeResponse(HttpResponse& response, 
                          const HttpRequest& request,
                          bool use_ssl,
                          const std::shared_ptr<DebugInfo>& debug_info);
    
    static void buildAndSendResponse(tcp::socket& socket,
                                     HttpResponse& custom_response,
                                     const std::string& client_ip,
                                     const std::string& path,
                                     bool keep_alive,
                                     bool should_compress,
                                     std::function<void()> on_complete = nullptr);
    
    static void buildAndSendResponse(std::shared_ptr<ssl::stream<tcp::socket>> ssl_socket_ptr,
                                     HttpResponse& custom_response,
                                     const std::string& client_ip,
                                     const std::string& path,
                                     bool keep_alive,
                                     bool should_compress,
                                     std::function<void()> on_complete = nullptr);
    
    // sendfile 零拷贝文件发送（当 HttpResponse::file_body_path 非空时使用）
    static void sendFileResponse(tcp::socket& socket,
                                  HttpResponse& custom_response,
                                  const std::string& client_ip,
                                  const std::string& path);
    
    static void sendFileResponse(std::shared_ptr<ssl::stream<tcp::socket>> ssl_socket_ptr,
                                  HttpResponse& custom_response,
                                  const std::string& client_ip,
                                  const std::string& path);
};

#endif // HTTPSERVER2_H