#include "terminal_grid_widget.h"

#include <algorithm>
#include <cmath>
#include <functional>

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

int utf8Len(uint32_t cp) { return cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4)); }

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

void TerminalGridWidget::compose(eui::Ui& ui, const platform::TerminalFrame& frame,
                                 int scrollbackCount, int scrollOffset, float availW,
                                 float availH, bool darkTheme,
                                 const std::function<bool(int, std::string&)>& scrollbackLine,
                                 GridLayout& out) {
    const auto& t = core::theme::current();
    const float cellH = fontSize_ * 1.25f;
    const float cellW = fontSize_ * 0.6f;
    const int viewRows = std::max(1, static_cast<int>(availH / cellH));
    const int viewCols = std::max(1, static_cast<int>(availW / cellW));
    out.rows = viewRows;
    out.cols = viewCols;
    out.cellW = cellW;
    out.cellH = cellH;

    const int totalLines = scrollbackCount + frame.rows;
    int start = totalLines - viewRows - scrollOffset;
    if (start < 0) start = 0;

    std::vector<RowRun> runs;
    std::string display;
    for (int v = 0; v < viewRows; v++) {
        const int lineIdx = start + v;
        const std::string id = "term.grid.row." + std::to_string(v);
        if (lineIdx < scrollbackCount) {
            // 回滚行：纯文本（核心 Line 无 cell 级导出面，v1 用主题次级色）
            std::string text;
            if (scrollbackLine(lineIdx, text) && !text.empty()) {
                ui.text(id)
                    .text(text)
                    .fontFamily("monospace")
                    .fontSize(fontSize_)
                    .color(t.colors.textSecondary)
                    .height(cellH);
            } else {
                ui.text(id)
                    .text(" ")
                    .fontFamily("monospace")
                    .fontSize(fontSize_)
                    .color(t.colors.textSecondary)
                    .height(cellH);
            }
            continue;
        }
        const int frameRow = lineIdx - scrollbackCount;
        if (frameRow >= frame.rows || frame.cells.empty()) {
            ui.text(id)
                .text(" ")
                .fontFamily("monospace")
                .fontSize(fontSize_)
                .color(t.colors.textPrimary)
                .height(cellH);
            continue;
        }
        const platform::TerminalCell* row =
            &frame.cells[static_cast<size_t>(frameRow) * frame.cols];
        runs = rowRuns(row, std::min(viewCols, frame.cols), darkTheme);
        display.clear();
        std::vector<core::TextRun> textRuns;
        int byteOffset = 0;
        int colCursor = 0;
        for (const RowRun& r : runs) {
            for (int gap = colCursor; gap < r.colBegin; gap++) {
                display += ' ';
                byteOffset += 1;
            }
            const int runStartByte = byteOffset;
            for (int c = r.colBegin; c < r.colEnd; c++) {
                appendScalar(display, row[c].cp);
                byteOffset += utf8Len(row[c].cp);
            }
            if (!r.defaultColor && byteOffset > runStartByte) {
                textRuns.push_back(core::TextRun{runStartByte, byteOffset, r.color});
            }
            colCursor = r.colEnd;
        }
        if (display.empty()) display = " ";
        auto el = ui.text(id)
                      .text(display)
                      .fontFamily("monospace")
                      .fontSize(fontSize_)
                      .color(t.colors.textPrimary)
                      .height(cellH);
        if (!textRuns.empty()) el.runs(textRuns);
    }
}

} // namespace terminal
