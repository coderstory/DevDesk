// src/core/window_layout.h
// 启动期主窗口尺寸决策（纯策略：无 EUI-NEO / 平台 / 文件系统依赖，可直接无头单测）。
//
// 背景（2026-09-27 缺陷）：DslAppConfig::windowSize(1600, 1000) 的尺寸直达后端
// glfwCreateWindow，语义是**客户区窗口单位**——Windows = 物理像素（EUI-NEO 在
// Windows GLFW 路径不设 GLFW_SCALE_TO_MONITOR，进程是 per-monitor DPI aware，
// 见 ../EUI-NEO/core/window/window_backend.cpp 与 3rd/glfw/src/win32_init.c）；
// macOS = 点（3rd/glfw/src/cocoa_window.m 经 [NSWindow setContentSize:] 落点，
// 且 core::window::windowScaleForPoint 在 macOS **恒返 1.0**——Retina 的 2.0 是
// contentScale/backing scale，不是「逻辑→窗口单位」的换算，乘上去会双倍放大）。
// 本机实测（Windows 11，1920×1080 @125%，工作区 1920×1020 物理，任务栏 60）：
// 窗口外框 1618×1047 落在 (96,33)-(1714,1080) → 客户区底部约 52 物理 px 被
// 任务栏盖住，所有贴底内容（md 笔记树底部「+ 笔记 / + 文件夹」按钮、编辑区
// 状态栏、侧边栏折叠钮）被切掉。
//
// 决策口径：请求尺寸按目标显示器缩放系数换算成窗口单位（+ 非客户区外框经验值
// 后）与显示器工作区比较——放得下就用请求尺寸；放不下就**启动即最大化**
//（用户可再拖回来，比"内容被切且用户不知道"温和）。最小尺寸走**同一条**换算
//（minWindowSizeForWorkArea）后再与工作区取 min，两个平台才各自自洽：
// Windows 落实 845 逻辑的设计下限、macOS 保证最小窗仍收得进工作区。
//
// 分层：本文件属 DevDesk core，不可调用 EUI-NEO 运行时 API（core 只做纯计算）；
// 工作区 / 缩放系数的取值（core::window::getPrimaryMonitorWorkArea /
// windowScaleForPoint）留在 app 层 main.cpp，作为参数传入——与 core::theme 用
// 回调注入平台能力的既有分层一致。**平台差异只经下面两处「平台注入」进入，
// 实现按仓库「平台适配接口隔离」惯例拆 window_layout_macos.cpp / _win.cpp /
// _posix.cpp（CMake 按平台选编），头文件只见平台无关签名，业务代码零平台宏**。
#pragma once

