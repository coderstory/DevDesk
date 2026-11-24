// src/platform/reduced_motion_macos.cpp
// macOS 实现：NSWorkspace.accessibilityDisplayShouldReduceMotion，Objective-C
// runtime C API（objc_msgSend），本 TU 保持纯 C++（同 window_visibility_macos
// 手法，不 import AppKit）。类查询在 GLFW 初始化（AppKit 已入进程）后才可靠；
// 拿不到类/对象一律返回 false（fail-open = 不减动效，见 reduced_motion.h）。
#include "reduced_motion.h"

#include <objc/message.h>
#include <objc/runtime.h>

namespace platform::reduced_motion {

bool systemReducedMotion() {
    // NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion
    // （AppKit BOOL 属性）。GLFW/Cocoa 后端首次 objc 查询前 AppKit 已随 GLFW
    // 加载，类可查；SDL2 后端 / objc runtime 未就绪时 objc_getClass 返回 nil
    // → false（不减动效）。
    const id cls = objc_getClass("NSWorkspace");
    if (cls == nil) return false;
    const id ws = objc_msgSend(cls, sel_getUid("sharedWorkspace"));
    if (ws == nil) return false;
    const BOOL reduced =
        objc_msgSend(ws, sel_getUid("accessibilityDisplayShouldReduceMotion"));
    return reduced == YES;
}

}  // namespace platform::reduced_motion
