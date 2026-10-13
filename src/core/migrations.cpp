#include "migrations.h"
#include "logging.h"

#include <sqlite3.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace core::migrations {

namespace {

// 用 PRAGMA table_info 检查列是否存在：预检用于 ALTER TABLE ADD COLUMN 的幂等跳过（SQLite
// 无 IF NOT EXISTS 语法，且重复 ALTER 会报错中断整个迁移事务）
bool columnExists(data::SqliteDb& db, const std::string& table, const std::string& col) {
    auto prep = db.prepare("PRAGMA table_info(" + table + ")");
    if (prep.isFail()) {
        LOG_WARN("migrations: PRAGMA table_info(" + table + ") 失败: " +
                 prep.errorMessage());
        return false;  // 读不到 = 假设不存在 → 执行 ALTER（重复即错，好过静默跳）
    }

    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return false;

    for (const auto& row : rows.value()) {
        auto name = row.getString("name");
        if (name.isOk() && name.value() == col) return true;
    }
    return false;
}

// 从 ALTER TABLE <table> ADD COLUMN <column> ... 提取表名与列名。
// 返回 {table, column}；解析失败返回空串（调用方执行整句，重复 ALTER 自行报错）。
struct AlterTarget { std::string table; std::string column; };

AlterTarget parseAlter(const char* sql) {
    const char* p = sql;
    // 跳过 "ALTER TABLE "
    const char* alterPrefix = "ALTER TABLE ";
    if (std::strncmp(p, alterPrefix, 12) != 0) return {};
    p += 12;
    // 跳前导空格（格式规范下不应有）
    while (*p == ' ') ++p;
    // 取表名：到 " ADD COLUMN " 或 " ADD " 截止
    const char* tableStart = p;
    while (*p && *p != ' ') ++p;
    std::string table(tableStart, static_cast<size_t>(p - tableStart));
    // 跳过空格到 "ADD"
    while (*p == ' ') ++p;
    if (std::strncmp(p, "ADD", 3) != 0) return {};
    p += 3;
    while (*p == ' ') ++p;
    // 跳过可选 "COLUMN "
    if (std::strncmp(p, "COLUMN ", 7) == 0) p += 7;
    while (*p == ' ') ++p;
    // 取列名：到空格或行尾
    const char* colStart = p;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r') ++p;
    std::string column(colStart, static_cast<size_t>(p - colStart));
    if (table.empty() || column.empty()) return {};
    return {table, column};
}

// 判断语句是否是 ALTER TABLE ... ADD COLUMN（只有这种需要幂等预检；CREATE IF NOT EXISTS
// 已内置幂等；其他 DML 由迁移作者保证幂等）
bool isAlterAddColumn(const char* sql) {
    // 跳过前导空白（格式规范下无）
    while (*sql == ' ' || *sql == '\t') ++sql;
    if (std::strncmp(sql, "ALTER TABLE ", 12) != 0) return false;
    // 确认含 ADD：找 "ADD" 子串（大小写一致），避免把 ALTER TABLE ... RENAME
    // 也判进来——本仓库迁移只写 ADD COLUMN，不设更复杂的语法分析
    const char* add = std::strstr(sql + 12, "ADD");
    return add != nullptr;
}

// 读当前 user_version
int readUserVersion(data::SqliteDb& db) {
    auto prep = db.prepare("PRAGMA user_version");
    if (prep.isFail()) {
        LOG_ERROR("migrations: 读 user_version 失败: " + prep.errorMessage());
        return -1;
    }
    auto& s = prep.value();
    auto step = s.step();
    if (step.isFail() || !step.value()) {
        LOG_ERROR("migrations: PRAGMA user_version 返回空");
        return -1;
    }
    auto row = s.currentRow();
    if (row.isFail()) {
        LOG_ERROR("migrations: 读 user_version 行失败");
        return -1;
    }
    auto v = row.value().getInt64("user_version");
    return v.isOk() ? static_cast<int>(v.value()) : -1;
}

// 写 user_version = val
Result<void> writeUserVersion(data::SqliteDb& db, int version) {
    auto prep = db.prepare("PRAGMA user_version = " + std::to_string(version));
    if (prep.isFail()) return Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto step = prep.value().step();
    if (step.isFail()) return Result<void>::fail(step.errorCode(), step.errorMessage());
    return Result<void>::ok();
}

// 执行单条 SQL（裸 sqlite3_exec，内部持锁）
Result<void> execOne(data::SqliteDb& db, const char* sql) {
    std::lock_guard<std::recursive_mutex> lock(db.mutex());
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db.handle(), sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        return Result<void>::fail(ErrorCode::DatabaseMigrationFailed,
                                  "SQL 执行失败: " + err + " [ " + std::string(sql) + " ]");
    }
    return Result<void>::ok();
}

