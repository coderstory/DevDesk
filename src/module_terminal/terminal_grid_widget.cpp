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

// ── 框线/块字符程序矢量自绘（bug244，clue103 行业口径）───────────────────────
// alacritty builtin_font / wezterm custom_block_glyphs / kitty box_drawing 同款：
// 框线与块字符不走字体字形，用矩形精确填充——宽度精确 = cellW（宽字 2cellW），
// 杜绝回退字形 advance 抖动导致的表格线错位/断线。单元格归一化坐标 [0,1]²；
// 线宽逻辑 px：单 1 / 重 2；双线 = 两条 1px 线（中心距 2px，缝 1px）。

// 框线臂权重：0 无 / 1 单线 / 2 重线 / 3 双线
struct BoxArms {
    uint8_t u = 0, d = 0, l = 0, r = 0;
};

struct GlyphRect {
    float x, y, w, h;  // 归一化 [0,1]
};

constexpr int kMaxGlyphRects = 8;

struct VectorGlyphPlan {
    BoxArms arms;
    int fillN = 0;
    GlyphRect fills[kMaxGlyphRects];
};

// 框线族（单/重/双三套 + 半行线）。单双混合族（2552 ╒ / 2553 ╓ / 2555 ╕ 等）与
// 圆角（256D-2570）、对角线（2571-2573）、虚线族（2504-2509 / 254C-254F / 257C-257F）
// 不入表：画错比落回字体字形更糟，这些码点走字体路径（仍绝对网格定位，不影响列对齐）。
bool boxArmsPlan(uint32_t cp, BoxArms& a) {
    switch (cp) {
        case 0x2500: a = {0, 0, 1, 1}; return true;  // ─
        case 0x2501: a = {0, 0, 2, 2}; return true;  // ━
        case 0x2502: a = {1, 1, 0, 0}; return true;  // │
        case 0x2503: a = {2, 2, 0, 0}; return true;  // ┃
        case 0x250C: a = {0, 1, 0, 1}; return true;  // ┌
        case 0x250F: a = {0, 2, 0, 2}; return true;  // ┏
        case 0x2510: a = {0, 1, 1, 0}; return true;  // ┐
        case 0x2513: a = {0, 2, 2, 0}; return true;  // ┓
        case 0x2514: a = {1, 0, 0, 1}; return true;  // └
        case 0x2517: a = {2, 0, 0, 2}; return true;  // ┗
        case 0x2518: a = {1, 0, 1, 0}; return true;  // ┘
        case 0x251B: a = {2, 0, 2, 0}; return true;  // ┛
        case 0x251C: a = {1, 1, 0, 1}; return true;  // ├
        case 0x2523: a = {2, 2, 0, 2}; return true;  // ┣
        case 0x2524: a = {1, 1, 1, 0}; return true;  // ┤
        case 0x252B: a = {2, 2, 2, 0}; return true;  // ┫
        case 0x252C: a = {0, 1, 1, 1}; return true;  // ┬
        case 0x2533: a = {0, 2, 2, 2}; return true;  // ┳
        case 0x2534: a = {1, 0, 1, 1}; return true;  // ┴
        case 0x253B: a = {2, 0, 2, 2}; return true;  // ┻
        case 0x253C: a = {1, 1, 1, 1}; return true;  // ┼
        case 0x254B: a = {2, 2, 2, 2}; return true;  // ╋
        case 0x2550: a = {0, 0, 3, 3}; return true;  // ═
        case 0x2551: a = {3, 3, 0, 0}; return true;  // ║
        case 0x2554: a = {0, 3, 0, 3}; return true;  // ╔
        case 0x2557: a = {0, 3, 3, 0}; return true;  // ╗
        case 0x255A: a = {3, 0, 0, 3}; return true;  // ╚
        case 0x255D: a = {3, 0, 3, 0}; return true;  // ╝
        case 0x2560: a = {3, 3, 0, 3}; return true;  // ╠
        case 0x2563: a = {3, 3, 3, 0}; return true;  // ╣
        case 0x2566: a = {0, 3, 3, 3}; return true;  // ╦
        case 0x2569: a = {3, 0, 3, 3}; return true;  // ╩
        case 0x256C: a = {3, 3, 3, 3}; return true;  // ╬
        case 0x2574: a = {0, 0, 1, 0}; return true;  // ╴
        case 0x2575: a = {1, 0, 0, 0}; return true;  // ╵
        case 0x2576: a = {0, 0, 0, 1}; return true;  // ╶
        case 0x2577: a = {0, 1, 0, 0}; return true;  // ╷
        case 0x2578: a = {0, 0, 2, 0}; return true;  // ╸
        case 0x2579: a = {2, 0, 0, 0}; return true;  // ╹
        case 0x257A: a = {0, 0, 0, 2}; return true;  // ╺
        case 0x257B: a = {0, 2, 0, 0}; return true;  // ╻
        default: return false;
    }
}

