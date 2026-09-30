// src/platform/tray_icon.h
// 托盘图标资产路径的**平台分发**接口（2026-09-30 Windows 托盘空图标修复）。
//
// 为什么需要它：EUI-NEO 的托盘图标加载两条路各认一种格式——
//   macOS  : eui_tray_image → NSImage，22×22 template 蒙版（纯黑 + alpha）
//   Windows: ExtractIconExW(icon_path)，**只认 .ico/.exe 资源位**，给 PNG
//            必然返回 NULL hIcon → 托盘栏一个空图标。
// 改前 DevDesk 不传路径（空串）也是 NULL，所以这不是回归，是**修复没覆盖
// Windows**：5e92aaf 传了 macOS 专属的 PNG 给 Windows。
//
// 为什么不放 main.cpp 用 #ifdef 分支：业务代码零平台宏是本仓库铁律，平台差异
// 全部经 src/platform/ 隔离（与 file_open / keychain / system_theme 同模式：
// 头文件平台无关，实现 *_macos / *_win / *_posix，CMake 按平台 target_sources 选）。
#pragma once

#include <string>

namespace platform::tray {

// 返回托盘图标的**相对路径**（相对运行目录；实际命中由 EUI-NEO
// resolveIconPath 的候选链负责：cwd/assets/ 与 exeDir/assets/ 两处）。
//   macOS  : "assets/tray-icon-template.png"（22×22 纯黑+alpha template 蒙版，
//            setTemplate:YES 后由系统反色，适配浅/深色菜单栏）
//   Windows: "assets/tray-icon-win.ico"（16/32/48 三档 32bpp BGRA DIB 条目，
//            品牌蓝底圆角方块 + 深色 ∞ 精灵，由母图 DevDesk.icns 派生）
//   其他  : 同 Windows 分支的 ico 不可用时返回空串 —— 调用方（tray 图标加载）
//            对空路径的兜底语义是「用应用图标」，与未传等价，不崩。
std::string defaultIconPath();

} // namespace platform::tray
