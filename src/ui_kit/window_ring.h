// src/ui_kit/window_ring.h
// presentation 能力层：应用窗口「绿白间隔圆角虚框」ring（池168，调研报告方案 a）。
// 双层：白色实线圆角底环（间隔露出的「白」）+ 沿四直边排布的绿色短段盖在上面；
// 全部非交互 rect（不吃八向 resize 热区 / 标题栏拖拽命中）。
// 平台观感差异（调研报告 F1/F3）：Windows = 真实窗框位置（undecorated 自绘壳，
// 客户区 = 整窗）；macOS = 系统窗框内侧内描边（系统标题栏在 ring 外）。
// 最大化态不绘（need184③，设计意图：最大化窗口无圆角，ring 圆角弧与直角窗缘
// 相切会被 DWM 裁切、观感错）；maximized 由调用方经 eui::app::mainWindowMaximized()
// 查询传入（能力已在 fork app.h:197 暴露），ui_kit 收参数不直接依赖 app 层保持可测。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <string>

namespace ui_kit {

// ring 几何（调研报告 §五 自由度，2026-11-03 首版静态）：底环线宽 2、内缩 2；
// 圆角 10 = Win11 系统圆角(~8) + 内缩（环弧必须包络系统圆角，否则四角被 DWM
// 裁掉）；绿段 14 / 白间隔 6（老板口径「间隔是白色且比较短」）。
constexpr float kWindowRingInset = 2.0f;
constexpr float kWindowRingBorder = 2.0f;
constexpr float kWindowRingRadius = 10.0f;
constexpr float kWindowRingDash = 14.0f;
constexpr float kWindowRingGap = 6.0f;

// 在窗口整幅 bounds 上叠 ring；调用点 = 应用 compose 根最后（最顶层）。
inline void drawWindowRing(eui::Ui& ui, float w, float h,
                           const core::theme::Tokens& t, bool maximized) {
    if (maximized) return;   // 最大化无圆角，ring 弧段会被直角窗缘裁切（见文件头）
    const float x0 = kWindowRingInset;
    const float y0 = kWindowRingInset;
    const float w0 = w - 2.0f * x0;
    const float h0 = h - 2.0f * y0;
    if (w0 <= 0.0f || h0 <= 0.0f) return;

    // 底环：白色实线圆角环——间隔露出的「白」就是它。
    // 填充必须显式透明（bug185 根因修复）：DSL Element 默认 color 是不透明白
    // {1,1,1,1}（EUI core/dsl.h:133），本 rect 全窗尺寸且 compose 最后构建，
    // 吃默认填充=一整块不透明白把整帧内容盖死、只露 ring——白帧回归的直接病灶。
    // border-only 覆盖层一律显式 .color(透明)，不吃 DSL 默认填充。
    ui.rect("app.ring.base")
        .position(x0, y0)
        .size(w0, h0)
        .radius(kWindowRingRadius)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .border(kWindowRingBorder, eui::Color{1.0f, 1.0f, 1.0f, 1.0f})
        .build();

    // 绿段：四直边避开角弧（直段 = 全长 − 两端圆角），起点对齐、步距 = 段+隔
    const auto& green = t.colors.primary;
    const float r = kWindowRingRadius;
    const float spanX = w0 - 2.0f * r;
    const float spanY = h0 - 2.0f * r;
    const float pitch = kWindowRingDash + kWindowRingGap;
    if (spanX > kWindowRingDash) {
        int n = 0;
        for (float x = x0 + r; x + kWindowRingDash <= x0 + r + spanX + 0.01f;
             x += pitch, ++n) {
            ui.rect("app.ring.top." + std::to_string(n))
                .position(x, y0)
                .size(kWindowRingDash, kWindowRingBorder)
                .color(green)
                .build();
            ui.rect("app.ring.bottom." + std::to_string(n))
                .position(x, y0 + h0 - kWindowRingBorder)
                .size(kWindowRingDash, kWindowRingBorder)
                .color(green)
                .build();
        }
    }
    if (spanY > kWindowRingDash) {
        int n = 0;
        for (float y = y0 + r; y + kWindowRingDash <= y0 + r + spanY + 0.01f;
             y += pitch, ++n) {
            ui.rect("app.ring.left." + std::to_string(n))
                .position(x0, y)
                .size(kWindowRingBorder, kWindowRingDash)
                .color(green)
                .build();
            ui.rect("app.ring.right." + std::to_string(n))
                .position(x0 + w0 - kWindowRingBorder, y)
                .size(kWindowRingBorder, kWindowRingDash)
                .color(green)
                .build();
        }
    }
}

} // namespace ui_kit
