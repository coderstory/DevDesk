// src/core/app_config.h
// 通用「键 → JSON 值」配置存储（2026-10-03 用户第 17 条需求「所有 app 配置
// 全部进 SQLite，不要 JSON 文件了」，批次 E2）
//
// 落点：单张 `app_config` 表（core/schema.h 的 kSql）。**不按配置文件各建一张表**
// ——六份配置各一张表 = 六个真相源，导出/备份/审计都得逐张拼，漏一张就丢一份。
//
// 分工边界（守层级铁律）：
//   - core 这一层只做 **TEXT 键值存取**，不知道任何一份配置的结构，也不碰
//     yyjson ——core 不可依赖 platform（YYJSON 住在 platform/json.h）。
//     每份配置「怎么把自己的结构编码成一个字符串」由各自的模块用
//     platform::json 做（module_* 可依赖 core + platform）。
//   - 落盘形态：**不再有 JSON 文件**。读写都是 SQLite 行。
//
// 并发纪律（memory `sqlite-connection-concurrency-crash`，真机 SIGSEGV 前科）：
//   本文件不直接调 sqlite3_*，一律经 SqliteDb::prepare / Statement（每个 API
//   调用各自入锁，含 finalize）与 SqliteDb::transaction（BEGIN…COMMIT 整段入锁）。
//   `setMany` 的多行写包在**一个** transaction 里 —— 既保证「改 N 个键要么全成
//   要么全不成」，也让别的线程在这段期间整段阻塞（不会出现读到半套键的中间态）。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "result.h"
#include "sqlite_db.h"

namespace core::appconfig {

// app_config 表的一行（schema：key TEXT PRIMARY KEY / value TEXT NOT NULL /
// updated_at INTEGER NOT NULL）。value 恒为**存得住任意字节**的 TEXT：中文、
// 空串、`{}`/`[]`、换行、超长串都按原样进出，不做任何转义以外的加工。
struct Entry {
    std::string key;
    std::string value;
    int64_t updatedAt = 0;
};

// 单键写（UPSERT，立即提交）。等价于旧实现「改完立刻写文件」。
// 表不存在 / SQL 失败 → fail（**不静默成功**：调用方据此知道配置没落下去）。
Result<void> set(core::data::SqliteDb& db, const std::string& key,
                 const std::string& value);

// 多键写：entries 逐行 UPSERT，整批包在**一个事务**里。任一行失败整批回滚。
// 顶层键拆分的写入路径走这里（一次 save = 一次事务 = 一次提交）。
Result<void> setMany(core::data::SqliteDb& db,
                     const std::vector<Entry>& entries);

// 单键读。键不存在 → fail(ErrorCode::NotFound)——与「值是空串」严格区分
// （空串是合法的配置值，必须能存能取回）。
Result<std::string> get(core::data::SqliteDb& db, const std::string& key);

// 键是否存在（不取值）。
Result<bool> has(core::data::SqliteDb& db, const std::string& key);

// 删键。键不存在视为成功（幂等）。
Result<void> erase(core::data::SqliteDb& db, const std::string& key);

// 按前缀列出全部 (key, value)，按 key 升序。前缀匹配用 `key LIKE ? || '%'`。
// 「列出某份配置的全部顶层键」这类操作用它（配置键一律带命名空间前缀）。
Result<std::vector<Entry>> listWithPrefix(core::data::SqliteDb& db,
                                          const std::string& prefix);

// 删掉 prefix 之下、**不在** keep 集合里的全部键。
// 用途 = 把「整份覆盖」语义搬进 SQLite：旧实现重写一份 JSON 文档时，此前存在、
// 这轮不再写出的键会随之消失；按行存之后必须显式清掉，否则废弃键会永久留在
// 库里（一次 rename 就多一行垃圾，且没人会想到去查）。keep 内的键原样保留。
Result<void> eraseWithPrefixExcept(core::data::SqliteDb& db,
                                   const std::string& prefix,
                                   const std::vector<std::string>& keep);

} // namespace core::appconfig
