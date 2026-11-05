// src/module_terminal/terminal_settings_platform_win.cpp
// 终端设置的 Windows 选项表（need250 设计 §4.2①/④：字体/shell 选项按平台各写死一套）。
#include "terminal_settings.h"

namespace terminal {

std::vector<std::string> terminalFontOptionValues() {
    return {"", "Cascadia Mono", "Consolas", "JetBrains Mono"};
}

std::vector<std::string> terminalShellOptionValues() {
    // 首项 "" = 平台默认（pwsh→powershell→COMSPEC 解析链在后端）
    return {"", "powershell", "cmd", "wsl"};
}

std::string terminalDefaultShellName() { return "pwsh"; }

} // namespace terminal
