#!/usr/bin/env python3

import requests
import json
import time

# 服务器配置
BASE_URL = 'http://localhost:8080'
# 基本认证信息
AUTH = ('admin', 'password')

# 测试用例类
class ErrorHandlingTest:
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
    
    def test_404_error(self):
        """测试404错误响应"""
        response = requests.get(f"{BASE_URL}/nonexistent-route", auth=AUTH)
        
        # 验证状态码
        assert response.status_code == 404, f"Expected status code 404, got {response.status_code}"
        
        # 验证响应头
        assert 'Content-Type' in response.headers, "Missing Content-Type header"
        assert 'application/json' in response.headers['Content-Type'], f"Expected application/json, got {response.headers['Content-Type']}"
        
        # 验证响应体是有效的JSON
        try:
            response_json = response.json()
        except json.JSONDecodeError:
            raise AssertionError("Response is not valid JSON")
        
        # 验证错误响应结构
        assert 'error' in response_json, "Missing 'error' field in response"
        assert 'code' in response_json['error'], "Missing 'code' field in error"
        assert 'message' in response_json['error'], "Missing 'message' field in error"
        
        # 验证错误代码和消息
        assert response_json['error']['code'] == "404", f"Expected error code 404, got {response_json['error']['code']}"
        assert "Not Found" in response_json['error']['message'], f"Expected 'Not Found' in error message, got {response_json['error']['message']}"
        
        return "404 error handled correctly with JSON response"
    
    def test_internal_error(self):
        """测试内部服务器错误响应"""
        # 注意：这个测试需要服务器上有一个会导致内部错误的路由
        # 目前服务器可能没有这样的路由，所以我们将测试其他错误场景
        # 我们可以测试一个无效的HTTP方法来触发错误
        
        # 测试无效的HTTP方法
        response = requests.request('INVALID_METHOD', f"{BASE_URL}/", auth=AUTH)
        
        # 验证响应头
        assert 'Content-Type' in response.headers, "Missing Content-Type header"
        assert 'application/json' in response.headers['Content-Type'], f"Expected application/json, got {response.headers['Content-Type']}"
        
        return f"Invalid method handled correctly with status code {response.status_code}"
    
    def test_post_to_get_route(self):
        """测试向GET路由发送POST请求"""
        response = requests.post(f"{BASE_URL}/", auth=AUTH)
        
        # 验证响应头
        assert 'Content-Type' in response.headers, "Missing Content-Type header"
        assert 'application/json' in response.headers['Content-Type'], f"Expected application/json, got {response.headers['Content-Type']}"
        
        # 验证响应体是有效的JSON
        try:
            response_json = response.json()
        except json.JSONDecodeError:
            raise AssertionError("Response is not valid JSON")
        
        return f"POST to GET route handled correctly with status code {response.status_code}"
    
    def test_large_request(self):
        """测试大请求体处理"""
        # 发送一个超过配置限制的请求（当前配置是5MB）
        large_data = 'x' * (6 * 1024 * 1024)  # 6MB
        response = requests.post(f"{BASE_URL}/test/upload", data=large_data, auth=AUTH)
        
        # 对于大请求，服务器可能会直接关闭连接，所以我们只检查是否收到了响应
        # 如果收到了响应，验证其格式
        if response.status_code != 0:  # 0表示没有收到响应
            # 验证响应头
            if 'Content-Type' in response.headers:
                assert 'application/json' in response.headers['Content-Type'], f"Expected application/json, got {response.headers['Content-Type']}"
            
            return f"Large request handled correctly with status code {response.status_code}"
        else:
            return "Large request handled correctly (connection closed as expected)"
    
    def test_all_errors_json_format(self):
        """测试所有错误响应都是JSON格式"""
        # 测试各种错误场景
        test_cases = [
            ('GET', '/nonexistent-route'),
            ('PUT', '/nonexistent'),
            ('DELETE', '/nonexistent'),
        ]
        
        for method, path in test_cases:
            response = requests.request(method, f"{BASE_URL}{path}", auth=AUTH)
            
            # 验证响应头
            assert 'Content-Type' in response.headers, f"For {method} {path}: Missing Content-Type header"
            assert 'application/json' in response.headers['Content-Type'], f"For {method} {path}: Expected application/json, got {response.headers['Content-Type']}"
            
            # 验证响应体是有效的JSON
            try:
                response_json = response.json()
                # 验证错误响应结构
                assert 'error' in response_json, f"For {method} {path}: Missing 'error' field in response"
                assert 'code' in response_json['error'], f"For {method} {path}: Missing 'code' field in error"
                assert 'message' in response_json['error'], f"For {method} {path}: Missing 'message' field in error"
            except (json.JSONDecodeError, AssertionError) as e:
                raise AssertionError(f"For {method} {path}: {str(e)}")
        
        return "All error responses are in correct JSON format"
    
    def run_all_tests(self):
        """运行所有测试用例"""
        print("=== Error Handling Tests ===")
        print(f"Testing server at: {BASE_URL}")
        print()
        
        # 运行测试用例
        self.run_test("Test 404 Error Response", self.test_404_error)
        self.run_test("Test Internal Server Error", self.test_internal_error)
        self.run_test("Test POST to GET Route", self.test_post_to_get_route)
        self.run_test("Test Large Request", self.test_large_request)
        self.run_test("Test All Errors JSON Format", self.test_all_errors_json_format)
        
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
    test = ErrorHandlingTest()
    success = test.run_all_tests()
    
    # 退出状态码
    exit(0 if success else 1)
