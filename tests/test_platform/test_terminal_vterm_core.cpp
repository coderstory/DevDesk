// tests/test_platform/test_terminal_vterm_core.cpp
// platform::VtermCore（libvterm→TerminalFrame 转译层）——need:127 Phase 1 本机 doctest。
// 用例面 = 技术卡 v2 §10-1：录制 VT 字节流喂 feed()，断言 cell/光标/attrs/颜色三档
// 锁值/alt-screen/reflow 尺寸/scrollback 差一/damage→generation/bracketed 跟踪/DSR 应答。
// 颜色锁值口径 = Corta Color.rawValue（kind 高 8 位+payload 低 24 位）；
// attrs 锁值口径 = Corta CellAttributes.rawValue 位面。
#include "doctest.h"

#include <cstdint>
#include <string>

#include "platform/terminal/terminal_vterm_core.h"

namespace {

constexpr uint32_t kIndexed(uint8_t idx) { return 0x01000000u | idx; }
constexpr uint32_t kRgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0x02000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}
constexpr uint16_t kAttrBold = 1 << 0;
constexpr uint16_t kAttrItalic = 1 << 2;
constexpr uint16_t kAttrUnderline = 1 << 3;
constexpr uint16_t kAttrReverse = 1 << 5;

void feed(platform::VtermCore& core, const std::string& s) {
    core.feed(reinterpret_cast<const uint8_t*>(s.data()), int32_t(s.size()), nullptr);
}

const platform::TerminalCell& cellAt(const platform::TerminalFrame& f, int row, int col) {
    return f.cells[size_t(row) * f.cols + col];
}

std::string rowText(const platform::TerminalFrame& f, int row) {
    std::string out;
    for (int c = 0; c < f.cols && cellAt(f, row, c).cp != 0; ++c)
        out.push_back(char(cellAt(f, row, c).cp));
    return out;
}

} // namespace

REGISTER_TEST(vtermPlainText, "vterm core: plain text 与光标推进", {
    platform::VtermCore core(5, 20);
    feed(core, "hello");
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(f.rows, 5);
    DOCTEST_CHECK_EQ(f.cols, 20);
    DOCTEST_CHECK_EQ(f.cursorRow, 0);
    DOCTEST_CHECK_EQ(f.cursorCol, 5);
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).cp, uint32_t('h'));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 4).cp, uint32_t('o'));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 5).cp, uint32_t(0));
    // 未设色 = 默认色 kind 0；空 cell 默认宽 1
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).fg, uint32_t(0));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).bg, uint32_t(0));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).width, uint8_t(1));
    DOCTEST_CHECK(core.pendingGeneration() > 0);
    return true;
})

REGISTER_TEST(vtermCrLf, "vterm core: CRLF 换行落行", {
    platform::VtermCore core(5, 20);
    feed(core, "ab\r\ncd");
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).cp, uint32_t('a'));
    DOCTEST_CHECK_EQ(cellAt(f, 1, 0).cp, uint32_t('c'));
    DOCTEST_CHECK_EQ(cellAt(f, 1, 1).cp, uint32_t('d'));
    DOCTEST_CHECK_EQ(f.cursorRow, 1);
    return true;
})

REGISTER_TEST(vtermCjkWide, "vterm core: CJK 宽字占 2 cell（width==2 + spacer）", {
    platform::VtermCore core(5, 20);
    feed(core, "\xe4\xbd\xa0\xe5\xa5\xbdx"); // 你好x
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).cp, uint32_t(0x4f60)); // 你
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).width, uint8_t(2));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 1).cp, uint32_t(0));      // 宽字 spacer
    DOCTEST_CHECK_EQ(cellAt(f, 0, 2).cp, uint32_t(0x597d)); // 好
    DOCTEST_CHECK_EQ(cellAt(f, 0, 4).cp, uint32_t('x'));    // x 落第 4 列
    return true;
})

REGISTER_TEST(vtermSgrThreeColorKinds, "vterm core: SGR 前景三档色锁值（16 色/256/RGB）", {
    platform::VtermCore core(5, 20);
    feed(core, "\x1b[31mR"); // ANSI 16 色 red=1
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).fg, kIndexed(1));

    feed(core, "\x1b[0m\x1b[38;5;196mX"); // 256 色索引 196
    f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 1).fg, kIndexed(196));

    feed(core, "\x1b[0m\x1b[38;2;255;128;0mO"); // RGB 直色
    f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 2).fg, kRgb(255, 128, 0));
    return true;
})

REGISTER_TEST(vtermSgrReset, "vterm core: SGR reset 恢复默认色", {
    platform::VtermCore core(5, 20);
    feed(core, "\x1b[31mR\x1b[0mN");
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).fg, kIndexed(1));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 1).fg, uint32_t(0)); // default kind
    DOCTEST_CHECK_EQ(cellAt(f, 0, 1).bg, uint32_t(0));
    return true;
})

