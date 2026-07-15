#include "session.h"
#include <random>
#include <sstream>
#include <chrono>

namespace http2 { namespace session {

// ============================================================
// Session ID 生成
// ============================================================
std::string MemorySessionStore::generateSessionId() {
    static const std::string charset = 
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, static_cast<int>(charset.size()) - 1);
    
    std::string id;
    id.reserve(64);
    for (int i = 0; i < 64; ++i) {
        id += charset[dist(gen)];
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
    return [store, config](const HttpRequest& req) -> HttpResponse {
        HttpResponse resp;
        resp.status_code = "200";
        resp.status_message = "OK";
        
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
        
        bool is_new = false;
        if (!session) {
            session_id = MemorySessionStore::generateSessionId();
            session = std::make_shared<Session>(session_id, config.ttl);
            is_new = true;
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
        resp.headers["Set-Cookie"] = cookie;
        
        // 通过响应头传递 Session 信息给下游处理器
        resp.headers["X-Session-Id"] = session->id();
        
        // 保存 Session
        store->save(session);
        
        // 注意：这里不能直接修改 HttpRequest（const 引用）
        // 实际使用中，Session 中间件应全局使用，且 Session ID 通过 Cookie 传递
        // Handler 中通过 req.headers["Cookie"] 获取 Session ID 再查询
        // 这里简化处理：将 Session ID 添加到响应头，供后续处理器使用
        
        return resp;
    };
}

}} // namespace http2::session