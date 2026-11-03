// src/module_terminal/terminal_grid_widget.h
// EUI 自绘终端网格（技术卡 §2/§3）：快照→行级 run 合并→text 元素（EUI TextPrimitive
// → flushTextBatch 批量管线）；光标 overlay；回滚滚动映射；主题映射（ANSI 16 色两套表）。
// 纯视图逻辑不持会话；滚动偏移与 resize 量化由 page/module 编排。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <eui/dsl.h>

#include "platform/terminal/terminal_backend.h"

namespace terminal {

// 可复用纯函数（doctest 直测）：cell 行 → 颜色 run 列表（列区间半开）。
struct RowRun {
    int colBegin = 0;
    int colEnd = 0;
    core::Color color;
    bool defaultColor = false; // 核心默认色（fg kind=default）→ 用主题默认前景
};
std::vector<RowRun> rowRuns(const platform::TerminalCell* row, int cols, bool darkTheme);

// ANSI indexed 0-15 → 设计 §3.3 主题色（darkTheme 选套）；非 indexed/rawValue 透传不在此表。
core::Color ansiIndexedColor(int index, bool darkTheme);

struct GridLayout {
    int rows = 0;
    int cols = 0;
    float cellW = 0.0f;
    float cellH = 0.0f;
};

class TerminalGridWidget {
public:
    // 装配可视网格：total = scrollbackCount + frame.rows；scrollOffset=N = 向上回滚 N 行（0=贴底）。
    // 度量按 fontSize 估算（EUI 无等宽度量查询，Phase 1 近似），供光标定位与 resize 量化。
    void compose(eui::Ui& ui, const platform::TerminalFrame& frame, int scrollbackCount,
                 int scrollOffset, float availW, float availH, bool darkTheme,
                 const std::function<bool(int, std::string&)>& scrollbackLine,
                 GridLayout& out);

private:
    float fontSize_ = 13.0f;
};

} // namespace terminal
