// src/module_terminal/terminal_search.h
// 池#365：终端历史搜索（最小版）——搜索条 + 全局行号空间匹配 + 命中行居中跳转。
// 行号口径与网格一致（terminal_grid_widget.cpp compose）：全局行 ∈ [0, sb+rows)，
// idx < scrollbackCount = 历史行（老→新），≥ = 屏上行；跳转写 Session.scrollOffset
//（复用池#364 滚动链路，0=贴底、正=向上回滚）。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <eui/dsl.h>

#include "core/theme.h"
#include "platform/terminal/terminal_backend.h"

namespace terminal {

// 页面持有的搜索瞬态。回调闭包跨帧存留——st 必须是页面成员级存活对象。
struct TerminalSearchState {
    bool open = false;
    std::string query;
    int matchLine = -1;       // 当前命中的全局行号（-1 = 无）
    int matchOrdinal = 0;     // 第几处（1 基，供计数位显示）
    int matchCount = 0;       // 总命中数
    bool searched = false;    // 是否执行过一次步进（区分「未搜」与「无匹配」显示）
    // Enter 步进请求：onEnter 只置位，下一帧 compose 消费——扫描用当帧现值
    //（scrollback 增长后 compose 期捕获的 totalLines 已过期，P3-2 同款坑）。
    bool pendingStep = false;
};

// 收集全部命中行（升序）。大小写不敏感（ASCII 折算，避免 locale 依赖）。
// needle 空 / totalLines ≤ 0 → 空。scrollbackLine 取不到的行按空串参与匹配。
std::vector<int> collectMatchLines(
    const std::string& needle, int totalLines, int scrollbackCount,
    const std::function<bool(int, std::string&)>& scrollbackLine,
    const platform::TerminalFrame& frame);

// 命中行滚到视口垂直居中 → 回滚偏移；钳 [0, maxOffset]（maxOffset=scrollbackCount）。
int scrollOffsetToCenter(int matchLine, int totalLines, int viewRows, int maxOffset);

// 右上悬浮搜索条（st.open 才建；输入 + 命中计数 + 关闭钮）。
// onChange 改查询并作废命中态；onEnter 置 pendingStep；onCancel(Esc)=onClose。
void drawTerminalSearchBar(eui::Ui& ui, float pageW, float gridTop,
                           const core::theme::Tokens& t,
                           TerminalSearchState& st,
                           const std::function<void()>& onClose);

} // namespace terminal
