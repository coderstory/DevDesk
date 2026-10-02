// tests/test_core/test_app_config.cpp
// core::appconfig（通用「键 → 值」配置存储）的直接用例
//
// 批次 E2（2026-10-03，用户第 17 条「所有 app 配置全部进 SQLite，不要 JSON
// 文件了」）。这里钉四件文件时代不存在、换库之后必须成立的事：
//   1) 值**逐字节**往返：中文 / `{}` / `[]` / 含换行 / 空串 / 超长串，
//      以及「键不存在 ≠ 值为空串」这条区分；
//   2) 键之间**互不影响**（顶层键拆分的存储层前提）：改 A 不动 B ——
//      必须真的写两条不同的键再只改一条，拿同一键写两遍是自证式断言；
//   3) 多线程各写各的键：全入锁后无崩溃、无丢失（memory 里那条真机 SIGSEGV
//      的教训正落在这一条上）；
//   4) 库未就绪 / 表不存在 → **返回 Result 错误**，不静默成功。
#include "doctest.h"
#include "test_support.h"

#include "core/app_config.h"
#include "core/database.h"
#include "core/result.h"
#include "core/schema.h"
#include "core/sqlite_db.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_appconfig_";

// 100 KB 的值：TEXT 列没有长度上限，这一条钉住「不会被截断」
std::string veryLongValue() {
    std::string s;
    s.reserve(100 * 1024);
    for (int i = 0; i < 100 * 1024; ++i) s.push_back(static_cast<char>('a' + (i % 26)));
    return s;
}

} // namespace

// ============ 1. 值逐字节往返 ============

REGISTER_TEST(app_config_value_roundtrip,
              "app_config: 值逐字节往返（中文/JSON/换行/空串/超长串）", {
    test_support::TestDb env(kDbPrefix, "roundtrip");
    core::data::SqliteDb& db = env.handle();

    struct Case { const char* name; std::string value; };
    const std::vector<Case> cases = {
        {"ascii",        "hello"},
        {"chinese",      "中文配置值：主题/字号/模块开关"},
        {"empty_object", "{}"},
        {"empty_array",  "[]"},
        {"nested_json",  R"({"a":[1,2,{"b":null}],"c":"中文"})"},
        {"newlines",     "line1\nline2\r\nline3\n"},
        {"empty",        ""},                       // 空串是合法值，不是「没有」
        {"quotes",       R"("he said \"hi\"")"},
        {"backslash",    "C:\\path\\to\\file"},
        {"long",         veryLongValue()},
    };

    for (const auto& c : cases) {
        const std::string key = std::string("cfg:") + c.name;
        auto wrote = core::appconfig::set(db, key, c.value);
        DOCTEST_REQUIRE(wrote.isOk());
        auto got = core::appconfig::get(db, key);
        DOCTEST_REQUIRE(got.isOk());
        // 逐字节相等：size 与每个字节都比，不做任何规范化
        DOCTEST_CHECK_EQ(got.value().size(), c.value.size());
        DOCTEST_CHECK(got.value() == c.value);
    }

    // 「键不存在」与「值为空串」必须能区分 —— 否则配置里的空串会被当成没配过
    auto missing = core::appconfig::get(db, "cfg:never-written");
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(missing.errorCode()),
                     static_cast<int>(core::ErrorCode::NotFound));
    auto emptyValued = core::appconfig::get(db, "cfg:empty");
    DOCTEST_REQUIRE(emptyValued.isOk());
    DOCTEST_CHECK_EQ(emptyValued.value(), std::string(""));

    // has() 与 get() 对同一键给出一致答案
    DOCTEST_CHECK(core::appconfig::has(db, "cfg:empty").value());
    DOCTEST_CHECK(!core::appconfig::has(db, "cfg:never-written").value());

    // 重复写同一键 = 整份替换（不追加、不拼接）
    DOCTEST_REQUIRE(core::appconfig::set(db, "cfg:ascii", "second").isOk());
    DOCTEST_CHECK_EQ(core::appconfig::get(db, "cfg:ascii").valueOr(""),
                     std::string("second"));

    // 删键后确实读不到；再删一次幂等成功
    DOCTEST_REQUIRE(core::appconfig::erase(db, "cfg:ascii").isOk());
    DOCTEST_CHECK(core::appconfig::get(db, "cfg:ascii").isFail());
    DOCTEST_CHECK(core::appconfig::erase(db, "cfg:ascii").isOk());
    return true;
})

// ============ 2. 键之间互不影响（顶层键拆分的前提）============

