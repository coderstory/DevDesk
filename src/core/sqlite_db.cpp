#include "sqlite_db.h"
#include "logging.h"

#include <cstring>
#include <sqlite3.h>

namespace core::data {

namespace {
    // 把 sqlite3 返回码转成 ErrorCode
    ErrorCode mapErrCode(int rc) {
        switch (rc) {
            case SQLITE_OK: return ErrorCode::Ok;
            case SQLITE_READONLY: case SQLITE_PERM: return ErrorCode::FilePermissionDenied;
            case SQLITE_FULL: return ErrorCode::FileWriteFailed;
            case SQLITE_CORRUPT: return ErrorCode::DatabaseCorrupted;
            case SQLITE_BUSY: case SQLITE_LOCKED: return ErrorCode::DatabaseOpenFailed;
            default: return ErrorCode::Internal;
        }
    }

    Result<void> checkRc(sqlite3* db, int rc, const std::string& op) {
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db);
            LOG_ERROR("SQLite " + op + " failed: " + err);
            return Result<void>::fail(mapErrCode(rc), op + ": " + err);
        }
        return Result<void>::ok();
    }

    std::string columnName(sqlite3_stmt* stmt, int idx) {
        const char* name = sqlite3_column_name(stmt, idx);
        return name ? name : "";
    }
}

bool SqlRow::isNull(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            return std::holds_alternative<std::monostate>(values[i]);
        }
    }
    return true;
}

Result<int64_t> SqlRow::getInt64(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            if (auto* v = std::get_if<int64_t>(&values[i])) return Result<int64_t>::ok(*v);
            return Result<int64_t>::fail(ErrorCode::ValidationError, "col not int64: " + col);
        }
    }
    return Result<int64_t>::fail(ErrorCode::NotFound, "col not found: " + col);
}

Result<double> SqlRow::getDouble(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            if (auto* v = std::get_if<double>(&values[i])) return Result<double>::ok(*v);
            return Result<double>::fail(ErrorCode::ValidationError, "col not double: " + col);
        }
    }
    return Result<double>::fail(ErrorCode::NotFound, "col not found: " + col);
}

Result<std::string> SqlRow::getString(const std::string& col) const {
    for (size_t i = 0; i < columnNames.size(); ++i) {
        if (columnNames[i] == col) {
            if (auto* v = std::get_if<std::string>(&values[i])) return Result<std::string>::ok(*v);
            return Result<std::string>::fail(ErrorCode::ValidationError, "col not string: " + col);
        }
    }
    return Result<std::string>::fail(ErrorCode::NotFound, "col not found: " + col);
}

// ============ Statement ============

Statement::Statement(sqlite3* db, sqlite3_stmt* stmt, std::recursive_mutex& mutex)
    : db_(db), stmt_(stmt), mutex_(mutex) {}

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), stmt_(other.stmt_), mutex_(other.mutex_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        db_ = other.db_;
        stmt_ = other.stmt_;
        // mutex_ 是引用，不可赋值
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

// 审计 L5：bind 原不持锁（step/currentRow 持），跨线程共用同一语句时
// bind 与 step 可以交错——全部统一到同一把 recursive_mutex
Result<void> Statement::bind(int index, int64_t value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return checkRc(db_, sqlite3_bind_int64(stmt_, index, value), "bind_int64");
}

Result<void> Statement::bind(int index, double value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return checkRc(db_, sqlite3_bind_double(stmt_, index, value), "bind_double");
}

Result<void> Statement::bind(int index, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return checkRc(db_, sqlite3_bind_text(stmt_, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT), "bind_text");
}

Result<void> Statement::bind(int index, std::nullptr_t) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return checkRc(db_, sqlite3_bind_null(stmt_, index), "bind_null");
}

Result<void> Statement::bind(int index, const std::vector<uint8_t>& blob) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return checkRc(db_, sqlite3_bind_blob(stmt_, index, blob.data(),
        static_cast<int>(blob.size()), SQLITE_TRANSIENT), "bind_blob");
}

Result<bool> Statement::step() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return Result<bool>::ok(true);
    if (rc == SQLITE_DONE) return Result<bool>::ok(false);
    return Result<bool>::fail(mapErrCode(rc),
        std::string("step: ") + sqlite3_errmsg(db_));
}

