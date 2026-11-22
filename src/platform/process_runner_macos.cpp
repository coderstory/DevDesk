// src/platform/process_runner_macos.cpp
// macOS 进程执行器：posix_spawn + 管道全捕获（stdin/stdout/stderr）。
// 超时 = 输出管道排空 + waitpid WNOHANG 轮询并行（排空优先，防 >64KB 输出互锁）；超时到期 kill 强杀。
// 线程安全：每次调用独立 posix_spawn，runner 自身无共享状态。
#include "process_runner.h"

#include "process_runner_common.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/logging.h"

extern char** environ;

namespace platform {

namespace {

void closeFdSafe(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

// 构建 argv 的 const char* 数组（posix_spawn 需要）
std::vector<const char*> buildArgvPtr(const std::vector<std::string>& argv) {
    std::vector<const char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (const auto& a : argv) ptrs.push_back(a.c_str());
    ptrs.push_back(nullptr);
    return ptrs;
}

// 裸命令名（不含 '/'）按 PATH 查找解析为可执行文件路径。
// 为什么在执行器层做：posix_spawn 语义等同 execv，不做 PATH 查找，裸命令名
// 必 ENOENT；而 Windows CreateProcess 与 POSIX 兜底 execvp 都会搜 PATH。
// 在这里补齐后三平台「裸命令名可 spawn」行为一致（agent 内置 git 八件套
// 传的就是裸名 "git"）。含 '/' 的路径原样返回；找不到返回空串。
std::string resolveExecutablePath(const std::string& cmd) {
    if (cmd.find('/') != std::string::npos) return cmd;
    const char* pathEnv = ::getenv("PATH");
    if (pathEnv == nullptr) return "";
    const std::string path = pathEnv;
    struct stat st;
    size_t start = 0;
    while (start <= path.size()) {
        size_t sep = path.find(':', start);
        if (sep == std::string::npos) sep = path.size();
        std::string dir = path.substr(start, sep - start);
        if (dir.empty()) dir = ".";  // PATH 里的空条目按惯例指当前目录
        const std::string candidate = dir + "/" + cmd;
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            ::access(candidate.c_str(), X_OK) == 0) {
            if (candidate[0] == '/') return candidate;
            // PATH 条目是相对路径时按调用方 cwd 钉成绝对——后续可能 chdir 到
            // 目标 cwd 再 spawn，相对形态会错位
            char cwdBuf[PATH_MAX] = {};
            if (getcwd(cwdBuf, sizeof(cwdBuf)) != nullptr) {
                return std::string(cwdBuf) + "/" + candidate;
            }
            return candidate;
        }
        start = sep + 1;
    }
    return "";
}

} // namespace

bool processRunnerSupported() { return true; }

std::string defaultShellPath() { return "/bin/zsh"; }

ProcessRunner systemProcessRunner() {
    if (ProcessRunner& injected = systemProcessRunnerForTest()) return injected;
    return [](const std::vector<std::string>& argv, const std::string& stdinText,
              const std::string& cwd, long timeoutMs) -> core::Result<ProcessResult> {
        if (argv.empty()) {
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::ValidationError, "进程执行器：argv 为空");
        }

        // 裸命令名先解析成可执行文件路径（posix_spawn 不搜 PATH，见 helper 注释）。
        // 放在管道创建之前，失败时不产生需要清理的 fd。
        const std::string spawnPath = resolveExecutablePath(argv[0]);
        if (spawnPath.empty()) {
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::NotFound,
                "进程执行器：命令未找到（PATH 查找失败）: " + argv[0]);
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
            int spawnErr = posix_spawn(&pid, spawnPath.c_str(),
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
            int spawnErr = posix_spawn(&pid, spawnPath.c_str(),
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

        // 排空 + 收尸主循环在 process_runner_common（need:380 批C item6）；
        // 平台刻意差异经开关保真（债-20 a/b，见 common 文件头）。
        return process_runner::drainAndReap(
            {pid, stdoutPipe[0], stderrPipe[0], stdinPipe[0], stdinPipe[1],
             !stdinText.empty()},
            argv[0], timeoutMs,
            /*closeStdinOnWaitError=*/false,
            /*warnOnSigkillReapFailure=*/true, &result);
    };
}

} // namespace platform