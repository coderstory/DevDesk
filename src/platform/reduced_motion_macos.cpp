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
    // objc_getClass 返回 objc_class*，C++ 下不隐式转 id → 显式 reinterpret_cast。
    // objc_msgSend 是变参声明，C++ 严格原型下直接调编译不过（clangd 报无匹配
    // 重载）→ 取地址后按目标签名强转（社区标准手法）。
    const id cls = reinterpret_cast<id>(objc_getClass("NSWorkspace"));
    if (cls == nil) return false;
    using MsgSendObj = id (*)(id, SEL);
    const id ws = reinterpret_cast<MsgSendObj>(&objc_msgSend)(
        cls, sel_getUid("sharedWorkspace"));
    if (ws == nil) return false;
    using MsgSendBool = signed char (*)(id, SEL);  // BOOL = signed char（arm64 同）
    const signed char reduced = reinterpret_cast<MsgSendBool>(&objc_msgSend)(
        ws, sel_getUid("accessibilityDisplayShouldReduceMotion"));
    return reduced != 0;
}

}  // namespace platform::reduced_motion
