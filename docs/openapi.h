#ifndef HTTPSERVER2_OPENAPI_H
#define HTTPSERVER2_OPENAPI_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "../httpserver2.h"

namespace http2 { namespace docs {

// OpenAPI 参数
struct ApiParameter {
    std::string name;
    std::string in = "query";  // query | path | header | cookie
    std::string description;
    bool required = false;
    std::string type = "string";  // string | integer | boolean | array
    std::string example;
};

// OpenAPI 响应
struct ApiResponse {
    std::string status_code = "200";
    std::string description = "OK";
    std::string content_type = "application/json";
    std::string schema;  // 简化的 schema 描述
};

// API 端点信息
struct ApiEndpoint {
    std::string method;
    std::string path;
    std::string summary;
    std::string description;
    std::vector<ApiParameter> parameters;
    std::vector<ApiResponse> responses;
    std::vector<std::string> tags;
    bool deprecated = false;
};

// OpenAPI 文档
class OpenApiDoc {
private:
    std::string title_ = "HttpServer2 API";
    std::string version_ = "2.5";
    std::string description_ = "HttpServer2 API Documentation";
    std::string host_ = "localhost:8080";
    std::vector<std::string> schemes_ = {"http", "https"};
    std::vector<ApiEndpoint> endpoints_;

public:
    void setTitle(const std::string& title) { title_ = title; }
    void setVersion(const std::string& version) { version_ = version; }
    void setDescription(const std::string& desc) { description_ = desc; }
    void setHost(const std::string& host) { host_ = host; }
    
    // 注册端点
    void addEndpoint(const ApiEndpoint& endpoint);
    
    // 生成 OpenAPI 3.0 JSON
    std::string toJson() const;
    
    // 生成 HTML 文档页面
    std::string toHtml() const;
    
    // 从已注册的路由生成文档（简化版）
    void generateFromRoutes(HttpServer2* server);
};

// 注册 API 文档路由的便捷函数
void registerApiDocs(HttpServer2* server, std::shared_ptr<OpenApiDoc> doc, 
                     const std::string& base_path = "/api/docs");

}} // namespace http2::docs

#endif // HTTPSERVER2_OPENAPI_H