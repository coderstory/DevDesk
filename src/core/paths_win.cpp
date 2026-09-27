// src/core/paths_win.cpp
// Windows 平台实现：appDataDir = %APPDATA%/DevDesk；
// executableDir = GetModuleFileNameW（宽字符）取 exe 所在目录
#include "paths.h"

#include <windows.h>

#include <cstdlib>
#include <string>

namespace fs = std::filesystem;

namespace core::paths {

namespace {

// UTF-16 → UTF-8（Windows API，无 locale 依赖）；失败返回空串
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                           static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

fs::path executableDir() {
    // GetModuleFileNameW 取宽字符路径（不经 ANSI 代码页，非 ASCII 安装路径不乱码），
    // 再转 UTF-8 交给 fs::path。缓冲不足时 API 返回 nSize（= 缓冲区长度）而非截断
    // 后的长度，故以 n == size 判定截断并翻倍重试
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};  // 失败：返回空，不回退 cwd
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const std::string utf8 = wideToUtf8(buf);
    if (utf8.empty()) return {};
    const fs::path exe(utf8);
    if (!exe.has_parent_path()) return {};  // 无目录成分（异常形态）：按失败处理
    return exe.parent_path();
}

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
