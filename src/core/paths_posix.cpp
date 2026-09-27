// src/core/paths_posix.cpp
// 其他平台（Linux 等）实现：appDataDir = ~/.local/share/DevDesk；
// executableDir = /proc/self/exe 所在目录（无 /proc 的平台返回空 path）
#include "paths.h"

#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace core::paths {

fs::path executableDir() {
    // /proc/self/exe 是指向可执行文件的符号链接，readlink 跟随后拿到的就是真实
    // 路径。readlink 截断时返回值 == 缓冲区长度，此时翻倍重试；上界 64 KiB 封顶
    // 防病态循环。平台无 /proc（如 BSD）或无权限时返回空，不回退 cwd
    std::vector<char> buf(1024);
    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return {};  // 失败：返回空，不回退 cwd
        if (static_cast<std::size_t>(n) < buf.size()) {
            const fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
            if (!exe.has_parent_path()) return {};
            return exe.parent_path();
        }
        if (buf.size() >= 64 * 1024) return {};
        buf.resize(buf.size() * 2);
    }
}

fs::path appDataDir() {
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / ".local" / "share" / "DevDesk";
}

fs::path claudeSettingsFile() {
    // 审计 CP-09：覆盖分支下沉到 paths.cpp::claudeConfigDirOverride()（三平台共用）
    const fs::path overridePath = claudeConfigDirOverride();
    if (!overridePath.empty()) {
        return overridePath;
    }
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / ".claude" / "settings.json";
}

fs::path claudeManagedSettingsFile() {
    return fs::path("/etc/claude-code/managed-settings.json");
}

} // namespace core::paths
