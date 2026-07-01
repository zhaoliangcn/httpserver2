---
name: gen-test
description: 为 HttpServer2 生成测试代码（C++ 单元测试 / Python 功能测试）
user_invocable: true
disable_model_invocation: true
arguments:
  type:
    type: string
    description: 测试类型（cpp / python / sh）
    required: false
    default: python
  target:
    type: string
    description: 测试目标功能（如 proxy / caching / auth / mime / compression / error-handling）
    required: false
---

# gen-test

为 HttpServer2 项目生成测试代码。

## 用法

- `gen-test python` — 生成 Python 功能测试脚本
- `gen-test cpp` — 生成 C++ 并发测试程序
- `gen-test python proxy` — 针对反向代理功能生成 Python 测试
- `gen-test python caching` — 针对 HTTP 缓存功能生成测试

## 模板参考

### Python 测试模板

```python
"""
HttpServer2 [功能] 测试

测试目标：[功能描述]
"""

import http.client
import json
import sys

HOST = "localhost"
PORT = 8080

def test_[feature]_[scenario]():
    """测试场景描述"""
    conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
    try:
        conn.request("GET", "/[path]")
        response = conn.getresponse()
        data = response.read()
        assert response.status == 200, f"期望 200, 得到 {response.status}"
        print(f"✅ [测试名称] 通过")
        return True
    except Exception as e:
        print(f"❌ [测试名称] 失败: {e}")
        return False
    finally:
        conn.close()

if __name__ == "__main__":
    tests = [test_[feature]_[scenario]]
    results = [t() for t in tests]
    sys.exit(0 if all(results) else 1)
```

### C++ 测试模板

```cpp
#include <iostream>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

bool test_[scenario]() {
    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);
        
        auto endpoints = resolver.resolve("localhost", "8080");
        boost::asio::connect(socket, endpoints);
        
        // 发送请求
        std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(request));
        
        // 读取响应
        boost::asio::streambuf response;
        boost::asio::read_until(socket, response, "\r\n\r\n");
        
        std::cout << "✅ [测试名称] 通过" << std::endl;
        return true;
    } catch (std::exception& e) {
        std::cerr << "❌ [测试名称] 失败: " << e.what() << std::endl;
        return false;
    }
}

int main() {
    bool result = test_[scenario]();
    return result ? 0 : 1;
}
```

## 注意事项
- Python 测试使用 `http.client`（标准库，无需额外安装）
- C++ 测试使用 Boost.Asio
- 测试运行：`python3 test_gen_*.py` 或 `make test_[name] && ./test_[name]`
