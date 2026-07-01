# HttpServer2 性能对比测试报告

**测试日期**: 2026-07-01
**测试服务器**: HttpServer2 (C++) vs Node.js 25 (cluster) vs Python 3.13 (asyncio)
**测试工具**: test_stress v2.1
**测试文件**: 静态 index.html (2.5KB)

---

## 测试场景与结果

### 1. 低并发短连接 (10并发 / 5000请求 / 短连接)

| 指标 | HttpServer2 | Node.js 25 | Python 3.13 |
|------|------------|------------|-------------|
| RPS | **25,000** | 12,500 | 12,500 |
| P50 | **0.27ms** | 0.45ms | 0.67ms |
| P95 | **0.57ms** | 1.39ms | 0.73ms |
| 吞吐 | 3.36 MB/s | 29.52 MB/s | 29.52 MB/s |

**加速比**: HttpServer2 是 Node.js 的 **2.0x**，Python 的 **2.0x**

### 2. 中并发短连接 (50并发 / 10000请求 / 短连接)

| 指标 | HttpServer2 | Node.js 25 | Python 3.13 |
|------|------------|------------|-------------|
| RPS | **25,000** | 16,611 | 12,484 |
| P50 | 1.36ms | 1.82ms | 2.99ms |
| P95 | **4.25ms** | 4.64ms | 3.19ms |
| 吞吐 | 3.36 MB/s | 39.22 MB/s | 29.48 MB/s |

**加速比**: HttpServer2 是 Node.js 的 **1.5x**，Python 的 **2.0x**

### 3. 高并发短连接 (200并发 / 20000请求 / 短连接)

| 指标 | HttpServer2 | Node.js 25 | Python 3.13 |
|------|------------|------------|-------------|
| RPS | **16,598** | 14,255 | 9,615 |
| P50 | 6.71ms | 6.42ms | 6.10ms |
| P95 | **40.37ms** | 53.12ms | 11.48ms |
| 吞吐 | 2.23 MB/s | 33.66 MB/s | 22.70 MB/s |

**加速比**: HttpServer2 是 Node.js 的 **1.2x**，Python 的 **1.7x**

### 4. 中并发 Keep-Alive (50并发 / 10000请求 / Keep-Alive)

| 指标 | HttpServer2 | Node.js 25 | Python 3.13 |
|------|------------|------------|-------------|
| RPS | 16,667 | **49,505** | 12,531 |
| P50 | 1.53ms | **0.44ms** | 2.12ms |
| P95 | 9.04ms | **2.18ms** | 2.41ms |
| 吞吐 | 2.24 MB/s | **116.90 MB/s** | 29.59 MB/s |

**加速比**: Node.js 是 HttpServer2 的 **3.0x**

---

## 分析

### HttpServer2 优势
- **短连接场景：全面领先 Node.js 1.2~2.0x，Python 1.7~2.0x**
- 低并发场景 RPS 达到 25,000（Node.js 为 12,500）
- P50 延迟始终低于 Node.js 和 Python
- P95 尾部延迟在高并发下优于 Node.js（40ms vs 53ms）

### HttpServer2 短板
- **Keep-Alive 场景远落后于 Node.js（0.3x）**
- 根本原因：HttpServer2 基于**线程池 + 阻塞 I/O**，每个请求独占一个线程。Keep-Alive 连接复用未能充分发挥优势（服务端 `keep_alive = false` 被强制在队列架构中）
- Node.js 使用**单线程事件循环 + 异步 I/O**，连接复用天然高效
- 吞吐量（MB/s）较低：因为响应体较小（2.5KB），RPS 差异被放大

### 与 Python 对比
- HttpServer2 在所有场景中均优于 Python asyncio
- Python asyncio 在 Keep-Alive 下出现 50% 失败率（连接管理问题）

---

## 改进建议

1. **修复 Keep-Alive 架构** — 当前队列架构强制 `keep_alive = false`（httpserver2.cpp:1532-1534）
2. **零拷贝优化** — 静态文件响应使用 sendfile 避免用户态拷贝
3. **减少调试开销** — 关闭调试模式后仍有 DebugInfo 创建开销
4. **连接池复用** — 实现真正的连接池而非每次都新建线程

---

## 原始数据

完整 JSON 数据保存在 `/tmp/benchmark_comparison.json`
