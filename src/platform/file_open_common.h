// src/platform/file_open_common.h
// file_open 族共用的纯逻辑声明（审计 CP-09 去重）。本头零平台宏、零平台头：
//   - shellSingleQuote：macOS(/usr/bin/open) 与其它 POSIX(xdg-open) 两份逐字
//     相同的转义实现下沉，定义在 file_open_common.cpp（全平台编译的 TU）
//   - platform::detail::utf8ToWide / wideToUtf8：UTF-8 ↔ UTF-16 编码链，
//     file_open_win.cpp 与 file_watcher_win.cpp 共用。因实现依赖
//     <windows.h>，定义放在仅 Windows 编译的 file_open_win.cpp——与
//     file_open_win.h 的 wideToUtf8ForTest「仅 Windows 提供定义、引用点留
//     _WIN32 守卫」同一先例，避免给全平台 TU 塞平台宏（项目核心约束 5）。
#pragma once

#include <string>

namespace platform::file_open {

// shell 单引号转义（' 拆成 '\''）：路径含空格/引号不破坏命令边界
std::string shellSingleQuote(const std::string& s);

} // namespace platform::file_open

namespace platform::detail {

// UTF-16 → UTF-8（Windows API，无 locale 依赖）；wstr == nullptr 或
// wlen <= 0 → 空串。定义在 file_open_win.cpp（仅 Windows 编译）
std::string wideToUtf8(const wchar_t* wstr, int wlen);

// UTF-8 → UTF-16（Windows 路径 API 用）；空串 / 非法输入 → 空 wstring。
// 定义在 file_open_win.cpp（仅 Windows 编译）
std::wstring utf8ToWide(const std::string& s);

} // namespace platform::detail
