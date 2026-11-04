// src/ui_kit/glass.h
// presentation 能力层：玻璃卡三件套（填充 / 高光边 / 冷阴影）。
// EUI-NEO 无 backdrop-filter，玻璃感来自「1px 高光边 + 半透 + 冷阴影」，不来自模糊。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

namespace ui_kit {

// 2026-11-03 池127 上调：功能卡接近实体（Light 0.94 / Dark 0.95），留少量透保玻璃呼吸感；
// 深色同样收紧半透，避免暗底透成一块灰压掉可读性。
// 2026-11-03 池166：Light 基色 白半透→浓绿半透。greenBase 走 primary 派生
// （kGlassGreenMix = 绿→白收敛比；k 每降 0.05 约 −0.22 对比度，k≥0.73 保卡面
// textSecondary 对比 ≥4.5:1），4 档强调色主题自动跟随；两层 alpha 池127 定稿一个不动。
// Dark 深底本无白卡问题，维持 surface 半透（逐字符不变）。
inline constexpr float kGlassGreenMix = 0.75f;

inline eui::Color glassGreenBase(const core::theme::Tokens& t) {
    const auto& p = t.colors.primary;
    const float k = kGlassGreenMix;
    return {p.r * (1.0f - k) + k, p.g * (1.0f - k) + k, p.b * (1.0f - k) + k,
            1.0f};
}

inline eui::Color glassFill(const core::theme::Tokens& t) {
    const auto& s = t.colors.surface;
    if (t.isDark) return {s.r, s.g, s.b, 0.95f};
    const auto g = glassGreenBase(t);
    return {g.r, g.g, g.b, 0.94f};
}

// L1 面板层（看板列 / 大面积底板）：比卡更透一档，让卡浮在面板上形成两层深度
// 2026-11-03 池127 上调至 0.80/0.78（原 0.55），容器提实但保持与 L2 卡 0.14/0.17 的层差
inline eui::Color glassPanelFill(const core::theme::Tokens& t) {
    const auto& s = t.colors.surface;
    if (t.isDark) return {s.r, s.g, s.b, 0.78f};
    const auto g = glassGreenBase(t);
    return {g.r, g.g, g.b, 0.80f};
}

// 高光边：白色带透明度，模拟玻璃边缘的反射亮线
// 2026-11-03 池127 配套：底更实后 Light 降白 0.72→0.60 防「奶圈」，Dark 补 0.10→0.12 边缘定义
inline eui::Color glassBorder(const core::theme::Tokens& t) {
    return t.isDark ? eui::Color{1.0f, 1.0f, 1.0f, 0.12f}
                    : eui::Color{1.0f, 1.0f, 1.0f, 0.60f};
}

// 冷绿黑阴影：纯黑压在绿白底上会发灰发脏
// 2026-11-03 池127 配套：Light 重量感上升，阴影 0.06→0.09 维持「浮起」
inline eui::Color glassShadowColor(const core::theme::Tokens& t) {
    return t.isDark ? eui::Color{0.0f, 0.0f, 0.0f, 0.34f}
                    : eui::Color{16.0f / 255.0f, 54.0f / 255.0f, 38.0f / 255.0f, 0.09f};
}

} // namespace ui_kit
