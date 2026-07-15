#ifndef HTTPSERVER2_JWT_H
#define HTTPSERVER2_JWT_H

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <optional>
#include <functional>
#include "../httpserver2.h"

namespace http2 { namespace auth {

// JWT 配置
struct JwtConfig {
    std::string secret = "change-me";           // HS256 密钥
    std::string public_key = "";                // RS256 公钥
    std::string private_key = "";               // RS256 私钥
    std::string issuer = "httpserver2";
    std::chrono::seconds ttl = std::chrono::hours(2);
    std::string algorithm = "HS256";            // HS256 | RS256
    std::string audience = "";
};

// JWT 声明
struct JwtClaims {
    std::string subject;                         // sub
    std::string issuer;                          // iss
    std::vector<std::string> roles;              // 自定义角色
    std::map<std::string, std::string> custom;   // 自定义声明
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point expires_at;
    std::string jti;                             // JWT ID
    std::string audience;                        // aud
    
    JwtClaims() {
        issued_at = std::chrono::system_clock::now();
        expires_at = issued_at + std::chrono::hours(2);
    }
};

// JWT 核心类
class Jwt {
public:
    // 签发 token
    static std::string sign(const JwtClaims& claims, const JwtConfig& config);
    
    // 验证 token，返回 claims（失败返回 nullopt）
    static std::optional<JwtClaims> verify(const std::string& token, const JwtConfig& config);
    
    // 从 Bearer 头提取 token
    static std::string extractFromBearer(const std::string& auth_header);
    
private:
    // Base64url 编码/解码
    static std::string base64UrlEncode(const std::string& input);
    static std::string base64UrlDecode(const std::string& input);
    
    // HMAC-SHA256 签名
    static std::string hmacSha256(const std::string& data, const std::string& key);
    
    // JSON 序列化/反序列化（简单实现，无需 nlohmann/json）
    static std::string claimsToJson(const JwtClaims& claims);
    static JwtClaims jsonToClaims(const std::string& json);
    static std::string escapeJson(const std::string& s);
};

// JWT 认证中间件工厂
Middleware jwtAuthMiddleware(const JwtConfig& config);

}} // namespace http2::auth

#endif // HTTPSERVER2_JWT_H