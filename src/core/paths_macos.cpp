// src/core/paths_macos.cpp
// macOS 平台实现：appDataDir = ~/Library/Application Support/DevDesk
#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace core::paths {

fs::path appDataDir() {
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / "Library" / "Application Support" / "DevDesk";
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
    return fs::path("/Library/Application Support/ClaudeCode/managed-settings.json");
}

} // namespace core::paths
