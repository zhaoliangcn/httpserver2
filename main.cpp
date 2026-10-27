// main.cpp - 测试HttpServer2
//
// 日志库实现由 logger_impl.cpp 提供（stb-style single-header：在唯一一个 .cpp 中定义实现）
#include "httpserver2.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <string>
// 使用 PipeYAML 库解析 YAML 配置文件
// 走 CMakeLists 里已经设置的 PIPEYAML_INCLUDE_DIR，不再硬编码兄弟目录路径
// （原写法 ../thirdparty/yaml-cpp/include/... 只在特定的目录布局下成立，
//   换机器或换平台就会找不到头文件）。
#include <pipeyaml/pipeyaml.h>

// 简单的YAML解析辅助函数
struct Config {
    bool enable_basic_auth = true;
    bool set_request_size_limit = true;
    size_t max_request_size = 5 * 1024 * 1024; // 5MB
    bool enable_csrf_protection = true;
    bool enable_debug_mode = true;
    std::string csrf_cookie_name = "XSRF-TOKEN";
    std::string csrf_header_name = "X-CSRF-Token";
};

// 从 YAML 映射节点中安全读取一个键。
//
// PipeYAML 与 yaml-cpp 在这里有三处语义差异，直接照搬 yaml-cpp 的写法会出 bug：
//  1. PipeYAML 的 Node 没有 operator bool，yaml-cpp 的 `if (root["k"])` 判定
//     键是否存在，在 PipeYAML 里必须显式调 is_defined()；
//  2. PipeYAML 的非 const operator[] 会在键不存在时「创建」一个 undefined
//     条目并写回映射，读取时必须用 const 引用查，否则会污染配置树；
//  3. 非 map 节点上做键查找会抛 YAML::Exception，调用前必须确认 is_map()。
//
// 读不到、类型不匹配一律返回 fallback，绝不把异常抛给调用方。
template <typename T>
T yamlGet(const YAML::Node& map, const std::string& key, T fallback) {
    if (!map.is_map()) return fallback;
    YAML::Node node = map[key];
    if (!node.is_defined()) return fallback;
    try {
        return node.as<T>();
    } catch (const YAML::Exception&) {
        return fallback;
    }
}

