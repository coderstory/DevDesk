// src/core/sql_dump.h
// SQLite 数据库导出/导入为 SQL 文本文件（供 GitHub Sync 备份用）。
// 纯函数，core 层，不依赖 platform。
// 导出只导出用户数据表（排除调用日志/运行设置等非用户数据）。
#pragma once

#include <string>

#include "result.h"
#include "sqlite_db.h"

namespace core::sql_dump {

// 导出整个数据库为 SQL 文本（CREATE TABLE + INSERT 语句，包在一个事务里）。
// skipTables: 跳过的表名集合（如 {"claude_call_logs", "claude_run_settings"}）。
// 空 skipTables = 导出全部表。
core::Result<std::string> exportToString(
    core::data::SqliteDb& db,
    const std::vector<std::string>& skipTables = {});

// 导出并写入文件（atomicWriteFile，先写临时文件再覆盖）。
core::Result<void> exportToFile(
    core::data::SqliteDb& db,
    const std::filesystem::path& file,
    const std::vector<std::string>& skipTables = {});

// 从 SQL 文本导入数据库（exec 逐条执行，一个事务）。
// 先清空目标表再导入（DROP TABLE IF EXISTS + 重建 + INSERT）。
core::Result<void> importFromString(
    core::data::SqliteDb& db,
    const std::string& sql);

// 从文件导入。
core::Result<void> importFromFile(
    core::data::SqliteDb& db,
    const std::filesystem::path& file);

} // namespace core::sql_dump