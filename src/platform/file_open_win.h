// src/platform/file_open_win.h
// Windows 专用声明：平台无关头不放 win-only 符号（否则非 Windows 目标没有该定义，
// 契约只靠注释约束）。只在 Windows 编译单元包含：file_open_win.cpp（定义处，
// 声明与定义由编译器锁一致）与 _WIN32 守卫内的 test_file_open.cpp（唯一引用点）。
#pragma once

#include <string>

namespace platform::file_open {

// 测试缝：Windows UTF-16 → UTF-8 转换，与 openFileFromArgv 的编码链同实现
//（定义见 file_open_win.cpp）。仅 Windows 链接目标含此符号。
std::string wideToUtf8ForTest(const std::wstring& w);

} // namespace platform::file_open
