#include "validator.h"
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace http2 { namespace validation {

// ============================================================
// 类型检查辅助函数
// ============================================================
bool Validator::isEmail(const std::string& value) {
    // 简单邮箱验证：xxx@xxx.xxx
    static const std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");
    return std::regex_match(value, email_regex);
}

bool Validator::isIP(const std::string& value) {
    // IPv4
    static const std::regex ipv4_regex(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
    std::smatch match;
    if (std::regex_match(value, match, ipv4_regex)) {
        for (size_t i = 1; i <= 4; ++i) {
            int octet = std::stoi(match[i].str());
            if (octet < 0 || octet > 255) return false;
        }
        return true;
    }
    // IPv6 (简化检查)
    static const std::regex ipv6_regex(R"(^([0-9a-fA-F:]{2,39})$)");
    return std::regex_match(value, ipv6_regex);
}

bool Validator::isURL(const std::string& value) {
    static const std::regex url_regex(R"(^https?://[^\s/$.?#].[^\s]*$)");
    return std::regex_match(value, url_regex);
}

bool Validator::isInteger(const std::string& value) {
    if (value.empty()) return false;
    size_t start = 0;
    if (value[0] == '-' || value[0] == '+') start = 1;
    if (start >= value.size()) return false;
    for (size_t i = start; i < value.size(); ++i) {
        if (!std::isdigit(value[i])) return false;
    }
    return true;
}

bool Validator::isFloat(const std::string& value) {
    if (value.empty()) return false;
    std::regex float_regex(R"(^[-+]?[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?$)");
    return std::regex_match(value, float_regex);
}

bool Validator::isBool(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "true" || lower == "false" || lower == "1" || lower == "0";
}

bool Validator::matchPattern(const std::string& value, const std::string& pattern) {
    try {
        std::regex re(pattern);
        return std::regex_match(value, re);
    } catch (const std::regex_error&) {
        return false;
    }
}

// ============================================================
// 单字段验证
// ============================================================
std::string Validator::validateField(const std::string& value, const Rule& rule) {
    std::string default_msg;
    
    switch (rule.type) {
        case RuleType::REQUIRED:
            if (value.empty()) {
                default_msg = rule.field + " is required";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::STRING:
            // 任何值都是字符串（除非是 REQUIRED 检查）
            break;
            
        case RuleType::INT:
            if (!value.empty() && !isInteger(value)) {
                default_msg = rule.field + " must be an integer";
                return rule.message.empty() ? default_msg : rule.message;
            }
            // 检查 MIN/MAX（如果 param 包含在规则中）
            break;
            
        case RuleType::FLOAT:
            if (!value.empty() && !isFloat(value)) {
                default_msg = rule.field + " must be a number";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::BOOL:
            if (!value.empty() && !isBool(value)) {
                default_msg = rule.field + " must be true/false";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::EMAIL:
            if (!value.empty() && !isEmail(value)) {
                default_msg = rule.field + " must be a valid email";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::IP:
            if (!value.empty() && !isIP(value)) {
                default_msg = rule.field + " must be a valid IP address";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::URL:
            if (!value.empty() && !isURL(value)) {
                default_msg = rule.field + " must be a valid URL";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::MIN: {
            if (value.empty()) break;
            // 尝试数值比较
            if (isFloat(value)) {
                double val = std::stod(value);
                double min_val = std::stod(rule.param);
                if (val < min_val) {
                    default_msg = rule.field + " must be >= " + rule.param;
                    return rule.message.empty() ? default_msg : rule.message;
                }
            } else {
                // 字符串长度比较
                if (value.size() < std::stoul(rule.param)) {
                    default_msg = rule.field + " length must be >= " + rule.param;
                    return rule.message.empty() ? default_msg : rule.message;
                }
            }
            break;
        }
        
        case RuleType::MAX: {
            if (value.empty()) break;
            if (isFloat(value)) {
                double val = std::stod(value);
                double max_val = std::stod(rule.param);
                if (val > max_val) {
                    default_msg = rule.field + " must be <= " + rule.param;
                    return rule.message.empty() ? default_msg : rule.message;
                }
            } else {
                if (value.size() > std::stoul(rule.param)) {
                    default_msg = rule.field + " length must be <= " + rule.param;
                    return rule.message.empty() ? default_msg : rule.message;
                }
            }
            break;
        }
        
        case RuleType::LENGTH: {
            // param 格式: "min-max"
            auto dash = rule.param.find('-');
            if (dash != std::string::npos) {
                size_t min_len = std::stoul(rule.param.substr(0, dash));
                size_t max_len = std::stoul(rule.param.substr(dash + 1));
                if (value.size() < min_len || value.size() > max_len) {
                    default_msg = rule.field + " length must be between " + rule.param;
                    return rule.message.empty() ? default_msg : rule.message;
                }
            }
            break;
        }
        
        case RuleType::PATTERN:
            if (!value.empty() && !matchPattern(value, rule.param)) {
                default_msg = rule.field + " format is invalid";
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
            
        case RuleType::ENUM: {
            // param 格式: "option1,option2,option3"
            std::stringstream ss(rule.param);
            std::string option;
            bool found = false;
            while (std::getline(ss, option, ',')) {
                // 去除首尾空格
                option.erase(0, option.find_first_not_of(" \t"));
                option.erase(option.find_last_not_of(" \t") + 1);
                if (option == value) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                default_msg = rule.field + " must be one of: " + rule.param;
                return rule.message.empty() ? default_msg : rule.message;
            }
            break;
        }
        
        case RuleType::CUSTOM:
            // 自定义验证由调用方处理，这里不执行
            break;
    }
    
    return ""; // 验证通过
}

// ============================================================
// 请求验证
// ============================================================
ValidationResult Validator::validate(const HttpRequest& request, const std::vector<Rule>& rules) {
    ValidationResult result;
    
    for (const auto& rule : rules) {
        std::string value;
        
        // 从不同来源获取值
        if (request.form_data.count(rule.field)) {
            value = request.form_data.at(rule.field);
        } else if (request.query_params.count(rule.field)) {
            value = request.query_params.at(rule.field);
        } else if (request.json_body.count(rule.field)) {
            value = request.json_body.at(rule.field);
        }
        
        std::string error = validateField(value, rule);
        if (!error.empty()) {
            result.addError(error);
        }
    }
    
    return result;
}

ValidationResult Validator::validateJson(const std::unordered_map<std::string, std::string>& json_data,
                                          const std::vector<Rule>& rules) {
    ValidationResult result;
    
    for (const auto& rule : rules) {
        std::string value;
        auto it = json_data.find(rule.field);
        if (it != json_data.end()) {
            value = it->second;
        }
        
        std::string error = validateField(value, rule);
        if (!error.empty()) {
            result.addError(error);
        }
    }
    
    return result;
}

ValidationResult Validator::validateForm(const std::unordered_map<std::string, std::string>& form_data,
                                          const std::vector<Rule>& rules) {
    ValidationResult result;
    
    for (const auto& rule : rules) {
        std::string value;
        auto it = form_data.find(rule.field);
        if (it != form_data.end()) {
            value = it->second;
        }
        
        std::string error = validateField(value, rule);
        if (!error.empty()) {
            result.addError(error);
        }
    }
    
    return result;
}

// ============================================================
// 验证中间件
// ============================================================
Middleware validateMiddleware(const std::vector<Rule>& rules) {
    return [rules](const HttpRequest& req) -> HttpResponse {
        ValidationResult result = Validator::validate(req, rules);
        
        if (!result.valid) {
            // 构建错误响应
            std::string body = "{\"error\":\"Validation failed\",\"details\":[";
            for (size_t i = 0; i < result.errors.size(); ++i) {
                if (i > 0) body += ",";
                body += "\"" + result.errors[i] + "\"";
            }
            body += "]}";
            
            HttpResponse resp;
            resp.status_code = "400";
            resp.status_message = "Bad Request";
            resp.headers["Content-Type"] = "application/json";
            resp.body = body;
            return resp;
        }
        
        // 验证通过，返回空响应让中间件链继续
        HttpResponse resp;
        resp.status_code = "200";
        resp.status_message = "OK";
        return resp;
    };
}

}} // namespace http2::validation