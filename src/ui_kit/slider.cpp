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
    // 手柄位置由 runtime transform 接管（见下方 handle 节点注释），此处不再给初值 x。
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

            // 2026-11-01：手柄 compose 期 x 必须为 0。EUI runtime 的 knob 位移
            // （sliderTransformForElement）是「累加到 compose 期位置」语义，
            // 而官方 components::slider 的 knob 恰是画在 x=0 靠 runtime 平移——
            // 这里若再叠一层 value*(w-S) 的初值，runtime 接管后手柄会跑到
            // 2×位置、与绿色 fill 明显分离。
            ui.rect(id + ".handle")
                .position(0.0f, handleY)
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
