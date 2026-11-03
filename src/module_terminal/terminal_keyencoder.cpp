// src/module_terminal/terminal_keyencoder.cpp — xterm 风格键表。
// 修饰变体用 CSI 1;<m> 形（m=1+位和：2=shift 3=alt 4=shift+alt 5=ctrl …）。
#include "terminal_keyencoder.h"

namespace terminal {

namespace {

// CSI 字母键（方向系）修饰变体：\x1b[1;<m><ch>
void csiArrow(char ch, const eui::KeyModifiers& mods, std::string& out) {
    int m = 1;
    if (mods.shift) m += 1;
    if (mods.alt) m += 2;
    if (mods.control) m += 4;
    if (m == 1) {
        out += "\x1b[";
        out += ch;
    } else {
        out += "\x1b[1;" + std::to_string(m) + ch;
    }
}

void csiTilde(int n, const eui::KeyModifiers& mods, std::string& out) {
    int m = 1;
    if (mods.shift) m += 1;
    if (mods.alt) m += 2;
    if (mods.control) m += 4;
    out += "\x1b[";
    out += std::to_string(n);
    if (m != 1) out += ";" + std::to_string(m);
    out += '~';
}

} // namespace

bool encodeKey(eui::InputKey key, const eui::KeyModifiers& mods, std::string& bytes) {
    using IK = eui::InputKey;
    // Cmd 系统快捷键（复制/粘贴/切换）不进终端
    if (mods.super) return false;

    switch (key) {
        case IK::Enter:
        case IK::NumpadEnter:
            bytes += '\r';
            return true;
        case IK::Tab:
            bytes += mods.shift ? "\x1b[Z" : "\t";
            return true;
        case IK::Backspace:
            bytes += '\x7f';
            return true;
        case IK::Escape:
            bytes += '\x1b';
            return true;
        case IK::Delete:
            csiTilde(3, mods, bytes);
            return true;
        case IK::Insert:
            csiTilde(2, mods, bytes);
            return true;
        case IK::Home:
            csiArrow('H', mods, bytes);
            return true;
        case IK::End:
            csiArrow('F', mods, bytes);
            return true;
        case IK::PageUp:
            csiTilde(5, mods, bytes);
            return true;
        case IK::PageDown:
            csiTilde(6, mods, bytes);
            return true;
        case IK::Up:
            csiArrow('A', mods, bytes);
            return true;
        case IK::Down:
            csiArrow('B', mods, bytes);
            return true;
        case IK::Right:
            csiArrow('C', mods, bytes);
            return true;
        case IK::Left:
            csiArrow('D', mods, bytes);
            return true;
        case IK::F1: bytes += "\x1bOP"; return true;
        case IK::F2: bytes += "\x1bOQ"; return true;
        case IK::F3: bytes += "\x1bOR"; return true;
        case IK::F4: bytes += "\x1bOS"; return true;
        case IK::F5: csiTilde(15, mods, bytes); return true;
        case IK::F6: csiTilde(17, mods, bytes); return true;
        case IK::F7: csiTilde(18, mods, bytes); return true;
        case IK::F8: csiTilde(19, mods, bytes); return true;
        case IK::F9: csiTilde(20, mods, bytes); return true;
        case IK::F10: csiTilde(21, mods, bytes); return true;
        case IK::F11: csiTilde(23, mods, bytes); return true;
        case IK::F12: csiTilde(24, mods, bytes); return true;
        default: break;
    }

    // Ctrl+字母/符号 → C0 控制码（Ctrl+I/J/M 与 Tab/Enter/Return 同码，直出等价值）
    if (mods.control && !mods.shift) {
        char base = 0;
        if (key >= IK::A && key <= IK::Z) base = 'A' + static_cast<char>(key) - static_cast<char>(IK::A);
        else if (key == IK::Space) base = '@';
        else if (key == IK::LeftBracket) base = '[';
        else if (key == IK::RightBracket) base = ']';
        else if (key == IK::Backslash) base = '\\';
        else if (key == IK::Slash) base = '/';
        else if (key == IK::Minus) base = '_';
        else if (key == IK::GraveAccent) base = '`';
        if (base != 0) {
            bytes += static_cast<char>(base & 0x1f);
            return true;
        }
    }
    // Alt+字母 → ESC 前缀（xterm meta）
    if (mods.alt && key >= IK::A && key <= IK::Z) {
        bytes += '\x1b';
        bytes += static_cast<char>('a' + static_cast<char>(key) - static_cast<char>(IK::A));
        return true;
    }
    return false; // 可打印字符/IME 文本走 TextInputEvent
}

bool encodeCtrlC(const platform::TerminalKeyConventions& conv, bool hasSelection,
                 std::string& bytes) {
    if (conv.ctrlCIsCopy && hasSelection) return true; // 宿主走复制，不发字节
    bytes.assign(1, '\x03');                           // ETX 中断（Ctrl+C keyencoder 语义）
    return false;
}

} // namespace terminal
