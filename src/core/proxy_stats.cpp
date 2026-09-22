// src/core/proxy_stats.cpp
// 共享流式统计实现：mutex + 每流累计 + 16 槽采样环（设计 §2.5 / §2.7）
#include "proxy_stats.h"

#include <chrono>
#include <map>
#include <mutex>

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

    // 最近 2s 窗口速率（设计 §2.7）：
    // 基线 = 2s 前最近一次采样的总累计；找不到（全部采样都在 2s 内）时
    // 用最老采样，窗口按实际经过时长算。静止 >2s 时基线追上当前值 → 自然 0。
    double rateTps(int64_t nowMs) const {
        if (ringCount == 0) return 0.0;
        bool found = false;
        int64_t baseTotal = 0;
        int64_t baseT = 0;
        forEach([&](const Sample& s) {
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

State& state() {
    static State s;
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
    testClock() = nullptr;
}

} // namespace core::proxy_stats
