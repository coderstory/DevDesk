// src/ui_kit/window_ring.h
// presentation 能力层：应用窗口细线黑边（池168；2026-11-04 老板撤销绿白虚框
// ring，原话「撤销窗体边缘 绿色线条的效果，改成细线黑边」）。
// 单层：细黑实线圆角描边；非交互 rect（不吃八向 resize 热区 / 标题栏拖拽命中）。
// 平台观感差异：Windows = 真实窗框位置（undecorated 自绘壳，客户区 = 整窗）；
// macOS = 系统窗框内侧内描边（系统标题栏在边框外）。
// 最大化态不绘（need184③ 保留：最大化窗口直角贴屏缘，细线黑边无增益）；
// maximized 由调用方经 eui::app::mainWindowMaximized() 查询传入（能力在 fork
// app.h:197 暴露），ui_kit 收参数不直接依赖 app 层保持可测。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <algorithm>

namespace ui_kit {

// 边框几何（2026-11-04 细线口径）：线宽 1（细线）、内缩 1。
constexpr float kWindowRingInset = 1.0f;
constexpr float kWindowRingBorder = 1.0f;
// 圆角半径（老板 2026-11-04「黑框需要考虑窗体的圆角」）：Win11 DWM 窗口圆角
// = 固定 8 物理px（不随 DPI 缩放）→ 逻辑半径 = 8 / contentScale；描边路径在
// inset 处要贴住 DWM 裁剪弧（近似同心），取 逻辑半径 − inset。此前按「8 逻辑」
// 写死 10，135% 屏上弧过圆、角落黑线内凹离窗缘。
inline float windowBorderRadius(float contentScaleParam) {
    const float scale = contentScaleParam > 0.0f ? contentScaleParam : 1.0f;
    return std::max(2.0f, 8.0f / scale - kWindowRingInset);
}

// 在窗口整幅 bounds 上叠边框；调用点 = 应用 compose 根最后（最顶层）。
// contentScaleParam：物理/逻辑比（调用方传 eui::app::contentScale()；测试传 1.0）。
inline void drawWindowRing(eui::Ui& ui, float w, float h,
                           const core::theme::Tokens& t, bool maximized,
                           float contentScaleParam = 1.0f) {
    (void)t;  // 黑色恒定不随主题（撤销绿白虚框后无主题联动参数）
    if (maximized) return;   // 最大化直角贴屏缘不绘（见文件头）
    const float x0 = kWindowRingInset;
    const float y0 = kWindowRingInset;
    const float w0 = w - 2.0f * x0;
    const float h0 = h - 2.0f * y0;
    if (w0 <= 0.0f || h0 <= 0.0f) return;

    // 细黑边：单 rect 描边。填充必须显式透明（bug185 根因教训）：DSL Element
    // 默认 color 是不透明白 {1,1,1,1}（EUI core/dsl.h:133），本 rect 全窗尺寸且
    // compose 最后构建，吃默认填充=一整块不透明白把整帧内容盖死。border-only
    // 覆盖层一律显式 .color(透明)。
    ui.rect("app.ring.base")
        .position(x0, y0)
        .size(w0, h0)
        .radius(windowBorderRadius(contentScaleParam))
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .border(kWindowRingBorder, eui::Color{0.0f, 0.0f, 0.0f, 1.0f})
        .build();
}

}  // namespace ui_kit
