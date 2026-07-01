---
name: build-project
description: 一键编译和运行 HttpServer2（C++17 HTTP 服务器）
user_invocable: true
disable_model_invocation: true
arguments:
  target:
    type: string
    description: 构建目标（server / concurrent / proxy / all / clean）
    required: true
  mode:
    type: string
    description: 构建系统（make / cmake）
    default: make
---

# build-project

编译和运行 HttpServer2 项目。

## 构建目标

| 目标 | 说明 | 命令 |
|------|------|------|
| `server` | 主服务器 httpserver2 | `make httpserver2` |
| `concurrent` | 并发测试 | `make test_concurrent` |
| `proxy` | 代理测试 | `make test_proxy` |
| `all` | 全部编译 | `make full` |
| `clean` | 清理 | `make clean` |

## 用法

- `make httpserver2` — 编译主服务器
- `make full` — 编译所有目标
- `cmake . && make` — 使用 CMake 构建
- `./httpserver2` — 运行服务器

## 注意
- 如果使用 CMake，先运行 `cmake .` 生成 Makefile
- 编译产物在项目根目录