// --- v1 升级组 ---
// 补齐从旧形态（9 月各周建的库，列集不一）到当前 schema.h 全部列：ALTER TABLE ADD COLUMN
// 每条走预检、存在即跳过；幂等保证对任意基线安全。带 DEFAULT 值与 schema.h 当前定义一致。
const char* kMigrationV1Sqls[] = {
    // claude_profiles 补列
    "ALTER TABLE claude_profiles ADD COLUMN api_key_encrypted TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE claude_profiles ADD COLUMN extra_settings_json TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE claude_profiles ADD COLUMN models_config TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE claude_profiles ADD COLUMN extra_env TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE claude_profiles ADD COLUMN shared_config_id INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE claude_profiles ADD COLUMN auth_field TEXT NOT NULL DEFAULT 'both'",
    "ALTER TABLE claude_profiles ADD COLUMN dialect_override TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE claude_profiles ADD COLUMN fallback_profile_id INTEGER NOT NULL DEFAULT 0",
    // claude_shared_configs 补列
    "ALTER TABLE claude_shared_configs ADD COLUMN extra_settings_json TEXT NOT NULL DEFAULT ''",
    // claude_call_logs 补五列 usage（全部可空，NULL = 未采集）
    "ALTER TABLE claude_call_logs ADD COLUMN input_tokens INTEGER",
    "ALTER TABLE claude_call_logs ADD COLUMN output_tokens INTEGER",
    "ALTER TABLE claude_call_logs ADD COLUMN cache_read_tokens INTEGER",
    "ALTER TABLE claude_call_logs ADD COLUMN cache_write_tokens INTEGER",
    "ALTER TABLE claude_call_logs ADD COLUMN first_event_ms INTEGER",
};

constexpr int kMigrationV1SqlCount =
    sizeof(kMigrationV1Sqls) / sizeof(kMigrationV1Sqls[0]);

// --- v3 升级组 ---
	// 旧库遗留列 keychain_account 带 NOT NULL 无默认值，新代码 createProfile 的 INSERT
	// 不写它 → 约束拦截。本组把表重建为可空遗留列（保留数据，消灭约束），在新表末尾。
	// 仅对含 keychain_account 列的旧库执行：新库（schema.h 建）无此列，requireColumn 守卫跳过。
	// 外键处理：SqliteDb::open 开启了 PRAGMA foreign_keys=ON，且 claude_call_logs 有
	// FOREIGN KEY (profile_id) REFERENCES claude_profiles(id) → DROP TABLE 会被外键拦截；
	// PRAGMA foreign_keys 在事务内是 no-op，故由 applyPending 在事务外关/开（见该函数）。
	const char* kMigrationV3Sqls[] = {
	    // 清理上一轮部分提交的中间表（迁移写 user_version 失败且事务已提交的极罕见路径）
	    "DROP TABLE IF EXISTS claude_profiles_v3",
	    // 重建表：17 列全量 + keychain_account 可空遗留列在末尾
	    "CREATE TABLE claude_profiles_v3 ("
	    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
	    "name TEXT UNIQUE NOT NULL,"
	    "base_url TEXT NOT NULL,"
	    "api_key_hint TEXT NOT NULL,"
	    "api_key_check TEXT NOT NULL,"
	    "api_key_encrypted TEXT NOT NULL DEFAULT '',"
	    "model TEXT NOT NULL,"
	    "is_active INTEGER NOT NULL DEFAULT 0,"
	    "created_at INTEGER NOT NULL,"
	    "updated_at INTEGER NOT NULL,"
	    "models_config TEXT NOT NULL DEFAULT '',"
	    "extra_env TEXT NOT NULL DEFAULT '',"
	    "shared_config_id INTEGER NOT NULL DEFAULT 0,"
	    "auth_field TEXT NOT NULL DEFAULT 'both',"
	    "dialect_override TEXT NOT NULL DEFAULT '',"
	    "fallback_profile_id INTEGER NOT NULL DEFAULT 0,"
	    "extra_settings_json TEXT NOT NULL DEFAULT '',"
	    "keychain_account TEXT"  // 可空、无默认——新库无此行，本组被 requireColumn 跳过
	    ")",
	    "INSERT INTO claude_profiles_v3 ("
	    "id, name, base_url, api_key_hint, api_key_check, api_key_encrypted, model,"
	    "is_active, created_at, updated_at, models_config, extra_env, shared_config_id,"
	    "auth_field, dialect_override, fallback_profile_id, extra_settings_json,"
	    "keychain_account"
	    ") SELECT "
	    "id, name, base_url, api_key_hint, api_key_check, api_key_encrypted, model,"
	    "is_active, created_at, updated_at, models_config, extra_env, shared_config_id,"
	    "auth_field, dialect_override, fallback_profile_id, extra_settings_json,"
	    "keychain_account "
	    "FROM claude_profiles",
	    "DROP TABLE claude_profiles",
	    "ALTER TABLE claude_profiles_v3 RENAME TO claude_profiles",
	    // DROP TABLE 连走了旧索引，按 schema.h 当前定义重建
	    "CREATE UNIQUE INDEX IF NOT EXISTS idx_claude_active "
	    "ON claude_profiles(is_active) WHERE is_active = 1",
	};

	constexpr int kMigrationV3SqlCount =
	    sizeof(kMigrationV3Sqls) / sizeof(kMigrationV3Sqls[0]);
