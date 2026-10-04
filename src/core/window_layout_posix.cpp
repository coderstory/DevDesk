// window_layout 的 posix（Linux/X11）平台注入——非当前开发目标平台，作用是让
// 「接口隔离」在非 Windows / 非 macOS 构建里仍有确定实现（不静默退化、不借用
// 别的平台的常量）。接口与语义见 window_layout.h「平台注入」段。
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // X11 下 windowScaleForPoint 恒返 1.0，与 macOS 同路径；保守估计只算标题栏、不假设左右边框
    //（装饰宽通常 0~8，高估方向比低估安全）
    return FrameInsetsBaseline{0.0f, 28.0f};
}

const char* windowUnitName() {
    return "pt";  // X11 无物理/逻辑分层，单位与 macOS 的点同为「窗口单位」
}

} // namespace core::window_layout
