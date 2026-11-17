// src/module_settings/notepad_assoc_card.cpp
// 「记事本文件关联」卡实现（F-18，need263 批 4 棒 B；视觉参数全部承接退役
// md 关联卡 = docs/design/2026-11-11-设置模块两卡重设计.md §2.3 三带格律）：
// 标题带 y16（19/600 正式标题）、说明带 y40、状态带 y64（点 6×6 + 字 12/500
// 左轨链）；引导带（未生效态）= 图标 0xF071 + 11px warningText（文字 y88 /
// Win 一键按钮独立一行 y108 左锚）。字色加深档走 theme token successText/
// warningText。卡高同源契约见 notepad_assoc_logic.h 的 kNotepadAssocCardHeight*。
// 开关钉标题行右缘 x = w−72（模块 tab 开关右锚单一口径）；卡壳 = cardSurface +
// radiusLarge + Sm 阴影 + 1px borderDefault（提醒卡同款）。引导行分平台经
// platform::open_settings::oneClickAvailable() 分流（Windows=一键按钮 /
// macOS=文字指引），业务代码零平台宏。
#include "notepad_assoc_card.h"

#include "notepad_assoc_logic.h"
#include "settings_ui_logic.h"

#include <core/component_theme.h>  // componentTokensWeChat（卡壳阴影/按钮主题）
#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth
#include <platform/open_settings.h>

#include <components/button.h>

#include <ui_kit/card.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace module::settings {

namespace {

// 卡内几何常量（100% 档基准；行盒/字号走 roundFs/fs 缩放，开关/按钮为设计稿
// 字面值不缩放——与提醒卡口径一致）
inline constexpr float kCardPadX = 28.0f;      // 内容左轨（cardTitle/cardDesc 同轨）
inline constexpr float kTitleY = 16.0f;        // 标题带 y（盒 22）
inline constexpr float kTitleH = 22.0f;
inline constexpr float kHintY = 40.0f;         // 说明带 y（盒 16）
inline constexpr float kHintH = 16.0f;
inline constexpr float kStatusY = 64.0f;       // 状态带 y（盒 20）
inline constexpr float kStatusH = 20.0f;
inline constexpr float kStatusDot = 6.0f;      // 状态点 6×6（与状态字同色同档）
inline constexpr float kSwitchW = 44.0f;       // 开关字面值（不随字号缩放）
inline constexpr float kSwitchH = 24.0f;
inline constexpr float kGuideTextY = 88.0f;    // 引导文案带 y（仅未生效态）
inline constexpr float kGuideLineH = kNotepadAssocGuideLineH; // 行高单一来源（logic.h）
inline constexpr float kGuideIconW = 12.0f;    // 警告图标占宽（先例 settings_ui_cards.cpp:714）
inline constexpr float kGuideIconGapX = 4.0f;
inline constexpr float kGuideBtnY = 108.0f;    // Win 一键按钮行 y（左锚）
inline constexpr float kBtnH = 28.0f;          // Win 一键按钮高（字面值，主导引导行盒）
inline constexpr float kBtnPadX = 12.0f;       // 按钮宽 = 文字实宽 + 左右内边距

// 引导文案可用宽推导（drawNotepadAssocCard 与行数实测共用，防两处漂移）。
float notepadAssocGuideAvailW(float cardW) {
    const float textX = kCardPadX + roundFs(kGuideIconGapX) + kGuideIconW;
    return std::max(0.0f, cardW - textX - kCardPadX);
}

} // namespace

int notepadAssocMacGuideLineCount(float cardW, const std::string& guideText) {
    const float availW = notepadAssocGuideAvailW(cardW);
    if (availW <= 0.0f) return 2;
    const float textW =
        core::TextPrimitive::measureTextWidth(guideText, {}, fs(11.0f), 400);
    // 2px 安全垫：整串实测 ÷ 可用宽的 ceil 近似 greedy 断行（断点空隙损耗）
    const int lines = static_cast<int>(
        std::ceil(textW / std::max(1.0f, availW - 2.0f)));
    return lines < 2 ? 2 : lines;
}

