#!/usr/bin/env python3
"""
功能性测试脚本 - 测试HttpServer2的反向代理功能
"""

import requests
import time
import sys

# 测试配置
TEST_PORT = 8080
BASE_URL = f"http://localhost:{TEST_PORT}"

def test_single_backend():
    """测试单个后端反向代理"""
    print("\n=== 测试单个后端反向代理 ===")
    
    try:
        response = requests.get(f"{BASE_URL}/single", timeout=10)
        print(f"状态码: {response.status_code}")
        print(f"响应头: {dict(response.headers)}")
        print(f"响应内容长度: {len(response.text)} 字节")
        
        if response.status_code == 200:
            print("✅ 单个后端测试通过 - 成功转发到外部服务器")
            return True
        else:
            print(f"❌ 单个后端测试失败 - 状态码: {response.status_code}")
            return False
            
    except Exception as e:
        print(f"❌ 单个后端测试失败 - 异常: {e}")
        return False

def test_multi_backend():
    """测试多个后端反向代理（轮询）"""
    print("\n=== 测试多个后端反向代理（轮询） ===")
    
    try:
        # 发送多个请求来测试轮询
        responses = []
        for i in range(6):
            response = requests.get(f"{BASE_URL}/multi", timeout=10)
            responses.append(response)
            print(f"请求 {i+1}: 状态码 {response.status_code}, 长度 {len(response.text)} 字节")
            time.sleep(0.5)  # 短暂延迟
        
        # 检查是否所有请求都成功
        success_count = sum(1 for r in responses if r.status_code == 200)
        
        if success_count == len(responses):
            print("✅ 多个后端测试通过 - 所有请求都成功")
            return True
        else:
            print(f"❌ 多个后端测试失败 - 成功请求: {success_count}/{len(responses)}")
            return False
            
    except Exception as e:
        print(f"❌ 多个后端测试失败 - 异常: {e}")
        return False

def test_custom_headers():
    """测试带自定义头的反向代理"""
    print("\n=== 测试带自定义头的反向代理 ===")
    
    try:
        response = requests.get(f"{BASE_URL}/custom", timeout=10)
        print(f"状态码: {response.status_code}")
        
        # 检查响应头中是否有自定义头
        headers = dict(response.headers)
        if 'X-Powered-By' in headers and headers['X-Powered-By'] == 'HttpServer2/2.0':
            print("✅ 自定义头测试通过 - 成功添加自定义响应头")
            return True
        else:
            print(f"❌ 自定义头测试失败 - 未找到预期的自定义头")
            print(f"   实际响应头: {headers}")
            return False
            
    except Exception as e:
        print(f"❌ 自定义头测试失败 - 异常: {e}")
        return False

def test_server_status():
    """测试服务器状态"""
    print("\n=== 测试服务器状态 ===")
    
    try:
        # 测试一个不存在的路径，看看服务器是否响应
        response = requests.get(f"{BASE_URL}/nonexistent", timeout=5)
        print(f"服务器响应状态码: {response.status_code}")
        print("✅ 服务器状态测试通过 - 服务器正在运行")
        return True
        
    except requests.exceptions.ConnectionError:
        print("❌ 服务器状态测试失败 - 无法连接到服务器")
        return False
    except Exception as e:
        print(f"❌ 服务器状态测试失败 - 异常: {e}")
        return False

def main():
    """主测试函数"""
    print("HttpServer2 反向代理功能测试")
    print("=" * 50)
    
    # 等待服务器完全启动
    print("等待服务器启动...")
    time.sleep(2)
    
    # 运行测试
    test_results = []
    
    # 测试服务器状态
    test_results.append(("服务器状态", test_server_status()))
    
    # 运行功能测试
    test_results.append(("单个后端", test_single_backend()))
    test_results.append(("多个后端", test_multi_backend()))
    test_results.append(("自定义头", test_custom_headers()))
    
    # 输出测试总结
    print("\n" + "=" * 50)
    print("测试总结:")
    
    passed = 0
    total = len(test_results)
    
    for test_name, result in test_results:
        status = "✅ 通过" if result else "❌ 失败"
        print(f"  {test_name}: {status}")
        if result:
            passed += 1
    
    print(f"\n总测试数: {total}")
    print(f"通过数: {passed}")
    print(f"失败数: {total - passed}")
    
    if passed == total:
        print("\n🎉 所有测试通过！反向代理功能正常工作。")
        return 0
    else:
        print(f"\n⚠️  {total - passed} 个测试失败，请检查服务器配置。")
        return 1

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\n测试被用户中断")
        sys.exit(1)
    except Exception as e:
        print(f"\n测试过程中发生错误: {e}")
        sys.exit(1)