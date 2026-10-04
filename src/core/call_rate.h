// src/core/call_rate.h
// 「单次调用速率」的唯一口径：首页 Token 速率卡与 Claude 详情页明细列读同一张 claude_call_logs
// 的同几列却分属两个 module_*，口径收进 core 单源（纯函数）。速率 = output_tokens / (latency_ms/1000)
// 单位 tok/s；仅成功调用有速率（status_code 200..299 且 output_tokens > 0，latency_ms > 0），
// 否则无速率 → UI 空态/「—」，不显示 0（0 是合法速率值，与没有速率必须可区分）。
// 分母用 latency_ms 不用扣首字：first_event_ms 只在流式末回填，非流式/老行是 NULL。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace core::call_rate {

// 「成功调用」判定
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

// 速率文案：恒 1 位小数（44.96→"45.0"）。与实时卡 formatRateTps 不同（后者 >=100 取整）——
// 本口径是单次调用值，规格要求保留 1 位小数
inline std::string formatTps(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
}

// 「无速率」占位文案（明细列用；首页卡走空态另有一套）
inline constexpr const char* kNoRate = "—";

} // namespace core::call_rate
