// src/module_settings/local_git_probe_macos.cpp — macOS/POSIX 探测实现。
#include "local_git_probe.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace module::settings {

namespace {

// CLT 安装路径第一行（`xcode-select -p` 输出形如
// /Library/Developer/CommandLineTools 或 Xcode.app 路径）。
bool readXcodeSelectPath(std::string& out) {
    FILE* pipe = ::popen("xcode-select -p 2>/dev/null", "r");
    if (pipe == nullptr) return false;
    std::string line;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        line += buf;
        if (line.find('\n') != std::string::npos) break;
    }
    const int status = ::pclose(pipe);
    if (status != 0) return false;   // 未安装 CLT → 不可用
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    out = line;
    return !line.empty();
}

} // namespace

// xcode-select 随系统基础系统自带，调用它不会触发 CLT 安装 GUI；会弹 GUI 的
// 是 /usr/bin/git 这类 shim——所以全程不执行 git，只查文件存在性。
bool localGitAvailable() {
    std::string root;
    if (!readXcodeSelectPath(root)) return false;
    std::error_code ec;
    return std::filesystem::exists(root + "/usr/bin/git", ec) && !ec;
}

} // namespace module::settings
