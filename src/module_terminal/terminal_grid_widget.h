// src/module_terminal/terminal_grid_widget.h
// EUI 自绘终端网格（need241 v2 度量改造）：快照→行级 run 合并→**逐 run 绝对定位**
// text 元素（x=列前缀宽和，宽字 ×2cellW）——弃整行空格拼接（列漂移根因，bug244）；
// 度量来自 TerminalCellMetrics（真实 advance/行高，弃估算）。
// 纯视图逻辑不持会话；滚动偏移与 resize 量化由 page/module 编排。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <eui/dsl.h>

#include "platform/terminal/terminal_backend.h"
#include "terminal_metrics.h"

namespace terminal {

// 可复用纯函数（doctest 直测）：cell 行 → 颜色 run 列表（列区间半开）。
struct RowRun {
    int colBegin = 0;
    int colEnd = 0;
    core::Color color;
    bool defaultColor = false; // 核心默认色（fg kind=default）→ 用主题默认前景
};
std::vector<RowRun> rowRuns(const platform::TerminalCell* row, int cols, bool darkTheme);

// 可复用纯函数（doctest 直测）：行内一段 cell 区间拼 UTF-8（run 文本组装）。
std::string rowSliceText(const platform::TerminalCell* row, int cols, int colBegin, int colEnd);

// 可复用纯函数（doctest 直测）：列 → 像素 x（前缀宽和；宽字 ×2cellW）；越界行按半角折算。
float cellXOf(const platform::TerminalCell* row, int cols, int col, float cellW);
// cell 区间 [colBegin, colEnd) 的像素宽（同上口径）。
float cellSpanOf(const platform::TerminalCell* row, int cols, int colBegin, int colEnd, float cellW);

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
    // metrics=真实 cell 度量（terminalCellMetrics(fs) 传入）；out 回填量化行列数与 cell 尺寸。
    void compose(eui::Ui& ui, const platform::TerminalFrame& frame, int scrollbackCount,
                 int scrollOffset, float availW, float availH, bool darkTheme,
                 const TerminalCellMetrics& metrics,
                 const std::function<bool(int, std::string&)>& scrollbackLine,
                 GridLayout& out);

private:
    float fontSize_ = 13.0f;
};

} // namespace terminal
