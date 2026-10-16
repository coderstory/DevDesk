// src/module_settings/local_git_probe_macos.cpp — macOS/POSIX 探测实现。
#include "local_git_probe.h"

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
    while (!(childExited && sawEof)) {
        char buf[256];
        const ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            line.append(buf, static_cast<size_t>(n));
            continue;   // 有数据可读就先读干净
        }
        if (n == 0) sawEof = true;
        if (!childExited && ::waitpid(pid, &status, WNOHANG) == pid) {
            childExited = true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);   // SIGKILL 后必退，阻塞收尸不会挂
            ::waitpid(pid, &status, 0);
            ::close(fds[0]);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
