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
// 圆角半径（老板 2026-11-04「黑框需要考虑窗体的圆角」；2026-11-06 修 mac 贴边）：
// 平台差异由调用方以 windowCornerRadiusParam 传入（业务层禁 #ifdef，见
// AGENTS.md §三；app_sidebar.cpp 按平台算好传进来）：
//   Windows = Win11 DWM 固定 8 物理px（不随 DPI 缩放）→ 传 8.0f；
//   macOS   = AppKit 原生窗口圆角（EUI 从不设 layer.cornerRadius，系统绘制），
//             macOS 26/27 实测约 12 逻辑pt 且不随 backingScale → 传 12.0f；
//   Linux   同 Windows 口径。
// 描边路径在 inset 处要贴住裁剪弧（近似同心），故再减 inset。
// contentScaleParam：物理/逻辑比（Windows 上圆角是物理px，需除回逻辑）。
inline float windowBorderRadius(float cornerRadiusLogical,
                                float contentScaleParam) {
    const float scale = contentScaleParam > 0.0f ? contentScaleParam : 1.0f;
    const float base = cornerRadiusLogical > 0.0f ? cornerRadiusLogical : 8.0f;
    // macOS 传的是逻辑 pt（scale 不参与）；Windows 传物理 px（需 /scale）。
    // 由调用方传「已换算到逻辑」的值，这里不再二次归一 —— 保持单一口径。
    (void)scale;
    return std::max(2.0f, base - kWindowRingInset);
}

// 在窗口整幅 bounds 上叠边框；调用点 = 应用 compose 根最后（最顶层）。
// cornerRadiusLogical：窗体真实圆角（逻辑px，调用方按平台给：mac 12 / Win 8/scale）。
// contentScaleParam：保留参数位（历史调用兼容），当前不参与半径计算。
inline void drawWindowRing(eui::Ui& ui, float w, float h,
                           const core::theme::Tokens& t, bool maximized,
                           float contentScaleParam = 1.0f,
                           float cornerRadiusLogical = 8.0f) {
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
        .radius(windowBorderRadius(cornerRadiusLogical, contentScaleParam))
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .border(kWindowRingBorder, eui::Color{0.0f, 0.0f, 0.0f, 1.0f})
        .build();
}

}  // namespace ui_kit