void drawNotepadAssocCard(const CardDrawArgs& args) {
    drawNotepadAssocCard(args, /*forceTextGuide=*/false);
}

void drawNotepadAssocCard(const CardDrawArgs& args, bool forceTextGuide) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    const bool oneClick =
        forceTextGuide ? false : platform::open_settings::oneClickAvailable();
    // 状态字四态（判定契约 = notepadAssocStatusKind；开关显本地意图、真值校正走状态字）
    const auto kind = notepadAssocStatusKind(in.cfg.notepadAssocEnabled,
                                             in.cfg.notepadAssocEverEnabled,
                                             in.notepadAssocState);
    const bool guide = kind == NotepadAssocStatusKind::NotEffective;
    // 未生效摘要（引导带数据源；compose 侧已按逐扩展回读组装，卡片只消费）
    const std::string summary = in.notepadGuideText;
    const std::string macGuideText = notepadAssocMacGuideText(summary);
    // Win 一键按钮档文案 = 摘要本身（按钮承接动作）；摘要空（无逐扩展快照）兜底
    // 旧文案语义
    const std::string winGuideText =
        summary.empty() ? "默认程序未指向 DevDesk，点击下方按钮修复。" : summary;

    ui.stack(id)
        .size(w, h)
        .content([&] {
        // 卡壳：白卡 + Sm 阴影 + 描边（提醒卡同款）
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(w, h)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f,
                                              0.02f))
            .border(1.0f, t.colors.borderDefault)
            .build();

        // 左轨恒 28 字面（与 cardTitle/cardDesc 同口径）：不可包 roundFs——字号档
        // 放大时本卡会与同配对行的提醒卡标题/开关错轨（承接 md 卡 review P1 契约）。
        const float padX = kCardPadX;
        const float innerW = std::max(0.0f, w - 2.0f * padX);

        // 标题带（19/600，cardTitle 同款——helper 在 settings_ui_cards.cpp
        // 匿名命名空间不可跨 TU，内联同参）
        ui.text(id + ".title")
            .position(padX, roundFs(kTitleY))
            .size(innerW, roundFs(kTitleH))
            .text("记事本文件关联")
            .fontSize(t.sizes.fontSizeLarge)
            .lineHeight(roundFs(kTitleH))
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        // 开关：钉标题行右缘（x = w−72 全 tab 单一口径），y15 = 标题行 16+(22−24)/2
        // 垂直居中（放大档行盒随 roundFs，开关字面值补位）
        const float swX = w - padX - kSwitchW;
        const float swY = roundFs(kTitleY) +
                          std::max(0.0f, (roundFs(kTitleH) - kSwitchH) * 0.5f);
        ui_kit::wswitch(ui, id + ".sw", in.cfg.notepadAssocEnabled, swX, swY,
                        kSwitchW, kSwitchH, t,
                        [f = a.applyNotepadAssocEnabled](bool v) { f(v); });

        // 说明带（id .hint 不换；单行省略三件套）
        ui.text(id + ".hint")
            .position(padX, roundFs(kHintY))
            .size(innerW, roundFs(kHintH))
            .text("双击记事本支持的文件时用 DevDesk 打开")
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(kHintH))
            .color(t.colors.textTertiary)
            .maxWidth(innerW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .build();

        // 状态带（None = 空串 → 整带不渲染，「关+从未开过=无状态字」）。
        // 点 6×6 与状态字同色同档（加深档走 token，浅底 ≥4.5:1）；字 x = 28+6+6 = 40。
        const char* statusText = notepadAssocStatusLabel(kind);
        if (statusText[0] != '\0') {
            const auto statusColor =
                kind == NotepadAssocStatusKind::Linked
                    ? t.colors.successText
                    : (kind == NotepadAssocStatusKind::NotEffective
                           ? t.colors.warningText
                           : t.colors.textTertiary);
            const float dotW = roundFs(kStatusDot);
            ui.rect(id + ".status.dot")
                .position(padX, roundFs(kStatusY) +
                                    (roundFs(kStatusH) - dotW) * 0.5f)
                .size(dotW, dotW)
                .radius(dotW)
                .color(statusColor)
                .build();
            const float statusW = core::TextPrimitive::measureTextWidth(
                statusText, {}, fs(12.0f), 500);
            ui.text(id + ".status")
                .position(padX + dotW + roundFs(6.0f), roundFs(kStatusY))
                .size(statusW, roundFs(kStatusH))
                .text(statusText)
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(kStatusH))
                .fontWeight(500)
                .color(statusColor)
                .build();
        }

        // 引导带（仅未生效态；卡高同源契约 notepadAssocCardHeight）：文案带 y88 =
        // Win 摘要单行（省略号兜底）/ mac 摘要+平台指引（盒高随实测行数派生），
        // Win 一键按钮独立一行 y108 左锚（launchAdvancedAssociationUI 直达）。
        if (guide) {
            const float gY = roundFs(kGuideTextY);
            const float iconW = kGuideIconW;
            ui.text(id + ".guide.icon")
                .position(padX, gY + (roundFs(kGuideLineH) - iconW) * 0.5f)
                .size(iconW, iconW)
                .icon(0xF071) // fa-triangle-exclamation
                .fontSize(fs(11.0f))
                .color(t.colors.warningText)
                .build();
            const float textX = padX + roundFs(kGuideIconGapX) + iconW;
            const float textW2 = notepadAssocGuideAvailW(w);
            if (oneClick) {
                // Windows：摘要单行（省略号兜底）+ 独立一行一键按钮（宽 = 文字
                // 实宽 + 左右 12 内边距，左轨锚）
                ui.text(id + ".guide.text")
                    .position(textX, gY)
                    .size(textW2, roundFs(kHintH))
                    .text(winGuideText)
                    .fontSize(fs(11.0f))
                    .lineHeight(roundFs(kGuideLineH))
                    .color(t.colors.warningText)
                    .maxWidth(textW2)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                const char* btnLabel = "打开系统默认应用设置";
                const float btnW =
                    core::TextPrimitive::measureTextWidth(btnLabel, {},
                                                          fs(13.0f), 400) +
                    2.0f * kBtnPadX;
                components::button(ui, id + ".guide.btn")
                    .position(padX, roundFs(kGuideBtnY))
                    .size(btnW, kBtnH)
                    .text(btnLabel)
                    .fontSize(fs(13.0f))
                    .theme(compTheme, /*primary=*/true)
                    .radius(t.sizes.radiusSmall)
                    .onClick([] {
                        // 直达系统默认应用设置页（F-18 api_map 路径锚唯一权威）
                        (void)platform::file_association::
                            launchAdvancedAssociationUI();
                    })
                    .build();
            } else {
                // macOS：无程序化直达（PRD D-5），文字指引盒高随实测行数派生
                //（承接 md 卡 review P2-1：600 最小窗/130% 字号档指引超两行，
                // 固定两行盒 + clip 会静默裁切第三行；行数与卡高契约同源函数）。
                const float macLines =
                    static_cast<float>(
                        notepadAssocMacGuideLineCount(w, macGuideText));
                ui.text(id + ".guide.text")
                    .position(textX, gY)
                    .size(textW2, roundFs(kGuideLineH) * macLines)
                    .text(macGuideText)
                    .fontSize(fs(11.0f))
                    .lineHeight(roundFs(kGuideLineH))
                    .wrap(true)
                    .color(t.colors.warningText)
                    .maxWidth(textW2)
                    .clip()
                    .build();
            }
        }
        })
        .build();
}

} // namespace module::settings
