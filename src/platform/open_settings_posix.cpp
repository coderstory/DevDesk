// src/platform/open_settings_posix.cpp
// POSIX 兜底：无系统默认应用设置页概念，恒不支持（与 autostart_posix 同款
// 诚实降级惯例；DevDesk 目标平台仅 Windows/macOS，此文件只为非双平台编译面
// 补齐符号）。
#include "open_settings.h"

namespace platform::open_settings {

bool oneClickAvailable() { return false; }

bool openDefaultAppsSettings() { return false; }

} // namespace platform::open_settings
