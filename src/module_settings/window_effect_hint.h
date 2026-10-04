// src/module_settings/window_effect_hint.h
// 磨砂档的降级文案按平台拆分（接口隔离，同仓库 *_macos / *_win 模式）。
//
// 文案里的系统版本要求是平台事实，故不能写在业务代码的 #ifdef 里；平台差异走
// *_macos.cpp / *_win.cpp，CMake 按平台选源。业务侧只问「本平台的降级文案是什么」。
#pragma once

#include <string>

namespace module::settings {

// 磨砂不可用时的降级文案（面向用户的中文，如「当前系统不支持磨砂（macOS 11+…）」）。
std::string frostedDegradedHint();

}  // namespace module::settings