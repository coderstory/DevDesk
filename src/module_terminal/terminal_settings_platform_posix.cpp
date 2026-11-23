// src/module_terminal/terminal_settings_platform_posix.cpp
// 终端设置的非 Windows 兜底选项表（macOS 一等公民口径：zsh 默认，设计 §4.2④）。
#include "terminal_settings.h"

#include <dirent.h>
#include <unistd.h>

namespace terminal {

std::vector<std::string> terminalFontOptionValues() {
    return {"", "Menlo", "SF Mono", "JetBrains Mono"};
}

std::vector<std::string> terminalShellOptionValues() {
    // 首项 "" = 平台默认（mac 四段解析链在后端）
    return {"", "bash", "sh"};
}

std::string terminalDefaultShellName() { return "zsh"; }

bool canEnterDirectory(const std::string& dir) {
    if (dir.empty()) return false;
    if (access(dir.c_str(), R_OK | X_OK) != 0) return false;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return false;
    closedir(d);
    return true;
}

} // namespace terminal
