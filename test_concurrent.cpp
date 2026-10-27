#include "httpserver2.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <fstream>
#include <signal.h>

// 全局服务器指针、线程和退出标志，用于信号处理
HttpServer2* g_server = nullptr;
std::thread g_server_thread;
bool g_should_exit = false;

// 信号处理函数
void signalHandler(int signal) {
    std::cout << "\n收到信号: " << (signal == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM") << "，开始信号处理..." << std::endl;
    
    // 设置退出标志
    g_should_exit = true;
    
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "正在关闭服务器..." << std::endl;
        
        if (g_server) {
            std::cout << "服务器指针存在，检查运行状态..." << std::endl;
            if (g_server->isRunning()) {
                std::cout << "服务器正在运行，调用stop()方法..." << std::endl;
                g_server->stop();
                std::cout << "服务器stop()方法已调用" << std::endl;
            } else {
                std::cout << "服务器未在运行状态" << std::endl;
            }
        } else {
            std::cout << "警告: 服务器指针为空" << std::endl;
        }
        
        // 等待服务器线程结束
        std::cout << "检查服务器线程是否可join..." << std::endl;
        if (g_server_thread.joinable()) {
            std::cout << "线程可join，正在等待线程完成..." << std::endl;
            g_server_thread.join();
            std::cout << "服务器线程已成功join完成" << std::endl;
        } else {
            std::cout << "服务器线程不可join" << std::endl;
        }
        
        std::cout << "信号处理完成，服务器已关闭。" << std::endl;
        // 添加exit(0)确保程序能够完全退出
        std::cout << "调用exit(0)确保程序完全退出" << std::endl;
        exit(0);
    }
}

void showHelp(const std::string& programName) {
    std::cout << "用法: " << programName << " [选项]" << std::endl;
    std::cout << "选项:" << std::endl;
    std::cout << "  --help, -h     显示此帮助信息" << std::endl;
    std::cout << "  --port, -p     指定服务器端口号 (默认: 8080)" << std::endl;
    std::cout << "  --threads, -t  指定工作线程数量 (默认: 8)" << std::endl;
    std::cout << "  --ssl, -s      启用SSL支持" << std::endl;
    std::cout << "  --no-ssl       禁用SSL支持 (默认)" << std::endl;
    std::cout << "  --cert, -c     指定SSL证书路径 (默认: ./server.crt)" << std::endl;
    std::cout << "  --key, -k      指定SSL密钥路径 (默认: ./server.key)" << std::endl;
    std::cout << "  --debug        启用调试模式 (默认: 已启用)" << std::endl;
    std::cout << "  --no-debug     禁用调试模式" << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  " << programName << "                  默认配置（端口8080，8个线程，调试模式启用，无SSL）" << std::endl;
    std::cout << "  " << programName << " --port 9000        在9000端口启动服务器" << std::endl;
    std::cout << "  " << programName << " --threads 4        使用4个工作线程" << std::endl;
    std::cout << "  " << programName << " --ssl              启用SSL支持（使用默认证书）" << std::endl;
    std::cout << "  " << programName << " --ssl --cert my.crt --key my.key  指定证书和密钥路径" << std::endl;
    std::cout << "  " << programName << " --no-debug         禁用调试模式运行" << std::endl;
}

