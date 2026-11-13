// src/module_git/git_colors.cpp
#include "git_colors.h"

#include <components/theme.h>

namespace module::git {

namespace {

using components::theme::withOpacity;

// UI 方案 A §3.2：领域底色 = 同域文字色低透明浅染（浅色档实色近似、深色档 alpha 派生
// 与 wtab 同法）。
eui::Color tint(const eui::Color& base, bool dark, float lightOpacity,
                float darkOpacity) {
    return withOpacity(base, dark ? darkOpacity : lightOpacity);
}

} // namespace

GitColors gitColors(const core::theme::Tokens& t) {
    const bool dark = t.isDark;
    GitColors c;
    // 文字色走既有语义槽位（浅档加深保 WCAG AA，深档提亮）——槽位本身即双套
    c.added = t.colors.primaryText;      // 绿系高亮语义（--st-added）
    c.modified = t.colors.warning;       // 琥珀（--st-modified）
    c.deleted = t.colors.errorText;      // 红系告警（--st-deleted）
    c.renamed = t.colors.info;           // 蓝系信息（--st-renamed）
    c.untracked = t.colors.textSecondary;
    c.untrackedBg = t.colors.surfaceElevated;  // U 徽标灰底（次按钮底语义）
    // 底色 = 对应文字色浅染：浅档 12~14% 叠页面底，深档同式（方案 A rgba 值同带）
    c.addedBg = tint(t.colors.primary, dark, 0.12f, 0.12f);
    c.modifiedBg = tint(t.colors.warning, dark, 0.14f, 0.14f);
    c.deletedBg = tint(t.colors.error, dark, 0.12f, 0.12f);
    c.renamedBg = tint(t.colors.info, dark, 0.14f, 0.14f);
    c.diffAddedBg = tint(t.colors.primary, dark, 0.12f, 0.10f);
    c.diffRemovedBg = tint(t.colors.error, dark, 0.12f, 0.10f);
    c.diffHunkBg = t.colors.surfaceElevated;
    c.diffGutterBg = dark ? withOpacity(t.colors.textPrimary, 0.04f)
                          : t.colors.background;
    return c;
}

} // namespace module::git
