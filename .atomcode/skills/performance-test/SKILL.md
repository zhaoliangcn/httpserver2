---
name: performance-test
description: 运行和记录 HttpServer2 性能基准测试
user_invocable: true
disable_model_invocation: true
arguments:
  type:
    type: string
    description: 测试类型（single / concurrency / static / all）
    required: false
    default: all
  concurrency:
    type: number
    description: 并发连接数
    required: false
    default: 100
---

# performance-test

运行和记录 HttpServer2 性能基准测试。

## 可用测试

| 类型 | 命令 | 输出文件 |
|------|------|----------|
| `single` | 单线程性能 | `performance_single.json` |
| `concurrency` | 并发性能 | `performance_{N}.json` |
| `static` | 静态文件性能 | `performance_static.json` |
| `all` | 全部运行 | 多个 `performance_*.json` |

## 用法

- `performance-test` — 运行全部性能测试
- `performance-test single` — 仅运行单线程测试
- `performance-test concurrency 200` — 200 并发测试
- `performance-test static` — 静态文件服务性能测试

## 测试流程

1. 编译服务器（如需要）：`make httpserver2`
2. 启动服务器：`./httpserver2 &`
3. 运行性能测试：`python3 performance_test.py [args]`
4. 收集结果到 `performance_results.json`
5. 停止服务器

## 结果参考（历史记录）

参考 `performance_results.json` 和 `performance_*.json` 中的历史数据，与当前结果对比，评估性能变化。

## 注意事项
- 测试前确保服务器未运行
- 测试结果自动附加时间戳，避免覆盖历史数据
- 使用 `performance_results_current.json` 作为当前运行的临时输出
