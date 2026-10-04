// src/platform/tray_icon_macos.cpp
// macOS 托盘图标：22×22 纯黑 + alpha 的 template 蒙版。不能换成应用图标：tray_bridge.c 拿到 NSImage 后
// 无条件 setTemplate:YES，模板渲染只取 alpha——彩色图标会退化成纯色实心方块。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return "assets/tray-icon-template.png";
}

} // namespace platform::tray
