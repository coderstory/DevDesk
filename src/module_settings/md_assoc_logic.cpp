// src/module_settings/md_assoc_logic.cpp
// 「关联 .md 文件」纯逻辑实现（契约见 md_assoc_logic.h）。
#include "md_assoc_logic.h"

namespace module::settings {

MdAssocStatusKind mdAssocStatusKind(
    bool enabled, bool everEnabled,
    platform::file_association::AssocState state) {
    if (!enabled) {
        // 关：-ever=无状态字（从未开过）；+ever=已停止自动维护（D-4 不注销）
        return everEnabled ? MdAssocStatusKind::MaintenanceStopped
                           : MdAssocStatusKind::None;
    }
    // 开：回读归本应用=已关联；其余（Others/Unknown）一律「未生效」
    return state == platform::file_association::AssocState::Ours
               ? MdAssocStatusKind::Linked
               : MdAssocStatusKind::NotEffective;
}

MdAssocHealAction mdAssocHealAction(
    bool enabled, platform::file_association::AssocState state) {
    if (!enabled) return MdAssocHealAction::Skip; // D3：关 = 整个检查跳过
    return state == platform::file_association::AssocState::Ours
               ? MdAssocHealAction::Skip // D5：一致不动
               : MdAssocHealAction::Repair; // D6：不一致/查询失败 → 修复一次
}

const char* mdAssocStatusLabel(MdAssocStatusKind kind) {
    switch (kind) {
    case MdAssocStatusKind::Linked: return "已关联";
    case MdAssocStatusKind::NotEffective: return "未生效";
    case MdAssocStatusKind::MaintenanceStopped: return "已停止自动维护";
    case MdAssocStatusKind::None: break;
    }
    return "";
}

} // namespace module::settings
