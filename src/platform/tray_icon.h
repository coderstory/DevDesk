// src/platform/tray_icon.h
// 托盘图标资产路径的平台分发。为什么需要：EUI-NEO 加载两条路各认一种格式——macOS: eui_tray_image →
// NSImage，22×22 template 蒙版；Windows: ExtractIconExW 只认 .ico/.exe 资源位，给 PNG 必返 NULL → 空图标。
// 不放 main.cpp 用 #ifdef：业务代码零平台宏，平台差异全经 src/platform/ 隔离（头平台无关，实现按平台拆）。
#pragma once

#include <string>

namespace platform::tray {

// 返回托盘图标**相对路径**（相对运行目录；命中由 EUI-NEO resolveIconPath 候选链负责）。
//   macOS: "assets/tray-icon-template.png"（22×22 纯黑+alpha template 蒙版，setTemplate:YES 后系统反色）
//   Windows: "assets/tray-icon-win.ico"（16/32/48 32bpp BGRA，品牌蓝底圆角 + 深色 ∞）；其他: ico 不可用时返回空串（= 用应用图标，不崩）
std::string defaultIconPath();

} // namespace platform::tray
