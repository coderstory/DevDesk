// tests/test_core/test_migrations.cpp
// 测试 Database + migrations
#include "doctest.h"
#include "core/database.h"
#include "core/migrations.h"
#include "core/result.h"
#include "core/sqlite_db.h"

#include <filesystem>
#include <set>
#include <string>

namespace {

// 给测试生成一个临时 db 路径，不污染应用数据库
std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_migrations_" + tag + "_" +
                std::to_string(counter) + ".db";
    return test_temp::tempPath(name);
}

// 读 sqlite_master，返回当前 db 里所有表名
std::set<std::string> listTables(core::data::SqliteDb& db) {
    auto prep = db.prepare("SELECT name FROM sqlite_master WHERE type='table'");
    if (prep.isFail()) return {};
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return {};
    std::set<std::string> tables;
    for (const auto& row : rows.value()) {
        auto name = row.getString("name");
        if (name.isOk()) tables.insert(name.value());
    }
    return tables;
}

} // namespace

REGISTER_TEST(migrate_creates_all_tables, "Database: migrate creates all 7 tables", {
    auto path = tempDbPath("create");
    // 预清理：无异常版本（Windows 上 remove 失败不应炸掉整个套件）
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto migrateResult = r.value().migrate();
        DOCTEST_REQUIRE(migrateResult.isOk());

        auto tables = listTables(r.value().db());

        // 7 个应用表（用 insert 避免在宏实参里写逗号，触发 C 预处理器 arg-split）
        std::set<std::string> expected;
        expected.insert("markdown_index");
        expected.insert("markdown_tags");
        expected.insert("markdown_note_tags");
        expected.insert("todo_columns");
        expected.insert("todo_tasks");
        expected.insert("claude_profiles");
        expected.insert("claude_call_logs");
        for (const auto& t : expected) {
            bool found = tables.count(t) > 0;
            DOCTEST_CHECK(found);
            if (!found) {
                std::cout << "  [DEBUG] missing table: " << t << std::endl;
            }
        }
    } // Database 在此析构（关闭 sqlite 句柄），Windows 不允许删除打开中的文件

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(migrate_seeds_default_todo_columns,
              "Database: migrate inserts 4 default Todo columns", {
    auto path = tempDbPath("seed");
    // 预清理：无异常版本
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());

        auto prep = r.value().db().prepare(
            "SELECT title FROM todo_columns ORDER BY position");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_CHECK_EQ(rows.value().size(), 4u);
        DOCTEST_CHECK_EQ(rows.value()[0].getString("title").value(), "待办");
        DOCTEST_CHECK_EQ(rows.value()[1].getString("title").value(), "进行中");
        DOCTEST_CHECK_EQ(rows.value()[2].getString("title").value(), "已完成");
        DOCTEST_CHECK_EQ(rows.value()[3].getString("title").value(), "已归档");
    } // Database 在此析构（关闭 sqlite 句柄）

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(migrate_is_idempotent, "Database: migrate is idempotent", {
    auto path = tempDbPath("idempotent");
    // 预清理：无异常版本
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 第一次 open + migrate（r1 用完即出作用域，Windows 上句柄不关无法删文件）
    {
        auto r1 = core::Database::open(path);
        DOCTEST_REQUIRE(r1.isOk());
        DOCTEST_REQUIRE(r1.value().migrate().isOk());
    }

    // 关掉再打开，第二次 migrate 不应报错也不应重复插入
    {
        auto r2 = core::Database::open(path);
        DOCTEST_REQUIRE(r2.isOk());
        DOCTEST_REQUIRE(r2.value().migrate().isOk());

        // 第三次 migrate 同样安全
        DOCTEST_REQUIRE(r2.value().migrate().isOk());

        // todo_columns 仍然只有 4 行（不被重复插入）
        auto prep = r2.value().db().prepare("SELECT COUNT(*) FROM todo_columns");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_CHECK_EQ(std::get<int64_t>(rows.value()[0].values[0]), 4);
    } // Database 在此析构（关闭 sqlite 句柄）

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(migrate_rejects_newer_schema,
              "Database: migrate rejects newer schema_version (downgrade guard)", {
    auto path = tempDbPath("newer");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto openR = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(openR.isOk());
        auto& db = openR.value();
        // 伪造一个来自未来版本的 schema_version 记录（模拟数据库被
        // 更新版 DevDesk 打开后回退旧 exe 运行）
        DOCTEST_REQUIRE(db.exec(
            "CREATE TABLE schema_version ("
            "version INTEGER PRIMARY KEY, description TEXT NOT NULL, "
            "applied_at INTEGER NOT NULL)").isOk());
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO schema_version (version, description, applied_at) "
            "VALUES (999, 'future', 0)").isOk());
        // runMigrations 必须显式失败（带明确文案），而不是静默跳过全部迁移
        auto r = db.runMigrations(core::migrations::all());
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK(r.errorCode() ==
                      core::ErrorCode::DatabaseMigrationFailed);
    } // SqliteDb 在此析构（关闭 sqlite 句柄）

    std::filesystem::remove(path, ec);
    return true;
})

