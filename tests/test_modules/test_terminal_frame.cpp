// tests/test_modules/test_terminal_frame.cpp
// TerminalFrame→run 合并纯逻辑 + ANSI→eui::Color 映射表断言（技术卡 §9，设计 §3.3 两套表）。
#include "doctest.h"
#include "module_terminal/terminal_grid_widget.h"

#include <vector>

namespace {
platform::TerminalCell cell(uint32_t cp, uint32_t fg) {
    platform::TerminalCell c;
    c.cp = cp;
    c.fg = fg;
    return c;
}
bool colorNear(const core::Color& a, float r, float g, float b) {
    auto near = [](float x, float y) { return (x - y) < 0.001f && (y - x) < 0.001f; };
    return near(a.r, r) && near(a.g, g) && near(a.b, b);
}
} // namespace

REGISTER_TEST(frameSingleDefaultRun,
              "terminal frame: 纯默认色行 → 单 default run", {
    std::vector<platform::TerminalCell> row(8);
    for (int i = 0; i < 4; i++) row[i] = cell('a', 0);
    auto runs = terminal::rowRuns(row.data(), 8, true);
    DOCTEST_CHECK(runs.size() == 1);
    DOCTEST_CHECK(runs[0].colBegin == 0);
    DOCTEST_CHECK(runs[0].colEnd == 4);
    DOCTEST_CHECK(runs[0].defaultColor);
    return true;
})

REGISTER_TEST(frameRunBoundaries,
              "terminal frame: 前景色变化与空 cell 切 run 边界", {
    uint32_t red = (1u << 24) | 1u;    // indexed 红=1
    uint32_t blue = (1u << 24) | 4u;   // indexed 蓝=4
    std::vector<platform::TerminalCell> row = {
        cell('a', red), cell('b', red), platform::TerminalCell{}, cell('c', blue),
        cell('d', blue)};
    auto runs = terminal::rowRuns(row.data(), 5, false);
    DOCTEST_CHECK(runs.size() == 2);
    DOCTEST_CHECK(runs[0].colBegin == 0);
    DOCTEST_CHECK(runs[0].colEnd == 2);
    DOCTEST_CHECK(!runs[0].defaultColor);
    DOCTEST_CHECK(runs[1].colBegin == 3);
    DOCTEST_CHECK(runs[1].colEnd == 5);
    return true;
})

REGISTER_TEST(frameAnsiPalette,
              "terminal frame: ANSI indexed 0-15 → 设计 §3.3 两套取值", {
    // 暗：red #F87171；bright green #6EE7B7；亮：red #B91C1C；blue #2563EB
    auto darkRed = terminal::ansiIndexedColor(1, true);
    DOCTEST_CHECK(colorNear(darkRed, 0xF8 / 255.0f, 0x71 / 255.0f, 0x71 / 255.0f));
    auto darkGreenBright = terminal::ansiIndexedColor(10, true);
    DOCTEST_CHECK(colorNear(darkGreenBright, 0x6E / 255.0f, 0xE7 / 255.0f, 0xB7 / 255.0f));
    auto lightRed = terminal::ansiIndexedColor(1, false);
    DOCTEST_CHECK(colorNear(lightRed, 0xB9 / 255.0f, 0x1C / 255.0f, 0x1C / 255.0f));
    auto lightBlue = terminal::ansiIndexedColor(4, false);
    DOCTEST_CHECK(colorNear(lightBlue, 0x25 / 255.0f, 0x63 / 255.0f, 0xEB / 255.0f));
    // 越界回落 index7（暗套 #C9C5BD）
    auto fallback = terminal::ansiIndexedColor(99, true);
    DOCTEST_CHECK(colorNear(fallback, 0xC9 / 255.0f, 0xC5 / 255.0f, 0xBD / 255.0f));
    return true;
})

REGISTER_TEST(frameRgbPassthrough,
              "terminal frame: rgb 直通色按 r<<16|g<<8|b 解码", {
    uint32_t purple = (2u << 24) | (0x12u << 16) | (0x34u << 8) | 0x56u;
    std::vector<platform::TerminalCell> row = {cell('x', purple)};
    auto runs = terminal::rowRuns(row.data(), 1, true);
    DOCTEST_CHECK(runs.size() == 1);
    DOCTEST_CHECK(colorNear(runs[0].color, 0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f));
    return true;
})

REGISTER_TEST(frameEmptyRow,
              "terminal frame: 全空行 → 无 run", {
    std::vector<platform::TerminalCell> row(8);
    auto runs = terminal::rowRuns(row.data(), 8, true);
    DOCTEST_CHECK(runs.empty());
    return true;
})