int main(int argc, char* argv[]) {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // 默认配置
    uint16_t port = 8080;
    int threads = 8;
    bool debug_mode = true; // 默认启用调试模式
    bool use_ssl = false; // 默认禁用SSL
    std::string cert_path = "./server.crt";
    std::string key_path = "./server.key";
    
    // 命令行参数处理
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            showHelp(argv[0]);
            return 0;
        } else if ((arg == "--port" || arg == "-p") && (i + 1 < argc)) {
            try {
                port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } catch (...) {
                LOG_ERROR("无效的端口号，使用默认端口 8080");
            }
        } else if ((arg == "--threads" || arg == "-t") && (i + 1 < argc)) {
            try {
                int t = std::stoi(argv[++i]);
                if (t > 0) {
                    threads = t;
                } else {
                    LOG_ERROR("线程数必须大于0，使用默认值 8");
                }
            } catch (...) {
                LOG_ERROR("无效的线程数，使用默认值 8");
            }
        } else if (arg == "--ssl" || arg == "-s") {
            use_ssl = true;
        } else if (arg == "--no-ssl") {
            use_ssl = false;
        } else if ((arg == "--cert" || arg == "-c") && (i + 1 < argc)) {
            cert_path = argv[++i];
        } else if ((arg == "--key" || arg == "-k") && (i + 1 < argc)) {
            key_path = argv[++i];
        } else if (arg == "--debug") {
            debug_mode = true;
        } else if (arg == "--no-debug") {
            debug_mode = false;
        } else {
            LOG_ERROR("未知选项: " + std::string(arg));
            LOG_ERROR("使用 --help 查看用法");
            return 1;
        }
    }
    
    // 创建服务器实例
    HttpServer2 server(use_ssl);
    g_server = &server;
    
    // 如果启用SSL，设置证书和密钥
    if (use_ssl) {
        try {
            std::cout << "设置SSL证书: " << cert_path << std::endl;
            std::cout << "设置SSL密钥: " << key_path << std::endl;
            server.setSslCertificates(cert_path, key_path);
        } catch (const std::exception& e) {
            LOG_ERROR("SSL证书设置失败: " + std::string(e.what()));
            return 1;
        }
    }
    
    // 启用日志
    log_init();
    log_add_console_sink();
    log_set_level(LOG_LEVEL_INFO);
    std::cout << "日志系统已初始化（控制台输出）" << std::endl;
    
    // 配置调试模式
    server.enableDebugMode(debug_mode);
    std::cout << "调试模式: " << (debug_mode ? "已启用" : "已禁用") << std::endl;
    
    // 设置路由
    server.get("/", [](const HttpRequest& /*request*/) {
        HttpResponse response;
        response.status_code = "200";
        response.status_message = "OK";
        response.headers["Content-Type"] = "text/plain";
        response.body = "Hello, Concurrent World!";
        return response;
    });
    
    server.get("/test", [](const HttpRequest& /*request*/) {
        // 模拟一些处理时间
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        HttpResponse response;
        response.status_code = "200";
        response.status_message = "OK";
        response.headers["Content-Type"] = "text/plain";
        response.body = "Test response with 100ms delay";
        return response;
    });
    
    // 添加性能监控路由
    server.get("/metrics", [&server](const HttpRequest& /*request*/) {
        HttpResponse response;
        response.status_code = "200";
        response.status_message = "OK";
        response.headers["Content-Type"] = "text/plain";
        response.body = server.getPerformanceReport();
        return response;
    });
    
    // 启动服务器
    g_server_thread = std::thread([&server, port, threads, debug_mode, use_ssl]() {
        std::cout << "正在启动" << (use_ssl ? "HTTPS" : "HTTP") << "服务器，监听端口 " << port << ", 使用" << threads << "个工作线程..." << std::endl;
        server.start(port, threads);
    });
    
    std::cout << "服务器已启动，监听端口 " << port << ", 使用" << threads << "个工作线程, " 
              << (use_ssl ? "SSL已启用, " : "SSL已禁用, ") 
              << "调试模式: " << (debug_mode ? "已启用" : "已禁用") << std::endl;
    std::cout << "按Enter键停止服务器，或使用Ctrl+C发送SIGINT信号..." << std::endl;
    
    // 创建一个单独的线程来等待用户输入
    std::thread input_thread([&]() {
        std::cin.get();
        g_should_exit = true;
    });
    
    // 主循环检查退出标志
    while (!g_should_exit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 等待输入线程完成
    if (input_thread.joinable()) {
        input_thread.join();
    }
    
    // 停止服务器
    std::cout << "正在停止服务器..." << std::endl;
    server.stop();
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
    
    // 打印性能报告
    std::cout << "\n==== 性能报告 ====\n" << server.getPerformanceReport() << std::endl;
    
    std::cout << "服务器已退出。请查看 access.log 和 error.log 获取详细日志。" << std::endl;
    return 0;
}