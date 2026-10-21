// src/platform/process_runner.h
// 进程执行器平台抽象（UAT6 智能体内置工具 batch）：头文件平台无关——
// 接口缝 + 纯逻辑全部 inline 在头里，实现按 *_win / *_macos / *_posix 拆（与
// autostart / file_open 同一模式，业务代码零平台宏）。
// Windows：CreateProcess + 匿名管道（stdin/stdout/stderr 全捕获）；
// macOS：posix_spawn + 管道（/bin/zsh -c）；
// POSIX 兜底：fork/exec + 管道（sh -c）。
// 模块级使用（module_agent 的 bash.run / git.* 工具经此 spawn 子进程），
// 单测经 ProcessRunner 构造注入假 runner 断言命令与参数，headless 不碰系统。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform {

struct ProcessResult {
    int exitCode = -1;             // 进程退出码；非零 = 命令执行失败
    std::string stdoutText;        // stdout 全文（未截断上限由调用方控制）
    std::string stderrText;        // stderr 全文
    bool timedOut = false;         // 超时杀掉
};

// 注入缝：单测用假 runner 断言命令与参数，headless 不碰系统
using ProcessRunner = std::function<core::Result<ProcessResult>(
    const std::vector<std::string>& argv,       // argv[0] = 可执行名
    const std::string& stdinText,               // 空串 = 无 stdin
    const std::string& cwd,                     // 空串 = 继承当前工作目录
    long timeoutMs                              // 毫秒，0 = 无限
)>;

// 生产 runner：按平台 spawn 真进程
ProcessRunner systemProcessRunner();

// bash.run 专用：探测本平台默认 shell 路径
// Windows：优先取 PATH 里的 bash.exe（排除 System32\\bash.exe = WSL），再兜底常见安装路径
// macOS：/bin/zsh
// POSIX 兜底：sh
std::string defaultShellPath();

// 平台支持标志（POSIX 兜底 false）
bool processRunnerSupported();

} // namespace platform