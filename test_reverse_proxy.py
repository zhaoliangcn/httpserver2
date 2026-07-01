#!/usr/bin/env python3
"""
Test script for reverse proxy functionality in HttpServer2
"""

import http.server
import socketserver
import threading
import time
import requests
import subprocess
import os
import sys

# Test configuration
HTTP_SERVER2_BINARY = "./httpserver2"
CONFIG_FILE = "test_proxy_config.yaml"
TEST_PORT = 8080

# Test backend servers
BACKENDS = [
    {"port": 8001, "message": "Backend 1 Response"},
    {"port": 8002, "message": "Backend 2 Response"},
    {"port": 8003, "message": "Backend 3 Response"}
]

class SimpleHTTPRequestHandler(http.server.BaseHTTPRequestHandler):
    """Simple HTTP handler for test backends"""
    
    def do_GET(self):
        # Get the backend message from the server's address
        backend_port = self.server.server_address[1]
        message = next(b["message"] for b in BACKENDS if b["port"] == backend_port)
        
        # Send response
        self.send_response(200)
        self.send_header("Content-type", "text/plain")
        self.end_headers()
        self.wfile.write(f"{message}".encode())
    
    def log_message(self, format, *args):
        """Suppress log messages"""
        return

def start_backend(port):
    """Start a simple HTTP backend server"""
    with socketserver.TCPServer(("", port), SimpleHTTPRequestHandler) as httpd:
        print(f"Starting backend server on port {port}...")
        httpd.serve_forever()

def create_config():
    """Create test configuration file"""
    config_content = f"""
server:
  port: {TEST_PORT}
  threads: 4
  timeout: 30
  log_level: INFO

# Single backend test
routes:
  - path: /single
    method: GET
    type: reverse_proxy
    target: http://localhost:{BACKENDS[0]['port']}
    
  # Multiple backends test with round-robin
  - path: /multi
    method: GET
    type: reverse_proxy
    targets:
      - http://localhost:{BACKENDS[0]['port']}
      - http://localhost:{BACKENDS[1]['port']}
      - http://localhost:{BACKENDS[2]['port']}
"""
    
    with open(CONFIG_FILE, "w") as f:
        f.write(config_content)
    print(f"Created test config file: {CONFIG_FILE}")

def start_httpserver2():
    """Start HttpServer2 with the test configuration"""
    if not os.path.exists(HTTP_SERVER2_BINARY):
        print(f"Error: {HTTP_SERVER2_BINARY} not found. Please compile first.")
        sys.exit(1)
    
    print(f"Starting HttpServer2 with config {CONFIG_FILE}...")
    process = subprocess.Popen([HTTP_SERVER2_BINARY, CONFIG_FILE], 
                              stdout=subprocess.PIPE, 
                              stderr=subprocess.PIPE)
    return process

def test_single_backend():
    """Test reverse proxy with a single backend"""
    print("\n=== Testing Single Backend Reverse Proxy ===")
    
    try:
        response = requests.get(f"http://localhost:{TEST_PORT}/single", timeout=5)
        print(f"Response status: {response.status_code}")
        print(f"Response text: {response.text}")
        
        if response.status_code == 200 and response.text == BACKENDS[0]["message"]:
            print("✅ Single backend test passed!")
            return True
        else:
            print("❌ Single backend test failed!")
            return False
    except Exception as e:
        print(f"❌ Single backend test failed with exception: {e}")
        return False

def test_multi_backend():
    """Test reverse proxy with multiple backends (round-robin)"""
    print("\n=== Testing Multiple Backends Reverse Proxy ===")
    
    responses = []
    expected_messages = [b["message"] for b in BACKENDS]
    
    # Send multiple requests to test round-robin
    for i in range(6):
        try:
            response = requests.get(f"http://localhost:{TEST_PORT}/multi", timeout=5)
            responses.append(response.text)
            print(f"Request {i+1}: {response.text}")
        except Exception as e:
            print(f"❌ Request {i+1} failed with exception: {e}")
            return False
    
    # Check if all backends were used
    unique_responses = set(responses)
    expected_set = set(expected_messages)
    
    if unique_responses == expected_set:
        print("✅ Multiple backends test passed! All backends were used.")
        return True
    else:
        print(f"❌ Multiple backends test failed! Expected: {expected_set}, Got: {unique_responses}")
        return False

def main():
    """Main test function"""
    print("HttpServer2 Reverse Proxy Test Suite")
    print("=" * 50)
    
    # Start backend servers
    backend_threads = []
    for backend in BACKENDS:
        thread = threading.Thread(target=start_backend, args=(backend["port"],), daemon=True)
        thread.start()
        backend_threads.append(thread)
    
    # Wait for backends to start
    time.sleep(1)
    
    # Create config file
    create_config()
    
    # Start HttpServer2
    server_process = start_httpserver2()
    
    # Wait for server to start
    time.sleep(2)
    
    try:
        # Run tests
        test1 = test_single_backend()
        test2 = test_multi_backend()
        
        # Summary
        print("\n" + "=" * 50)
        print("Test Summary:")
        print(f"Single backend: {'✅ PASSED' if test1 else '❌ FAILED'}")
        print(f"Multiple backends: {'✅ PASSED' if test2 else '❌ FAILED'}")
        
        if test1 and test2:
            print("\n🎉 All tests passed! Reverse proxy functionality is working correctly.")
            sys.exit(0)
        else:
            print("\n❌ Some tests failed!")
            sys.exit(1)
    
    finally:
        # Cleanup
        print("\nCleaning up...")
        
        # Terminate HttpServer2
        if 'server_process' in locals():
            server_process.terminate()
            try:
                server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_process.kill()
        
        # Remove config file
        if os.path.exists(CONFIG_FILE):
            os.remove(CONFIG_FILE)

if __name__ == "__main__":
    main()
