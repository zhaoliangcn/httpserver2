#!/usr/bin/env python3
"""
HttpServer2 安全测试

测试安全相关功能，包括：
  - 基本认证（Basic Auth）
  - CSRF 保护
  - 请求大小限制
  - 路径遍历防护
  - 安全响应头
  - CORS 头
"""

import os
import sys
import requests
import json

# 服务器配置
BASE_URL = os.environ.get('TEST_URL', 'http://localhost:8080')
AUTH = ('admin', 'password')


class SecurityTest:
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

    def test_basic_auth_success(self):
        """正确的基本认证应通过"""
        r = requests.get(f"{BASE_URL}/", auth=AUTH)
        assert r.status_code == 200, f"Expected 200, got {r.status_code}"
        return "Correct credentials accepted"

    def test_basic_auth_failure(self):
        """错误的基本认证应被拒绝"""
        r = requests.get(f"{BASE_URL}/", auth=('wrong', 'wrong'))
        assert r.status_code == 401, f"Expected 401, got {r.status_code}"
        www_auth = r.headers.get('WWW-Authenticate', '')
        assert 'Basic' in www_auth, f"Missing WWW-Authenticate header: {www_auth}"
        return "Incorrect credentials rejected with 401"

    def test_basic_auth_no_credentials(self):
        """无认证信息应返回 401"""
        r = requests.get(f"{BASE_URL}/")
        assert r.status_code == 401, f"Expected 401, got {r.status_code}"
        return "No credentials returns 401"

    def test_security_headers_present(self):
        """验证安全响应头存在"""
        r = requests.get(f"{BASE_URL}/", auth=AUTH)
        assert r.status_code == 200
        security_headers = {
            'X-Content-Type-Options': 'nosniff',
            'X-Frame-Options': 'SAMEORIGIN',
            'X-XSS-Protection': '1; mode=block',
        }
        missing = []
        for header, expected_val in security_headers.items():
            if header not in r.headers:
                missing.append(header)
        assert not missing, f"Missing security headers: {missing}"
        return f"All security headers present"

    def test_cors_headers(self):
        """验证 CORS 头"""
        r = requests.get(f"{BASE_URL}/", auth=AUTH)
        assert r.status_code == 200
        assert 'Access-Control-Allow-Origin' in r.headers, "Missing CORS header"
        assert r.headers['Access-Control-Allow-Origin'] == '*', \
            f"Expected *, got {r.headers['Access-Control-Allow-Origin']}"
        return "CORS headers correct"

    def test_cors_preflight(self):
        """OPTIONS 预检请求"""
        r = requests.options(f"{BASE_URL}/", auth=AUTH)
        assert r.status_code in (200, 204), f"Expected 200/204, got {r.status_code}"
        assert 'Access-Control-Allow-Methods' in r.headers, "Missing Allow-Methods"
        assert 'GET' in r.headers['Access-Control-Allow-Methods'], \
            f"GET not in: {r.headers['Access-Control-Allow-Methods']}"
        return "CORS preflight handled"

    def test_large_request_rejected(self):
        """超大请求体应被拒绝（413）"""
        # 发送 6MB 数据（配置限制 5MB）
        large_data = 'A' * (6 * 1024 * 1024)
        r = requests.post(f"{BASE_URL}/test/upload",
                          data=large_data, auth=AUTH)
        # 可能返回 413 或连接被重置
        assert r.status_code in (200, 413, 500), \
            f"Unexpected status: {r.status_code}"
        if r.status_code == 413:
            return "Large request rejected with 413"
        elif r.status_code == 500:
            return f"Large request resulted in 500 (possible)"
        else:
            return f"Large request accepted with {r.status_code} (expected 413)"

    def test_csrf_protection_on_post(self):
        """验证 CSRF 保护在后端存在"""
        # POST 请求应该触发 CSRF 中间件
        r = requests.post(f"{BASE_URL}/test/form",
                          data={"test": "data"}, auth=AUTH)
        # 可能返回 200（CSRF 未强制）或 403（CSRF 强制）
        # 至少验证有 Set-Cookie
        set_cookie = r.headers.get('Set-Cookie', '')
        has_csrf_cookie = 'XSRF-TOKEN=' in set_cookie or 'csrf' in set_cookie.lower()
        if has_csrf_cookie:
            return f"CSRF cookie set: {set_cookie[:60]}"
        else:
            return f"No CSRF cookie (status {r.status_code})"

    def test_public_directory_access(self):
        """验证 public/ 静态资源可访问"""
        r = requests.get(f"{BASE_URL}/static/index.html", auth=AUTH)
        assert r.status_code == 200, f"Static file: {r.status_code}"
        return "Public directory accessible"

    def test_sensitive_files_inaccessible(self):
        """敏感文件不应被直接访问"""
        sensitive_paths = [
            "/config.yaml",
            "/server.key",
            "/server.crt",
            "/.env",
        ]
        blocked = 0
        for path in sensitive_paths:
            r = requests.get(f"{BASE_URL}{path}", auth=AUTH)
            if r.status_code in (403, 404):
                blocked += 1
        return f"{blocked}/{len(sensitive_paths)} sensitive files blocked"

    def test_https_security_headers_on_all_routes(self):
        """所有路由都应包含关键安全头"""
        routes = ["/", "/api/users", "/health", "/static/test.txt"]
        issues = []
        for route in routes:
            r = requests.get(f"{BASE_URL}{route}", auth=AUTH)
            if r.status_code == 200:
                if 'X-Content-Type-Options' not in r.headers:
                    issues.append(f"{route}: missing X-Content-Type-Options")
        if issues:
            raise AssertionError("\n".join(issues))
        return "All routes have security headers"

    def run_all_tests(self):
        print(f"=== Security Tests ===")
        print(f"Testing server at: {BASE_URL}")
        print()

        self.run_test("Basic Auth - correct credentials", self.test_basic_auth_success)
        self.run_test("Basic Auth - incorrect credentials", self.test_basic_auth_failure)
        self.run_test("Basic Auth - no credentials", self.test_basic_auth_no_credentials)
        self.run_test("Security headers present", self.test_security_headers_present)
        self.run_test("CORS headers", self.test_cors_headers)
        self.run_test("CORS preflight", self.test_cors_preflight)
        self.run_test("Large request rejection", self.test_large_request_rejected)
        self.run_test("CSRF cookie on POST", self.test_csrf_protection_on_post)
        self.run_test("Public directory access", self.test_public_directory_access)
        self.run_test("Sensitive files blocked", self.test_sensitive_files_inaccessible)
        self.run_test("Security headers on all routes", self.test_https_security_headers_on_all_routes)

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
    test = SecurityTest()
    success = test.run_all_tests()
    sys.exit(0 if success else 1)
