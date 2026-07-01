/*
 * HttpServer2 压力测试工具 v2.1
 *
 * 功能特性：
 *   支持 GET/POST/PUT/DELETE 方法
 *   支持 Keep-Alive 连接复用
 *   支持自定义请求头和请求体
 *   支持预热阶段（不纳入统计）
 *   支持请求数模式 + 时长模式
 *   支持渐进并发（ramp-up）
 *   输出 JSON 结果文件
 *   支持 Ctrl+C 优雅退出
 *   延迟分布：P50/P75/P90/P95/P99 + 直方图
 *
 * 用法:
 *   ./test_stress [选项]
 *
 *   基本:
 *     --host HOST           目标主机 (默认 localhost)
 *     --port PORT           目标端口 (默认 8080)
 *     --endpoint PATH       测试路径 (默认 /)
 *     --concurrency N       并发连接数 (默认 100)
 *     --requests N          总请求数 (默认 10000)
 *     --duration N[s]       测试时长，如 30s (优先级高于 --requests)
 *     --ramp-up N[s]        渐进并发时间，如 5s (默认 0)
 *
 *   请求:
 *     --method M            HTTP 方法 GET/POST/PUT/DELETE (默认 GET)
 *     --body TEXT           请求体 (默认空)
 *     --body-file FILE      从文件读取请求体
 *     --content-type TYPE   Content-Type (默认 text/plain)
 *     --header "K: V"       自定义请求头，可重复
 *
 *   连接:
 *     --keep-alive          启用连接复用 (默认关闭)
 *     --keepalive-reuse N   单个连接最大复用次数 (默认 0 = 不限)
 *     --timeout N[s]        请求超时秒数 (默认 10s)
 *
 *   统计:
 *     --warmup N            预热请求数，不纳入统计 (默认 0)
 *     --json FILE           将结果写入 JSON 文件 (默认不写)
 *     --histogram           输出延迟直方图 (默认关闭)
 *
 *   其他:
 *     --help                帮助信息
 */

// 后端抽象层（支持 Boost.Asio / hp-async-io）
#include "stress_backend.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <sstream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <csignal>
#include <functional>

// ============================================================
// 全局退出标志（用于 Ctrl+C）
// ============================================================
static std::atomic<bool> g_should_exit{false};
static void signalHandler(int) {
    g_should_exit = true;
}

// ============================================================
// 配置
// ============================================================
constexpr int HTTP_VERSION = 11; // 1.1

struct StressConfig {
    // 目标
    std::string host = "localhost";
    int port = 8080;
    std::string endpoint = "/";

    // 负载
    int concurrency = 100;
    int total_requests = 10000;
    int duration_sec = 0;       // 0 = 使用 total_requests
    int ramp_up_sec = 0;

    // 请求
    std::string method = "GET";
    std::string body;
    std::string content_type = "text/plain";
    std::vector<std::pair<std::string, std::string>> custom_headers;

    // 连接
    bool keep_alive = false;
    int keepalive_max_reuse = 0; // 0 = 无限
    int timeout_sec = 10;

    // 统计
    int warmup = 0;
    std::string json_output;
    bool histogram = false;
};

// ============================================================
// 统计
// ============================================================
struct StressStats {
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> conn_errors{0};
    std::atomic<uint64_t> timeout_errors{0};

    // 延迟（微秒精度）
    std::vector<double> latencies;
    std::vector<uint64_t> status_codes;
    std::mutex data_mutex;

    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;

    void addLatency(double ms, int status_code) {
        std::lock_guard<std::mutex> lock(data_mutex);
        latencies.push_back(ms);
        status_codes.push_back(status_code);
    }

