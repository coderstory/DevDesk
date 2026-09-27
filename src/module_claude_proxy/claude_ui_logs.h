// src/module_claude_proxy/claude_ui_logs.h
// R4（P-B5）调用记录弹窗单元：过滤（搜索 + 状态码）/ 分页 / 后台取数
//（审计 [11] + CPX-06）—— 逐字自 claude_module.cpp 搬出。
//
// 取数模型（CPX-06，H2 同一套）：渲染路径零 DB —— 弹窗打开时 compose 每帧调
// refreshCallsCache：①在途结果就绪则搬进快照；②脏 + 无在途 + 过了防抖窗口才
// 发一次读意图（std::async 在后台线程跑 runCallsQuery）。搜索框按键只置脏 +
// 顺延防抖窗口（窗口内多次按键合并成一次派发）。
//
// 测试缝（审计 CPX-06）：派发计数与「实际执行查询的线程 id」两个计数器定义在
// claude_ui_logs.cpp，并按原符号名（extern "C"）导出给无头用例 —— 符号名与
// 语义不变（见 tests/test_modules/test_claude_ui.cpp 的用法）。
#pragma once

#include "claude_ui_state.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>

namespace module::claude {

// 「查看全部 →」：开弹窗 + 置脏（下一帧后台查第一页；渲染路径不再同步查库）
void openCallsDialog(LogsState& st);

// 完整调用记录弹窗（含本帧的后台取数收割 / 派发；关闭时零开销）。
// toast 供查询失败时提示（原 showToast("加载失败", ...)）
void drawCallsDialog(eui::Ui& ui, const eui::Screen& screen,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     LogsState& st,
                     const std::function<void(const std::string& title,
                                              const std::string& msg,
                                              bool isError)>& toast);

} // namespace module::claude
