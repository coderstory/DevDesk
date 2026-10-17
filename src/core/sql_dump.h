// src/core/sql_dump.h
// SQLite 数据库导出/导入为 SQL 文本文件（供 GitHub Sync 备份用）。
// 纯函数，core 层，不依赖 platform。
// 导出 = 建表 DDL → 索引 DDL → INSERT → 触发器 DDL（顺序约束见 exportToString）；
// FTS5 影子表与 sqlite_autoindex_% 不导出：前者由 VIRTUAL TABLE 的 DDL 重建，
// 后者是 UNIQUE / PRIMARY KEY 的内部自动索引，sql 列为 NULL 不可重建，重建表自动恢复。
#pragma once

#include <string>

#include "result.h"
#include "sqlite_db.h"

namespace core::sql_dump {

// 默认不进备份的表（GitHub Sync 固定用它）：调用日志量大、运行设置与同步基线
// 属机器本地状态（github_sync_state 随 dump 跨设备转移会污染新设备的比对基线，
// 见 schema.h 该表注释）。
// 消费方（github_sync_logic）必须取本函数返回值，不得另抄一份清单（两份必漂移）。
const std::vector<std::string>& defaultSkipTables();

// 导出整个数据库为 SQL 文本（PRAGMA foreign_keys 开关 + 建表 DDL + 索引 DDL +
// INSERT + 触发器 DDL，包在一个事务里，sqlite3 CLI 可直接执行）。
// skipTables: 跳过的表名集合；索引/触发器跟着所属表走（表被跳过 = 其上对象也不导）。
// 空 skipTables = 导出全部表。
core::Result<std::string> exportToString(
    core::data::SqliteDb& db,
    const std::vector<std::string>& skipTables = {});

// 导出并写入文件（atomicWriteFile，先写临时文件再覆盖）。
core::Result<void> exportToFile(
    core::data::SqliteDb& db,
    const std::filesystem::path& file,
    const std::vector<std::string>& skipTables = {});

// 从 SQL 文本导入数据库：剥掉 dump 的事务外壳后整份 exec，包在一个事务里，
// 任一句失败即整体回滚并返回 fail，不静默跳过。幂等：dump 自带
// 「DROP ... IF EXISTS + 原 DDL」，同一份 dump 可重复导入。
// 导入期间临时关 PRAGMA foreign_keys（dump 的 DDL/INSERT 按表名字典序，子表行可能
// 先于父表行落库），结束恢复原值——PRAGMA 在事务内是 no-op，故本函数必须在
// **无打开事务**的连接上调用（外层有事务时外键关不掉，导入可能因外键失败报错）。
// 被跳过的表不在 dump 里：恢复到全新库时它们不存在（编排层需先建好 schema 再导入）。
core::Result<void> importFromString(
    core::data::SqliteDb& db,
    const std::string& sql);

} // namespace core::sql_dump
