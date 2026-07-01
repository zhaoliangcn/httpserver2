#!/bin/bash

# 测试调试模式脚本
# 验证服务器在调试模式下是否正确添加调试头信息和生成调试日志

echo "========================================="
echo "测试调试模式验证脚本"
echo "========================================="

# 服务器地址和端口
SERVER_URL="http://localhost:8080"

# 检查curl是否安装
if ! command -v curl &> /dev/null; then
    echo "错误: curl 未安装，请先安装 curl 后再运行此脚本"
    exit 1
fi

echo "1. 发送测试请求并检查调试响应头..."
echo "-----------------------------------------"

# 发送请求并获取响应头和响应体
response=$(curl -s -i "$SERVER_URL/")

# 输出原始响应（可选，用于调试）
echo "原始响应:"
echo "$response"
echo "-----------------------------------------"

# 检查是否包含调试头信息
echo "2. 验证调试头信息..."
echo "-----------------------------------------"

debug_headers=(
    "X-Debug-Time"
    "X-Debug-Client-IP"
    "X-Debug-Route-Time"
)

all_headers_found=true
for header in "${debug_headers[@]}"; do
    if echo "$response" | grep -i "^$header:" > /dev/null; then
        header_value=$(echo "$response" | grep -i "^$header:" | awk '{print $2}')
        echo "✓ 找到调试头: $header: $header_value"
    else
        echo "✗ 未找到调试头: $header"
        all_headers_found=false
    fi
done

# 发送更多请求以生成不同类型的调试信息
echo "\n3. 测试不同路由和请求方法..."
echo "-----------------------------------------"

# 测试路径参数路由
response_params=$(curl -s -i "$SERVER_URL/api/users/123")
echo "路径参数路由响应状态码: $(echo "$response_params" | grep -i "^HTTP" | awk '{print $2}')"
if echo "$response_params" | grep -i "X-Debug-Time" > /dev/null; then
    echo "✓ 路径参数路由包含调试头"
fi

# 测试查询参数路由
response_query=$(curl -s -i "$SERVER_URL/api/search?q=test")
echo "查询参数路由响应状态码: $(echo "$response_query" | grep -i "^HTTP" | awk '{print $2}')"
if echo "$response_query" | grep -i "X-Debug-Time" > /dev/null; then
    echo "✓ 查询参数路由包含调试头"
fi

# 测试POST请求
response_post=$(curl -s -i -X POST "$SERVER_URL/api/users" -d "name=test")
echo "POST请求响应状态码: $(echo "$response_post" | grep -i "^HTTP" | awk '{print $2}')"
if echo "$response_post" | grep -i "X-Debug-Time" > /dev/null; then
    echo "✓ POST请求包含调试头"
fi

echo "\n4. 检查调试日志..."
echo "-----------------------------------------"

# 检查是否存在日志文件
if [ -f "error.log" ]; then
    echo "错误日志文件存在，检查DEBUG级别日志:"
    # 检查错误日志中的调试信息
    debug_logs=$(grep "Debug mode enabled" error.log | wc -l)
    echo "调试模式启用记录数量: $debug_logs"
    
    # 显示最近5条错误日志
    echo "最近5条错误日志:"
    tail -5 error.log
else
    echo "未找到错误日志文件 error.log"
fi

echo "\n5. 综合验证结果..."
echo "-----------------------------------------"

# 修复语法错误，使用默认值0初始化debug_logs
if [ -z "$debug_logs" ]; then
    debug_logs=0
fi

if $all_headers_found; then
    echo "✓ 调试模式验证成功!"
    echo "  - 所有调试响应头均已正确添加"
else
    echo "! 调试模式验证未完全通过，详情请查看上述输出"
    if ! $all_headers_found; then
        echo "  - 部分调试响应头未找到"
    fi
fi

echo "\n========================================="
echo "测试完成，请查看以上结果"
echo "提示: 确保服务器在调试模式下运行，通过 server.enableDebugMode(true) 启用"
echo "========================================="