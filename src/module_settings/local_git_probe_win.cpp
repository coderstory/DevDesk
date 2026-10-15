// src/module_settings/local_git_probe_win.cpp — Windows 探测实现。
#include "local_git_probe.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace module::settings {

namespace {

std::string envOrNull(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr ? std::string() : std::string(v);
}

} // namespace

// 纯文件系统检查，不 spawn 进程：探 PATH 各目录下的 git.exe，再探两个
// 已知安装位置（Git for Windows 默认路径，见审计 F8）。
// PATH 以 ';' 分隔（Windows 盘符里的 ':' 不是分隔符，按 ';' 切不会切碎 C:\）。
bool localGitAvailable() {
    std::vector<std::string> dirs;
    const std::string pathEnv = envOrNull("PATH");
    size_t start = 0;
    while (start <= pathEnv.size()) {
        const size_t semi = pathEnv.find(';', start);
        if (semi == std::string::npos) {
            dirs.push_back(pathEnv.substr(start));
            break;
        }
        dirs.push_back(pathEnv.substr(start, semi - start));
        start = semi + 1;
    }
    // Git for Windows 两个默认安装位置（优先于 PATH 命中的旧/便携版与否不做
    // 区分——本探测只回答「有没有」，不回答「用哪个」）。
    const std::string pf = envOrNull("ProgramFiles");
    if (!pf.empty()) dirs.push_back(pf + "\\Git\\cmd");
    const std::string lad = envOrNull("LOCALAPPDATA");
    if (!lad.empty()) dirs.push_back(lad + "\\Programs\\Git\\cmd");

    std::error_code ec;
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        if (std::filesystem::exists(dir + "\\git.exe", ec) && !ec) return true;
    }
    return false;
}

} // namespace module::settings
