// src/core/platform_info_win.cpp
#include "platform_info.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace core::platform_info {

std::string osName() { return "Windows"; }

std::string archName() {
#if defined(_M_X64) || defined(_M_AMD64)
    return "x86_64";
#elif defined(_M_ARM64)
    return "arm64";
#else
    return "未知架构";
#endif
}

} // namespace core::platform_info
