// src/ui_kit/glass.h
// presentation 能力层：玻璃卡三件套（填充 / 高光边 / 冷阴影）。
// EUI-NEO 无 backdrop-filter，玻璃感来自「1px 高光边 + 半透 + 冷阴影」，不来自模糊。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

namespace ui_kit {

// 浅色 0.72 让底色透出来；暗色保持 0.92——深色半透会把暗底透成一块灰，压掉可读性。
inline eui::Color glassFill(const core::theme::Tokens& t) {
    const auto& s = t.colors.surface;
    if (t.isDark) return {s.r, s.g, s.b, 0.92f};
    // 3% 绿染：白卡与绿族底同调，纯白压在半透层上会显突兀
    return {s.r * 0.97f + 0.001f, s.g * 0.97f + 0.023f, s.b * 0.97f + 0.011f, 0.72f};
}

// L1 面板层（看板列 / 大面积底板）：比卡更透一档，让卡浮在面板上形成两层深度
inline eui::Color glassPanelFill(const core::theme::Tokens& t) {
    const auto& s = t.colors.surface;
    if (t.isDark) return {s.r, s.g, s.b, 0.55f};
    return {s.r * 0.97f + 0.001f, s.g * 0.97f + 0.023f, s.b * 0.97f + 0.011f, 0.55f};
}

// 高光边：白色带透明度，模拟玻璃边缘的反射亮线
inline eui::Color glassBorder(const core::theme::Tokens& t) {
    return t.isDark ? eui::Color{1.0f, 1.0f, 1.0f, 0.10f}
                    : eui::Color{1.0f, 1.0f, 1.0f, 0.72f};
}

// 冷绿黑阴影：纯黑压在绿白底上会发灰发脏
inline eui::Color glassShadowColor(const core::theme::Tokens& t) {
    return t.isDark ? eui::Color{0.0f, 0.0f, 0.0f, 0.34f}
                    : eui::Color{16.0f / 255.0f, 54.0f / 255.0f, 38.0f / 255.0f, 0.06f};
}

} // namespace ui_kit
