#include "httpserver2.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>

// 简单的HTTP后端服务器（用于测试）
class SimpleBackendServer {
private:
    int port_;
    std::string message_;
    std::thread server_thread_;
    bool running_ = false;
    
public:
    SimpleBackendServer(int port, const std::string& message) 
        : port_(port), message_(message) {}
    
    void start() {
        running_ = true;
        server_thread_ = std::thread([this]() {
            // 简单的HTTP服务器实现
            try {
                boost::asio::io_context ioc;
                boost::asio::ip::tcp::acceptor acceptor(ioc, 
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port_));
                
                std::cout << "Backend server started on port " << port_ << std::endl;
                
                while (running_) {
                    boost::asio::ip::tcp::socket socket(ioc);
                    acceptor.accept(socket);
                    
                    // 简单的HTTP响应
                    std::string response = 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: " + std::to_string(message_.length()) + "\r\n"
                        "Connection: close\r\n"
                        "\r\n" + message_;
                    
                    boost::asio::write(socket, boost::asio::buffer(response));
                    
                    // 短暂关闭连接
                    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
                    socket.close();
                }
            } catch (const std::exception& e) {
                std::cerr << "Backend server error: " << e.what() << std::endl;
            }
        });
    }
    
    void stop() {
        running_ = false;
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
    ~SimpleBackendServer() {
        stop();
    }
};

int main() {
    std::cout << "=== HttpServer2 Reverse Proxy Test ===" << std::endl;
    
    // 启动后端服务器
    SimpleBackendServer backend1(8001, "Backend 1 Response");
    SimpleBackendServer backend2(8002, "Backend 2 Response");
    SimpleBackendServer backend3(8003, "Backend 3 Response");
    
    backend1.start();
    backend2.start();
    backend3.start();
    
    // 等待后端服务器启动
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    try {
        // 创建HttpServer2实例
        HttpServer2 server;
        
        // 禁用认证以简化测试
        // server.enableBasicAuth(false);
        
        // 配置单个后端反向代理
        server.addReverseProxy("/single", "http://localhost:8001");
        
        // 配置多个后端反向代理（轮询负载均衡）
        std::vector<std::string> backends = {
            "http://localhost:8001",
            "http://localhost:8002", 
            "http://localhost:8003"
        };
        server.addReverseProxy("/multi", backends);
        
        // 启动代理服务器
        server.start(8080);
        
        std::cout << "Reverse proxy server started on port 8080" << std::endl;
        std::cout << "Test URLs:" << std::endl;
        std::cout << "- Single backend: http://localhost:8080/single" << std::endl;
        std::cout << "- Multiple backends (round-robin): http://localhost:8080/multi" << std::endl;
        std::cout << "Press Ctrl+C to stop the server..." << std::endl;
        
        // 保持服务器运行
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}