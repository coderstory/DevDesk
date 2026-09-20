#include "doctest.h"
#include "core/sqlite_db.h"
#include "core/result.h"

#include <filesystem>

DOCTEST_TEST_CASE("SqliteDb: open in-memory") {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_CHECK(r.isOk());
}

DOCTEST_TEST_CASE("SqliteDb: open file") {
    auto path = std::filesystem::temp_directory_path() / "devdesk_test.db";
    std::filesystem::remove(path);
    auto r = core::data::SqliteDb::open(path);
    DOCTEST_CHECK(r.isOk());
    if (r.isOk()) {
        auto exec = r.value().exec("CREATE TABLE t (id INTEGER, name TEXT)");
        DOCTEST_CHECK(exec.isOk());
    }
    std::filesystem::remove(path);
}

DOCTEST_TEST_CASE("SqliteDb: prepare + bind + step") {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (id INTEGER, name TEXT)").isOk());
    auto prep = db.prepare("INSERT INTO t VALUES (?, ?)");
    DOCTEST_REQUIRE(prep.isOk());
    auto& stmt = prep.value();
    DOCTEST_REQUIRE(stmt.bind(1, static_cast<int64_t>(1)).isOk());
    DOCTEST_REQUIRE(stmt.bind(2, std::string("hello")).isOk());
    auto step = stmt.step();
    DOCTEST_CHECK(step.isOk());
}

DOCTEST_TEST_CASE("SqliteDb: fetchAll") {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (id INTEGER, name TEXT)").isOk());
    DOCTEST_REQUIRE(db.exec("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')").isOk());
    auto prep = db.prepare("SELECT id, name FROM t ORDER BY id");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_CHECK_EQ(rows.value().size(), 3u);
}

DOCTEST_TEST_CASE("SqliteDb: transaction commit") {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (x INTEGER)").isOk());
    auto tx = db.transaction([&]() {
        DOCTEST_REQUIRE(db.exec("INSERT INTO t VALUES (1)").isOk());
        DOCTEST_REQUIRE(db.exec("INSERT INTO t VALUES (2)").isOk());
        return core::Result<void>::ok();
    });
    DOCTEST_CHECK(tx.isOk());
    auto prep = db.prepare("SELECT COUNT(*) FROM t");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    auto row = rows.value()[0];
    DOCTEST_CHECK_EQ(std::get<int64_t>(row.values[0]), 2);
}