// src/module_claude_proxy/settings_takeover.h
// settings.json 接管/还原的协议侧（R10-A4 自 ClaudeService 抽出；职责边界见
// R10 spec §2.2 CPX-01）：
//   - 键级还原；接管失败不阻断上层操作（错误并入 warnings_，门面经
//     lastSyncWarnings() 透传 UI）
//   - 仅 UI 线程（现状同款）
// 子对象不反向依赖彼此：接管入参（激活 Profile + 当前代理端口 + 所绑片段
// env）由门面从 ProfileRepository / ProxyRuntime 取齐后一并传入
//（R10 spec §2.3 P-A4 双入参，子对象不反向依赖那两个子对象）。
// 反向同步 syncCaptureActiveProfile（168 行跨 DB+Keychain+文件三写）按 Q2
// 否决 4 留门面，本类不承载其任何一段（其唯一 settings_sync:: 读侧
// readSettingsEnvForSync 命中保留在门面，见 R10 spec §2.4 P-A4 断言）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"
#include "settings_sync.h"

namespace module::claude {

struct Profile;

// settings.json 接管/还原的协议侧（R10-A4）：读文件 → 算期望 → 写文件/sidecar
// 的逻辑与状态（路径注入 / 接管开关 / 托管快照检测 / 上层覆盖警告）都在本类；
// 门面 ClaudeService 保留 43 签名转发与策略编排（起代理后接管 / 停代理后还
// 原 / re-takeover 时机），调用顺序逐字保持
class SettingsTakeOver {
public:
    SettingsTakeOver() = default;

    SettingsTakeOver(const SettingsTakeOver&) = delete;
    SettingsTakeOver& operator=(const SettingsTakeOver&) = delete;

    // 注入接管通道路径并启用（生产由 main.cpp 一次性注入
    // settings_sync::defaultPaths()）。镜像门面 setSettingsSyncPaths 语义：
    // 注入即清空历史警告
    void setPaths(const settings_sync::Paths& paths);
    const settings_sync::Paths& paths() const { return paths_; }

    // 接管开关（2026-09-27 恢复为默认 true，用户拍板）。关闭 = 完全不碰
    // settings.json（起代理接管 / 停代理还原 / 启动补还原三条路径一起门控）
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // 已接管（sidecar 快照存在；设置页「接管状态」行数据源，Phase D UI 用）
    bool isClaudeCodeManaged() const;

    // 最近一次接管的上层覆盖检测警告（Phase D UI 展示；无警告为空）
    const std::vector<std::string>& warnings() const { return warnings_; }

    // 启动崩溃补还原：sidecar 残留（上次没走完还原）→ 先还原。
    // main.cpp 在任何 startProxy 之前调用（调用时序见 settings_sync.h）
    void recoverPendingSettingsRestore();

    // 接管（R10 spec §2.3 P-A4 双入参：激活 Profile + 当前代理端口，由门面
    // 从 ProfileRepository / ProxyRuntime 取。advanced-json-editor §5.2①/§8.4：
    // 生效高级设置 JSON 由门面预先装配进 Profile 副本的 extraSettingsJson
    //（片段叠加片段赢在门面完成），原 sharedExtraEnvJson 形参拆除——片段不再
    // 携带 env 键）。返回失败原因（settings.json / sidecar 读写或解析失败），
    // 供调用方决定呈现方式；**接管失败不阻断上层操作**（启代理/切 profile
    // 照常），失败原因并入 warnings_（门面经 lastSyncWarnings() 透传 UI）
    core::Result<void> syncTakeOver(const Profile& activeProfile, int proxyPort);

    // 还原（CPX-02，R10-A4 收尾）：不再 void——失败返回 Result 上报（唯一
    // 调用点门面 stopProxy 内），失败原因并入 warnings_
    core::Result<void> syncRestore();

private:
    settings_sync::Paths paths_;
    bool enabled_ = true;  // 接管默认开（2026-09-27 恢复，见 setEnabled）
    std::vector<std::string> warnings_;
};

} // namespace module::claude