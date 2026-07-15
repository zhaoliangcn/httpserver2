#include "cache.h"
#include <shared_mutex>

namespace http2 { namespace cache {

// ============================================================
// Cache 工厂方法
// ============================================================
std::shared_ptr<Cache> Cache::create(const CacheConfig& config) {
    if (config.type == "memory") {
        return std::make_shared<MemoryCache>(config.max_size_mb);
    }
    // Redis 暂未实现
    LOG_WARNING("Redis cache not yet implemented, falling back to MemoryCache");
    return std::make_shared<MemoryCache>(config.max_size_mb);
}

// ============================================================
// MemoryCache 实现
// ============================================================
MemoryCache::MemoryCache(size_t max_size_mb)
    : max_size_bytes_(max_size_mb * 1024 * 1024) {
    LOG_INFO("MemoryCache initialized: max " + std::to_string(max_size_mb) + " MB");
}

bool MemoryCache::isExpired(const CacheEntry& entry) const {
    return std::chrono::steady_clock::now() >= entry.expires_at;
}

void MemoryCache::evictIfNeeded(size_t needed_size) {
    // 如果当前大小 + 需要大小 > 最大大小，清空过期项
    if (current_size_ + needed_size > max_size_bytes_) {
        // 先清理过期项
        auto now = std::chrono::steady_clock::now();
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (now >= it->second.expires_at) {
                current_size_ -= it->second.size;
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
        
        // 如果仍然不够，删除最旧的项
        while (current_size_ + needed_size > max_size_bytes_ && !cache_.empty()) {
            auto oldest = cache_.begin();
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->second.expires_at < oldest->second.expires_at) {
                    oldest = it;
                }
            }
            current_size_ -= oldest->second.size;
            cache_.erase(oldest);
        }
    }
}

void MemoryCache::set(const std::string& key, const std::string& value, 
                       std::chrono::seconds ttl) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    size_t entry_size = key.size() + value.size() + sizeof(CacheEntry);
    evictIfNeeded(entry_size);
    
    // 如果已存在，先减去旧大小
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        current_size_ -= it->second.size;
    }
    
    CacheEntry entry;
    entry.data = value;
    entry.expires_at = std::chrono::steady_clock::now() + ttl;
    entry.size = entry_size;
    
    cache_[key] = std::move(entry);
    current_size_ += entry_size;
}

std::optional<std::string> MemoryCache::get(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return std::nullopt;
    }
    
    if (isExpired(it->second)) {
        // 过期项需要删除（但这里只有读锁，需要升级或标记）
        // 延迟删除：在 set/del 时清理
        return std::nullopt;
    }
    
    return it->second.data;
}

void MemoryCache::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        current_size_ -= it->second.size;
        cache_.erase(it);
    }
}

bool MemoryCache::exists(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    if (isExpired(it->second)) return false;
    return true;
}

void MemoryCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    cache_.clear();
    current_size_ = 0;
}

size_t MemoryCache::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return cache_.size();
}

// ============================================================
// 缓存中间件
// ============================================================
Middleware cacheMiddleware(std::shared_ptr<Cache> cache, 
                           std::chrono::seconds ttl) {
    return [cache, ttl](const HttpRequest& req) -> HttpResponse {
        // 只缓存 GET 请求
        if (req.method != "GET") {
            HttpResponse resp;
            resp.status_code = "200";
            resp.status_message = "OK";
            return resp;
        }
        
        // 生成缓存键
        std::string cache_key = req.path;
        // 按参数排序确保一致性
        std::map<std::string, std::string> sorted_params(req.query_params.begin(), 
                                                          req.query_params.end());
        for (const auto& [k, v] : sorted_params) {
            cache_key += "&" + k + "=" + v;
        }
        
        // 尝试从缓存获取
        auto cached = cache->get(cache_key);
        if (cached) {
            HttpResponse resp;
            resp.status_code = "200";
            resp.status_message = "OK (cached)";
            resp.body = *cached;
            resp.headers["Content-Type"] = "application/json";
            resp.headers["X-Cache"] = "HIT";
            return resp;
        }
        
        // 未命中 — 返回空响应让中间件链继续
        // 下游 handler 处理完请求后，需要通过响应头 X-Cache-Store 告知缓存
        HttpResponse resp;
        resp.status_code = "200";
        resp.status_message = "OK";
        resp.headers["X-Cache"] = "MISS";
        resp.headers["X-Cache-Key"] = cache_key;
        return resp;
    };
}

}} // namespace http2::cache