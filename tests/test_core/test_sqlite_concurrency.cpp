// 跨线程共用同一 sqlite 连接的并发纪律守卫。核心不变量：**锁必须盖住每一次句柄访问**，
// 包括 Statement 析构（sqlite3_finalize）与 SqliteDb::close。
//   - 全进程只有一条 core::data::SqliteDb（见 core/database.cpp 的
//     registerAppDatabase），UI 线程与 agent / scheduler 线程打的是同一个连接；
//   - macOS 系统 libsqlite3 是 THREADSAFE=2：sqlite3_threadsafe()==2 且
//     sqlite3_db_mutex()==NULL，连接内部没有自己的互斥锁。所以本仓库那把
//     recursive_mutex 是**唯一**的串行手段，漏一处就直接踩连接内部的空闲链。
// 本用例按真实线程形状打：持锁读（UI）× 无锁读（agent）× 持锁写（scheduler）。
//
// 本用例绿灯 ≠ 没漏锁：连接带 SQLITE_OPEN_FULLMUTEX 打开，同一次漏锁会被 sqlite 内部的
// 连接级互斥串行化掉，不再崩。要验漏锁必须回到无该 flag 的构建。
#include "doctest.h"
#include "core/sqlite_db.h"

#include <sqlite3.h>   // 仅 (c) 段用：sqlite3_threadsafe / sqlite3_db_mutex 证明 FULLMUTEX 生效

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 默认轮数取 5000，比「能稳定复现漏锁」所需多两个数量级。深压测用
// DEVDESK_SQLITE_STRESS_ITERS 放大，默认值不随环境漂移
long stressIters() {
    if (const char* p = std::getenv("DEVDESK_SQLITE_STRESS_ITERS")) {
        const long v = std::atol(p);
        if (v > 0) return v;
    }
    return 5000;
}

// 同时并发的「无外层锁」读线程数。真机上是代理 worker / agent run / scheduler /
// markdown 重扫多路同跑，取 4 贴近实际，也让无锁 finalize 的并发密度够高
constexpr int kAgentThreads = 4;
constexpr int kRows = 50;

} // namespace

