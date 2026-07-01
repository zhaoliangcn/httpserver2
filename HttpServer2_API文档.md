# HttpServer2 接口文档

## 1. 项目简介

HttpServer2 是一个基于 Boost.Beast 和 Boost.Asio 开发的高性能 HTTP/HTTPS 服务器，支持 HTTP/1.1 和 HTTP/2.0 协议。该服务器提供了丰富的路由功能、中间件支持、静态文件服务和 SSL 加密支持，适用于构建各种 Web 应用和 API 服务。

### 1.1 主要特性

- 支持 HTTP/1.1 和 HTTP/2.0 协议
- 支持 HTTPS（SSL/TLS 加密）
- 灵活的路由系统（精确路由、参数化路由、正则表达式路由）
- 中间件机制
- 静态文件服务
- 支持查询参数、表单数据、JSON 和文件上传的解析
- 完善的日志系统
- 可配置的请求超时机制

## 2. 核心数据结构

### 2.1 HttpRequest

HTTP 请求数据结构，包含请求的所有信息。

```cpp
struct HttpRequest {
    std::string method;                     // HTTP方法（GET, POST, PUT, DELETE等）
    std::string path;                       // 请求路径
    std::string version;                    // HTTP版本
    std::unordered_map<std::string, std::string> headers;  // 请求头
    std::unordered_map<std::string, std::string> query_params;  // 查询参数
    std::unordered_map<std::string, std::string> form_data;  // 表单数据
    std::vector<UploadedFile> files;        // 上传的文件
    std::string body;                       // 请求体
    std::unordered_map<std::string, std::string> path_params;  // 路径参数
    std::unordered_map<std::string, std::string> json_body;  // JSON请求体（解析后）
};
```

### 2.2 HttpResponse

HTTP 响应数据结构，用于构建服务器响应。

```cpp
struct HttpResponse {
    std::string status_code;                // 状态码（如 "200", "404", "500" 等）
    std::string status_message;             // 状态消息（如 "OK", "Not Found" 等）
    std::unordered_map<std::string, std::string> headers;  // 响应头
    std::string body;                       // 响应体
};
```

### 2.3 UploadedFile

文件上传信息结构，用于存储通过表单上传的文件信息。

```cpp
struct UploadedFile {
    std::string name;          // 表单字段名
    std::string filename;      // 上传的文件名
    std::string content_type;  // 文件MIME类型
    std::vector<char> data;    // 文件内容
};
```

### 2.4 函数类型定义

```cpp
// 中间件函数类型
typedef std::function<HttpResponse(const HttpRequest&)> Middleware;

// 路由处理器函数类型
typedef std::function<HttpResponse(const HttpRequest&)> RouteHandler;
```

## 3. HttpServer2 类

### 3.1 构造函数和析构函数

```cpp
/**
 * 创建HTTP服务器实例
 * @param use_ssl 是否启用SSL加密（HTTPS）
 */
HttpServer2(bool use_ssl = false);

/**
 * 服务器析构函数
 * 负责清理资源和停止服务器
 */
~HttpServer2();
```

### 3.2 服务器控制方法

```cpp
/**
 * 启动HTTP服务器
 * @param port 服务器监听端口
 */
void start(int port);

/**
 * 停止HTTP服务器
 * 安全地关闭所有连接并停止服务器
 */
void stop();

/**
 * 检查服务器是否正在运行
 * @return 服务器运行状态
 */
bool isRunning() const;

/**
 * 设置请求超时时间
 * @param timeout 超时时间（秒）
 */
void setRequestTimeout(std::chrono::seconds timeout);
```

### 3.3 SSL相关方法

```cpp
/**
 * 设置SSL证书和密钥
 * @param cert_path SSL证书路径
 * @param key_path SSL私钥路径
 * @throws std::runtime_error 如果SSL未启用或证书设置失败
 */
void setSslCertificates(const std::string& cert_path, const std::string& key_path);
```

### 3.4 路由注册方法

