#!/usr/bin/env python3
"""
HttpServer2 静态文件服务测试

测试静态文件服务的各项功能，包括：
  - ETag 缓存验证
  - 304 Not Modified 响应
  - Last-Modified / If-Modified-Since
  - MIME 类型识别
  - 目录默认文件（index.html）
  - 文件不存在（404）
  - 路径遍历防护
  - Cache-Control 头
"""

import os
import sys
import requests
import json
import hashlib

# 服务器配置
BASE_URL = os.environ.get('TEST_URL', 'http://localhost:8080')
AUTH = ('admin', 'password')


class StaticFilesTest:
    def __init__(self):
        self.test_results = []

    def run_test(self, test_name, test_function):
        try:
            result = test_function()
            self.test_results.append({
                'name': test_name, 'status': 'PASS', 'message': result
            })
            print(f"✅ {test_name}: PASS - {result}")
        except Exception as e:
            self.test_results.append({
                'name': test_name, 'status': 'FAIL', 'message': str(e)
            })
            print(f"❌ {test_name}: FAIL - {str(e)}")

    def test_static_txt_file(self):
        """测试静态 .txt 文件"""
        r = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        assert 'text/plain' in r.headers.get('Content-Type', ''), \
            f"Expected text/plain, got {r.headers.get('Content-Type')}"
        assert len(r.text) > 0, "Response body is empty"
        return f"Served test.txt ({len(r.text)} bytes)"

    def test_static_html_file(self):
        """测试静态 .html 文件"""
        r = requests.get(f"{BASE_URL}/static/index.html", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        ct = r.headers.get('Content-Type', '')
        assert 'text/html' in ct, f"Expected text/html, got {ct}"
        return f"Served index.html ({len(r.text)} bytes)"

    def test_static_json_file(self):
        """测试静态 .json 文件"""
        r = requests.get(f"{BASE_URL}/static/data.json", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        ct = r.headers.get('Content-Type', '')
        assert 'application/json' in ct, f"Expected application/json, got {ct}"
        try:
            json.loads(r.text)
        except json.JSONDecodeError:
            raise AssertionError("Response is not valid JSON")
        return "Served data.json with correct MIME type"

    def test_etag_header_present(self):
        """验证 ETag 头存在"""
        r = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        assert r.status_code == 200
        assert 'ETag' in r.headers, "Missing ETag header"
        assert len(r.headers['ETag']) > 0, "ETag is empty"
        return f"ETag: {r.headers['ETag']}"

    def test_last_modified_header(self):
        """验证 Last-Modified 头存在"""
        r = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        assert r.status_code == 200
        assert 'Last-Modified' in r.headers, "Missing Last-Modified header"
        assert len(r.headers['Last-Modified']) > 0, "Last-Modified is empty"
        return f"Last-Modified: {r.headers['Last-Modified']}"

    def test_304_not_modified_with_etag(self):
        """使用 ETag 请求 304"""
        r1 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        assert r1.status_code == 200
        etag = r1.headers.get('ETag')
        assert etag, "No ETag in first response"

        r2 = requests.get(f"{BASE_URL}/static/test.txt",
                          headers={'If-None-Match': etag}, auth=AUTH)
        assert r2.status_code == 304, f"Expected 304, got {r2.status_code}"
        return f"304 Not Modified with ETag: {etag}"

    def test_304_not_modified_with_modified_since(self):
        """使用 If-Modified-Since 请求 304"""
        r1 = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        assert r1.status_code == 200
        lm = r1.headers.get('Last-Modified')
        assert lm, "No Last-Modified in first response"

        r2 = requests.get(f"{BASE_URL}/static/test.txt",
                          headers={'If-Modified-Since': lm}, auth=AUTH)
        assert r2.status_code == 304, f"Expected 304, got {r2.status_code}"
        return f"304 Not Modified with If-Modified-Since: {lm}"

    def test_cache_control_header(self):
        """验证 Cache-Control 头"""
        r = requests.get(f"{BASE_URL}/static/test.txt", auth=AUTH)
        assert r.status_code == 200
        cc = r.headers.get('Cache-Control', '')
        assert 'max-age=' in cc, f"Cache-Control missing max-age: {cc}"
        assert 'public' in cc, f"Cache-Control missing 'public': {cc}"
        return f"Cache-Control: {cc}"

    def test_file_not_found_404(self):
        """不存在的文件返回 404"""
        r = requests.get(f"{BASE_URL}/static/nonexistent_file.xyz", auth=AUTH)
        assert r.status_code == 404, f"Expected 404, got {r.status_code}"
        return "Nonexistent file returns 404"

    def test_directory_default_file(self):
        """访问目录返回默认 index.html"""
        r = requests.get(f"{BASE_URL}/static/", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        ct = r.headers.get('Content-Type', '')
        assert 'text/html' in ct, f"Expected text/html, got {ct}"
        return "Directory serves index.html"

    def test_path_traversal_prevention(self):
        """路径遍历攻击防护"""
        payloads = [
            "/static/../config.yaml",
            "/static/../../../etc/passwd",
            "/static/..%2F..%2Fconfig.yaml",
        ]
        for path in payloads:
            r = requests.get(f"{BASE_URL}{path}", auth=AUTH)
            assert r.status_code in (403, 404), \
                f"Path traversal '{path}' should return 403/404, got {r.status_code}"
        return f"Path traversal blocked ({len(payloads)} payloads)"

    def test_mime_type_for_unknown_extension(self):
        """未知扩展名的文件应返回默认 MIME 类型"""
        r = requests.get(f"{BASE_URL}/static/test.unknown", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        return f"Served .unknown file as {r.headers.get('Content-Type')}"

    def test_uppercase_extension(self):
        """大写扩展名文件的 MIME 类型"""
        r = requests.get(f"{BASE_URL}/static/test.UPPERCASEJS", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        return f"Served .UPPERCASEJS as {r.headers.get('Content-Type')}"

    def run_all_tests(self):
        print(f"=== Static Files Tests ===")
        print(f"Testing server at: {BASE_URL}")
        print()

        self.run_test("Serve .txt file", self.test_static_txt_file)
        self.run_test("Serve .html file", self.test_static_html_file)
        self.run_test("Serve .json file", self.test_static_json_file)
        self.run_test("ETag header present", self.test_etag_header_present)
        self.run_test("Last-Modified header present", self.test_last_modified_header)
        self.run_test("304 Not Modified with ETag", self.test_304_not_modified_with_etag)
        self.run_test("304 Not Modified with If-Modified-Since", self.test_304_not_modified_with_modified_since)
        self.run_test("Cache-Control header", self.test_cache_control_header)
        self.run_test("File not found returns 404", self.test_file_not_found_404)
        self.run_test("Directory serves default file", self.test_directory_default_file)
        self.run_test("Path traversal prevention", self.test_path_traversal_prevention)
        self.run_test("Unknown extension MIME type", self.test_mime_type_for_unknown_extension)
        self.run_test("Uppercase extension", self.test_uppercase_extension)

        print()
        passed = sum(1 for r in self.test_results if r['status'] == 'PASS')
        failed = sum(1 for r in self.test_results if r['status'] == 'FAIL')
        total = len(self.test_results)

        for r in self.test_results:
            status = "✅ PASS" if r['status'] == 'PASS' else "❌ FAIL"
            print(f"{status}: {r['name']} - {r['message']}")

        print(f"\nSummary: {passed} passed, {failed} failed, {total} total")
        if failed == 0:
            print("🎉 All tests passed!")
        return failed == 0


if __name__ == "__main__":
    test = StaticFilesTest()
    success = test.run_all_tests()
    sys.exit(0 if success else 1)
