// src/platform/process_host_posix.cpp
// POSIX 兜底实现：fork/setsid/execvp + 进程组 SIGTERM。macOS/Linux 共用；
// 本仓库当前仅在 Windows 实机验证（DevDesk 交付平台），POSIX 侧按 TODO 口径待 mac 实测。
#include "process_host.h"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <vector>

namespace platform::process_host {

namespace {
// SIGCHLD 显式忽略 = 子进程退出由内核自动收尸：托管的长驻进程不留 zombie，
// isAlive 的 kill(pid,0) 也不会把 zombie 误判成存活
struct SigchldIgnorer {
    SigchldIgnorer() { ::signal(SIGCHLD, SIG_IGN); }
};
const SigchldIgnorer g_sigchldIgnorer;
} // namespace

core::Result<std::int64_t> spawnDetached(
    const std::vector<std::string>& argv,
    const std::vector<std::string>& envPairs,
    const std::string& workingDir) {
    if (argv.empty() || argv[0].empty()) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::Internal,
                                        "spawnDetached: argv 为空");
    }
    const pid_t child = fork();
    if (child < 0) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::Internal,
                                        "spawnDetached: fork 失败");
    }
    if (child == 0) {
        // per-child 环境构造（与 process_host_win 的 per-child 环境块语义对齐）：
        // 同名覆盖继承值，其余原样继承。密钥红线（telemem_config.h 契约）——
        // ANTHROPIC_API_KEY 等注入项只允许存在于「子进程环境」一条通道；严禁把
        // setenv 挪回 fork 之前的父进程（key 会常驻 DevDesk 主进程并被后续所有
        // 子进程——终端/git/sidecar——继承，等价于密钥外泄面）。
        for (const std::string& pair : envPairs) {
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            setenv(pair.substr(0, eq).c_str(), pair.substr(eq + 1).c_str(), 1);
        }
        // 子进程：新会话 + 新进程组（terminateTree 依赖 pgid == pid 杀全组）
        (void)setsid();
        if (!workingDir.empty()) {
            (void)chdir(workingDir.c_str());
        }
        // 常驻进程不接管道：stdio 重挂 /dev/null，输出丢弃
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const std::string& a : argv) {
            cargv.push_back(const_cast<char*>(a.c_str()));
        }
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    return core::Result<std::int64_t>::ok(static_cast<std::int64_t>(child));
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