```cpp
/**
 * 添加通用路由
 * @param method HTTP方法（GET, POST, PUT, DELETE等）
 * @param pattern 路由匹配模式
 * @param handler 路由处理函数
 */
void addRoute(const std::string& method, const std::string& pattern, const RouteHandler& handler);

/**
 * 添加GET路由
 * @param pattern 路由匹配模式
 * @param handler 路由处理函数
 */
void get(const std::string& pattern, const RouteHandler& handler);

/**
 * 添加POST路由
 * @param pattern 路由匹配模式
 * @param handler 路由处理函数
 */
void post(const std::string& pattern, const RouteHandler& handler);

/**
 * 添加PUT路由
 * @param pattern 路由匹配模式
 * @param handler 路由处理函数
 */
void put(const std::string& pattern, const RouteHandler& handler);

/**
 * 添加DELETE路由
 * @param pattern 路由匹配模式
 * @param handler 路由处理函数
 */
void del(const std::string& pattern, const RouteHandler& handler);
```

### 3.5 中间件方法

```cpp
/**
 * 添加全局中间件
 * 中间件按照添加顺序执行，且对所有路由生效
 * @param middleware 中间件函数
 */
void use(const Middleware& middleware);
```

### 3.6 静态文件服务方法

```cpp
/**
 * 提供静态文件服务
 * @param url_path URL路径前缀
 * @param directory_path 文件系统目录路径
 * @param default_file 默认文件（如index.html）
 */
void serveStatic(const std::string& url_path, const std::string& directory_path, const std::string& default_file);

/**
 * 提供静态文件服务（使用默认文件index.html）
 * @param url_path URL路径前缀
 * @param directory_path 文件系统目录路径
 */
void serveStatic(const std::string& url_path, const std::string& directory_path);

/**
 * 服务单个静态文件
 * @param url_path URL请求的文件路径
 * @param directory_path 基础目录路径
 * @param default_file 默认文件
 * @return HTTP响应对象
 */
HttpResponse serveStaticFile(const std::string& url_path, const std::string& directory_path, const std::string& default_file);
```

## 4. 路由匹配模式

HttpServer2 支持三种路由匹配模式：

### 4.1 精确路由

完全匹配路径的路由模式：

```cpp
server.get("/api/users", handler);  // 精确匹配 /api/users
```

### 4.2 参数化路由

使用大括号定义路径参数的路由模式：

```cpp
server.get("/api/users/{id}", [](const HttpRequest& req) {
    std::string user_id = req.path_params["id"];
    // 处理请求...
});
```

### 4.3 正则表达式路由

使用正则表达式匹配路径的路由模式：

```cpp
server.get("/api/articles/([0-9]+)", [](const HttpRequest& req) {
    std::string article_id = req.path_params["1"];  // 正则匹配组索引作为键
    // 处理请求...
});
```

## 5. 中间件使用

中间件是一个接收 HttpRequest 并返回 HttpResponse 的函数。中间件可以用于日志记录、认证授权、CORS 支持等。

### 5.1 日志中间件示例

```cpp
server.use([](const HttpRequest& request) -> HttpResponse {
    std::cout << "[LOG] " << request.method << " " << request.path << std::endl;
    if (!request.query_params.empty()) {
        std::cout << "[LOG] Query params:" << std::endl;
        for (const auto& [key, value] : request.query_params) {
            std::cout << "  " << key << " = " << value << std::endl;
        }
    }
    // 返回404让后续处理器继续处理
    HttpResponse response;
    response.status_code = "404";
    return response;
});
```

### 5.2 CORS 中间件示例

```cpp
server.use([](const HttpRequest& request) -> HttpResponse {
    HttpResponse response;
    if (request.method == "OPTIONS") {
        response.status_code = "204";
        response.status_message = "No Content";
    } else {
        response.status_code = "404";
    }
    response.headers["Access-Control-Allow-Origin"] = "*";
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
    response.headers["Access-Control-Allow-Headers"] = "Content-Type";
    return response;
});
```

## 6. 完整使用示例

### 6.1 创建基本HTTP服务器

```cpp
#include "httpserver2.h"
#include <iostream>

int main() {
    // 创建HTTP服务器实例
    HttpServer2 server(false);  // 不使用SSL
    
    // 添加路由
    server.get("/", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"message\": \"Hello, World!\"}";
        return response;
    });
    
    // 启动服务器
    server.start(8080);
    
    return 0;
}
```

