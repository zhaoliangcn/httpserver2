# HttpServer2

> **版本 2.6** — 高性能 C++17 HTTP Web 服务框架

基于 Boost.Beast 和 Boost.Asio，提供完整的 HTTP/HTTPS 服务、反向代理、WebSocket、JWT 认证、数据库集成、Session 管理、缓存、插件系统等功能。

> **2.6 破坏性变更**：中间件契约由「返回 404 表示放行」改为「返回 `std::nullopt` 表示放行，
> 返回 `HttpResponse` 表示已处理」。原因：旧约定与真实 404 无法区分，且与 JWT / Session /
> Cache / Validator 四个内置中间件（放行时返回 200）直接冲突 —— 启用后所有请求都会返回空 200。
> 迁移方式见 `HttpServer2_API文档.md` 第 5 章。

---

## 功能特性

### 核心 HTTP 引擎
| 特性 | 说明 |
|------|------|
| HTTP/1.1 | 完整的请求/响应处理，支持 Keep-Alive 连接复用 |
| HTTPS | SSL/TLS 加密通信，支持证书配置 |
| HTTP/2 | ⚠️ 实验：`http2/` 模块已实现但**未接入** acceptor（无 ALPN 协商），主链路只跑 HTTP/1.1 |
| 双后端 | 支持 Boost.Asio 和 hp-async-io 两种后端 |
| 虚拟主机 | 基于 Host 头的虚拟主机路由 |
| 请求超时 | 可配置的请求超时控制 |

### 路由系统
| 特性 | 说明 |
|------|------|
| 精确匹配 | `GET /api/users` |
| 参数匹配 | `GET /api/users/:id` |
| 正则匹配 | `GET /api/users/{[0-9]+}` |
| 虚拟主机路由 | 按 Host 头分发 |
| 方法便捷方法 | `get()` / `post()` / `put()` / `del()` |

### 中间件链
- 全局中间件（路由级中间件字段已预留，暂无注册 API）
- 内置中间件：错误处理、安全头、CORS、CSRF、JWT 认证、Session、请求验证、缓存、请求 ID 追踪
- 契约：`std::nullopt` = 放行，`HttpResponse` = 已处理；中间件抛异常按 500 处理（失败关闭）

### 安全机制
| 特性 | 说明 |
|------|------|
| Basic Auth | HTTP 基本认证 |
| JWT 认证 | HS256 签发/验证，Bearer Token 提取，中间件 |
| CSRF 保护 | 自动生成令牌，双提交 Cookie 模式 |
| CORS | 可配置的跨域支持（origin/methods/headers/credentials） |
| 安全头 | CSP、HSTS、X-Frame-Options、X-Content-Type-Options 等 |
| 限流 | 全局限流 + IP 级别速率限制 |
| 路径遍历防护 | 静态文件服务的路径安全检查 |

### 反向代理
- 单后端 / 多后端反向代理
- 健康检查（自动故障转移）
- 自定义请求/响应头
- 逐跳头过滤（Connection / Transfer-Encoding 等不原样透传）
- WebSocket 代理转发
- 流式转发默认关闭（`setProxyStreaming(true)` 开启；该路径为同步阻塞写）

### 静态文件服务
- ETag / 304 Not Modified 缓存验证
- MIME 类型自动检测
- Range 请求（断点续传）
- sendfile 零拷贝文件发送
- Cache-Control 头

### 业务后端能力

| 模块 | 说明 |
|------|------|
| **数据库** | SQLite 集成 + TinyDB 适配器，参数化查询，事务支持 |
| **Session** | 内存 Session 存储，自动 Cookie 管理，过期清理 |
| **JWT** | HS256 签名/验证，自定义声明，中间件 |
| **WebSocket** | 原生 WebSocket 端点，会话管理，广播，消息帧处理 |
| **请求验证** | 12 种规则：REQUIRED、EMAIL、INT、ENUM、PATTERN、LENGTH 等 |

### 高级特性

| 模块 | 说明 |
|------|------|
| **缓存层** | 内存缓存，TTL 过期，LRU 淘汰；缓存中间件目前只能命中、不能填充（缺 handler 后置钩子） |
| **流式处理** | 大文件上传流式写入临时文件，分块响应 |
| **插件系统** | 动态 `.so` 加载，内置插件注册 |
| **HTTP/2** | ⚠️ 代码已实现但未接入主流程，见「核心 HTTP 引擎」说明 |

### 可观测性

