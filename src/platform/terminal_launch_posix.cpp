// src/platform/terminal_launch_posix.cpp
// 其他平台（Linux 等）：非目标平台，明确不支持（spec §10.5）
#include "terminal_launch.h"

#include <cstdlib>
#include <filesystem>

namespace platform::terminal {

bool windowsTerminalMissing() { return false; }

std::string homeDir() {
    const char* p = std::getenv("HOME");
    if (p != nullptr && *p != '\0') return p;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string() : cwd.string();
}

core::Result<void> launch(const LaunchSpec&) {
    return core::Result<void>::fail(core::ErrorCode::NotImplemented,
        "终端启动在 Linux/其他平台未实现（非目标平台）");
}

} // namespace platform::terminal
