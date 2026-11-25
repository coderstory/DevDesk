// src/module_memory/panel/settings_store.h
// 记忆面板设置持久化（JSON，落 memoryDataDir()/panel_settings.json——随
// DEVDESK_DATA_ROOT 隔离，测试/演示用独立根）。红线：
//   - 本文件绝不携带任何密钥（端点卡的凭据通道是 settings.json → 引擎环境变量，
//     见 telemem_config.h）；落盘内容只有开关/阈值/回显元数据。
//   - 消费方（hook/采集管线）按需重读文件；UI 侧经 PanelState 缓存。
#pragma once

#include <cstdint>
#include <string>

#include "core/result.h"
#include "module_memory/proposals/usage_alert.h"

namespace module::memory::panel {

// bug490 记忆 token 预算默认（三档唯一定义源：成员默认 / 缺键回退 / 存量迁移目标）。
// 2026-11-24 用户澄清「日 1000 万 / 周 5000 万 / 月 1 亿」指记忆模块；产品无周档，
// 周 5000 万并入月软档。
inline constexpr long long kUsageDayAlertTokensDefault = 10'000'000;
inline constexpr long long kUsageSoftBudgetTokensDefault = 50'000'000;
inline constexpr long long kUsageHardBudgetTokensDefault = 100'000'000;

struct MemoryPanelSettings {
    bool sanitizeEnabled = true;  // 脱敏闸（默认开；关闭须二次确认，定案 2）
    int recallThresholdPct = 85;  // 升级阈值（Recall@5 低于此值提醒升级；红线 85）
    int twoWordBaseline = 5;      // 两字词提炼基线 · 命中次数（定案 7 基线 5）
    std::int64_t llmImportedAtSec = 0;  // 0 = 未从网关 profile 导入
    std::string llmProfileName;         // 导入时的激活 profile 名（纯回显，无凭据）
    bool mcpSearchEnabled = true;  // memory_search MCP 工具注册开关（旧文件缺键 = true）
    // 需-9 / need:380 B-5：用量告警三档阈值收敛为「单开关 + 内部值」——UI 只留
    // 开/关（usageAlertsEnabled），阈值不再有滑条入口；存量自定义值原键保留、
    // 开关开时继续生效（不丢用户配置）。bug490（2026-11-24）上调默认：日 1000 万
    // · 月软 5000 万 · 月硬 1 亿；月档与消耗卡/提炼暂停同一套数。旧默认
    // 100 万/300 万/500 万只作存量迁移识别键（等于旧默认 = 未自定义）。
    bool usageAlertsEnabled = true;              // UI 单开关；false = 只静默不告警
    long long usageDayAlertTokens = kUsageDayAlertTokensDefault;      // 日档；≤0 = 关闭
    long long usageSoftBudgetTokens = kUsageSoftBudgetTokensDefault;  // 月档·软档；≤0 = 关闭
    long long usageHardBudgetTokens = kUsageHardBudgetTokensDefault;  // 月档·硬档；≤0 = 关闭
    // G-1 提炼节流（spec 控耗 2「每小时云端调用上限可配置，默认保守值」）：
    // 默认 4 次/小时（OQ-2 保守取值；独立于预算档，不随 bug490 阈值上调放大）
    int inferHourlyLimit = 4;                    // ≤0 = 关闭节流
    // bug476：安装镜像源（pip 首跑安装用；空串 = 不传该旗标、回退环境变量）
    std::string pipIndexUrl;        // --index-url
    std::string pipExtraIndexUrl;   // --extra-index-url
    proposals::UsageAlertGate usageDayGate;
    proposals::UsageAlertGate usageSoftGate;
    proposals::UsageAlertGate usageHardGate;
};

MemoryPanelSettings loadPanelSettings();
// 写失败（磁盘满/权限）→ fail：memory_search 开关靠它回弹，不吞错
core::Result<void> savePanelSettings(const MemoryPanelSettings& settings);

}  // namespace module::memory::panel
