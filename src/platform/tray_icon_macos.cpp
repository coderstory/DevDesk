// src/platform/tray_icon_macos.cpp
// macOS 托盘图标：22×22 纯黑 + alpha 的 template 蒙版（2026-09-30 Bug A 修，
// commit 5e92aaf 起经 main.cpp 直传，本次只是把该路径搬进平台接口，行为不变）。
//
// 为什么不直接用应用图标（DevDesk.icns）：tray_bridge.c 拿到 NSImage 后会
// 无条件 setTemplate:YES，模板渲染只取 alpha 蒙版 —— 蓝底圆角方形应用图标
// 会退化成纯色实心方块（这正是 5e92aaf 之前托盘图标不可辨识的原因）。
// template 蒙版由系统按菜单栏明暗自动反色。
#include "platform/tray_icon.h"

namespace platform::tray {

std::string defaultIconPath() {
    return "assets/tray-icon-template.png";
}

} // namespace platform::tray
