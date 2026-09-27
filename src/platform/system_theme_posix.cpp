// src/platform/system_theme_posix.cpp
// 系统深浅检测兜底实现（Linux 等既非 Windows 也非 macOS 的平台）：恒返回浅色。
// macOS 已不再落到本文件——它有自己的实现 system_theme_macos.cpp（CFPreferences
// 读 AppleInterfaceStyle），由 CMake 的 elseif(APPLE) 分支编译。本文件只服务
// 「没有系统外观查询通道」的平台：GTK_THEME / D-Bus org.freedesktop.appearance
// 等接入前，统一按接口文档的 false（浅色）兜底；后续接 Linux 桌面环境检测时在
// 本文件内实现，业务侧零改动。
#include "system_theme.h"

namespace platform {

bool prefersDarkTheme() { return false; }

} // namespace platform
