#ifndef HTTPSERVER2_CACHE_H
#define HTTPSERVER2_CACHE_H

#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <functional>
#include "../httpserver2.h"

namespace http2 { namespace cache {

// 缓存配置
struct CacheConfig {
    std::string type = "memory";       // "memory" | "redis"
    size_t max_size_mb = 256;          // 最大内存（MB）
    std::chrono::seconds default_ttl = std::chrono::minutes(5);
    std::string redis_dsn = "";        // Redis 连接串
};

// 缓存项
struct CacheEntry {
    std::string data;
    std::chrono::steady_clock::time_point expires_at;
    size_t size = 0;
};

// 缓存抽象接口
class Cache {
public:
    static std::shared_ptr<Cache> create(const CacheConfig& config);
    
    virtual ~Cache() = default;
    
    virtual void set(const std::string& key, const std::string& value, 
                     std::chrono::seconds ttl) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual void del(const std::string& key) = 0;
    virtual bool exists(const std::string& key) = 0;
    virtual void clear() = 0;
    virtual size_t size() const = 0;
};

// 内存缓存实现
class MemoryCache : public Cache {
private:
    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::shared_mutex mutex_;
    size_t max_size_bytes_;
    size_t current_size_ = 0;

    bool isExpired(const CacheEntry& entry) const;
    void evictIfNeeded(size_t needed_size);
    
public:
    MemoryCache(size_t max_size_mb = 256);
    ~MemoryCache() override = default;
    
    void set(const std::string& key, const std::string& value, 
             std::chrono::seconds ttl) override;
    std::optional<std::string> get(const std::string& key) override;
    void del(const std::string& key) override;
    bool exists(const std::string& key) override;
    void clear() override;
    size_t size() const override;
};

// 缓存中间件工厂
// ttl: 缓存有效期，key_prefix: 缓存键前缀（用于区分路由）
Middleware cacheMiddleware(std::shared_ptr<Cache> cache, 
                           std::chrono::seconds ttl = std::chrono::minutes(5));

}} // namespace http2::cache

#endif // HTTPSERVER2_CACHE_H