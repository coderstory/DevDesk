// src/platform/tray_icon_posix.cpp
// Windows / macOS 之外的 POSIX 兜底（CMake 的 else() 分支，Linux 等）。
//
// 为什么返回空串而不是 .ico：EUI-NEO 3rd/tray/tray.h 在非 Windows 分支走
// libappindicator（app_indicator_set_icon(indicator, tray->icon)），该 API 期望
// 的是图标**主题名**或 GTK 能解的格式，传本项目专用的 Win .ico 不保证被解析。
// 空串 = 「不传图标」，与 5e92aaf 之前（未传 trayIcon）的行为一致：EUI-NEO 退
// 回应用图标，不崩、不出空图标（空图标只是 Windows 侧 ExtractIconExW 失败
// 时的表现，见 tray_icon_win.cpp）。
//
// 与 file_watcher_stub.cpp 同款定位：DevDesk 实际发布平台只有 Windows 与
// macOS，两端**都已有真实现**（tray_icon_win.cpp / tray_icon_macos.cpp）；
// 本文件只在 Linux 等其它 POSIX 构建上兜底。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return std::string();
}

} // namespace platform::tray
