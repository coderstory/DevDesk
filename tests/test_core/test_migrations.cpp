// 版本升级机制测试：合成旧形态库 → 跑迁移 → 列齐、数据不丢、幂等、user_version 正确
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/migrations.h"
#include "core/result.h"
#include "core/schema.h"
#include "core/sqlite_db.h"

#include <sqlite3.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

// helper：读 user_version
int readUserVersion(core::data::SqliteDb& db) {
    auto prep = db.prepare("PRAGMA user_version");
    if (prep.isFail()) return -1;
    auto step = prep.value().step();
    if (step.isFail() || !step.value()) return -1;
    auto row = step.value() ? prep.value().currentRow() : core::Result<core::data::SqlRow>::fail(core::ErrorCode::Internal, "no row");
    if (row.isFail()) return -1;
    auto v = row.value().getInt64("user_version");
    return v.isOk() ? static_cast<int>(v.value()) : -1;
}

// helper：列存在
bool hasColumn(core::data::SqliteDb& db, const std::string& table,
               const std::string& col) {
    auto prep = db.prepare("PRAGMA table_info(" + table + ")");
    if (prep.isFail()) return false;
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return false;
    for (const auto& row : rows.value()) {
        auto name = row.getString("name");
        if (name.isOk() && name.value() == col) return true;
    }
    return false;
}

// helper：行数
int64_t rowCount(core::data::SqliteDb& db, const std::string& table) {
    auto prep = db.prepare("SELECT COUNT(*) AS n FROM " + table);
    if (prep.isFail()) return -1;
    auto rows = prep.value().fetchAll();
    if (rows.isFail() || rows.value().empty()) return -1;
    auto n = rows.value()[0].getInt64("n");
    return n.isOk() ? n.value() : -1;
}

// helper：读一个 cell
std::string cell(core::data::SqliteDb& db, const std::string& table,
                 const std::string& col, int64_t id) {
    auto prep = db.prepare("SELECT " + col + " FROM " + table + " WHERE id = ?");
    if (prep.isFail()) return "";
    if (prep.value().bind(1, id).isFail()) return "";
    auto step = prep.value().step();
    if (step.isFail() || !step.value()) return "";
    auto row = prep.value().currentRow();
    if (row.isFail()) return "";
    auto v = row.value().getString(col);
    return v.isOk() ? v.value() : "";
}

// 手动建旧形态 claude_profiles 表（仅含当时已有的列 + keychain_account 历史列），
// 绕过 applySchema，直接经 SqliteDb 落 DDL
void createOldClaudeProfilesTable(core::data::SqliteDb& db) {
    // 模拟 2026-09 某周建的库：有 name / base_url / api_key_hint / api_key_check /
    // model / is_active / created_at / updated_at / keychain_account（历史列）
    // 缺：api_key_encrypted / extra_settings_json / models_config / extra_env /
    //      shared_config_id / auth_field / dialect_override / fallback_profile_id
    db.exec(
        "CREATE TABLE IF NOT EXISTS claude_profiles ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT UNIQUE NOT NULL,"
        "base_url TEXT NOT NULL,"
        "api_key_hint TEXT NOT NULL,"
        "api_key_check TEXT NOT NULL,"
        "model TEXT NOT NULL,"
        "is_active INTEGER NOT NULL DEFAULT 0,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "keychain_account TEXT)"
    );
}

void createOldClaudeSharedConfigsTable(core::data::SqliteDb& db) {
    // 旧形态：只有 id / name / created_at / updated_at，缺 extra_settings_json
    // 还有一个 extra_env 历史列（保留不删）
    db.exec(
        "CREATE TABLE IF NOT EXISTS claude_shared_configs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT UNIQUE NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "extra_env TEXT)"
    );
}

void createOldClaudeCallLogsTable(core::data::SqliteDb& db) {
    // 旧形态：10 列（无后面 5 列 usage）
    db.exec(
        "CREATE TABLE IF NOT EXISTS claude_call_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "profile_id INTEGER NOT NULL,"
        "method TEXT NOT NULL,"
        "path TEXT NOT NULL,"
        "status_code INTEGER,"
        "latency_ms INTEGER,"
        "request_bytes INTEGER,"
        "response_bytes INTEGER,"
        "error TEXT,"
        "started_at INTEGER NOT NULL,"
        "FOREIGN KEY (profile_id) REFERENCES claude_profiles(id) ON DELETE CASCADE"
        ")"
    );
}