REGISTER_TEST(vtermAttrsBits, "vterm core: attrs 位面锁值（bold/italic/underline/reverse + reset）", {
    platform::VtermCore core(5, 20);
    feed(core, "\x1b[1;3;4;7mB");
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).attrs,
                     uint16_t(kAttrBold | kAttrItalic | kAttrUnderline | kAttrReverse));
    feed(core, "\x1b[;m");
    f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 1).attrs, uint16_t(0));
    return true;
})

REGISTER_TEST(vtermCursorCup, "vterm core: CUP 光标定位（1 基转 0 基）", {
    platform::VtermCore core(5, 20);
    feed(core, "\x1b[3;5H");
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(f.cursorRow, 2);
    DOCTEST_CHECK_EQ(f.cursorCol, 4);
    return true;
})

REGISTER_TEST(vtermAltScreen, "vterm core: alt-screen 进出不崩 + 主屏覆盖", {
    platform::VtermCore core(5, 20);
    feed(core, "\x1b[?1049h");
    feed(core, "alt");
    feed(core, "\x1b[?1049l");
    feed(core, "main");
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).cp, uint32_t('m'));
    return true;
})

REGISTER_TEST(vtermScrollbackDiffOne, "vterm core: scrollback 差一语义（corta #22 口径）", {
    platform::VtermCore core(5, 20); // 5 行视口
    std::string script;
    for (int i = 1; i <= 12; ++i) script += "L" + std::to_string(i) + "\r\n";
    feed(core, script);
    DOCTEST_CHECK_EQ(core.scrollbackCount(), 8); // 12-5+1（尾部空行推送差一）
    std::string line;
    DOCTEST_REQUIRE(core.scrollbackLine(0, line));
    DOCTEST_CHECK_EQ(line, "L1");
    DOCTEST_REQUIRE(core.scrollbackLine(7, line));
    DOCTEST_CHECK_EQ(line, "L8");
    DOCTEST_CHECK_FALSE(core.scrollbackLine(8, line));
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).cp, uint32_t('L')); // 视口顶=L9
    DOCTEST_CHECK_EQ(cellAt(f, 0, 1).cp, uint32_t('9'));
    return true;
})

REGISTER_TEST(vtermResize, "vterm core: resize 行列重排 + 全量重转译", {
    platform::VtermCore core(5, 20);
    feed(core, "keep");
    int genBefore = core.pendingGeneration();
    core.resize(3, 8);
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(f.rows, 3);
    DOCTEST_CHECK_EQ(f.cols, 8);
    DOCTEST_CHECK_EQ(f.cells.size(), size_t(24));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 0).cp, uint32_t('k'));
    DOCTEST_CHECK_EQ(cellAt(f, 0, 3).cp, uint32_t('p'));
    DOCTEST_CHECK(core.pendingGeneration() >= genBefore);
    return true;
})

REGISTER_TEST(vtermReflowWiden, "vterm core: reflow 加宽不丢锚点文本", {
    platform::VtermCore core(4, 10);
    feed(core, "aaaaaaaaaabbbbbbbbbbcccc"); // 10 列下折行
    core.resize(4, 30);                     // 加宽：reflow 合并 continuation 行
    auto f = core.snapshot();
    DOCTEST_CHECK_EQ(f.cols, 30);
    std::string row0 = rowText(f, 0);
    DOCTEST_CHECK(row0.size() >= 10);
    DOCTEST_CHECK(row0.compare(0, 10, "aaaaaaaaaa") == 0);
    DOCTEST_CHECK(row0.find('b') != std::string::npos);
    DOCTEST_CHECK(row0.find('c') != std::string::npos);
    return true;
})

REGISTER_TEST(vtermBracketedPaste, "vterm core: bracketed paste 模式跟踪（含跨 chunk）", {
    platform::VtermCore core(5, 20);
    DOCTEST_CHECK_FALSE(core.bracketedPasteEnabled());
    feed(core, "\x1b[?2004h");
    DOCTEST_CHECK(core.bracketedPasteEnabled());
    feed(core, "\x1b[?200");
    feed(core, "4l");
    DOCTEST_CHECK_FALSE(core.bracketedPasteEnabled());
    return true;
})

REGISTER_TEST(vtermDsrReply, "vterm core: DSR 光标查询经 output 回调应答", {
    platform::VtermCore core(5, 20);
    feed(core, "\x1b[2;3H");
    std::string reply;
    core.feed(reinterpret_cast<const uint8_t*>("\x1b[6n"), 4, &reply);
    DOCTEST_CHECK(reply.size() > 0);
    DOCTEST_CHECK(reply.compare(0, 2, "\x1b[") == 0);
    DOCTEST_CHECK(reply.find("2;3R") != std::string::npos);
    return true;
})

REGISTER_TEST(vtermSelectionText, "vterm core: selectionText 双端闭区间 + 行尾 trim", {
    platform::VtermCore core(5, 20);
    feed(core, "abcdef");
    std::string out;
    DOCTEST_REQUIRE(core.selectionText(0, 1, 0, 3, out));
    DOCTEST_CHECK_EQ(out, "bcd");
    DOCTEST_REQUIRE(core.selectionText(0, 0, 0, 19, out)); // 超出内容列→trim 尾空白
    DOCTEST_CHECK_EQ(out, "abcdef");
    return true;
})