// 块元素族：八分块（U+2580-2595）+ 四分块（U+2596-259F）+ 阴影块条纹近似
bool boxFillPlan(uint32_t cp, GlyphRect* out, int& n) {
    n = 0;
    if (cp == 0x2588) { out[n++] = {0.0f, 0.0f, 1.0f, 1.0f}; return true; }     // █ 全块
    if (cp == 0x2580) { out[n++] = {0.0f, 0.0f, 1.0f, 0.5f}; return true; }     // ▀ 上半
    if (cp == 0x2594) { out[n++] = {0.0f, 0.0f, 1.0f, 0.125f}; return true; }   // ▔ 上 1/8
    if (cp >= 0x2581 && cp <= 0x2587) {  // ▁▂▃▄▅▆▇ 下 (k+1)/8
        const float f = static_cast<float>(cp - 0x2580) / 8.0f;
        out[n++] = {0.0f, 1.0f - f, 1.0f, f};
        return true;
    }
    if (cp >= 0x2589 && cp <= 0x258F) {  // ▉▊▋▌▍▎▏ 左 (2590−cp)/8
        const float f = static_cast<float>(0x2590 - cp) / 8.0f;
        out[n++] = {0.0f, 0.0f, f, 1.0f};
        return true;
    }
    if (cp == 0x2590) { out[n++] = {0.5f, 0.0f, 0.5f, 1.0f}; return true; }     // ▐ 右半
    if (cp == 0x2595) { out[n++] = {0.875f, 0.0f, 0.125f, 1.0f}; return true; } // ▕ 右 1/8
    if (cp == 0x2591) {  // ░ 浅阴影（横条纹确定性图案，≈25% 覆盖）
        out[n++] = {0.0f, 0.125f, 1.0f, 0.125f};
        out[n++] = {0.0f, 0.625f, 1.0f, 0.125f};
        return true;
    }
    if (cp == 0x2592) {  // ▒ 中阴影（≈50%）
        for (float ry : {0.125f, 0.375f, 0.625f, 0.875f}) out[n++] = {0.0f, ry, 1.0f, 0.125f};
        return true;
    }
    if (cp == 0x2593) {  // ▓ 深阴影（≈75%）
        for (float ry : {0.0f, 0.125f, 0.25f, 0.5f, 0.625f, 0.75f})
            out[n++] = {0.0f, ry, 1.0f, 0.125f};
        return true;
    }
    // 四分块（UL/UR/LL/LR 组合）
    const bool ul = (cp == 0x2598 || cp == 0x2599 || cp == 0x259A || cp == 0x259B || cp == 0x259C);
    const bool ur = (cp == 0x259B || cp == 0x259C || cp == 0x259D || cp == 0x259E || cp == 0x259F);
    const bool ll = (cp == 0x2596 || cp == 0x2599 || cp == 0x259A || cp == 0x259E || cp == 0x259F);
    const bool lr = (cp == 0x2596 || cp == 0x2597 || cp == 0x2599 || cp == 0x259B || cp == 0x259F);
    if (ul || ur || ll || lr) {
        if (ul) out[n++] = {0.0f, 0.0f, 0.5f, 0.5f};
        if (ur) out[n++] = {0.5f, 0.0f, 0.5f, 0.5f};
        if (ll) out[n++] = {0.0f, 0.5f, 0.5f, 0.5f};
        if (lr) out[n++] = {0.5f, 0.5f, 0.5f, 0.5f};
        return true;
    }
    return false;
}

