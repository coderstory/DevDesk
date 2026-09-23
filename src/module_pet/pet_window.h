// src/module_pet/pet_window.h
// 桌宠子窗口装配（2026-09-22-desktop-pet-design.md §4.1/§4.2）。
// 桌宠不是 Router 页面模块（无 PageId/侧边栏），开窗走 app::openWindow；
// 门控在 main.cpp（enabledModules 含 "pet" 才调 startPetWindow，零执行语义）。
#pragma once

#include <string>
#include <utility>

namespace module::pet {

struct PetWindowConfig {
    std::string skinName;   // 空 = kDefaultSkinName
    int x = -1;             // <0 = 未设置（取主显示器工作区右下角）
    int y = -1;
    bool clickThrough = false;  // 挂机穿透（右键菜单可切换，持久化）
};

// 无边框窗拖拽锚点（拖拽抖动修复，2026-09 真机验证症状 1）：
// DragEvent.delta/totalX 是「窗口内相对坐标」——窗口被拖拽回调自身移动后，
// 后续 delta 的参考系随之漂移（窗口 +d 移动使下一次光标读数偏 -d，
// +10/-5/+10 振荡 = 抖动）；且 delta 单位是帧缓冲像素，DPI≠100% 时与
// setWindowPos 的屏幕坐标差一个 contentScale。锚定「窗口原点 - 光标屏幕位」
//（getWindowPos + getCursorPosition，同为 GLFW 屏幕坐标系、同为实时值），
// 窗口位 = 光标屏幕位 + 锚点，精确 1:1 跟随、无反馈回路、DPI 无关。
struct PetDragAnchor {
    bool valid = false;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

// 拖拽跟随一步（纯函数，test_pet.cpp 断言用）：首次调用（或锚点无效）捕获
// 锚点。cursorX/Y = 窗口内光标原始值（getCursorPosition），windowX/Y = 当前
// 窗口位置（getWindowPos）。返回目标窗口位置（GLFW 屏幕坐标）
std::pair<int, int> petDragMove(PetDragAnchor& anchor, int windowX, int windowY,
                                double cursorX, double cursorY);

// 拖拽结束（释放/取消）清锚点，下次拖拽重新捕获（防止跨会话锚点跳变）
void petDragEnd(PetDragAnchor& anchor);

// 已知坐标钳制进主显示器工作区（纯函数，单测断言用；设计 §4.6，M1 仅主显
// 示器）。workW/workH <= 0（工作区查询失败：无头/早于 glfwInit）原样返回。
// 审计 [12]：落盘路径（savePetPosition/applyPetSkin）与启动路径共用本钳制
// ——负坐标若直接落盘，会被 pet_config 的 -1「未设置」哨兵吞掉，重启后桌宠
// 跳回默认位。已知坐标（>=0）才许进本函数，哨兵判断归调用方
std::pair<int, int> clampPetPosToWorkArea(int x, int y, int workX, int workY,
                                          int workW, int workH, int frameW,
                                          int frameH);

// 打开桌宠窗口（主循环起来前调用；openWindow 入队、首帧消费）。
// 皮肤缺失时自动生成内置占位皮肤；位置钳制进主显示器工作区。
void startPetWindow(const PetWindowConfig& config);

// 换肤（M2）：校验 skinName 是 discoverSkins() 发现到的皮肤后写盘并热切换
//（关旧窗 → 按新皮肤重开，位置沿用当前窗口）；未知名字拒绝不动。
// 窗口未开时只持久化（下次启动生效）
void applyPetSkin(const std::string& skinName);

// 位置重置（M2）：持久化坐标清回 -1（未设置），窗口开着则立刻移回
// 主显示器工作区右下角默认位
void resetPetPosition();

// 鼠标穿透（任务⑥「穿透锁死恢复路径」）：写 runtime 状态 + 即时
// setWindowMousePassthrough + 持久化 pet_config.json。设置页桌宠卡调用
// ——桌宠锁死（clickThrough=true 时右键也点不中自己）后，设置页是唯一
// 恢复入口。窗口未开（桌宠关/隐藏）时只持久化，下次 startPetWindow 生效
void setPetClickThrough(bool enabled);

} // namespace module::pet
