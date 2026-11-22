#include "platform/process_runner_common.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/logging.h"

namespace platform::process_runner {

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

}  // namespace

core::Result<ProcessResult> drainAndReap(
    DrainReapIO io, const std::string& argv0, int timeoutMs,
    bool closeStdinOnWaitError, bool warnOnSigkillReapFailure,
    ProcessResult* result) {
    // 排空 + 收尸一体：两输出管道全部读到 EOF 才算完。管道没排空前绝不
    // 阻塞等退出——输出一旦超管道缓冲（64KB 量级），先等退出会让子进程
    // 卡在 write、父进程卡在 wait 互锁：设超时被误杀+输出截断，不设永挂。
    // 收尸后 EOF 只再等 kReapGraceMs（债-20 ①）：孙进程持写端时 EOF 遥遥
    // 无期，主进程结果此刻已完整，宽限到点强关走人，不随孙进程挂满全程。
    if (io.stdoutFd >= 0) setNonBlocking(io.stdoutFd);
    if (io.stderrFd >= 0) setNonBlocking(io.stderrFd);
    bool outOpen = io.stdoutFd >= 0;
    bool errOpen = io.stderrFd >= 0;
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
            pfds[nfds++] = {io.stdoutFd, POLLIN, 0};
        }
        if (errOpen) {
            errIdx = nfds;
            pfds[nfds++] = {io.stderrFd, POLLIN, 0};
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
                    outOpen = drainOnce(io.stdoutFd, result->stdoutText);
                if (errIdx >= 0 &&
                    (pfds[errIdx].revents &
                     (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
                    errOpen = drainOnce(io.stderrFd, result->stderrText);
            } else if (pr < 0 && errno != EINTR) {
                // poll 异常（不该发生）：强关两管道防死循环，错误事实留给 waitpid
                closeFdSafe(io.stdoutFd);
                outOpen = false;
                closeFdSafe(io.stderrFd);
                errOpen = false;
            }
        }
        if (exited) {
            if (!outOpen && !errOpen) break;  // 两管道 EOF：正常收尾
            if (std::chrono::steady_clock::now() >= reapDeadline) {
                // 宽限到点 EOF 未至（孙进程持写端）：强关走人，结果已完整
                closeFdSafe(io.stdoutFd);
                outOpen = false;
                closeFdSafe(io.stderrFd);
                errOpen = false;
                break;
            }
            continue;  // 宽限内继续拖 EOF（poll 20ms 节流）
        }
        pid_t w = waitpid(io.pid, &status, WNOHANG);
        if (w == io.pid) {
            exited = true;
            reapDeadline =
                std::chrono::steady_clock::now() + kReapGraceMs;
            continue;
        }
        if (w < 0 && errno != EINTR) {
            // fail 路径同步关掉输出管道
            closeFdSafe(io.stdoutFd);
            closeFdSafe(io.stderrFd);
            if (closeStdinOnWaitError && io.stdinActive) {
                closeFdSafe(io.stdinReadFd);
                closeFdSafe(io.stdinWriteFd);
            }
            if (errno == ECHILD) {
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：waitpid ECHILD —— 子进程已被收尸"
                    "（SIGCHLD 全局被忽略或重复收尸），命令: " + argv0);
            }
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：waitpid 失败: " + std::string(strerror(errno)) +
                    "，命令: " + argv0);
        }
        if (!exited && std::chrono::steady_clock::now() >= deadline) {
            result->timedOut = true;
            kill(io.pid, SIGKILL);
            int killStatus = 0;
            pid_t wk;
            do {
                wk = waitpid(io.pid, &killStatus, 0);
            } while (wk < 0 && errno == EINTR);
            if (wk < 0) {
                if (warnOnSigkillReapFailure) {
                    // SIGKILL 后收尸失败 = 有并发收尸（异常信号面），记日志留痕；
                    // 超时事实已成立，不再改判失败
                    LOG_WARN("进程执行器：SIGKILL 后 waitpid 失败: " +
                             std::string(strerror(errno)) + "，命令: " + argv0);
                } else {
                    break;  // 收尸失败罕见且无恢复动作；timedOut 已成立
                }
            }
            result->exitCode =
                WIFEXITED(killStatus) ? WEXITSTATUS(killStatus) : -1;
            exited = true;
            reapDeadline =
                std::chrono::steady_clock::now() + kReapGraceMs;
        }
    }
    if (!result->timedOut) {
        result->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    if (io.stdinActive) {
        closeFdSafe(io.stdinReadFd);
        closeFdSafe(io.stdinWriteFd);
    }

    return core::Result<ProcessResult>::ok(*result);
}

}  // namespace platform::process_runner
