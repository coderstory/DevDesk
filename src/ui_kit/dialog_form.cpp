// src/ui_kit/dialog_form.cpp — 对话框外框 / 文本行 / 按钮对的唯一实现（边界见 .h）。
#include <ui_kit/dialog_form.h>

#include <components/button.h>
#include <components/dialog.h>

namespace ui_kit {

void drawDialogFrame(eui::Ui& ui, const eui::Screen& screen, const DialogFrame& frame,
                     const components::theme::ThemeColorTokens& themeTokens,
                     const std::function<void()>& content) {
    auto builder = components::dialog(ui, frame.id);
    builder.open(frame.open)
        .screen(screen.width, screen.height)
        .size(frame.widthPx(), frame.height)
        .theme(themeTokens);
    // claude / markdown 不调 .zIndex()（用组件默认 1000），todo 显式给值。
    if (frame.zIndex != kDialogZIndexUnset) {
        builder.zIndex(frame.zIndex);
    }
    builder.onOpenChange(frame.onOpenChange).content(content).build();
}

void drawDialogText(eui::Ui& ui, const std::string& id, float dialogWidth,
                    const std::string& text, float fontSize, const eui::Color& color,
                    const DialogTextOptions& options) {
    // 校验错误行是「空串不建节点」，不是画空文本。
    if (options.skipIfEmpty && text.empty()) {
        return;
    }
    const float width =
        options.width > 0.0f ? options.width : (dialogWidth - options.inset);

    auto builder = ui.text(id);
    builder.position(options.x, options.y)
        .size(width, options.height)
        .text(text)
        .fontSize(fontSize)
        .lineHeight(options.lineHeight);
    if (options.bold) {
        builder.fontWeight(700);
    }
    if (options.wrap) {
        builder.wrap();
    }
    builder.color(color).build();
}

void drawDialogTitle(eui::Ui& ui, const std::string& id, float dialogWidth,
                     const std::string& text, float fontSize, const eui::Color& color,
                     const DialogTitleOptions& options) {
    DialogTextOptions textOptions;
    textOptions.x = options.x;
    textOptions.y = options.y;
    textOptions.inset = options.inset;
    textOptions.height = options.height;
    textOptions.lineHeight = options.lineHeight;
    textOptions.bold = true;  // 标题一律 700
    drawDialogText(ui, id, dialogWidth, text, fontSize, color, textOptions);
}

namespace {

// 按钮对公共几何：取消在左、确定在右，各 (w − 2×内缩 − gap)/2，行底距弹窗底 60。
constexpr float kDialogButtonGap = 8.0f;
constexpr float kDialogButtonRowOffsetY = 60.0f;
constexpr float kDialogButtonHeight = 40.0f;

// (w-68)/2 —— 68 = 28×2 + 12，还原构成，不留裸 68。
float buttonWidth(float dialogWidth) {
    return (dialogWidth - kDialogInsetX * 2.0f - kDialogButtonGap) * 0.5f;
}

// 单按钮：几何 + 文案字号 + 样式组合，最后统一 onClick。
void buildDialogButton(eui::Ui& ui, const std::string& id, float x, float y, float width,
                       const DialogButtonSpec& spec, const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens) {
    auto button = components::button(ui, id);
    button.position(x, y)
        .size(width, kDialogButtonHeight)
        .text(spec.label)
        .fontSize(spec.fontSize);
    switch (spec.style) {
        case DialogButtonStyle::Secondary:
            button.colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault);
            break;
        case DialogButtonStyle::SecondaryThemed:
            button.theme(themeTokens, false)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault);
            break;
        case DialogButtonStyle::Primary:
            button
                .colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radius);
            break;
        case DialogButtonStyle::PrimaryThemed:
            button.theme(themeTokens, true)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radius);
            break;
        case DialogButtonStyle::Danger:
            button.colors(t.colors.error, t.colors.error, t.colors.error)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radius);
            break;
        case DialogButtonStyle::DangerThemed:
            button.theme(themeTokens, false)
                .colors(t.colors.error, t.colors.error, t.colors.error)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radius);
            break;
        case DialogButtonStyle::DangerOutline:
            button.colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                .textColor(t.colors.error)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault);
            break;
        case DialogButtonStyle::DangerOutlineThemed:
            button.theme(themeTokens, false)
                .textColor(t.colors.error)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault);
            break;
    }
    button.onClick(spec.onClick).build();
}

} // namespace

void drawDialogButtons(eui::Ui& ui, const std::string& id, float dialogWidth,
                       float dialogHeight, const DialogButtonSpec& cancel,
                       const DialogButtonSpec& confirm, const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens) {
    const float w = buttonWidth(dialogWidth);
    const float y = dialogHeight - kDialogButtonRowOffsetY;
    buildDialogButton(ui, id + ".cancel", kDialogInsetX, y, w, cancel, t, themeTokens);
    buildDialogButton(ui, id + ".confirm", kDialogInsetX + w + kDialogButtonGap, y,
                      w, confirm, t, themeTokens);
}

} // namespace ui_kit
