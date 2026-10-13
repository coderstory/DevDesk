// src/platform/exec_capture_posix.cpp
// POSIX 实现：fork + execvp + waitpid（阻塞等待，不弹窗）。
#include "exec_capture.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace platform {

namespace {

// 把 vector<string> 转成 execvp 可用的 char*[]（末尾 nullptr），内部拷贝。
std::vector<char*> toArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

} // namespace

bool executableOnPath(const std::string& name) {
    // 逐 PATH 目录探测：stat 检查是否存在 + 是否可执行。
    // 空 name 或含 '/' 视为绝对/相对路径，不走 PATH。
    if (name.empty() || name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0;
    }
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || *pathEnv == '\0') return false;
    std::string pathStr(pathEnv);
    size_t pos = 0;
    while (pos <= pathStr.size()) {
        const size_t next = pathStr.find(':', pos);
        const std::string dir = pathStr.substr(pos, next - pos);
        pos = (next == std::string::npos) ? pathStr.size() + 1 : next + 1;
        if (dir.empty()) continue;
        const std::string full = dir + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return true;
    }
    return false;
}

int execCapture(const std::string& workingDir,
                const std::vector<std::string>& args) {
    if (args.empty()) return -1;

    const pid_t pid = fork();
    if (pid < 0) {
        return -1;  // fork 失败
    }
    if (pid == 0) {
        // 子进程
        if (!workingDir.empty()) {
            if (chdir(workingDir.c_str()) != 0) {
                _exit(127);
            }
        }
        // 关闭标准输入/输出/错误（静默丢弃所有输出）
        const int nullFd = open("/dev/null", O_RDWR);
        if (nullFd >= 0) {
            dup2(nullFd, STDIN_FILENO);
            dup2(nullFd, STDOUT_FILENO);
            dup2(nullFd, STDERR_FILENO);
            if (nullFd > 2) close(nullFd);
        }
        auto argv = toArgv(args);
        execvp(args[0].c_str(), argv.data());
        // execvp 只在失败时返回
        _exit(127);
    }
    // 父进程：等待子进程结束
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    // 被信号终止 → 视为失败
    return -1;
}

} // namespace platform