REGISTER_TEST(app_config_keys_are_independent,
              "app_config: 改一个键不动另一个键（顶层键拆分前提）", {
    test_support::TestDb env(kDbPrefix, "independence");
    core::data::SqliteDb& db = env.handle();

    // 先写**两条不同**的键，值也刻意不同（写成同一个值的话，下面那条
    // 「B 没被动过」就成了恒真断言）
    DOCTEST_REQUIRE(core::appconfig::set(db, "cfg:alpha", "AAA-original").isOk());
    DOCTEST_REQUIRE(core::appconfig::set(db, "cfg:beta",  "BBB-original").isOk());
    DOCTEST_CHECK_EQ(core::appconfig::get(db, "cfg:alpha").valueOr(""),
                     std::string("AAA-original"));
    DOCTEST_CHECK_EQ(core::appconfig::get(db, "cfg:beta").valueOr(""),
                     std::string("BBB-original"));

    // 只改 alpha
    DOCTEST_REQUIRE(core::appconfig::set(db, "cfg:alpha", "AAA-changed").isOk());

    // alpha 真的变了
    DOCTEST_CHECK_EQ(core::appconfig::get(db, "cfg:alpha").valueOr(""),
                     std::string("AAA-changed"));
    // beta 一字未动
    DOCTEST_CHECK_EQ(core::appconfig::get(db, "cfg:beta").valueOr(""),
                     std::string("BBB-original"));

    // 删 alpha 也不牵连 beta
    DOCTEST_REQUIRE(core::appconfig::erase(db, "cfg:alpha").isOk());
    DOCTEST_CHECK(core::appconfig::get(db, "cfg:alpha").isFail());
    DOCTEST_CHECK_EQ(core::appconfig::get(db, "cfg:beta").valueOr(""),
                     std::string("BBB-original"));
    return true;
})

REGISTER_TEST(app_config_setmany_and_prefix,
              "app_config: setMany 批量写 + listWithPrefix 按命名空间取回", {
    test_support::TestDb env(kDbPrefix, "setmany");
    core::data::SqliteDb& db = env.handle();

    // 另一个命名空间的同名键，用来证明前缀过滤不是「全表扫」
    DOCTEST_REQUIRE(core::appconfig::set(db, "other:alpha", "OTHER").isOk());

    std::vector<core::appconfig::Entry> batch;
    batch.push_back({"ns1:one", "1", 0});
    batch.push_back({"ns1:two", "2", 0});
    batch.push_back({"ns1:three", "3", 0});
    DOCTEST_REQUIRE(core::appconfig::setMany(db, batch).isOk());

    auto listed = core::appconfig::listWithPrefix(db, "ns1:");
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_REQUIRE_EQ(listed.value().size(), static_cast<size_t>(3));
    // 按 key **字典序**升序（不是插入序）：ns1:one < ns1:three < ns1:two。
    // 期望值按字典序写死，这样将来谁把 ORDER BY 去掉改成插入序，这条立刻红。
    DOCTEST_CHECK_EQ(listed.value()[0].key, std::string("ns1:one"));
    DOCTEST_CHECK_EQ(listed.value()[1].key, std::string("ns1:three"));
    DOCTEST_CHECK_EQ(listed.value()[2].key, std::string("ns1:two"));
    DOCTEST_CHECK_EQ(listed.value()[2].value, std::string("2"));
    DOCTEST_CHECK_EQ(listed.value()[1].value, std::string("3"));
    // 前缀之外的键不在结果里
    DOCTEST_CHECK_EQ(core::appconfig::listWithPrefix(db, "other:").value().size(),
                     static_cast<size_t>(1));
    // updated_at 被写进去了（不是 0）
    DOCTEST_CHECK(listed.value()[0].updatedAt > 0);

    // 空批 = 无操作成功（不是失败）
    DOCTEST_CHECK(core::appconfig::setMany(db, {}).isOk());

    // 前缀含 LIKE 元字符时按字面比，不得当成通配符
    DOCTEST_REQUIRE(core::appconfig::set(db, "a%b:x", "P").isOk());
    DOCTEST_REQUIRE(core::appconfig::set(db, "axb:y", "Q").isOk());
    auto literal = core::appconfig::listWithPrefix(db, "a%b:");
    DOCTEST_REQUIRE(literal.isOk());
    DOCTEST_REQUIRE_EQ(literal.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(literal.value()[0].key, std::string("a%b:x"));

    // eraseWithPrefixExcept：保留 keep 里的，清掉其余（= 旧「重写整份文件」语义）
    DOCTEST_REQUIRE(core::appconfig::eraseWithPrefixExcept(
                        db, "ns1:", {"ns1:two"}).isOk());
    auto after = core::appconfig::listWithPrefix(db, "ns1:");
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_REQUIRE_EQ(after.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(after.value()[0].key, std::string("ns1:two"));
    return true;
})

// ============ 3. 并发（memory 里那条真机 SIGSEGV 的教训）============

REGISTER_TEST(app_config_concurrent_writers,
              "app_config: 多线程各写各的键——无崩溃、无丢失（sqlite 跨线程纪律）", {
    test_support::TestDb env(kDbPrefix, "concurrent");
    core::data::SqliteDb& db = env.handle();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 40;
    std::atomic<int> failures{0};

    // 8 个线程各写 40 个**互不相同**的键。漏一把锁的后果在真机上是连接内部
    // 空闲链损坏 → SIGSEGV；这里至少要证明：全程不崩，且一条都没丢。
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&db, &failures, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key =
                    "conc:t" + std::to_string(t) + ":i" + std::to_string(i);
                if (core::appconfig::set(db, key, "v" + std::to_string(i)).isFail()) {
                    ++failures;
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    DOCTEST_CHECK_EQ(failures.load(), 0);
    // 逐条读回：一条都不能少（丢失 = 某个线程的写被另一个线程的写覆盖）
    int found = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            const std::string key =
                "conc:t" + std::to_string(t) + ":i" + std::to_string(i);
            auto v = core::appconfig::get(db, key);
            if (v.isOk() && v.value() == "v" + std::to_string(i)) ++found;
        }
    }
    DOCTEST_CHECK_EQ(found, kThreads * kPerThread);

    auto listed = core::appconfig::listWithPrefix(db, "conc:");
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_CHECK_EQ(static_cast<int>(listed.value().size()),
                     kThreads * kPerThread);
    return true;
})

REGISTER_TEST(app_config_concurrent_readers_and_writers,
              "app_config: 多线程读写同一张表——读到的值永远自洽", {
    test_support::TestDb env(kDbPrefix, "rw");
    core::data::SqliteDb& db = env.handle();

    std::atomic<int> torn{0};
    std::atomic<int> writeFails{0};
    std::atomic<int> reads{0};

    // 迭代次数**定死**而不是「跑到 stop 标志」：早先那版让读者自旋等 stop、
    // 却又在 join 之后才置 stop —— 死锁，用例永远不返回（实测卡死）。
    // 定时长 + 定时长的双方都不需要停机标志，退出条件与挂钟无关。
    constexpr int kWrites = 3000;
    constexpr int kReadsEach = 3000;

    // 写者把同一个键反复在两个值之间翻；读者每次读到的必须是**其中一个**，
    // 不能是第三个值（说明读到了半写状态 = 锁范围没盖住）
    std::thread writer([&]() {
        for (int i = 0; i < kWrites; ++i) {
            const std::string v = (i % 2 == 0) ? "AAA" : "BBB";
            if (core::appconfig::set(db, "rw:k", v).isFail()) ++writeFails;
        }
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            for (int i = 0; i < kReadsEach; ++i) {
                auto v = core::appconfig::get(db, "rw:k");
                if (v.isOk()) {
                    ++reads;
                    if (v.value() != "AAA" && v.value() != "BBB") ++torn;
                }
            }
        });
    }
    for (auto& r : readers) r.join();
    writer.join();

    DOCTEST_CHECK_EQ(writeFails.load(), 0);
    DOCTEST_CHECK_EQ(torn.load(), 0);
    // 读者确实读到了东西（否则「读到自洽值」是恒真断言 —— 读者可能一次都没跑）
    DOCTEST_CHECK(reads.load() > 0);
    return true;
})

