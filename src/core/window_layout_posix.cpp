// src/core/window_layout_posix.cpp
// window_layout 的 posix（Linux/X11）平台注入——非当前开发目标平台，本文件的
// 作用是让「接口隔离」在非 Windows / 非 macOS 构建里仍有确定实现（不静默退化、
// 不借用别的平台的常量）。接口与语义见 window_layout.h「平台注入」段。
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // 无实测数据：X11 下 core::window::windowScaleForPoint 恒返 1.0（无独立
    // 物理/逻辑分层），与 macOS 走同一条「单位即窗口单位」路径，故沿用同一形状
    // 的保守估计——只算标题栏、不假设左右边框。装饰由窗口管理器接管、宽度通常
    // 为 0~8，高估方向（多触发一次最大化）比低估安全。
    // 将来在 X11 上取证后回填；当前平台无 CI、无可复现量测手段。
    return FrameInsetsBaseline{0.0f, 28.0f};
}

const char* windowUnitName() {
    return "pt";  // X11 无物理/逻辑分层，单位与 macOS 的点同为「窗口单位」
}

} // namespace core::window_layout
