// src/core/window_layout.h
// 启动期主窗口尺寸决策（纯策略：无 EUI-NEO / 平台 / 文件系统依赖，可直接无头单测）。
//
// 背景（2026-09-27 缺陷）：DslAppConfig::windowSize(1600, 1000) 的尺寸直达后端
// glfwCreateWindow，语义是**客户区物理像素**（EUI-NEO 在 Windows GLFW 路径不设
// GLFW_SCALE_TO_MONITOR，进程是 per-monitor DPI aware，见
// ../EUI-NEO/core/window/window_backend.cpp 与 3rd/glfw/src/win32_init.c）。
// 本机实测（1920×1080 @125%，工作区 1920×1020 物理，任务栏 60）：窗口外框
// 1618×1047 落在 (96,33)-(1714,1080) → 客户区底部约 52 物理 px 被任务栏盖住，
// 所有贴底内容（md 笔记树底部「+ 笔记 / + 文件夹」按钮、编辑区状态栏、侧边栏
// 折叠钮）被切掉。
//
// 决策口径：请求尺寸按目标显示器缩放系数换算成物理像素（+ 非客户区外框经验值
// 后）与显示器工作区比较——放得下就用请求尺寸；放不下就**启动即最大化**
//（用户可再拖回来，比"内容被切且用户不知道"温和）。
//
// 分层：本文件属 DevDesk core，不可调用 EUI-NEO 运行时 API（core 只做纯计算）；
// 工作区 / 缩放系数的取值（core::window::getPrimaryMonitorWorkArea /
// windowScaleForPoint）留在 app 层 main.cpp，作为参数传入——与 core::theme 用
// 回调注入平台能力的既有分层一致。
#pragma once

namespace core::window_layout {

// 显示器工作区（物理像素，不含任务栏/Dock，屏幕坐标）。字段语义与
// EUI-NEO core::window::MonitorWorkArea 一致，此处独立定义只为让本头零依赖
struct WorkArea {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 窗口非客户区外框开销（物理像素）：width = 左右边框合计，height = 标题栏 + 上下边框
struct FrameInsets {
    int width = 0;
    int height = 0;
};

// 请求的窗口尺寸与最小尺寸语义（**逻辑单位**，与 .windowSize()/.minWindowSize()
// 同源；缩放系数为 1.0 时逻辑单位 == 物理像素）。min 传 0 表示该方向不约束
struct InitialWindowRequest {
    int logicalWidth = 0;
    int logicalHeight = 0;
    int minLogicalWidth = 0;
    int minLogicalHeight = 0;
};

struct InitialWindowDecision {
    // true = 启动即最大化（换算后的外框放不进工作区）。落成
    // DslAppConfig::maximized() → GLFW_MAXIMIZED 创建期 hint
    bool maximize = false;
    // 实际创建用的**客户区物理像素**。maximize 时后端忽略该值（仅留档/日志）
    int width = 0;
    int height = 0;
};

/**
 * @brief 决策启动期主窗口尺寸：能放下用请求尺寸（换算成物理），放不下则最大化。
 *
 * 规则（顺序即优先级）：
 *  1. 缩放系数非法（<=0 / 非有限）按 1.0 处理，尺寸按整像素四舍五入；
 *  2. 逻辑尺寸先与最小尺寸语义取大（0 = 不约束）——决策永不小于既有最小尺寸；
 *  3. 工作区无效（宽或高 <= 0，无头 / 查询失败 / 无显示器）→ 不做最大化，
 *     原样用请求尺寸（= 修复前的行为，信息不足时不擅自改窗口形态）；
 *  4. 换算后「客户区 + 外框」严格超出工作区（宽或高任一方向）→ maximize；
 *     正好等于工作区算放得下（边界不最大化）。
 *
 * @param request     请求尺寸 + 最小尺寸（逻辑单位）
 * @param workArea    目标显示器工作区（物理像素）
 * @param scale       该显示器 DPI / 96（core::window::windowScaleForPoint）
 * @param frameInsets 非客户区外框开销（物理像素，见 frameInsetsForScale）
 */
InitialWindowDecision decideInitialWindow(const InitialWindowRequest& request,
                                          const WorkArea& workArea,
                                          float scale,
                                          const FrameInsets& frameInsets);

/**
 * @brief 非客户区外框经验值（物理像素），按显示器缩放系数等比放大。
 *
 * 取值依据：本机（Windows 11，125% = 120 DPI）实测有边框可缩放窗口外框
 * 相对客户区多 **+18 宽 / +47 高**；Windows 的标题栏/边框高度随显示器 DPI
 * 等比放大，故按 96 DPI 基准保存（14.4 / 37.6 = 实测值 ÷ 1.25）再乘缩放
 * 系数，向上取整（保守方向：宁可高估外框→多触发一次无害的最大化，也不低估
 * →窗口再次被任务栏吃掉）。125% 下正好还原实测的 18 / 47（自检见单测）。
 * 纯经验值，不追求逐版本精确——决策只关心"是否放得下"这个粗边界。
 */
FrameInsets frameInsetsForScale(float scale);

} // namespace core::window_layout
