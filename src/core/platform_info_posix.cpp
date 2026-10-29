// src/core/platform_info_posix.cpp — macOS 与 Linux 同实现（宏判 Apple）
#include "platform_info.h"

namespace core::platform_info {

std::string osName() {
#if defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

std::string archName() {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "arm64";
#else
    return "未知架构";
#endif
}

} // namespace core::platform_info
