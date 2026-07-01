// stress_backend.h — 压力测试 I/O 后端抽象层
//
// 通过 #define USE_HP_ASYNC_IO 切换后端：
//   默认（无定义）→ Boost.Asio + Boost.Beast
//   定义宏         → hp-async-io
//
// 提供统一的：
//   backend::Context     — I/O 上下文
//   backend::TCPSocket   — TCP 套接字
//   backend::connect()   — 连接到 host:port
//   backend::send()      — 发送数据
//   backend::recv_http() — 接收 HTTP 响应（解析状态码 + 体）
//   backend::close()     — 关闭连接

#pragma once

#include <string>
#include <cstddef>

#ifdef USE_HP_ASYNC_IO
#  include "backend_hp_async_io.h"
#else
#  include "backend_boost_asio.h"
#endif
