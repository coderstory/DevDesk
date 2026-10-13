// macOS 深色检测。用纯 C 的 CFPreferences 读 NSGlobalDomain 的 AppleInterfaceStyle
//（不用 NSUserDefaults，避免引入 .mm / ObjC++ TU）；读不到（浅色系统不写该键、无 GUI 会话）按浅色兜底。
#include "system_theme.h"

#include <CoreFoundation/CoreFoundation.h>

namespace platform {

bool prefersDarkTheme() {
    // 该 API 只接受 2 参：第二参即应用域（NSGlobalDomain = 全局域）
    CFTypeRef value = CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"),
                                                CFSTR("NSGlobalDomain"));
    if (value == nullptr) return false;  // 浅色系统不写该键 → 浅色
    // 值理论上是 CFString（"Dark"）；非字符串（外部写入畸形值）按浅色兜底，避免对非 CFString 做 CFStringCompare（UB）
    const bool dark =
        CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(value), CFSTR("Dark"), 0) ==
            kCFCompareEqualTo;
    CFRelease(value);  // Copy 规则：调用方负责释放
    return dark;
}

} // namespace platform
