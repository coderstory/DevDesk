// src/ui_kit/dialog_shell.cpp — 标准弹窗壳实现（规范参数出处见 .h / metrics.h）。
#include <ui_kit/dialog_shell.h>

#include <components/button.h>
#include <components/dialog.h>

#include <core/render/text.h>  // measureTextWidth（按钮自适应宽与渲染同引擎）

#include <algorithm>

namespace ui_kit {

namespace {

// 按钮字号 14（规范 §3.3：共享件 15 归一为 14；kButtonFontSize 同值不引依赖）。
constexpr float kShellButtonFontSize = 14.0f;
constexpr float kShellButtonH = 40.0f;
// 文字超出最小宽时的内边距 24×2（规范 §3.3「文字超出则自适应加宽」）。
constexpr float kShellButtonPadX = 24.0f;

// 宽 = max(96, 文本宽 + 48)；测量与渲染同一引擎（core/render/text.h）。
float shellButtonWidth(const std::string& label, const core::theme::Tokens& t) {
    const float textW = core::TextPrimitive::measureTextWidth(
        label, "", core::theme::scaled(kShellButtonFontSize), 400);
    return std::max(kDialogButtonMinW, textW + kShellButtonPadX * 2.0f);
}

// 单按钮：几何 + 14px 字 + 规范 §4.3 四态（主/副/危险/禁用），色全走 token。
void buildShellButton(eui::Ui& ui, const std::string& id, float x, float y, float w,
                      const DialogShellButton& spec, bool danger, bool primaryRole,
                      const core::theme::Tokens& t) {
    auto button = components::button(ui, id);
    button.position(x, y)
        .size(w, kShellButtonH)
        .text(spec.label)
        .fontSize(core::theme::scaled(kShellButtonFontSize))
        .radius(t.sizes.radius);
    if (!spec.enabled) {
        // 禁用态：无 hover/按压（三态同色）。
        if (primaryRole) {
            button.colors(t.colors.primarySoftBg, t.colors.primarySoftBg,
                          t.colors.primarySoftBg)
                .textColor(t.colors.textDisabled);
        } else {
            button.colors(t.colors.surfaceElevated, t.colors.surfaceElevated,
                          t.colors.surfaceElevated)
                .textColor(t.colors.textDisabled)
                .border(1.0f, t.colors.borderDefault);
        }
    } else if (danger) {
        // 危险变体：error 无 hover 档，三态同色加深禁用。
        button.colors(t.colors.error, t.colors.error, t.colors.error)
            .textColor(t.colors.onPrimary);
    } else if (primaryRole) {
        button.colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
            .textColor(t.colors.onPrimary);
    } else {
        button.colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .border(1.0f, t.colors.borderDefault);
    }
    if (spec.enabled && spec.onClick) {
        button.onClick(spec.onClick);
    }
    button.build();
}

} // namespace

void drawDialogShell(eui::Ui& ui, const eui::Screen& screen, const DialogShellSpec& spec,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     const std::function<void(const DialogContentArea&)>& content) {
    // 视口 clamp（规范 §3.4/§8）：宽高 ≤ 视口 − 96（上下左右边距各 48）；极窄视口
    // clamp 优先、可低于最小下限。框架内部另有 −48 一档 clamp，比这里更松不生效。
    const float w =
        std::min(dialogWidthPx(spec.width), std::max(0.0f, screen.width - 96.0f));
    const float h = std::min(std::max(spec.height, kDialogMinH),
                             std::max(0.0f, screen.height - 96.0f));

    auto builder = components::dialog(ui, spec.id);
    builder.open(spec.open)
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens);
    if (spec.zIndex != kDialogZIndexUnset) {
        builder.zIndex(spec.zIndex);
    }
    builder.onOpenChange(spec.onOpenChange)
        .content([&] {
            // 头区（固定 69）：标题 20/700（y=16，x=24）+ 分隔线 1px y=52 全宽。
            drawDialogTitle(ui, spec.id + ".title", w, spec.title, kDialogTitleFontSize,
                            t.colors.textPrimary, {});
            ui.rect(spec.id + ".title.line")
                .position(0.0f, 52.0f)
                .size(w, 1.0f)
                .color(t.colors.borderDefault)
                .build();
            // 底区（固定 73）：细线 1px y=h-73 全宽（borderMuted，比分隔线轻一档）。
            ui.rect(spec.id + ".footer.line")
                .position(0.0f, h - kDialogFooterH)
                .size(w, 1.0f)
                .color(t.colors.borderMuted)
                .build();
            // 按钮行右锚：右端 x=w-24、y=h-56、gap 8；副按钮在左、主按钮最右。
            const float rowY = h - kDialogFooterH + 17.0f;  // = h-56（线 1 + 隙 16）
            float xRight = w - kDialogInsetX;
            if (!spec.confirm.label.empty()) {
                const float bw = shellButtonWidth(spec.confirm.label, t);
                buildShellButton(ui, spec.id + ".confirm", xRight - bw, rowY, bw,
                                 spec.confirm, spec.dangerConfirm, true, t);
                xRight -= bw + kDialogButtonGap;
            }
            if (!spec.cancel.label.empty()) {
                const float bw = shellButtonWidth(spec.cancel.label, t);
                buildShellButton(ui, spec.id + ".cancel", xRight - bw, rowY, bw,
                                 spec.cancel, false, false, t);
            }
            // 内容区（唯一内滚区）：y=69 至细线上隙 16（底 = h-89）。
            if (content) {
                content({kDialogInsetX, kDialogHeaderH, w - kDialogBodyInsetX,
                         h - kDialogHeaderH - kDialogFooterH});
            }
        })
        .build();
}

} // namespace ui_kit