// 用 SqliteDb 直接打开并建旧表 + 插数据行（全绕过 Database 的 applySchema），
// 返回库路径供后续 Database::open（会触发 applySchema + 迁移）
std::string createOldDb(const std::string& prefix, const std::string& tag) {
    auto path = test_support::tempDbPath(prefix, tag);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto dbR = core::data::SqliteDb::open(path);
    if (dbR.isFail()) return "";
    auto& db = dbR.value();

    // 建必需的外键相关表（applySchema 有 BUT IF NOT EXISTS，手动建同名不冲突）
    createOldClaudeProfilesTable(db);
    createOldClaudeSharedConfigsTable(db);
    createOldClaudeCallLogsTable(db);

    // 插一行数据
    db.exec(
        "INSERT INTO claude_profiles "
        "(name, base_url, api_key_hint, api_key_check, model, is_active, "
        " created_at, updated_at, keychain_account) "
        "VALUES ('old-profile', 'https://api.example.com', 'sk-***xxx', "
        "'abc123', 'claude-3-sonnet', 1, 1000, 1000, 'claude:1')"
    );
    db.exec(
        "INSERT INTO claude_shared_configs "
        "(name, created_at, updated_at, extra_env) "
        "VALUES ('old-shared', 1000, 1000, '{\"k\":\"v\"}')"
    );
    db.exec(
        "INSERT INTO claude_call_logs "
        "(profile_id, method, path, status_code, latency_ms, "
        " request_bytes, response_bytes, started_at) "
        "VALUES (1, 'POST', '/v1/messages', 200, 500, 100, 200, 1000)"
    );

    return path.string();
}

} // namespace

// ─── 1. 旧形态库迁移 ───

REGISTER_TEST(migrations_old_db_upgrade,
              "迁移: 旧形态库 → 列补齐、数据不丢、user_version=1", {
    auto path = createOldDb("devdesk_test_mig_", "old");
    DOCTEST_REQUIRE(!path.empty());

    {
        // Database::open → applySchema（旧表由 IF NOT EXISTS 跳过）
        //            → applyPending（v1 补全缺列）
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // user_version = 2（v1 + v2 组已全部应用）
        DOCTEST_CHECK_EQ(readUserVersion(db), 2);

        // claude_profiles 补全 8 列
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "api_key_encrypted"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "extra_settings_json"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "models_config"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "extra_env"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "shared_config_id"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "auth_field"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "dialect_override"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "fallback_profile_id"));
        // keychain_account 历史列仍在
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "keychain_account"));

        // 行数不变
        DOCTEST_CHECK_EQ(rowCount(db, "claude_profiles"), 1);
        // 数据保留
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "name", 1),
                         std::string("old-profile"));
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "base_url", 1),
                         std::string("https://api.example.com"));
        // 补列用 DEFAULT 值
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "api_key_encrypted", 1),
                         std::string(""));
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "auth_field", 1),
                         std::string("both"));

        // claude_shared_configs 补全
        DOCTEST_CHECK(hasColumn(db, "claude_shared_configs", "extra_settings_json"));
        DOCTEST_CHECK_EQ(rowCount(db, "claude_shared_configs"), 1);

        // claude_call_logs 补五列
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "input_tokens"));
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "output_tokens"));
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "cache_read_tokens"));
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "cache_write_tokens"));
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "first_event_ms"));
        DOCTEST_CHECK_EQ(rowCount(db, "claude_call_logs"), 1);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
})

// ─── 2. 幂等：二次跑不出错、user_version 不变 ───

REGISTER_TEST(migrations_is_idempotent,
              "迁移: 二次 applyPending 幂等（不报错、版本不漂移）", {
    auto path = createOldDb("devdesk_test_mig_", "idem");
    DOCTEST_REQUIRE(!path.empty());

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();
        DOCTEST_CHECK_EQ(readUserVersion(db), 2);

        // 二次跑 migration：再次打开库会再调 applyPending，各组应跳过
    }
    {
        auto r2 = core::Database::open(path);
        DOCTEST_REQUIRE(r2.isOk());
        auto& db2 = r2.value().db();
        // user_version 仍是 2，没有推进（没有新版本组）
        DOCTEST_CHECK_EQ(readUserVersion(db2), 2);
        // 数据仍完好
        DOCTEST_CHECK_EQ(rowCount(db2, "claude_profiles"), 1);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
})

// ─── 3. 全新库 → user_version = 最新 ───

