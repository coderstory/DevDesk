// src/core/window_layout_macos.cpp
// window_layout 的 macOS 平台注入（接口与语义见 window_layout.h「平台注入」段）。
// 本文件只返回平台事实，零计算逻辑——换算/决策全在 window_layout.cpp（纯函数，
// 可无头单测）：接口隔离的边界就画在这里。
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // macOS 的窗口装饰由 AppKit 接管：无左右边框、无下边框，只有标题栏，
    // 且装饰高度不吃点数宽。
    // 实测（2026-09-28 审计，本机 1470×956 点屏）：窗口 frame 1470×833 点、
    // 客户区 805 点 → 装饰高 28、装饰宽 0。
    // 为什么必须平台化：原实现沿用 Windows 实测的 14.4/37.6（96 DPI 基准），
    // 在 macOS 上凭空多算一圈不存在的边框 → 判定「放不下」的阈值比真实可用
    // 面积小 15 点宽 / 约 10 点高，临界屏上会把本可完整显示的窗口无谓最大化。
    return FrameInsetsBaseline{0.0f, 28.0f};
}

const char* windowUnitName() {
    // 窗口尺寸单位 = 点：core::window::windowScaleForPoint 在 macOS 恒返 1.0
    //（Retina 的 2.0 是 contentScale/backing scale，不是「逻辑→窗口单位」的
    //  换算，乘上去会双倍放大），逻辑单位与点 1:1。
    return "pt";
}

} // namespace core::window_layout
