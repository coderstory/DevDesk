// src/ui_kit/grid.h
// 多列网格列宽算法（inline 零链接成本）。MCP 工具卡与智能体任务卡共用：
// 宽窗里卡片不能被流式布局拉成横条，所以列宽锁在 [minW, maxW]、列数上限 maxCols。
#pragma once

#include <algorithm>
#include <utility>

namespace ui_kit {

// 列数算法：先取「放得下多少列最小宽」的列数，再用剩余空间把列宽撑开；
// 撑过上限就减列重算，避免宽窗里卡片被拉成横条。
inline std::pair<int, float> gridColumns(float contentW, float gap, float minW,
                                         float maxW, int maxCols) {
    if (contentW <= 0.0f) return {1, minW};
    if (maxCols < 1) maxCols = 1;
    int cols = 1;
    for (int n = 1; n <= maxCols; ++n) {
        if (contentW >= n * minW + (n - 1) * gap) cols = n;
    }
    float cardW = (contentW - gap * (cols - 1)) / static_cast<float>(cols);
    if (cardW > maxW) {
        cols = std::max(1, static_cast<int>((contentW + gap) / (maxW + gap)));
        cardW = (contentW - gap * (cols - 1)) / static_cast<float>(cols);
    }
    return {cols, cardW};
}

}  // namespace ui_kit