| 特性 | 说明 |
|------|------|
| **Prometheus 指标** | `/metrics` 端点，请求量/延迟/错误率 |
| **性能监控** | 实时请求统计，路由级性能分析 |
| **调试模式** | 请求级调试头（X-Debug-Time、X-Debug-Route-Time） |
| **访问日志** | JSON 格式结构化日志，请求 ID 关联 |
| **请求 ID 追踪** | 自动生成 X-Request-ID，链路追踪 |
| **OpenAPI 文档** | 自动生成 OpenAPI 3.0 JSON + HTML 文档 |

### 可配置性
- YAML 配置文件
- ⚠️ 配置热重载：仅检测文件变更并写日志，`reloadConfig()` 为空实现
- ⚠️ 自适应线程池：只扩容不缩容，且 `pending_requests_` 未被计数，实际不会触发扩容
- 请求大小限制
- ⚠️ 连接池：`connection_pool_` 未接入请求流程，`setConnectionPoolSettings()` 仅保存参数
- 全局限流（`setGlobalRateLimit`）与 per-IP 窗口限流（`setRateLimit`）相互独立

### 安全配置（环境变量）

| 环境变量 | 用途 | 说明 |
|----------|------|------|
| `HTTP_AUTH_USERNAME` | 基本认证用户名 | 与 `HTTP_AUTH_PASSWORD` 同时设置才启用 |
| `HTTP_AUTH_PASSWORD` | 基本认证密码 | 建议使用强密码 |
| `JWT_SECRET` | JWT HS256 签名密钥 | 未设置时 `sign()` 会拒绝签发 Token |

> 所有凭据**必须**通过环境变量注入，禁止在代码或配置文件中硬编码。

---

## 快速开始

### 构建

```bash
# 安装依赖
# Ubuntu/Debian:
sudo apt-get install libboost-all-dev libssl-dev libsqlite3-dev libnghttp2-dev

# 克隆并构建
git clone <repo-url> && cd httpserver2
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行

```bash
# 设置安全配置（建议通过环境变量，避免硬编码凭据）
export HTTP_AUTH_USERNAME=admin
export HTTP_AUTH_PASSWORD=your_secure_password
export JWT_SECRET=your_strong_random_secret_here

# 默认 HTTP 模式（端口 8080）
./httpserver2

# HTTPS 模式（需要 server.crt 和 server.key）
./httpserver2 --ssl

# 指定端口和线程数
./httpserver2 3000 --threads 8
```

### 最小示例

```cpp
#include "httpserver2.h"

int main() {
    HttpServer2 server;
    
    server.get("/", [](const HttpRequest& req) -> HttpResponse {
        HttpResponse resp;
        resp.status_code = "200";
        resp.body = "Hello, World!";
        resp.headers["Content-Type"] = "text/plain";
        return resp;
    });
    
    server.get("/api/hello/:name", [](const HttpRequest& req) -> HttpResponse {
        HttpResponse resp;
        resp.status_code = "200";
        resp.body = "Hello, " + req.path_params.at("name") + "!";
        resp.headers["Content-Type"] = "text/plain";
        return resp;
    });
    
    server.start(8080);
    return 0;
}
```

---

## 模块使用示例

### JWT 认证

```cpp
#include "auth/jwt.h"

// 配置 — 密钥从环境变量读取，避免硬编码
http2::auth::JwtConfig config;
config.secret = std::getenv("JWT_SECRET") ? std::getenv("JWT_SECRET") : "";
config.issuer = "myapp";

// 登录接口 — 签发 Token
server.post("/api/auth/login", [](const HttpRequest& req) -> HttpResponse {
    http2::auth::JwtClaims claims;
    claims.subject = "user123";
    claims.roles = {"admin"};
    std::string token = http2::auth::Jwt::sign(claims, config);
    HttpResponse resp;
    resp.status_code = "200";
    resp.body = R"({"token":")" + token + R"("})";
    resp.headers["Content-Type"] = "application/json";
    return resp;
});

// 需要认证的路由
server.use(http2::auth::jwtAuthMiddleware(config));
```

> **安全提示**：JWT 密钥必须通过 `JWT_SECRET` 环境变量设置，HS256 模式下密钥为空时 `sign()` 会返回空字符串并记录错误日志。

### 数据库操作

```cpp
#include "db/database.h"

// SQLite
http2::db::DBConfig config;
config.dsn = "app.db";
auto db = http2::db::Database::create(config);
db->connect();
db->execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)");
db->execute("INSERT INTO users (name) VALUES (?)", {"Alice"});
auto result = db->execute("SELECT * FROM users");
for (const auto& row : result.rows) {
    std::cout << row[0] << ": " << row[1] << std::endl;
}

