// src/platform/process_runner_macos.cpp
// macOS 进程执行器：posix_spawn + 管道全捕获（stdin/stdout/stderr）。
// 超时用 waitpid + WNOHANG 轮询；超时到期 kill 强杀。
// 线程安全：每次调用独立 posix_spawn，runner 自身无共享状态。
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

// 从 fd 读全部数据直到关闭
std::string readFd(int fd) {
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// 构建 argv 的 const char* 数组（posix_spawn 需要）
std::vector<const char*> buildArgvPtr(const std::vector<std::string>& argv) {
    std::vector<const char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (const auto& a : argv) ptrs.push_back(a.c_str());
    ptrs.push_back(nullptr);
    return ptrs;
}

} // namespace

bool processRunnerSupported() { return true; }

std::string defaultShellPath() { return "/bin/zsh"; }

ProcessRunner systemProcessRunner() {
    return [](const std::vector<std::string>& argv, const std::string& stdinText,
              const std::string& cwd, long timeoutMs) -> core::Result<ProcessResult> {
        if (argv.empty()) {
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::ValidationError, "进程执行器：argv 为空");
        }

        ProcessResult result;

        // 创建管道对
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
        if (!stdinText.empty()) {
            if (pipe(stdinPipe) != 0) {
                closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
                closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：pipe stdin 失败: " + std::string(strerror(errno)));
            }
        }

        // posix_spawn 文件动作：重定向子进程的 fd
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);

        // stdin: 有内容时用管道读端，否则 /dev/null
        if (!stdinText.empty()) {
            posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], STDIN_FILENO);
        } else {
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                             O_RDONLY, 0);
        }

        // stdout → 管道写端
        posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
        // stderr → 管道写端
        posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);

        // 构建 spawn 参数
        auto argvPtrs = buildArgvPtr(argv);

        // 如有 cwd，用 posix_spawnattr_setflags + POSIX_SPAWN_SETCWD (macOS 13+)
        // 兼容旧版：若无此能力则 chdir 后 spawn（process_runner 调用方保证单线程）
        pid_t pid = -1;
        bool needsChdir = !cwd.empty();
        if (needsChdir) {
            char oldCwd[PATH_MAX] = {};
            getcwd(oldCwd, sizeof(oldCwd));
            if (chdir(cwd.c_str()) != 0) {
                posix_spawn_file_actions_destroy(&actions);
                closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
                closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
                closeFdSafe(stdinPipe[0]); closeFdSafe(stdinPipe[1]);
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：chdir 到 '" + cwd + "' 失败: " + strerror(errno));
            }
            int spawnErr = posix_spawn(&pid, argv[0].c_str(),
                                       &actions, nullptr,
                                       const_cast<char* const*>(argvPtrs.data()),
                                       environ);
            chdir(oldCwd);
            if (spawnErr != 0) {
                posix_spawn_file_actions_destroy(&actions);
                closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
                closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
                closeFdSafe(stdinPipe[0]); closeFdSafe(stdinPipe[1]);
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：posix_spawn 失败: " + std::string(strerror(spawnErr)));
            }
        } else {
            int spawnErr = posix_spawn(&pid, argv[0].c_str(),
                                       &actions, nullptr,
                                       const_cast<char* const*>(argvPtrs.data()),
                                       environ);
            if (spawnErr != 0) {
                posix_spawn_file_actions_destroy(&actions);
                closeFdSafe(stdoutPipe[0]); closeFdSafe(stdoutPipe[1]);
                closeFdSafe(stderrPipe[0]); closeFdSafe(stderrPipe[1]);
                closeFdSafe(stdinPipe[0]); closeFdSafe(stdinPipe[1]);
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：posix_spawn 失败: " + std::string(strerror(spawnErr)));
            }
        }

        posix_spawn_file_actions_destroy(&actions);

        // 关掉子进程侧的管道写端（在父进程不需要）
        closeFdSafe(stdoutPipe[1]);
        closeFdSafe(stderrPipe[1]);
        if (!stdinText.empty()) {
            closeFdSafe(stdinPipe[0]);  // 子进程侧读端，父进程不需要
        }

        // 写 stdin 并关写端
        if (!stdinText.empty() && stdinPipe[1] >= 0) {
            ssize_t written = write(stdinPipe[1], stdinText.data(), stdinText.size());
            (void)written;
            closeFdSafe(stdinPipe[1]);
        }

        // 等待进程退出（有超时）
        if (timeoutMs <= 0) {
            // 无限等待
            int status = 0;
            waitpid(pid, &status, 0);
            result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        } else {
            const long stepUs = 100000;  // 100ms
            long elapsed = 0;
            bool exited = false;
            while (elapsed < timeoutMs * 1000) {
                int status = 0;
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid) {
                    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    exited = true;
                    break;
                }
                if (w < 0 && errno != EINTR) break;
                usleep(static_cast<useconds_t>(stepUs));
                elapsed += stepUs;
            }
            if (!exited) {
                result.timedOut = true;
                kill(pid, SIGKILL);
                int status = 0;
                waitpid(pid, &status, 0);
            }
        }

        // 读输出
        result.stdoutText = readFd(stdoutPipe[0]);
        result.stderrText = readFd(stderrPipe[0]);
        closeFdSafe(stdoutPipe[0]);
        closeFdSafe(stderrPipe[0]);
        if (!stdinText.empty()) {
            closeFdSafe(stdinPipe[0]);
            closeFdSafe(stdinPipe[1]);
        }

        return core::Result<ProcessResult>::ok(result);
    };
}

} // namespace platform