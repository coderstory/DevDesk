// 跨模块共享的流式 token 统计（首页 Token 速率卡片）
// writer = module_claude_proxy 流式转发钩子（网络线程）；reader = home compose（UI 线程）。
// 全部经 mutex 保护（临界区是几十字节拷贝）；core 只依赖标准库。
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace core::proxy_stats {

// reader 一次拷贝拿走
struct Snapshot {
    int activeStreams = 0;
    int64_t sessionTokens = 0; // 进程启动以来累计 output tokens（多流总和）
    double rateTps = 0.0;     // 最近 ~2s 窗口的 tok/s
    int64_t lastActivityMs = 0; // 距最近一次 token 事件的毫秒数（无事件为 -1）
};

// writer API（流式钩子调用）
uint64_t beginStream();                                // 返回流 id，activeStreams+1
void updateStream(uint64_t id, int64_t cumulativeTokens); // usage 累计值口径
void endStream(uint64_t id);                           // 幂等：任意结束路径可重复调

// reader API（Home compose）
Snapshot snapshot();

// 最近速率采样序列（sparkline 数据源），最多保留 64 点。
// 空序列返回 {0.0f}（防 lineChart 对空 values 填演示数据）
std::vector<float> rateSeries(int maxPoints = 32);

// 测试专用
// setTestClock：注入假时钟（steady 毫秒），nullptr 恢复真实时钟
// resetForTest：清空全部状态（含时钟），测试间隔离用
void setTestClock(std::function<int64_t()> fn);
void resetForTest();

} // namespace core::proxy_stats
