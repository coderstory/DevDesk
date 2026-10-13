// macOS 平台实现：默认数据根 = ~/DevDesk（非 ~/Library/Application Support，
// 理由见 paths.h）；executableDir 取 _NSGetExecutablePath 所在目录
#include "paths.h"

#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace core::paths {

fs::path executableDir() {
    // _NSGetExecutablePath：先传 nullptr 回填所需长度（返回 -1）再取实际路径。
    // 返回值可能含符号链接，用 weakly_canonical 规范化；失败则退回原始路径
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

// 平台默认数据根（DEVDESK_DATA_ROOT 覆盖与优先级见 paths.h / paths.cpp；
// 业务代码不要直接调用）
fs::path platformDefaultAppDataDir() {
    // <主目录>/DevDesk —— 用平台惯例的 HOME，不硬编码 /Users/<名>，不用 cwd 兜底
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / "DevDesk";
}

fs::path claudeSettingsFile() {
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
