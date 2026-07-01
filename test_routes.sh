#!/bin/bash

# HTTP服务器路由测试脚本
# 测试服务器运行在 http://localhost:8080

SERVER_URL="http://localhost:8080"

echo "========================================"
echo "HTTP服务器路由功能测试脚本"
echo "========================================"
echo "测试服务器地址: $SERVER_URL"
echo "========================================"

# 测试函数
run_test() {
    local description=$1
    local method=$2
    local endpoint=$3
    local data=$4
    local extra_args=$5
    
    echo "\n[测试] $description"
    echo "命令: curl -X $method $SERVER_URL$endpoint $data $extra_args"
    
    if [ -z "$data" ]; then
        curl -v -X $method "$SERVER_URL$endpoint" $extra_args
    else
        curl -v -X $method "$SERVER_URL$endpoint" -H "Content-Type: application/json" -d "$data" $extra_args
    fi
    
    echo "\n----------------------------------------"
}

# 确保脚本可中断
set -e

# 1. 测试基本路由
echo "\n=== 测试基本路由 ==="
run_test "主页路由" "GET" "/" "" ""
run_test "用户列表API" "GET" "/api/users" "" ""

# 2. 测试路径参数路由
echo "\n=== 测试路径参数路由 ==="
run_test "获取特定用户(路径参数)" "GET" "/api/users/123" "" ""
run_test "更新特定用户(路径参数)" "PUT" "/api/users/123" '{"name":"Updated User","email":"updated@example.com"}' ""
run_test "删除特定用户(路径参数)" "DELETE" "/api/users/123" "" ""

# 3. 测试查询参数路由
echo "\n=== 测试查询参数路由 ==="
run_test "带查询参数的搜索" "GET" "/api/search?q=test" "" ""
run_test "多查询参数" "GET" "/api/search?q=test&limit=10&page=1" "" ""

# 4. 测试正则表达式路由
echo "\n=== 测试正则表达式路由 ==="
run_test "文章ID正则匹配" "GET" "/api/articles/42" "" ""
run_test "复杂正则路由(用户名和文章ID)" "GET" "/api/users/johndoe/posts/123" "" ""
run_test "可选部分正则路由(获取列表)" "GET" "/api/items" "" ""
run_test "可选部分正则路由(获取单个)" "GET" "/api/items/56" "" ""

# 5. 测试不同HTTP方法
echo "\n=== 测试不同HTTP方法 ==="
run_test "POST创建资源" "POST" "/api/users" '{"name":"New User","email":"new@example.com"}' ""
run_test "OPTIONS预检请求" "OPTIONS" "/api/users" "" ""

# 6. 测试不存在的路由
echo "\n=== 测试错误处理 ==="
run_test "不存在的路由" "GET" "/this-route-does-not-exist" "" ""
run_test "正则路由不匹配(非数字ID)" "GET" "/api/articles/abc" "" ""

# 7. 测试静态文件服务
echo "\n=== 测试静态文件服务 ==="
run_test "静态主页HTML" "GET" "/index.html" "" ""
run_test "CSS文件服务" "GET" "/styles.css" "" "-I"
run_test "JavaScript文件服务" "GET" "/app.js" "" "-I"
run_test "about页面" "GET" "/about.html" "" ""
run_test "静态目录默认页面" "GET" "/static/" "" ""
run_test "文本文件服务" "GET" "/static/test.txt" "" "-I"
run_test "JSON文件服务" "GET" "/static/data.json" "" "-I"
run_test "静态目录index" "GET" "/static/index.html" "" ""
run_test "不存在的静态文件" "GET" "/static/non_existent_file.txt" "" ""

echo "\n========================================"
echo "所有测试完成!"
echo "========================================"