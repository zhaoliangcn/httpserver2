#include "database.h"
#include <sqlite3.h>
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

namespace http2 { namespace db {

// ============================================================
// Database 工厂方法
// ============================================================
std::shared_ptr<Database> Database::create(const DBConfig& config) {
    switch (config.type) {
        case DBType::SQLITE:
            return std::make_shared<SQLiteDatabase>(config);
        case DBType::POSTGRES:
            // PostgreSQL 暂未实现，使用 TinyDB 替代
#ifdef HS2_WITH_TINYDB
            LOG_INFO("PostgreSQL not available, using TinyDB adapter");
            return std::make_shared<TinyDBDatabase>(config);
#else
            LOG_INFO("PostgreSQL not available, and TinyDB adapter is disabled "
                     "(build with -DHS2_WITH_TINYDB=ON to enable it)");
            return nullptr;
#endif
        default:
            return nullptr;
    }
}

// ============================================================
// 事务便捷方法
// ============================================================
bool Database::executeTransaction(const std::function<bool(std::shared_ptr<Database>)>& callback) {
    // 注意：此方法需要 Database 继承 enable_shared_from_this
    // 简化实现：直接调用 beginTransaction/commit/rollback
    // 子类应重写此方法
    if (!beginTransaction()) return false;
    // 无法直接获取 shared_from_this，这里简化处理
    // 实际使用中请直接调用 beginTransaction/commit/rollback
    return false;
}

// ============================================================
// SQLite 实现
// ============================================================
bool SQLiteDatabase::connect() {
    if (db_) return true;
    
    int rc = sqlite3_open(config_.dsn.c_str(), reinterpret_cast<sqlite3**>(&db_));
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQLite open failed: " + std::string(sqlite3_errmsg(reinterpret_cast<sqlite3*>(db_))));
        db_ = nullptr;
        return false;
    }
    
    // 设置超时
    sqlite3_busy_timeout(reinterpret_cast<sqlite3*>(db_), config_.timeout_seconds * 1000);
    
    // 启用 WAL 模式提升并发性能
    execute("PRAGMA journal_mode=WAL");
    execute("PRAGMA foreign_keys=ON");
    
    LOG_INFO("SQLite database opened: " + config_.dsn);
    return true;
}

void SQLiteDatabase::disconnect() {
    if (db_) {
        sqlite3_close(reinterpret_cast<sqlite3*>(db_));
        db_ = nullptr;
    }
}

bool SQLiteDatabase::isConnected() {
    return db_ != nullptr;
}

// ============================================================
// 执行 SQL（无参数）
// ============================================================
QueryResult SQLiteDatabase::execute(const std::string& sql) {
    return execute(sql, {});
}

// ============================================================
// 执行 SQL（带参数）
// ============================================================
QueryResult SQLiteDatabase::execute(const std::string& sql, 
                                     const std::vector<std::string>& params) {
    QueryResult result;
    sqlite3* db = reinterpret_cast<sqlite3*>(db_);
    
    if (!db) {
        result.error = "Database not connected";
        return result;
    }
    
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    
    int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt, &tail);
    if (rc != SQLITE_OK) {
        result.error = sqlite3_errmsg(db);
        LOG_ERROR("SQLite prepare error: " + result.error + " (SQL: " + sql + ")");
        return result;
    }
    
    // 绑定参数
    for (size_t i = 0; i < params.size(); ++i) {
        rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), 
                               params[i].c_str(), static_cast<int>(params[i].size()),
                               SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            result.error = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return result;
        }
    }
    
    // 执行
    bool is_query = false;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!is_query) {
            is_query = true;
            // 获取列名
            int col_count = sqlite3_column_count(stmt);
            for (int i = 0; i < col_count; ++i) {
                result.columns.push_back(sqlite3_column_name(stmt, i));
            }
        }
        
        // 获取行数据
        std::vector<std::string> row;
        int col_count = sqlite3_column_count(stmt);
        for (int i = 0; i < col_count; ++i) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.push_back(text ? text : "");
        }
        result.rows.push_back(std::move(row));
    }
    
    if (rc != SQLITE_DONE) {
        result.error = sqlite3_errmsg(db);
        LOG_ERROR("SQLite step error: " + result.error);
    }
    
    result.affected_rows = sqlite3_changes(db);
    result.last_insert_id = sqlite3_last_insert_rowid(db);
    
    sqlite3_finalize(stmt);
    return result;
}

// ============================================================
// 事务
// ============================================================
bool SQLiteDatabase::beginTransaction() {
    auto r = execute("BEGIN TRANSACTION");
    return r.success();
}

bool SQLiteDatabase::commit() {
    auto r = execute("COMMIT");
    return r.success();
}

bool SQLiteDatabase::rollback() {
    auto r = execute("ROLLBACK");
    return r.success();
}

}} // namespace http2::db