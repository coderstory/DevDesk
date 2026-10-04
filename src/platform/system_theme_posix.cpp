// src/platform/system_theme_posix.cpp
// 兜底实现（Linux 等平台）：恒返回浅色；GTK_THEME / D-Bus 等接入前统一按 false 兜底。
#include "system_theme.h"

namespace platform {

bool prefersDarkTheme() { return false; }

} // namespace platform
