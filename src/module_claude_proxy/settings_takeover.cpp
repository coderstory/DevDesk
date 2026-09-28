// src/module_claude_proxy/settings_takeover.cpp
// settings.json 接管/还原的协议侧实现（R10-A4 自 claude_service.cpp 整段迁出，
// 行为零变化；段级拆分见 R10 spec §2.3 P-A4，断言口径见 §2.4 P-A4）
#include "settings_takeover.h"

#include <string>
#include <utility>

#include "core/logging.h"
#include "profile_repository.h"  // Profile 完整类型（takeOver 消费其字段）

namespace module::claude {

void SettingsTakeOver::setPaths(const settings_sync::Paths& paths) {
    paths_ = paths;
    warnings_.clear();
}

bool SettingsTakeOver::isClaudeCodeManaged() const {
    return !paths_.settingsFile.empty() &&
           settings_sync::hasPendingSnapshot(paths_);
}

void SettingsTakeOver::recoverPendingSettingsRestore() {
    if (paths_.settingsFile.empty()) return;
    // 接管开着才补还原（2026-09-27 恢复默认开）：启动序列里这一步负责把上次
    // 异常退出残留的 sidecar 收尾，必须先于任何 startProxy（否则残留的托管键
    // 会被误快照为用户原值）。开关关掉时连它也不做——「不碰 settings.json」
    // 是关掉开关的整体语义，历史遗留 sidecar 留到下次开启接管时再处理。
    if (!enabled_) return;
    auto r = settings_sync::recoverPendingSnapshot(paths_);
    if (!r.ok) {
        LOG_WARN("Claude Code 接管崩溃补还原失败: " + r.error);
    }
}

core::Result<void> SettingsTakeOver::syncTakeOver(
    const Profile& activeProfile, int proxyPort,
    const std::string& sharedExtraEnvJson) {
    if (paths_.settingsFile.empty()) return core::Result<void>::ok();
    // 接管默认开（2026-09-27 恢复）：起代理即接管 settings.json，停代理还原。
    // 显式关掉开关才走下面这条——此时 DevDesk 完全不碰该文件（本机另装
    // cc-switch 等同类工具、不愿两边互相覆盖时的用法）。
    if (!enabled_) {
        LOG_INFO("Claude Code settings 接管未开启：不动 " +
                 paths_.settingsFile.string());
        return core::Result<void>::ok();
    }
    // 写进 settings.json 的是**合并后**的 env（片段 → profile 自身 → 托管键，
    // 设计 §2.5）：片段值不留痕迹（env 是扁平 map，无处放出处），片段键一并进
    // sidecar 托管键集 → 停代理还原时随快照撤走
    auto r = settings_sync::takeOver(activeProfile, proxyPort, paths_,
                                     sharedExtraEnvJson);
    warnings_ = std::move(r.warnings);
    if (!r.ok) {
        // 条目 3：失败不再只进日志——并入 warnings_（与 getter 一起
        // 供 UI 透传）。调用方语义不变：接管失败不阻断起代理/切 profile
        warnings_.push_back("Claude Code settings 接管失败: " + r.error);
        LOG_WARN("Claude Code settings 接管失败: " + r.error);
        return core::Result<void>::fail(core::ErrorCode::Internal, r.error);
    }
    LOG_INFO("Claude Code settings 已接管: " + paths_.settingsFile.string() +
             (warnings_.empty() ? std::string("")
                                : "（含 " + std::to_string(warnings_.size()) +
                                      " 条上层覆盖警告）"));
    return core::Result<void>::ok();
}

core::Result<void> SettingsTakeOver::syncRestore() {
    if (paths_.settingsFile.empty()) return core::Result<void>::ok();
    if (!enabled_) {
        // 接管关闭：不改写 settings.json。确有遗留接管态时只告警不代改
        //（该文件由 cc-switch 等工具负责，DevDesk 不抢写）
        if (settings_sync::hasPendingSnapshot(paths_)) {
            LOG_WARN("检测到遗留的 Claude Code 接管快照，但接管已关闭——不改写 " +
                     paths_.settingsFile.string());
        }
        return core::Result<void>::ok();
    }
    // 无 sidecar 也走 restore（条目 2②）：其中的「残留代理形态键」兜底负责把
    // settings.json 里仍指向死代理的键清掉；无该形态时 restore 内部是真 no-op
    //（不写文件）。启动补还原仍以 sidecar 为前提——不在每次启动就动别人的配置
    auto r = settings_sync::restore(paths_);
    if (!r.ok) {
        // CPX-02（R10-A4 收尾）：还原失败不再只进日志——并入 warnings_ 供 UI
        // 透传并返回 Result 上报（门面 stopProxy 经其 syncRestore() 包装
        // LOG_WARN 留痕）
        warnings_.push_back("Claude Code settings 还原失败: " + r.error);
        return core::Result<void>::fail(core::ErrorCode::Internal, r.error);
    }
    warnings_.clear();
    LOG_INFO("Claude Code settings 已还原（settings.json 不再指向本地代理）");
    return core::Result<void>::ok();
}

} // namespace module::claude