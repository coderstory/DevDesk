// src/platform/terminal/terminal_conpty_lookup.h — ConPTY v2 redist 查找顺序判定
// （debt246；纯函数无 IO，Windows TU 与单测共用本头——同 terminal_launch_common.h 模式）
#pragma once

#include <string>

namespace platform::terminal {

// ConPTY redist 目录三级查找顺序：
//   ① DEVDESK_CONPTY_DIR 环境变量值（去首尾空白与包裹引号后非空）
//   ② exe 旁 conpty\（exeDir 由调用方从 GetModuleFileNameW 派生）
//   ③ 返回空串 = 两级候选皆缺，调用方回落系统 kernel32 的 inbox CreatePseudoConsole
//
// 本函数只判**顺序与形态**；候选目录是否真有 conpty.dll + OpenConsole.exe 成对存在
// 由调用方文件面校验（缺对 = 静默回落 inbox conhost 的 wtmux 实证坑，clue105）。
inline std::wstring conptyDirCandidate(const std::wstring& envValue,
                                       const std::wstring& exeDir) {
    auto trim = [](const std::wstring& s) {
        size_t b = s.find_first_not_of(L" \t\"'");
        if (b == std::wstring::npos) return std::wstring();
        size_t e = s.find_last_not_of(L" \t\"'");
        return s.substr(b, e - b + 1);
    };
    const std::wstring env = trim(envValue);
    if (!env.empty()) return env;
    std::wstring dir = trim(exeDir);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) return {};
    return dir + L"\\conpty";
}

} // namespace platform::terminal
