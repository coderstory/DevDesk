// src/module_settings/md_assoc_logic.h
// 「关联 .md 文件」纯逻辑（F-10 开关行状态字四态 + F-11 启动自愈决策）。
// 判定规则唯一来源 = 方案卡 §二.F-07（写入成功≠行为生效，一切以查询回读为准；
// Unknown=查询失败，按需修复档，不与 Others 混判）。本头无 UI / 无 DB / 无系统
// 触达依赖，跨平台直测（tests/test_modules/test_md_assoc_logic.cpp）。
#pragma once

#include "platform/file_association.h"

namespace module::settings {

// 开关行行尾状态字四态（PRD §八 状态集）：
enum class MdAssocStatusKind {
    None,               // 关 + 从未开过 → 不渲染状态字
    Linked,             // 开 + 回读归本应用 → 「已关联」
    NotEffective,       // 开 + 回读非本应用（Others/Unknown 同档呈现）→「未生效」+ 平台引导
    MaintenanceStopped, // 关 + 曾开过 → 「已停止自动维护」（D-4：不注销，仅停自愈）
};

// 状态字四态推导（纯函数）：enabled × everEnabled × 回读归属 三输入矩阵。
MdAssocStatusKind mdAssocStatusKind(bool enabled, bool everEnabled,
                                    platform::file_association::AssocState state);

// 启动自愈决策（纯函数，F-11 决策矩阵）：关 × 任意 = Skip（D3：整个检查跳过）；
// 开 × Ours = Skip（一致不动，D5）；开 × Others/Unknown = Repair（修复一次，D6；
// 写入幂等由 file_association 保证，重复执行无累积）。
enum class MdAssocHealAction { Skip, Repair };
MdAssocHealAction mdAssocHealAction(bool enabled,
                                    platform::file_association::AssocState state);

// 状态字文案（None → 空串，调用方按空判断不建元素）。
const char* mdAssocStatusLabel(MdAssocStatusKind kind);

// 卡高（100% 档基准，运行期 roundFs 包裹）：标题 20 + 行距 6 + 行盒 40 + 上下
// 留白 8×2 = 82（设计文档 §3.5：行盒 40 = label 18 + 间距 4 + 说明 16 + 余 2）。
inline constexpr float kMdAssocCardHeight = 82.0f;
// 未生效引导行：距行盒 2。文字档（macOS 文字指引）行盒 16 → 82+2+16 = 100；
// 按钮档（Windows 一键按钮，高 28 字面值不随字号缩放、主导行高）→ 82+2+28 = 112。
inline constexpr float kMdAssocCardHeightTextGuide = 100.0f;
inline constexpr float kMdAssocCardHeightBtnGuide = 112.0f;
inline float mdAssocCardHeight(bool guide, bool oneClickButton) {
    if (!guide) return kMdAssocCardHeight;
    return oneClickButton ? kMdAssocCardHeightBtnGuide
                          : kMdAssocCardHeightTextGuide;
}

} // namespace module::settings
