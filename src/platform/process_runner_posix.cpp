// src/platform/process_runner_posix.cpp
// Windows/macOS 之外的 POSIX 兜底（Linux 等）：fork/exec + 管道全捕获（stdin/stdout/stderr）。
// 超时 = 输出管道排空 + waitpid WNOHANG 轮询并行（排空优先，防 >64KB 输出互锁）；超时 kill(SIGKILL) 强杀。
#include "process_runner.h"

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

// fd 设非阻塞：排空循环里 read 无数据立即 EAGAIN，绝不阻塞在管道上
void setNonBlocking(int& fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// 非阻塞读一轮：有数据全收；EOF 或真错误关 fd 返回 false；EAGAIN/EINTR 返回 true
bool drainOnce(int& fd, std::string& sink) {
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            sink.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) { closeFdSafe(fd); return false; }
        if (errno == EAGAIN || errno == EINTR) return true;
        closeFdSafe(fd);
        return false;
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

        // 排空 + 收尸一体：两输出管道全部读到 EOF 才算完。管道没排空前绝不
        // 阻塞等退出——输出一旦超管道缓冲（64KB 量级），先等退出会让子进程
        // 卡在 write、父进程卡在 wait 互锁：设超时被误杀+输出截断，不设永挂。
        if (stdoutPipe[0] >= 0) setNonBlocking(stdoutPipe[0]);
        if (stderrPipe[0] >= 0) setNonBlocking(stderrPipe[0]);
        bool outOpen = stdoutPipe[0] >= 0;
        bool errOpen = stderrPipe[0] >= 0;
        const auto deadline =
            timeoutMs > 0
                ? std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(timeoutMs)
                : std::chrono::steady_clock::time_point::max();
        bool exited = false;
        int status = 0;
        while (outOpen || errOpen || !exited) {
            struct pollfd pfds[2];
            int nfds = 0;
            int outIdx = -1, errIdx = -1;
            if (outOpen) {
                outIdx = nfds;
                pfds[nfds++] = {stdoutPipe[0], POLLIN, 0};
            }
            if (errOpen) {
                errIdx = nfds;
                pfds[nfds++] = {stderrPipe[0], POLLIN, 0};
            }
            if (nfds > 0) {
                int pr = poll(pfds, static_cast<nfds_t>(nfds), 20);
                if (pr > 0) {
                    if (outIdx >= 0 && (pfds[outIdx].revents & (POLLIN | POLLHUP)) != 0)
                        outOpen = drainOnce(stdoutPipe[0], result.stdoutText);
                    if (errIdx >= 0 && (pfds[errIdx].revents & (POLLIN | POLLHUP)) != 0)
                        errOpen = drainOnce(stderrPipe[0], result.stderrText);
                } else if (pr < 0 && errno != EINTR) {
                    // poll 异常（不该发生）：强关两管道防死循环，错误事实留给 waitpid
                    closeFdSafe(stdoutPipe[0]);
                    outOpen = false;
                    closeFdSafe(stderrPipe[0]);
                    errOpen = false;
                }
            }
            if (exited) continue;  // 已收尸：只剩把管道拖到 EOF（孙进程持写端时靠 poll 节流）
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exited = true;
                continue;
            }
            if (w < 0 && errno != EINTR) break;
            if (!exited && std::chrono::steady_clock::now() >= deadline) {
                result.timedOut = true;
                kill(pid, SIGKILL);
                int killStatus = 0;
                pid_t wk;
                do {
                    wk = waitpid(pid, &killStatus, 0);
                } while (wk < 0 && errno == EINTR);
                if (wk < 0) break;  // 收尸失败罕见且无恢复动作；timedOut 已成立
                result.exitCode =
                    WIFEXITED(killStatus) ? WEXITSTATUS(killStatus) : -1;
                exited = true;
            }
        }
        if (!result.timedOut) {
            result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (!stdinText.empty()) {
            closeFdSafe(stdinPipe[0]);
            closeFdSafe(stdinPipe[1]);
        }

        return core::Result<ProcessResult>::ok(result);
    };
}

} // namespace platform