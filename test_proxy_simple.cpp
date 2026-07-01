#include "httpserver2.h"
#include <iostream>
#include <thread>
#include <signal.h>

// 全局服务器指针和退出标志，用于信号处理
HttpServer2* g_server = nullptr;
bool g_should_exit = false;

// 信号处理函数
void signalHandler(int signal) {
    std::cout << "\n收到信号，关闭服务器..." << std::endl;
    g_should_exit = true;
    if (g_server) {
        g_server->stop();
    }
}

int main() {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        // 创建服务器实例，不使用SSL
        HttpServer2 server(false);
        g_server = &server;
        
        // 禁用基本认证：通过设置空的realm来实现
        std::cout << "禁用基本认证" << std::endl;
        server.setBasicAuth("");
        
        // 设置请求大小限制为5MB
        server.setMaxRequestSize(5 * 1024 * 1024);
        
        // 禁用CSRF保护
        server.enableCsrfProtection(false);
        
        // 启用调试模式
        server.enableDebugMode(true);
        
        // 禁用速率限制：将速率限制设置为一个很大的值
        server.setRateLimit(1000000, 1); // 每秒允许100万个请求
        
        // 添加反向代理路由
        // 单个后端测试路由
        std::cout << "添加单个后端代理路由: /single -> http://localhost:8001" << std::endl;
        server.addReverseProxy("GET", "/single", "http://localhost:8001");
        
        // 多个后端轮询测试路由
        std::cout << "添加多个后端轮询代理路由: /multi -> http://localhost:8001, http://localhost:8002, http://localhost:8003" << std::endl;
        std::vector<std::string> backends = {
            "http://localhost:8001",
            "http://localhost:8002",
            "http://localhost:8003"
        };
        server.addReverseProxy("GET", "/multi", backends);
        
        // 带自定义头的反向代理测试路由
        std::cout << "添加带自定义头的反向代理路由: /custom -> http://localhost:8001" << std::endl;
        std::unordered_map<std::string, std::string> request_headers = {
            {"X-Custom-Proxy", "HttpServer2"},
            {"X-Forwarded-For", "127.0.0.1"}
        };
        std::unordered_map<std::string, std::string> response_headers = {
            {"X-Powered-By", "HttpServer2/2.0"}
        };
        server.addReverseProxy("GET", "/custom", "http://localhost:8001", request_headers, response_headers);
        
        // 健康检查端点
        server.get("/health", [](const HttpRequest& request) -> HttpResponse {
            HttpResponse response;
            response.status_code = "200";
            response.status_message = "OK";
            response.headers["Content-Type"] = "application/json";
            response.body = "{\"status\": \"healthy\"}";
            return response;
        });
        
        // 启动服务器在8080端口，使用8个线程
        std::cout << "启动服务器在端口8080，使用8个线程" << std::endl;
        server.start(8080, 8);
        
        std::cout << "服务器已启动，按Ctrl+C停止" << std::endl;
        
        // 等待信号
        while (!g_should_exit) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        std::cout << "退出服务器" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "服务器错误: " << e.what() << std::endl;
        return 1;
    }
}