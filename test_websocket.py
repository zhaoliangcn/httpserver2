#!/usr/bin/env python3
"""
HttpServer2 WebSocket 升级握手测试

测试 WebSocket 协议升级功能：
  - Upgrade 头
  - Connection: Upgrade
  - Sec-WebSocket-Key / Accept
  - HTTP 101 响应
"""

import os
import sys
import socket
import base64
import hashlib
import struct

# 服务器配置
HOST = os.environ.get('TEST_HOST', 'localhost')
PORT = int(os.environ.get('TEST_PORT', os.environ.get('TEST_URL', 'http://localhost:8080').rsplit(':', 1)[-1].rstrip('/')))
if PORT == 8080:
    # 默认端口覆盖
    PORT = 8080


class WebSocketTest:
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

    def _ws_handshake(self, path="/", headers=None):
        """执行 WebSocket 握手，返回 (sock, response_headers)"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((HOST, PORT))

        # 生成 WebSocket 密钥
        import random
        ws_key = base64.b64encode(bytes(random.getrandbits(8) for _ in range(16))).decode()

        # 构造升级请求
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {HOST}:{PORT}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {ws_key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
        )
        if headers:
            for k, v in headers.items():
                req += f"{k}: {v}\r\n"
        req += "\r\n"

        sock.sendall(req.encode())

        # 读取响应
        response = b""
        while b"\r\n\r\n" not in response:
            data = sock.recv(4096)
            if not data:
                break
            response += data

        # 解析响应头
        header_end = response.find(b"\r\n\r\n")
        header_section = response[:header_end].decode()
        status_line = header_section.split("\r\n")[0]
        resp_headers = {}
        for line in header_section.split("\r\n")[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                resp_headers[k.strip()] = v.strip()

        return sock, status_line, resp_headers, ws_key

    def test_ws_upgrade_success(self):
        """WebSocket 升级应返回 101"""
        sock, status, headers, key = self._ws_handshake()
        try:
            assert "101" in status, f"Expected 101, got: {status}"
            assert headers.get('Upgrade', '').lower() == 'websocket', \
                f"Missing Upgrade: websocket: {headers.get('Upgrade')}"
            assert 'websocket' in headers.get('Connection', '').lower(), \
                f"Missing Connection: Upgrade: {headers.get('Connection')}"

            # 验证 Sec-WebSocket-Accept
            accept = headers.get('Sec-WebSocket-Accept', '')
            expected_accept = base64.b64encode(
                hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
            ).decode()
            assert accept == expected_accept, \
                f"Sec-WebSocket-Accept mismatch: got {accept}, expected {expected_accept}"
            return f"WebSocket upgrade successful (101)"
        finally:
            sock.close()

    def test_ws_without_upgrade_header(self):
        """无 Upgrade 头的普通 HTTP 请求不应返回 101"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((HOST, PORT))
        try:
            sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            response = b""
            while b"\r\n\r\n" not in response:
                data = sock.recv(4096)
                if not data:
                    break
                response += data
            status_line = response.split(b"\r\n")[0].decode()
            assert "101" not in status_line, f"Unexpected 101 for normal request: {status_line}"
            return f"Normal request returns {status_line.split()[1]} (not 101)"
        finally:
            sock.close()

    def test_ws_with_auth(self):
        """带基本认证的 WebSocket 握手"""
        import base64 as b64
        auth = b64.b64encode(b"admin:password").decode()
        sock, status, headers, key = self._ws_handshake(
            headers={"Authorization": f"Basic {auth}"}
        )
        try:
            assert "101" in status, f"Expected 101 with auth, got: {status}"
            return "Authenticated WebSocket upgrade successful"
        finally:
            sock.close()

    def run_all_tests(self):
        print(f"=== WebSocket Tests ===")
        print(f"Testing server at: {HOST}:{PORT}")
        print()

        self.run_test("WebSocket upgrade (101)", self.test_ws_upgrade_success)
        self.run_test("Normal HTTP request (not 101)", self.test_ws_without_upgrade_header)
        self.run_test("WS upgrade with auth", self.test_ws_with_auth)

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
    test = WebSocketTest()
    success = test.run_all_tests()
    sys.exit(0 if success else 1)
