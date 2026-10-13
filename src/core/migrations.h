// 数据库版本升级机制：一个版本一组升级 SQL。
// applySchema 把新建库直接建成当前形态；applyPending 读完 PRAGMA user_version 后逐版本执行，
// 每版本包在一个事务里，成功后 user_version 推进；ALTER TABLE ADD COLUMN 全经 PRAGMA table_info
// 预检列存在性再执行（SQLite 无 ADD COLUMN IF NOT EXISTS），让各组对任意旧基线幂等。
// 版本号仅在 PRAGMA user_version 存储；schemas_version 表（历史遗留）不读不写。
#pragma once

#include "sqlite_db.h"

namespace core::migrations {

struct Migration {
    int version;              // PRAGMA user_version 目标值
    const char* description;  // 面向日志的变动说明
    const char* const* sql;   // 版本组内 SQL（逐条执行，ALTER 经预检跳过）
    int sqlCount;
    const char* requireColumn; // "表.列" 格式，非 NULL 时整组仅在列存在时执行；
                               // 新库无此列 → 跳过 + 版本照常推进（视为"已满足"）。
                               // 面向旧形态遗留数据清理——新库天然没有遗留列，免修。
};

// 执行未应用的升级组（version > current），全部成功则 user_version = max(version)。
// 全新库（applySchema 刚建）= user_version 0 → 各版本 ALTER 预检全跳过 → user_version 直落最新。
Result<void> applyPending(data::SqliteDb& db);

} // namespace core::migrations