#include "request_id.h"
#include <iomanip>
#include <fstream>

namespace http2 { namespace middleware {

// ============================================================
// 请求 ID 中间件
// ============================================================
Middleware requestIdMiddleware() {
    return [](const HttpRequest& req) -> HttpResponse {
        HttpResponse resp;
        resp.status_code = "404";  // 继续处理，让后续中间件和路由处理器处理
        return resp;
    };
}

// ============================================================
// 结构化日志
// ============================================================
std::string LogEntry::toJson() const {
    std::stringstream ss;
    ss << "{";
    ss << "\"timestamp\":\"" << timestamp << "\",";
    ss << "\"level\":\"" << level << "\",";
    ss << "\"logger\":\"" << logger << "\",";
    ss << "\"message\":\"" << message << "\"";
    
    if (!request_id.empty())
        ss << ",\"request_id\":\"" << request_id << "\"";
    if (!client_ip.empty())
        ss << ",\"client_ip\":\"" << client_ip << "\"";
    if (!method.empty())
        ss << ",\"method\":\"" << method << "\"";
    if (!path.empty())
        ss << ",\"path\":\"" << path << "\"";
    if (status_code > 0)
        ss << ",\"status_code\":" << status_code;
    if (response_time_ms > 0)
        ss << ",\"response_time_ms\":" << response_time_ms;
    
    for (const auto& [key, value] : extra) {
        ss << ",\"" << key << "\":\"" << value << "\"";
    }
    
    ss << "}";
    return ss.str();
}

void logStructured(const LogEntry& entry) {
    std::string json = entry.toJson();
    // 写入标准日志
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, "%s", json.c_str());
}

}} // namespace http2::middleware