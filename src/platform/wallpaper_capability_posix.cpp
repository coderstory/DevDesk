// src/platform/wallpaper_capability_posix.cpp
#include "wallpaper_capability.h"

namespace platform::wallpaper {

// 非 Windows/macOS 兜底：无壁纸宿主能力，不注册壁纸页（工厂走 nullptr 分支）。
bool supported() { return false; }

} // namespace platform::wallpaper