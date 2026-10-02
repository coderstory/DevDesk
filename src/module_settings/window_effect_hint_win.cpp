// src/module_settings/window_effect_hint_win.cpp
// Windows：磨砂要求 Win11 22H2+（Acrylic 的系统版本下限，macos-three-features
// 设计 §5.3）。Win10 上的 DWM 合成不保证 Acrylic 的 blur 效果，强行请求会得到
// 一块实色——所以文案里写明版本要求，而不是笼统说「不支持」。
#include "window_effect_hint.h"

namespace module::settings {

std::string frostedDegradedHint() {
    return "当前系统不支持磨砂（需 Windows 11 22H2+），已回退为半透";
}

}  // namespace module::settings