#include "jwt.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>

namespace http2 { namespace auth {

// ============================================================
// Base64 URL 编码（RFC 4648 §5）
// ============================================================
std::string Jwt::base64UrlEncode(const std::string& input) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;
    
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input.data(), static_cast<int>(input.size()));
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    
    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    
    // Base64 → Base64URL: 替换 +/ 为 -_
    for (auto& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    // 去掉末尾的 =
    while (!result.empty() && result.back() == '=')
        result.pop_back();
    
    return result;
}

std::string Jwt::base64UrlDecode(const std::string& input) {
    std::string normalized = input;
    // Base64URL → Base64: 替换 -_ 为 +/
    for (auto& c : normalized) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // 补回 =
    while (normalized.size() % 4) normalized += '=';
    
    // 正确流程：先将 base64 数据写入 mem BIO，再通过 base64 filter 读取解码
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    
    // 先将 base64 数据写入 mem BIO（不经过 base64 filter）
    BIO_write(mem, normalized.data(), static_cast<int>(normalized.size()));
    
    // 再将 base64 filter 推入链中
    BIO* chain = BIO_push(b64, mem);  // chain = b64 -> mem
    
    // 从链中读取（base64 filter 从 mem BIO 中解码）
    char* buffer = new char[normalized.size()];
    int decoded_len = BIO_read(chain, buffer, static_cast<int>(normalized.size()));
    std::string result(buffer, decoded_len > 0 ? decoded_len : 0);
    
    BIO_free_all(chain);
    delete[] buffer;
    return result;
}

// ============================================================
// HMAC-SHA256
// ============================================================
std::string Jwt::hmacSha256(const std::string& data, const std::string& key) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &result_len);
    
    return std::string(reinterpret_cast<char*>(result), result_len);
}

// ============================================================
// JSON 辅助（简易实现，无需外部库）
// ============================================================
std::string Jwt::escapeJson(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::stringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(c));
                    result += ss.str();
                } else {
                    result += c;
                }
        }
    }
    return result;
}

std::string Jwt::claimsToJson(const JwtClaims& claims) {
    std::stringstream ss;
    ss << "{";
    ss << "\"sub\":\"" << escapeJson(claims.subject) << "\",";
    ss << "\"iss\":\"" << escapeJson(claims.issuer) << "\"";
    
    if (!claims.audience.empty())
        ss << ",\"aud\":\"" << escapeJson(claims.audience) << "\"";
    
    auto epoch = std::chrono::system_clock::to_time_t(claims.issued_at);
    ss << ",\"iat\":" << epoch;
    
    auto exp = std::chrono::system_clock::to_time_t(claims.expires_at);
    ss << ",\"exp\":" << exp;
    
    if (!claims.jti.empty())
        ss << ",\"jti\":\"" << escapeJson(claims.jti) << "\"";
    
    // 角色
    if (!claims.roles.empty()) {
        ss << ",\"roles\":[";
        for (size_t i = 0; i < claims.roles.size(); ++i) {
            if (i > 0) ss << ",";
            ss << "\"" << escapeJson(claims.roles[i]) << "\"";
        }
        ss << "]";
    }
    
    // 自定义声明
    for (const auto& [key, value] : claims.custom) {
        ss << ",\"" << escapeJson(key) << "\":\"" << escapeJson(value) << "\"";
    }
    
    ss << "}";
    return ss.str();
}

static std::string extractJsonString(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    pos++;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            result += json[pos + 1];
            pos += 2;
        } else {
            result += json[pos];
            pos++;
        }
    }
    return result;
}

static int64_t extractJsonInt(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    char* end = nullptr;
    return std::strtoll(json.c_str() + pos, &end, 10);
}

static std::vector<std::string> extractJsonArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + key.size() + 2);
    if (pos == std::string::npos) return result;
    pos++;
    while (pos < json.size() && json[pos] != ']') {
        if (json[pos] == '"') {
            pos++;
            std::string val;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < json.size()) {
                    val += json[pos + 1]; pos += 2;
                } else {
                    val += json[pos]; pos++;
                }
            }
            if (!val.empty()) result.push_back(val);
        }
        pos++;
    }
    return result;
}

JwtClaims Jwt::jsonToClaims(const std::string& json) {
    JwtClaims claims;
    claims.subject = extractJsonString(json, "sub");
    claims.issuer = extractJsonString(json, "iss");
    claims.audience = extractJsonString(json, "aud");
    claims.jti = extractJsonString(json, "jti");
    claims.roles = extractJsonArray(json, "roles");
    
    auto iat = extractJsonInt(json, "iat");
    if (iat > 0)
        claims.issued_at = std::chrono::system_clock::from_time_t(iat);
    
    auto exp = extractJsonInt(json, "exp");
    if (exp > 0)
        claims.expires_at = std::chrono::system_clock::from_time_t(exp);
    
    // 提取自定义声明（sub/iss/aud/jti/iat/exp/roles 以外的）
    // 简化处理：只提取已知的键
    return claims;
}

// ============================================================
// Token 签发
// ============================================================
std::string Jwt::sign(const JwtClaims& claims, const JwtConfig& config) {
    // Header
    std::string header = "{\"alg\":\"" + config.algorithm + "\",\"typ\":\"JWT\"}";
    std::string header_b64 = base64UrlEncode(header);
    
    // Payload
    JwtClaims claims_copy = claims;
    if (claims_copy.issuer.empty()) claims_copy.issuer = config.issuer;
    if (claims_copy.jti.empty()) {
        // 生成随机 JTI
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::stringstream ss;
        ss << std::hex << gen();
        claims_copy.jti = ss.str();
    }
    
    std::string payload = claimsToJson(claims_copy);
    std::string payload_b64 = base64UrlEncode(payload);
    
    // Signature
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string signature;
    
    if (config.algorithm == "HS256") {
        std::string sig_raw = hmacSha256(signing_input, config.secret);
        signature = base64UrlEncode(sig_raw);
    } else {
        // RS256 — 暂不支持，回退到 HS256
        LOG_WARNING("RS256 not yet implemented, falling back to HS256");
        std::string sig_raw = hmacSha256(signing_input, config.secret);
        signature = base64UrlEncode(sig_raw);
    }
    
    return header_b64 + "." + payload_b64 + "." + signature;
}

