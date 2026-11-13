// src/module_git/git_views.h
// Git 页三栏视图 compose（need:7 批次二；UI 方案 A 逐区块参数）。
// 渲染纪律（clue#22）：所有行盒/命中矩形 = （行模型, 宽度）的纯函数，compose
// measure pass 每帧两跑幂等；零矩形缓存。
#pragma once

#include <core/theme.h>

#include <eui/dsl.h>
#include <eui/types.h>

#include "git_page_controller.h"

namespace module::git {

// 整页入口：phase 分发（Empty → 空态页；Ready → 三栏主界面）
void composeGitPage(eui::Ui& ui, const eui::Screen& screen,
                    GitPageController& controller,
                    const core::theme::Tokens& t);

} // namespace module::git
