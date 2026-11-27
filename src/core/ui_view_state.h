// src/core/ui_view_state.h
// 本机视图状态（滚动位置 / 选中项 / 上次打开路径）的库内落点。
//
// 为什么独立成表（P2-16 / #386 交集）：视图状态原先存在 app_config，而 app_config
// 是整库 dump 的一部分，GitHub Sync 跨机导入会把另一台机器的滚动/选中带过来，
// 两台互相覆盖。sql_dump::defaultSkipTables() 整表剔除本表 → 视图状态只留本机。
//   app_config 无法只剔前缀（补列/迁移共用一张表），故必须单独建表；与
//   github_sync_state 同款理由（见 schema.h 该表注释）。
//
// 一次性迁移：getOrMigrate() 在新表缺行时读旧 app_config 键，落到新表后删旧行，
// 既保住老用户当前状态，又停止它继续随整库同步外泄。
#pragma once

#include <string>

#include "result.h"
#include "sqlite_db.h"

namespace core::ui_view_state {

// 单模块整份状态读。行不存在 → fail(NotFound)（与「值是空串」区分）。
Result<std::string> get(core::data::SqliteDb& db, const std::string& module);

// 行是否存在。
Result<bool> has(core::data::SqliteDb& db, const std::string& module);

// 整份 UPSERT（一次写 = 一次提交）。
Result<void> set(core::data::SqliteDb& db, const std::string& module,
                 const std::string& value);

// 删行（幂等：不存在视为成功；测试清场同一入口）。
Result<void> erase(core::data::SqliteDb& db, const std::string& module);

// 读新表；缺行且 legacyKey 非空时，从 app_config 的 legacyKey 迁移（成功后删旧行）。
Result<std::string> getOrMigrate(core::data::SqliteDb& db,
                                 const std::string& module,
                                 const std::string& legacyKey);

}  // namespace core::ui_view_state