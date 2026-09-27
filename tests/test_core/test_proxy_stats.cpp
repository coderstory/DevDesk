// tests/test_core/test_proxy_stats.cpp
// 共享流式统计测试（首页 Token 速率卡片，设计 §2.5 / §2.7 / §6.3）
//
// 全部注入假时钟（steady 毫秒），测试前后 resetForTest 隔离全局状态。
#include "doctest.h"
#include "core/proxy_stats.h"

#include <atomic>
#include <thread>
#include <vector>

namespace {

// 假时钟 RAII：测试结束自动 reset（防污染后续用例）
struct FakeClock {
    int64_t now = 1000000;
    FakeClock() { core::proxy_stats::setTestClock([this] { return now; }); }
    ~FakeClock() { core::proxy_stats::resetForTest(); }
};

} // namespace

REGISTER_TEST(proxy_stats_single_stream_rate,
              "proxy_stats: 单流 2s 窗口速率 + 累计", {
    core::proxy_stats::resetForTest();
    FakeClock clock;
    const uint64_t id = core::proxy_stats::beginStream();

    // t=0s message_start usage=5；t=0.5s 28；t=2s 102（累计值口径）
    core::proxy_stats::updateStream(id, 5);
    clock.now += 500;
    core::proxy_stats::updateStream(id, 28);
    clock.now += 1500;
    core::proxy_stats::updateStream(id, 102);

    const auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 1);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 102);
    // 窗口 [t-2s, t]：基线 t=0s 采样（total 5），增益 97 → 97/2 = 48.5
    DOCTEST_CHECK(snap.rateTps > 48.0 && snap.rateTps < 49.0);
    DOCTEST_CHECK(snap.lastActivityMs == 0);
    return true;
})

REGISTER_TEST(proxy_stats_idle_decay,
              "proxy_stats: 静止 >2s 速率衰减为 0，累计保留", {
    core::proxy_stats::resetForTest();
    FakeClock clock;
    const uint64_t id = core::proxy_stats::beginStream();
    core::proxy_stats::updateStream(id, 10);
    clock.now += 5000; // 5s 无新事件

    const auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.sessionTokens, 10);
    DOCTEST_CHECK(snap.rateTps < 0.001);
    DOCTEST_CHECK_EQ(snap.lastActivityMs, 5000);
    core::proxy_stats::endStream(id);
    return true;
})

REGISTER_TEST(proxy_stats_multi_stream_sum,
              "proxy_stats: 多流总和 + endStream 后累计保留", {
    core::proxy_stats::resetForTest();
    FakeClock clock;
    const uint64_t a = core::proxy_stats::beginStream();
    const uint64_t b = core::proxy_stats::beginStream();
    DOCTEST_CHECK(a != b); // 流 id 唯一

    core::proxy_stats::updateStream(a, 20);
    core::proxy_stats::updateStream(b, 30); // total 50 @ t=0s
    clock.now += 2000;
    core::proxy_stats::updateStream(a, 40);  // +20
    core::proxy_stats::updateStream(b, 50);  // +20 → total 90 @ t=2s

    auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 2);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 90);
    // 窗口增益 90-50=40 / 2s = 20 tps
    DOCTEST_CHECK(snap.rateTps > 19.0 && snap.rateTps < 21.0);

    core::proxy_stats::endStream(a);
    snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 1);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 90); // 累计不回退
    core::proxy_stats::endStream(b);
    snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 0);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 90);
    return true;
})

REGISTER_TEST(proxy_stats_idempotent,
              "proxy_stats: endStream 幂等 + 未知 id 安全 + 重复/回退 update 忽略", {
    core::proxy_stats::resetForTest();
    FakeClock clock;
    const uint64_t id = core::proxy_stats::beginStream();
    core::proxy_stats::updateStream(id, 100);
    core::proxy_stats::updateStream(id, 100); // 累计值重复：不算增量
    core::proxy_stats::updateStream(id, 90);  // 回退：忽略
    auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.sessionTokens, 100);

    core::proxy_stats::endStream(id);
    core::proxy_stats::endStream(id);            // 幂等
    core::proxy_stats::endStream(999999);        // 未知 id 不崩
    core::proxy_stats::updateStream(id, 200);    // 已结束：忽略
    snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 0);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 100);
    return true;
})

REGISTER_TEST(proxy_stats_first_burst_rate,
              "proxy_stats: 首 burst 前 2s 无老采样时按最老采样算速率", {
    // 回归（W6 终验发现）：环内没有 ≥2s 老采样时基线曾取 0/绝对时钟，
    // 导致首 burst 速率低估。假时钟起点取小值 + 首采样累计非零，
    // 确保覆盖 found=false 分支（既有用例起点 1e6ms 恰好绕开该分支）。
    core::proxy_stats::resetForTest();
    FakeClock clock;
    clock.now = 1000;
    const uint64_t id = core::proxy_stats::beginStream();

    // t=1000ms 累计 100；t=2500ms 累计 400（窗口内全部采样 <2s 老）
    core::proxy_stats::updateStream(id, 100);
    clock.now += 1500;
    core::proxy_stats::updateStream(id, 400);

    const auto snap = core::proxy_stats::snapshot();
    // 基线 = 最老采样（t=1000, total=100）：增益 300 / 1500ms = 200 tps
    // （旧缺陷算法：(400-0)/2500ms = 160 tps）
    DOCTEST_CHECK(snap.rateTps > 199.0 && snap.rateTps < 201.0);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 400);

    // 首采样即快照（窗口 < kMinWindowMs）→ 报 0 不除微小数
    core::proxy_stats::resetForTest();
    FakeClock clock2;
    clock2.now = 1000;
    const uint64_t id2 = core::proxy_stats::beginStream();
    core::proxy_stats::updateStream(id2, 50);
    clock2.now += 50;
    const auto snap2 = core::proxy_stats::snapshot();
    DOCTEST_CHECK(snap2.rateTps < 0.001);
    return true;
})

REGISTER_TEST(proxy_stats_concurrent,
              "proxy_stats: 多线程 begin/update/end 小压测", {
    core::proxy_stats::resetForTest();
    constexpr int kThreads = 8;
    constexpr int kUpdates = 200;
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&ready] {
            const uint64_t id = core::proxy_stats::beginStream();
            ++ready;
            while (ready.load() < kThreads) {} // 尽量同时开跑
            for (int i = 1; i <= kUpdates; ++i) {
                core::proxy_stats::updateStream(id, i);
            }
            core::proxy_stats::endStream(id);
        });
    }
    for (auto& th : threads) th.join();

    const auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 0);
    DOCTEST_CHECK_EQ(snap.sessionTokens,
                     static_cast<int64_t>(kThreads * kUpdates));
    DOCTEST_CHECK(snap.rateTps >= 0.0); // 真实时钟下速率不定，只验不炸
    core::proxy_stats::resetForTest();
    return true;
})
