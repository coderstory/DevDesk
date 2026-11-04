// tests/test_modules/test_terminal_keyencoder.cpp
// 键表驱动断言（技术卡 §9）：方向键修饰变体、Enter=\r、C0 控制码、F 键、SS3、
// 可打印键/修饰键单按/Cmd 系统键不出字节（走 TextInputEvent 通道）。纯函数零 IO。
#include "doctest.h"
#include "module_terminal/terminal_keyencoder.h"

namespace {
eui::KeyModifiers mods(bool ctrl = false, bool alt = false, bool shift = false,
                       bool super = false) {
    eui::KeyModifiers m;
    m.control = ctrl;
    m.alt = alt;
    m.shift = shift;
    m.super = super;
    return m;
}
}

REGISTER_TEST(keyencoderBasicKeys,
              "terminal keyencoder: Enter/Backspace/Escape/Tab 基础键", {
    std::string out;
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Enter, mods(), out));
    DOCTEST_CHECK(out == "\r");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Backspace, mods(), out));
    DOCTEST_CHECK(out == "\x7f");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Escape, mods(), out));
    DOCTEST_CHECK(out == "\x1b");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Tab, mods(), out));
    DOCTEST_CHECK(out == "\t");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Tab, mods(false, false, true), out));
    DOCTEST_CHECK(out == "\x1b[Z");
    return true;
})

REGISTER_TEST(keyencoderArrows,
              "terminal keyencoder: 方向键裸键与 CSI 1;m 修饰变体", {
    std::string out;
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Up, mods(), out));
    DOCTEST_CHECK(out == "\x1b[A");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Down, mods(), out));
    DOCTEST_CHECK(out == "\x1b[B");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Right, mods(), out));
    DOCTEST_CHECK(out == "\x1b[C");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Left, mods(), out));
    DOCTEST_CHECK(out == "\x1b[D");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Up, mods(true, false, false), out));
    DOCTEST_CHECK(out == "\x1b[1;5A");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Left, mods(false, false, true), out));
    DOCTEST_CHECK(out == "\x1b[1;2D");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Right, mods(true, true, false), out));
    DOCTEST_CHECK(out == "\x1b[1;7C");
    return true;
})

REGISTER_TEST(keyencoderTildeKeys,
              "terminal keyencoder: Home/End/PgUp/PgDn/Delete tilde 形", {
    std::string out;
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Home, mods(), out));
    DOCTEST_CHECK(out == "\x1b[H");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::End, mods(), out));
    DOCTEST_CHECK(out == "\x1b[F");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::PageUp, mods(), out));
    DOCTEST_CHECK(out == "\x1b[5~");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::PageDown, mods(), out));
    DOCTEST_CHECK(out == "\x1b[6~");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Delete, mods(true), out));
    DOCTEST_CHECK(out == "\x1b[3;5~");
    return true;
})

REGISTER_TEST(keyencoderFunctionKeys,
              "terminal keyencoder: F1-F4 SS3 与 F5-F12 tilde", {
    std::string out;
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::F1, mods(), out));
    DOCTEST_CHECK(out == "\x1bOP");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::F4, mods(), out));
    DOCTEST_CHECK(out == "\x1bOS");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::F5, mods(), out));
    DOCTEST_CHECK(out == "\x1b[15~");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::F12, mods(), out));
    DOCTEST_CHECK(out == "\x1b[24~");
    return true;
})

REGISTER_TEST(keyencoderControlCombos,
              "terminal keyencoder: Ctrl 组合 → C0", {
    std::string out;
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::C, mods(true), out));
    DOCTEST_CHECK(out == "\x03");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::D, mods(true), out));
    DOCTEST_CHECK(out == "\x04");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::L, mods(true), out));
    DOCTEST_CHECK(out == "\x0c");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::LeftBracket, mods(true), out));
    DOCTEST_CHECK(out == "\x1b");
    return true;
})

REGISTER_TEST(keyencoderSilentKeys,
              "terminal keyencoder: 可打印/修饰单按/Cmd 不出字节", {
    std::string out;
    DOCTEST_CHECK(!terminal::encodeKey(eui::InputKey::A, mods(), out));
    DOCTEST_CHECK(!terminal::encodeKey(eui::InputKey::LeftShift, mods(), out));
    DOCTEST_CHECK(!terminal::encodeKey(eui::InputKey::C, mods(false, false, false, true), out));
    DOCTEST_CHECK(out.empty());
    return true;
})

REGISTER_TEST(keyencoderCtrlCDuality,
              "terminal keyencoder: Ctrl+C 二象性三态（技术卡 v2 决策 4/§5）", {
    std::string out;
    const platform::TerminalKeyConventions win{.ctrlCIsCopy = true, .ctrlVIsPaste = true};
    const platform::TerminalKeyConventions mac{.ctrlCIsCopy = false, .ctrlVIsPaste = false};

    // ① mac 惯例：恒走 keyencoder 语义（\x03），与选区无关（零行为变化锚）
    DOCTEST_CHECK(!terminal::encodeCtrlC(mac, false, false, out));
    DOCTEST_CHECK(out == "\x03");
    DOCTEST_CHECK(!terminal::encodeCtrlC(mac, true, false, out));
    DOCTEST_CHECK(out == "\x03");

    // ② win 惯例 + 无选区 → \x03 中断
    DOCTEST_CHECK(!terminal::encodeCtrlC(win, false, false, out));
    DOCTEST_CHECK(out == "\x03");

    // ③ win 惯例 + 有选区 → 复制分派（bytes 保持上次值不变即"未写"，宿主走剪贴板）
    DOCTEST_CHECK(terminal::encodeCtrlC(win, true, false, out));
    DOCTEST_CHECK(out == "\x03");

    // ④ P2-2：Shift 恒中断（Ctrl+Shift+C 原终端语义，mac 零变化锚），选区不改变分派
    DOCTEST_CHECK(!terminal::encodeCtrlC(win, true, true, out));
    DOCTEST_CHECK(out == "\x03");
    DOCTEST_CHECK(!terminal::encodeCtrlC(win, false, true, out));
    DOCTEST_CHECK(out == "\x03");
    return true;
})
