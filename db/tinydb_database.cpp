#include "database.h"
#include <sstream>
#include <cstring>
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

// TinyDB C Client SDK 头文件
#include <client/tinydb_client.h>

namespace http2 { namespace db {

// ============================================================
// TinyDB 适配器实现
// ============================================================
TinyDBDatabase::TinyDBDatabase(const DBConfig& config) : config_(config) {
    // 解析 DSN: "host=127.0.0.1 port=5432" 或 "127.0.0.1:5432"
    auto parse_dsn = [](const std::string& dsn, const std::string& key, 
                         const std::string& default_val) -> std::string {
        auto pos = dsn.find(key + "=");
        if (pos == std::string::npos) return default_val;
        pos += key.size() + 1;
        auto end = dsn.find_first_of(" \t", pos);
        return dsn.substr(pos, end - pos);
    };
    
    // 支持两种格式: "host=... port=..." 或 "host:port"
    auto colon = config_.dsn.find(':');
    if (colon != std::string::npos && config_.dsn.find('=') == std::string::npos) {
        host_ = config_.dsn.substr(0, colon);
        port_ = std::stoi(config_.dsn.substr(colon + 1));
    } else {
        host_ = parse_dsn(config_.dsn, "host", "127.0.0.1");
        port_ = std::stoi(parse_dsn(config_.dsn, "port", "5432"));
    }
}

TinyDBDatabase::~TinyDBDatabase() {
    disconnect();
}

bool TinyDBDatabase::connect() {
    if (client_) return true;
    
    // 不用 TINYDB_DEFAULT_OPTIONS 宏：它内部是 C++20 的指定初始化器
    // （.host = ... 形式），在 C++17 下 GCC 只是当扩展放行，MSVC 会直接拒绝
    // （error C7555）。这里逐字段赋值，语义与宏完全一致，只依赖 C++17。
    TinyDBConnectOptions options{};
    options.host = "127.0.0.1";
    options.port = 5432;
    options.user = "";
    options.password = "";
    options.database = "";
    options.timeout_ms = 30000;
    options.use_ssl = false;
    options.ssl_cert = nullptr;
    options.ssl_key = nullptr;

    options.host = host_.c_str();
    options.port = port_;
    options.timeout_ms = config_.timeout_seconds * 1000;
    
    void* handle = tinydb_connect(&options);
    if (!handle) {
        LOG_ERROR("TinyDB connect failed to " + host_ + ":" + std::to_string(port_));
        return false;
    }
    
    client_ = handle;
    LOG_INFO("TinyDB connected: " + host_ + ":" + std::to_string(port_));
    return true;
}

void TinyDBDatabase::disconnect() {
    if (client_) {
        tinydb_disconnect(client_);
        client_ = nullptr;
    }
}

bool TinyDBDatabase::isConnected() {
    return client_ ? tinydb_is_connected(client_) : false;
}

// ============================================================
// 执行 SQL（无参数）
// ============================================================
QueryResult TinyDBDatabase::execute(const std::string& sql) {
    QueryResult result;
    
    if (!client_ || !tinydb_is_connected(client_)) {
        result.error = "Not connected to TinyDB server";
        return result;
    }
    
    TinyDBResult* tres = tinydb_query(client_, sql.c_str());
    if (!tres) {
        result.error = "TinyDB query returned null";
        return result;
    }
    
    if (tres->error != TINYDB_OK) {
        result.error = tres->error_message ? tres->error_message : "Unknown error";
        LOG_ERROR("TinyDB query error: " + result.error + " (SQL: " + sql + ")");
        tinydb_free_result(tres);
        return result;
    }
    
    // 转换列名
    for (size_t i = 0; i < tres->num_columns; ++i) {
        if (tres->column_names[i]) {
            result.columns.push_back(tres->column_names[i]);
        }
    }
    
    // 转换行数据
    for (size_t r = 0; r < tres->num_rows; ++r) {
        std::vector<std::string> row;
        for (size_t c = 0; c < tres->rows[r].num_columns; ++c) {
            TinyDBValue& val = tres->rows[r].values[c];
            if (val.is_null) {
                row.push_back("");
            } else {
                switch (val.type) {
                    case TINYDB_TYPE_INTEGER:
                        row.push_back(std::to_string(val.int_val));
                        break;
                    case TINYDB_TYPE_BIGINT:
                        row.push_back(std::to_string(val.bigint_val));
                        break;
                    case TINYDB_TYPE_FLOAT:
                    case TINYDB_TYPE_DOUBLE:
                        row.push_back(std::to_string(val.double_val));
                        break;
                    case TINYDB_TYPE_BOOLEAN:
                        row.push_back(val.bool_val ? "true" : "false");
                        break;
                    case TINYDB_TYPE_TEXT:
                        row.push_back(val.text_val ? val.text_val : "");
                        break;
                    default:
                        row.push_back(val.text_val ? val.text_val : "");
                        break;
                }
            }
        }
        result.rows.push_back(std::move(row));
    }
    
    result.affected_rows = static_cast<int64_t>(tres->num_rows);
    result.last_insert_id = 0;  // TinyDB C API 不直接提供 last_insert_id
    
    tinydb_free_result(tres);
    return result;
}

// ============================================================
// 执行 SQL（带参数 — 简单替换实现）
// ============================================================
QueryResult TinyDBDatabase::execute(const std::string& sql, 
                                     const std::vector<std::string>& params) {
    // 简单参数替换：将 ? 替换为参数值
    std::string processed_sql;
    size_t param_idx = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '?' && param_idx < params.size()) {
            // 用单引号包裹字符串值
            processed_sql += "'";
            for (char c : params[param_idx]) {
                if (c == '\'') processed_sql += "''";
                else processed_sql += c;
            }
            processed_sql += "'";
            param_idx++;
        } else {
            processed_sql += sql[i];
        }
    }
    
