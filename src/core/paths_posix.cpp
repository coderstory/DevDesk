// src/core/paths_posix.cpp
// 其他平台（Linux 等）实现：appDataDir = ~/.local/share/DevDesk
#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace core::paths {

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
