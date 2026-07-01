/*
 * HttpServer2 压力测试工具
 *
 * 使用原始 TCP socket 进行高性能并发测试，无外部依赖。
 * 支持多并发级别、多端点、延迟分布统计。
 *
 * 用法:
 *   ./test_stress [选项]
 *     --host HOST      目标主机 (默认 localhost)
 *     --port PORT      目标端口 (默认 8080)
 *     --concurrency N  并发连接数 (默认 100)
 *     --requests N     总请求数 (默认 10000)
 *     --endpoint PATH  测试路径 (默认 /)
 *     --help           帮助信息
 */

#include "httpserver2.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cmath>

// ============================================================
// 配置
// ============================================================
struct StressConfig {
    std::string host = "localhost";
    int port = 8080;
    int concurrency = 100;
    int total_requests = 10000;
    std::string endpoint = "/";
    int timeout_ms = 5000;
};

// ============================================================
// 统计
// ============================================================
struct StressStats {
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> bytes{0};
    std::vector<double> latencies;
    std::mutex lat_mutex;

    void addLatency(double ms) {
        std::lock_guard<std::mutex> lock(lat_mutex);
        latencies.push_back(ms);
    }

    void report() const {
        if (latencies.empty()) return;
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

        std::cout << "\n========================================\n";
        std::cout << "  压力测试结果\n";
        std::cout << "========================================\n";
        std::cout << "  总请求: " << sent.load() << "\n";
        std::cout << "  成功:   " << ok.load() << "\n";
        std::cout << "  失败:   " << failed.load() << "\n";
        std::cout << "  成功率: " << std::fixed << std::setprecision(1)
                  << (ok.load() * 100.0 / sent.load()) << "%\n";
        std::cout << "  总字节: " << bytes.load() << "\n";
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
        std::cout << "----------------------------------------\n";
    }
};

// ============================================================
// 发送单个 HTTP 请求并测量延迟
// ============================================================
static bool doRequest(const StressConfig& cfg, StressStats& stats) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(cfg.host, std::to_string(cfg.port));
        tcp::socket sock(ioc);
        boost::asio::connect(sock, endpoints);

        // 构造 HTTP 请求
        std::stringstream req_ss;
        req_ss << "GET " << cfg.endpoint << " HTTP/1.1\r\n"
               << "Host: " << cfg.host << ":" << cfg.port << "\r\n"
               << "Connection: close\r\n"
               << "User-Agent: StressTest/1.0\r\n"
               << "\r\n";
        std::string request = req_ss.str();

        auto start = std::chrono::steady_clock::now();

        // 发送请求
        boost::asio::write(sock, boost::asio::buffer(request));

        // 接收响应
        boost::system::error_code ec;
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::string_body> response;
        http::read(sock, buffer, response, ec);

        auto end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

        stats.sent++;
        if (!ec && response.result() == http::status::ok) {
            stats.ok++;
            stats.bytes += response.body().size();
            stats.addLatency(elapsed_ms);
        } else {
            stats.failed++;
        }

        boost::system::error_code close_ec;
        sock.shutdown(tcp::socket::shutdown_both, close_ec);
        sock.close(close_ec);
        return true;
    } catch (...) {
        stats.sent++;
        stats.failed++;
        return false;
    }
}

// ============================================================
// 工作线程
// ============================================================
static void worker(const StressConfig& cfg, StressStats& stats, std::atomic<int>& remaining) {
    while (true) {
        int cur = remaining.fetch_sub(1);
        if (cur <= 0) break;
        doRequest(cfg, stats);
    }
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char* argv[]) {
    StressConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [选项]\n";
            std::cout << "  --host HOST          目标主机 (默认 localhost)\n";
            std::cout << "  --port PORT          目标端口 (默认 8080)\n";
            std::cout << "  --concurrency N      并发连接数 (默认 100)\n";
            std::cout << "  --requests N         总请求数 (默认 10000)\n";
            std::cout << "  --endpoint PATH      测试路径 (默认 /)\n";
            return 0;
        } else if (arg == "--host" && i + 1 < argc) cfg.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) cfg.port = std::stoi(argv[++i]);
        else if (arg == "--concurrency" && i + 1 < argc) cfg.concurrency = std::stoi(argv[++i]);
        else if (arg == "--requests" && i + 1 < argc) cfg.total_requests = std::stoi(argv[++i]);
        else if (arg == "--endpoint" && i + 1 < argc) cfg.endpoint = argv[++i];
    }

    std::cout << "========================================\n";
    std::cout << "  HttpServer2 压力测试\n";
    std::cout << "========================================\n";
    std::cout << "  目标:     " << cfg.host << ":" << cfg.port << cfg.endpoint << "\n";
    std::cout << "  并发:     " << cfg.concurrency << "\n";
    std::cout << "  总请求:   " << cfg.total_requests << "\n";
    std::cout << "----------------------------------------\n";

    StressStats stats;
    std::atomic<int> remaining(cfg.total_requests);
    std::vector<std::thread> threads;

    auto start_total = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.concurrency; ++i) {
        threads.emplace_back(worker, std::ref(cfg), std::ref(stats), std::ref(remaining));
    }

    // 进度显示
    while (remaining.load() > 0) {
        int done = cfg.total_requests - remaining.load();
        int pct = done * 100 / cfg.total_requests;
        std::cout << "\r  进度: " << done << "/" << cfg.total_requests
                  << " (" << pct << "%)  \"" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    for (auto& t : threads) t.join();

    auto end_total = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration_cast<std::chrono::milliseconds>(end_total - start_total).count() / 1000.0;

    stats.report();
    std::cout << "  总耗时: " << std::fixed << std::setprecision(2) << total_sec << " s\n";
    std::cout << "  RPS:    " << std::fixed << std::setprecision(0) << (stats.ok.load() / total_sec) << "\n";
    std::cout << "========================================\n";

    return stats.failed.load() > (stats.sent.load() / 2) ? 1 : 0;
}
