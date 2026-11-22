// src/platform/process_runner_posix.cpp
// Windows/macOS 之外的 POSIX 兜底（Linux 等）：fork/exec + 管道全捕获（stdin/stdout/stderr）。
// 超时 = 输出管道排空 + waitpid WNOHANG 轮询并行（排空优先，防 >64KB 输出互锁）；超时 kill(SIGKILL) 强杀。
#include "process_runner.h"

#include "process_runner_common.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace platform {

namespace {

void closeFdSafe(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

std::vector<const char*> buildArgvPtr(const std::vector<std::string>& argv) {
    std::vector<const char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (const auto& a : argv) ptrs.push_back(a.c_str());
    ptrs.push_back(nullptr);
    return ptrs;
}

} // namespace

bool processRunnerSupported() { return true; }

std::string defaultShellPath() { return "/bin/sh"; }

ProcessRunner systemProcessRunner() {
    if (ProcessRunner& injected = systemProcessRunnerForTest()) return injected;
    return [](const std::vector<std::string>& argv, const std::string& stdinText,
              const std::string& cwd, long timeoutMs) -> core::Result<ProcessResult> {
        if (argv.empty()) {
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::ValidationError, "进程执行器：argv 为空");
        }

        ProcessResult result;

        int stdoutPipe[2] = {-1, -1};
        int stderrPipe[2] = {-1, -1};
        int stdinPipe[2] = {-1, -1};

        if (pipe(stdoutPipe) != 0) {
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：pipe stdout 失败: " + std::string(strerror(errno)));
        }
        if (pipe(stderrPipe) != 0) {
            closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：pipe stderr 失败: " + std::string(strerror(errno)));
        }
        if (!stdinText.empty() && pipe(stdinPipe) != 0) {
            closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
            closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：pipe stdin 失败: " + std::string(strerror(errno)));
        }

        pid_t pid = fork();
        if (pid < 0) {
            closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
            closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
            closeFdSafe(stdinPipe[0]); closeFdSafe(stdinPipe[1]);
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：fork 失败: " + std::string(strerror(errno)));
        }

        if (pid == 0) {
            // 子进程
            if (!cwd.empty()) chdir(cwd.c_str());

            // 重定向 stdin
            if (!stdinText.empty()) {
                dup2(stdinPipe[0], STDIN_FILENO);
            } else {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    close(devnull);
                }
            }
            // 重定向 stdout/stderr
            dup2(stdoutPipe[1], STDOUT_FILENO);
            dup2(stderrPipe[1], STDERR_FILENO);

            // 关闭管线端副本
            closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
            closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
            closeFdSafe(stdinPipe[0]); closeFdSafe(stdinPipe[1]);

            auto argvPtrs = buildArgvPtr(argv);
            execvp(argv[0].c_str(), const_cast<char* const*>(argvPtrs.data()));
            // execvp 失败
            perror("execvp");
            _exit(127);
        }

        // 父进程：关掉子进程侧管道端
        closeFdSafe(stdoutPipe[1]);
        closeFdSafe(stderrPipe[1]);
        if (!stdinText.empty()) {
            closeFdSafe(stdinPipe[0]);
        }

        // 写 stdin
        if (!stdinText.empty() && stdinPipe[1] >= 0) {
            ssize_t written = write(stdinPipe[1], stdinText.data(), stdinText.size());
            (void)written;
            closeFdSafe(stdinPipe[1]);
        }

        // 排空 + 收尸主循环在 process_runner_common（need:380 批C item6）；
        // 平台刻意差异经开关保真（债-20 a/b，见 common 文件头）。
        return process_runner::drainAndReap(
            {pid, stdoutPipe[0], stderrPipe[0], stdinPipe[0], stdinPipe[1],
             !stdinText.empty()},
            argv[0], timeoutMs,
            /*closeStdinOnWaitError=*/true,
            /*warnOnSigkillReapFailure=*/false, &result);
    };
}

} // namespace platform