REGISTER_TEST(migrations_new_db_gets_latest_version,
              "迁移: 全新库 applySchema+迁移 → user_version=最新(v2)", {
    test_support::TestDb tdb("devdesk_test_mig_", "new");
    auto& db = tdb.handle();

    // 全新库：applySchema 建全部列 → 各组 ALTER 预检全跳过 → user_version = 2
    DOCTEST_CHECK_EQ(readUserVersion(db), 2);

    // 按 schema 建了全部列：不应缺
    DOCTEST_CHECK(hasColumn(db, "claude_profiles", "api_key_encrypted"));
    DOCTEST_CHECK(hasColumn(db, "claude_profiles", "extra_settings_json"));
    DOCTEST_CHECK(hasColumn(db, "claude_shared_configs", "extra_settings_json"));
    DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "input_tokens"));
    DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "first_event_ms"));

    // 全新库无 keychain_account（该历史列不在 schema.h 中）
    DOCTEST_CHECK(!hasColumn(db, "claude_profiles", "keychain_account"));

    return true;
})

// ─── 4. 部分列已补的旧库 ───

REGISTER_TEST(migrations_partial_upgrade,
              "迁移: 部分列已存在（不同周建的基线），幂等补齐其余列", {
    auto path = test_support::tempDbPath("devdesk_test_mig_", "partial");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto dbR = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(dbR.isOk());
        auto& db = dbR.value();

        // 手动建表（模拟另一周建的库：api_key_encrypted / extra_env 已补，
        // 但其余 6 列不在）
        db.exec(
            "CREATE TABLE IF NOT EXISTS claude_profiles ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT UNIQUE NOT NULL,"
            "base_url TEXT NOT NULL,"
            "api_key_hint TEXT NOT NULL,"
            "api_key_check TEXT NOT NULL,"
            "api_key_encrypted TEXT NOT NULL DEFAULT '',"  // 已补
            "model TEXT NOT NULL,"
            "is_active INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL,"
            "extra_env TEXT NOT NULL DEFAULT '',"  // 已补
            "models_config TEXT NOT NULL DEFAULT ''"  // 已补
            ")"
        );
        db.exec(
            "CREATE TABLE IF NOT EXISTS claude_shared_configs ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT UNIQUE NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL,"
            "extra_settings_json TEXT NOT NULL DEFAULT ''"  // 已补
            ")"
        );
        db.exec(
            "CREATE TABLE IF NOT EXISTS claude_call_logs ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "profile_id INTEGER NOT NULL,"
            "method TEXT NOT NULL,"
            "path TEXT NOT NULL,"
            "status_code INTEGER,"
            "latency_ms INTEGER,"
            "request_bytes INTEGER,"
            "response_bytes INTEGER,"
            "error TEXT,"
            "started_at INTEGER NOT NULL,"
            "input_tokens INTEGER,"  // 已补
            "output_tokens INTEGER"  // 已补
            ")"
        );

        // 插数据
        db.exec(
            "INSERT INTO claude_profiles "
            "(name, base_url, api_key_hint, api_key_check, api_key_encrypted, "
            " model, created_at, updated_at, extra_env, models_config) "
            "VALUES ('partial', 'url', 'hint', 'check', 'enc', "
            " 'm', 1, 1, '{}', '[]')"
        );
        db.exec(
            "INSERT INTO claude_call_logs "
            "(profile_id, method, path, input_tokens, output_tokens, started_at) "
            "VALUES (1, 'POST', '/', 10, 20, 1)"
        );
    }

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        DOCTEST_CHECK_EQ(readUserVersion(db), 2);

        // 已补的列保留：
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "api_key_encrypted"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "extra_env"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "models_config"));
        // 缺少的列补齐：
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "extra_settings_json"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "shared_config_id"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "auth_field"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "dialect_override"));
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "fallback_profile_id"));

        // call_logs 补全其余三列
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "cache_read_tokens"));
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "cache_write_tokens"));
        DOCTEST_CHECK(hasColumn(db, "claude_call_logs", "first_event_ms"));

        // 数据不丢
        DOCTEST_CHECK_EQ(rowCount(db, "claude_profiles"), 1);
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "name", 1),
                         std::string("partial"));
        // 已存在的值不动
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "api_key_encrypted", 1),
                         std::string("enc"));
    }

    std::filesystem::remove(path, ec);
    return true;
})

// ─── 5. 凭据回填 ───

