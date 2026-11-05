// src/module_terminal/terminal_grid_widget.cpp
#include "terminal_grid_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "core/logging.h"
#include "core/render/text.h"
#include "core/theme.h"

namespace terminal {

using core::Color;

namespace {

Color hexRGB(uint32_t hex, float alpha = 1.0f) {
    return Color{
        static_cast<float>((hex >> 16) & 0xff) / 255.0f,
        static_cast<float>((hex >> 8) & 0xff) / 255.0f,
        static_cast<float>(hex & 0xff) / 255.0f,
        alpha};
}

void appendScalar(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

} // namespace

// ANSI 16 色（设计 §3.3 亮/暗两套表）：0-7 基础，8-15 bright。
core::Color ansiIndexedColor(int index, bool darkTheme) {
    static const Color kDark[16] = {
        hexRGB(0x3E3B39), hexRGB(0xF87171), hexRGB(0x34D399), hexRGB(0xFBBF24),
        hexRGB(0x60A5FA), hexRGB(0xDE8FB4), hexRGB(0x22D3EE), hexRGB(0xC9C5BD),
        hexRGB(0x9A948A), hexRGB(0xFCA5A5), hexRGB(0x6EE7B7), hexRGB(0xFDE68A),
        hexRGB(0x93C5FD), hexRGB(0xF0ABFC), hexRGB(0x67E8F9), hexRGB(0xFFFFFF),
    };
    static const Color kLight[16] = {
        hexRGB(0x1F2937), hexRGB(0xB91C1C), hexRGB(0x157E4E), hexRGB(0xA16207),
        hexRGB(0x2563EB), hexRGB(0xA21CAF), hexRGB(0x0E7490), hexRGB(0x4B5563),
        hexRGB(0x6B7280), hexRGB(0xEF4444), hexRGB(0x22C55E), hexRGB(0xD97706),
        hexRGB(0x3B82F6), hexRGB(0xC026D3), hexRGB(0x0891B2), hexRGB(0x111827),
    };
    if (index < 0 || index > 15) return darkTheme ? kDark[7] : kLight[7];
    return darkTheme ? kDark[index] : kLight[index];
}

std::vector<RowRun> rowRuns(const platform::TerminalCell* row, int cols, bool darkTheme) {
    std::vector<RowRun> runs;
    constexpr uint32_t kKindShift = 24;
    int i = 0;
    while (i < cols) {
        const platform::TerminalCell& c = row[i];
        if (c.cp == 0) {
            i++;
            continue;
        }
        const uint32_t fg = c.fg;
        const int start = i;
        Color color = ansiIndexedColor(7, darkTheme);
        bool defaultColor = (fg >> kKindShift) == 0;
        if (!defaultColor) {
            uint32_t kind = fg >> kKindShift;
            if (kind == 1) {
                color = ansiIndexedColor(static_cast<int>(fg & 0xff), darkTheme);
            } else if (kind == 2) {
                color = Color{static_cast<float>((fg >> 16) & 0xff) / 255.0f,
                              static_cast<float>((fg >> 8) & 0xff) / 255.0f,
                              static_cast<float>(fg & 0xff) / 255.0f, 1.0f};
            }
        }
        int j = i;
        while (j < cols && row[j].cp != 0 && row[j].fg == fg) j++;
        RowRun r;
        r.colBegin = start;
        r.colEnd = j;
        r.color = color;
        r.defaultColor = defaultColor;
        runs.push_back(r);
        i = j;
    }
    return runs;
}

std::string rowSliceText(const platform::TerminalCell* row, int cols, int colBegin, int colEnd) {
    std::string out;
    const int end = std::min(colEnd, cols);
    for (int c = colBegin; c < end; c++) {
        appendScalar(out, row[c].cp);
    }
    return out;
}

// CellTrace（bug244 临时诊断）：该行是否含 CJK/框线等非 ASCII（>0x7F）
bool rowTraceWorthy(const platform::TerminalCell* row, int cols) {
    for (int c = 0; c < cols; c++) {
        if (row[c].cp > 0x7F) return true;
    }
    return false;
}

float cellXOf(const platform::TerminalCell* row, int cols, int col, float cellW) {
    if (row == nullptr) {
        // 无行数据（空帧/滚动态兜底）：按半角折算，不贴 0
        return col > 0 ? static_cast<float>(col) * cellW : 0.0f;
    }
    if (col < 0) return 0.0f;
    if (cols <= 0 || col >= cols) return static_cast<float>(col) * cellW;
    float x = 0.0f;
    for (int c = 0; c < col; c++) {
        // 宽字 spacer 占位 width=0（P1-1）：零宽不计列前缀；空 cell 默认 width=1 不受影响
        x += std::max(0, static_cast<int>(row[c].width)) * cellW;
    }
    return x;
}

float cellSpanOf(const platform::TerminalCell* row, int cols, int colBegin, int colEnd,
                 float cellW) {
    if (colEnd <= colBegin) return 0.0f;
    if (row == nullptr || cols <= 0 || colBegin >= cols) {
        return static_cast<float>(colEnd - colBegin) * cellW;
    }
    float w = 0.0f;
    const int end = std::min(colEnd, cols);
    for (int c = colBegin; c < end; c++) {
        w += std::max(0, static_cast<int>(row[c].width)) * cellW; // spacer 零宽（P1-1）
    }
    if (colEnd > cols) w += static_cast<float>(colEnd - cols) * cellW;
    return w;
}

void TerminalGridWidget::compose(eui::Ui& ui, const platform::TerminalFrame& frame,
                                 int scrollbackCount, int scrollOffset, float availW,
                                 float availH, bool darkTheme,
                                 const TerminalCellMetrics& metrics,
                                 const std::function<bool(int, std::string&)>& scrollbackLine,
                                 GridLayout& out) {
    const auto& t = core::theme::current();
    // 真实度量（need241）；查询失败 metrics 内含估算回落值，布局恒可用
    const float cellH = metrics.cellH;
    const float cellW = metrics.cellW;
    const int viewRows = std::max(1, static_cast<int>(availH / cellH));
    const int viewCols = std::max(1, static_cast<int>(availW / cellW));
    out.rows = viewRows;
    out.cols = viewCols;
    out.cellW = cellW;
    out.cellH = cellH;

    const int totalLines = scrollbackCount + frame.rows;
    int start = totalLines - viewRows - scrollOffset;
    if (start < 0) start = 0;

    for (int v = 0; v < viewRows; v++) {
        const int lineIdx = start + v;
        const std::string id = "term.grid.row." + std::to_string(v);
        // 行容器：固定高 stack，run 元素按列前缀宽绝对定位（列漂移根除）
        ui.stack(id)
            .size(availW, cellH)
            .content([&] {
                if (lineIdx < scrollbackCount) {
                    // 回滚行：纯文本整行（核心 Line 无 cell 级导出面，v1 同口径）
                    std::string text;
                    if (!scrollbackLine(lineIdx, text) || text.empty()) text = " ";
                    ui.text(id + ".sb")
                        .size(availW, cellH)
                        .text(text)
                        .fontFamily("monospace")
                        .fontSize(fontSize_)
                        .color(t.colors.textSecondary)
                        .build();
                    return;
                }
                const int frameRow = lineIdx - scrollbackCount;
                if (frameRow >= frame.rows || frame.cells.empty()) return;
                const platform::TerminalCell* cells =
                    &frame.cells[static_cast<size_t>(frameRow) * frame.cols];
                const int drawCols = std::min(viewCols, frame.cols);
                // 列前缀宽（宽字 2 格；逐 run 从列起点画——CJK 回退字形 advance
                // ≠2cellW 只影响字形间隙，不产生列漂移，设计 §4.3）
                float runX = 0.0f;
                int col = 0;
                for (const RowRun& r : rowRuns(cells, drawCols, darkTheme)) {
                    // run 起点对齐：补上 run 前空 cell 的列宽（spacer width=0 不计）
                    float x = runX;
                    for (int gap = col; gap < r.colBegin; gap++) {
                        x += std::max(0, static_cast<int>(cells[gap].width)) * cellW;
                    }
                    float w = 0.0f;
                    for (int c = r.colBegin; c < r.colEnd; c++) {
                        w += std::max(0, static_cast<int>(cells[c].width)) * cellW;
                    }
                    std::string text = rowSliceText(cells, drawCols, r.colBegin, r.colEnd);
                    // CellTrace（bug244 临时诊断，总量≤60 条）：含 CJK/框线行逐 run
                    // 对照——x/advance/runW/width 四值区分取整累积/非等宽回退/宽字折算
                    static int cellTraceN = 0;
                    if (cellTraceN < 60 && rowTraceWorthy(cells, drawCols)) {
                        int units = 0;
                        for (int c = r.colBegin; c < r.colEnd; c++) {
                            units += std::max(0, static_cast<int>(cells[c].width));
                        }
                        const float adv = core::TextPrimitive::measureTextWidth(
                            text, "monospace", fontSize_, 400);
                        char line[224];
                        std::snprintf(line, sizeof(line),
                                      "CellTrace: col=%d x=%.1f cellW=%.2f advance=%.2f "
                                      "runW=%.1f width=%d",
                                      r.colBegin, static_cast<double>(x),
                                      static_cast<double>(cellW), static_cast<double>(adv),
                                      static_cast<double>(w), units);
                        LOG_INFO(line);
                        cellTraceN++;
                    }
                    if (!text.empty()) {
                        ui.text(id + ".run." + std::to_string(r.colBegin))
                            .position(x, 0.0f)
                            .size(std::max(w, 1.0f), cellH)
                            .text(text)
                            .fontFamily("monospace")
                            .fontSize(fontSize_)
                            .color(r.defaultColor ? t.colors.textPrimary : r.color)
                            .build();
                    }
                    col = r.colEnd;
                    runX = x + w;
                }
            })
            .build();
    }
}

} // namespace terminal
