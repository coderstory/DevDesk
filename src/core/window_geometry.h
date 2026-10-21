// src/core/window_geometry.h
// 窗体记忆的进程级几何账本（UAT6「记住窗体大小」）：app::compose 每帧把主窗客户区
// 逻辑尺寸 + 最大化态记进来，退出时拍快照落盘（settings_config app_config 表）。
// 与 window_layout（纯决策）分工：本文件只管「记录与快照」，不做屏适配——恢复侧的
// 收缩钳制链在 window_layout::restoreInitialWindow。无平台依赖，可无头单测。
#pragma once

namespace core::window_geometry {

struct Snapshot {
    int logicalWidth = 0;    // 还原态客户区宽（逻辑单位）
    int logicalHeight = 0;
    bool maximized = false;  // 最近一帧的最大化态（退出落盘即「上次退出是否最大化」）
    bool valid = false;      // false = 未播种也没见过帧 → 调用方不应落盘（保旧记录）
};

// 启动期播种还原态尺寸（窗口创建前的请求值，main.cpp 在 resolveWindowSizing 里调）：
// 启动即恢复最大化的会话全程没有非最大化帧，退出快照的尺寸靠本播种兜底；此后任何
// 非最大化帧都会覆盖它。非正尺寸忽略
void seedRestoreSize(int logicalWidth, int logicalHeight);

// 每帧记账（app::compose 顶部调，须早于 splash 早退）：非最大化帧更新还原态尺寸；
// 最大化帧只刷新标志——尺寸保留 = 还原态（最大化的铺满值不进账本，否则「记住」的
// 是全屏值，退出最大化后窗口弹回全屏大小）。非正/非有限尺寸忽略
void noteFrame(float logicalWidth, float logicalHeight, bool maximized);

// 当前快照（退出落盘用；尺寸四舍五入成整数）
Snapshot current();

// 测试缝：清空账本（进程级状态，用例间隔离用；生产不得触碰）
void resetForTest();

} // namespace core::window_geometry
