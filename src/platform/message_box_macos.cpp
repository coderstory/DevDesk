// src/platform/message_box_macos.cpp
// macOS：osascript display dialog（无 NSApplication/窗口也能弹系统级对话框，启动期唯一可用的
// 原生模态通道）。子进程用 posix_spawn + pipe 读 stdout，不走 system()（shell 引注入）。
// 本文件只在 macOS 编译（CMake target_sources 按平台选择），Windows/CI 无法运行验证——
// 改动须保持「每个系统调用都检查返回值、失败必 LOG」的纪律，并注明无法实测的边界。
#include "message_box.h"
#include "core/logging.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace platform::msgbox {
namespace {

// AppleScript 字符串字面量转义：\ 与 " 前加反斜杠；换行换成空格（display dialog
// 字面量不接受裸换行）。文案由调用方给定，无用户数据，转义只为合法不为安全
std::string applescriptEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += ' '; break;
            default:   out += c; break;
        }
    }
    return out;
}

} // namespace

Choice show(const std::string& title, const std::string& message, bool withCancel) {
    // 双按钮：Ok=还原（default button 让回车=还原）；单按钮：仅知道了（恒 Ok）
    const std::string script =
        "display dialog \"" + applescriptEscape(message) + "\"" +
        " with title \"" + applescriptEscape(title) + "\"" +
        " with icon critical buttons " +
        (withCancel ? "{\"退出\", \"还原\"} default button \"还原\""
                    : "{\"知道了\"} default button \"知道了\"");

    int outPipe[2];
    if (pipe(outPipe) != 0) {
        LOG_WARN("msgbox: pipe 失败: " + std::string(std::strerror(errno)));
        return Choice::Cancel;
    }
    int errPipe[2];
    if (pipe(errPipe) != 0) {
        LOG_WARN("msgbox: errPipe 失败: " + std::string(std::strerror(errno)));
        close(outPipe[0]);
        close(outPipe[1]);
        return Choice::Cancel;
    }
    posix_spawn_file_actions_t actions;
    // file_actions 系列每个调用都可能失败（内存/句柄上限）：失败时用户看不到任何
    // 对话框，必须 LOG 后按 Cancel 走（调用方对 Cancel 的语义就是退出，安全缺省）
    if (posix_spawn_file_actions_init(&actions) != 0) {
        LOG_WARN("msgbox: posix_spawn_file_actions_init 失败");
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return Choice::Cancel;
    }
    if (posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO) != 0) {
        LOG_WARN("msgbox: posix_spawn_file_actions_adddup2 失败");
        posix_spawn_file_actions_destroy(&actions);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return Choice::Cancel;
    }

    pid_t pid = -1;
    const char* argv[] = {"/usr/bin/osascript", "-e", script.c_str(), nullptr};
    const int spawnRc = posix_spawn(&pid, "/usr/bin/osascript", &actions, nullptr,
                                    const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    close(errPipe[1]);
    if (spawnRc != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        LOG_WARN("msgbox: posix_spawn osascript 失败: " + std::string(std::strerror(spawnRc)));
        return Choice::Cancel;
    }

    std::string output;
    std::string errOutput;
    char buf[512];
    ssize_t n = 0;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    close(outPipe[0]);
    while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) {
        errOutput.append(buf, static_cast<size_t>(n));
    }
    close(errPipe[0]);
    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    if (waited != pid) {
        // waitpid 失败（-1）/ 被信号中断后不可重试：子进程状态未知，按 Cancel 兜底并 LOG
        LOG_ERROR("msgbox: waitpid 失败: " + std::string(std::strerror(errno)));
        return Choice::Cancel;
    }
    // exit != 0 的两种来源：① 用户按取消（display dialog cancel 抛 "User canceled. (-128)"，
    // 这是正常用户动作，静默按 Cancel）；② osascript 运行期失败（无 GUI 会话/自动化权限被拒/
    // 被策略禁用——此刻用户根本没看到对话框，静默降级成「退出」违背「损坏必须弹原生对话框」
    // 的需求，必须 LOG_ERROR 暴露）。两者靠 stderr 内容区分。
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            LOG_ERROR("msgbox: osascript 被信号杀死 (signal " +
                      std::to_string(WTERMSIG(status)) + "): " + errOutput);
        } else if (errOutput.find("User canceled") == std::string::npos) {
            LOG_ERROR("msgbox: osascript 运行期失败（用户未见到对话框，按 Cancel/退出兜底）: " +
                      errOutput);
        }
        return Choice::Cancel;
    }
    // stdout 形如 "button returned:还原"
    return output.find("button returned:还原") != std::string::npos ? Choice::Ok
                                                                    : Choice::Cancel;
}

} // namespace platform::msgbox
