#include "doctest.h"
#include "core/sqlite_db.h"
#include "core/result.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>

REGISTER_TEST(sqlite_open_memory, "SqliteDb: open in-memory", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_CHECK(r.isOk());
    return true;
})

REGISTER_TEST(sqlite_open_file, "SqliteDb: open file", {
    auto path = test_temp::tempPath("devdesk_test.db");
    // 预清理：无异常版本
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        auto r = core::data::SqliteDb::open(path);
        DOCTEST_CHECK(r.isOk());
        if (r.isOk()) {
            auto exec = r.value().exec("CREATE TABLE t (id INTEGER, name TEXT)");
            DOCTEST_CHECK(exec.isOk());
        }
    } // SqliteDb 在此析构（关闭 sqlite 句柄），Windows 不允许删除打开中的文件
    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(sqlite_bind_step, "SqliteDb: prepare + bind + step", {
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
    return true;
})

REGISTER_TEST(sqlite_fetch_all, "SqliteDb: fetchAll", {
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
    return true;
})

REGISTER_TEST(sqlite_transaction, "SqliteDb: transaction commit", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec("CREATE TABLE t (x INTEGER)").isOk());
    auto tx = db.transaction([&]() -> core::Result<void> {
        if (db.exec("INSERT INTO t VALUES (1)").isFail()) return core::Result<void>::fail(core::ErrorCode::Internal, "insert 1");
        if (db.exec("INSERT INTO t VALUES (2)").isFail()) return core::Result<void>::fail(core::ErrorCode::Internal, "insert 2");
        return core::Result<void>::ok();
    });
    DOCTEST_CHECK(tx.isOk());
    auto prep = db.prepare("SELECT COUNT(*) FROM t");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    auto row = rows.value()[0];
    DOCTEST_CHECK_EQ(std::get<int64_t>(row.values[0]), 2);
    return true;
})
// 审计 2026-09-23 [8] 回归：transaction() 全程持锁，并发 INSERT 不再穿插
// 进开放事务。旧实现锁只盖 BEGIN/COMMIT，fn() 期间放开——worker 线程的
// exec INSERT 落进 UI 线程未提交的事务里，fn 失败 ROLLBACK 时被连带吞掉
// （表现为 count==0）。修复后 worker 阻塞在 BEGIN 前的锁上，事务结束才
// 落语句（count==1：只有自己的写被回滚）。
REGISTER_TEST(sqlite_transaction_serializes_concurrent_writers,
              "SqliteDb: transaction 全程持锁，并发 INSERT 不穿插（审计[8]）", {
    auto r = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(r.isOk());
    auto& db = r.value();
    DOCTEST_REQUIRE(db.exec(
        "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, who TEXT)").isOk());

    std::atomic<bool> fnInside{false};
    core::Result<void> workerInsert = core::Result<void>::ok();

    std::thread worker([&] {
        while (!fnInside.load()) std::this_thread::yield();
        workerInsert = db.exec("INSERT INTO t (who) VALUES ('worker')");
    });

    auto tx = db.transaction([&]() -> core::Result<void> {
        auto p = db.prepare("INSERT INTO t (who) VALUES ('tx')");
        if (p.isFail()) return core::Result<void>::fail(p.errorCode(), p.errorMessage());
        p.value().step();
        fnInside.store(true);
        // 给 worker 充分的穿插机会（旧实现此时锁已放开）
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return core::Result<void>::fail(core::ErrorCode::Internal, "deliberate rollback");
    });
    worker.join();

    DOCTEST_CHECK(tx.isFail());
    DOCTEST_CHECK(workerInsert.isOk());

    auto prep = db.prepare("SELECT COUNT(*) AS c FROM t");
    DOCTEST_REQUIRE(prep.isOk());
    auto rows = prep.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
    auto c = rows.value()[0].getInt64("c");
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK_EQ(c.value(), static_cast<int64_t>(1));
    return true;
})

// 审计 L5 回归：析构时仍有未 finalize 的语句。旧实现用 sqlite3_close（返回
// SQLITE_BUSY 且被忽略）→ 连接静默不关、文件句柄泄漏；改 sqlite3_close_v2
// 后是延迟关闭：db 析构只是把连接变 zombie，最后一条语句 finalize 时才真正
// 回收。用例断言：析构不炸 + 语句 finalize 后 db 文件句柄确实还回来了
// （Windows 上「文件能否删除」即句柄是否泄漏的判据）。
REGISTER_TEST(sqlite_close_with_unfinalized_stmt,
              "SqliteDb: 未 finalize 语句存在时析构延迟关闭、句柄随 finalize 回收（审计 L5）", {
    auto path = test_temp::tempPath("devdesk_test_close_v2.db");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);

    std::optional<core::data::Statement> kept;
    {
        auto r = core::data::SqliteDb::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value();
        DOCTEST_REQUIRE(db.exec("CREATE TABLE t (id INTEGER)").isOk());
        auto prep = db.prepare("SELECT id FROM t");
        DOCTEST_REQUIRE(prep.isOk());
        // 语句比 db 活得久（移出内层作用域）；注意语句析构前不可再 step/bind：
        // Statement 持有 db 的 mutex 引用，db 没了就是悬垂（本用例只让它析构）
        kept.emplace(std::move(prep.value()));
    } // db 在此析构：有未 finalize 的语句 → 延迟关闭，不得崩

    kept.reset(); // Statement 析构 → sqlite3_finalize → 延迟关闭此时生效

    DOCTEST_CHECK(std::filesystem::remove(path, ec)); // 句柄已归还（旧实现此处必失败）
    DOCTEST_CHECK_FALSE(std::filesystem::exists(path));
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return true;
})
