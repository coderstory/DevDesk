// src/core/window_effect_state.h
// 当前窗口效果档位的进程内单值 + 整面铺色 alpha 映射（header-only，无新增 .cpp）。
// 为什么放 core：写者 = module_settings::applyWindowVisuals（settings_config.cpp），
// 读者 = app 层 sidebar（app_sidebar.cpp）与 module_about（about_module.cpp）的
// 磨砂底色按档带 alpha——module_* 之间不可互相依赖（模块 CMakeLists 隔离铁律），
// core 是双方公共下游。档位字符串与 module_settings/settings_config.h 的
// kWindowEffectOff/Translucent/Frosted 同值（test_settings 用后者喂本头锁同值防漂移）。
// 线程：仅 UI/主线程读写（applyWindowVisuals 与 compose 同线程），无锁。
#pragma once

#include <string>

namespace core::window_effect_state {

// 档位单值存取：applyWindowVisuals 在启动恢复 / 切档 / 主题切换写入；默认 off（实色）。
// inline 函数内 static 跨 TU 单实例（C++ 语义保证）。
inline std::string& storage() {
    static std::string mode = "off";
    return mode;
}

inline void setCurrent(const std::string& mode) { storage() = mode; }

inline const std::string& current() { return storage(); }

// 档位 → 整面铺色（侧栏底 / about 页底）alpha：
// frosted=0.15 透 Acrylic backdrop、translucent=0.5 半透见壁纸、off/未知=1.0 实色。
// 与 settings_config 的 windowEffectAlpha（clearColor 窗口纱）不同层：那是窗口层，这是
// 页面内铺色层；两处取值各自独立，别合并推导。
inline float surfaceAlpha(const std::string& mode) {
    if (mode == "frosted") return 0.15f;
    if (mode == "translucent") return 0.5f;
    return 1.0f;
}

} // namespace core::window_effect_state
