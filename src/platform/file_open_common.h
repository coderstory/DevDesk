// src/platform/file_open_common.h
// file_open 族共用纯逻辑声明。本头零平台宏、零平台头。
// shellSingleQuote 定义在 file_open_common.cpp（全平台编译）。
// platform::detail::utf8ToWide / wideToUtf8 依赖 <windows.h>，定义放仅 Windows 编译的
// file_open_win.cpp，引用点留 _WIN32 守卫——同 file_open_win.h 的 wideToUtf8ForTest 先例。
#pragma once

#include <string>

namespace platform::file_open {

// shell 单引号转义（' → '\''）：路径含空格/引号不破坏命令边界
std::string shellSingleQuote(const std::string& s);

} // namespace platform::file_open

namespace platform::detail {

// UTF-16 → UTF-8（Windows API，无 locale 依赖）；wstr==nullptr 或 wlen<=0 → 空串。
std::string wideToUtf8(const wchar_t* wstr, int wlen);

// UTF-8 → UTF-16（Windows 路径 API 用）；空串/非法输入 → 空 wstring。
std::wstring utf8ToWide(const std::string& s);

} // namespace platform::detail
