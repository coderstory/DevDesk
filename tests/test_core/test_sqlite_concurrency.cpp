// tests/test_core/test_sqlite_concurrency.cpp
// 跨线程共用同一 sqlite 连接的并发纪律守卫（2026-10-02 真机 1.7.4 崩溃后补）。
//
// 背景（崩溃现场）：UI 线程 AgentModule::refreshData → AgentStore::listRuns 的
// sqlite3_step 里 SIGSEGV（sqlite3Reprepare … sqlite3DbMallocRawNNTyped），同一
// 时刻 agent run 线程正在 ProfileRepository::getActiveProfile 一类的**纯读**路径
// 上取数。两条路径用的是同一个 core::data::SqliteDb（进程级唯一连接，见
// core/database.cpp 的 registerAppDatabase）。
//
// 根因不是「AgentStore 没加锁」，而是**锁没有盖住每一次句柄访问**：
//   - macOS 系统 libsqlite3 是 THREADSAFE=2（多线程模式）：sqlite3_threadsafe()==2、
//     sqlite3_db_mutex()==NULL —— 连接内部没有任何自己的互斥锁，同一连接被两个
//     线程同时调用就是直接踩连接内部的 lookaside 空闲链；
//   - 因此本仓库那把 recursive_mutex 是**唯一**的串行手段，而旧实现的
//     ~Statement（sqlite3_finalize）与 SqliteDb::close 都在锁外执行。
//     AgentStore 的方法外层持锁（析构落在锁内），ProfileRepository/ClaudeService
//     等着眼于「每次调用自带锁」的读路径，其 Statement 析构点落在锁外 →
//     一次无锁 finalize 撞上另一线程在跑的 step = 堆/空闲链损坏。
//
// 本用例就是按这两条形状打：持锁读（UI）× 无锁读（agent）× 持锁写（scheduler），
// 三个线程 barrier 对齐、有界轮数、无 sleep。
// **变异验证口径**：回退 src/core/sqlite_db.cpp 的持锁 finalize/close（或把
// Statement::releaseStmt 改回裸 sqlite3_finalize），本用例在数百轮内即
// SIGSEGV/SIGBUS 或挂死（= ctest 失败）；恢复修复则 8 线程 × 5000 轮稳定全绿。
#include "doctest.h"
#include "core/sqlite_db.h"

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

// 默认轮数：本机（arm64 / macOS）实测无修复时 300 轮就足以稳定崩，取 5000 留
// 两个数量级余量；Debug 构建约 0.35 s，套件可接受。深压测用环境变量放大
// （DEVDESK_SQLITE_STRESS_ITERS=200000 之类），默认值不随环境漂移。
long stressIters() {
    if (const char* p = std::getenv("DEVDESK_SQLITE_STRESS_ITERS")) {
        const long v = std::atol(p);
        if (v > 0) return v;
    }
    return 5000;
}

// 同时并发的「无外层锁」读线程数：真机上是代理 worker / agent run / scheduler /
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
    DOCTEST_REQUIRE(opened.isOk());   // 以下断言全部前置在起线程之前：
    auto& db = opened.value();        // 线程跑起来后 fail-fast 直接 return 会撞
                                      // joinable thread 析构 → terminate

    // 表形状照 agent_runs：INTEGER + **短 TEXT**（短串分配走连接的 lookaside，
    // 正是崩溃点 sqlite3DbMallocRawNNTyped 里出问题的那块内存）
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

    // ① UI 形 = AgentStore::listRuns（AgentModule::refreshData 每帧走的路）：
    //    外层 db.mutex() 覆盖 prepare → bind → fetchAll → Statement 析构整段
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
    //    **不持外层锁**，只靠每次调用的 Statement 自带锁——析构点天然落在锁外
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

    // 崩/挂是旧实现的主症状（SIGSEGV/SIGBUS，见文件头）；下面两条是把「没崩但
    // 结果被写坏」也变成红灯的兜底断言
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
