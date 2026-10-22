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
          const std::function<void()>& onClick, float fontSize) {
    // S2.2 块面 Tab：选中=primarySoftBg底+primary字+8px圆角；未选中=透明底+textSecondary字
    // H10：primarySoftBg（#E6FBF2）在白/淡绿底上对比度不足，加 borderDefault 描边。
    const eui::Color txtColor = active ? t.colors.primary : t.colors.textSecondary;

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // 选中态背景 rect：primarySoftBg（#E6FBF2）+ 8px 圆角 + H10 描边
            if (active) {
                ui.rect(id + ".bg")
                    .size(w, h)
                    .color(t.colors.primarySoftBg)
                    .radius(8.0f)
                    .border(1.0f, t.colors.borderDefault)
                    .build();
            }

            // hit area：透明 rect 挂 onClick
            auto hit = ui.rect(id + ".hit")
                .size(w, h)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f});
            if (onClick) {
                hit.onClick(onClick);
            }
            hit.build();

            // 文字：水平居中 + 垂直居中
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(fontSize)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(txtColor)
                .build();
        })
        .build();
}

} // namespace ui_kit
