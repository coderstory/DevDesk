// src/module_settings/window_effect_hint_macos.cpp
// macOS：磨砂要求 macOS 11+ **系统磨砂**（不是窗口模糊 API——后者对无边框
// 透明窗不可靠，macos-three-features 设计 §5.3 记录了实测结论）。
#include "window_effect_hint.h"

namespace module::settings {

std::string frostedDegradedHint() {
    return "当前系统不支持磨砂（macOS 11+ 系统磨砂），已回退为半透";
}

}  // namespace module::settings