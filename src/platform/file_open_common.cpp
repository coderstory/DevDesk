// src/platform/file_open_common.cpp
// file_open 的平台无关纯逻辑（审计 [17]）：argv → 打开文件参数的「选择」这一步。
// 各平台 openFileFromArgv 负责把原始命令行解码为 UTF-8（Windows 走
// GetCommandLineW+CommandLineToArgvW，macOS 走 _NSGetArgv），选参数规则共用本文件。
// 审计 CP-09 起，本文件同时承载 shell 转义（macOS / POSIX 两份逐字实现的下沉）。
#include "file_open.h"
#include "file_open_common.h"

namespace platform::file_open {

std::string selectOpenFileArg(const std::vector<std::string>& utf8Args) {
    // argv[0] 是 exe 自身路径；argv[1] 才是文件路径（文件关联/命令行启动传入）
    if (utf8Args.size() < 2) return {};
    const std::string& candidate = utf8Args[1];
    if (candidate.empty()) return {};
    if (candidate[0] == '-') return {};   // 跳过 flag 类参数
    return candidate;
}

// shell 单引号转义（审计 CP-09）：macOS(/usr/bin/open) 与 POSIX(xdg-open)
// 此前各持一份逐字相同实现，此处下沉共用
std::string shellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace platform::file_open
