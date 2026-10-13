// src/module_claude_proxy/claude_ui_logs.h
// 调用记录弹窗单元：过滤（搜索 + 状态码）/ 分页 / 后台取数。
// 取数模型：渲染路径零 DB —— 弹窗打开时 compose 每帧调 refreshCallsCache：在途
// 结果就绪则搬进快照；脏 + 无在途 + 过防抖窗口才发一次读意图。测试缝计数器见 .cpp。
#pragma once

#include "claude_ui_state.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>

namespace module::claude {

// 「查看全部 →」：开弹窗 + 置脏（下一帧后台查第一页）
void openCallsDialog(LogsState& st);

// 完整调用记录弹窗（含本帧的后台取数收割 / 派发；关闭时零开销）。
// toast 供查询失败时提示。
void drawCallsDialog(eui::Ui& ui, const eui::Screen& screen,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     LogsState& st,
                     const std::function<void(const std::string& title,
                                              const std::string& msg,
                                              bool isError)>& toast);

} // namespace module::claude
