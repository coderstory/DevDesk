// src/module_settings/notepad_assoc_logic.cpp
// 「记事本文件关联」纯逻辑实现（契约见 notepad_assoc_logic.h）。
#include "notepad_assoc_logic.h"

#include <algorithm>

namespace module::settings {

NotepadAssocStatusKind notepadAssocStatusKind(
    bool enabled, bool everEnabled,
    platform::file_association::AssocState state) {
    if (!enabled) {
        // 关：-ever=无状态字；+ever=已停止自动维护（F-18：win 侧关已撤销关联、
        // mac 无注销 API 停自愈，两侧语义的共同呈现）
        return everEnabled ? NotepadAssocStatusKind::MaintenanceStopped
                           : NotepadAssocStatusKind::None;
    }
    // 开：聚合回读归本应用=已关联；其余（Others/Unknown）一律「未生效」
    return state == platform::file_association::AssocState::Ours
               ? NotepadAssocStatusKind::Linked
               : NotepadAssocStatusKind::NotEffective;
}

NotepadAssocHealAction notepadAssocHealAction(
    bool enabled, platform::file_association::AssocState state) {
    if (!enabled) return NotepadAssocHealAction::Skip; // 关 = 整个检查跳过
    return state == platform::file_association::AssocState::Ours
               ? NotepadAssocHealAction::Skip // 一致不动
               : NotepadAssocHealAction::Repair; // 不一致/查询失败 → 修复一次
}

const char* notepadAssocStatusLabel(NotepadAssocStatusKind kind) {
    switch (kind) {
    case NotepadAssocStatusKind::Linked: return "已关联";
    case NotepadAssocStatusKind::NotEffective: return "未生效";
    case NotepadAssocStatusKind::MaintenanceStopped: return "已停止自动维护";
    case NotepadAssocStatusKind::None: break;
    }
    return "";
}

std::string notepadAssocFailingSummary(
    const std::vector<std::string>& extensions,
    const std::vector<platform::file_association::AssocState>& states) {
    const size_t n = std::min(extensions.size(), states.size());
    std::vector<std::string> failing;
    for (size_t i = 0; i < n; ++i) {
        if (states[i] != platform::file_association::AssocState::Ours) {
            failing.push_back(extensions[i]);
        }
    }
    if (failing.empty()) return {};
    std::string out = std::to_string(failing.size()) + " 项未生效：";
    const size_t shown = std::min(failing.size(), size_t{4});
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0) out += " ";
        out += failing[i];
    }
    if (failing.size() > shown) out += " …";
    return out;
}

std::string notepadAssocMacGuideText(const std::string& failingSummary) {
    // 文案承接退役 md 卡指引（need260 §2.3），主语从 .md 泛化为任一文件
    constexpr const char* kGuide =
        "macOS 无一键入口：Finder 右键任一文件 → 显示简介 → "
        "打开方式 → 选 DevDesk → 全部更改";
    if (failingSummary.empty()) return kGuide;
    return failingSummary + "。" + kGuide;
}

} // namespace module::settings
