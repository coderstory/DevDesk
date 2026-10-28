// src/module_memory/panel/settings_store.h
// 记忆面板设置持久化（JSON，落 memoryDataDir()/panel_settings.json——随
// DEVDESK_DATA_ROOT 隔离，测试/演示用独立根）。红线：
//   - 本文件绝不携带任何密钥（端点卡的凭据通道是 settings.json → 引擎环境变量，
//     见 telemem_config.h）；落盘内容只有开关/阈值/回显元数据。
//   - 消费方（hook/采集管线）按需重读文件；UI 侧经 PanelState 缓存。
#pragma once

#include <cstdint>
#include <string>

namespace module::memory::panel {

struct MemoryPanelSettings {
    bool sanitizeEnabled = true;  // 脱敏闸（默认开；关闭须二次确认，定案 2）
    int recallThresholdPct = 85;  // 升级阈值（Recall@5 低于此值提醒升级；红线 85）
    int twoWordBaseline = 5;      // 两字词提炼基线 · 命中次数（定案 7 基线 5）
    std::int64_t llmImportedAtSec = 0;  // 0 = 未从网关 profile 导入
    std::string llmProfileName;         // 导入时的激活 profile 名（纯回显，无凭据）
};

MemoryPanelSettings loadPanelSettings();
void savePanelSettings(const MemoryPanelSettings& settings);

}  // namespace module::memory::panel
