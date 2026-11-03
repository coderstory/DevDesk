// src/module_terminal/terminal_keyencoder.h
// KeyEvent→VT/转义字节表（技术卡 §5.4）。平台中立纯逻辑、表驱动可单测。
// 分工：可打印字符/IME 组合文本走 TextInputEvent（clue #4 同款分工），本表只编码
// 非文本键（方向/Ctrl 组合/Home-End/PgUp-Dn/F1-F12/Enter/Backspace/Delete/Tab/Esc）。
#pragma once

#include <string>

#include <eui/types.h>

#include "platform/terminal/terminal_backend.h"

namespace terminal {

// 编码一个按键事件；返回 false = 不产生字节（可打印键、修饰键单独按下、Cmd 系统快捷键）。
// bytes 追加编码结果。
bool encodeKey(eui::InputKey key, const eui::KeyModifiers& mods, std::string& bytes);

// Ctrl+C 二象性分派（技术卡 v2 决策 4/§5，纯函数可单测）：
// win 惯例（conv.ctrlCIsCopy）且选区非空 → 返回 true、bytes 不写（宿主走复制）；
// 其余 → 返回 false、bytes=\x03（ETX 中断，keyencoder 同语义）。
bool encodeCtrlC(const platform::TerminalKeyConventions& conv, bool hasSelection,
                 std::string& bytes);

} // namespace terminal
