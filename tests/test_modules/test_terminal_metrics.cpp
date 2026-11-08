// tests/test_modules/test_terminal_metrics.cpp
// cell 度量纯函数单测（need241 字体改造）：取整/clamp/估算回落 + 列几何。
#include "doctest.h"
#include "module_terminal/terminal_grid_widget.h"
#include "module_terminal/terminal_metrics.h"
#include "platform/terminal/terminal_backend.h"

REGISTER_TEST(terminalMetricsFromAdvance,
              "terminal metrics: advance 折算取整 + 行高 clamp[1.20,1.35]×fs + 估算回落", {
    using terminal::metricsFromAdvance;
    using terminal::TerminalCellMetrics;

    // 正常：advance 7.8→cellW 8；行高 16.9 在 [15.6,17.55] 内→17
    const TerminalCellMetrics m1 = metricsFromAdvance(7.8f, 16.9f, 13.0f);
    DOCTEST_CHECK(m1.valid);
    DOCTEST_CHECK(m1.cellW == 8.0f);
    DOCTEST_CHECK(m1.cellH == 17.0f);

    // clamp 下界：行高 14 → 15.6 → 16
    const TerminalCellMetrics m2 = metricsFromAdvance(7.0f, 14.0f, 13.0f);
    DOCTEST_CHECK(m2.valid);
    DOCTEST_CHECK(m2.cellH == 16.0f);

    // clamp 上界：行高 20 → 17.55 → 18
    const TerminalCellMetrics m3 = metricsFromAdvance(7.0f, 20.0f, 13.0f);
    DOCTEST_CHECK(m3.valid);
    DOCTEST_CHECK(m3.cellH == 18.0f);

    // 查询失败回落：advance 0 → 旧估算（13×0.6=8 / 13×1.25=16），valid=false 标注
    const TerminalCellMetrics m4 = metricsFromAdvance(0.0f, 16.9f, 13.0f);
    DOCTEST_CHECK(!m4.valid);
    DOCTEST_CHECK(m4.cellW == 8.0f);
    DOCTEST_CHECK(m4.cellH == 16.0f);

    // 非法 fontSize → 全零拒绝
    const TerminalCellMetrics m5 = metricsFromAdvance(7.8f, 16.9f, 0.0f);
    DOCTEST_CHECK(!m5.valid);
    DOCTEST_CHECK(m5.cellW == 0.0f);
    return true;
})

REGISTER_TEST(terminalCellGeometry,
              "terminal metrics: cellXOf/cellSpanOf 前缀宽和（宽字 ×2cellW）", {
    namespace pt = platform;
    // 行：半角 半角 宽字 半角（widths 1 1 2 1），cellW=8
    pt::TerminalCell row[4];
    row[0].width = 1; row[1].width = 1; row[2].width = 2; row[3].width = 1;
    constexpr float kCellW = 8.0f;

    DOCTEST_CHECK(terminal::cellXOf(row, 4, 0, kCellW) == 0.0f);
    DOCTEST_CHECK(terminal::cellXOf(row, 4, 2, kCellW) == 16.0f);  // 宽字前两格
    DOCTEST_CHECK(terminal::cellXOf(row, 4, 3, kCellW) == 32.0f);  // 宽字占 2 格
    // 越界行（nullptr）：按半角折算（选区在滚动态/空帧下的兜底口径）
    DOCTEST_CHECK(terminal::cellXOf(nullptr, 4, 3, kCellW) == 24.0f);

    DOCTEST_CHECK(terminal::cellSpanOf(row, 4, 2, 4, kCellW) == 24.0f);  // 宽字+半角
    DOCTEST_CHECK(terminal::cellSpanOf(row, 4, 0, 2, kCellW) == 16.0f);  // 两个半角
    DOCTEST_CHECK(terminal::cellSpanOf(row, 4, 3, 3, kCellW) == 0.0f);   // 空区间

    // P1-1/P2-4：含真实 gap cell（宽字右侧占位 cp=0、width=0，translateCell 规范化
    // 产物）的行——[A][中][spacer][B][C]：列前缀 1+2+0+1=4 格，col4 恰在 4*cellW
    //（旧口径 spacer 记 1 格 → 5*cellW，整行右移）
    pt::TerminalCell wide[5];
    wide[0].cp = 'A';    wide[0].width = 1;
    wide[1].cp = 0x4E2D; wide[1].width = 2;
    wide[2].cp = 0;      wide[2].width = 0;
    wide[3].cp = 'B';    wide[3].width = 1;
    wide[4].cp = 'C';    wide[4].width = 1;
    DOCTEST_CHECK(terminal::cellXOf(wide, 5, 3, kCellW) == 3.0f * kCellW);
    DOCTEST_CHECK(terminal::cellXOf(wide, 5, 4, kCellW) == 4.0f * kCellW);
    DOCTEST_CHECK(terminal::cellSpanOf(wide, 5, 2, 3, kCellW) == 0.0f);  // 占位区间零宽
    DOCTEST_CHECK(terminal::cellSpanOf(wide, 5, 3, 5, kCellW) == 2.0f * kCellW);
    return true;
})