    return execute(processed_sql);
}

// ============================================================
// 事务
// ============================================================
bool TinyDBDatabase::beginTransaction() {
    if (!client_) return false;
    return tinydb_begin(client_);
}

bool TinyDBDatabase::commit() {
    if (!client_) return false;
    return tinydb_commit(client_);
}

bool TinyDBDatabase::rollback() {
    if (!client_) return false;
    return tinydb_rollback(client_);
}

// ============================================================
// 元数据查询
//
// 引擎侧返回的是堆上的 JSON 字符串，这里接过来拷进 std::string
// 并立刻释放，避免调用方忘记 free 造成泄漏。
// ============================================================
namespace {
// 统一处理 "引擎返回 char* → 填充 json/error" 的搬运逻辑
bool TakeEngineJson(char* raw, void* client, std::string& json, std::string& error) {
    if (!raw) {
        const char* msg = client ? tinydb_error(client) : nullptr;
        error = (msg && msg[0]) ? msg : "metadata query failed";
        return false;
    }
    json.assign(raw);
    tinydb_free_string(raw);
    return true;
}
} // namespace

bool TinyDBDatabase::listTables(std::string& json, std::string& error) {
    if (!client_) { error = "not connected"; return false; }
    return TakeEngineJson(tinydb_list_tables(client_), client_, json, error);
}

bool TinyDBDatabase::describeTable(const std::string& table_name,
                                   std::string& json, std::string& error) {
    if (!client_) { error = "not connected"; return false; }
    if (table_name.empty()) { error = "table name is empty"; return false; }
    return TakeEngineJson(tinydb_describe_table(client_, table_name.c_str()),
                          client_, json, error);
}

bool TinyDBDatabase::listIndexes(const std::string& table_name,
                                 std::string& json, std::string& error) {
    if (!client_) { error = "not connected"; return false; }
    if (table_name.empty()) { error = "table name is empty"; return false; }
    return TakeEngineJson(tinydb_list_indexes(client_, table_name.c_str()),
                          client_, json, error);
}

}} // namespace http2::db