// src/platform/window_visibility_macos.cpp
// macOS 实现：主窗可见态 = GLFW 同源真值（GLFW_ICONIFIED=Cocoa isMiniaturized、
// GLFW_VISIBLE=Cocoa isVisible）+ NSWindow occlusionState（bug#218：锁屏/被其他
// 窗口完全遮挡/所在 Space 未显示时，窗口未最小化且 isVisible 仍 true，仅凭 GLFW
// 两查询判不出 → 流质泵门控失真、遮挡态照常 20fps 出帧烧 CPU）。
//
// occlusion 查询走 Objective-C runtime C API（objc_msgSend 读 occlusionState），
// 本文件保持纯 C++ 翻译单元、不 import AppKit（glfw3native.h 的 Cocoa 分支会拉
// AppKit 头，纯 .cpp 吃不下；DevDesk 唯一 .mm 在 module_wallpaper）。
// NSWindowOcclusionState.visible（AppKit，值 1UL<<1）：窗口至少部分可见；被完全
// 遮挡 / 最小化 / 所在 Space 未显示 / 锁屏时该位清零。部分遮挡不清位（还看得见）。
//
// 句柄来源：EUI-NEO 主循环在 app::update（compose）之前对主窗 makeCurrent，
// 故门控查询时 glfwGetCurrentContext() 即主窗的 GLFWwindow*。保守降级：
// 拿不到句柄（nullptr——glfwInit 前 / 无 GL 上下文 / SDL2 后端）、未初始化、
// 或拿不到 NSWindow（非 Cocoa 后端）——一律返回 true（fail-open，不阻塞渲染）；
// EUI-NEO 主循环的 hidden/iconified 跳帧仍是第一道零成本兜底。
#include "window_visibility.h"

#if !defined(EUI_WINDOW_BACKEND_SDL2)
// GLFW_INCLUDE_NONE：只取 C 查询 API，不拉平台 GL 头（probe_harness 同款）
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <objc/message.h>
#include <objc/runtime.h>

// glfw3native.h 的 Cocoa 扩展：NSWindow* glfwGetCocoaWindow(GLFWwindow*)。
// 为避开 AppKit 头按 ABI 等价签名手声明（libglfw3 导出的 C 符号，指针返回）。
extern "C" void* glfwGetCocoaWindow(GLFWwindow* window);

namespace {

// NSWindowOcclusionStateVisible（AppKit 头文件值，手抄需锚注释）：1UL << 1
constexpr unsigned long kNSWindowOcclusionStateVisible = 1UL << 1;

} // namespace
#endif // !EUI_WINDOW_BACKEND_SDL2

namespace platform::window_visibility {

bool mainWindowVisible() {
#if defined(EUI_WINDOW_BACKEND_SDL2)
    // SDL2 后端无 GLFW 窗口指针可查：无法判定 → fail-open
    return true;
#else
    GLFWwindow* window = glfwGetCurrentContext();
    if (window == nullptr) return true;  // 无当前上下文 → fail-open
    // 第一层：GLFW 同源真值（最小化 / 藏托盘 hidden 两态，语义不变）
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != GLFW_FALSE) return false;
    if (glfwGetWindowAttrib(window, GLFW_VISIBLE) != GLFW_TRUE) return false;
    // 第二层（bug#218）：occlusion——完全遮挡/锁屏/Space 未显示 → 不可见。
    // NSWindow 拿不到（非 Cocoa 后端）→ fail-open，保持旧口径。
    void* nsWindow = glfwGetCocoaWindow(window);
    if (nsWindow == nullptr) return true;
    const unsigned long occlusion =
        ((unsigned long (*)(id, SEL))objc_msgSend)(
            static_cast<id>(nsWindow), sel_registerName("occlusionState"));
    return (occlusion & kNSWindowOcclusionStateVisible) != 0;
#endif
}

} // namespace platform::window_visibility