bool decodeVectorGlyph(uint32_t cp, VectorGlyphPlan& plan) {
    if (cp < 0x2500 || cp > 0x259F) return false;
    return boxArmsPlan(cp, plan.arms) || boxFillPlan(cp, plan.fills, plan.fillN);
}

// 解码结果 → 像素矩形（臂条带端点延伸半线宽，保证角/三通接缝无 hairline 缝隙；
// 显式 .color 防 DSL 默认不透明白整面覆盖——clue37 陷阱）
void drawVectorGlyph(eui::Ui& ui, const std::string& id, float x, float y, float cellW,
                     float cellH, uint32_t cp, const core::Color& color) {
    VectorGlyphPlan plan;
    if (!decodeVectorGlyph(cp, plan)) return;
    for (int i = 0; i < plan.fillN; i++) {
        const GlyphRect& q = plan.fills[i];
        ui.rect(id + ".f." + std::to_string(i))
            .position(x + q.x * cellW, y + q.y * cellH)
            .size(q.w * cellW, q.h * cellH)
            .color(color)
            .build();
    }
    const BoxArms& a = plan.arms;
    if (a.u == 0 && a.d == 0 && a.l == 0 && a.r == 0) return;
    const float cx = cellW * 0.5f;
    const float cy = cellH * 0.5f;
    GlyphRect strips[kMaxGlyphRects];
    int n = 0;
    const auto pushV = [&](uint8_t weight) {
        const float half = (weight == 2) ? 1.0f : 0.5f;  // 重 2px / 单 1px
        const float y0 = a.u ? 0.0f : (cy - half) / cellH;
        const float y1 = a.d ? 1.0f : (cy + half) / cellH;
        if (weight == 3) {  // 双线：两条 1px，中心距 2px
            strips[n++] = {(cx - 1.5f) / cellW, y0, 1.0f / cellW, y1 - y0};
            strips[n++] = {(cx + 0.5f) / cellW, y0, 1.0f / cellW, y1 - y0};
        } else {
            strips[n++] = {(cx - half) / cellW, y0, half * 2.0f / cellW, y1 - y0};
        }
    };
    const auto pushH = [&](uint8_t weight) {
        const float half = (weight == 2) ? 1.0f : 0.5f;
        const float x0 = a.l ? 0.0f : (cx - half) / cellW;
        const float x1 = a.r ? 1.0f : (cx + half) / cellW;
        if (weight == 3) {
            strips[n++] = {x0, (cy - 1.5f) / cellH, x1 - x0, 1.0f / cellH};
            strips[n++] = {x0, (cy + 0.5f) / cellH, x1 - x0, 1.0f / cellH};
        } else {
            strips[n++] = {x0, (cy - half) / cellH, x1 - x0, half * 2.0f / cellH};
        }
    };
    const uint8_t vw = std::max(a.u, a.d);
    const uint8_t hw = std::max(a.l, a.r);
    if (vw != 0) pushV(vw);
    if (hw != 0) pushH(hw);
    for (int i = 0; i < n; i++) {
        const GlyphRect& q = strips[i];
        ui.rect(id + ".s." + std::to_string(i))
            .position(x + q.x * cellW, y + q.y * cellH)
            .size(q.w * cellW, q.h * cellH)
            .color(color)
            .build();
    }
}

} // namespace

