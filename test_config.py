#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
测试YAML配置文件功能
"""

import os
import subprocess
import time
import signal
import sys

# 测试配置文件路径
test_config_path = "test_config.yaml"

# 创建测试配置文件（禁用所有功能）
def create_test_config():
    config_content = """
# 测试配置文件 - 禁用所有功能
enable_basic_auth: false
set_request_size_limit: false
enable_csrf_protection: false
enable_debug_mode: false
"""
    
    with open(test_config_path, 'w', encoding='utf-8') as f:
        f.write(config_content)
    
    print(f"创建测试配置文件: {test_config_path}")

# 启动服务器并收集输出
def start_server_and_collect_output(config_file=None):
    cmd = ["./httpserver2", "8080"]
    if config_file:
        cmd.extend(["--config", config_file])
    
    print(f"启动服务器: {' '.join(cmd)}")
    
    # 启动服务器进程
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True
    )
    
    # 读取输出直到服务器启动
    output = []
    start_time = time.time()
    timeout = 5  # 5秒超时
    
    while time.time() - start_time < timeout:
        if proc.stdout.readable():
            line = proc.stdout.readline()
            if line:
                output.append(line.strip())
                print(f"[服务器输出] {line.strip()}")
                # 检查是否启动成功
                if "服务器已启动" in line or "监听端口" in line:
                    return proc, output
        
        # 检查进程是否已退出
        if proc.poll() is not None:
            print("服务器进程意外退出")
            break
        
        time.sleep(0.1)
    
    # 如果超时，终止进程
    if proc.poll() is None:
        print("服务器启动超时")
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=2)
    
    return None, output

# 停止服务器
def stop_server(proc):
    if proc and proc.poll() is None:
        print("停止服务器...")
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
            print("服务器已停止")
        except subprocess.TimeoutExpired:
            print("服务器停止超时，强制终止")
            proc.kill()

# 分析服务器输出以验证配置是否生效
def analyze_output(output):
    print("\n=== 分析服务器配置 ===")
    
    # 检查基本认证
    basic_auth_enabled = any("已设置基本认证用户" in line for line in output)
    print(f"基本认证: {'已启用' if basic_auth_enabled else '已禁用'}")
    
    # 检查请求大小限制
    request_limit_enabled = any("已设置请求大小限制" in line for line in output)
    print(f"请求大小限制: {'已启用' if request_limit_enabled else '已禁用'}")
    
    # 检查CSRF保护
    csrf_enabled = any("已启用CSRF保护" in line for line in output)
    print(f"CSRF保护: {'已启用' if csrf_enabled else '已禁用'}")
    
    # 检查调试模式
    debug_enabled = any("调试模式已启用" in line for line in output)
    print(f"调试模式: {'已启用' if debug_enabled else '已禁用'}")

# 主测试函数
def main():
    print("开始测试YAML配置文件功能...")
    
    # 1. 测试默认配置（启用所有功能）
    print("\n[测试1] 使用默认配置文件")
    proc, output = start_server_and_collect_output()
    if proc:
        analyze_output(output)
        stop_server(proc)
    
    # 2. 创建并测试自定义配置（禁用所有功能）
    create_test_config()
    print("\n[测试2] 使用自定义配置文件（禁用所有功能）")
    proc, output = start_server_and_collect_output(test_config_path)
    if proc:
        analyze_output(output)
        stop_server(proc)
    
    # 清理测试文件
    try:
        os.remove(test_config_path)
        print(f"\n清理测试配置文件: {test_config_path}")
    except:
        pass
    
    print("\n测试完成！")

if __name__ == "__main__":
    main()