REGISTER_TEST(terminalBoxGlyphGridGeometry,
              "terminal metrics: 框线行绝对网格（bug244/clue103）——第 4 列 x==4*cellW + 矢量码点分类", {
    namespace pt = platform;
    // 行：┌ ─ 中 spacer ┐（框线码点 + 宽字混合；widths 1 1 2 0 1），cellW=8
    pt::TerminalCell row[5];
    row[0].cp = 0x250C; row[0].width = 1;  // ┌
    row[1].cp = 0x2500; row[1].width = 1;  // ─
    row[2].cp = 0x4E2D; row[2].width = 2;  // 中
    row[3].cp = 0;      row[3].width = 0;  // 宽字 spacer 占位
    row[4].cp = 0x2510; row[4].width = 1;  // ┐
    constexpr float kCellW = 8.0f;
    // 框线码点不改变布局口径：列位仍=前缀宽和×cellW，第 4 列恰在 4*cellW
    DOCTEST_CHECK_EQ(terminal::cellXOf(row, 5, 2, kCellW), 2.0f * kCellW);
    DOCTEST_CHECK_EQ(terminal::cellXOf(row, 5, 4, kCellW), 4.0f * kCellW);
    DOCTEST_CHECK_EQ(terminal::cellSpanOf(row, 5, 0, 2, kCellW), 2.0f * kCellW);

    // 矢量自绘分类器：单/重/双框线 + 半行线 + 八分/四分/阴影块 = true
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x2500));  // ─
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x250C));  // ┌
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x254B));  // ╋ 重
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x256C));  // ╬ 双
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x2575));  // ╵ 半行
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x2588));  // █
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x2591));  // ░
    DOCTEST_CHECK(terminal::isVectorGlyphCp(0x259E));  // ▞ 四分
    // 不入表（画错比字体更糟）：圆角/对角线/虚线/单双混合/普通字符
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x256D));  // ╭ 圆角
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x2571));  // ╱ 对角线
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x2504));  // ┄ 虚线
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x2552));  // ╒ 单双混合
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x257C));  // ╼ 重虚线
    DOCTEST_CHECK(!terminal::isVectorGlyphCp('A'));
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x4E2D));
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x24FF));
    DOCTEST_CHECK(!terminal::isVectorGlyphCp(0x25A0));
    return true;
})

REGISTER_TEST(terminalRowSliceText,
              "terminal metrics: rowSliceText 按 cell 区间拼 UTF-8（run 文本组装）", {
    namespace pt = platform;
    pt::TerminalCell row[3];
    row[0].cp = 'A';
    row[1].cp = 0x4E2D;  // 中（UTF-8 3 字节 E4 B8 AD）
    row[2].cp = 0;
    const std::string s = terminal::rowSliceText(row, 3, 0, 2);
    DOCTEST_CHECK(s.size() == 4);
    DOCTEST_CHECK(static_cast<unsigned char>(s[0]) == 'A');
    DOCTEST_CHECK(static_cast<unsigned char>(s[1]) == 0xE4);
    DOCTEST_CHECK(static_cast<unsigned char>(s[2]) == 0xB8);
    DOCTEST_CHECK(static_cast<unsigned char>(s[3]) == 0xAD);
    DOCTEST_CHECK(terminal::rowSliceText(row, 3, 2, 2).empty());
    return true;
})

// 池 bug248：宽字右侧占位格（cp=0）不得在文本里留下 NUL 字节。
// 背景：terminal_vterm_core.cpp:172 把 libvterm 的 (uint32_t)-1 规范化成 cp=0 spacer，
// 旧 rowSliceText 逐 cell appendScalar(0) → cp<0x80 分支写入 '\0'，于是 "杭州" 实际
// 存成 "杭\0州\0"。UI 渲染看不出异常（字形照画），但 std::string::find("杭州")
// 永不命中——UI 自动化断言与任何文本提取都因此失效。此用例钉住「宽字行拼出的文本
// 必须是干净的连续 UTF-8」。
REGISTER_TEST(terminalRowSliceTextWideSpacer,
              "terminal metrics: rowSliceText 跳过宽字占位格（无内嵌 NUL）", {
    namespace pt = platform;
    // 「杭州」在 cells 里的真实形态：杭 / spacer / 州 / spacer
    pt::TerminalCell row[4];
    row[0].cp = 0x676D;  // 杭
    row[0].width = 2;
    row[1].cp = 0;       // 宽字右侧占位格
    row[1].width = 0;
    row[2].cp = 0x5DDE;  // 州
    row[2].width = 2;
    row[3].cp = 0;
    row[3].width = 0;
    const std::string s = terminal::rowSliceText(row, 4, 0, 4);
    DOCTEST_CHECK(s.find('\0') == std::string::npos);   // 无内嵌 NUL
    DOCTEST_CHECK(s.find("杭州") != std::string::npos);  // 连续子串可命中
    DOCTEST_CHECK(s.size() == 6);                        // 两个 CJK 各 3 字节
    return true;
})
