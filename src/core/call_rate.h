// src/core/call_rate.h
// 「单次调用速率」的唯一口径（2026-09-29 真机反馈 ①②）
//
// 背景：这条口径被两处消费 —— 首页「Token 速率」卡（最近一次成功调用的速率，
// module_home::HomeService）与 Claude 详情页「最近调用」明细列的逐行速率
//（module_claude_proxy::claude_ui_detail）。两处读的是同一张 claude_call_logs
// 表的同几列，却分属两个 module_*，而 module_* 之间不可互相依赖（架构红线），
// 故口径收进 core 单源：core 只能提供纯函数（先例 core/overdue.h，THS-04）。
// 任一处改了「成功」的判定或除法口径，首页卡与明细列就会给出互相矛盾的数字，
// 且无从判断谁对。
//
// 口径（用户原话：「最近一次调用输出 token 数量 / 接口耗时」）：
//   速率 = output_tokens / (latency_ms / 1000)      单位 tok/s
//   仅「成功调用」有速率：status_code 在 200..299 且 output_tokens > 0；
//   latency_ms <= 0（未采集 / 异常行）同样无速率。
//   无速率 → UI 显示空态 / 「—」，**不显示 0 或负数**——0 是合法的速率值，
//   「0 tok/s」与「没有速率」必须可区分（同 migration v5 里 NULL ≠ 0 的口径）。
//
// 为什么分母用 latency_ms 而不是 latency_ms − first_event_ms
//（首字之后才算生成，理论上更纯）：
//   ① 用户口径原话是「接口耗时」，未要求扣掉首字等待；
//   ② first_event_ms 只在**流式转发流末回填**时写入
//      （proxy_router.cpp writeCallLogUsage），非流式行 / 老行 / 上游没给
//      usage 的行一律 NULL —— 拿它做分母会让这批行整批「无速率」（明细列
//      一片「—」），而 latency_ms 每行都有（logCall 必写）；
//   ③ 一旦两处改成一处用 latency_ms、一处用减法，①② 的判定条件就不再同源。
//   若日后要改扣首字的算法，改本文件一处即可（这正是单源的目的）——注意
//   改口径会让旧落库行与新行口径不一致，需同时想清楚怎么标注。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace core::call_rate {

// 「成功调用」判定（口径见文件头）
inline bool isSuccessful(int statusCode, int64_t outputTokens) {
    return statusCode >= 200 && statusCode <= 299 && outputTokens > 0;
}

// 速率（tok/s）：成功且有耗时才成立；否则返回 false（out 不写）
inline bool tps(int statusCode, int64_t outputTokens, int64_t latencyMs,
                double& out) {
    if (!isSuccessful(statusCode, outputTokens)) return false;
    if (latencyMs <= 0) return false;
    out = static_cast<double>(outputTokens) * 1000.0 /
          static_cast<double>(latencyMs);
    return true;
}

// 速率文案：恒 1 位小数（44.96 → "45.0"，0.04 → "0.0"）。
// 与实时卡的 module::home::formatRateTps 不同（后者 >=100 取整）——本口径是
// 单次调用的值，规格要求保留 1 位小数，不随量级切换格式。
inline std::string formatTps(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
}

// 「无速率」的占位文案（明细列用；首页卡走空态，另有自己的文案）
inline constexpr const char* kNoRate = "—";

} // namespace core::call_rate
