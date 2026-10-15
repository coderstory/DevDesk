// src/platform/message_box_macos.cpp
// macOS：osascript display dialog（无 NSApplication/窗口也能弹系统级对话框，启动期唯一可用的
// 原生模态通道）。子进程用 posix_spawn + pipe 读 stdout，不走 system()（shell 引注入）。
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
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);

    pid_t pid = -1;
    const char* argv[] = {"/usr/bin/osascript", "-e", script.c_str(), nullptr};
    const int spawnRc = posix_spawn(&pid, "/usr/bin/osascript", &actions, nullptr,
                                    const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    if (spawnRc != 0) {
        close(outPipe[0]);
        LOG_WARN("msgbox: posix_spawn osascript 失败: " + std::string(std::strerror(spawnRc)));
        return Choice::Cancel;
    }

    std::string output;
    char buf[512];
    ssize_t n = 0;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    close(outPipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    // exit != 0 = 用户按了取消（display dialog cancel button 抛错）或 osascript 本身失败——
    // 两种情况都按 Cancel 处理（调用方语义：退出），失败已由空输出兜底
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return Choice::Cancel;
    }
    // stdout 形如 "button returned:还原"
    return output.find("button returned:还原") != std::string::npos ? Choice::Ok
                                                                   : Choice::Cancel;
}

} // namespace platform::msgbox