// TinyDB
config.dsn = "host=127.0.0.1 port=5432";
auto tinydb = http2::db::Database::create(config);
tinydb->connect();
```

### 请求验证

```cpp
#include "validation/validator.h"

server.post("/api/users", http2::validation::validateMiddleware({
    {"username", http2::validation::RuleType::REQUIRED, "", "用户名必填"},
    {"username", http2::validation::RuleType::LENGTH, "3-32", "长度 3-32"},
    {"email", http2::validation::RuleType::EMAIL, "", "邮箱格式错误"},
    {"age", http2::validation::RuleType::INT, "0-150", "年龄范围"},
}), [](const HttpRequest& req) -> HttpResponse {
    // 已验证通过，直接使用
    return HttpResponse{};
});
```

### WebSocket

```cpp
#include "ws/websocket.h"

server.addWebSocket("/ws/chat", {
    .on_open = [](auto session) {
        LOG_INFO("New WS connection: " + session->remoteIp());
        session->send("{\"type\":\"welcome\"}");
    },
    .on_message = [](auto session, auto msg) {
        std::string text = msg.textData();
        // 广播给所有连接
        http2::ws::g_ws_manager.broadcast(text);
    },
    .on_close = [](auto session, uint16_t code, const std::string& reason) {
        LOG_INFO("WS closed: " + std::to_string(code));
    }
});
```

### Session

```cpp
#include "session/session.h"

auto store = std::make_shared<http2::session::MemorySessionStore>(std::chrono::hours(24));
http2::session::SessionConfig sess_config;
sess_config.cookie_name = "MY_SESSION";
server.use(http2::session::sessionMiddleware(store, sess_config));
```

### 缓存

```cpp
#include "cache/cache.h"

auto cache = std::make_shared<http2::cache::MemoryCache>(256);
server.use(http2::cache::cacheMiddleware(cache, std::chrono::minutes(10)));
```

### OpenAPI 文档

```cpp
#include "docs/openapi.h"

auto doc = std::make_shared<http2::docs::OpenApiDoc>();
doc->setTitle("My API");
doc->setVersion("1.0.0");
http2::docs::registerApiDocs(&server, doc, "/api/docs");
// 访问 GET /api/docs → HTML 文档
// 访问 GET /api/docs/openapi.json → OpenAPI 3.0 JSON
```

### 静态文件

```cpp
server.serveStatic("/static", "/var/www/static");
server.serveStatic("/", "/var/www/public", "index.html");
```

### 反向代理

```cpp
// 单后端
server.addReverseProxy("/api", "http://backend:3000");

// 多后端（负载均衡 + 健康检查）
server.addReverseProxy("/api", {"http://backend1:3000", "http://backend2:3000"});
```

### CORS 配置

```cpp
CorsConfig cors;
cors.enabled = true;
cors.allowed_origins = "https://myapp.example.com";
cors.allow_credentials = true;
server.enableCors(cors);
```

---

## 模块目录结构

```
├── auth/              JWT 认证
├── validation/        请求验证器
├── session/           Session 管理
├── db/                数据库抽象层（SQLite + TinyDB）
├── ws/                原生 WebSocket
├── cache/             缓存层
├── stream/            流式处理
├── plugin/            插件系统
├── http2/             HTTP/2 支持
├── middleware/        请求 ID 追踪
├── docs/              OpenAPI 文档
├── public/            静态文件示例
├── single_include/    单头文件日志库
└── build/             构建输出
```

## 依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| Boost | ≥1.86 | Asio、Beast、Regex |
| OpenSSL | ≥3.0 | SSL/TLS、加密 |
| yaml-cpp | - | 配置文件解析 |
| SQLite3 | ≥3.31 | 嵌入式数据库 |
| nghttp2 | ≥1.40 | HTTP/2 协议 |
| TinyDB | - | 可选数据库后端 |

## 测试

```bash
# C++ 单元测试（226 用例）
./test_unit

# Python 集成测试
python3 run_tests.py

# 构建并运行所有测试
cmake --build . --target test-auto
```

## 性能

- 基于 Boost.Asio 多 io_context 线程池
- 自适应线程池（根据负载动态调整）
- sendfile 零拷贝文件发送
- 连接池复用
- 请求批处理（减少锁竞争）

---

**许可证**: MIT
**版本**: 2.6
**更新日期**: 2026-10-27