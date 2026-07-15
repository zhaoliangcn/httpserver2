#ifndef HTTPSERVER2_REQUEST_ID_H
#define HTTPSERVER2_REQUEST_ID_H

#include <string>
#include <random>
#include <sstream>
#include "../httpserver2.h"

namespace http2 { namespace middleware {

// 生成唯一请求 ID
inline std::string generateRequestId() {
    static std::atomic<uint64_t> counter{0};
    uint64_t seq = counter++;
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::stringstream ss;
    ss << std::hex << now_us << "-" << std::hex << seq;
    return ss.str();
}

// 请求 ID 中间件 — 自动生成/读取 X-Request-ID
// 优先级：客户端提供的 X-Request-ID > 自动生成
Middleware requestIdMiddleware();

// 结构化日志工具
struct LogEntry {
    std::string timestamp;
    std::string level;
    std::string logger;
    std::string message;
    std::string request_id;
    std::string client_ip;
    std::string method;
    std::string path;
    int status_code = 0;
    int64_t response_time_ms = 0;
    std::map<std::string, std::string> extra;
    
    // 序列化为 JSON 字符串
    std::string toJson() const;
};

// 结构化日志写入
void logStructured(const LogEntry& entry);

}} // namespace http2::middleware

#endif // HTTPSERVER2_REQUEST_ID_H