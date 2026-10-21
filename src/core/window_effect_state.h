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

// M-3（UAT2 一审）降级门控的回查缝：实际生效档位由 EUI-NEO 主循环在每次应用后
// 回写（app::activeWindowEffect，eui/app.h——请求 backdrop 材质（UAT6 起磨砂档
// 请求 Mica）而系统 < Win11 22621 时降级 Transparent/None），core 层不可见——
// app 装配段把 WindowEffect→档位串
// 的桥注入这里（main.cpp，同 module_toggle_bridge 的装配段注入模式）；测试可
// 注桩锁降级场景。未注入 = 无降级信息，按请求档取值（headless 单测默认路径，
// 行为同门控前）。线程：仅 UI/主线程读写。
using ActiveTierProvider = std::string (*)();
inline ActiveTierProvider& activeTierProviderSlot() {
    static ActiveTierProvider provider = nullptr;
    return provider;
}
inline void setActiveTierProvider(ActiveTierProvider provider) {
    activeTierProviderSlot() = provider;
}

// 档位 → 整面铺色（侧栏底 / about 页底）alpha：
// frosted=0.15 透 DWM backdrop、translucent=0.72 半透见壁纸、off/未知=1.0 实色。
// 与 settings_config 的 windowEffectAlpha（clearColor 窗口纱）不同层：那是窗口层，这是
// 页面内铺色层；两处取值各自独立，别合并推导。
inline float surfaceAlpha(const std::string& mode) {
    if (mode == "frosted") {
        // M-3 门控：frosted 的 0.15 依赖系统 DWM backdrop 真生效；不支持时
        // 主循环降级回写、active 回查与请求不一致 → 按 active 档取安全值
        // （translucent=0.72 半透 / off=1.0 实色），sidebar/about 回可读底色。
        // off/translucent 不门控：铺色不依赖 backdrop，且两档请求都是 None、
        // active 也区分不了它们。
        if (const ActiveTierProvider provider = activeTierProviderSlot()) {
            const std::string active = provider();
            if (active != "frosted") return surfaceAlpha(active);
        }
        return 0.15f;
    }
    // UAT6 真机两连（第 2 轮调值）：0.5→0.72。0.5 时侧栏/about 铺色叠在窗口纱
    //（0.85）之上，铺色有效不透明度仅 1−0.18×0.5=0.91，亮壁纸（白）下浅 tint
    // 混底变浅灰绿、正文/导航对比度塌（浅色主题次要文字 4.22:1<4.5）。0.72 后
    // 有效不透明度 0.958，两主题正文 ≥4.5:1（护栏：test_settings
    // settings_window_effect_contrast_guard）。
    if (mode == "translucent") return 0.72f;
    return 1.0f;
}

} // namespace core::window_effect_state
