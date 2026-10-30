// src/module_memory/proposals/usage_alert.h
// 需-9 用量告警纯判定：日/月两档阈值比较 + 每档每日一次节流闸（边沿触发——
// 越线击发一次并卸武，回落/阈值上调越线解除后重新武装；未解除次日不重响）。
// 零 UI / 零平台依赖：判定输入与日键由调用方注入（测试可造假账本/假时钟），
// 文案生成同为纯函数；出口投递与闸状态持久化在 panel 侧（settings_store +
// PanelState 挂点），本文件不关心。
#pragma once

#include <string>
#include <vector>

namespace module::memory::proposals {

// 阈值档（token，估算口径；月档与消耗卡预算是同一套数，spec §3.1）。
// ≤0 = 该档关闭。
struct UsageAlertThresholds {
    long long dayTokens = 500000;       // 日档（默认 50 万/日）
    long long monthSoftTokens = 2000000;  // 月档·软档（警告级）
    long long monthHardTokens = 3000000;  // 月档·硬档（严重级）
};

enum class UsageAlertTier { Day, MonthSoft, MonthHard };

// 节流闸：armed = 上次判定未越线（越线即击发并卸武）；lastFireDay = 最近击发
// 日键（YYYYMMDD 本地时区）。同档同日至多一条由此两者共同保证；闸随设置持久
// 化，重启不重复轰炸。
struct UsageAlertGate {
    bool armed = true;
    long long lastFireDay = 0;
};

// 一次击发（档位 + 触发时用量/阈值，文案生成的全部输入）
struct UsageAlertFire {
    UsageAlertTier tier = UsageAlertTier::Day;
    long long usageTokens = 0;
    long long thresholdTokens = 0;
};

// 判定（纯函数，原地更新三闸；恰好等于阈值算越线）。月档取当前最高相位
//（硬档文案覆盖软档：跳升硬档不补软档一条）；日档与月档独立，同一次判定
// 可各击发一条（跨档同日可各响）。
std::vector<UsageAlertFire> evaluateUsageAlert(
    const UsageAlertThresholds& th, long long todayTokens,
    long long monthTokens, long long dayKey, UsageAlertGate& day,
    UsageAlertGate& soft, UsageAlertGate& hard);

// 通知文案：统一带「估算」口径（码点折算，非引擎真值，spec §3.1）
std::string usageAlertTitle(UsageAlertTier tier);
std::string usageAlertBody(const UsageAlertFire& fire);

// 档位稳定标识（通知 id 前缀用，platform::notify 去重键）
const char* usageAlertTierTag(UsageAlertTier tier);

// 日键（YYYYMMDD，本地时区）——装配/挂点用；判定本身只吃键，不吃时刻。
long long localDayKey(long long nowSec);

}  // namespace module::memory::proposals
