// src/platform/process_runner_macos.cpp
// macOS 进程执行器：posix_spawn + 管道全捕获（stdin/stdout/stderr）。
// 超时 = 输出管道排空 + waitpid WNOHANG 轮询并行（排空优先，防 >64KB 输出互锁）；超时到期 kill 强杀。
// 线程安全：每次调用独立 posix_spawn，runner 自身无共享状态。
#include "process_runner.h"

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

        // 排空 + 收尸一体：两输出管道全部读到 EOF 才算完。管道没排空前绝不
        // 阻塞等退出——输出一旦超管道缓冲（64KB 量级），先等退出会让子进程
        // 卡在 write、父进程卡在 wait 互锁：设超时被误杀+输出截断，不设永挂。
        // 收尸后 EOF 只再等 kReapGraceMs（债-20 ①）：孙进程持写端时 EOF 遥遥
        // 无期，主进程结果此刻已完整，宽限到点强关走人，不随孙进程挂满全程。
        if (stdoutPipe[0] >= 0) setNonBlocking(stdoutPipe[0]);
        if (stderrPipe[0] >= 0) setNonBlocking(stderrPipe[0]);
        bool outOpen = stdoutPipe[0] >= 0;
        bool errOpen = stderrPipe[0] >= 0;
        constexpr auto kReapGraceMs = std::chrono::milliseconds(2000);
        const auto deadline =
            timeoutMs > 0
                ? std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(timeoutMs)
                : std::chrono::steady_clock::time_point::max();
        auto reapDeadline = std::chrono::steady_clock::time_point::max();
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
                    // POLLERR/POLLNVAL 也要排水（债-20 ②）：只认 POLLIN|POLLHUP 会让
                    // 坏 fd 永远留在 poll 集里 20ms 空转死循环；drainOnce 对坏 fd
                    // read 一次即出错收尾，两种 revents 统一走它闭环。
                    if (outIdx >= 0 &&
                        (pfds[outIdx].revents &
                         (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
                        outOpen = drainOnce(stdoutPipe[0], result.stdoutText);
                    if (errIdx >= 0 &&
                        (pfds[errIdx].revents &
                         (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
                        errOpen = drainOnce(stderrPipe[0], result.stderrText);
                } else if (pr < 0 && errno != EINTR) {
                    // poll 异常（不该发生）：强关两管道防死循环，错误事实留给 waitpid
                    closeFdSafe(stdoutPipe[0]);
                    outOpen = false;
                    closeFdSafe(stderrPipe[0]);
                    errOpen = false;
                }
            }
            if (exited) {
                if (!outOpen && !errOpen) break;  // 两管道 EOF：正常收尾
                if (std::chrono::steady_clock::now() >= reapDeadline) {
                    // 宽限到点 EOF 未至（孙进程持写端）：强关走人，结果已完整
                    closeFdSafe(stdoutPipe[0]);
                    outOpen = false;
                    closeFdSafe(stderrPipe[0]);
                    errOpen = false;
                    break;
                }
                continue;  // 宽限内继续拖 EOF（poll 20ms 节流）
            }
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exited = true;
                reapDeadline =
                    std::chrono::steady_clock::now() + kReapGraceMs;
                continue;
            }
            if (w < 0 && errno != EINTR) {
                if (errno == ECHILD) {
                    return core::Result<ProcessResult>::fail(
                        core::ErrorCode::Internal,
                        "进程执行器：waitpid ECHILD —— 子进程已被收尸"
                        "（SIGCHLD 全局被忽略或重复收尸），命令: " + argv[0]);
                }
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：waitpid 失败: " + std::string(strerror(errno)) +
                        "，命令: " + argv[0]);
            }
            if (!exited && std::chrono::steady_clock::now() >= deadline) {
                result.timedOut = true;
                kill(pid, SIGKILL);
                int killStatus = 0;
                pid_t wk;
                do {
                    wk = waitpid(pid, &killStatus, 0);
                } while (wk < 0 && errno == EINTR);
                if (wk < 0) {
                    // SIGKILL 后收尸失败 = 有并发收尸（异常信号面），记日志留痕；
                    // 超时事实已成立，不再改判失败
                    LOG_WARN("进程执行器：SIGKILL 后 waitpid 失败: " +
                             std::string(strerror(errno)) + "，命令: " + argv[0]);
                }
                result.exitCode =
                    WIFEXITED(killStatus) ? WEXITSTATUS(killStatus) : -1;
                exited = true;
                reapDeadline =
                    std::chrono::steady_clock::now() + kReapGraceMs;
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