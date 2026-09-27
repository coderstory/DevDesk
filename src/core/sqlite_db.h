// src/core/sqlite_db.h
// SQLite 封装：单连接 + recursive_mutex（详见 spec §6.3）
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include <filesystem>

#include "result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace core::data {

// SQLite 支持的标量类型（替代 std::any，类型安全）
using SqlValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// 一行结果：按列名索引到值
struct SqlRow {
    std::vector<std::string> columnNames;
    std::vector<SqlValue> values;

    bool isNull(const std::string& col) const;
    Result<int64_t> getInt64(const std::string& col) const;
    Result<double> getDouble(const std::string& col) const;
    Result<std::string> getString(const std::string& col) const;
};

// 单条迁移
struct Migration {
    int version;
    std::string description;
    std::string sql;
};

// Statement 句柄
class Statement {
public:
    Statement(sqlite3* db, sqlite3_stmt* stmt, std::recursive_mutex& mutex);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    Result<void> bind(int index, int64_t value);
    Result<void> bind(int index, double value);
    Result<void> bind(int index, const std::string& value);
    Result<void> bind(int index, std::nullptr_t);
    Result<void> bind(int index, const std::vector<uint8_t>& blob);

    Result<bool> step();
    Result<SqlRow> currentRow() const;
    Result<std::vector<SqlRow>> fetchAll();

    int columnCount() const;

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_;
    std::recursive_mutex& mutex_;
};

class SqliteDb {
public:
    // 审计 CP-07：默认构造私有化——「未打开的空连接」不再是可表达状态，
    // open() 是唯一入口（返回 Result，失败即失败，不留半成品对象）。
    // 确实需要可空/延迟打开语义的调用方用指针或 std::optional 表达。
    static Result<SqliteDb> open(const std::filesystem::path& file);
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;

    Result<void> exec(const std::string& sql);
    Result<Statement> prepare(const std::string& sql);

    Result<void> transaction(std::function<Result<void>()> fn);
    Result<void> runMigrations(const std::vector<Migration>& migrations);

    int schemaVersion() const;

    std::recursive_mutex& mutex() { return mutex_; }
    sqlite3* handle() { return handle_; }

private:
    SqliteDb() = default;   // 仅 open()（本类静态成员）可用，见上方说明

    sqlite3* handle_ = nullptr;
    std::recursive_mutex mutex_;
};

} // namespace core::data