// todo_tasks 加 archived_at（INTEGER，NULL = 未归档）；存量回填已入库的归档列任务。
// 迁移进入/离开归档列的时间戳由 TodoService::moveTask 运行时计算，不靠本组。
const char* kMigrationV2Sqls[] = {
    "ALTER TABLE todo_tasks ADD COLUMN archived_at INTEGER",
    // 回填已在归档列（kind='archive'）但 archived_at 为 NULL 的行：用 updated_at 作近似归档时刻，
    // 保证强制排序对老数据也有确定键（updated_at 是本行知道的唯一可靠时间）。WHERE 幂等，重复执行无副作用。
    "UPDATE todo_tasks SET archived_at = updated_at "
    "WHERE column_id IN (SELECT id FROM todo_columns WHERE kind = 'archive') "
    "AND archived_at IS NULL",
};

constexpr int kMigrationV2SqlCount =
    sizeof(kMigrationV2Sqls) / sizeof(kMigrationV2Sqls[0]);

constexpr Migration kMigrations[] = {
    {1, "补齐 claude_profiles / claude_shared_configs / claude_call_logs 历史缺列",
     kMigrationV1Sqls, kMigrationV1SqlCount, nullptr},
    {2, "todo_tasks 加 archived_at + 存量归档列回填",
     kMigrationV2Sqls, kMigrationV2SqlCount, nullptr},
    {3, "claude_profiles 表重建：keychain_account 遗留列除 NOT NULL 约束",
     kMigrationV3Sqls, kMigrationV3SqlCount,
     "claude_profiles.keychain_account"},
};

constexpr int kMigrationCount = sizeof(kMigrations) / sizeof(kMigrations[0]);

// 当前最新版本（= 最大 migration.version）；无迁移组时为 0
constexpr int kLatestVersion = []() {
    int v = 0;
    for (int i = 0; i < kMigrationCount; ++i) {
        if (kMigrations[i].version > v) v = kMigrations[i].version;
    }
    return v;
}();

} // namespace

