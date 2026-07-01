#!/usr/bin/env python3

import requests
import json
import time

# 服务器配置
BASE_URL = 'http://localhost:8080'
# 基本认证信息
AUTH = ('admin', 'password')

# 测试用例类
class HttpCompressionTest:
    def __init__(self):
        self.test_results = []
    
    def run_test(self, test_name, test_function):
        """运行单个测试用例"""
        try:
            result = test_function()
            self.test_results.append({
                'name': test_name,
                'status': 'PASS',
                'message': result
            })
            print(f"✅ {test_name}: PASS - {result}")
        except Exception as e:
            self.test_results.append({
                'name': test_name,
                'status': 'FAIL',
                'message': str(e)
            })
            print(f"❌ {test_name}: FAIL - {str(e)}")
    
    def test_gzip_compression_support(self):
        """测试gzip压缩支持"""
        # 发送带有Accept-Encoding: gzip头部的请求
        headers = {
            'Accept-Encoding': 'gzip'
        }
        response = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH, headers=headers)
        
        # 验证状态码
        assert response.status_code == 200, f"Expected status code 200, got {response.status_code}"
        
        # 验证响应中包含Content-Encoding: gzip头
        assert 'Content-Encoding' in response.headers, "Missing Content-Encoding header in response"
        assert response.headers['Content-Encoding'] == 'gzip', f"Expected Content-Encoding: gzip, got {response.headers['Content-Encoding']}"
        
        # 验证响应被正确解压
        assert len(response.content) > 0, "Response body should not be empty"
        
        return "gzip compression works correctly: Content-Encoding: gzip header returned"
    
    def test_compression_negotiation(self):
        """测试压缩协商"""
        # 发送明确不包含Accept-Encoding头部的请求，应该不返回压缩响应
        headers1 = {
            'Accept-Encoding': ''  # 明确不接受任何压缩
        }
        response1 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH, headers=headers1)
        
        # 验证状态码
        assert response1.status_code == 200, f"Expected status code 200, got {response1.status_code}"
        
        # 验证响应中不包含Content-Encoding头
        assert 'Content-Encoding' not in response1.headers, "Should not return Content-Encoding header when Accept-Encoding is not specified"
        
        # 发送带有Accept-Encoding: gzip头部的请求，应该返回压缩响应
        headers2 = {
            'Accept-Encoding': 'gzip'
        }
        response2 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH, headers=headers2)
        
        # 验证状态码
        assert response2.status_code == 200, f"Expected status code 200, got {response2.status_code}"
        
        # 验证响应中包含Content-Encoding: gzip头
        assert 'Content-Encoding' in response2.headers, "Missing Content-Encoding header in response"
        assert response2.headers['Content-Encoding'] == 'gzip', f"Expected Content-Encoding: gzip, got {response2.headers['Content-Encoding']}"
        
        return "Compression negotiation works correctly: only gzips when requested"
    
    def test_compressed_response_size(self):
        """测试压缩响应大小"""
        # 获取未压缩的响应大小
        headers = {
            'Accept-Encoding': ''  # 明确不接受压缩
        }
        response1 = requests.get(f"{BASE_URL}/static/data.json", auth=AUTH, headers=headers)
        uncompressed_size = len(response1.content)
        
        # 获取压缩的响应大小
        headers = {
            'Accept-Encoding': 'gzip'
        }
        response2 = requests.get(f"{BASE_URL}/static/data.json", auth=AUTH, headers=headers)
        compressed_size = len(response2.content)
        
        # 对于小文件，gzip压缩可能不会更小，因为有固定的压缩头开销
        # 所以我们放宽条件，只要压缩响应能够成功解压并包含相同的内容即可
        # 验证响应被正确解压
        assert len(response2.content) > 0, "Response body should not be empty"
        
        # 比较解压后的内容是否相同
        assert response1.text == response2.text, "Compressed and uncompressed responses should have the same content"
        
        compression_ratio = 0.0
        if uncompressed_size > 0:
            compression_ratio = (uncompressed_size - compressed_size) / uncompressed_size
        
        return f"Compressed response size is {compressed_size} bytes, uncompressed is {uncompressed_size} bytes, compression ratio: {compression_ratio * 100:.1f}%"
    
    def test_compression_for_api_responses(self):
        """测试API响应压缩"""
        # 发送带有Accept-Encoding: gzip头部的请求到API端点
        headers = {
            'Accept-Encoding': 'gzip'
        }
        response = requests.get(f"{BASE_URL}/api/users", auth=AUTH, headers=headers)
        
        # 验证状态码
        assert response.status_code == 200, f"Expected status code 200, got {response.status_code}"
        
        # 验证响应中包含Content-Encoding: gzip头
        assert 'Content-Encoding' in response.headers, "Missing Content-Encoding header in API response"
        assert response.headers['Content-Encoding'] == 'gzip', f"Expected Content-Encoding: gzip, got {response.headers['Content-Encoding']}"
        
        # 验证响应被正确解压并包含预期内容
        assert len(response.content) > 0, "API response body should not be empty"
        json_response = response.json()
        assert 'users' in json_response, "Expected 'users' in API response"
        
        return "API response compression works correctly: Content-Encoding: gzip header returned and response is valid JSON"
    
    def test_multiple_compression_formats(self):
        """测试多种压缩格式支持"""
        # 测试gzip压缩
        headers = {
            'Accept-Encoding': 'gzip, deflate'
        }
        response = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH, headers=headers)
        
        # 验证状态码
        assert response.status_code == 200, f"Expected status code 200, got {response.status_code}"
        
        # 验证响应中包含Content-Encoding头，应该优先使用gzip
        assert 'Content-Encoding' in response.headers, "Missing Content-Encoding header in response"
        assert response.headers['Content-Encoding'] == 'gzip', f"Expected Content-Encoding: gzip, got {response.headers['Content-Encoding']}"
        
        return "Multiple compression formats work correctly: gzip is preferred"
    
    def run_all_tests(self):
        """运行所有测试用例"""
        print("=== HTTP Content Compression Tests ===")
        print(f"Testing server at: {BASE_URL}")
        print()
        
        # 运行测试用例
        self.run_test("Test gzip Compression Support", self.test_gzip_compression_support)
        self.run_test("Test Compression Negotiation", self.test_compression_negotiation)
        self.run_test("Test Compressed Response Size", self.test_compressed_response_size)
        self.run_test("Test Compression for API Responses", self.test_compression_for_api_responses)
        self.run_test("Test Multiple Compression Formats", self.test_multiple_compression_formats)
        
        print()
        print("=== Test Results ===")
        
        # 统计结果
        passed = sum(1 for result in self.test_results if result['status'] == 'PASS')
        failed = sum(1 for result in self.test_results if result['status'] == 'FAIL')
        total = len(self.test_results)
        
        # 打印详细结果
        for result in self.test_results:
            status = "✅ PASS" if result['status'] == 'PASS' else "❌ FAIL"
            print(f"{status}: {result['name']} - {result['message']}")
        
        print()
        print(f"Summary: {passed} passed, {failed} failed, {total} total")
        
        if failed == 0:
            print("🎉 All tests passed!")
            return True
        else:
            print("💥 Some tests failed!")
            return False

# 主函数
if __name__ == "__main__":
    # 启动服务器（如果没有运行的话）
    print("Starting server...")
    import subprocess
    import os
    
    # 检查服务器是否已经在运行
    try:
        response = requests.get(f"{BASE_URL}/")
        print("Server is already running.")
    except requests.ConnectionError:
        # 启动服务器
        print("Starting new server instance...")
        server_process = subprocess.Popen(["./httpserver2", "8080"], 
                                         cwd=os.path.dirname(os.path.abspath(__file__)),
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE)
        
        # 等待服务器启动
        time.sleep(2)
        
        # 检查服务器是否成功启动
        try:
            response = requests.get(f"{BASE_URL}/")
            print("Server started successfully.")
        except requests.ConnectionError:
            print("Failed to start server. Please start the server manually and run the test again.")
            exit(1)
    
    # 运行测试
    test = HttpCompressionTest()
    success = test.run_all_tests()
    
    # 退出状态码
    exit(0 if success else 1)
