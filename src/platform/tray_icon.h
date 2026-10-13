// src/platform/tray_icon.h
// 托盘图标资产路径的平台分发：macOS 走 NSImage 的 22×22 template 蒙版；Windows ExtractIconExW 只认
// .ico/.exe 资源位、给 PNG 必返 NULL。头平台无关，实现按平台拆，业务零平台宏。
#pragma once

#include <string>

namespace platform::tray {

// 返回托盘图标**相对路径**（相对运行目录；命中由 EUI-NEO resolveIconPath 候选链负责）。
//   macOS: "assets/tray-icon-template.png"（22×22 纯黑+alpha template 蒙版，setTemplate:YES 后系统反色）
//   Windows: "assets/tray-icon-win.ico"（16/32/48 32bpp BGRA，品牌蓝底圆角 + 深色 ∞）；其他: ico 不可用时返回空串（= 用应用图标，不崩）
std::string defaultIconPath();

} // namespace platform::tray