// 使用 PipeYAML 库解析 YAML 配置文件
Config parseConfigFile(const std::string& config_path) {
    Config config;

    try {
        // 加载并解析 YAML 配置文件。
        // 注意：PipeYAML 的 LoadFile 有 std::string 与 std::filesystem::path 两个
        // 重载，直接传 const char* 字面量会因转换歧义编译失败，必须显式给
        // std::string。
        YAML::Node root = YAML::LoadFile(std::string(config_path));

        if (!root.is_map()) {
            LOG_WARNING("配置文件根节点不是映射结构: " + config_path + "，使用默认配置");
            return config;
        }

        // const 引用：避免 PipeYAML 的 operator[] 在键缺失时创建条目
        const YAML::Node& map = root;

        // 解析基本认证配置
        config.enable_basic_auth =
            yamlGet<bool>(map, "enable_basic_auth", config.enable_basic_auth);

        // 解析请求大小限制配置
        config.set_request_size_limit =
            yamlGet<bool>(map, "set_request_size_limit", config.set_request_size_limit);
        config.max_request_size =
            yamlGet<size_t>(map, "max_request_size", config.max_request_size);

        // 解析 CSRF 保护配置
        config.enable_csrf_protection =
            yamlGet<bool>(map, "enable_csrf_protection", config.enable_csrf_protection);
        config.csrf_cookie_name =
            yamlGet<std::string>(map, "csrf_cookie_name", config.csrf_cookie_name);
        config.csrf_header_name =
            yamlGet<std::string>(map, "csrf_header_name", config.csrf_header_name);

        // 解析调试模式配置
        config.enable_debug_mode =
            yamlGet<bool>(map, "enable_debug_mode", config.enable_debug_mode);

        LOG_INFO("成功解析配置文件: " + config_path);
    } catch (const YAML::Exception& e) {
        // PipeYAML 没有 yaml-cpp 的 YAML::BadFile，文件打不开与语法错误
        // 都归到 YAML::Exception（前者是 ParserException，后者可能是
        // ScannerException / ParserException / RepresentationException）。
        // 这里按消息区分，文件不存在属于可预期的正常情况，只记警告。
        std::string msg = e.what();
        if (msg.find("Cannot open file") != std::string::npos) {
            LOG_WARNING("无法打开配置文件: " + config_path + " (" + msg + ")，使用默认配置");
        } else {
            LOG_ERROR("解析配置文件时出错: " + msg + "，使用默认配置");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("读取配置文件时发生未知错误: " + std::string(e.what()) + "，使用默认配置");
    }

    return config;
}

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

int main(int argc, char* argv[]) {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // 默认配置
    uint16_t port = 8080;
    bool use_ssl = false;
    int thread_count = 0;  // 0=使用CPU核心数
    std::string cert_path = "./server.crt";
    std::string key_path = "./server.key";
    std::string config_path = "config.yaml"; // 默认配置文件路径
    
    // 命令行参数处理
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ssl" || arg == "-s") {
            use_ssl = true;
        } else if (arg == "--no-ssl") {
            use_ssl = false;
        } else if ((arg == "--cert" || arg == "-c") && (i + 1 < argc)) {
            cert_path = argv[++i];
        } else if ((arg == "--key" || arg == "-k") && (i + 1 < argc)) {
            key_path = argv[++i];
        } else if ((arg == "--config" || arg == "-f") && (i + 1 < argc)) {
            config_path = argv[++i];
        } else if ((arg == "--threads" || arg == "-t") && (i + 1 < argc)) {
            try {
                thread_count = std::max(1, std::stoi(argv[++i]));
            } catch (...) {
                LOG_ERROR("无效的线程数，使用默认值（CPU核心数）");
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [端口] [选项]" << std::endl;
            std::cout << "选项:" << std::endl;
            std::cout << "  --ssl, -s      启用SSL支持" << std::endl;
            std::cout << "  --no-ssl       禁用SSL支持" << std::endl;
            std::cout << "  --cert, -c     指定SSL证书路径 (默认: ./server.crt)" << std::endl;
            std::cout << "  --key, -k      指定SSL密钥路径 (默认: ./server.key)" << std::endl;
            std::cout << "  --config, -f   指定配置文件路径 (默认: config.yaml)" << std::endl;
            std::cout << "  --threads, -t  指定工作线程数 (默认: CPU核心数)" << std::endl;
            std::cout << "  --help, -h     显示此帮助信息" << std::endl;
            std::cout << "配置文件示例 (YAML格式):" << std::endl;
            std::cout << "  # 基本认证配置" << std::endl;
            std::cout << "  enable_basic_auth: true" << std::endl;
            std::cout << "  # 请求大小限制配置" << std::endl;
            std::cout << "  set_request_size_limit: true" << std::endl;
            std::cout << "  max_request_size: 5242880  # 5MB (字节)" << std::endl;
            std::cout << "  # CSRF保护配置" << std::endl;
            std::cout << "  enable_csrf_protection: true" << std::endl;
            std::cout << "  csrf_cookie_name: XSRF-TOKEN" << std::endl;
            std::cout << "  csrf_header_name: X-CSRF-Token" << std::endl;
            std::cout << "  # 调试模式配置" << std::endl;
            std::cout << "  enable_debug_mode: true" << std::endl;
            std::cout << "示例:" << std::endl;
            std::cout << "  " << argv[0] << " 8080               在8080端口启动，默认禁用SSL，使用默认配置文件" << std::endl;
            std::cout << "  " << argv[0] << " 443 --ssl          在443端口启动，启用SSL（使用默认证书）" << std::endl;
            std::cout << "  " << argv[0] << " 8080 --config my_config.yaml  使用指定配置文件" << std::endl;
            return 0;
        } else if (i == 1) { // 第一个参数可能是端口号
            try {
                port = static_cast<uint16_t>(std::stoi(arg));
            } catch (...) {
                LOG_ERROR("无效的端口号，使用默认端口 8080");
            }
        } else {
            LOG_ERROR("未知选项: " + std::string(arg));
            LOG_ERROR("使用 --help 查看用法");
        }
    }
    
    // 解析配置文件
    std::cout << "尝试从配置文件读取设置: " << config_path << std::endl;
    Config config = parseConfigFile(config_path);
    
    try {
        // 创建服务器实例
        HttpServer2 server(use_ssl);
        g_server = &server;
        
        // 根据配置文件设置基本认证
        if (config.enable_basic_auth) {
            server.setBasicAuth("Test Server");
            // 从环境变量读取用户名和密码，避免硬编码
            const char* env_user = std::getenv("HTTP_AUTH_USERNAME");
            const char* env_pass = std::getenv("HTTP_AUTH_PASSWORD");
            if (env_user && env_pass && strlen(env_user) > 0 && strlen(env_pass) > 0) {
                server.addUser(env_user, env_pass);
                std::cout << "已设置基本认证用户（来自环境变量）" << std::endl;
            } else {
                std::cout << "警告：未设置 HTTP_AUTH_USERNAME/HTTP_AUTH_PASSWORD 环境变量，基本认证已跳过" << std::endl;
                std::cout << "提示：可设置环境变量并重新启动，或使用配置文件启用认证" << std::endl;
            }
        } else {
            std::cout << "基本认证已禁用" << std::endl;
        }
        
        // 根据配置文件设置请求大小限制
        if (config.set_request_size_limit) {
            server.setMaxRequestSize(config.max_request_size);
            std::cout << "已设置请求大小限制：" << (config.max_request_size / 1024 / 1024) << "MB" << std::endl;
        } else {
            std::cout << "请求大小限制已禁用" << std::endl;
        }
        
        // 根据配置文件启用CSRF保护
        if (config.enable_csrf_protection) {
            server.enableCsrfProtection(config.csrf_cookie_name, config.csrf_header_name);
            std::cout << "已启用CSRF保护，Cookie名称：" << config.csrf_cookie_name 
                      << "，Header名称：" << config.csrf_header_name << std::endl;
        } else {
            std::cout << "CSRF保护已禁用" << std::endl;
        }
        
        // 根据配置文件启用调试模式
        server.enableDebugMode(config.enable_debug_mode);
        std::cout << "调试模式已" << (config.enable_debug_mode ? "启用" : "禁用") << std::endl;
        
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
        
        // 添加错误处理中间件（放在最前面）
        server.use(createErrorHandlingMiddleware());
        
        // 添加日志中间件
        server.use([](const HttpRequest& request) -> std::optional<HttpResponse> {
        std::cout << "[LOG] " << request.method << " " << request.path << std::endl;
        if (!request.query_params.empty()) {
            std::cout << "[LOG] Query params:" << std::endl;
            for (const auto& [key, value] : request.query_params) {
                std::cout << "  " << key << " = " << value << std::endl;
            }
        }
        // 返回 nullopt = 放行给后续中间件 / 路由处理器
        return std::nullopt;
    });

    // 添加CORS中间件
        server.use([](const HttpRequest& request) -> std::optional<HttpResponse> {
            if (request.method != "OPTIONS") {
                return std::nullopt;   // 非预检请求：放行
            }
            HttpResponse response;
            response.status_code = "204";
            response.status_message = "No Content";
            response.headers["Access-Control-Allow-Origin"] = "*";
            response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
            response.headers["Access-Control-Allow-Headers"] = "Content-Type";
            return response;   // 有值 = 已处理预检请求
        });

        // 添加示例路由 - 主页
        server.get("/", [](const HttpRequest&) -> HttpResponse {
            HttpResponse response;
            response.status_code = "200";
            response.status_message = "OK";
            response.headers["Content-Type"] = "application/json";
            response.body = "{\"message\": \"Hello, HttpServer2!\", \"version\": \"2.6\", \"features\": [\"路径参数\", \"正则路由\", \"HTTP方法区分\", \"中间件机制\"]}";
            return response;
        });

    // 测试GET方法
    server.get("/api/users", [](const HttpRequest&) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"users\": [{\"id\": 1, \"name\": \"User 1\"}, {\"id\": 2, \"name\": \"User 2\"}]}";
        return response;
    });

    // 测试路径参数
    server.get("/api/users/{id}", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        std::string user_id = request.path_params.count("id") ? request.path_params.at("id") : "unknown";
        response.body = "{\"message\": \"User profile\", \"user_id\": \"" + user_id + "\"}";
        return response;
    });

    // 测试POST方法
    server.post("/api/users", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "201";
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"message\": \"User created successfully!\", \"received_data\": \"" + request.body + "\"}";
        return response;
    });

    // 测试PUT方法
    server.put("/api/users/{id}", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        std::string user_id = request.path_params.count("id") ? request.path_params.at("id") : "unknown";
        response.body = "{\"message\": \"User updated\", \"user_id\": \"" + user_id + "\", \"data\": \"" + request.body + "\"}";
        return response;
    });

    // 测试DELETE方法
    server.del("/api/users/{id}", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        std::string user_id = request.path_params.count("id") ? request.path_params.at("id") : "unknown";
        response.body = "{\"message\": \"User deleted\", \"user_id\": \"" + user_id + "\"}";
        return response;
    });

    // 测试带查询参数的路由
    server.get("/api/search", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        std::string query = request.query_params.count("q") ? request.query_params.at("q") : "";
        response.body = "{\"message\": \"Search results\", \"query\": \"" + query + "\"}";
        return response;
    });
    
    // 测试正则表达式路由 - 匹配以/api/articles/开头且后面跟着数字ID的路径
    server.get("/api/articles/([0-9]+)", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        // 正则表达式匹配的参数会自动放入path_params中，以匹配组索引为键
        std::string article_id = request.path_params.count("1") ? request.path_params.at("1") : "unknown";
        response.body = "{\"message\": \"Article details\", \"article_id\": \"" + article_id + "\"}";
        return response;
    });
    
    // 测试更复杂的正则表达式路由 - 匹配多种格式的URL
    server.get("/api/users/([a-zA-Z0-9_]+)/posts/([0-9]+)", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        std::string username = request.path_params.count("1") ? request.path_params.at("1") : "unknown";
        std::string post_id = request.path_params.count("2") ? request.path_params.at("2") : "unknown";
        
        response.body = "{\"message\": \"User post\", \"username\": \"" + username + "\", \"post_id\": \"" + post_id + "\"}";
        return response;
    });
    
    // 测试带可选部分的正则表达式路由
    server.get("/api/items(/([0-9]+))?", [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.status_code = "200";
        response.headers["Content-Type"] = "application/json";
        
        if (request.path_params.count("2")) {
            // 如果提供了ID参数
            std::string item_id = request.path_params.at("2");
            response.body = "{\"message\": \"Single item\", \"item_id\": \"" + item_id + "\"}";
        } else {
            // 如果没有提供ID参数，返回列表
            response.body = "{\"message\": \"List of all items\"}";
        }
        return response;
    });
    
    // 添加性能监控路由 - 返回Prometheus格式指标
    server.get("/metrics", [&server](const HttpRequest&) {
        HttpResponse response;
        response.status_code = "200";
        response.status_message = "OK";
        response.headers["Content-Type"] = "text/plain";
        response.body = server.getPrometheusMetrics();
        return response;
    });
    
    // 添加传统性能报告路由
    server.get("/performance", [&server](const HttpRequest&) {
        HttpResponse response;
        response.status_code = "200";
        response.status_message = "OK";
        response.headers["Content-Type"] = "text/plain";
        response.body = server.getPerformanceReport();
        return response;
    });
    
    // 添加健康检查API
    server.get("/health", [](const HttpRequest&) {
        HttpResponse response;
        response.status_code = "200";
        response.status_message = "OK";
        response.headers["Content-Type"] = "application/json";
        // 构建简单的JSON响应，避免复杂的字符串转义
        std::string status = "ok";
        std::string service = "HttpServer2";
        std::time_t now = std::time(nullptr);
        
        response.body = "{\"status\":\"" + status + "\",\"timestamp\":\"" + std::to_string(now) + "\",\"service\":\"" + service + "\"}";
        return response;
    });
    std::cout << "健康检查API已配置：/health\n";
    std::cout << "性能监控路由已配置：/metrics" << std::endl;
    
    // 使用服务器提供的静态文件服务方法，更可靠
    server.serveStatic("/static", "./static", "index.html");
    std::cout << "静态文件服务已配置：/static 映射到 ./static 目录\n";

    // 测试查询参数解析
    server.get("/test/query", [](const HttpRequest& req) {
        HttpResponse res;
        res.status_code = "200";
        res.headers["Content-Type"] = "application/json";
        
        std::string body = "{\"query_params\": {";
        bool first = true;
        for (const auto& [key, value] : req.query_params) {
            if (!first) body += ", ";
            body += "\"" + key + "\": \"" + value + "\"";
            first = false;
        }
        body += "}}";
        
        res.body = body;
        return res;
    });
        
        // 测试表单数据解析 (application/x-www-form-urlencoded)
        server.post("/test/form", [](const HttpRequest& req) {
            HttpResponse res;
            res.status_code = "200";
            res.headers["Content-Type"] = "application/json";
            
            std::string body = "{\"form_data\": {";
            bool first = true;
            for (const auto& [key, value] : req.form_data) {
                if (!first) body += ", ";
                body += "\"" + key + "\": \"" + value + "\"";
                first = false;
            }
            body += "}}";
            
            res.body = body;
            return res;
        });
        
        // 测试JSON请求体解析
        server.post("/test/json", [](const HttpRequest& req) {
            HttpResponse res;
            res.status_code = "200";
            res.headers["Content-Type"] = "application/json";
            
            std::string body = "{\"json_data\": {";
            bool first = true;
            for (const auto& [key, value] : req.form_data) {  // JSON数据也存储在form_data中
                if (!first) body += ", ";
                body += "\"" + key + "\": \"" + value + "\"";
                first = false;
            }
            body += "}}";
            
            res.body = body;
            return res;
        });
        
        // 测试文件上传功能
        server.post("/test/upload", [](const HttpRequest& req) {
            HttpResponse res;
            res.status_code = "200";
            res.headers["Content-Type"] = "application/json";
            
            std::string body = "{";
            
            // 添加表单字段
            body += "\"form_data\": {";
            bool first = true;
            for (const auto& [key, value] : req.form_data) {
                if (!first) body += ", ";
                body += "\"" + key + "\": \"" + value + "\"";
                first = false;
            }
            body += "}, ";
            
            // 添加上传的文件信息
            body += "\"uploaded_files\": [";
            first = true;
            for (const auto& file : req.files) {
                if (!first) body += ", ";
                body += "{\"name\": \"" + file.name + "\", " +
                        "\"filename\": \"" + file.filename + "\", " +
                        "\"content_type\": \"" + file.content_type + "\", " +
                        "\"size\": " + std::to_string(file.data.size()) + "}";
                first = false;
            }
            body += "]}";
            
            res.body = body;
            return res;
        });
    
    
        
        // 初始化新日志库：添加控制台和文件 sink
        log_init();
        log_add_console_sink();
        log_add_file_sink("server.log", 10 * 1024 * 1024, 5);
        log_set_level(LOG_LEVEL_DEBUG);
        std::cout << "日志系统已初始化（控制台 + 文件日志 server.log）" << std::endl;
        
        // 启用调试模式
        std::cout << "启用调试模式..." << std::endl;
        server.enableDebugMode(true);
        
        // 启动服务器，使用指定线程数（0=CPU核心数）
        g_server_thread = std::thread([&server, port, use_ssl, thread_count]() {
            int tc = thread_count > 0 ? thread_count : static_cast<int>(std::thread::hardware_concurrency());
            std::cout << "正在启动" << (use_ssl ? "HTTPS" : "HTTP") << "服务器，监听端口 " << port << "，使用" << tc << "个工作线程..." << std::endl;
            server.start(port, thread_count > 0 ? static_cast<size_t>(thread_count) : 0);
        });
        
        std::cout << "服务器已启动，监听端口 " << port;
        if (thread_count > 0) {
            std::cout << "，使用" << thread_count << "个工作线程";
        } else {
            std::cout << "，使用CPU核心数个线程";
        }
        std::cout << std::endl;
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
        
    } catch (const std::exception& e) {
        LOG_ERROR("服务器启动失败: " + std::string(e.what()));
        return 1;
    }
    
    std::cout << "服务器已退出" << std::endl;
    return 0;
}