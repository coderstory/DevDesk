// window_layout 的 macOS 平台注入：本文件只返回平台事实，零计算逻辑——
// 换算/决策全在 window_layout.cpp（纯函数，可无头单测）
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // macOS 装饰由 AppKit 接管：无左右/下边框，只有标题栏，装饰宽 0。
    // 实测（1470×956 点屏）：frame 1470×833、客户区 805 → 装饰高 28、宽 0。
    // 必须平台化：沿用 Windows 的 14.4/37.6 会多算不存在的边框，使「放不下」
    // 阈值比真实可用面积小，临界屏上把本可完整显示的窗口无谓最大化
    return FrameInsetsBaseline{0.0f, 28.0f};
}

const char* windowUnitName() {
    // 单位为点：windowScaleForPoint 在 macOS 恒返 1.0（Retina 的 2.0 是
    // contentScale，不是「逻辑→窗口单位」换算，乘上去会双倍放大）
    return "pt";
}

} // namespace core::window_layout
