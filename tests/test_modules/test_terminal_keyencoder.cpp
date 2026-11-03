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
