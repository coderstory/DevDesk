// tests/test_modules/test_terminal_web_grid.cpp
// need#490 W4 网格帧编解码：run 切分/宽字占位/默认色位/行哈希增量/往返/80×24 最坏体积。
#include "doctest.h"
#include "module_terminal/terminal_grid_widget.h"
#include "module_terminal/web/web_grid.h"

#include <cstdint>
#include <vector>

namespace {

platform::TerminalCell cell(uint32_t cp, uint32_t fg = 0, uint32_t bg = 0,
                            uint16_t attrs = 0, uint8_t width = 1) {
    platform::TerminalCell c;
    c.cp = cp;
    c.fg = fg;
    c.bg = bg;
    c.attrs = attrs;
    c.width = width;
    return c;
}

constexpr uint32_t kRed = (1u << 24) | 1u;   // indexed 红
constexpr uint32_t kBlue = (1u << 24) | 4u;  // indexed 蓝

platform::TerminalFrame frameOf(int rows, int cols, platform::TerminalCell fill) {
    platform::TerminalFrame f;
    f.rows = rows;
    f.cols = cols;
    f.cells.assign(static_cast<size_t>(rows) * cols, fill);
    return f;
}

} // namespace

REGISTER_TEST(webGridRoundtrip,
              "web grid: 全量帧编码→解码往返（头字段/行/runs 一致）", {
    using namespace terminal::web;
    platform::TerminalFrame f = frameOf(2, 4, cell('a'));
    f.cursorRow = 1;
    f.cursorCol = 3;
    WebGridEncoder enc;
    WebFrame web = enc.encode(f, 7, 2, 100, true, false);
    DOCTEST_CHECK(web.header.full);
    DOCTEST_CHECK_EQ(web.header.generation, 7u);
    DOCTEST_CHECK_EQ(web.rows.size(), 2u);

    const std::vector<uint8_t> bytes = encodeWebFrame(web);
    WebFrame back;
    DOCTEST_CHECK(decodeWebFrame(bytes, back));
    DOCTEST_CHECK_EQ(back.header.rows, 2);
    DOCTEST_CHECK_EQ(back.header.cols, 4);
    DOCTEST_CHECK_EQ(back.header.cursorRow, 1);
    DOCTEST_CHECK_EQ(back.header.cursorCol, 3);
    DOCTEST_CHECK_EQ(back.header.scrollOffset, 2u);
    DOCTEST_CHECK_EQ(back.header.scrollbackCount, 100u);
    DOCTEST_CHECK_EQ(back.rows.size(), 2u);
    DOCTEST_CHECK_EQ(back.rows[0].rowIndex, 0);
    DOCTEST_CHECK_EQ(back.rows[1].rowIndex, 1);
    DOCTEST_CHECK_EQ(back.rows[0].runs.size(), 1u);
    DOCTEST_CHECK_EQ(back.rows[0].runs[0].text, std::string("aaaa"));
    DOCTEST_CHECK_EQ(back.rows[0].runs[0].cells, 4);
    return true;
})

REGISTER_TEST(webGridWideChar,
              "web grid: 宽字占位并入同 run（cells=列数，不产重复列/重复字符）", {
    using namespace terminal::web;
    // 杭(2) 占位(0) 州(2) 占位(0)：总列 4，单 run width=2，文本「杭州」
    std::vector<platform::TerminalCell> row = {
        cell(0x676D, 0, 0, 0, 2), cell(0, 0, 0, 0, 0),
        cell(0x5DDE, 0, 0, 0, 2), cell(0, 0, 0, 0, 0)};
    auto runs = encodeRowRuns(row.data(), 4, true);
    DOCTEST_CHECK_EQ(runs.size(), 1u);
    DOCTEST_CHECK_EQ(runs[0].cells, 4);
    DOCTEST_CHECK_EQ(runs[0].width, 2);
    DOCTEST_CHECK_EQ(runs[0].text, std::string("杭州"));
    return true;
})

REGISTER_TEST(webGridRunSplit,
              "web grid: 颜色/属性/宽度任一变化切 run（空白格按空格占列）", {
    using namespace terminal::web;
    std::vector<platform::TerminalCell> row = {
        cell('a', kRed), cell('b', kRed),
        cell(0, 0, 0, 0, 1),               // 真空白格
        cell('c', kBlue, 0, 0x08 /*下划线*/, 1),
    };
    auto runs = encodeRowRuns(row.data(), 4, false);
    DOCTEST_CHECK_EQ(runs.size(), 3u);
    DOCTEST_CHECK_EQ(runs[0].text, std::string("ab"));
    DOCTEST_CHECK_EQ(runs[1].text, std::string(" "));   // 空白格
    DOCTEST_CHECK((runs[2].flags & kRunUnderline) != 0);
    int sum = 0;
    for (const auto& r : runs) sum += r.cells;
    DOCTEST_CHECK_EQ(sum, 4);  // 各 run 列数之和 = 行宽（网页按列定位）
    return true;
})

