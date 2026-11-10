// src/module_settings/md_assoc_card.cpp
// 「关联 .md 文件」开关行卡实现（F-10）。
// 视觉参数全部按设计文档 docs/design/2026-11-10-md-readonly-viewer.md §3.5：
// 行盒 40（label 18 + 间距 4 + 说明 16 + 余 2）、开关 44×24 wswitch（字面值
// 不随字号缩放）、状态字 12px 三色（success/warning/textTertiary）钉开关左缘
// 8px、引导行（未生效态）= 图标 0xF071 + 11px warning，行盒动态增高（卡高
// 同源契约见 md_assoc_logic.h 的 kMdAssocCardHeight*）。
// 开关行先例口径 = github_sync_card.cpp（label 左块、开关钉行右缘、
// kSwitchLabelGap 8）；卡壳 = cardSurface + radiusLarge + shadow + border
//（github 卡同款）。引导行分平台经 platform::open_settings::oneClickAvailable()
// 分流（Windows=一键按钮 / macOS=文字指引），业务代码零平台宏。
#include "md_assoc_card.h"

#include "md_assoc_logic.h"
#include "settings_ui_logic.h"

#include <core/component_theme.h>  // componentTokensWeChat（卡壳阴影/按钮主题）
#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth
#include <platform/open_settings.h>

#include <components/button.h>

#include <ui_kit/card.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <string>

namespace module::settings {

namespace {

// 卡内几何常量（100% 档基准；行盒/字号走 roundFs/fs 缩放，开关/按钮为设计稿
// 字面值不缩放——与 github 卡口径一致）
inline constexpr float kCardPaddingX = 28.0f;
inline constexpr float kCardPaddingY = 8.0f;
inline constexpr float kTitleH = 20.0f;       // 卡标题行高（github 卡同值）
inline constexpr float kTitleRowGapY = 6.0f;  // 标题底与开关行盒顶间距
inline constexpr float kRowH = 40.0f;         // 开关行行盒（设计文档 §3.5）
inline constexpr float kLabelH = 18.0f;       // label 行高
inline constexpr float kLabelHintGapY = 4.0f; // label 与说明行间距（设计 §3.5）
inline constexpr float kHintH = 16.0f;        // 说明行高
inline constexpr float kSwitchW = 44.0f;      // 开关字面值（不随字号缩放）
inline constexpr float kSwitchH = 24.0f;
inline constexpr float kStatusSwitchGapX = 8.0f; // 状态字右缘与开关左缘间距
inline constexpr float kStatusH = 16.0f;         // 状态字行高
inline constexpr float kGuideRowGapY = 2.0f;     // 行盒底与引导行顶间距
inline constexpr float kGuideIconW = 12.0f;      // 警告图标占宽（先例 settings_ui_cards.cpp:714）
inline constexpr float kGuideIconGapX = 4.0f;
inline constexpr float kBtnH = 28.0f;  // Win 一键按钮高（字面值，主导引导行盒）
inline constexpr float kBtnPadX = 12.0f; // 按钮宽 = 文字实宽 + 左右内边距（设计 §3.5）

} // namespace

void drawMdAssocCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    const bool oneClick = platform::open_settings::oneClickAvailable();
    // 状态字四态（判定契约 = mdAssocStatusKind；开关显本地意图、真值校正走状态字）
    const auto kind = mdAssocStatusKind(in.cfg.mdAssocEnabled,
                                        in.cfg.mdAssocEverEnabled,
                                        in.mdAssocState);
    const bool guide = kind == MdAssocStatusKind::NotEffective;

    ui.stack(id)
        .size(w, h)
        .content([&] {
        // 卡壳：白卡 + Sm 阴影 + 描边（github 卡同款）
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(w, h)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f,
                                              0.02f))
            .border(1.0f, t.colors.borderDefault)
            .build();

        const float padX = roundFs(kCardPaddingX);
        const float padY = roundFs(kCardPaddingY);
        const float rowY = padY + roundFs(kTitleH) + roundFs(kTitleRowGapY);
        const float rowH = roundFs(kRowH);

        // 卡标题（设置页各卡一致：15px 加粗）
        ui.text(id + ".title")
            .position(padX, padY)
            .size(w - 2.0f * padX, roundFs(kTitleH))
            .text("文件关联")
            .fontSize(fs(15.0f))
            .lineHeight(roundFs(kTitleH))
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        // 开关（钉行右缘，行盒内垂直居中；字面 44×24）
        const float swX = w - padX - kSwitchW;
        const float swY = rowY + (rowH - kSwitchH) * 0.5f;

