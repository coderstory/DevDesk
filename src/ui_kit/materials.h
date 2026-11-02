// src/ui_kit/materials.h
#pragma once
#include <eui/types.h>        // eui::Color / eui::mixColor 显式 include（审计 P2：不依赖 glass.h 传递可达）
#include "core/theme.h"
#include "ui_kit/glass.h"

namespace ui_kit {
// L1 容器卡：装条目的面板
inline eui::Color containerSurface(const core::theme::Tokens& t) { return glassPanelFill(t); }
// L2 业务卡：容器内的业务条目
inline eui::Color itemSurface(const core::theme::Tokens& t) { return glassFill(t); }
// L2 的 hover/pressed 派生色（半透底上不能直接用 t.colors.hover —— 那是给不透明底的）
inline eui::Color itemSurfaceHover(const core::theme::Tokens& t) {
    return eui::mixColor(glassFill(t), eui::Color(0.0f,0.0f,0.0f,1.0f), t.isDark ? 0.10f : 0.05f);
}
inline eui::Color itemSurfacePressed(const core::theme::Tokens& t) {
    return eui::mixColor(glassFill(t), eui::Color(0.0f,0.0f,0.0f,1.0f), t.isDark ? 0.16f : 0.09f);
}
}
