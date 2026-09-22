// src/core/paths_win.cpp
// Windows 平台实现：appDataDir = %APPDATA%/DevDesk
#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace core::paths {

fs::path appDataDir() {
    const char* appdata = std::getenv("APPDATA");
    fs::path base(appdata ? appdata : "");
    return base / "DevDesk";
}

fs::path claudeSettingsFile() {
    // 覆盖入口（测试隔离 / 自定义 .claude 目录，见 paths.h 注释）
    if (const char* env = std::getenv("DEVDESK_CLAUDE_CONFIG_DIR")) {
        return fs::path(env) / "settings.json";
    }
    const char* home = std::getenv("USERPROFILE");
    fs::path base(home ? home : "");
    return base / ".claude" / "settings.json";
}

fs::path claudeManagedSettingsFile() {
    return fs::path("C:/ProgramData/ClaudeCode/managed-settings.json");
}

} // namespace core::paths
