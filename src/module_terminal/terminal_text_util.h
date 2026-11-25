// src/module_terminal/terminal_text_util.h
// 终端文本 → 列坐标的小工具（M2 Task 2 双击选词用）。纯函数、可单测。
// 列口径：ASCII 1 列、3/4 字节 UTF-8（CJK/emoji）2 列、2 字节族 1 列——与 findMatchColumns 一致。
#pragma once

#include <string>
#include <utility>

namespace terminal {

// 字符是否算「词」内：字母/数字/下划线/非 ASCII（CJK 等）
bool isWordByte(unsigned char c);

// 第 col 列所在词的列区间 [begin, end)；空白/标点处返回 [col, col+1) 单列。
std::pair<int, int> wordBoundsAt(const std::string& text, int col);

// 文本总显示列宽
int textDisplayColumns(const std::string& text);

} // namespace terminal
