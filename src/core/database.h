// src/core/database.h
// DevDesk 数据库门面（封装 SqliteDb + 当前 schema）。open() 是唯一入口：open → migrate → db()，
// 失败即 Result fail（main.cpp 里 fail 走 LOG_FATAL+abort）
#pragma once

#include <filesystem>

#include "result.h"
#include "sqlite_db.h"

namespace core {

// 应用级数据库：单连接 + schema 落库。打开底层 SqliteDb、应用当前 schema（单份幂等脚本）、
// 提供 db() 给 module_* 层的 service
class Database {
public:
    // 默认构造明确禁用（open() 是唯一入口）。SqliteDb 默认构造是 private 故 `= default` 被隐式删除，
    // clang 会对隐式删除的显式 default 报 -Wdefaulted-function-deleted（GCC 不报），`= delete` 跨编译器一致
    Database() = delete;
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // 在 path 处打开并应用当前 schema（幂等）
    static core::Result<Database> open(const std::filesystem::path& path);

    // 应用当前 schema（core::schema::kSql）
    core::Result<void> migrate();

    // 滚动清理调用日志：删 started_at 早于 now - retentionDays 的行，返回删除条数；幂等，retentionDays<0 按 0
    core::Result<int64_t> pruneCallLogs(int retentionDays);

    // 访问底层 SqliteDb（给 module_* 层用）
    core::data::SqliteDb& db() { return db_; }
    const core::data::SqliteDb& db() const { return db_; }

private:
    explicit Database(core::data::SqliteDb db);

    core::data::SqliteDb db_;
};

// 滚动清理调用日志（自由函数版）：供 module_* 层经 appDatabase() 拿连接后直接调用（切档位时立即清理）
core::Result<int64_t> pruneCallLogs(core::data::SqliteDb& db, int retentionDays);

// 进程级数据库共享访问器：main.cpp 启动时（落完 schema 后）registerAppDatabase()，
// module_* 在 onStart 用 appDatabase() 拿同一份连接。nullptr = 尚未注册（模块画空态）
void registerAppDatabase(core::data::SqliteDb* db);
core::data::SqliteDb* appDatabase();

} // namespace core
