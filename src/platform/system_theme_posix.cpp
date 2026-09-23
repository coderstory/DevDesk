// src/platform/system_theme_posix.cpp
// 系统深浅检测兜底实现（非 Windows，含 macOS/Linux）：恒返回浅色。
// 桌面环境检测（D-Bus org.freedesktop.appearance / GTK_THEME / macOS
// CFPreferences AppleInterfaceStyle）不在当前 Windows 主开发周期范围，
// 统一按接口文档的 false（浅色）兜底；后续需要时在 CMake 里拆出
// *_macos.cpp / 各自实现，业务侧零改动。
#include "system_theme.h"

namespace platform {

bool prefersDarkTheme() { return false; }

} // namespace platform
