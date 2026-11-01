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

// 微信风格组件主题映射（mint green 迁移后统一走主 token 槽位，不再拆双色源）
inline components::theme::ThemeColorTokens componentTokensWeChat(const Tokens& t) {
    components::theme::ThemeColorTokens tokens;
    tokens.background = t.colors.background;
    tokens.primary = t.colors.primary;
    tokens.surface = t.colors.surface;
    tokens.surfaceHover = t.colors.hover;
    tokens.surfaceActive = t.colors.primarySoftBg;
    tokens.text = t.colors.textPrimary;
    tokens.border = t.colors.borderDefault;
    tokens.dark = t.isDark;
    // 控件圆角统一到 DevDesk 的 radius 档（10px）。EUI 的 button 按 level 取
    // metrics.radius.overlay(16) / control(8)，而 DevDesk 的 ui_kit::button 用 10px
    // —— 全站两套按钮组件并存时，同一页面会出现两种圆角的按钮。
    // DevDesk 未使用 colorpicker / datepicker / timepicker / segmented / tabs
    // （这几个也读这两个槽位），覆写只影响 button 与 toast，反而一并对齐。
    tokens.metrics.radius.overlay = t.sizes.radius;
    tokens.metrics.radius.control = t.sizes.radius;
    // EUI 的字号槽位同样要接进全局缩放档。EUI 的 typography.* 是写死值，不经
    // DevDesk 的 fontScale —— 全站 66 个 EUI 按钮的文字、以及未显式设 fontSize 的
    // 输入框，改字号设置时纹丝不动。这里按比例缩放全部字号槽位（lineGap 属间距，
    // 不随字号变，与 DevDesk「只缩字号、不动间距」的策略一致）。
    const float fs = fontScale();
    tokens.metrics.typography.micro *= fs;
    tokens.metrics.typography.caption *= fs;
    tokens.metrics.typography.hint *= fs;
    tokens.metrics.typography.label *= fs;
    tokens.metrics.typography.option *= fs;
    tokens.metrics.typography.body *= fs;
    tokens.metrics.typography.input *= fs;
    tokens.metrics.typography.control *= fs;
    tokens.metrics.typography.cardTitle *= fs;
    tokens.metrics.typography.subtitle *= fs;
    tokens.metrics.typography.title *= fs;
    tokens.metrics.typography.heading *= fs;
    tokens.metrics.typography.headline *= fs;
    tokens.metrics.typography.displayCompact *= fs;
    tokens.metrics.typography.display *= fs;
    tokens.metrics.typography.hero *= fs;
    return tokens;
}

} // namespace core::theme
