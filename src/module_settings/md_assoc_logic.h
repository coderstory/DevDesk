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

// 卡高（100% 档基准，运行期 roundFs 包裹；need260 设计 §2.3 三带格律）：
// 标题带 y16 盒22 + 说明带 y40 盒16 + 状态带 y64 盒20（带底 84）+ 下边距 24
// = 108。标题「关联 .md 文件」19/600 正式标题带（原 13px label 退役），与
// 提醒卡/模块卡同格律；开关钉标题行右缘（x = w−72，全 tab 开关右锚单一口径）。
// 零滚动预算（need260 §6）：配对行高 = max(提醒 140, 本卡 108) = 140 →
// 模块 tab 310+8+140 = 402 ≤ 420 零滚动（本卡默认态不掌行高）。
inline constexpr float kMdAssocCardHeight = 108.0f;
// 未生效引导带（need260 §2.3）：引导文案 y88（Win 单行盒16 / mac 两行盒32），
// Windows 另有一键按钮 y108 盒28（字面值不随字号缩放、主导卡高）。
// 文字档（macOS 文字指引）行底 120 + 下边距 20 = 140；按钮档行底 136 + 16 = 152。
// 引导态仍零滚动：152 → 模块 tab 310+8+152 = 414 ≤ 420（旧设计引导档 424/436
// 溢出的病灶由本次半宽重排一并消除）。
inline constexpr float kMdAssocCardHeightTextGuide = 140.0f;
inline constexpr float kMdAssocCardHeightBtnGuide = 152.0f;
inline float mdAssocCardHeight(bool guide, bool oneClickButton) {
    if (!guide) return kMdAssocCardHeight;
    return oneClickButton ? kMdAssocCardHeightBtnGuide
                          : kMdAssocCardHeightTextGuide;
}

} // namespace module::settings