Result<void> applyPending(data::SqliteDb& db) {
    int currentVersion = readUserVersion(db);
    if (currentVersion < 0) {
        return Result<void>::fail(ErrorCode::DatabaseMigrationFailed,
                                  "无法读取当前数据库版本（PRAGMA user_version）");
    }

    if (currentVersion >= kLatestVersion) {
        LOG_INFO("数据库版本 v" + std::to_string(currentVersion) +
                 "，无需升级（最新 v" + std::to_string(kLatestVersion) + "）");
        return Result<void>::ok();
    }

    LOG_INFO("数据库版本升级：v" + std::to_string(currentVersion) + " → v" +
             std::to_string(kLatestVersion) + "（共 " +
             std::to_string(kMigrationCount) + " 组）");

    for (int i = 0; i < kMigrationCount; ++i) {
        const Migration& m = kMigrations[i];
        if (m.version <= currentVersion) continue;  // 已应用，跳过

        // requireColumn 守卫：组有 requireColumn 且该列不存在 → 整组跳过 + 版本照常推进。
        // 用途：新库没有遗留列，该组对这类库本就该视为已满足。
        bool skipGroup = false;
        if (m.requireColumn != nullptr) {
            auto dot = std::strchr(m.requireColumn, '.');
            if (dot) {
                std::string table(m.requireColumn, static_cast<size_t>(dot - m.requireColumn));
                std::string col(dot + 1);
                if (!table.empty() && !col.empty() && !columnExists(db, table, col)) {
                    LOG_INFO("跳过 v" + std::to_string(m.version) +
                             "（非目标库形态：缺 " + std::string(m.requireColumn) + "）");
                    auto wv = writeUserVersion(db, m.version);
                    if (wv.isFail()) {
                        return Result<void>::fail(wv.errorCode(),
                            "数据库升级 v" + std::to_string(m.version) +
                            "：requireColumn 跳过但 user_version 写入失败: " +
                            wv.errorMessage());
                    }
                    skipGroup = true;
                }
            }
        }
        if (skipGroup) continue;

        LOG_INFO("  应用 v" + std::to_string(m.version) + ": " + m.description +
                 "（" + std::to_string(m.sqlCount) + " 条 SQL）");

        // requireColumn 组的 SQL 含表重建（DROP + CREATE），claude_call_logs 有 FK 引用
        // claude_profiles(id)。PRAGMA foreign_keys 在事务内是 no-op（SQLite 文档），
        // 故在事务外先关再开。迁移在 Database::open 路径内单线程执行，锁间隙无并发访问。
        const bool needFkOff = (m.requireColumn != nullptr);
        if (needFkOff) {
            auto fkResult = execOne(db, "PRAGMA foreign_keys = OFF");
            if (fkResult.isFail()) {
                return Result<void>::fail(fkResult.errorCode(),
                    "数据库升级 v" + std::to_string(m.version) +
                    "：无法关闭外键约束: " + fkResult.errorMessage());
            }
        }

        // 每组一个事务：中途失败则整组回滚，user_version 不推进
        auto txResult = db.transaction([&]() -> Result<void> {
            for (int j = 0; j < m.sqlCount; ++j) {
                const char* sql = m.sql[j];
                if (isAlterAddColumn(sql)) {
                    auto target = parseAlter(sql);
                    if (!target.table.empty() && !target.column.empty() &&
                        columnExists(db, target.table, target.column)) {
                        // 幂等跳过：旧库部分列已存在（各周建的库列集不一）
                        LOG_INFO("    跳过（列已存在）: " + target.table + "." +
                                 target.column);
                        continue;
                    }
                }
                auto r = execOne(db, sql);
                if (r.isFail()) return r;
            }
            return Result<void>::ok();
        });

        // 恢复外键：无论事务成败都执行。PRAGMA 失败不阻升级——FK 不生效比
        // user_version 停滞的代价小，且 SqliteDb::open 已设过 ON，本处是还原。
        if (needFkOff) {
            auto fkOn = execOne(db, "PRAGMA foreign_keys = ON");
            if (fkOn.isFail()) {
                LOG_WARN("v" + std::to_string(m.version) +
                         "：迁移后恢复 PRAGMA foreign_keys=ON 失败: " +
                         fkOn.errorMessage());
            }
        }

        if (txResult.isFail()) {
            return Result<void>::fail(txResult.errorCode(),
                "数据库升级 v" + std::to_string(m.version) + " 失败: " +
                txResult.errorMessage());
        }

        // 事务成功后推进 user_version
        auto wv = writeUserVersion(db, m.version);
        if (wv.isFail()) {
            // user_version 写失败是严重不一致状态——SQL 已执行但版本未记录
            return Result<void>::fail(wv.errorCode(),
                "数据库升级 v" + std::to_string(m.version) +
                "：SQL 已执行但 user_version 写入失败: " + wv.errorMessage());
        }

        LOG_INFO("  数据库已升级到 v" + std::to_string(m.version));
    }

    return Result<void>::ok();
}

} // namespace core::migrations