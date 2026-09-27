// src/platform/file_open_win.h
// Windows 专用声明（平台无关头 file_open.h 不放 win-only 符号——审计：此前该
// 测试缝声明在平台无关头里，声明与定义分居两处、非 Windows 目标根本没有该符号，
// 契约被打破，只靠注释约束引用点）。
//
// 本头文件只在 Windows 编译单元里被包含：
//   ① src/platform/file_open_win.cpp —— 定义处（包含它，声明与定义由编译器锁一致）
//   ② tests/test_platform/test_file_open.cpp —— 唯一引用点（在 _WIN32 守卫内包含）
#pragma once

#include <string>

namespace platform::file_open {

// 测试缝（审计 [17]）：Windows UTF-16 → UTF-8 转换，与 openFileFromArgv 的
// 编码链同实现（定义见 file_open_win.cpp）。仅 Windows 链接目标含此符号。
std::string wideToUtf8ForTest(const std::wstring& w);

} // namespace platform::file_open
