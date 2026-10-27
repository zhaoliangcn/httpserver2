---
name: project-conventions
description: HttpServer2 项目约定和架构知识，帮助 AtomCode 更好地理解和维护代码
user_invocable: false
disable_model_invocation: false
---

# HttpServer2 — 项目约定

## 技术栈
- **语言**: C++17
- **HTTP/HTTPS**: Boost.Asio + OpenSSL 3.0
- **配置**: PipeYAML (YAML 1.2 格式，替代 yaml-cpp)
- **构建**: Makefile + CMake
- **编译**: g++ -std=c++17 -O2

## 目录结构
```
testhttpserver2/
  httpserver2.cpp / .h   — 核心 HTTP 服务器实现（3724 行）
  main.cpp                — 入口，路由和配置（584 行）
  config.yaml             — 服务器配置（YAML）
  public/                 — Web 静态资源（HTML/CSS/JS）
  static/                 — 测试静态文件
  test_*.py / .cpp / .sh — 测试脚本
  *.yaml                  — 测试配置文件
  *.log                   — 日志文件
```

## 架构规则
1. **核心类**: `HttpServer2` 在 `httpserver2.h` 中声明，`httpserver2.cpp` 中实现
2. **配置**: 使用 YAML 格式（PipeYAML 解析），通过 `config.yaml` 加载
3. **安全**: 支持基本认证、CSRF 保护、请求大小限制、TLS/SSL
4. **功能**: HTTP/HTTPS 服务、WebSocket、反向代理、内容压缩、缓存、MIME 类型
5. **日志**: 写入 `server.log`、`access.log`、`error.log`
6. **端口**: 默认 8080（HTTP）和 8443（HTTPS）
7. **SSL**: 使用 `server.crt` / `server.key`

## 代码风格
- C++17 标准，Boost 命名规范
- 头文件使用 `#pragma once`
- 错误处理使用异常 + 日志（macOS 和 Linux 兼容）
- 全局配置通过 `config.yaml` 管理
