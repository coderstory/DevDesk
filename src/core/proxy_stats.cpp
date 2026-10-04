// src/core/proxy_stats.cpp
// 共享流式统计实现：mutex + 每流累计 + 16 槽采样环
#include "proxy_stats.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace core::proxy_stats {
namespace {

// 速率窗口：最新采样 − 2s 前最近采样
constexpr double kWindowMs = 2000.0;
// 窗口短于此值直接报 0（刚 begin 就 snapshot，避免除微小数）
constexpr double kMinWindowMs = 100.0;

struct Sample {
    int64_t tMs;     // steady 毫秒
    int64_t total;   // 全部流总累计
};

struct State {
    std::mutex mutex;
    uint64_t nextId = 1;
    std::map<uint64_t, int64_t> streams; // id -> 该流累计 output tokens
    int64_t sessionTotal = 0;
    int64_t lastEventMs = -1;            // 最近一次 usage 事件的时刻

    static constexpr int kRingSize = 16;
    Sample ring[kRingSize];
    int ringCount = 0;
    int ringNext = 0;

    // 速率采样序列环（sparkline 数据源）：每个 usage 事件采样一次 rateTps（约 1-2s 一个，
    // 64 点覆盖 ~1-2 分钟），rateSeries 按时间序取最近 N 点
    static constexpr int kRateRingSize = 64;
    double rateRing[kRateRingSize];
    int rateCount = 0;
    int rateNext = 0;

    void pushRateSample(double rate) {
        rateRing[rateNext] = rate;
        rateNext = (rateNext + 1) % kRateRingSize;
        if (rateCount < kRateRingSize) ++rateCount;
    }

    void pushSample(int64_t nowMs) {
        ring[ringNext] = Sample{nowMs, sessionTotal};
        ringNext = (ringNext + 1) % kRingSize;
        if (ringCount < kRingSize) ++ringCount;
    }

    // 按时间序遍历采样环
    template <typename Fn>
    void forEach(Fn&& fn) const {
        const int first = (ringCount < kRingSize)
                              ? 0
                              : ringNext; // 环满时 ringNext 是最老的
        for (int k = 0; k < ringCount; ++k) {
            fn(ring[(first + k) % kRingSize]);
        }
    }

    // 最近 2s 窗口速率：基线 = 2s 前最近一次采样的总累计；找不到（全在 2s 内）则用最老采样、
    // 窗口按实际经过时长算（静止 >2s 时基线追上当前值 → 自然 0）。found=false 分支不能用 baseT=0：
    // steady 时钟是绝对值，会让 windowMs = 绝对时钟，首 burst 前 2 秒速率严重低估
    double rateTps(int64_t nowMs) const {
        if (ringCount == 0) return 0.0;
        bool found = false;
        bool oldest = true;
        int64_t baseTotal = 0;
        int64_t baseT = 0;
        forEach([&](const Sample& s) {
            if (oldest) { // 时间序第一个 = 最老采样：找不到 ≥2s 时的基线
                baseTotal = s.total;
                baseT = s.tMs;
                oldest = false;
            }
            if (s.tMs <= nowMs - static_cast<int64_t>(kWindowMs)) {
                baseTotal = s.total;
                baseT = s.tMs;
                found = true;
            }
        });
        double windowMs = found ? kWindowMs
                                : static_cast<double>(nowMs - baseT);
        if (windowMs < kMinWindowMs) return 0.0;
        return static_cast<double>(sessionTotal - baseTotal) * 1000.0 / windowMs;
    }
};

// 永不析构持有：原函数局部 static 首次使用点是 home compose 的 snapshot()，只跑在 armExitHook
// 之后 → atexit LIFO 使 ~State 恒早于 exit hook；而 exit hook 的 stopProxy 会 join worker，
// worker 取消路径（endStream）仍要写这里 → 对已析构 mutex/map 操作是退出期 UB。与进程同寿即根除。
// resetForTest 仍可清状态
State& state() {
    static auto& s = *new State();
    return s;
}

std::function<int64_t()>& testClock() {
    static std::function<int64_t()> fn;
    return fn;
}

int64_t nowMs() {
    if (auto& fn = testClock()) return fn();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

uint64_t beginStream() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const uint64_t id = s.nextId++;
    s.streams.emplace(id, 0);
    return id;
}

void updateStream(uint64_t id, int64_t cumulativeTokens) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.streams.find(id);
    if (it == s.streams.end()) return; // 已 endStream：忽略（幂等安全）
    const int64_t delta = cumulativeTokens - it->second;
    if (delta <= 0) return; // 累计值口径：非增量直接忽略
    it->second = cumulativeTokens;
    s.sessionTotal += delta;
    const int64_t now = nowMs();
    s.lastEventMs = now;
    s.pushSample(now);
    s.pushRateSample(s.rateTps(now)); // sparkline 数据源
}

void endStream(uint64_t id) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.streams.erase(id); // 不存在时 erase 是 no-op → 天然幂等
}

Snapshot snapshot() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const int64_t now = nowMs();
    Snapshot snap;
    snap.activeStreams = static_cast<int>(s.streams.size());
    snap.sessionTokens = s.sessionTotal;
    snap.rateTps = s.rateTps(now);
    snap.lastActivityMs =
        s.lastEventMs < 0 ? -1 : now - s.lastEventMs;
    return snap;
}

std::vector<float> rateSeries(int maxPoints) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    // 空序列兜底 {0.0f}：防 lineChart 对空 values 填演示数据
    if (s.rateCount == 0 || maxPoints <= 0) return {0.0f};
    const int take = std::min(maxPoints, s.rateCount);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(take));
    // 环满时 rateNext 是最老下标；取时间序最后 take 个点（末点 = 最近采样）
    const int first = (s.rateCount < State::kRateRingSize) ? 0 : s.rateNext;
    for (int k = s.rateCount - take; k < s.rateCount; ++k) {
        out.push_back(static_cast<float>(s.rateRing[(first + k) % State::kRateRingSize]));
    }
    return out;
}

void setTestClock(std::function<int64_t()> fn) {
    testClock() = std::move(fn);
}

void resetForTest() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.streams.clear();
    s.sessionTotal = 0;
    s.lastEventMs = -1;
    s.ringCount = 0;
    s.ringNext = 0;
    s.rateCount = 0;
    s.rateNext = 0;
    testClock() = nullptr;
}

} // namespace core::proxy_stats
