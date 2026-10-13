// src/platform/terminal_launch.h
// 平台无关接口：在系统终端里打开一个程序。Windows: wt.exe 优先，缺失退化传统控制台；macOS: 写
// .command 脚本 → /usr/bin/open；其他: NotImplemented。启动是**分离式**的：不等待被启动进程退出
//（不持有会话句柄，停代理/退出时只能提示，不能阻止）。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform::terminal {

struct LaunchSpec {
    std::string title;                        // 窗口/标签标题
    std::string workingDir;                   // 起始目录
    std::string program;                      // 可执行名（固定 "claude"）
    std::vector<std::string> args;            // 追加参数（固定：--settings + 临时文件路径）
};

// 在终端里以 LaunchSpec 打开程序；成功 = 已成功派发（分离式，不等待退出）
core::Result<void> launch(const LaunchSpec& spec);

// 用户主目录（起始目录默认值）：Windows %USERPROFILE% / macOS·POSIX $HOME；环境变量缺失时退化为进程
// 当前工作目录（保证非空）。
std::string homeDir();

// Windows Terminal 是否缺失：Windows: %LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe 不存在 → true；
// macOS/其他：恒 false。调用方在 launch 成功后若得 true，附一句「未检测到 Windows Terminal」提示。
bool windowsTerminalMissing();

} // namespace platform::terminal
