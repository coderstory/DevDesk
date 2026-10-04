// 通用「键 → JSON 值」配置存储：单张 app_config 表（多表 = 多个真相源）。core 层只做 TEXT 键值
// 存取，不知配置结构、不碰 yyjson（编码由各配置模块经 platform::json 做）。并发：不直接调
// sqlite3_*，一律经 SqliteDb prepare/Statement 与 transaction；setMany 整批一个事务
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "result.h"
#include "sqlite_db.h"

namespace core::appconfig {

// app_config 表一行（key TEXT PK / value TEXT NOT NULL / updated_at INTEGER）；value 恒为 TEXT，
// 中文/空串/`{}`/换行/超长串都按原样进出
struct Entry {
    std::string key;
    std::string value;
    int64_t updatedAt = 0;
};

// 单键写（UPSERT，立即提交）。表不存在/SQL 失败 → fail（不静默成功）
Result<void> set(core::data::SqliteDb& db, const std::string& key,
                 const std::string& value);

// 多键写：逐行 UPSERT，整批一个事务，任一行失败整批回滚（一次 save = 一次提交）
Result<void> setMany(core::data::SqliteDb& db,
                     const std::vector<Entry>& entries);

// 单键读。键不存在 → fail(NotFound)，与「值是空串」严格区分（空串是合法配置值）
Result<std::string> get(core::data::SqliteDb& db, const std::string& key);

// 键是否存在（不取值）。
Result<bool> has(core::data::SqliteDb& db, const std::string& key);

// 删键。键不存在视为成功（幂等）。
Result<void> erase(core::data::SqliteDb& db, const std::string& key);

// 按前缀列出全部 (key, value)，按 key 升序（配置键一律带命名空间前缀）
Result<std::vector<Entry>> listWithPrefix(core::data::SqliteDb& db,
                                          const std::string& prefix);

// 删掉 prefix 之下、不在 keep 集合里的全部键：整份覆盖语义要求不再写出的键显式清掉，
// 否则废弃键永久留在库里。keep 内的键原样保留
Result<void> eraseWithPrefixExcept(core::data::SqliteDb& db,
                                   const std::string& prefix,
                                   const std::vector<std::string>& keep);

} // namespace core::appconfig