### 6.2 创建HTTPS服务器

```cpp
#include "httpserver2.h"
#include <iostream>

int main() {
    // 创建HTTPS服务器实例
    HttpServer2 server(true);  // 使用SSL
    
    // 设置SSL证书和密钥
    try {
        server.setSslCertificates("./server.crt", "./server.key");
    } catch (const std::exception& e) {
        std::cerr << "SSL证书设置失败: " << e.what() << std::endl;
        return 1;
    }
    
    // 添加路由
    server.get("/", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "text/html";
        response.body = "<h1>Hello, HTTPS!</h1>";
        return response;
    });
    
    // 启动服务器
    server.start(443);
    
    return 0;
}
```

### 6.3 处理各种请求类型

```cpp
// 处理JSON请求
server.post("/api/data", [](const HttpRequest& request) -> HttpResponse {
    HttpResponse response;
    response.status_code = "200";
    response.headers["Content-Type"] = "application/json";
    
    // 访问解析后的JSON数据
    std::string name = request.form_data.count("name") ? request.form_data["name"] : "Unknown";
    
    response.body = "{\"status\": \"success\", \"received_name\": \"" + name + "\"}";
    return response;
});

// 处理表单提交
server.post("/api/form", [](const HttpRequest& request) -> HttpResponse {
    HttpResponse response;
    response.status_code = "200";
    response.headers["Content-Type"] = "application/json";
    
    std::string username = request.form_data.count("username") ? request.form_data["username"] : "";
    
    response.body = "{\"status\": \"success\", \"username\": \"" + username + "\"}";
    return response;
});

// 处理文件上传
server.post("/api/upload", [](const HttpRequest& request) -> HttpResponse {
    HttpResponse response;
    response.status_code = "200";
    response.headers["Content-Type"] = "application/json";
    
    if (!request.files.empty()) {
        const auto& file = request.files[0];
        response.body = "{\"status\": \"success\", \"filename\": \"" + file.filename + "\", \"size\": " + std::to_string(file.data.size()) + "}";
    } else {
        response.body = "{\"status\": \"error\", \"message\": \"No files uploaded\"}";
    }
    
    return response;
});
```

## 7. 静态文件服务

### 7.1 基本静态文件服务配置

```cpp
// 将/static路径映射到./static目录，默认文件为index.html
server.serveStatic("/static", "./static", "index.html");

// 或使用简化版本（默认文件为index.html）
server.serveStatic("/static", "./static");
```

### 7.2 静态文件访问示例

配置后，可以通过以下方式访问静态文件：

- `http://localhost:8080/static/index.html` - 访问静态目录下的index.html
- `http://localhost:8080/static/styles.css` - 访问静态目录下的styles.css
- `http://localhost:8080/static/` - 自动访问static目录下的默认文件（index.html）

## 8. 注意事项

1. **线程安全性**：HttpServer2 内部使用互斥锁保护关键操作，但处理器函数可能在不同线程中执行，请注意线程安全。

2. **资源管理**：服务器会自动管理连接资源，但处理大文件上传时应注意内存使用。

3. **SSL配置**：使用SSL时，请确保证书和密钥文件路径正确且具有适当的访问权限。

4. **中间件执行顺序**：中间件按照添加顺序执行，返回非404响应的中间件会中断后续中间件链。

5. **路由匹配优先级**：精确路由匹配优先于参数化路由，参数化路由优先于正则表达式路由。

6. **文件路径安全**：静态文件服务实现了路径遍历保护，不允许访问基础目录外的文件。

## 9. 错误处理

HttpServer2 提供了默认的404错误处理，但您也可以添加自定义错误处理：

```cpp
// 添加通用错误处理中间件
server.use([](const HttpRequest& request) -> HttpResponse {
    // 这里可以实现自定义的错误处理逻辑
    // 例如日志记录、友好的错误页面等
    HttpResponse response;
    response.status_code = "404"; // 让其他处理器继续处理
    return response;
});
```

---

**文档版本**：1.0.0  
**更新日期**：2025-10-26  
**作者**：HttpServer2 开发团队