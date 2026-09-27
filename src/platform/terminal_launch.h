// src/platform/terminal_launch.h
// 平台无关接口（2026-09-28 profile-run 设计 §4）：在系统终端里打开一个程序。
//   Windows: Windows Terminal（wt.exe）优先，缺失则退化传统控制台（D-5）
//   macOS:   写 .command 脚本 → /usr/bin/open（跟随用户默认终端 app，D-7）
//   其他:    NotImplemented（非目标平台）
// 启动是**分离式**的：不等待被启动的进程退出（D-9：DevDesk 不持有会话句柄，
// 故停代理/退出时只能提示，不能阻止）。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform::terminal {

struct LaunchSpec {
    std::string title;                        // 窗口/标签标题（如 "官方 · Claude Code"）
    std::string workingDir;                   // 起始目录（D-3）
    std::string program;                      // 可执行名（本需求固定 "claude"）
    std::vector<std::string> args;            // 追加参数（本需求为空）
};

// 在终端里以 LaunchSpec 打开一个程序；成功 = 已成功派发（分离式，不等待退出）
core::Result<void> launch(const LaunchSpec& spec);

// 用户主目录（D-3 起始目录默认值）：Windows %USERPROFILE% / macOS·POSIX $HOME；
// 环境变量缺失时退化为进程当前工作目录（保证非空——起始目录必须有值）。
// 各平台实现读各自的环境变量，业务层零 #ifdef。
std::string homeDir();

// Windows Terminal 是否缺失（D-5 提示用，调用方可无条件查询）：
//   Windows：%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe 不存在 → true
//   macOS / 其他：恒 false（不用 wt，无退化路径）
// 调用方在 launch 成功后若得 true，附一句「已用传统控制台打开」——不静默、不阻断。
bool windowsTerminalMissing();

} // namespace platform::terminal
