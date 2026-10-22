// src/ui_kit/selection_bar.cpp — 微信风格选中态 helper 实现（spec §4.3）。
// 色值全部走 core::theme::current().colors.wechat.* 令牌，禁裸 hex（D-03）。
// 既有页面选中态调用不动；本 helper 供 W3 nav / W4 settings 逐页切换调用。
// 仿 button.cpp 模式：helper 自由函数 + const auto& wc = t.colors.wechat +
// eui::Ui DSL stack / rect / text + 接收 const core::theme::Tokens& t。
#include <ui_kit/selection_bar.h>

namespace ui_kit {

void selectionBar(eui::Ui& ui, const std::string& id, const std::string& label,
                  float x, float y, float w, float h,
                  const core::theme::Tokens& t) {

    // stack 包裹 bg + accent + text，保证竖线与文字浮在底色之上；调用方可把
    // stack 放进任意布局（如 nav 列表项的 row stack）。
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // 底色 rect：wechat.selectedBg（#E5E5E5 浅灰，§4.3 "深灰背景"）
            ui.rect(id + ".bg")
                .size(w, h)
                .color(t.colors.primarySoftBg)
                .build();

            // 左 2px 竖线：wechat.primary（#07C160，§4.3 "左侧 2px 微信绿竖线"）
            // 贴 stack 左缘 (0, 0)，宽 2px、高 h
            ui.rect(id + ".accent")
                .position(0.0f, 0.0f)
                .size(kSelectionBarAccentWidth, h)
                .color(t.colors.primary)
                .build();

            // 文字：wechat.textPrimary（#1A1A1A 深灰）+ 14px + 左对齐 + 垂直居中
            // 左 padding kSelectionBarTextPadX（12px），让文字距竖线留白
            ui.text(id + ".text")
                .position(kSelectionBarTextPadX, 0.0f)
                .size(w - kSelectionBarTextPadX, h)
                .text(label)
                .fontSize(kSelectionBarFontSize)
                .horizontalAlign(eui::HorizontalAlign::Left)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
        })
        .build();
}

} // namespace ui_kit