        // 状态字（None = 空串 → 不建元素，PRD §八「关+从未开过=无状态字」）。
        // 宽度实量：右缘贴开关左缘 - 8，右对齐。
        const char* statusText = mdAssocStatusLabel(kind);
        const float statusW = statusText[0] == '\0'
                                  ? 0.0f
                                  : core::TextPrimitive::measureTextWidth(
                                        statusText, {}, fs(12.0f), 400);
        if (statusText[0] != '\0') {
            const auto statusColor =
                kind == MdAssocStatusKind::Linked
                    ? t.colors.success
                    : (kind == MdAssocStatusKind::NotEffective
                           ? t.colors.warning
                           : t.colors.textTertiary);
            ui.text(id + ".status")
                .position(swX - roundFs(kStatusSwitchGapX) - statusW,
                          rowY + (rowH - roundFs(kStatusH)) * 0.5f)
                .size(statusW, roundFs(kStatusH))
                .text(statusText)
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(kStatusH))
                .color(statusColor)
                .build();
        }

        // 左块：label（13px 主语义档）+ 说明行（12px 三级灰），flex 吃掉
        // 窗宽增量（右锚组之外的剩余宽，maxWidth 截断防溢出）
        const float textW = std::max(
            0.0f,
            swX - roundFs(kStatusSwitchGapX) - statusW - padX);
        ui.text(id + ".label")
            .position(padX, rowY)
            .size(textW, roundFs(kLabelH))
            .text("关联 .md 文件")
            .fontSize(fs(13.0f))
            .lineHeight(roundFs(kLabelH))
            .fontWeight(500)
            .color(t.colors.textPrimary)
            .maxWidth(textW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .build();
        ui.text(id + ".hint")
            .position(padX, rowY + roundFs(kLabelH) + roundFs(kLabelHintGapY))
            .size(textW, roundFs(kHintH))
            .text("双击 .md 文件时用 DevDesk 只读查看器打开")
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(kHintH))
            .color(t.colors.textTertiary)
            .maxWidth(textW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .build();

        ui_kit::wswitch(ui, id + ".sw", in.cfg.mdAssocEnabled, swX, swY,
                        kSwitchW, kSwitchH, t,
                        [f = a.applyMdAssocEnabled](bool v) { f(v); });

        // 引导行（仅未生效态；行盒动态增高——卡高同源契约 mdAssocCardHeight）
        if (guide) {
            const float gY = rowY + rowH + roundFs(kGuideRowGapY);
            const float guideH = oneClick ? kBtnH : roundFs(kHintH);
            ui.text(id + ".guide.icon")
                .position(padX, gY + (guideH - kGuideIconW) * 0.5f)
                .size(kGuideIconW, kGuideIconW)
                .icon(0xF071) // fa-triangle-exclamation
                .fontSize(fs(11.0f))
                .color(t.colors.warning)
                .build();
            if (oneClick) {
                // Windows：文字 + 行内一键按钮（高 28 字面值右锚；宽 = 文字
                // 实宽 + 左右 12 内边距，最小窗不溢出）
                const char* btnLabel = "打开系统默认应用设置";
                const float btnW =
                    core::TextPrimitive::measureTextWidth(btnLabel, {},
                                                          fs(13.0f), 400) +
                    2.0f * kBtnPadX;
                const float btnX = w - padX - btnW;
                const char* guideText =
                    "Windows 提示当前默认程序未指向 DevDesk，点击右侧按钮修复。";
                const float textW2 = std::max(
                    0.0f, btnX - roundFs(kGuideIconGapX) - padX -
                              kGuideIconW);
                ui.text(id + ".guide.text")
                    .position(padX + kGuideIconW + kGuideIconGapX,
                              gY + (guideH - roundFs(kHintH)) * 0.5f)
                    .size(textW2, roundFs(kHintH))
                    .text(guideText)
                    .fontSize(fs(11.0f))
                    .lineHeight(roundFs(kHintH))
                    .color(t.colors.warning)
                    .maxWidth(textW2)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                components::button(ui, id + ".guide.btn")
                    .position(btnX, gY)
                    .size(btnW, kBtnH)
                    .text(btnLabel)
                    .fontSize(fs(13.0f))
                    .theme(compTheme, /*primary=*/true)
                    .radius(t.sizes.radiusSmall)
                    .onClick([f = platform::open_settings::
                                  openDefaultAppsSettings] { f(); })
                    .build();
            } else {
                // macOS：无程序化直达（PRD D-5），如实呈现文字指引
                const char* guideText =
                    "macOS 无一键入口：Finder 右键任一 .md → 显示简介 → "
                    "打开方式 → 选 DevDesk → 全部更改";
                const float textW2 = std::max(
                    0.0f, w - padX * 2.0f - kGuideIconW - kGuideIconGapX);
                ui.text(id + ".guide.text")
                    .position(padX + kGuideIconW + kGuideIconGapX,
                              gY + (guideH - roundFs(kHintH)) * 0.5f)
                    .size(textW2, roundFs(kHintH))
                    .text(guideText)
                    .fontSize(fs(11.0f))
                    .lineHeight(roundFs(kHintH))
                    .color(t.colors.warning)
                    .maxWidth(textW2)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
            }
        }
        (void)h;
        })
        .build();
}

} // namespace module::settings
