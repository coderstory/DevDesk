// src/core/database.h
// DevDesk 数据库门面（封装 SqliteDb + 当前 schema）
//
// 用法（main.cpp）：
//   auto r = core::Database::open(core::paths::sqliteDbFile());
//   if (r.isFail()) { LOG_FATAL(...); }
//   if (auto m = r.value().migrate(); m.isFail()) { LOG_FATAL(...); }
//   auto& db = r.value().db();
//
#pragma once

#include <filesystem>

#include "result.h"
#include "sqlite_db.h"

namespace core {

// 应用级数据库：单连接 + schema 落库
//
// 职责：
//   - 打开底层的 SqliteDb
//   - 应用当前 schema（core::schema::kSql，单份幂等脚本）
//   - 提供 db() 访问器给 module_* 层的 service 用
class Database {
public:
    // 审计 CP-07：默认构造明确禁用（open() 是唯一入口，见 test_sqlite.cpp 的
    // static_assert）。此前写 `= default` 只是「碰巧」不可默认构造——SqliteDb 的
    // 默认构造是 private（sqlite_db.h），故本行被隐式删除；clang 会对隐式删除的
    // 显式 default 报 -Wdefaulted-function-deleted（GCC 不报）。改成 `= delete`
    // 明确表达同一语义，跨编译器一致。
    Database() = delete;
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // 在 path 处打开 SQLite 文件并应用当前 schema（幂等，重复调用安全）
    static core::Result<Database> open(const std::filesystem::path& path);

    // 应用当前 schema（core::schema::kSql）
    core::Result<void> migrate();

    // 滚动清理调用日志（spec §11.5 保留策略）：删除 started_at 早于
    // now - retentionDays 的 claude_call_logs 行，返回删除条数。
    // 幂等（重复执行删除 0 行）；retentionDays < 0 按 0 处理。
    core::Result<int64_t> pruneCallLogs(int retentionDays);

    // 访问底层 SqliteDb（给 module_* 层用）
    core::data::SqliteDb& db() { return db_; }
    const core::data::SqliteDb& db() const { return db_; }

private:
    explicit Database(core::data::SqliteDb db);

    core::data::SqliteDb db_;
};

// 滚动清理调用日志（自由函数版，实现见 database.cpp）：Database::pruneCallLogs
// 的同款逻辑，供 module_* 层经 core::appDatabase() 拿连接后直接调用
// （设置页切换保留档位时立即执行一次清理，不必重启）
core::Result<int64_t> pruneCallLogs(core::data::SqliteDb& db, int retentionDays);

// ============ 进程级数据库共享访问器 ============
//
// main.cpp 在启动时（落完 schema 之后）调用 registerAppDatabase()，
// module_* 层在 onStart() 里用 appDatabase() 拿到同一份连接，
// 保持 spec §6.3 的"单连接 + recursive_mutex"设计。
//
// 返回 nullptr 表示尚未注册（模块应跳过数据加载，只画空态）。
void registerAppDatabase(core::data::SqliteDb* db);
core::data::SqliteDb* appDatabase();

} // namespace core
