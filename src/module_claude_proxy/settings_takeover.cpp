// src/module_claude_proxy/settings_takeover.cpp
// settings.json 接管/还原的协议侧实现（协议见 settings_takeover.h）
#include "settings_takeover.h"

#include <string>
#include <utility>

#include "core/logging.h"
#include "profile_repository.h"  // Profile 完整类型（takeOver 消费其字段）

namespace module::claude {

void SettingsTakeOver::setPaths(const settings_sync::Paths& paths) {
    paths_ = paths;
    std::lock_guard<std::mutex> lock(mu_);
    warnings_.clear();
}

std::vector<std::string> SettingsTakeOver::warningsSnapshot() const {
    // 值返回 + 锁内拷贝。返回活引用会让 UI 每帧遍历的缓冲被写者（agent
    // 线程的 setActiveProfile → syncTakeOver）整体替换释放 → 竞争 / UAF
    std::lock_guard<std::mutex> lock(mu_);
    return warnings_;
}

bool SettingsTakeOver::isClaudeCodeManaged() const {
    return !paths_.settingsFile.empty() &&
           settings_sync::hasPendingSnapshot(paths_);
}

void SettingsTakeOver::recoverPendingSettingsRestore() {
    if (paths_.settingsFile.empty()) return;
    // 接管开着才补还原：启动序列里这一步负责把上次异常退出残留的 sidecar
    // 收尾，必须先于任何 startProxy（否则残留的托管键会被误快照为用户原值）。
    // 开关关掉时连它也不做——「不碰 settings.json」是关掉开关的整体语义。
    if (!enabled_) return;
    auto r = settings_sync::recoverPendingSnapshot(paths_);
    if (!r.ok) {
        LOG_WARN("Claude Code 接管崩溃补还原失败: " + r.error);
    }
}

core::Result<void> SettingsTakeOver::syncTakeOver(
    const Profile& activeProfile, int proxyPort) {
    if (paths_.settingsFile.empty()) return core::Result<void>::ok();
    // 接管默认开：起代理即接管 settings.json，停代理还原。显式关掉开关才走
    // 下面这条——此时 DevDesk 完全不碰该文件。
    if (!enabled_) {
        LOG_INFO("Claude Code settings 接管未开启：不动 " +
                 paths_.settingsFile.string());
        return core::Result<void>::ok();
    }
    // 写进 settings.json 的是**合并后**的 env + 生效高级设置 JSON（托管键在最
    // 上层；片段叠加已在门面装配进 extraSettingsJson——片段值不留痕迹，
    // 片段键经 JSON 域进 sidecar nonEnv 记账 → 停代理还原时随快照撤走）
    auto r = settings_sync::takeOver(activeProfile, proxyPort, paths_);
    size_t warnCount = 0;
    {
        // warnings_ 整体重写（vector 缓冲释放）——与读侧 warningsSnapshot()
        // 同一把 mu_，读者拿到的永远是某一次写完之后的自洽快照
        std::lock_guard<std::mutex> lock(mu_);
        warnings_ = std::move(r.warnings);
        if (!r.ok) {
            // 失败并入 warnings_ 供 UI 透传；接管失败不阻断起代理/切 profile
            warnings_.push_back("Claude Code settings 接管失败: " + r.error);
        }
        warnCount = warnings_.size();
    }
    if (!r.ok) {
        LOG_WARN("Claude Code settings 接管失败: " + r.error);
        return core::Result<void>::fail(core::ErrorCode::Internal, r.error);
    }
    LOG_INFO("Claude Code settings 已接管: " + paths_.settingsFile.string() +
             (warnCount == 0
                  ? std::string("")
                  : "（含 " + std::to_string(warnCount) + " 条上层覆盖警告）"));
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
    // 无 sidecar 也走 restore：其中的「残留代理形态键」兜底负责把 settings.json
    // 里仍指向死代理的键清掉；无该形态时 restore 内部是真 no-op（不写文件）
    auto r = settings_sync::restore(paths_);
    if (!r.ok) {
        // 还原失败并入 warnings_ 供 UI 透传并返回 Result 上报
        std::lock_guard<std::mutex> lock(mu_);   // 写侧持 mu_
        warnings_.push_back("Claude Code settings 还原失败: " + r.error);
        return core::Result<void>::fail(core::ErrorCode::Internal, r.error);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);   // 写侧持 mu_
        warnings_.clear();
    }
    LOG_INFO("Claude Code settings 已还原（settings.json 不再指向本地代理）");
    return core::Result<void>::ok();
}

} // namespace module::claude