Result<SqlRow> Statement::currentRow() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    SqlRow row;
    int n = sqlite3_column_count(stmt_);
    row.columnNames.reserve(n);
    row.values.reserve(n);
    for (int i = 0; i < n; ++i) {
        row.columnNames.push_back(columnName(stmt_, i));
        int type = sqlite3_column_type(stmt_, i);
        switch (type) {
            case SQLITE_INTEGER:
                row.values.emplace_back(static_cast<int64_t>(sqlite3_column_int64(stmt_, i)));
                break;
            case SQLITE_FLOAT:
                row.values.emplace_back(sqlite3_column_double(stmt_, i));
                break;
            case SQLITE_TEXT: {
                const unsigned char* t = sqlite3_column_text(stmt_, i);
                int len = sqlite3_column_bytes(stmt_, i);
                row.values.emplace_back(std::string(reinterpret_cast<const char*>(t), len));
                break;
            }
            case SQLITE_BLOB: {
                const void* b = sqlite3_column_blob(stmt_, i);
                int len = sqlite3_column_bytes(stmt_, i);
                std::vector<uint8_t> v(reinterpret_cast<const uint8_t*>(b),
                    reinterpret_cast<const uint8_t*>(b) + len);
                row.values.emplace_back(std::move(v));
                break;
            }
            default:
                row.values.emplace_back(std::monostate{});
                break;
        }
    }
    return Result<SqlRow>::ok(std::move(row));
}

Result<std::vector<SqlRow>> Statement::fetchAll() {
    std::vector<SqlRow> rows;
    while (true) {
        auto stepResult = step();
        if (stepResult.isFail()) return Result<std::vector<SqlRow>>::fail(
            stepResult.errorCode(), stepResult.errorMessage());
        if (!stepResult.value()) break;
        auto rowResult = currentRow();
        if (rowResult.isFail()) return Result<std::vector<SqlRow>>::fail(
            rowResult.errorCode(), rowResult.errorMessage());
        rows.push_back(std::move(rowResult).value());
    }
    return Result<std::vector<SqlRow>>::ok(std::move(rows));
}

int Statement::columnCount() const {
    return sqlite3_column_count(stmt_);
}

// ============ SqliteDb ============

Result<SqliteDb> SqliteDb::open(const std::filesystem::path& file) {
    sqlite3* handle = nullptr;
    int rc = sqlite3_open(file.string().c_str(), &handle);
    if (rc != SQLITE_OK) {
        std::string err = handle ? sqlite3_errmsg(handle) : "unknown";
        if (handle) sqlite3_close(handle);
        return Result<SqliteDb>::fail(ErrorCode::DatabaseOpenFailed,
            "open: " + err);
    }
    // 启用外键 + WAL 模式
    sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(handle, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);

    SqliteDb db;
    db.handle_ = handle;
    return Result<SqliteDb>::ok(std::move(db));
}

