// src/platform/wallpaper_capability_macos.cpp
#include "wallpaper_capability.h"

namespace platform::wallpaper {

// macOS：AVFoundation 原生播放 + CGWindowLevel 桌面层窗口 + 五路让路 watcher
// （锁屏/熄屏/睡眠/电池/全屏），平台层在 src/module_wallpaper/platform/*_macos.mm。
// 与 Windows 版能力对等，因此返回 true（壁纸页可注册）。
bool supported() { return true; }

} // namespace platform::wallpaper