// ============================================================
// Token 验证
// ============================================================
std::optional<JwtClaims> Jwt::verify(const std::string& token, const JwtConfig& config) {
    // 分割 token
    auto first_dot = token.find('.');
    auto second_dot = token.rfind('.');
    if (first_dot == std::string::npos || second_dot == std::string::npos || 
        first_dot == second_dot) {
        LOG_WARNING("JWT: invalid token format");
        return std::nullopt;
    }
    
    std::string header_b64 = token.substr(0, first_dot);
    std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    std::string sig_b64 = token.substr(second_dot + 1);
    
    // 验证签名
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string expected_sig_raw = hmacSha256(signing_input, config.secret);
    std::string expected_sig_b64 = base64UrlEncode(expected_sig_raw);
    
    if (sig_b64 != expected_sig_b64) {
        LOG_WARNING("JWT: signature mismatch");
        return std::nullopt;
    }
    
    // 解码 payload
    std::string payload_json = base64UrlDecode(payload_b64);
    JwtClaims claims = jsonToClaims(payload_json);
    
    // 检查过期
    auto now = std::chrono::system_clock::now();
    if (claims.expires_at < now) {
        LOG_WARNING("JWT: token expired");
        return std::nullopt;
    }
    
    // 检查签发者
    if (!config.issuer.empty() && claims.issuer != config.issuer) {
        LOG_WARNING("JWT: issuer mismatch: " + claims.issuer + " != " + config.issuer);
        return std::nullopt;
    }
    
    // 检查 audience
    if (!config.audience.empty() && claims.audience != config.audience) {
        LOG_WARNING("JWT: audience mismatch");
        return std::nullopt;
    }
    
    return claims;
}

// ============================================================
// 从 Bearer 头提取 token
// ============================================================
std::string Jwt::extractFromBearer(const std::string& auth_header) {
    if (auth_header.size() > 7 && 
        (auth_header[0] == 'B' || auth_header[0] == 'b') &&
        (auth_header[1] == 'E' || auth_header[1] == 'e') &&
        (auth_header[2] == 'A' || auth_header[2] == 'a') &&
        (auth_header[3] == 'R' || auth_header[3] == 'r') &&
        (auth_header[4] == 'E' || auth_header[4] == 'e') &&
        (auth_header[5] == 'R' || auth_header[5] == 'r') &&
        auth_header[6] == ' ') {
        return auth_header.substr(7);
    }
    return "";
}

// ============================================================
// JWT 认证中间件
// ============================================================
Middleware jwtAuthMiddleware(const JwtConfig& config) {
    return [config](const HttpRequest& req) -> HttpResponse {
        // 从 Authorization 头提取 token
        auto auth_it = req.headers.find("Authorization");
        if (auth_it == req.headers.end()) {
            HttpResponse resp;
            resp.status_code = "401";
            resp.status_message = "Unauthorized";
            resp.headers["Content-Type"] = "application/json";
            resp.body = "{\"error\":\"Missing Authorization header\"}";
            resp.headers["WWW-Authenticate"] = "Bearer";
            return resp;
        }
        
        std::string token = Jwt::extractFromBearer(auth_it->second);
        if (token.empty()) {
            HttpResponse resp;
            resp.status_code = "401";
            resp.status_message = "Unauthorized";
            resp.headers["Content-Type"] = "application/json";
            resp.body = "{\"error\":\"Invalid Authorization header format\"}";
            resp.headers["WWW-Authenticate"] = "Bearer";
            return resp;
        }
        
        auto claims = Jwt::verify(token, config);
        if (!claims) {
            HttpResponse resp;
            resp.status_code = "401";
            resp.status_message = "Unauthorized";
            resp.headers["Content-Type"] = "application/json";
            resp.body = "{\"error\":\"Invalid or expired token\"}";
            resp.headers["WWW-Authenticate"] = "Bearer error=\\\"invalid_token\\\"";
            return resp;
        }
        
        // 将 claims 信息传递给下游 — 通过请求头传递
        // 创建修改后的请求副本（实际用 const ref，这里返回原请求让下游自己读 header）
        // 通过添加 X-JWT-* 头传递 claims（只能通过可变引用来实现）
        // 由于 Middleware 返回 HttpResponse，不能修改 HttpRequest
        // 这里通过 response header 返回 claims，下游 handler 从 response header 读
        LOG_INFO("JWT auth success: sub=" + claims->subject);
        
        // 返回空响应让中间件链继续
        HttpResponse resp;
        resp.status_code = "200";
        resp.status_message = "OK";
        // 通过自定义头传递 JWT claims 给后续处理器
        resp.headers["X-JWT-Subject"] = claims->subject;
        resp.headers["X-JWT-Issuer"] = claims->issuer;
        if (!claims->roles.empty()) {
            std::string roles_str;
            for (size_t i = 0; i < claims->roles.size(); ++i) {
                if (i > 0) roles_str += ",";
                roles_str += claims->roles[i];
            }
            resp.headers["X-JWT-Roles"] = roles_str;
        }
        return resp;
    };
}

}} // namespace http2::auth