// test_proxy.cpp - 测试反向代理功能
//
#include "httpserver2.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <signal.h>

// 全局服务器指针和退出标志
HttpServer2* g_server = nullptr;
bool g_should_exit = false;

// 信号处理函数
void signalHandler(int signal) {
    std::cout << "\n收到信号: " << (signal == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM") << std::endl;
    g_should_exit = true;
    
    if (g_server && g_server->isRunning()) {
        std::cout << "正在停止服务器..." << std::endl;
        g_server->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        // 创建服务器实例（不使用SSL）
        HttpServer2 server(false);
        g_server = &server;
        
        // 配置服务器
        server.setMaxRequestSize(5 * 1024 * 1024); // 5MB
        server.enableDebugMode(true);
        
        // 禁用基本认证
        server.setBasicAuth(""); // 空字符串表示禁用基本认证
        
        // 启用日志
        log_init();
        log_add_console_sink();
        log_set_level(LOG_LEVEL_INFO);
        
        // 添加基本中间件
        server.use([](const HttpRequest& request) -> std::optional<HttpResponse> {
            std::cout << "[LOG] " << request.method << " " << request.path << std::endl;
            return std::nullopt;   // 放行
        });
        
        // 添加CORS中间件
        server.use([](const HttpRequest& request) -> std::optional<HttpResponse> {
            if (request.method != "OPTIONS") {
                return std::nullopt;   // 非预检请求：放行
            }
            HttpResponse response;
            response.status_code = "204";
            response.headers["Access-Control-Allow-Origin"] = "*";
            response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
            response.headers["Access-Control-Allow-Headers"] = "Content-Type";
            return response;
        });
        
        // 测试1: 单个后端反向代理
        std::cout << "配置单个后端反向代理: /single -> http://example.com" << std::endl;
        server.addReverseProxy("GET", "/single", "http://example.com");
        
        // 测试2: 多个后端反向代理（轮询）
        std::cout << "配置多个后端反向代理: /multi -> http://example.com, http://httpbin.org, http://example.org" << std::endl;
        std::vector<std::string> backends = {
            "http://example.com",
            "http://httpbin.org",
            "http://example.org"
        };
        server.addReverseProxy("GET", "/multi", backends);
        
        // 测试3: 带自定义头的反向代理
        std::cout << "配置带自定义头的反向代理: /custom -> http://example.com" << std::endl;
        std::unordered_map<std::string, std::string> request_headers = {
            {"X-Proxy", "HttpServer2"},
            {"X-Forwarded-For", "127.0.0.1"}
        };
        std::unordered_map<std::string, std::string> response_headers = {
            {"X-Powered-By", "HttpServer2/2.0"}
        };
        server.addReverseProxy("GET", "/custom", "http://example.com", request_headers, response_headers);
        
        // 启动服务器
        uint16_t port = 8080;
        std::cout << "启动反向代理测试服务器，监听端口 " << port << "..." << std::endl;
        std::cout << "测试端点:" << std::endl;
        std::cout << "  - 单个后端: http://localhost:8080/single" << std::endl;
        std::cout << "  - 多个后端（轮询）: http://localhost:8080/multi" << std::endl;
        std::cout << "  - 自定义头: http://localhost:8080/custom" << std::endl;
        std::cout << "按 Ctrl+C 停止服务器" << std::endl;
        
        // 启动服务器线程
        std::thread server_thread([&server, port]() {
            server.start(port, 4);
        });
        
        // 主循环
        while (!g_should_exit) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // 停止服务器
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
