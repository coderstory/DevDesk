// src/core/proxy_stats.h
// 跨模块共享的流式 token 统计（首页 Token 速率卡片，spec 2026-09-22-token-rate-card §2.5）
//
// writer：module_claude_proxy 流式转发钩子（网络线程）
// reader：module_home compose（UI 线程，每 0.5s 一次）
// 全部经 mutex，临界区是几十字节的拷贝。core 只依赖标准库，分层零违反。
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace core::proxy_stats {

// reader 一次拷贝拿走
struct Snapshot {
    int activeStreams = 0;    // 活动流数
    int64_t sessionTokens = 0; // 进程启动以来累计 output tokens（多流总和）
    double rateTps = 0.0;     // 最近 ~2s 窗口的 tok/s
    int64_t lastActivityMs = 0; // 距最近一次 token 事件的毫秒数（无事件为 -1）
};

// writer API（流式钩子调用）
uint64_t beginStream();                                // 返回流 id，activeStreams+1
void updateStream(uint64_t id, int64_t cumulativeTokens); // usage 事件（累计值口径）
void endStream(uint64_t id);                           // 幂等：任意结束路径可重复调

// reader API（Home compose）
Snapshot snapshot();

// UI 现代化 D-6（2026-09-28 plan）：最近速率采样序列（sparkline 数据源）。
// 纯数据扩展——既有字段/口径不动；writer 侧（updateStream 事件点）采样当前
// rateTps 入环，最多保留最近 64 点。
// 空序列返回 {0.0f}（防 lineChart 对空 values 填演示数据，linechart.h build()）。
std::vector<float> rateSeries(int maxPoints = 32);

// ---- 测试专用 ----
// 注入假时钟（返回 steady 毫秒）；传 nullptr 恢复真实时钟
void setTestClock(std::function<int64_t()> fn);
// 清空全部状态（含时钟），测试间隔离用
void resetForTest();

} // namespace core::proxy_stats
