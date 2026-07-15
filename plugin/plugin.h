#ifndef HTTPSERVER2_PLUGIN_H
#define HTTPSERVER2_PLUGIN_H

#include <string>
#include <memory>
#include <vector>
#include <functional>

// 前向声明
class HttpServer2;

namespace http2 { namespace plugin {

// 插件接口
class Plugin {
public:
    virtual ~Plugin() = default;
    
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    virtual std::string description() const = 0;
    
    // 初始化（传入服务器实例，可注册路由/中间件）
    virtual bool initialize(HttpServer2* server) = 0;
    
    // 关闭/清理
    virtual void shutdown() = 0;
};

// 插件描述符（从 .so 导出）
struct PluginDescriptor {
    std::string name;
    std::string version;
    std::string description;
    std::function<std::unique_ptr<Plugin>()> create;
};

// 插件创建函数类型（从 .so 导出）
typedef PluginDescriptor* (*GetPluginDescriptorFunc)();

// 插件管理器
class PluginManager {
private:
    std::vector<void*> handles_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    bool initialized_ = false;

public:
    PluginManager() = default;
    ~PluginManager();
    
    // 加载单个 .so 插件
    bool load(const std::string& so_path);
    
    // 加载目录下所有 .so 插件
    size_t loadDir(const std::string& dir);
    
    // 注册内置插件（无需 .so）
    void registerPlugin(std::unique_ptr<Plugin> plugin);
    
    // 初始化所有已加载的插件
    bool initializeAll(HttpServer2* server);
    
    // 关闭所有插件
    void shutdownAll();
    
    // 获取插件列表
    std::vector<std::string> listPlugins() const;
    
    // 获取插件数量
    size_t count() const { return plugins_.size(); }
};

}} // namespace http2::plugin

#endif // HTTPSERVER2_PLUGIN_H