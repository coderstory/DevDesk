// src/platform/system_theme.h
// 平台无关接口：系统是否偏好深色主题（spec §8.5「跟随系统主题」）。
// 接口隔离约定（CLAUDE.md「平台适配代码必须接口隔离」）：本头文件零平台宏，
// 实现按平台拆文件 *_win.cpp / *_posix.cpp，CMake 按平台选择编译。
//
// 依赖方向：platform 可依赖 core；本接口连 core 都不需要（纯值返回），
// core::theme 不得反向依赖 platform——「auto→浅/深」的解析由 module 层
// （module_settings 经 resolveThemeDark(mode, prefersDarkTheme())）完成。
#pragma once

namespace platform {

// true = 系统当前为深色；false = 浅色或无法检测。
//   Windows: 注册表 HKCU\Software\Microsoft\Windows\CurrentVersion
//            \Themes\Personalize\AppsUseLightTheme（DWORD，0=深色；读取失败按浅色）
//            → system_theme_win.cpp
//   macOS:   CFPreferences 读 NSGlobalDomain 的 AppleInterfaceStyle，等于 "Dark"
//            为深色（浅色系统不写该键、无 GUI 会话一律按浅色）→ system_theme_macos.cpp
//   Linux 等: 恒 false（浅色兜底）→ system_theme_posix.cpp
bool prefersDarkTheme();

} // namespace platform
