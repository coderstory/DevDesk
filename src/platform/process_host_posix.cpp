// src/platform/process_host_posix.cpp
// POSIX 实现：double-fork + setsid + execvp + 进程组 SIGTERM。macOS/Linux 共用。
// 本仓库当前在 macOS arm64 实机运行（POSIX 主平台），Windows 走 process_host_win。
#include "process_host.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/logging.h"

namespace platform::process_host {

namespace {

// 中间子进程的约定退出码：fork 孙进程失败（127 留给孙进程 exec 失败，语义与
// 单 fork 版一致）。
constexpr int kExitMiddleForkFailed = 121;

// 为什么不用 signal(SIGCHLD, SIG_IGN)（历史版本 ccbeaac2 的做法）：
// POSIX 语义是 SIGCHLD 置 SIG_IGN 后内核自动收尸所有子进程，父进程一切
// waitpid 必返 -1/ECHILD —— 连带 exec_capture / process_runner / message_box
// 全部拿不到子进程退出码（bash.run 误判超时、git 自动提交误报失败）。
// 改 double-fork：中间子进程由本函数 waitpid 收尸（不留 zombie），工作孙进程
// 被 launchd 收养、死后由 launchd 收尸 —— 同样无 zombie，且不动全局信号状态。
// 同构先例：src/platform/file_open_macos.cpp openDirectory 的双 fork。

} // namespace

core::Result<std::int64_t> spawnDetached(
    const std::vector<std::string>& argv,
    const std::vector<std::string>& envPairs,
    const std::string& workingDir) {
    if (argv.empty() || argv[0].empty()) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::Internal,
                                        "spawnDetached: argv 为空");
    }

    // 孙 pid 回传管道：double-fork 后孙 pid 只有中间子进程知道，fork 前建管道，
    // 中间进程写回、父进程读取。FD_CLOEXEC 保证 exec 时自动关闭，不泄漏 fd 给
    // 常驻工作进程；中间进程写完即关 + _exit，父进程 read 的 EOF 语义 = 失败。
    int pidPipe[2] = {-1, -1};
    if (::pipe(pidPipe) != 0) {
        return core::Result<std::int64_t>::fail(
            core::ErrorCode::Internal,
            "spawnDetached: pipe 失败: " + std::string(std::strerror(errno)));
    }
    (void)::fcntl(pidPipe[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(pidPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t middle = ::fork();
    if (middle < 0) {
        ::close(pidPipe[0]);
        ::close(pidPipe[1]);
        return core::Result<std::int64_t>::fail(
            core::ErrorCode::Internal,
            "spawnDetached: fork 失败: " + std::string(std::strerror(errno)));
    }
    if (middle == 0) {
        // ── 中间子进程：fork 出工作孙进程后立即退出（父进程收尸它）──
        ::close(pidPipe[0]);
        const pid_t grandchild = ::fork();
        if (grandchild < 0) {
            ::close(pidPipe[1]);
            ::_exit(kExitMiddleForkFailed);
        }
        if (grandchild == 0) {
            // ── 工作孙进程（过继给 launchd）──
            ::close(pidPipe[1]);  // 不持有回传管道
            // per-child 环境构造（与 process_host_win 的 per-child 环境块语义对齐）：
            // 同名覆盖继承值，其余原样继承。密钥红线（telemem_config.h 契约）——
            // ANTHROPIC_API_KEY 等注入项只允许存在于「子进程环境」一条通道；严禁把
            // setenv 挪回 fork 之前的父进程（key 会常驻 DevDesk 主进程并被后续所有
            // 子进程——终端/git/sidecar——继承，等价于密钥外泄面）。
            for (const std::string& pair : envPairs) {
                const size_t eq = pair.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                ::setenv(pair.substr(0, eq).c_str(), pair.substr(eq + 1).c_str(), 1);
            }
            // 新会话 + 新进程组（terminateTree 依赖 pgid == 孙 pid 杀全组）
            (void)::setsid();
            if (!workingDir.empty()) {
                (void)::chdir(workingDir.c_str());
            }
            // 常驻进程不接管道：stdio 重挂 /dev/null，输出丢弃
            (void)::freopen("/dev/null", "r", stdin);
            (void)::freopen("/dev/null", "w", stdout);
            (void)::freopen("/dev/null", "w", stderr);
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (const std::string& a : argv) {
                cargv.push_back(const_cast<char*>(a.c_str()));
            }
            cargv.push_back(nullptr);
            ::execvp(cargv[0], cargv.data());
            ::_exit(127);
        }
        // 写回孙 pid 后立即 _exit：父进程 waitpid 收尸中间进程，孙进程被
        // launchd 收养，两端都不留 zombie。
        const std::int64_t pidOut = static_cast<std::int64_t>(grandchild);
        const ssize_t written = ::write(pidPipe[1], &pidOut, sizeof(pidOut));
        (void)written;  // 8 字节 ≤ PIPE_BUF，原子写不会短写；失败由 EOF 路径兜底
        ::close(pidPipe[1]);
        ::_exit(0);
    }

    // ── 父进程：读回孙 pid → 收尸中间子进程 ──
    ::close(pidPipe[1]);

    std::int64_t grandchildPid = -1;
    bool gotPid = false;
    while (true) {
        const ssize_t n =
            ::read(pidPipe[0], &grandchildPid, sizeof(grandchildPid));
        if (n == static_cast<ssize_t>(sizeof(grandchildPid))) {
            gotPid = true;
            break;
        }
        if (n == 0) break;                      // 中间进程未写即退出 = 失败
        if (n < 0 && errno == EINTR) continue;  // 信号打断：重试
        break;                                  // 其它 read 错误按失败走（下方仍收尸）
    }
    ::close(pidPipe[0]);

    // 收尸中间子进程——无论成败都必须收，不留 zombie（EINTR 重试）
    int status = 0;
    while (::waitpid(middle, &status, 0) < 0) {
        if (errno == EINTR) continue;
        LOG_ERROR(std::string("spawnDetached: waitpid 中间子进程失败: ") +
                  std::strerror(errno));
        break;
    }

    if (!gotPid || grandchildPid <= 1) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == kExitMiddleForkFailed) {
            // fork 的 errno 发生在中间子进程内，无法回传父进程，只报事实
            return core::Result<std::int64_t>::fail(
                core::ErrorCode::Internal,
                "spawnDetached: 孙进程 fork 失败（中间进程退出码 121）");
        }
        return core::Result<std::int64_t>::fail(
            core::ErrorCode::Internal,
            "spawnDetached: 中间子进程未回传孙进程 pid（退出状态 " +
                std::to_string(status) + "）");
    }
    return core::Result<std::int64_t>::ok(grandchildPid);
}

core::Result<void> terminateTree(std::int64_t pid) {
    if (pid <= 1) return core::Result<void>::ok();
    // setsid 后 pgid == pid：负号 = 杀全组（含后代）
    if (::kill(static_cast<pid_t>(-pid), SIGTERM) != 0 && errno == ESRCH) {
        (void)::kill(static_cast<pid_t>(pid), SIGTERM);
    }
    return core::Result<void>::ok();
}

bool isAlive(std::int64_t pid) {
    if (pid <= 1) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

} // namespace platform::process_host
