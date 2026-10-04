// src/platform/system_theme.h
// 平台无关接口：系统是否偏好深色（「跟随系统主题」）。本头零平台宏，实现拆 *_win.cpp / *_posix.cpp /
// *_macos.cpp，CMake 按平台选。依赖方向：本接口连 core 都不需要（纯值返回）；core::theme 不得反向
// 依赖 platform——「auto→浅/深」的解析在 module 层完成。
#pragma once

namespace platform {

// true = 系统当前为深色；false = 浅色或无法检测。
//   Windows: 注册表 ...\Themes\Personalize\AppsUseLightTheme（DWORD，0=深色；读取失败按浅色）
//   macOS:   CFPreferences 读 NSGlobalDomain 的 AppleInterfaceStyle == "Dark"（浅色系统不写该键、无 GUI 会话按浅色）
//   Linux 等: 恒 false（浅色兜底）
bool prefersDarkTheme();

} // namespace platform
