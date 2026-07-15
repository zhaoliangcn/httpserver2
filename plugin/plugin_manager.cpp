#include "plugin.h"
#include <dlfcn.h>
#include <filesystem>
#include "../single_include/logger.h"

#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_FATAL

#define LOG_DEBUG(msg)    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())
#define LOG_INFO(msg)     log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())
#define LOG_WARNING(msg)  log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())
#define LOG_ERROR(msg)    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, "%s", std::string(msg).c_str())

namespace http2 { namespace plugin {

// ============================================================
// PluginManager 实现
// ============================================================
PluginManager::~PluginManager() {
    shutdownAll();
}

bool PluginManager::load(const std::string& so_path) {
    if (!std::filesystem::exists(so_path)) {
        LOG_ERROR("Plugin file not found: " + so_path);
        return false;
    }
    
    // 动态加载 .so
    void* handle = dlopen(so_path.c_str(), RTLD_NOW);
    if (!handle) {
        LOG_ERROR("Failed to load plugin: " + so_path + " - " + dlerror());
        return false;
    }
    
    // 获取插件描述符
    auto get_desc = reinterpret_cast<GetPluginDescriptorFunc>(
        dlsym(handle, "getPluginDescriptor"));
    if (!get_desc) {
        LOG_ERROR("Invalid plugin (no getPluginDescriptor): " + so_path);
        dlclose(handle);
        return false;
    }
    
    PluginDescriptor* desc = get_desc();
    if (!desc) {
        LOG_ERROR("Plugin descriptor is null: " + so_path);
        dlclose(handle);
        return false;
    }
    
    // 创建插件实例
    auto plugin = desc->create();
    if (!plugin) {
        LOG_ERROR("Failed to create plugin instance: " + so_path);
        dlclose(handle);
        return false;
    }
    
    LOG_INFO("Plugin loaded: " + desc->name + " v" + desc->version + " - " + desc->description);
    
    handles_.push_back(handle);
    plugins_.push_back(std::move(plugin));
    return true;
}

size_t PluginManager::loadDir(const std::string& dir) {
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        LOG_WARNING("Plugin directory not found: " + dir);
        return 0;
    }
    
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".so") {
            if (load(entry.path().string())) {
                count++;
            }
        }
    }
    
    LOG_INFO("Loaded " + std::to_string(count) + " plugins from " + dir);
    return count;
}

void PluginManager::registerPlugin(std::unique_ptr<Plugin> plugin) {
    if (plugin) {
        LOG_INFO("Built-in plugin registered: " + plugin->name());
        plugins_.push_back(std::move(plugin));
    }
}

bool PluginManager::initializeAll(HttpServer2* server) {
    if (initialized_) {
        LOG_WARNING("Plugins already initialized");
        return true;
    }
    
    bool all_success = true;
    for (auto& plugin : plugins_) {
        if (!plugin->initialize(server)) {
            LOG_ERROR("Failed to initialize plugin: " + plugin->name());
            all_success = false;
        } else {
            LOG_INFO("Plugin initialized: " + plugin->name());
        }
    }
    
    initialized_ = true;
    return all_success;
}

void PluginManager::shutdownAll() {
    // 反向关闭（后加载的先关闭）
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        try {
            (*it)->shutdown();
            LOG_INFO("Plugin shutdown: " + (*it)->name());
        } catch (const std::exception& e) {
            LOG_ERROR("Plugin shutdown error: " + (*it)->name() + " - " + e.what());
        }
    }
    plugins_.clear();
    
    // 卸载 .so
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
        dlclose(*it);
    }
    handles_.clear();
    
    initialized_ = false;
}

std::vector<std::string> PluginManager::listPlugins() const {
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& plugin : plugins_) {
        names.push_back(plugin->name() + " v" + plugin->version());
    }
    return names;
}

}} // namespace http2::plugin