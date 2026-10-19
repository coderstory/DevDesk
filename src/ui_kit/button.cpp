// src/ui_kit/button.cpp — 微信风格四级按钮 helper 实现（spec §4.1）。
// 色值全部走 core::theme::current().colors.wechat.* 令牌，禁裸 hex（D-03）。
// 既有页面 button 调用不动；本 helper 供 W5-W10 逐页切换调用。
#include <ui_kit/button.h>

#include <algorithm>

namespace ui_kit {

namespace {

// Danger hover 派生色：warningRed #FA5151 加深一档（spec §4.1 允许「派生或用
// warningRed」）。r 维持高位（红感），g/b 降一档 → 视觉上比正常态更深。
// 不引入新 token 槽位：Danger 是唯一红系按钮，派生在本 helper 内闭合即可。
eui::Color darkenRed(const eui::Color& c) {
    return eui::Color{
        std::max(0.0f, c.r * 0.92f),
        std::max(0.0f, c.g * 0.85f),
        std::max(0.0f, c.b * 0.85f),
        c.a
    };
}

} // namespace

void button(eui::Ui& ui, const std::string& id, const std::string& label,
            ButtonLevel level, float x, float y, float w, float h,
            const core::theme::Tokens& t,
            const ButtonOptions& options) {
    const auto& wc = t.colors.wechat;
    eui::Color bgNormal, bgHover, bgPressed, txtColor;

    // §4.1 四级配色：每级 (normal, hover, pressed) + 文字色。
    // hover 走 spec §4.13「hover 时背景变浅灰」对填充按钮的口径（Primary 用
    // primaryHover 派生绿，Secondary/Text 用 dividerSecondary 浅灰，Danger 派生深红）。
    switch (level) {
        case ButtonLevel::Primary:
            // 核心确认：微信绿 + 白字；hover 走 primaryHover（派生绿，#06AE56）
            bgNormal = wc.primary;
            bgHover = wc.primaryHover;
            bgPressed = wc.primaryPressed;
            txtColor = wc.onPrimary;
            break;
        case ButtonLevel::Secondary:
            // 次要操作：浅灰底 + 深灰字；hover 走 dividerSecondary（#EDEDED）
            bgNormal = wc.secondaryButtonBg;
            bgHover = wc.dividerSecondary;
            bgPressed = wc.dividerSecondary;
            txtColor = wc.textPrimary;
            break;
        case ButtonLevel::Text:
            // 辅助操作：透明底 + 绿字（默认）；hover bg=dividerSecondary（§4.13）
            bgNormal = eui::Color{0.0f, 0.0f, 0.0f, 0.0f};
            bgHover = wc.dividerSecondary;
            bgPressed = wc.dividerSecondary;
            txtColor = wc.primary;
            break;
        case ButtonLevel::Danger:
            // 删除/高危：警示红 + 白字；hover 加深一档（派生）
            bgNormal = wc.warningRed;
            bgHover = darkenRed(wc.warningRed);
            bgPressed = darkenRed(wc.warningRed);
            txtColor = wc.onPrimary;
            break;
    }

    // 文字色 override（Text 级绿/灰按语义；其他级一般不覆盖）
    if (options.hasTextColorOverride) {
        txtColor = options.textColorOverride;
    }

    // stack 包裹 rect.bg + text，保证文字浮在填充之上；调用方可把 stack 放进任意布局。
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // rect.bg：三态 hover + 点击 + radius；不加 border/shadow（§4.1 禁止项）
            auto bg = ui.rect(id + ".bg")
                .size(w, h)
                .states(bgNormal, bgHover, bgPressed)
                .radius(t.sizes.radiusWeChat);
            if (options.onClick) {
                bg.onClick(options.onClick);
            }
            bg.build();

            // 文字：水平+垂直居中，14px（kButtonFontSize）
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(kButtonFontSize)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(txtColor)
                .build();
        })
        .build();
}

} // namespace ui_kit