REGISTER_TEST(sqlite_cross_thread_same_connection,
              "SqliteDb: 同一连接跨线程并发（UI 持锁读 × agent 无锁读 × scheduler 持锁写）", {
    const long iters = stressIters();

    auto path = test_temp::tempPath("devdesk_sqlite_concurrency.db");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto opened = core::data::SqliteDb::open(path);
    DOCTEST_REQUIRE(opened.isOk());   //以下断言全部前置在起线程之前：
    auto& db = opened.value();        //线程跑起来后 fail-fast 直接 return 会撞
                                      // joinable thread 析构 → terminate

    // 表形状照 agent_runs：INTEGER + **短 TEXT**。短串分配走连接的 lookaside，正是
    // sqlite3DbMallocRawNNTyped 里出问题的那块内存，改成 TEXT 列不加长度约束就失去意义
    DOCTEST_REQUIRE(db.exec(
        "CREATE TABLE runs(id INTEGER PRIMARY KEY, task_id INTEGER, status TEXT,"
        " started_at INTEGER, error TEXT)").isOk());
    {
        std::string seed = "INSERT INTO runs(id,task_id,status,started_at,error) VALUES";
        for (int i = 0; i < kRows; ++i) {
            if (i) seed += ",";
            seed += "(" + std::to_string(i + 1) + ",1,'running',1700000000,NULL)";
        }
        DOCTEST_REQUIRE(db.exec(seed).isOk());
    }

    std::barrier<> gate(3 + kAgentThreads);
    std::atomic<long> uiTurns{0}, agentTurns{0}, schedTurns{0}, failures{0};

    // ① UI 形 = AgentStore::listRuns：外层 db.mutex() 必须覆盖
    //    prepare → bind → fetchAll → Statement 析构整段
    std::thread ui([&] {
        gate.arrive_and_wait();
        for (long i = 0; i < iters; ++i) {
            std::lock_guard<std::recursive_mutex> lock(db.mutex());
            auto st = db.prepare(
                "SELECT id,task_id,status,started_at,error FROM runs WHERE task_id=?"
                " ORDER BY started_at DESC, id DESC LIMIT ?");
            if (st.isFail()) { ++failures; continue; }
            auto& s = st.value();
            if (s.bind(1, static_cast<int64_t>(1)).isFail() ||
                s.bind(2, static_cast<int64_t>(5)).isFail()) { ++failures; continue; }
            auto rows = s.fetchAll();
            if (rows.isFail() || rows.value().size() != 5) { ++failures; continue; }
            ++uiTurns;
        }
    });

    // ② agent 形 = ProfileRepository::getActiveProfile / ClaudeService 纯读路径：
    //    **不持外层锁**，只靠每次调用的 Statement 自带锁，析构点天然落在锁外
    std::vector<std::thread> agents;
    for (int t = 0; t < kAgentThreads; ++t) {
        agents.emplace_back([&, t] {
            gate.arrive_and_wait();
            for (long i = 0; i < iters; ++i) {
                auto st = db.prepare(
                    "SELECT id,task_id,status,started_at,error FROM runs WHERE id=?");
                if (st.isFail()) { ++failures; continue; }
                auto& s = st.value();
                if (s.bind(1, static_cast<int64_t>((i + t) % kRows + 1)).isFail()) {
                    ++failures; continue;
                }
                auto has = s.step();
                if (has.isFail() || !has.value()) { ++failures; continue; }
                auto row = s.currentRow();
                if (row.isFail() || row.value().getString("status").isFail()) {
                    ++failures; continue;
                }
                ++agentTurns;
            }
        });
    }

    // ③ scheduler 形 = AgentStore::appendMessage / startRunExclusive：外层锁内写
    std::thread sched([&] {
        gate.arrive_and_wait();
        for (long i = 0; i < iters; ++i) {
            std::lock_guard<std::recursive_mutex> lock(db.mutex());
            auto st = db.prepare(
                "UPDATE runs SET started_at=started_at+1, status=? WHERE id=?");
            if (st.isFail()) { ++failures; continue; }
            auto& s = st.value();
            if (s.bind(1, std::string("running")).isFail() ||
                s.bind(2, static_cast<int64_t>(i % kRows + 1)).isFail()) {
                ++failures; continue;
            }
            if (s.step().isFail()) { ++failures; continue; }
            ++schedTurns;
        }
    });

    gate.arrive_and_wait();
    ui.join();
    for (auto& a : agents) a.join();
    sched.join();

    // 主症状是崩/挂（SIGSEGV/SIGBUS）；下面几条把「没崩但结果被写坏」也变成红灯
    DOCTEST_CHECK(failures.load() == 0);
    DOCTEST_CHECK(uiTurns.load() == iters);
    DOCTEST_CHECK(agentTurns.load() == static_cast<long>(kAgentThreads) * iters);
    DOCTEST_CHECK(schedTurns.load() == iters);

    auto check = db.prepare("PRAGMA integrity_check");
    DOCTEST_REQUIRE(check.isOk());
    auto stepR = check.value().step();
    DOCTEST_REQUIRE(stepR.isOk());
    DOCTEST_REQUIRE(stepR.value());
    auto row = check.value().currentRow();
    DOCTEST_REQUIRE(row.isOk());
    auto verdict = row.value().getString("integrity_check");
    DOCTEST_REQUIRE(verdict.isOk());
    DOCTEST_CHECK(verdict.value() == "ok");
    return true;
})

// 补强用例：主用例的三种线程形状之外的两个边界，外加一条 FULLMUTEX 自证。
//   (a) columnCount 被无外层锁地打，同时另一线程不断 prepare + 析构（每次析构 = 一次
//       持锁 finalize）。columnCount 必须自带锁，见 sqlite_db.cpp 顶部注释。
//   (b) 语句活得比宿主 SqliteDb 久：语句 move 到宿主作用域外，宿主先析构
//       （close_v2 → 连接转 zombie，内存留到最后一条语句 finalize），两线程再用它。
//       锁对象必须随 Statement 的 shared_ptr 存活——Statement 若持「db 的 mutex
//       引用」，宿主先亡就是悬垂加锁。语句内存也必须活到它自己 finalize（zombie 上读
//       元数据仍返回正确列数）。
//   (c) FULLMUTEX 兜底确实生效：库支持线程安全时（sqlite3_threadsafe() != 0），带 flag
//       打开的连接内部必须有真锁。
// 轮数复用主用例的 stressIters()：本条打的是「一次调用 vs 一次 finalize」的窄窗口，
// 轮数不够等于没打。

