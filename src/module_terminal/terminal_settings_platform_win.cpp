// src/module_terminal/terminal_settings_platform_win.cpp
// 终端设置的 Windows 选项表（need250 设计 §4.2①/④：字体/shell 选项按平台各写死一套）。
#include "terminal_settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace terminal {

namespace {
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
} // namespace

std::vector<std::string> terminalFontOptionValues() {
    return {"", "Cascadia Mono", "Consolas", "JetBrains Mono"};
}

std::vector<std::string> terminalShellOptionValues() {
    // 首项 "" = 平台默认（pwsh→powershell→COMSPEC 解析链在后端）
    return {"", "powershell", "cmd", "wsl"};
}

std::string terminalDefaultShellName() { return "pwsh"; }

// need#452：Windows 路径去重大小写不敏感（CompareStringOrdinal 语义比较，不做 locale 折叠）
bool startupDirPathEqual(const std::string& a, const std::string& b) {
    if (a == b) return true;
    const std::wstring wa = utf8ToWide(a);
    const std::wstring wb = utf8ToWide(b);
    if (wa.empty() || wb.empty()) return false;
    return CompareStringOrdinal(wa.c_str(), -1, wb.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool canEnterDirectory(const std::string& dir) {
    if (dir.empty()) return false;
    const std::wstring w = utf8ToWide(dir);
    if (w.empty()) return false;
    const DWORD attrs = GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    // FILE_LIST_DIRECTORY 打开即验证可列举；FILE_FLAG_BACKUP_SEMANTICS 才能开目录句柄。
    HANDLE h = CreateFileW(w.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

} // namespace terminal