// 导出分类器（terminal_grid_widget.h 声明；doctest 直测）——单一真相在 decodeVectorGlyph
bool isVectorGlyphCp(uint32_t cp) {
    VectorGlyphPlan plan;
    return decodeVectorGlyph(cp, plan);
}

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
        // 宽字 spacer 占位 width=0（P1-1）：零宽不计列前缀；空 cell 默认 width=1 不受影响。
        // 布局口径（bug244/clue103 ③）：列宽恒 = cell.width × cellW（半角 cellW /
        // CJK 2×cellW / spacer 0），只读核心给定的 cell.width，**不读**字形测量
        // advance——字体回退字形（框线/生僻字）advance ≠ cellW 只影响字形间隙，
        // 永不影响列位与列宽。
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
                        .fontFamily(fontFamily_)
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
                // 绝对网格定位（clue103，bug244 ②）：段 x/宽一律由 cellXOf/cellSpanOf
                // 从列 0 前缀宽和直算（=列号×cellW 乘法口径；宽字 ×2cellW、spacer 零宽
                // 不计）——run 之间不再接力累加 runX，取整/浮点残差不跨 run 累积；
                // fallback 字形 advance 不参与布局（列宽恒 cellW / CJK=2×cellW，
                // 设计 §4.3，cellXOf 口径见下）。
                const auto emitTextSegment = [&](int colBegin, int colEnd,
                                                 const core::Color& segColor) {
                    if (colEnd <= colBegin) return;
                    const float x = cellXOf(cells, drawCols, colBegin, cellW);
                    const float w =
                        std::max(cellSpanOf(cells, drawCols, colBegin, colEnd, cellW), 1.0f);
                    std::string text = rowSliceText(cells, drawCols, colBegin, colEnd);
                    if (text.empty()) return;
                    // CellTrace（bug244 临时诊断，总量≤60 条）：含 CJK/框线行逐段
                    // 对照——x/advance/segW/width 四值区分取整累积/非等宽回退/宽字折算
                    static int cellTraceN = 0;
                    if (cellTraceN < 60 && rowTraceWorthy(cells, drawCols)) {
                        int units = 0;
                        for (int c = colBegin; c < colEnd; c++) {
                            units += std::max(0, static_cast<int>(cells[c].width));
                        }
                        const float adv = core::TextPrimitive::measureTextWidth(
                            text, fontFamily_, fontSize_, 400);
                        char line[224];
                        std::snprintf(line, sizeof(line),
                                      "CellTrace: col=%d x=%.1f cellW=%.2f advance=%.2f "
                                      "runW=%.1f width=%d",
                                      colBegin, static_cast<double>(x),
                                      static_cast<double>(cellW), static_cast<double>(adv),
                                      static_cast<double>(w), units);
                        LOG_INFO(line);
                        cellTraceN++;
                    }
                    ui.text(id + ".run." + std::to_string(colBegin))
                        .position(x, 0.0f)
                        .size(w, cellH)
                        .text(text)
                        .fontFamily(fontFamily_)
                        .fontSize(fontSize_)
                        .color(segColor)
                        .build();
                };
                for (const RowRun& r : rowRuns(cells, drawCols, darkTheme)) {
                    const core::Color segColor =
                        r.defaultColor ? t.colors.textPrimary : r.color;
                    // 框线/块字符段拆分（bug244 ①）：矢量自绘码点从文本 run 中切出，
                    // 逐格矩形精确填充（宽度精确=cellW/2cellW，不走字体字形）
                    int segStart = r.colBegin;
                    for (int c = r.colBegin; c < r.colEnd; c++) {
                        if (!isVectorGlyphCp(cells[c].cp)) continue;
                        emitTextSegment(segStart, c, segColor);
                        drawVectorGlyph(ui, id + ".bx." + std::to_string(c),
                                        cellXOf(cells, drawCols, c, cellW), 0.0f, cellW,
                                        cellH, cells[c].cp, segColor);
                        segStart = c + 1;
                    }
                    emitTextSegment(segStart, r.colEnd, segColor);
                }
            })
            .build();
    }
}

} // namespace terminal