namespace core::window_layout {

// 显示器工作区（窗口单位，不含任务栏/Dock，屏幕坐标）。字段语义与
// EUI-NEO core::window::MonitorWorkArea 一致，此处独立定义只为让本头零依赖
struct WorkArea {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 窗口非客户区外框开销（窗口单位）：width = 左右边框合计，height = 标题栏 + 上下边框
struct FrameInsets {
    int width = 0;
    int height = 0;
};

// 窗口尺寸（窗口单位）。0 = 该方向不约束（透传给后端的 "no limit" 语义）
struct WindowSize {
    int width = 0;
    int height = 0;
};

// 请求的窗口尺寸与最小尺寸语义（**逻辑单位**，与 .windowSize()/.minWindowSize()
// 同源；缩放系数为 1.0 时逻辑单位 == 窗口单位）。min 传 0 表示该方向不约束
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
    // 实际创建用的**客户区窗口单位**。maximize 时后端忽略该值（仅留档/日志）
    int width = 0;
    int height = 0;
};

// ---- 平台注入（实现按平台拆文件，接口见签名）----

// 非客户区外框的**平台基准值**（窗口单位，可含小数：Windows 的 96 DPI 基准是
// 14.4 / 37.6）。含义由平台实现自己解释——Windows 是 96 DPI 下的物理像素（随
// 缩放系数等比放大），macOS 是点（windowScaleForPoint 恒 1.0，缩放不生效）。
struct FrameInsetsBaseline {
    float width = 0.0f;
    float height = 0.0f;
};

/**
 * @brief 本平台窗口单位下的非客户区外框基准（平台注入，见上）。
 *
 * macOS = {0, 28}：仅标题栏（无左右/下边框）。实测——本机窗口 frame 1470×833
 * 点而客户区 805 点 → 装饰高 28、装饰宽 0。
 * Windows = {14.4, 37.6}：本机 125%（120 DPI）实测有边框可缩放窗口外框相对
 * 客户区多 +18 宽 / +47 高，按 96 DPI 反推保存（18÷1.25 / 47÷1.25）。
 */
FrameInsetsBaseline frameInsetsBaseline();

/**
 * @brief 平台窗口尺寸单位名（诊断日志用）：Windows = "物理px"，macOS = "pt"。
 *
 * 为什么单独抽出来：启动日志曾把 macOS 的「点」标成「物理px」并打印「缩放
 * 1.000000」，任何按这行日志排查 macOS 布局的人都会得出「框架在 Retina 上没
 * 折算」的错误前提（2026-09-28 审计的病因之一）。单位名是平台事实，随上面
 * 的注入一起按平台拆文件。
 */
const char* windowUnitName();

// ---- 纯计算 ----

/**
 * @brief 决策启动期主窗口尺寸：能放下用请求尺寸（换算成窗口单位），放不下则最大化。
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
 * @param workArea    目标显示器工作区（窗口单位）
 * @param scale       该显示器 DPI / 96（core::window::windowScaleForPoint）
 * @param frameInsets 非客户区外框开销（窗口单位，见 frameInsetsForScale）
 */
InitialWindowDecision decideInitialWindow(const InitialWindowRequest& request,
                                          const WorkArea& workArea,
                                          float scale,
                                          const FrameInsets& frameInsets);

/**
 * @brief 非客户区外框（窗口单位）：平台基准 × 缩放系数，向上取整。
 *
 * 基准值来自 frameInsetsBaseline()（平台注入），本重载是业务路径的便捷入口。
 */
FrameInsets frameInsetsForScale(float scale);

/**
 * @brief 同上，但基准显式传入——纯计算，供单测覆盖两个平台的参数档。
 *
 * 向上取整：高估外框 → 最坏是多触发一次最大化（用户可还原），低估则窗口再次
 * 被任务栏/Dock 吃掉（2026-09-27 缺陷）。纯经验值，不追求逐版本精确——决策只
 * 关心"是否放得下"这个粗边界。
 */
FrameInsets frameInsetsForScale(float scale, const FrameInsetsBaseline& baselineAt96Dpi);

/**
 * @brief 最小窗口尺寸（窗口单位）：逻辑最小尺寸 → 与 windowSize **同一条**换算
 *        → 再与「工作区 − 外框」取 min。
 *
 * 为什么（2026-09-28 macOS 审计，两条症状同源）：main.cpp 的 kMinWindowLogical*
 * 之前原值直传 .minWindowSize，而同一份配置里的 .windowSize 走了
 * windowScaleForPoint 换算——同一条尺寸链两套语义。macOS 上常量按点解释：
 * 1330×845 里 845 > 工作区 833 点（本机 1470×956 点屏），"满足最小尺寸"与
 * "窗口完整可见"结构上不可兼得（用户一旦手动缩放，底沿只能停在屏外/Dock 下）；
 * Windows 上同一数值被后端当物理像素 → 实际下限缩到 676 逻辑，低于 845 的
 * 设计意图。**两条必须一起做**：只做换算不做钳制，会把 macOS 的病复制到
 * Windows——845 逻辑 = 1056 物理 > 本机 1020 工作区。
 *
 * 规则（顺序即优先级）：
 *  1. 缩放系数非法（<=0 / 非有限）按 1.0 处理（与 decideInitialWindow 同口径）；
 *  2. min 逻辑值 <= 0 = 该方向不约束，保持 0 透传，不换算、不钳制；
 *  3. 换算：逻辑 × scale 四舍五入（与 windowSize 同一条公式，见 physicalSize）；
 *  4. 工作区无效（宽或高 <= 0）→ 不钳制，返回换算值（与 decideInitialWindow
 *     第 3 条同口径）；
 *  5. 钳制：与「工作区 − 外框」取 min——保证窗口缩到最小时外框仍落在工作区内。
 *     工作区比外框还小（可用 <= 0）时该方向返回 0 = 不约束：此时任何下限都
 *     装不进屏，交给后端的"无约束"语义，而不是给一个负值或伪下限。
 *
 * @param request     请求尺寸（只消费 minLogicalWidth / minLogicalHeight）
 * @param workArea    目标显示器工作区（窗口单位）
 * @param scale       该显示器 DPI / 96（与 decideInitialWindow 同一个值）
 * @param frameInsets 非客户区外框开销（窗口单位）
 */
WindowSize minWindowSizeForWorkArea(const InitialWindowRequest& request,
                                    const WorkArea& workArea,
                                    float scale,
                                    const FrameInsets& frameInsets);

} // namespace core::window_layout
