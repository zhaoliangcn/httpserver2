#ifndef HTTPSERVER2_SESSION_H
#define HTTPSERVER2_SESSION_H

#include <string>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <mutex>
#include <functional>
#include "../httpserver2.h"

namespace http2 { namespace session {

// Session 配置
struct SessionConfig {
    std::string cookie_name = "SESSION_ID";
    std::chrono::seconds ttl = std::chrono::hours(24);
    bool secure = false;
    bool http_only = true;
    std::string same_site = "Lax";
    std::string store_type = "memory";  // "memory" | "redis"
    std::string redis_dsn = "";
};

// Session 类
class Session {
private:
    std::string id_;
    std::unordered_map<std::string, std::string> data_;
    std::chrono::steady_clock::time_point last_access_;
    std::chrono::seconds ttl_;
    bool dirty_ = false;
    bool invalidated_ = false;

public:
    Session(const std::string& id, std::chrono::seconds ttl)
        : id_(id), last_access_(std::chrono::steady_clock::now()), ttl_(ttl) {}

    const std::string& id() const { return id_; }
    bool isExpired() const {
        return std::chrono::steady_clock::now() - last_access_ > ttl_;
    }
    bool isInvalidated() const { return invalidated_; }
    bool isDirty() const { return dirty_; }
    void markClean() { dirty_ = false; }
    void touch() { last_access_ = std::chrono::steady_clock::now(); }

    void set(const std::string& key, const std::string& value) {
        data_[key] = value;
        dirty_ = true;
        touch();
    }

    std::string get(const std::string& key, const std::string& default_val = "") const {
        auto it = data_.find(key);
        return it != data_.end() ? it->second : default_val;
    }

    bool has(const std::string& key) const {
        return data_.count(key) > 0;
    }

    void remove(const std::string& key) {
        data_.erase(key);
        dirty_ = true;
    }

    void clear() {
        data_.clear();
        dirty_ = true;
    }

    void invalidate() {
        invalidated_ = true;
        dirty_ = true;
    }

    std::unordered_map<std::string, std::string> all() const {
        return data_;
    }
};

// Session 存储接口
class SessionStore {
public:
    virtual ~SessionStore() = default;
    virtual std::shared_ptr<Session> load(const std::string& id) = 0;
    virtual void save(const std::shared_ptr<Session>& session) = 0;
    virtual void destroy(const std::string& id) = 0;
    virtual void cleanup() = 0;
};

// 内存 Session 存储
class MemorySessionStore : public SessionStore {
private:
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::mutex mutex_;
    std::chrono::seconds ttl_;

public:
    MemorySessionStore(std::chrono::seconds ttl = std::chrono::hours(24))
        : ttl_(ttl) {}

    std::shared_ptr<Session> load(const std::string& id) override;
    void save(const std::shared_ptr<Session>& session) override;
    void destroy(const std::string& id) override;
    void cleanup() override;

    // 生成新 Session ID
    static std::string generateSessionId();
};

// Session 中间件工厂
Middleware sessionMiddleware(std::shared_ptr<SessionStore> store, const SessionConfig& config);

}} // namespace http2::session

#endif // HTTPSERVER2_SESSION_H