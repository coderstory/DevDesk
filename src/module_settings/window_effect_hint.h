// src/module_settings/window_effect_hint.h
// 磨砂档的**降级文案**按平台拆分（接口隔离，与仓库既有 *_macos / *_win 同一模式）
//
// 为什么单独成文件：这两句文案里的系统版本要求是**平台事实**——macOS 要 11+
// 系统磨砂、Windows 要 Win11 22H2+。原先它写在 settings_config.cpp 里、用
// `#if defined(__APPLE__)` 二选一，那是业务代码里的平台宏，违反项目铁律
// 「禁止在业务代码里散布 #ifdef _WIN32 / __APPLE__」（平台差异走
// `*_macos.cpp` / `*_win.cpp`，CMake 按平台选源）。
//
// 口径：业务侧只问「本平台的磨砂最低要求文案是什么」，不自己判断平台。
#pragma once

#include <string>

namespace module::settings {

// 磨砂不可用时的降级文案（面向用户的中文）。例如 macOS 侧返回
// 「当前系统不支持磨砂（macOS 11+ 系统磨砂），已回退为半透」。
std::string frostedDegradedHint();

}  // namespace module::settings