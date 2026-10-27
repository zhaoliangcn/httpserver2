#include "jwt.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
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

// 恒定时间比较：普通 != 会在首个不同字节处提前返回，理论上可被用来
// 逐字节猜测签名（时序侧信道）。
static bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
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
    // 安全检查：HS256 模式下 secret 不能为空
    if (config.secret.empty()) {
        LOG_ERROR("JWT sign failed: HS256 secret is empty — set JWT_SECRET environment variable");
        return "";
    }
    
    // 只实现了 HS256。旧代码在 algorithm != "HS256" 时仍然用 HMAC 签名，
    // 但把 header 写成 alg=RS256 —— 签出来的 token 与声明算法不符，
    // 很难排查。这里直接拒绝。
    const std::string algorithm = config.algorithm.empty() ? "HS256" : config.algorithm;
    if (algorithm != "HS256") {
        LOG_ERROR("JWT sign failed: unsupported algorithm '" + algorithm + "' (only HS256 is implemented)");
        return "";
    }
    
    // Header
    std::string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
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
    std::string sig_raw = hmacSha256(signing_input, config.secret);
    std::string signature = base64UrlEncode(sig_raw);
    
    return header_b64 + "." + payload_b64 + "." + signature;
}

// ============================================================
// Token 验证
// ============================================================
std::optional<JwtClaims> Jwt::verify(const std::string& token, const JwtConfig& config) {
    // 关键安全检查：密钥为空时 HMAC 的 key 就是 ""，
    // 任何人都能在本地算出"合法"签名。sign() 早就有这个检查，verify() 之前没有，
    // 于是未设置 JWT_SECRET 的部署等于完全没有验签。
    if (config.secret.empty()) {
        LOG_ERROR("JWT verify failed: empty secret — refusing to verify (set JWT_SECRET environment variable)");
        return std::nullopt;
    }
    
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
    
    // 校验 header 中的 alg：只接受 HS256，避免算法混淆（alg=none / alg=RS256）。
    std::string header_json = base64UrlDecode(header_b64);
    std::string alg = extractJsonString(header_json, "alg");
    if (alg != "HS256") {
        LOG_WARNING("JWT: unsupported alg '" + alg + "'");
        return std::nullopt;
    }
    
    // 验证签名（恒定时间比较）
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string expected_sig_raw = hmacSha256(signing_input, config.secret);
    std::string expected_sig_b64 = base64UrlEncode(expected_sig_raw);
    
    if (!constantTimeEquals(sig_b64, expected_sig_b64)) {
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
    return [config](HttpRequest& req) -> std::optional<HttpResponse> {
        auto unauthorized = [](const std::string& msg) -> std::optional<HttpResponse> {
            HttpResponse resp;
            resp.status_code = "401";
            resp.status_message = "Unauthorized";
            resp.headers["Content-Type"] = "application/json";
            // msg 全部是编译期字面量，不含需要转义的字符
            resp.body = "{\"error\":\"" + msg + "\"}";
            resp.headers["WWW-Authenticate"] = "Bearer";
            return resp;
        };
        
        // 从 Authorization 头提取 token
        auto auth_it = req.headers.find("Authorization");
        if (auth_it == req.headers.end()) {
            return unauthorized("Missing Authorization header");
        }
        
        std::string token = Jwt::extractFromBearer(auth_it->second);
        if (token.empty()) {
            return unauthorized("Invalid Authorization header format");
        }
        
        auto claims = Jwt::verify(token, config);
        if (!claims) {
            return unauthorized("Invalid or expired token");
        }
        
        // 把 claims 写回请求头，下游 handler 直接读 req.headers["X-JWT-Subject"] 即可。
        // 中间件签名改为 HttpRequest& 正是为了支持这一点：
        // 旧实现只能把 claims 塞进响应头，而响应头根本到不了 handler。
        req.headers["X-JWT-Subject"] = claims->subject;
        req.headers["X-JWT-Issuer"] = claims->issuer;
        if (!claims->roles.empty()) {
            std::string roles_str;
            for (size_t i = 0; i < claims->roles.size(); ++i) {
                if (i > 0) roles_str += ",";
                roles_str += claims->roles[i];
            }
            req.headers["X-JWT-Roles"] = roles_str;
        }
        
        LOG_INFO("JWT auth success: sub=" + claims->subject);
        
        // nullopt = 放行到下一个中间件 / 最终 handler
        return std::nullopt;
    };
}

}} // namespace http2::auth