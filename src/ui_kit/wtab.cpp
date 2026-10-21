// src/ui_kit/wtab.cpp — 微信风格 Tab helper 实现（spec §4.2）。
// 色值全部走 core::theme::current().colors.wechat.* 令牌，禁裸 hex（D-03）。
// 既有页面 Tab 调用不动；本 helper 供 W3 / W4 逐页切换调用。
// 仿 button.cpp 模式：helper 自由函数 + const auto& wc = t.colors.wechat +
// eui::Ui DSL stack / rect / text + 接收 const core::theme::Tokens& t。
#include <ui_kit/wtab.h>

namespace ui_kit {

void wtab(eui::Ui& ui, const std::string& id, const std::string& label,
          bool active, float x, float y, float w, float h,
          const core::theme::Tokens& t,
          const std::function<void()>& onClick) {
    const auto& wc = t.colors.wechat;
    // §4.2：未激活=textSecondary（#666）灰字；激活=textPrimary（#1A1A1A）深灰字
    const eui::Color txtColor = active ? t.colors.textPrimary : t.colors.textSecondary;

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // hit area：透明 rect 挂 onClick；不画 bg（§4.2 "无背景"），防
            // "实心填充按钮做 Tab" 禁止项（§4.2 / §8 #1）。透明 = Color{0,0,0,0}
            // 是 spec 允许的唯一裸字面量（透明色无 token 槽位）。
            auto hit = ui.rect(id + ".hit")
                .size(w, h)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f});
            if (onClick) {
                hit.onClick(onClick);
            }
            hit.build();

            // 文字：水平居中 + 垂直居中；颜色按 active 切换
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(kWtabFontSize)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(txtColor)
                .build();

            // 下划线：仅 active 时画；2px 高，宽=w，贴 stack 底部
            // 色 = wechat.primary（#07C160，§4.2 "底部 2px 微信绿下划线"）
            if (active) {
                ui.rect(id + ".underline")
                    .position(0.0f, h - kWtabUnderlineHeight)
                    .size(w, kWtabUnderlineHeight)
                    .color(t.colors.primary)
                    .build();
            }
        })
        .build();
}

} // namespace ui_kit
