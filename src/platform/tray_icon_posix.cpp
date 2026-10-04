// src/platform/tray_icon_posix.cpp
// Windows/macOS 之外的 POSIX 兜底（Linux 等）。为什么返回空串而非 .ico：EUI-NEO 非 Windows 分支走
// libappindicator（app_indicator_set_icon 期望主题名或 GTK 能解的格式），传本项目专用 Win .ico 不保证
// 被解析。空串 = 「不传图标」，EUI-NEO 退回应用图标，不崩、不出空图标。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return std::string();
}

} // namespace platform::tray
