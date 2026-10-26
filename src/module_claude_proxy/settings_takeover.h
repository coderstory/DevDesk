// src/module_claude_proxy/settings_takeover.h
// settings.json 接管/还原的协议侧：键级还原；接管失败不阻断上层操作（错误并入
// warnings_，门面经 lastSyncWarnings() 透传 UI）。
// 线程口径：warnings_ 由 UI 线程与 agent 工作线程跨线程读写，mu_ 护、读侧值返回；
// 接管入参（激活 Profile + 代理端口）由门面取齐后传入，本类不依赖其他子对象。
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"
#include "settings_sync.h"

namespace module::claude {

struct Profile;

// settings.json 接管/还原的协议侧：读文件 → 算期望 → 写文件/sidecar 的逻辑与
// 状态都在本类；策略编排（起代理后接管 / 停代理后还原 / re-takeover 时机）留门面。
class SettingsTakeOver {
public:
    SettingsTakeOver() = default;

    SettingsTakeOver(const SettingsTakeOver&) = delete;
    SettingsTakeOver& operator=(const SettingsTakeOver&) = delete;

    // 注入接管通道路径并启用（生产由 main.cpp 一次性注入 defaultPaths()）；
    // 注入即清空历史警告
    void setPaths(const settings_sync::Paths& paths);
    const settings_sync::Paths& paths() const { return paths_; }

    // 接管开关（默认开）。关闭 = 完全不碰 settings.json（起代理接管 /
    // 停代理还原 / 启动补还原三条路径一起门控）
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // 已接管（sidecar 快照存在；设置页「接管状态」行数据源）
    bool isClaudeCodeManaged() const;

    // 最近一次接管的上层覆盖检测警告（UI 展示；无警告为空）。
    // 返回副本而非活引用：写者含 agent 线程（整体重写 warnings_ 触发缓冲
    // 释放），活引用是数据竞争 / UAF；读侧锁内拷贝，写侧持同一把 mu_。
    std::vector<std::string> warningsSnapshot() const;

    // 启动崩溃补还原：sidecar 残留（上次没走完还原）→ 先还原。
    // main.cpp 在任何 startProxy 之前调用（调用时序见 settings_sync.h）
    void recoverPendingSettingsRestore();

    // 接管：把激活 Profile 的托管 env 写进 settings.json。
    // 返回失败原因（settings.json / sidecar 读写或解析失败），供调用方决定呈现
    // 方式；接管失败不阻断上层操作（启代理/切 profile 照常），失败原因并入
    // warnings_（门面经 lastSyncWarnings() 透传 UI）
    core::Result<void> syncTakeOver(const Profile& activeProfile, int proxyPort);

    // 还原：失败返回 Result 上报（唯一调用点门面 stopProxy 内），失败原因并入 warnings_
    core::Result<void> syncRestore();

private:
    settings_sync::Paths paths_;
    bool enabled_ = true;  // 接管默认开（见 setEnabled）
    // warnings_ 的跨线程互斥：写者 = UI 线程（startProxy/stopProxy 尾）或
    // agent 线程（setActiveProfile → syncTakeOver），读者 = UI 每帧 + agent。
    // 只护 warnings_ 这一个字段；paths_ 是启动期一次性注入的单写者状态
    mutable std::mutex mu_;
    std::vector<std::string> warnings_;   // mu_ 护
};

} // namespace module::claude