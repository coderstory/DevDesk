// src/module_settings/local_git_probe_macos.cpp — macOS/POSIX 探测实现。
#include "local_git_probe.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace module::settings {

namespace {

// 子进程读取上限：popen 没有超时原语，xcode-select 若卡住（系统弹窗 /
// CLT 安装态异常）会无限期挂住调用线程——探测挪到后台线程仍会泄漏一个卡死
// 线程且 future 永不就绪。这里 fork/exec + 非阻塞读 + 截止时间，到点 SIGKILL
// 收尸，保证探测恒有上界。
constexpr int kProbeTimeoutMs = 3000;

// 采集上限：探测只读一行路径（PATH_MAX 量级），但病态子进程可能猛写 —— 只留前
// N 字节（多出的继续排空管道但不累积，见 readXcodeSelectPath 的循环注释）。
constexpr size_t kMaxProbeBytes = 4096;

// CLT 安装路径第一行（`xcode-select -p` 输出形如
// /Library/Developer/CommandLineTools 或 Xcode.app 路径）。
bool readXcodeSelectPath(std::string& out) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        // 子进程：stdout → 管道，stderr → /dev/null（等价原 2>/dev/null）
        ::dup2(fds[1], STDOUT_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl("/usr/bin/xcode-select", "xcode-select", "-p",
                static_cast<char*>(nullptr));
        ::_exit(127);   // exec 失败：与「CLT 未安装」同判（不可用）
    }
    ::close(fds[1]);
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    std::string line;
    bool childExited = false;
    bool sawEof = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kProbeTimeoutMs);
    // 每圈**先**过 pipeDrainStep（优先级见其注释）：超时优先于「还有数据要读」。
    // 旧写法把 deadline 检查放在 `n > 0` 的 continue 之后 —— 子进程持续输出时
    // 循环永不查表，3s 上界名存实亡（探测线程卡死、卡片永久「检测中」）。
    // 判据：tests/test_modules/test_local_git_probe.cpp ①。
    PipeDrainStep step = PipeDrainStep::ReadMore;
    while (true) {
        step = pipeDrainStep(childExited, sawEof,
                             std::chrono::steady_clock::now() >= deadline);
        if (step != PipeDrainStep::ReadMore) break;
        char buf[256];
        const ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            // 只留前 kMaxProbeBytes 字节（xcode-select -p 只输出一条路径）；
            // 超出部分继续排空但不累积 —— 防「子进程猛写」把内存也拖崩。
            if (line.size() < kMaxProbeBytes) {
                const size_t room = kMaxProbeBytes - line.size();
                line.append(buf, std::min(static_cast<size_t>(n), room));
            }
            continue;
        }
        if (n == 0) sawEof = true;
        if (!childExited && ::waitpid(pid, &status, WNOHANG) == pid) {
            childExited = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (step == PipeDrainStep::Timeout) {
        ::kill(pid, SIGKILL);   // SIGKILL 后必退，阻塞收尸不会挂
        ::waitpid(pid, &status, 0);
        ::close(fds[0]);
        return false;
    }
    ::close(fds[0]);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        return false;   // 未安装 CLT → 不可用
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    out = line;
    return !line.empty();
}

} // namespace

// xcode-select 随系统基础系统自带，调用它不会触发 CLT 安装 GUI；会弹 GUI 的
// 是 /usr/bin/git 这类 shim——所以全程不执行 git，只查文件存在性。
// 调用方在后台线程跑，本函数自带 3s 上界，可安全反复调用。
bool localGitAvailableImpl() {
    std::string root;
    if (!readXcodeSelectPath(root)) return false;
    std::error_code ec;
    return std::filesystem::exists(root + "/usr/bin/git", ec) && !ec;
}

} // namespace module::settings
