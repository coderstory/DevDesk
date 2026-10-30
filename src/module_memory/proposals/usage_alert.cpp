// src/module_memory/proposals/usage_alert.cpp
#include "usage_alert.h"

#include <cstdio>
#include <ctime>

#include "core/time.h"

namespace module::memory::proposals {

namespace {

// 单档判定：低于阈值武装；越线且武装且当日未响 → 击发并卸武。
// threshold ≤0（关闭）不动闸。
bool evalGate(long long usage, long long threshold, long long dayKey,
              UsageAlertGate& g) {
    if (threshold <= 0) return false;
    if (usage < threshold) {
        g.armed = true;
        return false;
    }
    if (g.armed && g.lastFireDay != dayKey) {
        g.armed = false;
        g.lastFireDay = dayKey;
        return true;
    }
    return false;
}

const char* tierTag(UsageAlertTier tier) {
    switch (tier) {
        case UsageAlertTier::Day:
            return "day";
        case UsageAlertTier::MonthSoft:
            return "month_soft";
        case UsageAlertTier::MonthHard:
            return "month_hard";
    }
    return "day";
}

}  // namespace

std::vector<UsageAlertFire> evaluateUsageAlert(
    const UsageAlertThresholds& th, long long todayTokens,
    long long monthTokens, long long dayKey, UsageAlertGate& day,
    UsageAlertGate& soft, UsageAlertGate& hard) {
    std::vector<UsageAlertFire> fired;
    if (evalGate(todayTokens, th.dayTokens, dayKey, day)) {
        fired.push_back({UsageAlertTier::Day, todayTokens, th.dayTokens});
    }
    if (th.monthHardTokens > 0 && monthTokens >= th.monthHardTokens) {
        // 硬档相位：软档已被跨越且未解除（硬档文案覆盖，不补软档一条）
        soft.armed = false;
        if (evalGate(monthTokens, th.monthHardTokens, dayKey, hard)) {
            fired.push_back(
                {UsageAlertTier::MonthHard, monthTokens, th.monthHardTokens});
        }
    } else if (th.monthSoftTokens > 0 && monthTokens >= th.monthSoftTokens) {
        // 软档相位：硬档越线解除（阈值上调越过用量/回落）
        hard.armed = true;
        if (evalGate(monthTokens, th.monthSoftTokens, dayKey, soft)) {
            fired.push_back(
                {UsageAlertTier::MonthSoft, monthTokens, th.monthSoftTokens});
        }
    } else {
        soft.armed = true;
        hard.armed = true;
    }
    return fired;
}

std::string usageAlertTitle(UsageAlertTier tier) {
    switch (tier) {
        case UsageAlertTier::Day:
            return "用量告警 · 日档";
        case UsageAlertTier::MonthSoft:
            return "用量告警 · 月档 · 软档";
        case UsageAlertTier::MonthHard:
            return "用量告警 · 月档 · 硬档";
    }
    return "用量告警";
}

std::string usageAlertBody(const UsageAlertFire& fire) {
    char buf[320];
    switch (fire.tier) {
        case UsageAlertTier::Day:
            std::snprintf(buf, sizeof(buf),
                          "今日 token 估算 %lld 已达日阈值 %lld（估算口径：码点"
                          "折算）。若有批量提炼或长会话，考虑收一收。",
                          fire.usageTokens, fire.thresholdTokens);
            break;
        case UsageAlertTier::MonthSoft:
            std::snprintf(buf, sizeof(buf),
                          "本月 token 估算 %lld 已进软档（阈值 %lld）：用量进入"
                          "黄档区（估算口径：码点折算）。",
                          fire.usageTokens, fire.thresholdTokens);
            break;
        case UsageAlertTier::MonthHard:
            std::snprintf(buf, sizeof(buf),
                          "本月 token 估算 %lld 已达硬档（阈值 %lld）：提炼按预"
                          "算硬档暂停、检索不受影响（估算口径：码点折算）。",
                          fire.usageTokens, fire.thresholdTokens);
            break;
    }
    return buf;
}

long long localDayKey(long long nowSec) {
    std::tm tm{};
    core::time::localTime(static_cast<std::time_t>(nowSec), &tm);
    return (static_cast<long long>(tm.tm_year) + 1900) * 10000 +
           (static_cast<long long>(tm.tm_mon) + 1) * 100 +
           static_cast<long long>(tm.tm_mday);
}

const char* usageAlertTierTag(UsageAlertTier tier) { return tierTag(tier); }

}  // namespace module::memory::proposals
