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
    if (const char* env = std::getenv("DEVDESK_CLAUDE_CONFIG_DIR")) {
        return fs::path(env) / "settings.json";
    }
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / ".claude" / "settings.json";
}

fs::path claudeManagedSettingsFile() {
    return fs::path("/Library/Application Support/ClaudeCode/managed-settings.json");
}

} // namespace core::paths