// migration v5（claude_call_logs 用量列）：旧库升级后 5 个新列存在、存量行
// 不被破坏（新列全 NULL = 未采集，不是 0），并且新列可写可读回
REGISTER_TEST(migrate_call_logs_usage_columns,
              "migration v5: 旧库升级加调用日志用量列，存量行零迁移", {
    auto path = tempDbPath("usage_cols");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 手工构造 v4 旧库（只跑前 4 条 migration）+ 存量 profile / 调用日志行
    {
        auto openR = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(openR.isOk());
        auto& db = openR.value();
        std::vector<core::data::Migration> oldMigrations;
        oldMigrations.push_back(core::migrations::kCreateInitialTables);
        oldMigrations.push_back(core::migrations::kSeedDefaultTodoColumns);
        oldMigrations.push_back(core::migrations::kClaudeProviderConfigColumns);
        oldMigrations.push_back(core::migrations::kTodoColumnKind);
        DOCTEST_REQUIRE(db.runMigrations(oldMigrations).isOk());

        // profile_id 有外键（open 即 PRAGMA foreign_keys=ON）→ 先补 profile 行
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO claude_profiles (id, name, base_url, api_key_hint, "
            "api_key_check, keychain_account, model, is_active, created_at, "
            "updated_at) VALUES (1, '旧供应商', 'https://old.example.com', "
            "'sk-***abcd', 'deadbeef', 'claude:1', 'claude-3-5-haiku', 1, 0, 0)")
                            .isOk());
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO claude_call_logs (profile_id, method, path, status_code, "
            "latency_ms, request_bytes, response_bytes, error, started_at) VALUES "
            "(1, 'POST', '/v1/messages', 200, 42, 100, 200, NULL, 1700000000)")
                            .isOk());
    } // 句柄关闭（Windows 文件锁）

    // 重新打开 + 全量 migrate → v5 补 5 列
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();
        DOCTEST_CHECK_EQ(db.schemaVersion(), 5);

        auto prep = db.prepare(
            "SELECT status_code, latency_ms, request_bytes, response_bytes, "
            "input_tokens, output_tokens, cache_read_tokens, cache_write_tokens, "
            "first_event_ms FROM claude_call_logs");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        const auto& row = rows.value()[0];

        // 存量列原样保留
        DOCTEST_CHECK_EQ(row.getInt64("status_code").value(), 200);
        DOCTEST_CHECK_EQ(row.getInt64("latency_ms").value(), 42);
        DOCTEST_CHECK_EQ(row.getInt64("request_bytes").value(), 100);
        DOCTEST_CHECK_EQ(row.getInt64("response_bytes").value(), 200);
        // 新增 5 列为 NULL（未采集，与真实的 0 区分）
        DOCTEST_CHECK(row.isNull("input_tokens"));
        DOCTEST_CHECK(row.isNull("output_tokens"));
        DOCTEST_CHECK(row.isNull("cache_read_tokens"));
        DOCTEST_CHECK(row.isNull("cache_write_tokens"));
        DOCTEST_CHECK(row.isNull("first_event_ms"));

        // 新列可写可读回（流末回填走的 UPDATE 依赖这几列存在）
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO claude_call_logs (profile_id, method, path, started_at, "
            "input_tokens, output_tokens, cache_read_tokens, cache_write_tokens, "
            "first_event_ms) VALUES (1, 'POST', '/v1/messages', 1700000001, "
            "10, 40, 2, 5, 123)")
                            .isOk());
        auto q2 = db.prepare(
            "SELECT input_tokens, output_tokens, cache_read_tokens, "
            "cache_write_tokens, first_event_ms FROM claude_call_logs "
            "WHERE started_at = 1700000001");
        DOCTEST_REQUIRE(q2.isOk());
        auto r2 = q2.value().fetchAll();
        DOCTEST_REQUIRE(r2.isOk());
        DOCTEST_REQUIRE_EQ(r2.value().size(), 1u);
        DOCTEST_CHECK_EQ(r2.value()[0].getInt64("input_tokens").value(), 10);
        DOCTEST_CHECK_EQ(r2.value()[0].getInt64("output_tokens").value(), 40);
        DOCTEST_CHECK_EQ(r2.value()[0].getInt64("cache_read_tokens").value(), 2);
        DOCTEST_CHECK_EQ(r2.value()[0].getInt64("cache_write_tokens").value(), 5);
        DOCTEST_CHECK_EQ(r2.value()[0].getInt64("first_event_ms").value(), 123);
    } // Database 在此析构（关闭 sqlite 句柄）

    std::filesystem::remove(path, ec);
    return true;
})
