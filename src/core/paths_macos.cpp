// src/core/paths_macos.cpp
// macOS 平台实现：platformDefaultAppDataDir = ~/DevDesk（不再是系统惯例的
// ~/Library/Application Support/DevDesk，理由见 paths.h）；
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

// 平台**默认**数据根（DEVDESK_DATA_ROOT 覆盖与优先级见 paths.h / paths.cpp；
// 业务代码不要直接调用本函数）
fs::path platformDefaultAppDataDir() {
    // <用户主目录>/DevDesk —— 主目录取平台惯例的 HOME（与同文件
    // claudeSettingsFile 同源）；不硬编码 /Users/<名>，也不拿 cwd 兜底
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / "DevDesk";
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
