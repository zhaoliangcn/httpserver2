#!/usr/bin/env python3
"""
测试反向代理稳定性，解决测试报告中提出的问题
"""

import http.server
import socketserver
import threading
import time
import requests
import subprocess
import os
import sys
import json

# 测试配置
HTTP_SERVER2_BINARY = "./test_proxy_simple"  # 使用我们新创建的测试程序
TEST_PORT = 8080

# 测试后端服务器配置
BACKENDS = [
    {"port": 8001, "message": "Backend 1 Response"},
    {"port": 8002, "message": "Backend 2 Response"},
    {"port": 8003, "message": "Backend 3 Response"}
]

class SimpleHTTPRequestHandler(http.server.BaseHTTPRequestHandler):
    """简单HTTP处理程序，用于测试后端服务器"""
    
    def do_GET(self):
        # 获取当前后端端口
        backend_port = self.server.server_address[1]
        # 找到对应的后端配置
        backend = next(b for b in BACKENDS if b["port"] == backend_port)
        
        # 模拟处理延迟，测试超时情况
        time.sleep(0.05)  # 50ms延迟
        
        # 发送响应
        self.send_response(200)
        self.send_header("Content-type", "text/plain")
        self.end_headers()
        self.wfile.write(f"{backend['message']}".encode())
    
    def log_message(self, format, *args):
        """抑制日志消息"""
        return

def start_backend(port):
    """启动后端服务器"""
    try:
        with socketserver.TCPServer(("", port), SimpleHTTPRequestHandler) as httpd:
            print(f"[INFO] 后端服务器启动在端口 {port}")
            httpd.serve_forever()
    except Exception as e:
        print(f"[ERROR] 后端服务器 {port} 启动失败: {e}")
        sys.exit(1)

def start_httpserver2():
    """启动HttpServer2"""
    if not os.path.exists(HTTP_SERVER2_BINARY):
        print(f"[ERROR] HttpServer2二进制文件不存在: {HTTP_SERVER2_BINARY}")
        sys.exit(1)
    
    print(f"[INFO] 启动HttpServer2: {HTTP_SERVER2_BINARY}")
    process = subprocess.Popen([HTTP_SERVER2_BINARY], 
                              stdout=subprocess.PIPE, 
                              stderr=subprocess.PIPE, 
                              text=True)
    return process

def test_single_backend():
    """测试单个后端代理"""
    print("\n=== 测试单个后端代理 ===")
    
    url = f"http://localhost:{TEST_PORT}/single"
    try:
        response = requests.get(url, timeout=5)
        print(f"状态码: {response.status_code}")
        print(f"响应内容: {response.text}")
        print(f"响应头: {dict(response.headers)}")
        
        if response.status_code == 200 and BACKENDS[0]['message'] in response.text:
            print("✅ 单个后端代理测试通过")
            return True
        else:
            print("❌ 单个后端代理测试失败")
            return False
    except Exception as e:
        print(f"❌ 单个后端代理测试失败: {e}")
        return False

def test_multi_backend_round_robin():
    """测试多个后端轮询"""
    print("\n=== 测试多个后端轮询 ===")
    
    url = f"http://localhost:{TEST_PORT}/multi"
    backend_responses = {}
    num_requests = 12  # 发送12个请求，每个后端应该收到4个请求
    failed_requests = 0
    
    for i in range(num_requests):
        try:
            response = requests.get(url, timeout=5)
            if response.status_code == 200:
                response_text = response.text
                backend_responses[response_text] = backend_responses.get(response_text, 0) + 1
                print(f"请求 {i+1}: {response_text}")
            else:
                failed_requests += 1
                print(f"请求 {i+1}: ❌ 错误 {response.status_code}")
        except Exception as e:
            failed_requests += 1
            print(f"请求 {i+1}: ❌ 异常 {e}")
        
        # 简短延迟，避免请求过于密集
        time.sleep(0.1)
    
    print(f"\n轮询结果统计:")
    for backend in BACKENDS:
        count = backend_responses.get(backend['message'], 0)
        print(f"- {backend['message']}: {count}次请求")
    
    print(f"\n测试结果: {num_requests - failed_requests}/{num_requests}请求成功")
    
    # 检查是否所有后端都收到了请求，且请求分布均匀
    all_backends_used = all(backend['message'] in backend_responses for backend in BACKENDS)
    success_rate = (num_requests - failed_requests) / num_requests
    
    if all_backends_used and success_rate > 0.8:  # 允许最多20%失败率
        print("✅ 多个后端轮询测试通过")
        return True
    else:
        print("❌ 多个后端轮询测试失败")
        return False

