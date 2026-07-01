#!/usr/bin/env python3

import requests
import json
import time

# 服务器配置
BASE_URL = 'http://localhost:8080'
# 基本认证信息
AUTH = ('admin', 'password')

# 测试用例类
class HttpCachingTest:
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
    
    def test_etag_support(self):
        """测试ETag支持"""
        # 首先获取一个静态资源，获取ETag
        response1 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        
        # 验证状态码
        assert response1.status_code == 200, f"Expected status code 200, got {response1.status_code}"
        
        # 验证响应中包含ETag
        assert 'ETag' in response1.headers, "Missing ETag header in response"
        etag = response1.headers['ETag']
        
        # 使用ETag发送If-None-Match头
        response2 = requests.get(f"{BASE_URL}/static/test.txt", 
                              headers={'If-None-Match': etag}, 
                              auth=AUTH)
        
        # 验证返回304 Not Modified
        assert response2.status_code == 304, f"Expected status code 304, got {response2.status_code}"
        
        # 验证响应不包含响应体
        assert len(response2.content) == 0, f"Expected empty body for 304 response, got {len(response2.content)} bytes"
        
        return "ETag support works correctly: 304 Not Modified returned when ETag matches"
    
    def test_last_modified_support(self):
        """测试Last-Modified和If-Modified-Since支持"""
        # 首先获取一个静态资源，获取Last-Modified头
        response1 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        
        # 验证状态码
        assert response1.status_code == 200, f"Expected status code 200, got {response1.status_code}"
        
        # 验证响应中包含Last-Modified
        assert 'Last-Modified' in response1.headers, "Missing Last-Modified header in response"
        last_modified = response1.headers['Last-Modified']
        
        # 使用Last-Modified发送If-Modified-Since头
        response2 = requests.get(f"{BASE_URL}/static/test.txt", 
                              headers={'If-Modified-Since': last_modified}, 
                              auth=AUTH)
        
        # 验证返回304 Not Modified
        assert response2.status_code == 304, f"Expected status code 304, got {response2.status_code}"
        
        # 验证响应不包含响应体
        assert len(response2.content) == 0, f"Expected empty body for 304 response, got {len(response2.content)} bytes"
        
        return "Last-Modified/If-Modified-Since support works correctly: 304 Not Modified returned when resource not modified"
    
    def test_cache_control_header(self):
        """测试Cache-Control响应头"""
        response = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        
        # 验证状态码
        assert response.status_code == 200, f"Expected status code 200, got {response.status_code}"
        
        # 验证响应中包含Cache-Control头
        assert 'Cache-Control' in response.headers, "Missing Cache-Control header in response"
        cache_control = response.headers['Cache-Control']
        
        # 验证Cache-Control头包含正确的缓存策略
        assert 'max-age' in cache_control, f"Expected 'max-age' in Cache-Control header, got {cache_control}"
        assert 'public' in cache_control, f"Expected 'public' in Cache-Control header, got {cache_control}"
        
        return f"Cache-Control header works correctly: {cache_control}"
    
    def test_static_resource_caching(self):
        """测试静态资源缓存"""
        # 测试多个静态资源
        test_resources = [
            '/static/test.txt',
            '/static/index.html',
            '/static/data.json'
        ]
        
        for resource in test_resources:
            # 获取资源，检查是否包含缓存相关头
            response = requests.get(f"{BASE_URL}{resource}", auth=AUTH)
            
            # 验证状态码
            assert response.status_code == 200, f"For {resource}: Expected status code 200, got {response.status_code}"
            
            # 验证响应中包含缓存相关头
            assert 'ETag' in response.headers, f"For {resource}: Missing ETag header"
            assert 'Last-Modified' in response.headers, f"For {resource}: Missing Last-Modified header"
            assert 'Cache-Control' in response.headers, f"For {resource}: Missing Cache-Control header"
        
        return f"All static resources have proper caching headers"
    
    def test_cache_invalidation(self):
        """测试缓存失效"""
        # 首先获取资源，获取ETag
        response1 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        etag1 = response1.headers['ETag']
        
        # 等待一段时间，确保文件修改时间变化
        time.sleep(1)
        
        # 修改测试文件
        with open('/Users/macmima1234/code/testhttpserver2/static/test.txt', 'a') as f:
            f.write(f"\nModified at {time.time()}")
        
        # 再次获取资源，验证ETag变化
        response2 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        etag2 = response2.headers['ETag']
        
        # 验证ETag变化
        assert etag1 != etag2, f"Expected ETag to change after file modification, but both were {etag1}"
        
        # 使用旧ETag发送请求，验证返回200而不是304
        response3 = requests.get(f"{BASE_URL}/static/test.txt", 
                              headers={'If-None-Match': etag1}, 
                              auth=AUTH)
        assert response3.status_code == 200, f"Expected status code 200 for changed resource, got {response3.status_code}"
        
        return "Cache invalidation works correctly: ETag changes when resource is modified"
    
    def run_all_tests(self):
        """运行所有测试用例"""
        print("=== HTTP Caching Tests ===")
        print(f"Testing server at: {BASE_URL}")
        print()
        
        # 运行测试用例
        self.run_test("Test ETag Support", self.test_etag_support)
        self.run_test("Test Last-Modified Support", self.test_last_modified_support)
        self.run_test("Test Cache-Control Header", self.test_cache_control_header)
        self.run_test("Test Static Resource Caching", self.test_static_resource_caching)
        self.run_test("Test Cache Invalidation", self.test_cache_invalidation)
        
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
    test = HttpCachingTest()
    success = test.run_all_tests()
    
    # 退出状态码
    exit(0 if success else 1)
