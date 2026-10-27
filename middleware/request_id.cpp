#include "request_id.h"
#include <iomanip>
#include <fstream>

namespace http2 { namespace middleware {

// ============================================================
// 请求 ID 中间件
// ============================================================
Middleware requestIdMiddleware() {
    return [](HttpRequest& req) -> std::optional<HttpResponse> {
        // 优先沿用客户端传入的 X-Request-ID（便于跨服务串联），否则生成新的。
        // 旧实现是空转：既不读取也不写入，完全不生成任何 ID。
        std::string id;
        auto it = req.headers.find("X-Request-ID");
        if (it != req.headers.end() && !it->second.empty()) {
            // 限制长度，避免客户端塞入超长值污染日志
            id = it->second.substr(0, 128);
        } else {
            id = generateRequestId();
        }
        
        req.request_id = id;
        // 回写请求头（下游 handler 可读）与响应头（发送前由框架合并）
        req.headers["X-Request-ID"] = id;
        req.pending_response_headers["X-Request-ID"] = id;
        
        return std::nullopt;   // 放行
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