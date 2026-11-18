// src/module_settings/notepad_assoc_logic.h
// 「记事本文件关联」纯逻辑（F-18 开关行状态字四态 + 启动自愈决策 + 未生效摘要，
// need263 批 4 棒 B；接管退役的 md_assoc_logic.h，矩阵契约不变、逐扩展化）。
// 判定规则唯一来源 = 方案卡 §十（写入成功≠行为生效，一切以查询回读为准；
// Unknown=查询失败，按需修复档，不与 Others 混判）。本头无 UI / 无 DB / 无系统
// 触达依赖，跨平台直测（tests/test_modules/test_notepad_assoc.cpp）。
#pragma once

#include <string>
#include <vector>

#include "platform/file_association.h"

namespace module::settings {

// 开关行行尾状态字四态（原 mdAssoc 四态矩阵沿用）：
enum class NotepadAssocStatusKind {
    None,               // 关 + 从未开过 → 不渲染状态字
    Linked,             // 开 + 聚合回读归本应用 → 「已关联」
    NotEffective,       // 开 + 聚合非本应用（Others/Unknown 同档呈现）→「未生效」+ 平台引导
    MaintenanceStopped, // 关 + 曾开过 → 「已停止自动维护」
};

// 状态字四态推导（纯函数）：enabled × everEnabled × 聚合回读归属 三输入矩阵。
NotepadAssocStatusKind notepadAssocStatusKind(bool enabled, bool everEnabled,
                                              platform::file_association::AssocState state);

// 启动自愈决策（纯函数，矩阵沿用）：关 × 任意 = Skip（整个检查跳过）；
// 开 × Ours = Skip（一致不动）；开 × Others/Unknown = Repair（修复一次；
// 写入幂等由 file_association 保证，重复执行无累积）。
enum class NotepadAssocHealAction { Skip, Repair };
NotepadAssocHealAction notepadAssocHealAction(bool enabled,
                                              platform::file_association::AssocState state);

// 状态字文案（None → 空串，调用方按空判断不建元素）。
const char* notepadAssocStatusLabel(NotepadAssocStatusKind kind);

// 未生效扩展名摘要行（纯函数，引导带数据源）：按 extensions×states 配对收集
// 非 Ours 项，格式「N 项未生效：.a .b .c …」；超 4 项截断补「…」（省略号由
// UI 层 maxWidth 兜底，此处只限项数防超长串）；全 Ours / 无数据 = 空串。
std::string notepadAssocFailingSummary(
    const std::vector<std::string>& extensions,
    const std::vector<platform::file_association::AssocState>& states);

// macOS 文字指引全文（纯函数，卡片绘制与 compose 卡高派生共用单一来源）：
// 未生效摘要 + 平台指引（无摘要时仅指引）。
std::string notepadAssocMacGuideText(const std::string& failingSummary);

// ── 卡高契约（数值与格律承接退役 md_assoc_logic.h，need260 设计 §2.3）──
// 卡高（100% 档基准，运行期 roundFs 包裹）：标题带 y16 盒22 + 说明带 y40 盒16
// + 状态带 y64 盒20（带底 84）+ 下边距 24 = 108。标题「记事本文件关联」19/600
// 正式标题带；开关钉标题行右缘（x = w−72，全 tab 开关右锚单一口径）。
// 零滚动预算（need260 §6）：配对行高 = max(提醒 148, 本卡 108) = 148 →
// 模块 tab 254+16+148 = 418 ≤ 420 零滚动（本卡默认态不掌行高）。
inline constexpr float kNotepadAssocCardHeight = 108.0f;
// review 260 P2-1：状态带缺失（None 态）时不留状态带占位——标题带 16+22、
// 说明带 40+16，内容底 56 + 下边距 24 = 80（旧 108 让卡底多出 28px 空洞）。
inline constexpr float kNotepadAssocCardHeightNoStatus = 80.0f;
// 窄窗（<640 内容宽）单列堆叠档：说明改 2 行（行高 18、盒 36），其下各带整体下移 20
// → 无状态 56+20+24 = 100 / 有状态 108+16 = 124 / 引导档 +16（review 260 P2-2）。
inline constexpr float kNotepadAssocNarrowHintExtra = 20.0f;
// 未生效引导带：引导文案 y88（Win 单行盒16 / mac 多行盒32+），Windows 另有
// 一键按钮 y108 盒28（字面值不随字号缩放、主导卡高）。
// 文字档（macOS 文字指引）行底 120 + 下边距 20 = 140；按钮档行底 136 + 16 = 152。
// 引导态仍零滚动：152 → 模块 tab 310+8+152 = 414 ≤ 420。
inline constexpr float kNotepadAssocCardHeightTextGuide = 140.0f;
inline constexpr float kNotepadAssocCardHeightBtnGuide = 152.0f;
// 引导文案行高（mac 文字档盒高 = 行数 × 本值；单一来源，notepad_assoc_card.cpp
// 引导行盒与卡高派生共用）。100% 档两行 = 设计 §2.3 基线。
inline constexpr float kNotepadAssocGuideLineH = 16.0f;
// 卡高（纯算术，无 UI 依赖）：文字档行数由调用方实测传入（notepad_assoc_card.h
// notepadAssocMacGuideLineCount，600 最小窗/130% 字号档指引超两行，盒高随行数
// 派生防静默裁切——承接 md 卡 review P2-1）；参考几何（100% 档两行）= 140 不变。
inline float notepadAssocCardHeight(bool guide, bool oneClickButton,
                                    int textGuideLines = 2,
                                    bool hasStatus = true, bool narrow = false) {
    const float narrowExtra = narrow ? kNotepadAssocNarrowHintExtra : 0.0f;
    if (!guide) {
        return (hasStatus ? kNotepadAssocCardHeight
                          : kNotepadAssocCardHeightNoStatus) +
               narrowExtra;
    }
    if (oneClickButton) return kNotepadAssocCardHeightBtnGuide + narrowExtra;
    const int lines = textGuideLines < 2 ? 2 : textGuideLines;
    return kNotepadAssocCardHeightTextGuide +
           static_cast<float>(lines - 2) * kNotepadAssocGuideLineH + narrowExtra;
}

} // namespace module::settings
