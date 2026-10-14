// src/platform/folder_picker_macos.cpp
// macOS 实现：osascript 调 choose folder（免 ObjC 链接）
#include "folder_picker.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace platform::folderpicker {

namespace {

// 对双引号做 shell 转义（title 传给 AppleScript 的双引号字符串）
std::string escapeAppleScriptString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

} // namespace

core::Result<std::string> pickFolder(const std::string& title,
                                     const std::string& /*startDir*/) {
    // 拼 osascript 命令：choose folder 不支持指定初始目录，忽略 startDir
    std::string prompt = title.empty() ? "选择目录" : title;
    std::string escaped = escapeAppleScriptString(prompt);
    std::string cmd =
        "osascript -e \"POSIX path of (choose folder with prompt \\\""
        + escaped + "\\\")\"";

    std::string output;
    output.reserve(4096);
    // popen 以文本模式读 osascript 的 POSIX 路径输出
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "无法执行 osascript（popen 失败）");
    }

    char buf[1024];
    while (::fgets(buf, sizeof(buf), pipe) != nullptr) {
        output += buf;
    }
    int status = ::pclose(pipe);

    // osascript 返回非零 = 用户取消或出错
    if (status != 0) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "用户取消选择");
    }

    // 去掉末尾换行
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }

    if (output.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "用户取消选择");
    }

    return core::Result<std::string>::ok(std::move(output));
}

} // namespace platform::folderpicker