    void report(const StressConfig& cfg) const {
        if (latencies.empty()) {
            std::cout << "\n  无成功请求，无法生成统计报告。\n";
            return;
        }
        auto sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();
        double avg = 0;
        for (auto v : sorted) avg += v;
        avg /= n;

        auto p = [&](double pct) -> double {
            size_t idx = std::min(static_cast<size_t>(n * pct), n - 1);
            return sorted[idx];
        };

        double total_sec = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;

        std::cout << "\n========================================\n";
        std::cout << "  压力测试结果\n";
        std::cout << "========================================\n";
        std::cout << "  目标:     " << cfg.host << ":" << cfg.port << cfg.endpoint << "\n";
        std::cout << "  方法:     " << cfg.method << "\n";
        std::cout << "  总请求:   " << sent.load() << "\n";
        std::cout << "  成功:     " << ok.load() << "\n";
        std::cout << "  失败:     " << failed.load() << "\n";
        std::cout << "  连接错误: " << conn_errors.load() << "\n";
        std::cout << "  超时:     " << timeout_errors.load() << "\n";
        std::cout << "  成功率:   " << std::fixed << std::setprecision(1)
                  << (sent.load() > 0 ? (ok.load() * 100.0 / sent.load()) : 0.0) << "%\n";
        std::cout << "  总字节:   " << bytes.load() << "\n";
        std::cout << "  总耗时:   " << std::fixed << std::setprecision(2) << total_sec << " s\n";
        double rps = ok.load() / std::max(total_sec, 0.001);
        double mbps = (bytes.load() / 1024.0 / 1024.0) / std::max(total_sec, 0.001);
        std::cout << "  RPS:      " << std::fixed << std::setprecision(0) << rps << "\n";
        std::cout << "  吞吐:     " << std::fixed << std::setprecision(2) << mbps << " MB/s\n";
        std::cout << "----------------------------------------\n";
        std::cout << "  延迟分布:\n";
        std::cout << "    最低:  " << std::fixed << std::setprecision(2) << sorted.front() << " ms\n";
        std::cout << "    P50:   " << p(0.50) << " ms\n";
        std::cout << "    P75:   " << p(0.75) << " ms\n";
        std::cout << "    P90:   " << p(0.90) << " ms\n";
        std::cout << "    P95:   " << p(0.95) << " ms\n";
        std::cout << "    P99:   " << p(0.99) << " ms\n";
        std::cout << "    最高:  " << sorted.back() << " ms\n";
        std::cout << "    平均:  " << avg << " ms\n";

        // 标准差
        double variance = 0;
        for (auto v : sorted) variance += (v - avg) * (v - avg);
        variance /= n;
        std::cout << "    标准差: " << std::sqrt(variance) << " ms\n";
        std::cout << "----------------------------------------\n";

        // 状态码分布
        std::map<int, uint64_t> code_map;
        for (auto sc : status_codes) code_map[sc]++;
        std::cout << "  状态码:\n";
        for (auto& kv : code_map) {
            std::cout << "    " << kv.first << ": " << kv.second;
            if (kv.first >= 200 && kv.first < 300) std::cout << " OK";
            else if (kv.first >= 300 && kv.first < 400) std::cout << " Redirect";
            else if (kv.first >= 400 && kv.first < 500) std::cout << " Client Error";
            else if (kv.first >= 500) std::cout << " Server Error";
            std::cout << "\n";
        }

        // 直方图
        if (cfg.histogram && !sorted.empty()) {
            std::cout << "----------------------------------------\n";
            std::cout << "  延迟直方图 (ms):\n";
            double bins[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 5000};
            int num_bins = sizeof(bins) / sizeof(bins[0]);
            double last_bin = bins[num_bins - 1];
            for (int i = 0; i < num_bins; ++i) {
                double lo = (i == 0) ? 0 : bins[i-1];
                double hi = bins[i];
                auto cnt = std::count_if(sorted.begin(), sorted.end(),
                    [lo, hi](double v) { return v > lo && v <= hi; });
                if (cnt == 0) continue;
                int bar = static_cast<int>(cnt * 50 / n);
                std::cout << "  " << std::setw(5) << lo << " - " << std::setw(5) << hi
                          << " |" << std::string(bar, '#') << " " << cnt << "\n";
            }
            auto cnt = std::count_if(sorted.begin(), sorted.end(),
                [last_bin](double v) { return v > last_bin; });
            if (cnt > 0) {
                std::cout << "  " << std::setw(5) << last_bin << "+     |"
                          << std::string(cnt * 50 / n, '#') << " " << cnt << "\n";
            }
        }
        std::cout << "========================================\n";
    }
};