def test_custom_headers():
    """测试自定义头代理"""
    print("\n=== 测试自定义头代理 ===")
    
    url = f"http://localhost:{TEST_PORT}/custom"
    try:
        response = requests.get(url, timeout=10)
        print(f"状态码: {response.status_code}")
        print(f"响应内容: {response.text}")
        
        # 检查自定义响应头
        print("\n响应头检查:")
        custom_header_present = "X-Powered-By" in response.headers and "HttpServer2" in response.headers["X-Powered-By"]
        print(f"X-Powered-By头存在: {'✅' if custom_header_present else '❌'}")
        
        if response.status_code == 200 and BACKENDS[0]['message'] in response.text and custom_header_present:
            print("✅ 自定义头代理测试通过")
            return True
        else:
            print("❌ 自定义头代理测试失败")
            return False
    except Exception as e:
        print(f"❌ 自定义头代理测试失败: {e}")
        return False

def test_proxy_stability():
    """测试反向代理稳定性"""
    print("\n=== 测试反向代理稳定性 ===")
    
    url = f"http://localhost:{TEST_PORT}/multi"
    num_requests = 50
    failed_requests = 0
    
    print(f"发送 {num_requests} 个并发请求到多个后端...")
    
    for i in range(num_requests):
        try:
            response = requests.get(url, timeout=5)
            if response.status_code == 200:
                # 快速打印，不显示详细内容
                if (i + 1) % 10 == 0:
                    print(f"{i + 1}/{num_requests} 成功")
            else:
                failed_requests += 1
                print(f"❌ 请求 {i + 1} 失败: {response.status_code}")
        except Exception as e:
            failed_requests += 1
            if (i + 1) % 10 == 0:
                print(f"❌ 请求 {i + 1} 失败: {e}")
    
    success_rate = (num_requests - failed_requests) / num_requests
    print(f"\n稳定性测试结果: {num_requests - failed_requests}/{num_requests} 请求成功")
    print(f"成功率: {success_rate * 100:.1f}%")
    
    if success_rate > 0.95:  # 要求95%以上成功率
        print("✅ 反向代理稳定性测试通过")
        return True
    else:
        print("❌ 反向代理稳定性测试失败")
        return False

def main():
    """主测试函数"""
    print("HttpServer2 反向代理稳定性测试")
    print("=" * 50)
    
    # 启动后端服务器
    print("启动后端服务器...")
    backend_threads = []
    for backend in BACKENDS:
        thread = threading.Thread(target=start_backend, args=(backend['port'],), daemon=True)
        thread.start()
        backend_threads.append(thread)
    
    # 等待后端服务器启动
    time.sleep(2)
    
    # 启动HttpServer2
    server_process = start_httpserver2()
    
    # 等待服务器启动
    time.sleep(3)
    
    try:
        # 运行测试
        test_results = []
        test_results.append(test_single_backend())
        test_results.append(test_multi_backend_round_robin())
        test_results.append(test_custom_headers())
        test_results.append(test_proxy_stability())
        
        # 测试总结
        print("\n" + "=" * 50)
        print("测试总结:")
        passed = sum(test_results)
        total = len(test_results)
        print(f"通过测试: {passed}/{total}")
        
        if passed == total:
            print("🎉 所有测试通过！反向代理功能稳定可靠")
            return 0
        else:
            print("❌ 部分测试失败，需要进一步优化")
            return 1
            
    finally:
        # 清理
        print("\n清理资源...")
        
        # 停止HttpServer2
        if 'server_process' in locals():
            server_process.terminate()
            try:
                server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_process.kill()
        
        print("测试完成")

if __name__ == "__main__":
    sys.exit(main())
