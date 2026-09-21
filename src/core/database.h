// src/core/database.h
// DevDesk 数据库门面（封装 SqliteDb + migrations）
//
// 用法（main.cpp）：
//   auto r = core::Database::open(core::paths::sqliteDbFile());
//   if (r.isFail()) { LOG_FATAL(...); }
//   if (auto m = r.value().migrate(); m.isFail()) { LOG_FATAL(...); }
//   auto& db = r.value().db();
//
#pragma once

#include <filesystem>
#include <vector>

#include "result.h"
#include "sqlite_db.h"

namespace core {

// 应用级数据库：单连接 + 迁移管理
//
// 职责：
//   - 打开底层的 SqliteDb
//   - 应用所有 registered migrations（顺序按 version 升序）
//   - 提供 db() 访问器给 module_* 层的 service 用
class Database {
public:
    Database() = default;
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // 在 path 处打开 SQLite 文件，应用 migrations（已应用过的会被跳过）
    static core::Result<Database> open(const std::filesystem::path& path);

    // 应用所有未跑的 migrations
    core::Result<void> migrate();

    // 访问底层 SqliteDb（给 module_* 层用）
    core::data::SqliteDb& db() { return db_; }
    const core::data::SqliteDb& db() const { return db_; }

private:
    Database(core::data::SqliteDb db, std::vector<core::data::Migration> migrations);

    core::data::SqliteDb db_;
    std::vector<core::data::Migration> migrations_;
};

// ============ 进程级数据库共享访问器 ============
//
// main.cpp 在启动时（跑完 migrations 之后）调用 registerAppDatabase()，
// module_* 层在 onStart() 里用 appDatabase() 拿到同一份连接，
// 保持 spec §6.3 的"单连接 + recursive_mutex"设计。
//
// 返回 nullptr 表示尚未注册（模块应跳过数据加载，只画空态）。
void registerAppDatabase(core::data::SqliteDb* db);
core::data::SqliteDb* appDatabase();

} // namespace core
