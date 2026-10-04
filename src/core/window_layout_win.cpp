// window_layout 的 Windows 平台注入：本文件只返回平台事实，零计算逻辑
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // Windows 标题栏/边框随 DPI 等比放大，故按 96 DPI 基准保存：本机
    //（1920×1080 @125% = 120 DPI）实测外框比客户区多 +18 宽 / +47 高，折算回
    // 96 DPI 得 14.4 / 37.6；取浮点基准 + 向上取整，125% 下正好还原实测值
    return FrameInsetsBaseline{14.4f, 37.6f};
}

const char* windowUnitName() {
    // 单位 = 客户区物理像素：Windows GLFW 路径不设 GLFW_SCALE_TO_MONITOR，进程是
    // per-monitor DPI aware，glfwCreateWindow / glfwSetWindowSizeLimits 收物理像素
    return "物理px";
}

} // namespace core::window_layout
