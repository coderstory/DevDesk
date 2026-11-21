// src/platform/process_runner_common.h
// macOS(_macos) 与 POSIX 兜底(_posix) 两版进程执行器的公共「排空 + 收尸」poll 主循环
// （need:380 批C item6：两副本逐行同构，仅两处刻意平台差异，开关保真禁统一）：
//   a) waitpid 错误路径是否关 stdin 双端：posix 关（原 posix:235-238），macos 不关；
//   b) SIGKILL 后收尸失败：macos LOG_WARN 留痕后继续宽限流程（原 macos:332-337），
//      posix 直接 break（原 posix:258）。
#pragma once

#include <string>
#include <sys/types.h>

#include "core/result.h"
#include "platform/process_runner.h"

namespace platform::process_runner {

// 两输出管道 + stdin 管道的 fd 快照（closeFdSafe 会就地置 -1，故传值拷贝）
struct DrainReapIO {
    pid_t pid = -1;
    int stdoutFd = -1;
    int stderrFd = -1;
    int stdinReadFd = -1;
    int stdinWriteFd = -1;
    bool stdinActive = false;  // stdinText 非空（stdin 管道存在）
};

// 排空 stdout/stderr 至 EOF + waitpid WNOHANG 轮询 + 超时 SIGKILL；结果写 *result
// 后返回 ok。argv0 仅用于错误消息。
core::Result<ProcessResult> drainAndReap(
    DrainReapIO io, const std::string& argv0, int timeoutMs,
    bool closeStdinOnWaitError, bool warnOnSigkillReapFailure,
    ProcessResult* result);

}  // namespace platform::process_runner
