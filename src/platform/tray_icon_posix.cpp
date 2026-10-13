// src/platform/tray_icon_posix.cpp
// Windows/macOS 之外的 POSIX 兜底（Linux 等）。返回空串而非 .ico：libappindicator 期望主题名或 GTK
// 能解的格式，本项目专用 Win .ico 不保证被解析；空串 = 退回应用图标，不崩、不出空图标。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return std::string();
}

} // namespace platform::tray