// ============================================================
// 构造 HTTP 请求字符串
// ============================================================
static std::string buildRequest(const StressConfig& cfg, int /* reuse_count */) {
    std::stringstream ss;
    ss << cfg.method << " " << cfg.endpoint << " HTTP/1.1\r\n"
       << "Host: " << cfg.host << ":" << cfg.port << "\r\n"
       << "User-Agent: StressTest/2.0\r\n"
       << "Accept: */*\r\n";

    // Connection
    if (cfg.keep_alive) {
        ss << "Connection: keep-alive\r\n";
    } else {
        ss << "Connection: close\r\n";
    }

    // 自定义头
    for (auto& h : cfg.custom_headers) {
        ss << h.first << ": " << h.second << "\r\n";
    }

    // 请求体
    if (!cfg.body.empty()) {
        ss << "Content-Type: " << cfg.content_type << "\r\n"
           << "Content-Length: " << cfg.body.size() << "\r\n";
    }

    ss << "\r\n";
    if (!cfg.body.empty()) {
        ss << cfg.body;
    }
    return ss.str();
}

// ============================================================
// 读取响应头中的 Content-Length
// ============================================================
// (unused — kept for future raw-header parsing)
#if 0
static size_t parseContentLength(const std::string& header) {
    auto pos = header.find("Content-Length:");
    if (pos == std::string::npos) pos = header.find("content-length:");
    if (pos == std::string::npos) pos = header.find("Content-length:");
    if (pos == std::string::npos) return 0;

    pos += 15; // skip "Content-Length:"
    while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos;
    size_t val = 0;
    while (pos < header.size() && header[pos] >= '0' && header[pos] <= '9') {
        val = val * 10 + (header[pos] - '0');
        ++pos;
    }
    return val;
}

static int parseStatusLine(const std::string& header) {
    auto sp1 = header.find(' ');
    if (sp1 == std::string::npos) return 0;
    auto sp2 = header.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return 0;
    return std::stoi(header.substr(sp1 + 1, sp2 - sp1 - 1));
}
#endif

// ============================================================
// Keep-Alive 连接上的单个请求（使用后端抽象）
// ============================================================
static backend::Response doRequestOnSocket(backend::TCPSocket& sock, const std::string& request,
                                           const std::chrono::steady_clock::time_point& deadline)
{
    if (std::chrono::steady_clock::now() >= deadline) return {};
    if (!backend::send(sock, request)) return {};
    return backend::recv_http(sock);
}

// ============================================================
// 工作线程（支持 Keep-Alive 连接复用）
// ============================================================
static void worker(const StressConfig& cfg, StressStats& stats,
                   std::atomic<int>& remaining_req,
                   std::atomic<int>& remaining_time_ms,
                   int /* thread_id */)
{
    backend::Context ctx;
    backend::TCPSocket sock(ctx);
    bool has_conn = false;
    int reuse_count = 0;

    while (!g_should_exit) {
        // --- 请求分配 ---
        if (cfg.duration_sec == 0) {
            int cur = remaining_req.fetch_sub(1);
            if (cur <= 0) break;
        } else {
            if (remaining_time_ms.load() <= 0) break;
        }

        // --- 连接管理 ---
        bool need_new_conn = false;
        if (!has_conn) {
            need_new_conn = true;
        } else if (cfg.keep_alive && cfg.keepalive_max_reuse > 0 &&
                   reuse_count >= cfg.keepalive_max_reuse) {
            need_new_conn = true;
        } else if (!cfg.keep_alive) {
            // 短连接：每次请求都新建连接
            need_new_conn = true;
        }

        if (need_new_conn) {
            backend::close(sock);
            bool connected = backend::connect(sock, cfg.host, cfg.port, cfg.timeout_sec);
            // 重试最多 3 次
            for (int retry = 0; !connected && retry < 3 && !g_should_exit; ++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                connected = backend::connect(sock, cfg.host, cfg.port, cfg.timeout_sec);
            }
            if (!connected) {
                stats.sent++;
                stats.failed++;
                stats.conn_errors++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                has_conn = false;
                continue;
            }
            has_conn = true;
            reuse_count = 0;
        }

        // --- 构造请求 ---
        std::string req = buildRequest(cfg, reuse_count);

        // --- 发送并接收响应 ---
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.timeout_sec);
        auto start = std::chrono::steady_clock::now();

        backend::Response rr = doRequestOnSocket(sock, req, deadline);

        auto end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

        stats.sent++;
        if (rr.success && rr.status_code >= 200 && rr.status_code < 300) {
            stats.ok++;
            stats.bytes += rr.body_size;
            stats.addLatency(elapsed_ms, rr.status_code);
            if (!rr.keep_alive) {
                backend::close(sock);
                has_conn = false;
                reuse_count = 0;
            } else {
                reuse_count++;
            }
        } else if (rr.success) {
            stats.failed++;
            stats.addLatency(elapsed_ms, rr.status_code);
            if (!rr.keep_alive) {
                backend::close(sock);
                has_conn = false;
                reuse_count = 0;
            } else {
                reuse_count++;
            }
        } else {
            stats.failed++;
            stats.conn_errors++;
            backend::close(sock);
            has_conn = false;
            reuse_count = 0;
        }
    }

    backend::close(sock);
}

