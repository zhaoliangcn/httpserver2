#ifndef HTTPSERVER2_DATABASE_H
#define HTTPSERVER2_DATABASE_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace http2 { namespace db {

// 数据库类型
enum class DBType {
    SQLITE,
    POSTGRES
};

// 数据库连接配置
struct DBConfig {
    DBType type = DBType::SQLITE;
    std::string dsn = "data.db";  // sqlite: /path/file.db, pg: postgresql://user:pass@host/db
    int max_connections = 10;
    int timeout_seconds = 5;
};

// 查询结果
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    int64_t affected_rows = 0;
    int64_t last_insert_id = 0;
    std::string error;
    
    bool success() const { return error.empty(); }
    size_t rowCount() const { return rows.size(); }
    size_t colCount() const { return columns.size(); }
    
    // 获取单行
    std::vector<std::string> row(size_t index) const {
        return index < rows.size() ? rows[index] : std::vector<std::string>();
    }
    
    // 获取单值（第一行第一列）
    std::string value() const {
        return (!rows.empty() && !rows[0].empty()) ? rows[0][0] : "";
    }
};

// 数据库抽象接口
class Database {
public:
    static std::shared_ptr<Database> create(const DBConfig& config);
    
    virtual ~Database() = default;
    
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() = 0;
    
    // 执行 SQL（无参数）
    virtual QueryResult execute(const std::string& sql) = 0;
    
    // 执行 SQL（带参数，? 占位符）
    virtual QueryResult execute(const std::string& sql, 
                                 const std::vector<std::string>& params) = 0;
    
    // 事务
    virtual bool beginTransaction() = 0;
    virtual bool commit() = 0;
    virtual bool rollback() = 0;
    
    // 便捷方法
    bool executeTransaction(const std::function<bool(std::shared_ptr<Database>)>& callback);
};

// SQLite 实现
class SQLiteDatabase : public Database {
private:
    void* db_ = nullptr;  // sqlite3*
    DBConfig config_;
    
public:
    SQLiteDatabase(const DBConfig& config) : config_(config) {}
    ~SQLiteDatabase() override { disconnect(); }
    
    bool connect() override;
    void disconnect() override;
    bool isConnected() override;
    
    QueryResult execute(const std::string& sql) override;
    QueryResult execute(const std::string& sql, 
                         const std::vector<std::string>& params) override;
    
    bool beginTransaction() override;
    bool commit() override;
    bool rollback() override;
};

// TinyDB 实现
class TinyDBDatabase : public Database {
private:
    void* client_ = nullptr;  // TinyDBClient* (opaque pointer)
    DBConfig config_;
    std::string host_ = "127.0.0.1";
    int port_ = 5432;
    
public:
    TinyDBDatabase(const DBConfig& config);
    ~TinyDBDatabase() override;
    
    bool connect() override;
    void disconnect() override;
    bool isConnected() override;
    
    QueryResult execute(const std::string& sql) override;
    QueryResult execute(const std::string& sql, 
                         const std::vector<std::string>& params) override;
    
    bool beginTransaction() override;
    bool commit() override;
    bool rollback() override;

    // --- 元数据查询（TinyDB 专有） ---
    //
    // 为什么单独开方法而不是走 execute()：TinyDB 嵌入式路径的 SQL 解析器
    // 不支持 pg_catalog / information_schema，SHOW DATABASES 也没有接上
    // 执行分支，所以表/索引清单只能由引擎的 catalog 直接导出。
    // 这些方法不参与 Database 抽象接口，仅 TinyDBDatabase 提供。
    //
    // 成功返回 true，json 为引擎给出的 JSON 文本；
    // 失败返回 false，error 为原因。
    bool listTables(std::string& json, std::string& error);
    bool describeTable(const std::string& table_name, std::string& json, std::string& error);
    bool listIndexes(const std::string& table_name, std::string& json, std::string& error);
};

}} // namespace http2::db

#endif // HTTPSERVER2_DATABASE_H