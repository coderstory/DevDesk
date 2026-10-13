// src/module_settings/window_effect_hint_win.cpp
// Windows 磨砂要求 Win11 22H2+（Acrylic 下限）；Win10 强求会得到一块实色，
// 故文案写明版本要求而非笼统「不支持」。
#include "window_effect_hint.h"

namespace module::settings {

std::string frostedDegradedHint() {
    return "当前系统不支持磨砂（需 Windows 11 22H2+），已回退为半透";
}

}  // namespace module::settings