// src/platform/file_open_win.h
// Windows 专用声明：win-only 符号不放平台无关头（非 Windows 目标没有该定义）。
// 只在 Windows 编译单元包含：file_open_win.cpp（定义处）与 _WIN32 守卫内的 test_file_open.cpp。
#pragma once

#include <string>

namespace platform::file_open {

// 测试缝：Windows UTF-16 → UTF-8 转换，与 openFileFromArgv 的编码链同实现。
std::string wideToUtf8ForTest(const std::wstring& w);

} // namespace platform::file_open
