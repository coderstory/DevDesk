// src/platform/window_visibility_macos.cpp
// macOS 实现：主窗可见态用 GLFW 的同源真值——GLFW_ICONIFIED（Cocoa
// isMiniaturized）与 GLFW_VISIBLE（Cocoa isVisible），语义对齐 Windows 的
// IsIconic/IsWindowVisible（最小化 + 藏托盘两态都判）。
//
// 句柄来源：EUI-NEO 主循环在 app::update（compose）之前对主窗 makeCurrent，
// 故门控查询时 glfwGetCurrentContext() 即主窗的 GLFWwindow*。保守降级：
// 拿不到句柄（nullptr——glfwInit 前 / 无 GL 上下文 / SDL2 后端）或未初始化，
// 一律返回 true（fail-open，不阻塞渲染）；EUI-NEO 主循环的 hidden/iconified
// 跳帧仍是第一道零成本兜底。
#include "window_visibility.h"

#if !defined(EUI_WINDOW_BACKEND_SDL2)
// GLFW_INCLUDE_NONE：只取 C 查询 API，不拉平台 GL 头（probe_harness 同款）
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#endif

namespace platform::window_visibility {

bool mainWindowVisible() {
#if defined(EUI_WINDOW_BACKEND_SDL2)
    // SDL2 后端无 GLFW 窗口指针可查：无法判定 → fail-open
    return true;
#else
    GLFWwindow* window = glfwGetCurrentContext();
    if (window == nullptr) return true;  // 无当前上下文 → fail-open
    return glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_FALSE &&
           glfwGetWindowAttrib(window, GLFW_VISIBLE) == GLFW_TRUE;
#endif
}

} // namespace platform::window_visibility