REGISTER_TEST(sqlite_stmt_lifecycle_cross_thread,
              "SqliteDb: columnCount 跨线程竞争 + 语句活过宿主（锁 shared_ptr 存活）+ FULLMUTEX 兜底生效", {
    const long iters = stressIters();

    auto path = test_temp::tempPath("devdesk_sqlite_stmt_lifecycle.db");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto opened = core::data::SqliteDb::open(path);
    DOCTEST_REQUIRE(opened.isOk());   //断言全部前置在起线程之前（同主用例：线程跑
    auto& db = opened.value();        //起来后 fail-fast return 会撞 joinable thread）

    //(c) FULLMUTEX 兜底生效的自证
    if (sqlite3_threadsafe() != 0) {   //THREADSAFE=0 的库会忽略该 flag，按库能力门控
        DOCTEST_CHECK(sqlite3_db_mutex(db.handle()) != nullptr);
    }

    DOCTEST_REQUIRE(db.exec(
        "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)").isOk());
    DOCTEST_REQUIRE(db.exec("INSERT INTO t(id,v) VALUES(1,'x')").isOk());

    // (a) 稳定语句 columnCount（自带锁）× 另一线程 prepare→析构（持锁 finalize）
    auto stable = db.prepare("SELECT id,v FROM t WHERE id=?");
    DOCTEST_REQUIRE(stable.isOk());
    DOCTEST_REQUIRE(stable.value().bind(1, static_cast<int64_t>(1)).isOk());
    DOCTEST_REQUIRE(stable.value().columnCount() == 2);   //列数恒定，供跨线程比对

    std::barrier<> gate(3);   //2 个 worker + 本线程（计数含 main，同主用例）
    std::atomic<long> cols{0}, finals{0}, failures{0};

    std::thread reader([&] {
        gate.arrive_and_wait();
        for (long i = 0; i < iters; ++i) {
            // 刻意不持外层锁：columnCount 必须自带锁
            if (stable.value().columnCount() != 2) { ++failures; continue; }
            ++cols;
        }
    });
    std::thread churn([&] {
        gate.arrive_and_wait();
        for (long i = 0; i < iters; ++i) {
            auto st = db.prepare("SELECT id,v FROM t WHERE id=1");
            if (st.isFail()) { ++failures; continue; }
            auto rows = st.value().fetchAll();
            if (rows.isFail() || rows.value().size() != 1) { ++failures; continue; }
            ++finals;
        }   // 本轮 st 在此析构 → releaseStmt() 持锁 finalize
    });

    gate.arrive_and_wait();
    reader.join();
    churn.join();
    DOCTEST_CHECK(failures.load() == 0);
    DOCTEST_CHECK(cols.load() == iters);
    DOCTEST_CHECK(finals.load() == iters);

    // (b) 语句活过宿主（move 出 + 宿主先析构，close_v2 转 zombie）
    auto orphanPath = test_temp::tempPath("devdesk_sqlite_orphan_stmt.db");
    std::filesystem::remove(orphanPath, ec);
    std::unique_ptr<core::data::Statement> orphan;
    {
        auto owner = core::data::SqliteDb::open(orphanPath);
        DOCTEST_REQUIRE(owner.isOk());
        auto st = owner.value().prepare("SELECT 1 AS one");
        DOCTEST_REQUIRE(st.isOk());
        orphan = std::make_unique<core::data::Statement>(std::move(st.value()));
    }   // owner 析构：句柄 close_v2；语句未 finalize → zombie，内存留到语句终结

    DOCTEST_REQUIRE(orphan != nullptr);
    DOCTEST_REQUIRE(orphan->columnCount() == 1);

    std::barrier<> gate2(3);   // 2 个 worker + 本线程
    std::atomic<long> orphanReads{0}, orphanBad{0};
    auto hammerOrphan = [&] {
        gate2.arrive_and_wait();
        for (long i = 0; i < iters; ++i) {
            // 锁若没随 shared_ptr 存活（Statement 持 db 的 mutex 引用），这里就是对已释放 mutex
            // 加锁 → UAF
            if (orphan->columnCount() != 1) { ++orphanBad; continue; }
            ++orphanReads;
        }
    };
    std::thread o1(hammerOrphan), o2(hammerOrphan);
    gate2.arrive_and_wait();
    o1.join();
    o2.join();
    DOCTEST_CHECK(orphanBad.load() == 0);
    DOCTEST_CHECK(orphanReads.load() == 2 * iters);

    orphan.reset();   //Statement 析构 → 持锁 finalize → zombie 连接此刻真正回收
    std::filesystem::remove(orphanPath, ec);
    return true;
})
