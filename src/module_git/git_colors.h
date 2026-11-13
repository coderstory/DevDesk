// src/module_git/git_colors.h
// git 管理器领域色 token（need:7 批次二；UI 方案 A §3.2 领域编码组）：
// 与 mdHighlightColors 同法——从主题 Tokens 语义槽位派生（t.isDark 分档取值），
// 不在业务代码裸写 UI-A 之外的色值。浅/深双套随主题自动成立。
#pragma once

#include <core/theme.h>

#include <eui/types.h>

namespace module::git {

struct GitColors {
    eui::Color added;        // 新增（A 徽标字/着色字）
    eui::Color addedBg;      // 新增徽标底
    eui::Color modified;     // 修改（琥珀，git 行业惯例）
    eui::Color modifiedBg;
    eui::Color deleted;      // 删除（红）
    eui::Color deletedBg;
    eui::Color renamed;      // 重命名（蓝，信息传递）
    eui::Color renamedBg;
    eui::Color untracked;    // 未跟踪（中性弱化，组头已承担区分）
    eui::Color untrackedBg;  // 灰徽标底（次按钮底槽位）
    eui::Color diffAddedBg;    // diff 增行底（批次 #10 消费，先落 token）
    eui::Color diffRemovedBg;  // diff 删行底
    eui::Color diffHunkBg;     // hunk 头/折叠条底
    eui::Color diffGutterBg;   // 行号列底
};

GitColors gitColors(const core::theme::Tokens& tokens);

} // namespace module::git
