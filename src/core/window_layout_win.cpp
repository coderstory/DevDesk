// src/core/window_layout_win.cpp
// window_layout 的 Windows 平台注入（接口与语义见 window_layout.h「平台注入」段）。
// 本文件只返回平台事实，零计算逻辑——换算/决策全在 window_layout.cpp。
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // Windows 的标题栏/边框高度随显示器 DPI 等比放大，故按 96 DPI 基准保存：
    // 本机（Windows 11，1920×1080 @125% = 120 DPI）实测有边框可缩放窗口的外框
    // 相对客户区多 +18 宽 / +47 高（1618−1600 / 1047−1000），折算回 96 DPI 得
    // 14.4 / 37.6。取浮点基准 + 向上取整，保证 125% 下正好还原实测值
    //（14.4×1.25 = 18.0、37.6×1.25 = 47.0），其余缩放档按比例放大。
    return FrameInsetsBaseline{14.4f, 37.6f};
}

const char* windowUnitName() {
    // 窗口尺寸单位 = 客户区物理像素：EUI-NEO 在 Windows GLFW 路径不设
    // GLFW_SCALE_TO_MONITOR，进程是 per-monitor DPI aware，glfwCreateWindow /
    // glfwSetWindowSizeLimits 收的都是物理像素（见 ../EUI-NEO/core/window/
    // window_backend.cpp 与 3rd/glfw/src/win32_init.c）。
    return "物理px";
}

} // namespace core::window_layout
