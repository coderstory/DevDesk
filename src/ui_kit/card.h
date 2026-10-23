// src/ui_kit/card.h
// presentation 能力层：卡片底板绘制。本层被 module_* 与 app 层链接，core / platform
// 不得反向依赖；被 settings 各卡共用。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <ui_kit/metrics.h>

#include <string>

namespace ui_kit {

// H15：白卡半透表面色——t.colors.surface 叠加 kCardGlassAlpha 半透 alpha 通道
inline eui::Color cardSurface(const core::theme::Tokens& t) {
    return {t.colors.surface.r, t.colors.surface.g, t.colors.surface.b, kCardGlassAlpha};
}

// 卡片底板：表面色矩形 + 大圆角 + 1px 描边，铺在卡根 stack 的 (0,0)；几何由调用方决定。
// 节点 id = `<id>.bg`（单测按此定位，不得改）。内边距四周 ≥16px，调用方负责内容留白。
void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
               const core::theme::Tokens& t);

} // namespace ui_kit
