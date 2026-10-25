// src/ui_kit/slider.cpp — 横向滑杆 helper 实现（UAT6.2 字号无级调节）。
// 仿 wswitch.cpp 模式：helper 自由函数 + const core::theme::Tokens& t +
// eui::Ui DSL。取值通道走 EUI-NEO 运行时托管 sliderState / sliderInputFrom
//（按下跳值、拖动连续取值、松手结束），fill 宽度与 handle x 由运行时驱动，
// compose 期写入的宽 / x 只是运行时接管前的初值。
#include "ui_kit/slider.h"

#include <algorithm>

namespace ui_kit {

void slider(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
            float value, const core::theme::Tokens& t,
            const std::function<void(float)>& onChange) {
    value = std::clamp(value, 0.0f, 1.0f);
    const float trackY = (h - kSliderTrackHeight) * 0.5f;
    // 手柄行程映射与 runtime sliderValueFromPointer 同口径（手柄中心 = value*行程）
    const float handleX = value * (w - kSliderHandleSize);
    const float handleY = (h - kSliderHandleSize) * 0.5f;

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .sliderState(id, value, w, kSliderHandleSize, onChange)
        .content([&] {
            ui.rect(id + ".track")
                .position(0.0f, trackY)
                .size(w, kSliderTrackHeight)
                .color(t.colors.hover)
                .radius(t.sizes.radiusSmall)
                .build();

            ui.rect(id + ".fill")
                .position(0.0f, trackY)
                .size(w * value, kSliderTrackHeight)
                .color(t.colors.primary)
                .radius(t.sizes.radiusSmall)
                .sliderFillFrom(id)
                .build();

            ui.rect(id + ".handle")
                .position(handleX, handleY)
                .size(kSliderHandleSize, kSliderHandleSize)
                .color(t.colors.surface)
                .radius(kSliderHandleSize * 0.5f)
                .border(1.0f, t.colors.borderDefault)
                .sliderKnobFrom(id)
                .build();

            ui.rect(id + ".hit")
                .position(0.0f, 0.0f)
                .size(w, h)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .zIndex(10)
                .cursor(eui::CursorShape::Hand)
                .interactive()
                .sliderInputFrom(id)
                .build();
        })
        .build();
}

} // namespace ui_kit
