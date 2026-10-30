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

struct MemoryPanelSettings {
    bool sanitizeEnabled = true;  // 脱敏闸（默认开；关闭须二次确认，定案 2）
    int recallThresholdPct = 85;  // 升级阈值（Recall@5 低于此值提醒升级；红线 85）
    int twoWordBaseline = 5;      // 两字词提炼基线 · 命中次数（定案 7 基线 5）
    std::int64_t llmImportedAtSec = 0;  // 0 = 未从网关 profile 导入
    std::string llmProfileName;         // 导入时的激活 profile 名（纯回显，无凭据）
    bool mcpSearchEnabled = true;  // memory_search MCP 工具注册开关（旧文件缺键 = true）
    // 需-9 用量告警：阈值（token 估算口径；月档与消耗卡预算同一套数）+ 节流闸。
    // 闸随设置落盘 = 重启不重复轰炸；armed=false 表示越线未解除（跨日不重响）。
    long long usageDayAlertTokens = 500000;      // 日档阈值；≤0 = 关闭
    long long usageSoftBudgetTokens = 2000000;   // 月档·软档；≤0 = 关闭
    long long usageHardBudgetTokens = 3000000;   // 月档·硬档；≤0 = 关闭
    proposals::UsageAlertGate usageDayGate;
    proposals::UsageAlertGate usageSoftGate;
    proposals::UsageAlertGate usageHardGate;
};

MemoryPanelSettings loadPanelSettings();
// 写失败（磁盘满/权限）→ fail：memory_search 开关靠它回弹，不吞错
core::Result<void> savePanelSettings(const MemoryPanelSettings& settings);

}  // namespace module::memory::panel