// ============ 4. 库未就绪 / 表不存在 → 明确失败，不静默成功 ============

REGISTER_TEST(app_config_missing_table_fails,
              "app_config: 表不存在 → 每个操作都返回 fail（不静默成功）", {
    // 故意**不** applySchema：只有一条连接，没有任何表
    test_support::InMemoryDb mem;
    core::data::SqliteDb& db = mem.handle();

    auto setR = core::appconfig::set(db, "k", "v");
    DOCTEST_CHECK(setR.isFail());
    auto getR = core::appconfig::get(db, "k");
    DOCTEST_CHECK(getR.isFail());
    auto hasR = core::appconfig::has(db, "k");
    DOCTEST_CHECK(hasR.isFail());
    auto eraseR = core::appconfig::erase(db, "k");
    DOCTEST_CHECK(eraseR.isFail());
    auto listR = core::appconfig::listWithPrefix(db, "k");
    DOCTEST_CHECK(listR.isFail());
    auto manyR = core::appconfig::setMany(db, {{"a", "1", 0}});
    DOCTEST_CHECK(manyR.isFail());
    auto pruneR = core::appconfig::eraseWithPrefixExcept(db, "k", {"k"});
    DOCTEST_CHECK(pruneR.isFail());
    // 错误消息要点名缺的是什么，别只回一个泛化失败
    DOCTEST_CHECK(!setR.errorMessage().empty());
    return true;
})

REGISTER_TEST(app_config_unexpected_value_type_fails,
              "app_config: value 列类型不是 TEXT 时读回如实报错", {
    test_support::InMemoryDb mem;
    core::data::SqliteDb& db = mem.handle();
    // 手搓一张「value 是 INTEGER」的同名表（不靠 applySchema，故能造出
    // 与 schema 不一致的形态）——读侧必须报错而不是悄悄把 0 当成字符串
    DOCTEST_REQUIRE(db.exec(
        "CREATE TABLE app_config (key TEXT PRIMARY KEY, value INTEGER, "
        "updated_at INTEGER)").isOk());
    DOCTEST_REQUIRE(db.exec(
        "INSERT INTO app_config (key, value, updated_at) "
        "VALUES ('n', 12345, 1)").isOk());

    auto got = core::appconfig::get(db, "n");
    DOCTEST_CHECK(got.isFail());   // 不是 string → getString 报错
    DOCTEST_CHECK_EQ(static_cast<int>(got.errorCode()),
                     static_cast<int>(core::ErrorCode::ValidationError));
    return true;
})
