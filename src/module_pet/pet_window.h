// src/module_pet/pet_window.h
// 桌宠子窗口装配（2026-09-22-desktop-pet-design.md §4.1/§4.2）。
// 桌宠不是 Router 页面模块（无 PageId/侧边栏），开窗走 app::openWindow；
// 门控在 main.cpp（enabledModules 含 "pet" 才调 startPetWindow，零执行语义）。
#pragma once

#include <string>

namespace module::pet {

struct PetWindowConfig {
    std::string skinName;   // 空 = kDefaultSkinName
    int x = -1;             // <0 = 未设置（取主显示器工作区右下角）
    int y = -1;
    bool clickThrough = false;  // 挂机穿透（右键菜单可切换，持久化）
};

// 打开桌宠窗口（主循环起来前调用；openWindow 入队、首帧消费）。
// 皮肤缺失时自动生成内置占位皮肤；位置钳制进主显示器工作区。
void startPetWindow(const PetWindowConfig& config);

} // namespace module::pet
