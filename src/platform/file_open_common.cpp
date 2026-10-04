// src/platform/file_open_common.cpp
// file_open 的平台无关纯逻辑：argv 选参数 + shell 单引号转义。
#include "file_open.h"
#include "file_open_common.h"

namespace platform::file_open {

std::string selectOpenFileArg(const std::vector<std::string>& utf8Args) {
    // argv[0]=exe，argv[1]=文件路径（文件关联/命令行启动传入）
    if (utf8Args.size() < 2) return {};
    const std::string& candidate = utf8Args[1];
    if (candidate.empty()) return {};
    if (candidate[0] == '-') return {};
    return candidate;
}

// shell 单引号转义：macOS(/usr/bin/open) 与 POSIX(xdg-open) 共用
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
