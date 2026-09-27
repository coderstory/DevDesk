// src/core/paths_macos.cpp
// macOS 平台实现：appDataDir = ~/Library/Application Support/DevDesk；
// executableDir = _NSGetExecutablePath 所在目录（.app 内为 Contents/MacOS）
#include "paths.h"

#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace fs = std::filesystem;

namespace core::paths {

fs::path executableDir() {
    // _NSGetExecutablePath：先传 buf=nullptr 只回填所需缓冲长度（返回 -1），
    // 再按长度取实际路径。它返回的可能是含符号链接的路径，用 weakly_canonical
    // 规范化；规范化失败则退回原始路径（不影响"exe 所在目录"语义）
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};  // 失败：返回空，不回退 cwd
    std::vector<char> buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    const fs::path raw(buf.data());
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(raw, ec);
    const fs::path& exe = ec ? raw : canonical;
    if (!exe.has_parent_path()) return {};
    return exe.parent_path();
}

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