SqliteDb::~SqliteDb() {
    if (handle_) {
        // 审计 L5：原用 sqlite3_close，未 finalize 的语句会让它返回 SQLITE_BUSY
        // 且被忽略——句柄静默不关（Windows 上文件句柄泄漏）。close_v2 是延迟
        // 关闭语义：连接变 zombie，最后一条语句 finalize 时真正回收。
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept
    : handle_(other.handle_), mutex_() {
    other.handle_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept {
    if (this != &other) {
        if (handle_) sqlite3_close(handle_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Result<void> SqliteDb::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        return Result<void>::fail(mapErrCode(rc), "exec: " + err);
    }
    return Result<void>::ok();
}

Result<Statement> SqliteDb::prepare(const std::string& sql) {
    // 审计 L5：prepare 与 exec/step/currentRow 共用同一把锁
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<Statement>::fail(mapErrCode(rc),
            std::string("prepare: ") + sqlite3_errmsg(handle_));
    }
    return Result<Statement>::ok(Statement(handle_, stmt, mutex_));
}

Result<void> SqliteDb::transaction(std::function<Result<void>()> fn) {
    // 全程持锁（审计 2026-09-23 [8]）：BEGIN + fn() + COMMIT/ROLLBACK 必须
    // 在同一把 recursive_mutex 内完成。旧实现锁只盖住 BEGIN 和 COMMIT，
    // fn() 期间是放开的——recursive_mutex 只对同线程重入有效，跨线程没有
    // 任何串行：代理 worker 线程的单条 INSERT（logCall）能穿插进 UI 线程
    // 的开放事务（被 ROLLBACK 连带吞掉），两个线程的 BEGIN 也会互相撞
    // "cannot start a transaction within a transaction"，多语句原子性形同
    // 虚设。现在并发方在 BEGIN 前就阻塞在这把锁上，等事务结束才能落语句。
    // fn() 内部的 exec/prepare/step 取的是同一把锁（同线程重入，不死锁）；
    // 前提约束：fn() 内只做 DB 操作（现有全部调用方满足），不得做长阻塞
    // 的 IO/网络，否则会拖住其他线程的语句。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    char* err = nullptr;
    if (sqlite3_exec(handle_, "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "?";
        sqlite3_free(err);
        return Result<void>::fail(ErrorCode::DatabaseCorrupted, "BEGIN: " + e);
    }
    auto result = fn();
    if (result.isOk()) {
        err = nullptr;
        if (sqlite3_exec(handle_, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "?";
            sqlite3_free(err);
            return Result<void>::fail(ErrorCode::DatabaseCorrupted, "COMMIT: " + e);
        }
    } else {
        err = nullptr;
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    }
    return result;
}

Result<void> SqliteDb::runMigrations(const std::vector<Migration>& migrations) {
    // 创建 schema_version 表
    auto initResult = exec(R"(
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER PRIMARY KEY,
            description TEXT NOT NULL,
            applied_at INTEGER NOT NULL
        )
    )");
    if (initResult.isFail()) return initResult;

    int current = schemaVersion();
    // 「数据库比二进制新」守卫（审计 2026-09-23 [37]）：降级运行旧 exe 时
    // schema_version 高于本二进制已知的最大迁移版本，下方循环会静默跳过
    // 全部迁移，后续 SQL 得到莫名的列缺失错误。这里显式失败并给明确文案。
    int knownMax = 0;
    for (const auto& m : migrations) {
        knownMax = (m.version > knownMax) ? m.version : knownMax;
    }
    if (current > knownMax) {
        const std::string msg =
            "数据库 schema 版本 v" + std::to_string(current) +
            " 高于本版本 DevDesk 支持的最大迁移版本 v" + std::to_string(knownMax) +
            "（数据库被更新版本的 DevDesk 打开过，请使用新版 DevDesk）";
        LOG_FATAL(msg);
        return Result<void>::fail(ErrorCode::DatabaseMigrationFailed, msg);
    }
    for (const auto& m : migrations) {
        if (m.version <= current) continue;
        auto txResult = transaction([&]() -> core::Result<void> {
            auto execResult = exec(m.sql);
            if (execResult.isFail()) return execResult;
            auto prep = prepare(
                "INSERT INTO schema_version (version, description, applied_at) VALUES (?, ?, ?)");
            if (prep.isFail()) {
                return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
            }
            auto& stmt = prep.value();
            if (auto b = stmt.bind(1, static_cast<int64_t>(m.version)); b.isFail()) {
                return core::Result<void>::fail(b.errorCode(), b.errorMessage());
            }
            if (auto b = stmt.bind(2, m.description); b.isFail()) {
                return core::Result<void>::fail(b.errorCode(), b.errorMessage());
            }
            if (auto b = stmt.bind(3, static_cast<int64_t>(std::time(nullptr))); b.isFail()) {
                return core::Result<void>::fail(b.errorCode(), b.errorMessage());
            }
            auto stepResult = stmt.step();
            if (stepResult.isFail()) {
                return core::Result<void>::fail(stepResult.errorCode(), stepResult.errorMessage());
            }
            return core::Result<void>::ok();
        });
        if (txResult.isFail()) {
            return Result<void>::fail(ErrorCode::DatabaseMigrationFailed,
                "迁移 v" + std::to_string(m.version) + " 失败: " + txResult.errorMessage());
        }
        LOG_INFO("已应用迁移 v" + std::to_string(m.version) + ": " + m.description);
    }
    return Result<void>::ok();
}

int SqliteDb::schemaVersion() const {
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(mutex_));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle_, "SELECT MAX(version) FROM schema_version", -1, &stmt, nullptr) != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return 0;
    }
    int v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        v = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return v;
}

} // namespace core::data