// ============================================================
// 写入 JSON 结果
// ============================================================
static void writeJson(const StressStats& stats, const StressConfig& cfg) {
    if (cfg.json_output.empty()) return;

    auto sorted = stats.latencies;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    double avg = 0;
    for (auto v : sorted) avg += v;
    if (n > 0) avg /= n;

    auto p = [&](double pct) -> double {
        if (n == 0) return 0;
        return sorted[std::min(static_cast<size_t>(n * pct), n - 1)];
    };

    double variance = 0;
    for (auto v : sorted) variance += (v - avg) * (v - avg);
    if (n > 0) variance /= n;

    double total_sec = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time).count() / 1000.0;

    // 状态码分布
    std::map<int, uint64_t> code_map;
    for (auto sc : stats.status_codes) code_map[sc]++;

    std::stringstream ss;
    ss << "{\n";
    ss << "  \"tool\": \"test_stress\",\n";
    ss << "  \"version\": \"2.1\",\n";
    ss << "  \"timestamp\": \"" << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "\",\n";
    ss << "  \"target\": \"" << cfg.host << ":" << cfg.port << cfg.endpoint << "\",\n";
    ss << "  \"config\": {\n";
    ss << "    \"method\": \"" << cfg.method << "\",\n";
    ss << "    \"concurrency\": " << cfg.concurrency << ",\n";
    if (cfg.duration_sec > 0) {
        ss << "    \"duration_sec\": " << cfg.duration_sec << ",\n";
    } else {
        ss << "    \"requests\": " << cfg.total_requests << ",\n";
    }
    ss << "    \"keep_alive\": " << (cfg.keep_alive ? "true" : "false") << ",\n";
    ss << "    \"warmup\": " << cfg.warmup << "\n";
    ss << "  },\n";
    ss << "  \"summary\": {\n";
    ss << "    \"sent\": " << stats.sent.load() << ",\n";
    ss << "    \"ok\": " << stats.ok.load() << ",\n";
    ss << "    \"failed\": " << stats.failed.load() << ",\n";
    ss << "    \"conn_errors\": " << stats.conn_errors.load() << ",\n";
    ss << "    \"timeout_errors\": " << stats.timeout_errors.load() << ",\n";
    ss << "    \"total_bytes\": " << stats.bytes.load() << ",\n";
    ss << "    \"duration_sec\": " << total_sec << ",\n";
    ss << "    \"rps\": " << (total_sec > 0 ? stats.ok.load() / total_sec : 0) << ",\n";
    ss << "    \"mbps\": " << (total_sec > 0 ? (stats.bytes.load() / 1048576.0) / total_sec : 0) << ",\n";
    ss << "    \"success_rate\": " << (stats.sent.load() > 0 ? stats.ok.load() * 100.0 / stats.sent.load() : 0) << "\n";
    ss << "  },\n";
    ss << "  \"latency\": {\n";
    if (n > 0) {
        ss << "    \"min_ms\": " << sorted.front() << ",\n";
        ss << "    \"max_ms\": " << sorted.back() << ",\n";
        ss << "    \"avg_ms\": " << avg << ",\n";
        ss << "    \"stddev_ms\": " << std::sqrt(variance) << ",\n";
        ss << "    \"p50_ms\": " << p(0.50) << ",\n";
        ss << "    \"p75_ms\": " << p(0.75) << ",\n";
        ss << "    \"p90_ms\": " << p(0.90) << ",\n";
        ss << "    \"p95_ms\": " << p(0.95) << ",\n";
        ss << "    \"p99_ms\": " << p(0.99) << "\n";
    } else {
        ss << "    \"min_ms\": 0,\n";
        ss << "    \"max_ms\": 0,\n";
        ss << "    \"avg_ms\": 0,\n";
        ss << "    \"stddev_ms\": 0,\n";
        ss << "    \"p50_ms\": 0,\n";
        ss << "    \"p75_ms\": 0,\n";
        ss << "    \"p90_ms\": 0,\n";
        ss << "    \"p95_ms\": 0,\n";
        ss << "    \"p99_ms\": 0\n";
    }
    ss << "  },\n";
    ss << "  \"status_codes\": {\n";
    bool first = true;
    for (auto& kv : code_map) {
        if (!first) ss << ",\n";
        first = false;
        ss << "    \"" << kv.first << "\": " << kv.second;
    }
    ss << "\n  }\n";
    ss << "}\n";

    std::ofstream ofs(cfg.json_output);
    if (ofs) {
        ofs << ss.str();
        std::cout << "  结果已保存至: " << cfg.json_output << "\n";
    } else {
        std::cerr << "  警告: 无法写入 " << cfg.json_output << "\n";
    }
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char* argv[]) {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    StressConfig cfg;

    // ============================================================
    // 命令行参数解析
    // ============================================================
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "  错误: " << arg << " 需要参数\n";
            exit(1);
        };

        if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [选项]\n\n";
            std::cout << "基本:\n";
            std::cout << "  --host HOST           目标主机 (默认 localhost)\n";
            std::cout << "  --port PORT           目标端口 (默认 8080)\n";
            std::cout << "  --endpoint PATH       测试路径 (默认 /)\n";
            std::cout << "  --concurrency N       并发连接数 (默认 100)\n";
            std::cout << "  --requests N          总请求数 (默认 10000)\n";
            std::cout << "  --duration N[s]       测试时长，如 30s (优先于 --requests)\n";
            std::cout << "  --ramp-up N[s]        渐进并发时间，如 5s (默认 0)\n\n";
            std::cout << "请求:\n";
            std::cout << "  --method M            HTTP 方法 GET/POST/PUT/DELETE (默认 GET)\n";
            std::cout << "  --body TEXT           请求体 (默认空)\n";
            std::cout << "  --body-file FILE      从文件读取请求体\n";
            std::cout << "  --content-type TYPE   Content-Type (默认 text/plain)\n";
            std::cout << "  --header \"K: V\"       自定义请求头，可重复\n\n";
            std::cout << "连接:\n";
            std::cout << "  --keep-alive          启用连接复用 (默认关闭)\n";
            std::cout << "  --keepalive-reuse N   单连接最大复用次数 (默认 0 = 不限)\n";
            std::cout << "  --timeout N[s]        请求超时秒数 (默认 10s)\n\n";
            std::cout << "统计:\n";
            std::cout << "  --warmup N            预热请求数，不纳入统计 (默认 0)\n";
            std::cout << "  --json FILE           将结果写入 JSON 文件\n";
            std::cout << "  --histogram           输出延迟直方图\n\n";
            std::cout << "示例:\n";
            std::cout << "  " << argv[0] << " --port 8080 --concurrency 50 --requests 5000\n";
            std::cout << "  " << argv[0] << " --host 127.0.0.1 --port 8080 --keep-alive --duration 30s\n";
            std::cout << "  " << argv[0] << " --method POST --body '{\"key\":\"value\"}' --content-type 'application/json'\n";
            std::cout << "  " << argv[0] << " --header 'Authorization: Bearer token123' --header 'X-Custom: value'\n";
            std::cout << "  " << argv[0] << " --concurrency 200 --warmup 1000 --histogram --json result.json\n";
            return 0;
        } else if (arg == "--host") cfg.host = next();
        else if (arg == "--port") cfg.port = std::stoi(next());
        else if (arg == "--endpoint") cfg.endpoint = next();
        else if (arg == "--concurrency") cfg.concurrency = std::stoi(next());
        else if (arg == "--requests") cfg.total_requests = std::stoi(next());
        else if (arg == "--duration") {
            std::string val = next();
            if (!val.empty() && (val.back() == 's' || val.back() == 'S')) {
                val.pop_back();
            }
            cfg.duration_sec = std::stoi(val);
        } else if (arg == "--ramp-up") {
            std::string val = next();
            if (!val.empty() && (val.back() == 's' || val.back() == 'S')) {
                val.pop_back();
            }
            cfg.ramp_up_sec = std::stoi(val);
        } else if (arg == "--method") {
            cfg.method = next();
            std::transform(cfg.method.begin(), cfg.method.end(), cfg.method.begin(), ::toupper);
        } else if (arg == "--body") cfg.body = next();
        else if (arg == "--body-file") {
            std::string path = next();
            std::ifstream ifs(path);
            if (!ifs) {
                std::cerr << "  错误: 无法读取 " << path << "\n";
                return 1;
            }
            cfg.body.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        } else if (arg == "--content-type") cfg.content_type = next();
        else if (arg == "--header") {
            std::string kv = next();
            auto colon = kv.find(':');
            if (colon == std::string::npos) {
                std::cerr << "  错误: --header 格式应为 \"Key: Value\"，收到: " << kv << "\n";
                return 1;
            }
            std::string key = kv.substr(0, colon);
            std::string val = kv.substr(colon + 1);
            // 去除 key 两端空格
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            // 去除 val 前导空格
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            cfg.custom_headers.emplace_back(key, val);
        } else if (arg == "--keep-alive") cfg.keep_alive = true;
        else if (arg == "--keepalive-reuse") cfg.keepalive_max_reuse = std::stoi(next());
        else if (arg == "--timeout") {
            std::string val = next();
            if (!val.empty() && (val.back() == 's' || val.back() == 'S')) val.pop_back();
            cfg.timeout_sec = std::stoi(val);
        } else if (arg == "--warmup") cfg.warmup = std::stoi(next());
        else if (arg == "--json") cfg.json_output = next();
        else if (arg == "--histogram") cfg.histogram = true;
        else {
            std::cerr << "  错误: 未知选项 " << arg << "\n";
            std::cerr << "  使用 --help 查看帮助信息\n";
            return 1;
        }
    }

    // ============================================================
    // 预热阶段
    // ============================================================
    int effective_requests = cfg.total_requests;
    if (cfg.duration_sec == 0 && cfg.warmup > 0) {
        effective_requests += cfg.warmup;
    }

    std::cout << "========================================\n";
    std::cout << "  HttpServer2 压力测试工具 v2.1\n";
    std::cout << "========================================\n";
    std::cout << "  目标:     " << cfg.host << ":" << cfg.port << cfg.endpoint << "\n";
    std::cout << "  方法:     " << cfg.method << "\n";
    std::cout << "  并发:     " << cfg.concurrency << "\n";
    if (cfg.duration_sec > 0) {
        std::cout << "  时长:     " << cfg.duration_sec << " s\n";
    } else {
        std::cout << "  总请求:   " << effective_requests;
        if (cfg.warmup > 0) std::cout << " (含预热 " << cfg.warmup << ")";
        std::cout << "\n";
    }
    std::cout << "  连接:     " << (cfg.keep_alive ? "Keep-Alive" : "短连接") << "\n";
    if (cfg.ramp_up_sec > 0) std::cout << "  渐进:     " << cfg.ramp_up_sec << " s\n";
    std::cout << "----------------------------------------\n";

    // ============================================================
    // 预热
    // ============================================================
    if (cfg.warmup > 0 && cfg.duration_sec == 0) {
        std::cout << "  预热中 (" << cfg.warmup << " 请求) ..." << std::flush;
        StressStats warmup_stats;
        std::atomic<int> warmup_remaining(cfg.warmup);
        std::atomic<int> dummy_timer(0);
        int warmup_concurrency = std::min(cfg.concurrency, cfg.warmup);
        std::vector<std::thread> warmup_threads;
        for (int i = 0; i < warmup_concurrency; ++i) {
            warmup_threads.emplace_back(worker, std::ref(cfg), std::ref(warmup_stats),
                                         std::ref(warmup_remaining), std::ref(dummy_timer), i);
        }
        for (auto& t : warmup_threads) t.join();
        std::cout << " 完成 (" << warmup_stats.ok.load() << " OK)\n";
    }

    // ============================================================
    // 主测试
    // ============================================================
    StressStats stats;
    std::atomic<int> remaining_req(cfg.duration_sec > 0 ? 0 : cfg.total_requests);

    // 时长模式：持续更新 remaining_time_ms
    std::atomic<int> remaining_time_ms(cfg.duration_sec * 10); // 以 100ms 为单位
    std::thread timer_thread;
    if (cfg.duration_sec > 0) {
        timer_thread = std::thread([&]() {
            for (int i = 0; i < cfg.duration_sec * 10 && !g_should_exit; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                remaining_time_ms.fetch_sub(1);
            }
        });
    }

    // 创建工作线程
    std::vector<std::thread> threads;
    stats.start_time = std::chrono::steady_clock::now();

    if (cfg.ramp_up_sec > 0) {
        // 渐进模式：先起一个线程，逐步增加
        for (int i = 0; i < cfg.concurrency; ++i) {
            if (g_should_exit) break;
            threads.emplace_back(worker, std::ref(cfg), std::ref(stats),
                                 std::ref(remaining_req), std::ref(remaining_time_ms), i);
            if (cfg.ramp_up_sec > 0 && i < cfg.concurrency - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg.ramp_up_sec * 1000 / cfg.concurrency));
            }
        }
    } else {
        for (int i = 0; i < cfg.concurrency; ++i) {
            threads.emplace_back(worker, std::ref(cfg), std::ref(stats),
                                 std::ref(remaining_req), std::ref(remaining_time_ms), i);
        }
    }

    // 进度显示
    bool duration_mode = (cfg.duration_sec > 0);
    int total_work = duration_mode ? cfg.duration_sec * 10 : effective_requests;

    if (duration_mode) {
        while (remaining_time_ms.load() > 0 && !g_should_exit) {
            int remaining = remaining_time_ms.load();
            int done = total_work - remaining;
            int pct = done * 100 / std::max(total_work, 1);
            double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stats.start_time).count() / 1000.0;
            int cur_rps = (elapsed > 0) ? static_cast<int>(stats.ok.load() / elapsed) : 0;
            std::cout << "\r  进度: " << (done / 10) << "s/" << cfg.duration_sec << "s"
                      << " (" << pct << "%) | OK=" << stats.ok.load()
                      << " FAIL=" << stats.failed.load()
                      << " RPS=" << cur_rps
                      << "  \"" << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    } else {
        while (remaining_req.load() > 0 && !g_should_exit) {
            int remaining = remaining_req.load();
            int done = total_work - remaining;
            int pct = done * 100 / std::max(total_work, 1);
            double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stats.start_time).count() / 1000.0;
            int cur_rps = (elapsed > 0) ? static_cast<int>(stats.ok.load() / elapsed) : 0;
            std::cout << "\r  进度: " << done << "/" << total_work
                      << " (" << pct << "%) | OK=" << stats.ok.load()
                      << " FAIL=" << stats.failed.load()
                      << " RPS=" << cur_rps << "  \"" << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // 等待所有线程完成
    if (timer_thread.joinable()) timer_thread.join();
    for (auto& t : threads) t.join();

    stats.end_time = std::chrono::steady_clock::now();

    // ============================================================
    // 输出结果
    // ============================================================
    stats.report(cfg);

    // JSON 输出
    writeJson(stats, cfg);

    // 收到信号提示
    if (g_should_exit) {
        std::cout << "  (用户中断，以上为已收集到的部分结果)\n";
    }

    return stats.failed.load() > (stats.sent.load() / 2) ? 1 : 0;
}
