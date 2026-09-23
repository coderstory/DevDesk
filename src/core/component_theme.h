// src/core/component_theme.h
// core::theme 设计 token → EUI-NEO 组件主题（components::theme::ThemeColorTokens）
// 的唯一映射。模块层所有 EUI-NEO 组件的 .theme() 注入一律走本函数；
// 禁止直接用 components::theme::light()/dark() 内置默认——那是冷灰底 +
// 蓝主色的通用默认，与方案 B 暖米色系（spec §6.5.1）不一致，曾导致浅色
// 模式下"页面米色、组件纯白"的色源不统一 bug。
//
// 分层说明：本头文件供模块层使用，core 静态库自身的 .cpp 不包含它
//（core 实现不依赖 EUI-NEO components，分层铁律不变；与 theme.h 引
// eui/types.h 同属"类型桥接"性质的例外，且以 header-only 形式存在）。
#pragma once

#include "theme.h"

#include <components/theme.h>

namespace core::theme {

// 从 DevDesk 设计 token 派生 EUI-NEO 组件主题（唯一色源 = core::theme）。
// 字段映射：background←background / primary←primary / surface←surface /
// surfaceHover←hover / surfaceActive←active / text←textPrimary /
// border←borderDefault / dark←isDark；metrics 用 EUI-NEO 默认
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
