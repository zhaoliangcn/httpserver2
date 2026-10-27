#include "openapi.h"
#include <sstream>
#include <iomanip>

namespace http2 { namespace docs {

// ============================================================
// OpenAPI 文档实现
// ============================================================
void OpenApiDoc::addEndpoint(const ApiEndpoint& endpoint) {
    endpoints_.push_back(endpoint);
}

std::string OpenApiDoc::toJson() const {
    std::stringstream ss;
    ss << "{\n";
    ss << "  \"openapi\": \"3.0.3\",\n";
    ss << "  \"info\": {\n";
    ss << "    \"title\": \"" << title_ << "\",\n";
    ss << "    \"version\": \"" << version_ << "\",\n";
    ss << "    \"description\": \"" << description_ << "\"\n";
    ss << "  },\n";
    ss << "  \"servers\": [{\n";
    ss << "    \"url\": \"" << schemes_[0] << "://" << host_ << "\"\n";
    ss << "  }],\n";
    ss << "  \"paths\": {\n";
    
    // 按路径分组
    std::map<std::string, std::vector<const ApiEndpoint*>> path_groups;
    for (const auto& ep : endpoints_) {
        path_groups[ep.path].push_back(&ep);
    }
    
    bool first_path = true;
    for (const auto& [path, eps] : path_groups) {
        if (!first_path) ss << ",\n";
        first_path = false;
        
        ss << "    \"" << path << "\": {\n";
        bool first_method = true;
        for (const auto* ep : eps) {
            if (!first_method) ss << ",\n";
            first_method = false;
            
            std::string method_lower = ep->method;
            std::transform(method_lower.begin(), method_lower.end(), method_lower.begin(), ::tolower);
            
            ss << "      \"" << method_lower << "\": {\n";
            ss << "        \"summary\": \"" << ep->summary << "\",\n";
            if (!ep->description.empty())
                ss << "        \"description\": \"" << ep->description << "\",\n";
            if (ep->deprecated)
                ss << "        \"deprecated\": true,\n";
            
            // 参数
            if (!ep->parameters.empty()) {
                ss << "        \"parameters\": [\n";
                for (size_t i = 0; i < ep->parameters.size(); ++i) {
                    if (i > 0) ss << ",\n";
                    ss << "          {\n";
                    ss << "            \"name\": \"" << ep->parameters[i].name << "\",\n";
                    ss << "            \"in\": \"" << ep->parameters[i].in << "\",\n";
                    ss << "            \"description\": \"" << ep->parameters[i].description << "\",\n";
                    ss << "            \"required\": " << (ep->parameters[i].required ? "true" : "false") << ",\n";
                    ss << "            \"schema\": {\"type\": \"" << ep->parameters[i].type << "\"}\n";
                    ss << "          }";
                }
                ss << "\n        ],\n";
            }
            
            // 响应
            ss << "        \"responses\": {\n";
            for (size_t i = 0; i < ep->responses.size(); ++i) {
                if (i > 0) ss << ",\n";
                ss << "          \"" << ep->responses[i].status_code << "\": {\n";
                ss << "            \"description\": \"" << ep->responses[i].description << "\"\n";
                ss << "          }";
            }
            ss << "\n        }\n";
            ss << "      }";
        }
        ss << "\n    }";
    }
    
    ss << "\n  }\n";
    ss << "}\n";
    return ss.str();
}

std::string OpenApiDoc::toHtml() const {
    std::stringstream ss;
    ss << "<!DOCTYPE html>\n";
    ss << "<html lang=\"zh-CN\">\n<head>\n";
    ss << "<meta charset=\"UTF-8\">\n";
    ss << "<title>" << title_ << " - API Documentation</title>\n";
    ss << "<style>\n";
    ss << "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; ";
    ss << "max-width: 1200px; margin: 0 auto; padding: 20px; background: #f5f5f5; }\n";
    ss << "h1 { color: #333; border-bottom: 2px solid #4A90D9; padding-bottom: 10px; }\n";
    ss << ".endpoint { background: white; border-radius: 8px; padding: 16px; margin: 12px 0; ";
    ss << "box-shadow: 0 1px 3px rgba(0,0,0,0.1); }\n";
    ss << ".method { display: inline-block; padding: 4px 12px; border-radius: 4px; ";
    ss << "color: white; font-weight: bold; font-size: 14px; }\n";
    ss << ".method-get { background: #61AFFE; }\n";
    ss << ".method-post { background: #49CC90; }\n";
    ss << ".method-put { background: #FCA130; }\n";
    ss << ".method-delete { background: #F93E3E; }\n";
    ss << ".path { font-family: monospace; font-size: 16px; margin-left: 12px; }\n";
    ss << ".summary { margin-top: 8px; color: #666; }\n";
    ss << ".param { margin: 4px 0; font-size: 14px; }\n";
    ss << ".param-name { font-family: monospace; font-weight: bold; }\n";
    ss << ".tag { display: inline-block; padding: 2px 8px; border-radius: 12px; ";
    ss << "background: #E8E8E8; font-size: 12px; margin: 2px; }\n";
    ss << "</style>\n</head>\n<body>\n";
    ss << "<h1>" << title_ << " v" << version_ << "</h1>\n";
    ss << "<p>" << description_ << "</p>\n";
    ss << "<hr>\n";
    
    for (const auto& ep : endpoints_) {
        ss << "<div class=\"endpoint\">\n";
        std::string method_lower = ep.method;
        std::transform(method_lower.begin(), method_lower.end(), method_lower.begin(), ::tolower);
        ss << "  <span class=\"method method-" << method_lower << "\">" << ep.method << "</span>\n";
        ss << "  <span class=\"path\">" << ep.path << "</span>\n";
        ss << "  <div class=\"summary\">" << ep.summary << "</div>\n";
        
        if (!ep.tags.empty()) {
            ss << "  <div>\n";
            for (const auto& tag : ep.tags) {
                ss << "    <span class=\"tag\">" << tag << "</span>\n";
            }
            ss << "  </div>\n";
        }
        
        if (!ep.parameters.empty()) {
            ss << "  <h4>Parameters</h4>\n";
            for (const auto& param : ep.parameters) {
                ss << "  <div class=\"param\">";
                ss << "<span class=\"param-name\">" << param.name << "</span> ";
                ss << "(" << param.type << ", " << param.in << ")";
                if (param.required) ss << " <strong>*required</strong>";
                ss << " - " << param.description;
                ss << "</div>\n";
            }
        }
        
        ss << "</div>\n";
    }
    
    ss << "</body>\n</html>\n";
    return ss.str();
}

void OpenApiDoc::generateFromRoutes(HttpServer2* /*server*/) {
    // 简化版：从服务器注册的路由生成文档
    // 实际实现需要服务器提供路由列表的访问接口
    LOG_INFO("OpenAPI doc generated from registered routes");
}

void registerApiDocs(HttpServer2* server, std::shared_ptr<OpenApiDoc> doc, 
                     const std::string& base_path) {
    // 注册 JSON 文档端点
    server->get(base_path + "/openapi.json", [doc](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.status_code = "200";
        resp.headers["Content-Type"] = "application/json";
        resp.body = doc->toJson();
        return resp;
    });
    
    // 注册 HTML 文档端点
    server->get(base_path, [doc](const HttpRequest&) -> HttpResponse {
        HttpResponse resp;
        resp.status_code = "200";
        resp.headers["Content-Type"] = "text/html";
        resp.body = doc->toHtml();
        return resp;
    });
    
    LOG_INFO("API docs registered at: " + base_path + " and " + base_path + "/openapi.json");
}

}} // namespace http2::docs