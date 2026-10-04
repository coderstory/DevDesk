// src/platform/system_theme_posix.cpp
// 兜底实现（Linux 等既非 Windows 也非 macOS 的平台）：恒返回浅色。macOS 不再落到本文件（有
// system_theme_macos.cpp）。GTK_THEME / D-Bus org.freedesktop.appearance 等接入前统一按 false 兜底。
#include "system_theme.h"

namespace platform {

bool prefersDarkTheme() { return false; }

} // namespace platform
