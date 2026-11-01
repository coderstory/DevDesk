// src/ui_kit/wtab.cpp — 微信风格 Tab helper 实现（spec §4.2）。
// 色值全部走 core::theme::current().colors.wechat.* 令牌，禁裸 hex（D-03）。
// 既有页面 Tab 调用不动；本 helper 供 W3 / W4 逐页切换调用。
// 仿 button.cpp 模式：helper 自由函数 + const auto& wc = t.colors.wechat +
// eui::Ui DSL stack / rect / text + 接收 const core::theme::Tokens& t。
#include <ui_kit/wtab.h>

#include <components/theme.h>

namespace ui_kit {

void wtab(eui::Ui& ui, const std::string& id, const std::string& label,
          bool active, float x, float y, float w, float h,
          const core::theme::Tokens& t,
          const std::function<void()>& onClick, float fontSize) {
    // S2.2 块面 Tab：选中=primarySoftBg底+primary字+8px圆角；未选中=透明底+textSecondary字
    // H10：primarySoftBg（#E6FBF2）在白/淡绿底上对比度不足，加 borderDefault 描边。
    //
    // 2026-11-01 三处修正：
    // ① 选中字改用 primaryText（浅色 #157E4E on #F5FAF7 = 4.99:1）。原用 primary
    //    #07C160 只有 2.38:1 —— 14px 文字不达 WCAG AA 4.5:1。
    // ② 浅色选中底改由 primary 按 14% 派生：原 primarySoftBg #E8F8EE 与页面底
    //    #F5FAF7 的 RGB 差 < 13，肉眼读不出「选中」，设置页的 tab 因此退化成
    //    第三种样式（纯文字）。深色主题沿用 primarySoftBg（#0D3326 在深底上本就可辨）。
    // ③ 选中字加 600 字重，配合底色强化状态可辨性。
    const eui::Color txtColor = active ? t.colors.primaryText : t.colors.textSecondary;
    const eui::Color activeBg =
        t.isDark ? t.colors.primarySoftBg
                  : components::theme::withOpacity(t.colors.primary,
                                                   kWtabSelectedBgOpacity);

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // 选中态背景 rect：8px 圆角 + H10 描边
            if (active) {
                ui.rect(id + ".bg")
                    .size(w, h)
                    .color(activeBg)
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

            // 文字：水平居中 + 垂直居中（选中 600 字重，强化状态可辨性）
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(core::theme::scaled(fontSize))
                .fontWeight(active ? 600 : 400)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(txtColor)
                .build();
        })
        .build();
}

} // namespace ui_kit
