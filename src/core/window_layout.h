// src/core/window_layout.h
// 启动期主窗口尺寸决策（纯策略，无平台依赖，可无头单测）。windowSize 是客户区窗口单位：
// Windows=物理像素、macOS=点（windowScaleForPoint 在 macOS 恒 1.0，Retina 的 2.0 是
// contentScale，别乘）；放不下工作区则启动即最大化。最小尺寸走同一换算后再与「工作区-外框」
// 取 min（只换算不钳制会把 macOS 的病复制到 Windows）。工作区/缩放取值在 app 层传入，
// 平台差异只经下方「平台注入」进入，实现在 _macos/_win/_posix.cpp，头文件零平台宏。
#pragma once

namespace core::window_layout {

// 显示器工作区（窗口单位，不含任务栏/Dock，屏幕坐标）；独立定义只为让本头零依赖
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

// 窗口尺寸（窗口单位）。0 = 该方向不约束——注意下游 all-or-nothing：任一轴 0 → 两轴最小
// 尺寸一起失效（cocoa/win32 均如此），不是「只放开这一轴」
struct WindowSize {
    int width = 0;
    int height = 0;
};

// 请求的窗口尺寸与最小尺寸（逻辑单位，与 .windowSize()/.minWindowSize() 同源，scale=1.0 时相等）；min 0 = 不约束
struct InitialWindowRequest {
    int logicalWidth = 0;
    int logicalHeight = 0;
    int minLogicalWidth = 0;
    int minLogicalHeight = 0;
};

struct InitialWindowDecision {
    // true = 启动即最大化（换算后外框放不进工作区），落成 GLFW_MAXIMIZED 创建期 hint
    bool maximize = false;
    // 实际创建用的客户区窗口单位；maximize 时后端忽略（仅留档/日志）
    int width = 0;
    int height = 0;
};

// 平台注入（实现按平台拆文件）
// 非客户区外框的平台基准值（窗口单位，可含小数）。Windows 是 96 DPI 下物理像素（随缩放
// 等比放大，本机基准 14.4/37.6），macOS 是点（缩放不生效）
struct FrameInsetsBaseline {
    float width = 0.0f;
    float height = 0.0f;
};

// 本平台窗口单位下的非客户区外框基准：macOS {0,28}（仅标题栏）；Windows {14.4,37.6}
//（本机 125% 实测 +18宽/+47高，按 96 DPI 反推）
FrameInsetsBaseline frameInsetsBaseline();

// 平台窗口尺寸单位名（诊断日志用）：Windows="物理px"，macOS="pt"；单位名是平台事实
const char* windowUnitName();

// 决策启动期主窗口尺寸：能放下用请求尺寸（换算成窗口单位），放不下则最大化。规则：
// 1) scale 非法按 1.0；2) 逻辑尺寸与最小尺寸取大（0=不约束）；3) 工作区无效（<=0）→
// 不最大化、原样用请求尺寸；4) 换算后「客户区+外框」严格超出工作区才 maximize（等于算放得下）
InitialWindowDecision decideInitialWindow(const InitialWindowRequest& request,
                                          const WorkArea& workArea,
                                          float scale,
                                          const FrameInsets& frameInsets);

// 非客户区外框（窗口单位）：平台基准 × scale 向上取整（基准来自 frameInsetsBaseline()）
FrameInsets frameInsetsForScale(float scale);

// 同上但基准显式传入（纯计算，供单测覆盖两平台档）。向上取整：高估最多多触发一次
// 最大化，低估则窗口再被任务栏/Dock 吃掉；纯经验值，只关心「是否放得下」
FrameInsets frameInsetsForScale(float scale, const FrameInsetsBaseline& baselineAt96Dpi);

// 最小窗口尺寸（窗口单位）：逻辑最小尺寸 → 与 windowSize 同一条换算 → 再与「工作区−外框」
// 取 min。规则：1) scale 非法按 1.0；2) min<=0 保持 0 透传不换算不钳制；3) 换算=逻辑×scale
// 四舍五入；4) 工作区无效不钳制返回换算值；5) 钳到「工作区−外框」，可用<=0 时返回 0 交给
// 后端「无约束」语义（注意该 0 是 all-or-nothing，任一轴 0 会让两轴最小尺寸一起失效）。
// 只做换算不做钳制会把 macOS「845 点 > 工作区，收不进屏」的病复制到 Windows
WindowSize minWindowSizeForWorkArea(const InitialWindowRequest& request,
                                    const WorkArea& workArea,
                                    float scale,
                                    const FrameInsets& frameInsets);

} // namespace core::window_layout
