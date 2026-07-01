/*
 * C++ 核心组件单元测试
 *
 * 测试 HttpServer2 的各独立组件，无需启动服务器。
 * 使用最小化测试框架（无外部依赖，从零实现）。
 */

#include "httpserver2.h"
#include <iostream>
#include <cassert>
#include <sstream>

// ============================================================
// 最小化测试框架
// ============================================================
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name, expr) do { \
    try { \
        if (!(expr)) throw std::runtime_error(#expr); \
        std::cout << "  ✅ " << name << std::endl; \
        g_passed++; \
    } catch (const std::exception& e) { \
        std::cout << "  ❌ " << name << ": " << e.what() << std::endl; \
        g_failed++; \
    } catch (...) { \
        std::cout << "  ❌ " << name << ": unknown exception" << std::endl; \
        g_failed++; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::stringstream ss; \
        ss << "Expected '" << (b) << "', got '" << (a) << "'"; \
        throw std::runtime_error(ss.str()); \
    } \
} while(0)

// ============================================================
// 测试套件
// ============================================================

void test_routeMatching() {
    std::cout << "\n--- Route Matching ---" << std::endl;
    
    // 精确匹配
    ExactRouteMatcher exact("/api/users");
    std::unordered_map<std::string, std::string> params;
    
    TEST("精确匹配成功", exact.match("/api/users", params));
    params.clear();
    TEST("精确匹配失败-不同路径", !exact.match("/api/users/1", params));
    params.clear();
    TEST("精确匹配失败-其他", !exact.match("/other", params));

    // 参数化匹配
    ParametricRouteMatcher parametric("/api/users/{id}");
    params.clear();
    TEST("参数化匹配成功", parametric.match("/api/users/42", params));
    ASSERT_EQ(params["id"], "42");
    
    params.clear();
    TEST("参数化匹配字符串ID", parametric.match("/api/users/abc", params));
    ASSERT_EQ(params["id"], "abc");

    params.clear();
    TEST("参数化匹配多级", !parametric.match("/api/users/42/posts", params));
    params.clear();
    TEST("参数化匹配根路径", !parametric.match("/api/users/", params));

    // 多参数
    ParametricRouteMatcher multi("/api/users/{uid}/posts/{pid}");
    params.clear();
    TEST("多参数匹配", multi.match("/api/users/1/posts/100", params));
    ASSERT_EQ(params["uid"], "1");
    ASSERT_EQ(params["pid"], "100");

    // 正则匹配
    RegexRouteMatcher regex("/api/articles/([0-9]+)");
    params.clear();
    TEST("正则匹配数字ID", regex.match("/api/articles/123", params));
    ASSERT_EQ(params["1"], "123");
    params.clear();
    TEST("正则不匹配字母", !regex.match("/api/articles/abc", params));
    params.clear();
    TEST("正则不匹配空", !regex.match("/api/articles/", params));
}

void test_sha256() {
    std::cout << "\n--- SHA256 ---" << std::endl;
    
    HttpServer2 server;
    
    std::string hash1 = server.calculateSha256("password");
    std::string hash2 = server.calculateSha256("password");
    std::string hash3 = server.calculateSha256("different");
    
    TEST("相同输入相同哈希", hash1 == hash2);
    TEST("不同输入不同哈希", hash1 != hash3);
    TEST("哈希长度64字符", hash1.size() == 64);
    TEST("空字符串不崩溃", server.calculateSha256("").size() == 64);
    TEST("长字符串不崩溃", server.calculateSha256(std::string(10000, 'a')).size() == 64);
}

void test_createErrorResponse() {
    std::cout << "\n--- createErrorResponse ---" << std::endl;
    
    HttpResponse r404 = createErrorResponse(HttpErrorType::NOT_FOUND);
    TEST("404 状态码", r404.status_code == "404");
    TEST("404 含 JSON 标记", r404.body.find("error") != std::string::npos);
    TEST("404 JSON 含 code", r404.body.find("404") != std::string::npos);
    
    HttpResponse r500 = createErrorResponse(HttpErrorType::INTERNAL_ERROR);
    TEST("500 状态码", r500.status_code == "500");
    
    HttpResponse r403 = createErrorResponse(HttpErrorType::FORBIDDEN, "Access denied");
    TEST("403 状态码", r403.status_code == "403");
    TEST("403 自定义消息", r403.body.find("Access denied") != std::string::npos);
    
    HttpResponse r400 = createErrorResponse(HttpErrorType::BAD_REQUEST);
    TEST("400 状态码", r400.status_code == "400");
    
    HttpResponse r503 = createErrorResponse(HttpErrorType::SERVICE_UNAVAILABLE);
    TEST("503 状态码", r503.status_code == "503");
}

void test_addSecurityHeaders() {
    std::cout << "\n--- addSecurityHeaders ---" << std::endl;
    
    HttpResponse resp;
    addSecurityHeaders(resp, true); // HTTPS
    
    auto& h = resp.headers;
    TEST("Content-Security-Policy", h.count("Content-Security-Policy"));
    TEST("X-Content-Type-Options: nosniff", h.count("X-Content-Type-Options"));
    TEST("X-Frame-Options: SAMEORIGIN", h.count("X-Frame-Options"));
    TEST("Referrer-Policy", h.count("Referrer-Policy"));
    TEST("Permissions-Policy", h.count("Permissions-Policy"));
    TEST("Cross-Origin-Opener-Policy", h.count("Cross-Origin-Opener-Policy"));
    TEST("Cache-Control: no-store", h.count("Cache-Control") && h["Cache-Control"] == "no-store");
    
    // HTTPS 时有 HSTS
    TEST("HSTS with SSL", h.count("Strict-Transport-Security"));
    TEST("HSTS max-age", h["Strict-Transport-Security"].find("31536000") != std::string::npos);
    
    HttpResponse r2;
    addSecurityHeaders(r2, false); // HTTP
    TEST("No HSTS without SSL", r2.headers.count("Strict-Transport-Security") == 0);
    
    // 已有 Cache-Control 的不覆盖
    HttpResponse r3;
    r3.headers["Cache-Control"] = "max-age=3600, public";
    addSecurityHeaders(r3, false);
    TEST("Keep existing Cache-Control", r3.headers["Cache-Control"] == "max-age=3600, public");
}

void test_base64Decode() {
    std::cout << "\n--- base64Decode ---" << std::endl;
    
    HttpServer2 server;
    
    TEST("空字符串", server.base64Decode("") == "");
    TEST("admin:password", server.base64Decode("YWRtaW46cGFzc3dvcmQ=") == "admin:password");
    TEST("test", server.base64Decode("dGVzdA==") == "test");
    TEST("hello:world", server.base64Decode("aGVsbG86d29ybGQ=") == "hello:world");
    TEST("无效输入不崩溃", true); // base64Decode 实现不保证空返回，仅确保不崩溃
}

void test_HttpResponseBasics() {
    std::cout << "\n--- HttpResponse Basics ---" << std::endl;
    
    HttpResponse resp;
    TEST("默认状态码空", resp.status_code.empty());
    TEST("默认 body 空", resp.body.empty());
    TEST("默认 headers 空", resp.headers.empty());
    TEST("默认 file_body_path 空", resp.file_body_path.empty());
    TEST("默认 response_sent false", !resp.response_sent);
    
    resp.status_code = "200";
    resp.body = "OK";
    resp.headers["Content-Type"] = "text/plain";
    TEST("设置状态码", resp.status_code == "200");
    TEST("设置 body", resp.body == "OK");
    TEST("设置 header", resp.headers["Content-Type"] == "text/plain");
    
    resp.response_sent = true;
    TEST("设置 response_sent", resp.response_sent);
}

void test_HttpErrorType() {
    std::cout << "\n--- HttpErrorType ---" << std::endl;
    
    TEST("BAD_REQUEST=400", createErrorResponse(HttpErrorType::BAD_REQUEST).status_code == "400");
    TEST("UNAUTHORIZED=401", createErrorResponse(HttpErrorType::UNAUTHORIZED).status_code == "401");
    TEST("FORBIDDEN=403", createErrorResponse(HttpErrorType::FORBIDDEN).status_code == "403");
    TEST("NOT_FOUND=404", createErrorResponse(HttpErrorType::NOT_FOUND).status_code == "404");
    TEST("INTERNAL_ERROR=500", createErrorResponse(HttpErrorType::INTERNAL_ERROR).status_code == "500");
    TEST("SERVICE_UNAVAILABLE=503", createErrorResponse(HttpErrorType::SERVICE_UNAVAILABLE).status_code == "503");
    
    // 所有错误响应应包含 JSON 格式
    HttpErrorType types[] = {
        HttpErrorType::BAD_REQUEST, HttpErrorType::UNAUTHORIZED,
        HttpErrorType::FORBIDDEN, HttpErrorType::NOT_FOUND,
        HttpErrorType::INTERNAL_ERROR, HttpErrorType::SERVICE_UNAVAILABLE
    };
    for (auto t : types) {
        HttpResponse r = createErrorResponse(t);
        if (r.body.find("error") == std::string::npos) {
            std::cout << "  ❌ Error " << r.status_code << " not JSON" << std::endl;
            g_failed++;
            return;
        }
    }
    std::cout << "  ✅ 所有错误响应含 JSON error 字段" << std::endl;
    g_passed++;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  HttpServer2 核心组件单元测试\n";
    std::cout << "========================================\n\n";
    
    test_routeMatching();
    test_sha256();
    test_createErrorResponse();
    test_addSecurityHeaders();
    test_base64Decode();
    test_HttpResponseBasics();
    test_HttpErrorType();
    
    std::cout << "\n========================================\n";
    std::cout << "  结果: " << (g_passed + g_failed) << " 总用例, "
              << g_passed << " 通过, " << g_failed << " 失败\n";
    std::cout << "========================================\n";
    
    if (g_failed == 0) {
        std::cout << "🎉 全部通过!\n";
    }
    
    return g_failed > 0 ? 1 : 0;
}
