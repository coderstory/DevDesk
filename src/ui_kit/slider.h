// src/ui_kit/slider.h
// presentation 能力层：横向滑杆 helper（UAT6.2 字号无级调节 80-130%）。
// 本层被 module_* 与 app 层链接，core / platform 不得反向依赖；被各页面共用。
// 色值全走 core::theme::Tokens 令牌，禁裸 hex。
//
// 取值通道 = EUI-NEO 运行时托管（stack 上 sliderState + 命中层 sliderInputFrom）：
// 按下即跳值、按住拖动连续取值、松手结束拖拽；拖拽中的 recompose 不抢值
//（runtime 对 dragging 态不回写 element 值）。value / onChange 都是归一化
// [0,1] 刻度（0 = min，1 = max），业务量映射由调用方负责。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <eui/types.h>

#include <functional>
#include <string>

namespace ui_kit {

// 几何契约：轨道恒 4px 圆角条、手柄恒 16px 圆（行盒高由调用方给，二者垂直居中）。
inline constexpr float kSliderTrackHeight = 4.0f;
inline constexpr float kSliderHandleSize = 16.0f;

// 渲染单滑杆：stack(id) + sliderState，内含
//   rect.track   轨道（hover 底色 / radiusSmall）
//   rect.fill    已填充段（primary，宽度由运行时按 state 驱动）
//   rect.handle  圆手柄（surface 白底 + 1px borderDefault，x 由运行时驱动）
//   rect.hit     命中层（透明，sliderInputFrom —— 点击跳值 / 拖拽取值的唯一入口）
// 节点 id = `<id>.track` / `<id>.fill` / `<id>.handle` / `<id>.hit`，单测按此定位。
// 几何：(x, y) 左上角，(w, h) 尺寸；value 归一化 [0,1]（越界钳制进 range），
// onChange 收同刻度连续值（拖拽中每次变动都回调，调用方自行量化/钳制）。
void slider(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
            float value, const core::theme::Tokens& t,
            const std::function<void(float)>& onChange = {});

} // namespace ui_kit
