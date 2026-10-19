// src/ui_kit/wswitch.cpp — 微信风格开关 helper 实现（spec §4.4）。
// 色值全部走 core::theme::current().colors.wechat.* 令牌，禁裸 hex（D-03）。
// 既有页面开关调用不动；本 helper 供 W3 / W4 逐页切换调用。
// 仿 button.cpp 模式：helper 自由函数 + const auto& wc = t.colors.wechat +
// eui::Ui DSL stack / rect + 接收 const core::theme::Tokens& t。
#include <ui_kit/wswitch.h>

namespace ui_kit {

void wswitch(eui::Ui& ui, const std::string& id, bool on,
             float x, float y, float w, float h,
             const core::theme::Tokens& t,
             const std::function<void(bool)>& onToggle) {
    const auto& wc = t.colors.wechat;

    // §4.4 轨道色：开=wechat.primary 绿；关=wechat.dividerSecondary 灰（派生灰
    // 用现有 token，不引入新槽位——开关是唯一用灰轨道的组件，本 helper 内闭合）
    const eui::Color trackColor = on ? wc.primary : wc.dividerSecondary;

    // 滑块几何：直径 = h - 2*padding（在轨道内留白）；圆心 y = padding + thumbR
    const float thumbD = h - 2.0f * kWswitchThumbPadding;
    const float thumbR = thumbD / 2.0f;
    // 滑块 x：关时贴左（x=padding）；开时贴右（x=w-thumbD-padding）
    const float thumbX = on ? (w - thumbD - kWswitchThumbPadding)
                            : kWswitchThumbPadding;
    const float thumbY = kWswitchThumbPadding;

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // 轨道：pill 形（radius = h/2，强制圆角轨道），填满 w×h
            auto track = ui.rect(id + ".track")
                .size(w, h)
                .color(trackColor)
                .radius(h / 2.0f);
            if (onToggle) {
                // 点击=切到对面态：onToggle(!on)
                bool next = !on;
                track.onClick([onToggle, next]() { onToggle(next); });
            }
            track.build();

            // 滑块：白色圆形（radius = thumbD/2）
            // 色 = wechat.cardBg（#FFFFFF 白，§4.4 "白色滑块"）
            // 滑块位移用简单 position；EUI 若有 transition 可后续加 .animate
            ui.rect(id + ".thumb")
                .position(thumbX, thumbY)
                .size(thumbD, thumbD)
                .color(wc.cardBg)
                .radius(thumbR)
                .build();
        })
        .build();
}

} // namespace ui_kit
