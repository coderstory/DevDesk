// macOS 系统深浅检测（system_theme 的 Apple 分支实现；CMake APPLE 分支编译）。
// 审计 CP-12：此前 APPLE 分支误用 system_theme_posix.cpp（恒 false），设置页
// themeMode=auto 在 macOS 上永远解析成浅色。
// 用纯 C 的 CFPreferences 读 NSGlobalDomain 的 AppleInterfaceStyle（不用
// NSUserDefaults，避免引入 .mm / ObjC++ TU）；读不到（浅色系统不写该键、
// 无 GUI 会话 / SSH、键被删）一律按浅色兜底——与 system_theme.h 的接口承诺一致。
#include "system_theme.h"

#include <CoreFoundation/CoreFoundation.h>

namespace platform {

bool prefersDarkTheme() {
    // 该 API 只接受 2 参：第二参即应用域（NSGlobalDomain = 全局域）
    CFTypeRef value = CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"),
                                                CFSTR("NSGlobalDomain"));
    if (value == nullptr) return false;  // 浅色系统不写该键 → 浅色
    // 值理论上是 CFString（"Dark"）；非字符串（外部写入畸形值）按浅色兜底，
    // 避免对非 CFString 做 CFStringCompare（未定义行为）
    const bool dark =
        CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(value), CFSTR("Dark"), 0) ==
            kCFCompareEqualTo;
    CFRelease(value);  // Copy 规则：调用方负责释放
    return dark;
}

} // namespace platform
