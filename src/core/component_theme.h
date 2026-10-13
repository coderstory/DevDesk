// DevDesk 设计 token → EUI-NEO 组件主题的唯一映射：模块层组件的 .theme() 一律走本函数。
// 禁用 components::theme::light()/dark() 内置默认（冷灰+蓝主色，与暖米色系不一致，
// 曾致浅色模式「页面米色、组件纯白」色源分裂）。
// 分层例外：core → EUI-NEO 仅限 header-only 类型桥接（类型 + 值 + inline 纯映射），
// 禁止调用其运行时行为；core 静态库 .cpp 不含此头。
#pragma once

#include "theme.h"

#include <components/theme.h>

namespace core::theme {

// 从 design token 派生组件主题（唯一色源 = core::theme）
inline components::theme::ThemeColorTokens componentTokens(const Tokens& t) {
    components::theme::ThemeColorTokens tokens;
    tokens.background = t.colors.background;
    tokens.primary = t.colors.primary;
    tokens.surface = t.colors.surface;
    tokens.surfaceHover = t.colors.hover;
    tokens.surfaceActive = t.colors.active;
    tokens.text = t.colors.textPrimary;
    tokens.border = t.colors.borderDefault;
    tokens.dark = t.isDark;
    return tokens;
}

} // namespace core::theme
