#ifndef HTTPSERVER2_VALIDATOR_H
#define HTTPSERVER2_VALIDATOR_H

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include "../httpserver2.h"

namespace http2 { namespace validation {

// 验证规则类型
enum class RuleType {
    REQUIRED,   // 必填
    STRING,     // 字符串类型
    INT,        // 整数类型
    FLOAT,      // 浮点数类型
    BOOL,       // 布尔类型
    EMAIL,      // 邮箱格式
    IP,         // IP 地址
    URL,        // URL 格式
    MIN,        // 最小值（数值）或最小长度（字符串）
    MAX,        // 最大值（数值）或最大长度（字符串）
    LENGTH,     // 长度范围 "min-max"
    PATTERN,    // 正则匹配
    ENUM,       // 枚举值
    CUSTOM      // 自定义验证函数
};

// 验证规则
struct Rule {
    std::string field;       // 字段名
    RuleType type;           // 规则类型
    std::string param;       // 参数（如 MIN 的数值、PATTERN 的正则、ENUM 的选项列表）
    std::string message;     // 自定义错误消息
    
    Rule() = default;
    Rule(const std::string& f, RuleType t, const std::string& p = "", const std::string& m = "")
        : field(f), type(t), param(p), message(m) {}
};

// 验证结果
struct ValidationResult {
    bool valid = true;
    std::vector<std::string> errors;
    
    void addError(const std::string& error) {
        valid = false;
        errors.push_back(error);
    }
};

// 验证器
class Validator {
public:
    // 验证请求
    static ValidationResult validate(const HttpRequest& request, const std::vector<Rule>& rules);
    
    // 便捷方法：验证 JSON body
    static ValidationResult validateJson(const std::unordered_map<std::string, std::string>& json_data,
                                         const std::vector<Rule>& rules);
    
    // 便捷方法：验证表单数据
    static ValidationResult validateForm(const std::unordered_map<std::string, std::string>& form_data,
                                          const std::vector<Rule>& rules);

private:
    // 单字段验证
    static std::string validateField(const std::string& value, const Rule& rule);
    
    // 类型检查
    static bool isEmail(const std::string& value);
    static bool isIP(const std::string& value);
    static bool isURL(const std::string& value);
    static bool isInteger(const std::string& value);
    static bool isFloat(const std::string& value);
    static bool isBool(const std::string& value);
    static bool matchPattern(const std::string& value, const std::string& pattern);
};

// 验证中间件工厂
Middleware validateMiddleware(const std::vector<Rule>& rules);

}} // namespace http2::validation

#endif // HTTPSERVER2_VALIDATOR_H