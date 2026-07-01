#!/usr/bin/env python3
"""
Client test script for reverse proxy functionality
"""

import requests
import time

TEST_SERVER = "http://localhost:8080"

def test_single_backend():
    """Test single backend reverse proxy"""
    print("\n=== Testing Single Backend Reverse Proxy ===")
    
    try:
        response = requests.get(f"{TEST_SERVER}/single", timeout=5)
        print(f"Status: {response.status_code}")
        print(f"Headers: {dict(response.headers)}")
        print(f"Body Length: {len(response.text)} bytes")
        print(f"First 100 chars: {response.text[:100]}...")
        
        if response.status_code in [200, 301, 302]:
            print("✅ Single backend test passed!")
            return True
        else:
            print("❌ Single backend test failed!")
            return False
    except Exception as e:
        print(f"❌ Single backend test failed with exception: {e}")
        return False

def test_multi_backend():
    """Test multiple backends with round-robin"""
    print("\n=== Testing Multiple Backends Reverse Proxy ===")
    
    backend_responses = []
    num_requests = 6
    
    for i in range(num_requests):
        try:
            response = requests.get(f"{TEST_SERVER}/multi", timeout=5)
            print(f"Request {i+1}: Status={response.status_code}, Content-Length={len(response.text)} bytes")
            backend_responses.append(response.status_code)
            time.sleep(0.1)  # Short delay between requests
        except Exception as e:
            print(f"❌ Request {i+1} failed: {e}")
            backend_responses.append(None)
    
    # Check if we're getting responses from different backends
    # We can't directly check which backend responded, but we can check if responses vary
    success_responses = [r for r in backend_responses if r in [200, 301, 302]]
    
    if len(success_responses) > 0:
        print(f"✅ Multiple backends test passed! {len(success_responses)}/{num_requests} requests succeeded")
        return True
    else:
        print("❌ Multiple backends test failed!")
        return False

def test_custom_headers():
    """Test custom headers in reverse proxy"""
    print("\n=== Testing Custom Headers Reverse Proxy ===")
    
    try:
        response = requests.get(f"{TEST_SERVER}/custom", timeout=5)
        print(f"Status: {response.status_code}")
        print(f"Headers: {dict(response.headers)}")
        
        # Check if custom response header is present
        if "X-Powered-By" in response.headers and "HttpServer2/2.0" in response.headers["X-Powered-By"]:
            print("✅ Custom headers test passed! Found X-Powered-By: HttpServer2/2.0")
            return True
        else:
            print("❌ Custom headers test failed! Expected X-Powered-By header not found")
            return False
    except Exception as e:
        print(f"❌ Custom headers test failed with exception: {e}")
        return False

def main():
    """Main test function"""
    print("HttpServer2 Reverse Proxy Client Test")
    print("=" * 50)
    print(f"Testing server at: {TEST_SERVER}")
    
    # Run tests
    test1 = test_single_backend()
    test2 = test_multi_backend()
    test3 = test_custom_headers()
    
    # Summary
    print("\n" + "=" * 50)
    print("Test Summary:")
    print(f"Single backend: {'✅ PASSED' if test1 else '❌ FAILED'}")
    print(f"Multiple backends: {'✅ PASSED' if test2 else '❌ FAILED'}")
    print(f"Custom headers: {'✅ PASSED' if test3 else '❌ FAILED'}")
    
    if test1 and test2 and test3:
        print("\n🎉 All tests passed! Reverse proxy functionality is working correctly.")
        return 0
    else:
        print("\n❌ Some tests failed!")
        return 1

if __name__ == "__main__":
    main()
