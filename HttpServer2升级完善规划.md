# HttpServer2 升级完善规划

> 版本: 2.0 → 2.5  
> 基于对代码库 `httpserver2.h` (625行)、`httpserver2.cpp` (~3700行)、`main.cpp` (587行) 及 12 个测试文件的全面审查  
> 日期: 2026-07-15

---

## 目录

1. [现状评估](#1-现状评估)
2. [总体目标](#2-总体目标)
3. [升级路线图](#3-升级路线图)
4. [第一阶段：基础修复与加固（P0）](#4-第一阶段基础修复与加固p0)
5. [第二阶段：业务后端能力补齐（P1）](#5-第二阶段业务后端能力补齐p1)
6. [第三阶段：高级特性与性能优化（P2）](#6-第三阶段高级特性与性能优化p2)
7. [第四阶段：生态与可观测性（P3）](#7-第四阶段生态与可观测性p3)
8. [工作量估算汇总](#8-工作量估算汇总)
9. [测试策略](#9-测试策略)
10. [迁移路线图（以 netadmin 后台为例）](#10-迁移路线图以-netadmin-后台为例)

---

## 1. 现状评估

### 1.1 已有优势

| 维度 | 说明 |
|------|------|
| 路由系统 | 三种匹配模式（精确/参数/正则）、虚拟主机、方法便捷方法 |
| 安全机制 | Basic Auth、CSRF、限流、安全头、路径遍历防护 |
| 中间件链 | 全局中间件 + 路由级中间件 |
| 反向代理 | 单后端/多后端代理、WebSocket 代理转发 |
| 静态文件 | ETag、304、MIME、sendfile、Cache-Control |
| 监控 | Prometheus metrics、性能报告、调试模式 |
| 日志 | 访问日志、文件日志滚动、控制台日志 |
| 并发 | 多 io_context 线程池、自适应线程池、连接池 |
| 配置 | YAML 解析、热重载 |

### 1.2 核心短板

| 短板 | 严重度 | 说明 |
|------|--------|------|
| 无数据库集成 | 🔴 阻塞 | 无法做业务数据持久化 |
| 无 Session 管理 | 🔴 阻塞 | 无法做用户登录态保持 |
| 无 JWT/Token 认证 | 🔴 阻塞 | 无法对接现代前端认证体系 |
| 无原生 WebSocket 处理器 | 🔴 阻塞 | 无法做实时推送/聊天 |
| 无请求体验证 | 🔴 阻塞 | 每个 handler 需手写校验 |
| 无模板引擎 | 🟠 重要 | 无法做 SSR |
| HTTP/2 未实现 | 🟠 重要 | 名不副实 |
| Keep-Alive Bug | 🟠 重要 | 连接复用实际失效 |
| 大文件 OOM 风险 | 🟠 重要 | 无流式处理 |
| 无 C++ 单元测试 | 🟡 改进 | 只有 Python 集成测试 |

---

## 2. 总体目标

将 HttpServer2 从**高性能 HTTP 框架**升级为**完整的企业级 Web 服务框架**，使其能直接用于构建业务后端，而不仅仅是 API 网关/反向代理。

**目标版本**：2.5  
**核心原则**：
- 向后兼容（已有路由/中间件/配置不破坏）
- 渐进式改进（分阶段可交付）
- 不引入过多外部依赖

---

## 3. 升级路线图

```
阶段           时间           交付物                          依赖
─────────────────────────────────────────────────────────────────────
P0 基础修复    第1-2周    Keep-Alive修复、HTTP/HTTPS合并、   无
                          sendfile优化、graceful shutdown
                          
P1 业务能力    第3-6周    数据库ORM、Session、JWT、           需 P0 完成
                          WebSocket原生处理器、请求验证
                          
P2 高级特性    第7-10周   HTTP/2、流式处理、插件系统、        需 P1 完成
                          集群支持、缓存层
                          
P3 生态完善    第11-12周  单元测试体系、API文档、CI/CD、       需 P2 完成
                          结构化日志、性能优化
```

---

## 4. 第一阶段：基础修复与加固（P0）

### 4.1 修复 Keep-Alive Bug

**当前问题**：`handleClientHttp1` 中每次响应后调用 `socket->shutdown(shutdown_send)`，与 `keep_alive(true)` 冲突，导致连接复用失效。

**解决方案**：
```cpp
// 当前（有 Bug）
response->keep_alive(keep_alive);
// ... 发送响应 ...
socket->shutdown(tcp::socket::shutdown_send, close_ec);  // 强制关闭发送通道

// 修复后
response->keep_alive(keep_alive);
// ... 发送响应 ...
if (!keep_alive) {
    socket->shutdown(tcp::socket::shutdown_send, close_ec);
}
// 如果 keep_alive，保留连接供复用
```

**涉及文件**：`httpserver2.cpp` `handleClientHttp1` 函数  
**工作量**：0.5 天  
**验证**：用 wrk/ab 压测，确认连接复用率从 0% 提升到 90%+

---

### 4.2 合并 HTTP/HTTPS 处理路径

**当前问题**：`handleClientHttp1` 和 `handleClientHttps` 有约 800 行重复代码（认证、CSRF、限流、路由、日志等逻辑复制了两遍）。

**解决方案**：模板化 + 策略模式

```cpp
// 统一处理函数（模板化）
template <typename StreamType>
void handleClient(StreamType& stream, bool use_ssl) {
    // 认证、CSRF、限流、路由、日志 —— 一份代码，不再重复
    // ...
}

// 薄包装
void handleClientHttp1(tcp::socket socket) {
    handleClient(socket, false);
}

void handleClientHttps(ssl::stream<tcp::socket> socket) {
    handleClient(socket, true);
}
```

**涉及文件**：`httpserver2.cpp`，重构 `handleClientHttp1`/`handleClientHttps`  
**工作量**：2-3 天  
**验证**：所有 Python 测试用例通过，HTTP/HTTPS 行为一致

---

### 4.3 静态文件改用 sendfile + 流式发送

**当前问题**：`serveStaticFile` 将整个文件读入 `std::vector<char>` 再发送，大文件(>100MB)会 OOM。

**解决方案**：

```cpp
// 当前
std::vector<char> buffer(size);
file.read(buffer.data(), size);
response.body = std::string(buffer.data(), size);

// 改进后：利用现有的 file_body_path + sendfile
// 文件路径设置到 response.file_body_path
// buildAndSendResponse 中检测到 file_body_path 非空时调用 sendfile()
response.file_body_path = full_path;
```

`sendFileResponse` 在 `httpserver2.cpp` 中已有桩代码，需完善实现并使用 Linux `sendfile()` 系统调用。

**涉及文件**：`httpserver2.cpp` `serveStaticFile` + `sendFileResponse`  
**工作量**：1-2 天  
**验证**：测试 1GB 大文件下载，内存占用稳定在 <10MB

---

### 4.4 完善 Graceful Shutdown

**当前问题**：`stop()` 立即关闭 acceptor，但不等待活跃请求完成，可能中断处理中的请求。

**解决方案**：

```cpp
void HttpServer2::stop() {
    // 1. 停止接受新连接
    acceptor_->close();
    
    // 2. 通知所有处理中的请求，等待完成（最多等 30 秒）
    shutdown_in_progress_ = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (active_connections_ > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 3. 强制关闭剩余连接
    // 4. 停止 io_context
    for (auto& ctx : io_contexts_) {
        ctx->stop();
    }
    // 5. 等待线程退出
    for (auto& t : thread_pool_) {
        if (t.joinable()) t.join();
    }
}
```

需要添加 `std::atomic<size_t> active_connections_` 计数器，在 `handleClient` 入口 +1，出口 -1。

**涉及文件**：`httpserver2.h` + `httpserver2.cpp`  
**工作量**：1 天  
**验证**：压测中发送 SIGTERM，确认所有进行中的请求都收到完整响应

---

### 4.5 添加 CORS 框架内置支持

**当前问题**：CORS 只在 `main.cpp` 的 demo 中间件中，不是框架的内置可配置功能。

**解决方案**：

```cpp
// 新增 CORS 配置类
struct CorsConfig {
    bool enabled = false;
    std::string allowed_origins = "*";      // 或逗号分隔列表
    std::string allowed_methods = "GET, POST, PUT, DELETE, OPTIONS";
    std::string allowed_headers = "Content-Type, Authorization";
    bool allow_credentials = false;
    int max_age_seconds = 86400;
    std::vector<std::string> exposed_headers;
};

// 新增方法
void HttpServer2::enableCors(const CorsConfig& config);
void HttpServer2::setCorsOrigin(const std::string& origin);
```

**涉及文件**：`httpserver2.h` + `httpserver2.cpp`  
**工作量**：1 天  
**验证**：CORS 预检请求和跨域请求功能正常

---

### 4.6 P0 阶段小结

| 任务 | 优先级 | 工作量 | 依赖 |
|------|--------|--------|------|
| 4.1 Keep-Alive 修复 | P0 | 0.5天 | 无 |
| 4.2 HTTP/HTTPS 合并 | P0 | 2-3天 | 无 |
| 4.3 sendfile 流式发送 | P0 | 1-2天 | 无 |
| 4.4 Graceful Shutdown | P0 | 1天 | 无 |
| 4.5 CORS 内置支持 | P0 | 1天 | 无 |
| **合计** | | **5.5-7.5天** | |

---

## 5. 第二阶段：业务后端能力补齐（P1）

### 5.1 数据库集成层（ORM/DB）

**设计目标**：轻量级数据库抽象层，支持 SQLite 和 PostgreSQL，可选集成。

**接口设计**：

```cpp
// 数据库抽象层（新增 db/ 目录）
namespace http2 { namespace db {

// 连接配置
struct DBConfig {
    enum Type { SQLITE, POSTGRES };
    Type type = SQLITE;
    std::string dsn = "data.db";  // sqlite: /path/file.db, pg: postgresql://user:pass@host/db
    int max_connections = 10;
    int timeout_seconds = 5;
};

// 查询结果
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    int64_t affected_rows = 0;
    int64_t last_insert_id = 0;
    std::string error;
};

// 数据库接口
class Database {
public:
    static std::shared_ptr<Database> create(const DBConfig& config);
    virtual ~Database() = default;
    
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() = 0;
    
    virtual QueryResult execute(const std::string& sql) = 0;
    virtual QueryResult execute(const std::string& sql, 
                                 const std::vector<std::string>& params) = 0;
    
    // 事务
    virtual bool beginTransaction() = 0;
    virtual bool commit() = 0;
    virtual bool rollback() = 0;
};

// SQLite 实现
class SQLiteDatabase : public Database { /* ... */ };

// PostgreSQL 实现（使用 libpq）
class PostgresDatabase : public Database { /* ... */ };
}};
```

**集成到服务器**：

```cpp
// 在 HttpServer2 中注册
void HttpServer2::setDatabase(const std::shared_ptr<http2::db::Database>& db);

// 在路由 handler 中通过 HttpRequest 获取
// HttpRequest 新增 db() 方法
std::shared_ptr<http2::db::Database> HttpRequest::db() const;
```

**涉及文件**：新增 `db/database.h`、`db/sqlite_database.cpp`、`db/postgres_database.cpp`  
**工作量**：3-4 天  
**测试**：单元测试 CRUD、事务、并发查询

---

### 5.2 Session 管理

**设计目标**：支持内存和 Redis 两种后端，自动生成 session ID，cookie 自动管理。

**接口设计**：

```cpp
namespace http2 { namespace session {

struct SessionConfig {
    std::string cookie_name = "SESSION_ID";
    std::chrono::seconds ttl = std::chrono::hours(24);
    bool secure = false;
    bool http_only = true;
    std::string same_site = "Lax";
    std::string store_type = "memory";  // "memory" | "redis"
    std::string redis_dsn = "";
};

class Session {
public:
    std::string id() const;
    void set(const std::string& key, const std::string& value);
    std::string get(const std::string& key, const std::string& default_val = "") const;
    bool has(const std::string& key) const;
    void remove(const std::string& key);
    void clear();
    void invalidate();  // 销毁 session
};

class SessionStore {
public:
    virtual std::shared_ptr<Session> load(const std::string& id) = 0;
    virtual void save(const std::shared_ptr<Session>& session) = 0;
    virtual void destroy(const std::string& id) = 0;
    virtual void cleanup() = 0;
};

// 中间件 — 自动加载/创建 session
Middleware sessionMiddleware(std::shared_ptr<SessionStore> store, const SessionConfig& config);
}};
```

**使用方式**：

```cpp
server.use(sessionMiddleware(session_store, session_config));

server.get("/api/profile", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.session;
    std::string username = session->get("username");
    // ...
});
```

**涉及文件**：新增 `session/session.h`、`session/memory_store.cpp`、`session/redis_store.cpp`  
**工作量**：2-3 天  
**测试**：session 创建/读取/销毁、过期清理、Redis 持久化

---

### 5.3 JWT 认证支持

**设计目标**：内置 JWT 签发和验证，自动 Bearer Token 解析，支持 HS256/RS256。

**接口设计**：

```cpp
namespace http2 { namespace auth {

struct JwtConfig {
    std::string secret = "change-me";  // HS256 密钥
    std::string public_key = "";       // RS256 公钥
    std::string issuer = "httpserver2";
    std::chrono::seconds ttl = std::chrono::hours(2);
    std::string algorithm = "HS256";   // HS256 | RS256
};

struct JwtClaims {
    std::string subject;
    std::string issuer;
    std::vector<std::string> roles;
    std::map<std::string, std::string> custom;
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point expires_at;
};

class Jwt {
public:
    static std::string sign(const JwtClaims& claims, const JwtConfig& config);
    static std::optional<JwtClaims> verify(const std::string& token, const JwtConfig& config);
};

// 中间件 — 验证 Bearer Token
Middleware jwtAuthMiddleware(const JwtConfig& config);
}};
```

**使用方式**：

```cpp
// 登录接口
server.post("/api/auth/login", [](const HttpRequest& req) -> HttpResponse {
    // 验证用户名密码
    JwtClaims claims;
    claims.subject = username;
    claims.roles = {"admin"};
    std::string token = Jwt::sign(claims, jwt_config);
    // 返回 token
});

// 需要认证的路由
server.use(jwtAuthMiddleware(jwt_config));  // 全局中间件
// 或单路由
server.get("/api/profile", jwtAuthMiddleware(jwt_config), handler);
```

**涉及文件**：新增 `auth/jwt.h`、`auth/jwt.cpp`  
**工作量**：1-2 天  
**测试**：token 签发/验证、过期、签名伪造、RS256 密钥对

---

### 5.4 原生 WebSocket 处理器

**当前问题**：只有 `addWebSocketProxy`（转发到另一台服务器），不能直接在框架内注册 WebSocket 端点处理消息。

**设计方案**：新增 WebSocket 会话管理和消息路由

```cpp
namespace http2 { namespace ws {

enum class OpCode {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA
};

struct WebSocketMessage {
    OpCode opcode;
    std::vector<char> data;
    bool fin;  // 是否是最后一帧
};

class WebSocketSession {
public:
    std::string id() const;
    void send(const std::string& text);
    void send(const std::vector<char>& binary);
    void send(const WebSocketMessage& msg);
    void close(uint16_t code = 1000, const std::string& reason = "");
    bool isOpen() const;
    
    // 连接信息
    std::string remoteIp() const;
    std::unordered_map<std::string, std::string> headers() const;
    std::unordered_map<std::string, std::string> queryParams() const;
};

// WebSocket 事件处理器
struct WebSocketHandler {
    std::function<void(std::shared_ptr<WebSocketSession>)> on_open;
    std::function<void(std::shared_ptr<WebSocketSession>, const WebSocketMessage&)> on_message;
    std::function<void(std::shared_ptr<WebSocketSession>, uint16_t, const std::string&)> on_close;
    std::function<void(std::shared_ptr<WebSocketSession>, const std::string&)> on_error;
};

// 注册方法
void HttpServer2::addWebSocket(const std::string& path, const WebSocketHandler& handler);
}};
```

**使用方式**：

```cpp
server.addWebSocket("/ws/chat", {
    .on_open = [](auto session) {
        LOG_INFO("New WS connection: " + session->remoteIp());
        session->send("{\"type\":\"welcome\"}");
    },
    .on_message = [](auto session, auto msg) {
        std::string text(msg.data.begin(), msg.data.end());
        // 广播给所有连接
        broadcast(text);
    },
    .on_close = [](auto session, auto code, auto reason) {
        LOG_INFO("WS closed: " + std::to_string(code));
    }
});
```

**涉及文件**：新增 `ws/websocket.h`、`ws/websocket.cpp`，修改 `httpserver2.h` + `httpserver2.cpp`  
**工作量**：3-4 天  
**测试**：文本/二进制帧发送接收、分片、Ping/Pong、关闭握手、并发连接

---

### 5.5 请求体参数验证

**设计目标**：声明式请求体验证，减少 handler 中的样板代码。

**接口设计**：

```cpp
namespace http2 { namespace validation {

struct Rule {
    std::string field;
    enum Type { REQUIRED, STRING, INT, FLOAT, BOOL, EMAIL, IP, URL, MIN, MAX, LENGTH, PATTERN, ENUM, CUSTOM };
    Type type;
    std::string param;   // 如 MIN 的数值、PATTERN 的正则、ENUM 的选项
    std::string message;  // 自定义错误消息
};

struct ValidationResult {
    bool valid = true;
    std::vector<std::string> errors;
};

// 验证函数
ValidationResult validate(const HttpRequest& request, const std::vector<Rule>& rules);

// 验证中间件工厂
Middleware validateMiddleware(const std::vector<Rule>& rules);
}};
```

**使用方式**：

```cpp
server.post("/api/users", validateMiddleware({
    {"username", Rule::REQUIRED, "", "用户名必填"},
    {"username", Rule::LENGTH, "3-32", "用户名长度3-32"},
    {"email", Rule::EMAIL, "", "邮箱格式不正确"},
    {"age", Rule::INT, "0-150", "年龄范围0-150"},
    {"role", Rule::ENUM, "admin,user,guest", "角色必须是 admin/user/guest"},
}), [](const HttpRequest& req) -> HttpResponse {
    // 参数已验证，直接使用
    std::string username = req.form_data["username"];
    // ...
});
```

**涉及文件**：新增 `validation/validator.h`、`validation/validator.cpp`  
**工作量**：1-2 天  
**测试**：每种规则类型的单元测试、错误消息、嵌套验证

---

### 5.6 P1 阶段小结

| 任务 | 优先级 | 工作量 | 依赖 |
|------|--------|--------|------|
| 5.1 数据库集成层 | P1 | 3-4天 | 无 |
| 5.2 Session 管理 | P1 | 2-3天 | 5.1（Redis 可选） |
| 5.3 JWT 认证 | P1 | 1-2天 | 无 |
| 5.4 原生 WebSocket | P1 | 3-4天 | 4.2 |
| 5.5 请求验证 | P1 | 1-2天 | 无 |
| **合计** | | **10-15天** | |

---

## 6. 第三阶段：高级特性与性能优化（P2）

### 6.1 HTTP/2 支持（引入 nghttp2）

**方案**：集成 `nghttp2` 库实现 HTTP/2 协议。

```cpp
// 新增 HTTP/2 处理路径
void handleClientHttp2(tcp::socket socket);
void handleClientH2C(ssl::stream<tcp::socket> socket);

// ALPN 协商（在 SSL 握手时）
// 优先 HTTP/2，回退 HTTP/1.1
```

**实现要点**：
- 多路复用（一个 TCP 连接上多个流）
- 头部压缩（HPACK）
- 服务器推送（Server Push）
- 流优先级

**工作量**：1-2 周  
**依赖**：`nghttp2` 库，`openssl` ALPN 支持

---

### 6.2 请求体流式处理

**当前问题**：大请求体先完整读入内存再处理，`max_request_size_` 默认 10MB，超出即拒绝。

**设计方案**：分段读取 + 分段转发

```cpp
// 代理场景：边读边转发
void handleProxyStreaming(...) {
    const size_t CHUNK_SIZE = 65536;
    std::vector<char> buffer(CHUNK_SIZE);
    while (true) {
        size_t bytes_read = stream.read_some(asio::buffer(buffer));
        if (bytes_read == 0) break;
        // 转发到上游
        upstream.write(asio::buffer(buffer.data(), bytes_read));
    }
}

// 大文件上传：写入临时文件，提供 stream API
class UploadedStream {
public:
    std::string saveToFile(const std::string& dir);
    std::string saveToTemp();
    size_t totalSize() const;
    std::string mimeType() const;
};
```

**工作量**：2-3 天  
**测试**：100MB 文件上传验证内存占用 < 20MB

---

### 6.3 缓存层

**设计目标**：内存缓存 + Redis 后端，支持 API 响应缓存和数据库查询缓存。

```cpp
namespace http2 { namespace cache {

struct CacheConfig {
    std::string type = "memory";  // "memory" | "redis"
    size_t max_size_mb = 256;
    std::chrono::seconds default_ttl = std::chrono::minutes(5);
    std::string redis_dsn = "";
};

class Cache {
public:
    static std::shared_ptr<Cache> create(const CacheConfig& config);
    virtual void set(const std::string& key, const std::string& value, 
                     std::chrono::seconds ttl) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual void del(const std::string& key) = 0;
    virtual bool exists(const std::string& key) = 0;
    virtual void clear() = 0;
};

// 缓存中间件
Middleware cacheMiddleware(std::shared_ptr<Cache> cache, 
                           std::chrono::seconds ttl = std::chrono::minutes(5));
}};
```

**工作量**：2-3 天

---

### 6.4 插件系统

**设计目标**：支持动态加载 so 插件，扩展框架功能。

```cpp
namespace http2 { namespace plugin {

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    virtual bool initialize(HttpServer2* server) = 0;
    virtual void shutdown() = 0;
};

// 插件管理器
class PluginManager {
public:
    bool load(const std::string& so_path);
    bool loadDir(const std::string& dir);
    void initializeAll(HttpServer2* server);
    void shutdownAll();
    
private:
    std::vector<void*> handles_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
};
}};
```

**工作量**：3-4 天

---

### 6.5 P2 阶段小结

| 任务 | 优先级 | 工作量 | 依赖 |
|------|--------|--------|------|
| 6.1 HTTP/2 支持 | P2 | 5-10天 | 无 |
| 6.2 流式处理 | P2 | 2-3天 | 无 |
| 6.3 缓存层 | P2 | 2-3天 | 无 |
| 6.4 插件系统 | P2 | 3-4天 | 无 |
| **合计** | | **12-20天** | |

---

## 7. 第四阶段：生态与可观测性（P3）

### 7.1 C++ 核心单元测试

**当前问题**：所有测试是 Python 集成测试，C++ 核心组件无单元测试。

**引入 Google Test**：

```cmake
# CMakeLists.txt 新增
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# 测试目标
add_executable(test_unit_core
    test/test_route_matcher.cpp
    test/test_http_parser.cpp
    test/test_jwt.cpp
    test/test_session.cpp
    test/test_validator.cpp
)
target_link_libraries(test_unit_core PRIVATE gtest gtest_main)
```

**测试覆盖**：
- 路由匹配器（精确/参数/正则）
- 参数解析（query/form/JSON/multipart）
- JWT 签发/验证
- Session 存储
- 请求验证器
- 数据库 CRUD

**工作量**：2-3 天

---

### 7.2 OpenAPI/Swagger 文档自动生成

**设计目标**：从路由注册信息自动生成 OpenAPI 3.0 规范文档。

```cpp
// 路由注册时附带文档信息
server.get("/api/users", handler)
    .describe("获取用户列表")
    .param("page", "integer", "页码", true)
    .param("size", "integer", "每页数量", true)
    .response("200", "用户列表", UserListSchema);

// 文档端点
server.get("/api/docs/openapi.json", [](const HttpRequest& req) -> HttpResponse {
    // 自动生成 OpenAPI JSON
});
```

**工作量**：2-3 天

---

### 7.3 结构化日志（JSON 格式）

**接口设计**：

```cpp
struct LogEntry {
    std::string timestamp;      // ISO 8601
    std::string level;          // "info" | "warn" | "error"
    std::string logger;         // 模块名
    std::string message;
    std::string request_id;     // 关联请求
    std::string client_ip;
    std::string method;
    std::string path;
    int status_code = 0;
    int64_t response_time_ms = 0;
    std::map<std::string, std::string> extra;  // 自定义字段
};

// 日志输出 JSON 格式到文件
// 访问日志也输出 JSON 格式（每行一个 JSON 对象）
```

**工作量**：1 天

---

### 7.4 请求 ID 追踪

**设计目标**：每个请求自动分配唯一 ID，支持从 `X-Request-ID` 头读取或自动生成，透传至日志和上游请求。

```cpp
// 中间件
Middleware requestIdMiddleware();

// 在日志中自动包含 request_id
// 在反向代理中透传 X-Request-ID
```

**工作量**：0.5 天

---

### 7.5 P3 阶段小结

| 任务 | 优先级 | 工作量 | 依赖 |
|------|--------|--------|------|
| 7.1 C++ 单元测试 | P3 | 2-3天 | 无 |
| 7.2 OpenAPI 文档 | P3 | 2-3天 | 无 |
| 7.3 结构化日志 | P3 | 1天 | 无 |
| 7.4 请求 ID 追踪 | P3 | 0.5天 | 无 |
| **合计** | | **5.5-7.5天** | |

---

## 8. 工作量估算汇总

| 阶段 | 任务数 | 工作量 | 交付物 |
|------|--------|--------|--------|
| **P0 基础修复** | 5 | 5.5-7.5天 | 稳定可靠的 HTTP 核心 |
| **P1 业务能力** | 5 | 10-15天 | 完整业务后端框架 |
| **P2 高级特性** | 4 | 12-20天 | 企业级特性 |
| **P3 生态完善** | 4 | 5.5-7.5天 | 开发体验提升 |
| **总计** | **18** | **33-50天** | **HttpServer2 v2.5** |

---

## 9. 测试策略

### 9.1 测试层次

```
层次              工具                    覆盖范围
──────────────────────────────────────────────────────────
单元测试          Google Test             路由匹配器、参数解析器、
                                          JWT、Session、验证器、DB
                                          
集成测试          Python 测试套件          已有 12 个 test_*.py 文件
                  (已有框架)              增补 WS 消息、DB CRUD、JWT
                                          
性能测试          wrk / ab / bench.py      吞吐量、延迟、连接复用率
                                          
压力测试          test_stress.cpp          高并发、长连接、内存泄漏
```

### 9.2 新增测试文件

| 文件 | 测试内容 |
|------|---------|
| `test/test_route_matcher.cpp` | 精确/参数/正则路由匹配 |
| `test/test_http_parser.cpp` | Query/Form/JSON/Multipart 解析 |
| `test/test_jwt.cpp` | 签发/验证/过期/伪造 |
| `test/test_session.cpp` | 内存 Store CRUD/过期 |
| `test/test_validator.cpp` | 各规则类型验证 |
| `test/test_database.cpp` | SQLite CRUD/事务 |
| `test/test_websocket_msg.cpp` | 帧编码/解码/分片 |
| `test/test_cors.cpp` | CORS 配置/预检/跨域 |

---

## 10. 迁移路线图（以 netadmin 后台为例）

如果要用 HttpServer2 替换当前 Go 后台，推荐的迁移路径：

### 10.1 当前 Go 后台功能清单

```
netadmin/backend (Go + Gin)            →  HttpServer2 替代方案
──────────────────────────────────────────────────────────────────
Gin 框架                                →  HttpServer2 核心（已有）
路由注册                                →  addRoute / get / post（已有）
CORS 中间件                             →  P0.5 内置 CORS
JWT 认证                                →  P1.3 JWT 模块
SQLite 存储（audit_logs/users/metrics） →  P1.1 数据库层
WebSocket 日志推送                      →  P1.4 原生 WS 处理器
WebSocket 监控推送                      →  P1.4 原生 WS 处理器
IPC 通信（Unix socket）                 →  P1 后社区扩展或自实现
Config 文件管理（JSON 配置文件）         →  已有路由 handler 即可实现
规则管理（JSON 文件 CRUD）              →  P1.1 数据库层 + 路由
脚本管理（Lua 文件 CRUD）               →  P1.1 数据库层 + 路由
进程管理（sudo start/stop）             →  路由 handler 直接实现
系统监控（/proc 读取）                   →  路由 handler 直接实现
CSV 导出                               →  路由 handler 直接实现
日志同步协程（5秒轮询）                 →  P1.4 定时器或 WS 推送
```

### 10.2 迁移建议顺序

```
第1步：P0 完成 → 搭建 HttpServer2 基础框架（2周）
第2步：P1.1 数据库层 → 数据迁移到 SQLite（1周）
第3步：P1.3 JWT 认证 → 替代原有认证（1周）
第4步：P1.4 WS 处理器 → 实现实时推送（1周）
第5步：逐一实现业务路由 handler（2-3周）
第6步：P0/P1 未覆盖的已有功能补充（1周）

总计：约 8-10 周
```

---

## 附录：文件变更清单

### 新增文件

```
db/database.h              — 数据库抽象接口
db/sqlite_database.cpp     — SQLite 实现
db/postgres_database.cpp   — PostgreSQL 实现
session/session.h          — Session 接口
session/memory_store.cpp   — 内存 Session 存储
session/redis_store.cpp    — Redis Session 存储
auth/jwt.h                 — JWT 签发/验证
auth/jwt.cpp               — JWT 实现
ws/websocket.h             — WebSocket 会话/帧
ws/websocket.cpp           — WebSocket 实现
validation/validator.h      — 请求验证器
validation/validator.cpp    — 验证器实现
cache/cache.h              — 缓存抽象
cache/memory_cache.cpp     — 内存缓存实现
cache/redis_cache.cpp      — Redis 缓存实现
plugin/plugin.h            — 插件接口
plugin/plugin_manager.cpp  — 插件管理器
test/                      — 单元测试目录
```

### 修改文件

```
httpserver2.h              — 新增公共方法声明
httpserver2.cpp            — 新增功能实现 + Keep-Alive 修复 + HTTP/HTTPS 合并
CMakeLists.txt             — 新增源文件、Google Test、nghttp2
config.yaml                — 新增 CORS/DB/Session/JWT 配置段
```

---

> **文档版本**：1.0  
> **更新日期**：2026-07-15  
> **作者**：HttpServer2 开发团队