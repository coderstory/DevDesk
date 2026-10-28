// src/platform/process_host.h
// 常驻子进程托管平台抽象（memcpp S2：telemem / llama-server 拉起与终止）。
// 与 process_runner（同步捕获式）的分工：本层拉起**长期常驻**进程——不接管道、不等待、
// 输出丢弃；生命周期由托管方显式 terminateTree。
// 接口缝 + 零纯逻辑：实现按平台拆（*_win / *_posix），业务代码零平台宏。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform::process_host {

// 拉起常驻子进程：脱离父进程独立存活（Windows CREATE_NO_WINDOW + 新进程组；
// POSIX setsid + 新进程组），stdout/stderr 丢弃。返回子进程 pid。
// envPairs：追加环境项（"KEY=VALUE"），同名键覆盖继承值；空 = 纯继承。
// workingDir：空 = 继承父进程 cwd。启动失败返回 fail（含系统错误文本）。不抛异常。
core::Result<std::int64_t> spawnDetached(
    const std::vector<std::string>& argv,
    const std::vector<std::string>& envPairs,
    const std::string& workingDir);

// 终止进程树（子进程 + 其后代）。幂等：进程已不存在视为成功（ok）。
// Windows = taskkill /PID <pid> /T /F；POSIX = kill 进程组 SIGTERM。
core::Result<void> terminateTree(std::int64_t pid);

// pid 是否仍对应一个存活进程（不区分僵尸；仅探活用）
bool isAlive(std::int64_t pid);

} // namespace platform::process_host
