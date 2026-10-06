// proxy_stats 是进程级全局统计，updateStream 收的是**累计值**而非增量；全部用例注入假
// 时钟（steady 毫秒），并在前后 resetForTest 隔离全局状态。
#include "doctest.h"
#include "core/proxy_stats.h"

#include <atomic>
#include <thread>
#include <vector>

namespace {

// 假时钟 RAII：析构时自动 reset，防污染后续用例
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

    // t=0s 累计 5；t=0.5s 累计 28；t=2s 累计 102
    core::proxy_stats::updateStream(id, 5);
    clock.now += 500;
    core::proxy_stats::updateStream(id, 28);
    clock.now += 1500;
    core::proxy_stats::updateStream(id, 102);

    const auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 1);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 102);
    // 窗口 [t-2s, t]：基线取 t=0s 那次采样（total 5），增益 97 → 97/2 = 48.5
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
    clock.now += 5000;  // 5s 无新事件 → 速率归零

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
    DOCTEST_CHECK(a != b);  // 流 id 唯一

    core::proxy_stats::updateStream(a, 20);
    core::proxy_stats::updateStream(b, 30);  // b 累计 30，此时总和 50
    clock.now += 2000;
    core::proxy_stats::updateStream(a, 40);  // +20
    core::proxy_stats::updateStream(b, 50);  // 两次各 +20 → total 90 @ t=2s

    auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 2);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 90);
    // 窗口增益 90-50=40，40 / 2s = 20 tps
    DOCTEST_CHECK(snap.rateTps > 19.0 && snap.rateTps < 21.0);

    core::proxy_stats::endStream(a);
    snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 1);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 90);  // endStream 不回退累计值
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
    core::proxy_stats::updateStream(id, 100); // 累计值重复上报 = 不算增量
    core::proxy_stats::updateStream(id, 90);  // 累计值回退 = 忽略
    auto snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.sessionTokens, 100);

    core::proxy_stats::endStream(id);
    core::proxy_stats::endStream(id);            // 重复 endStream 幂等
    core::proxy_stats::endStream(999999);        // 未知 id 不崩
    core::proxy_stats::updateStream(id, 200);    // 已结束的流再 update 忽略
    snap = core::proxy_stats::snapshot();
    DOCTEST_CHECK_EQ(snap.activeStreams, 0);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 100);
    return true;
})

REGISTER_TEST(proxy_stats_first_burst_rate,
              "proxy_stats: 首 burst 前 2s 无老采样时按最老采样算速率", {
    // 假时钟起点必须取小值（这里 1000ms）且首采样累计非零，否则会绕开「窗口内找不到
    // ≥2s 老采样」那条分支，本用例就退化成首 burst 路径的普通用例
    core::proxy_stats::resetForTest();
    FakeClock clock;
    clock.now = 1000;
    const uint64_t id = core::proxy_stats::beginStream();

    // t=1000ms 累计 100；t=2500ms 累计 400（窗口内所有采样都不足 2s 老）
    core::proxy_stats::updateStream(id, 100);
    clock.now += 1500;
    core::proxy_stats::updateStream(id, 400);

    const auto snap = core::proxy_stats::snapshot();
    // 基线取最老那次采样（t=1000, total=100）：增益 300 / 1500ms = 200 tps
    DOCTEST_CHECK(snap.rateTps > 199.0 && snap.rateTps < 201.0);
    DOCTEST_CHECK_EQ(snap.sessionTokens, 400);

    // 首采样即快照（窗口 < kMinWindowMs）→ 报 0，不除以微小的时间窗
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
            while (ready.load() < kThreads) {} // 尽量让 8 个线程同时开跑
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
    DOCTEST_CHECK(snap.rateTps >= 0.0); // 本例用真实时钟，速率不定，故只验不炸
    core::proxy_stats::resetForTest();
    return true;
})

REGISTER_TEST(proxy_stats_rate_series_empty,
              "proxy_stats: rateSeries 空序列兜底 {0.0f}（防 lineChart demo 数据）", {
    core::proxy_stats::resetForTest();
    FakeClock clock;

    const auto series = core::proxy_stats::rateSeries(24);
    DOCTEST_CHECK_EQ(series.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(series[0], 0.0f);

    core::proxy_stats::resetForTest();
    return true;
})

REGISTER_TEST(proxy_stats_rate_series_trim,
              "proxy_stats: rateSeries 裁剪到 maxPoints、末点 == 最近采样", {
    core::proxy_stats::resetForTest();
    FakeClock clock;
    const uint64_t id = core::proxy_stats::beginStream();

    // 8 个 usage 事件（每 400ms 一个，累计值口径），每个事件采样一次速率。第 k 次事件
    // （t=(k-1)*400，累计 V_k）按 2s 窗口口径：k=1: 0（窗口 <100ms）；k=2: 150；
    // k=3: 175；k=4: 183.3；k=5: 200；k=6: 210（基线 t=0）；k=7: 230；k=8: 250
    const int64_t totals[8] = {40,  100, 180, 260,
                               360, 460, 560, 680};
    for (int i = 0; i < 8; ++i) {
        core::proxy_stats::updateStream(id, totals[i]);
        clock.now += 400;
    }

    // 裁剪到 maxPoints=5：取最近 5 点 = [183.3, 200, 210, 230, 250]
    const auto last5 = core::proxy_stats::rateSeries(5);
    DOCTEST_CHECK_EQ(last5.size(), static_cast<size_t>(5));
    DOCTEST_CHECK(last5.front() > 183.0f && last5.front() < 184.0f);
    DOCTEST_CHECK(last5.back() > 249.9f && last5.back() < 250.1f);

    // maxPoints 足够大时不裁剪：8 事件 → 8 点，时间序 = 写入序
    const auto all = core::proxy_stats::rateSeries(64);
    DOCTEST_CHECK_EQ(all.size(), static_cast<size_t>(8));
    DOCTEST_CHECK_EQ(all[0], 0.0f);
    DOCTEST_CHECK_EQ(all[1], 150.0f);
    DOCTEST_CHECK(all[7] > 249.9f && all[7] < 250.1f);

    core::proxy_stats::endStream(id);
    core::proxy_stats::resetForTest();
    return true;
})
