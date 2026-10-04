// src/platform/tray_icon_macos.cpp
// macOS 托盘图标：22×22 纯黑 + alpha 的 template 蒙版。为什么不用应用图标（DevDesk.icns）：
// tray_bridge.c 拿到 NSImage 后无条件 setTemplate:YES，模板渲染只取 alpha——蓝底方形图标会退化成纯色实心方块。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return "assets/tray-icon-template.png";
}

} // namespace platform::tray
