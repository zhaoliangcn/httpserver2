#!/bin/bash

# 测试HTTP请求解析功能

echo "========== 测试请求解析功能 =========="

# 测试1: 查询参数解析
echo "\n[测试1] 查询参数解析测试"
curl -s "http://localhost:8080/test/query?name=test&value=123&flag=true"

# 测试2: 表单数据解析 (application/x-www-form-urlencoded)
echo "\n[测试2] 表单数据解析测试"
curl -s -X POST -H "Content-Type: application/x-www-form-urlencoded" \
     -d "username=admin&password=secret&remember=true" \
     http://localhost:8080/test/form

# 测试3: JSON请求体解析
echo "\n[测试3] JSON请求体解析测试"
curl -s -X POST -H "Content-Type: application/json" \
     -d '{"user": "testuser", "age": 25, "active": true}' \
     http://localhost:8080/test/json

# 测试4: 文件上传测试
echo "\n[测试4] 文件上传测试"

# 创建一个测试文件
echo "This is a test file for upload" > test_upload.txt

curl -s -X POST -F "description=This is a test upload" \
     -F "file=@test_upload.txt" \
     http://localhost:8080/test/upload

# 清理测试文件
rm test_upload.txt

echo "\n========== 测试完成 =========="