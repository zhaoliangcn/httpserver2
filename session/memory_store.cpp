#include "session.h"
#include <random>
#include <sstream>
#include <chrono>
#include <stdexcept>
#include <openssl/rand.h>

namespace http2 { namespace session {

// ============================================================
// Session ID 生成
// ============================================================
std::string MemorySessionStore::generateSessionId() {
    // 32 字节 CSPRNG → 64 个十六进制字符。
    // 旧实现用 std::mt19937：输出可由少量样本推导，会话 ID 可被预测（会话劫持）。
    unsigned char buf[32];
    if (RAND_bytes(buf, static_cast<int>(sizeof(buf))) != 1) {
        throw std::runtime_error("generateSessionId: RAND_bytes failed");
    }
    static const char* hex = "0123456789abcdef";
    std::string id;
    id.reserve(sizeof(buf) * 2);
    for (unsigned char c : buf) {
        id += hex[c >> 4];
        id += hex[c & 0x0f];
    }
    return id;
}

// ============================================================
// 内存 Session 存储实现
// ============================================================
std::shared_ptr<Session> MemorySessionStore::load(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        if (it->second->isExpired()) {
            sessions_.erase(it);
            return nullptr;
        }
        it->second->touch();
        return it->second;
    }
    return nullptr;
}

void MemorySessionStore::save(const std::shared_ptr<Session>& session) {
    if (!session || session->isInvalidated()) {
        if (session) destroy(session->id());
        return;
    }
    if (!session->isDirty()) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session->id()] = session;
    session->markClean();
}

void MemorySessionStore::destroy(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

void MemorySessionStore::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second->isExpired()) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================
// Session 中间件
// ============================================================
Middleware sessionMiddleware(std::shared_ptr<SessionStore> store, const SessionConfig& config) {
    return [store, config](HttpRequest& req) -> std::optional<HttpResponse> {
        // 从 Cookie 中获取 Session ID
        std::string session_id;
        auto cookie_it = req.headers.find("Cookie");
        if (cookie_it != req.headers.end()) {
            std::string cookies = cookie_it->second;
            size_t pos = cookies.find(config.cookie_name + "=");
            if (pos != std::string::npos) {
                pos += config.cookie_name.size() + 1;
                size_t end = cookies.find(';', pos);
                session_id = cookies.substr(pos, end - pos);
            }
        }
        
        // 加载或创建 Session
        std::shared_ptr<Session> session;
        if (!session_id.empty()) {
            session = store->load(session_id);
        }
        
        if (!session) {
            session_id = MemorySessionStore::generateSessionId();
            session = std::make_shared<Session>(session_id, config.ttl);
        }
        session->touch();
        
        // 设置 Session Cookie
        std::string cookie = config.cookie_name + "=" + session->id() + 
                           "; Path=/; Max-Age=" + std::to_string(config.ttl.count());
        if (config.http_only) cookie += "; HttpOnly";
        if (config.secure) cookie += "; Secure";
        if (!config.same_site.empty()) {
            cookie += "; SameSite=" + config.same_site;
        }
        // 不能直接写进"响应"：本中间件在 handler 之前执行，最终响应还不存在。
        // 旧实现返回一个 200 响应，既中断了中间件链（请求再也到不了 handler），
        // 又让 handler 自己的 Set-Cookie 被这个空响应覆盖。
        req.pending_response_cookies.push_back(cookie);
        
        // 下游 handler 通过 req.headers["X-Session-Id"] 取当前会话
        req.headers["X-Session-Id"] = session->id();
        
        // 保存 Session（新建的 Session 构造时即为 dirty，确保能落库）
        store->save(session);
        
        // nullopt = 放行
        return std::nullopt;
    };
}

}} // namespace http2::session