REGISTER_TEST(webGridDefaultColorBits,
              "web grid: 默认前景/背景位与显式 indexed 色位", {
    using namespace terminal::web;
    const terminal::CellColor defFg = terminal::decodeCellColor(0, true, true);
    DOCTEST_CHECK(defFg.isDefault);
    DOCTEST_CHECK_EQ(defFg.rgba, 0xC9C5BDFFu);  // 暗套 index7
    const terminal::CellColor defBg = terminal::decodeCellColor(0, true, false);
    DOCTEST_CHECK(defBg.isDefault);
    DOCTEST_CHECK_EQ(defBg.rgba, 0u);  // 默认背景透明

    const terminal::CellColor red = terminal::decodeCellColor(kRed, true, true);
    DOCTEST_CHECK(!red.isDefault);
    DOCTEST_CHECK_EQ(red.rgba, 0xF87171FFu);
    // 与 rowRuns 共享 ansiIndexedColor，不复制映射。
    const core::Color table = terminal::ansiIndexedColor(1, true);
    DOCTEST_CHECK(table.r > 0.9f);

    std::vector<platform::TerminalCell> row = {cell('x', 0)};
    auto runs = encodeRowRuns(row.data(), 1, true);
    DOCTEST_CHECK((runs[0].flags & kRunFgDefault) != 0);
    DOCTEST_CHECK((runs[0].flags & kRunBgDefault) != 0);
    return true;
})

REGISTER_TEST(webGridIncremental,
              "web grid: 首帧全量/同帧无变化 0 行/改一行只发该行（行哈希）", {
    using namespace terminal::web;
    platform::TerminalFrame f = frameOf(3, 4, cell('a'));
    WebGridEncoder enc;
    WebFrame first = enc.encode(f, 1, 0, 0, true, false);
    DOCTEST_CHECK(first.header.full);
    DOCTEST_CHECK_EQ(first.rows.size(), 3u);

    WebFrame same = enc.encode(f, 1, 0, 0, true, false);
    DOCTEST_CHECK(!same.header.full);
    DOCTEST_CHECK_EQ(same.rows.size(), 0u);

    f.cells[1 * 4 + 2] = cell('Z', kRed);
    WebFrame delta = enc.encode(f, 2, 0, 0, true, false);
    DOCTEST_CHECK(!delta.header.full);
    DOCTEST_CHECK_EQ(delta.rows.size(), 1u);
    DOCTEST_CHECK_EQ(delta.rows[0].rowIndex, 1);
    DOCTEST_CHECK_EQ(delta.rows[0].runs.size(), 3u);  // aa / Z(红) / a
    return true;
})

REGISTER_TEST(webGridWorstCaseSize,
              "web grid: 80×24 每格异色最坏帧体积（1920 run，17B/run + 行框架）", {
    using namespace terminal::web;
    platform::TerminalFrame f;
    f.rows = 24;
    f.cols = 80;
    f.cells.resize(static_cast<size_t>(f.rows) * f.cols);
    for (size_t i = 0; i < f.cells.size(); i++) {
        f.cells[i] = cell('a', (1u << 24) | static_cast<uint32_t>(i % 16));
    }
    WebGridEncoder enc;
    WebFrame web = enc.encode(f, 0, 0, 0, false, true);
    const size_t bytes = encodeWebFrame(web).size();
    // 24 头 + 2 rowCount + 24×(2 rowIndex + 2 runCount) + 1920×(16 run 头 + 1 文本)
    const size_t expected = 24 + 2 + 24 * 4 + 1920 * 17;
    DOCTEST_CHECK_EQ(bytes, expected);
    DOCTEST_CHECK(bytes < 33000);
    return true;
})

REGISTER_TEST(webGridDecodeRejectsBadFrame,
              "web grid: 坏 magic/版本/截断 → decode false（不越界）", {
    using namespace terminal::web;
    std::vector<uint8_t> bad = {0, 0, 1, 0};
    WebFrame out;
    DOCTEST_CHECK(!decodeWebFrame(bad, out));
    std::vector<uint8_t> truncated = {kGridMagic0, kGridMagic1, kGridVersion, 0, 1};
    DOCTEST_CHECK(!decodeWebFrame(truncated, out));
    return true;
})
