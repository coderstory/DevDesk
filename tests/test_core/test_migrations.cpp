// tests/test_core/test_migrations.cpp
// 测试 Database + migrations
#include "doctest.h"
#include "core/database.h"
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
    return std::filesystem::temp_directory_path() / name;
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
    std::filesystem::remove(path);

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

    std::filesystem::remove(path);
    return true;
})

REGISTER_TEST(migrate_seeds_default_todo_columns,
              "Database: migrate inserts 4 default Todo columns", {
    auto path = tempDbPath("seed");
    std::filesystem::remove(path);

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

    std::filesystem::remove(path);
    return true;
})

REGISTER_TEST(migrate_is_idempotent, "Database: migrate is idempotent", {
    auto path = tempDbPath("idempotent");
    std::filesystem::remove(path);

    // 第一次 open + migrate
    auto r1 = core::Database::open(path);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(r1.value().migrate().isOk());

    // 关掉再打开，第二次 migrate 不应报错也不应重复插入
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

    std::filesystem::remove(path);
    return true;
})
