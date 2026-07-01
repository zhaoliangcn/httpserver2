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
        
        // 禁用基本认证
        std::cout << "禁用基本认证" << std::endl;
        server.setBasicAuth("");
        
        // 设置请求大小限制为5MB
        server.setMaxRequestSize(5 * 1024 * 1024);
        
        // 禁用CSRF保护
        server.enableCsrfProtection(false);
        
        // 启用调试模式
        server.enableDebugMode(true);
        
        // 禁用速率限制
        server.setRateLimit(static_cast<uint32_t>(1000000), static_cast<uint32_t>(1)); // 每秒允许100万个请求
        
        // 添加静态文件服务路由
        std::cout << "添加静态文件服务路由: /static -> ./static" << std::endl;
        server.serveStatic("/static", "./static");
        
        // 添加健康检查端点
        server.get("/health", [](const HttpRequest& request) -> HttpResponse {
            HttpResponse response;
            response.status_code = "200";
            response.status_message = "OK";
            response.headers["Content-Type"] = "application/json";
            response.body = "{\"status\": \"healthy\"}";
            return response;
        });
        
        // 启动服务器在端口8080，使用8个线程
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