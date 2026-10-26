// src/platform/wallpaper_capability_macos.cpp
#include "wallpaper_capability.h"

namespace platform::wallpaper {

// 第二阶段 ObjC++ 平台层落地前不注册壁纸页（工厂走 nullptr 分支）。
bool supported() { return false; }

} // namespace platform::wallpaper