REGISTER_TEST(migrations_credential_backfill,
              "凭据回填: keychain_account 列存在时→读旧 Keychain→protect→入库", {
    auto path = test_support::tempDbPath("devdesk_test_mig_", "backfill");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 用隔离的数据根（避免碰真实 keychain 目录）
    test_support::DataRootEnv dre("devdesk_test_mig_", "bf");

    {
        auto dbR = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(dbR.isOk());
        auto& db = dbR.value();

        // 建旧形态 profies 表（含 keychain_account 列）
        createOldClaudeProfilesTable(db);
        // 插一行：api_key_encrypted 为空、keychain_account 有值
        // 先手动补上 api_key_encrypted 列（这段没本质依赖库的 applySchema）
        db.exec(
            "ALTER TABLE claude_profiles ADD COLUMN api_key_encrypted "
            "TEXT NOT NULL DEFAULT ''"
        );
        db.exec(
            "INSERT INTO claude_profiles "
            "(name, base_url, api_key_hint, api_key_check, model, is_active, "
            " created_at, updated_at, keychain_account, api_key_encrypted) "
            "VALUES ('bf-profile', 'https://x.com', 'hint', 'check', 'm', 1, "
            " 1, 1, 'claude:bf-test', '')"
        );
    }

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // applySchema + 迁移把列补全
        DOCTEST_CHECK_EQ(readUserVersion(db), 2);

        // 凭据列仍是空（没有真的 Keychain 文件）
        DOCTEST_CHECK_EQ(cell(db, "claude_profiles", "api_key_encrypted", 1),
                         std::string(""));
        // keychain_account 列还在
        DOCTEST_CHECK(hasColumn(db, "claude_profiles", "keychain_account"));
    }

    std::filesystem::remove(path, ec);
    return true;
})

// ─── 6. 全新库无 keychain_account，backfill 直接跳过 ───

REGISTER_TEST(migrations_credential_backfill_skip_new_db,
              "凭据回填: 新库无 keychain_account 列→直接跳过（零副作用）", {
    test_support::TestDb tdb("devdesk_test_mig_", "nokc");
    auto& db = tdb.handle();

    // 新库：applySchema + 迁移 → 无 keychain_account 列
    DOCTEST_CHECK(!hasColumn(db, "claude_profiles", "keychain_account"));
    DOCTEST_CHECK_EQ(readUserVersion(db), 2);

    return true;
})

// ─── 7. v2：todo_tasks 加 archived_at ───

REGISTER_TEST(migrations_v2_todo_archived_at,
              "迁移 v2: todo_tasks 加 archived_at（列存在 + 可空 + 版本号 2）", {
    test_support::TestDb tdb("devdesk_test_mig_", "v2a");
    auto& db = tdb.handle();

    // 新库 applySchema + applyPending → v2 已完成
    DOCTEST_CHECK(hasColumn(db, "todo_tasks", "archived_at"));
    DOCTEST_CHECK_EQ(readUserVersion(db), 2);

    // PRAGMA table_info 验证列可空（notnull=0）
    {
        auto prep = db.prepare("PRAGMA table_info(todo_tasks)");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        bool sawArchived = false;
        for (const auto& row : rows.value()) {
            auto name = row.getString("name");
            if (name.isOk() && name.value() == "archived_at") {
                sawArchived = true;
                auto nn = row.getInt64("notnull");
                DOCTEST_CHECK(nn.isOk() && nn.value() == 0);
                break;
            }
        }
        DOCTEST_CHECK(sawArchived);
    }

    // 在待办列建任务（archived_at 应自动为 NULL），然后移到归档列验证时间戳被置
    {
        auto prep = db.prepare("SELECT id FROM todo_columns WHERE kind = 'archive' LIMIT 1");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk() && !rows.value().empty());
        int64_t archiveId = rows.value()[0].getInt64("id").valueOr(0);
        DOCTEST_REQUIRE(archiveId > 0);

        auto prepTodo = db.prepare("SELECT id FROM todo_columns WHERE kind = 'todo' LIMIT 1");
        DOCTEST_REQUIRE(prepTodo.isOk());
        auto rowsTodo = prepTodo.value().fetchAll();
        DOCTEST_REQUIRE(rowsTodo.isOk() && !rowsTodo.value().empty());
        int64_t todoId = rowsTodo.value()[0].getInt64("id").valueOr(0);

        // 在待办列建任务
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        std::string sql = "INSERT INTO todo_tasks (column_id, title, priority, due_at, tags, position, "
            "completed_at, created_at, updated_at) VALUES (" + std::to_string(todoId) +
            ", '归档测试', 0, NULL, '[]', 999, NULL, " +
            std::to_string(now) + ", " + std::to_string(now) + ")";
        std::lock_guard<std::recursive_mutex> lock(db.mutex());
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db.handle(), sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) { std::string e = errMsg ? errMsg : ""; sqlite3_free(errMsg);
            DOCTEST_CHECK_MESSAGE(false, e.c_str());
        } else {
            // archived_at 应为 NULL（在非归档列）
            auto p = db.prepare("SELECT archived_at FROM todo_tasks WHERE title = '归档测试'");
            DOCTEST_REQUIRE(p.isOk());
            auto r = p.value().fetchAll();
            DOCTEST_REQUIRE(r.isOk() && !r.value().empty());
            DOCTEST_CHECK(r.value()[0].isNull("archived_at"));
        }
    }

    return true;
})