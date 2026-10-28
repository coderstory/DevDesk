// src/platform/file_open_macos.cpp
// macOS：_NSGetArgc/_NSGetArgv 读取进程命令行（EUI-NEO 的 main 不接 argc/argv）。
// _NSGetArgc/_NSGetArgv 返回的是 argc/argv 的**地址**，必须解引用一次。
#include "file_open.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" int* _NSGetArgc(void);
extern "C" char*** _NSGetArgv(void);

// ── 待 macOS 实机接入：odoc（Finder 双击 / 右键「打开方式」）事件桥 ──
// 这类请求由 LaunchServices 以 kAEOpenDocuments Apple Event 投递，不写进 argv，
// 因此 openFileFromArgv() 接不住。接入方式：在 Cocoa 的
// application:openFile:（或 NSAppleEventManager 的 kAEOpenDocuments 处理器）回调里
// 对每个路径调用 platform::file_open::enqueueOpenFile(path)，由 app 层捕获时取走。
// 本文件刻意不实现该桥：本机无 macOS，未经编译验证的 Objective-C++ 桥风险高于收益；
// 留待 macOS 实机接入并补一次双击/打开方式的真机验证。
// 接入前须确认：①不与 EUI-NEO 现有的 kAEOpenDocuments 处理器重复注册（同一
// event class/id 后注册者覆盖前者，会让 onOpenFile 通道失效）；②运行期事件需要
// 主循环取走点，否则队列只在启动捕获时被消费。

namespace platform::file_open {

std::string openFileFromArgv() {
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    if (argc < 2 || argv == nullptr) return {};

    // argv[0..1] 交共用纯函数选参数（macOS argv 按 UTF-8 处理）
    std::vector<std::string> utf8Args;
    utf8Args.push_back(argv[0] != nullptr ? argv[0] : "");
    utf8Args.push_back(argv[1] != nullptr ? argv[1] : "");
    return selectOpenFileArg(utf8Args);
}

core::Result<void> openDirectory(const std::string& path) {
    if (path.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "目录路径为空");
    }
    // 目录必须先存在（非阻塞派生下 /usr/bin/open 的退出码不可见）
    if (::access(path.c_str(), F_OK) != 0) {
        return core::Result<void>::fail(
            core::ErrorCode::ValidationError,
            "目录不存在: " + path + "（" + std::strerror(errno) + "）");
    }
    // 非阻塞派生：不用 std::system（经 /bin/sh 拼命令，且阻塞等 LaunchServices 握手）；
    // 双 fork 让孙进程被 launchd 收养、不产生僵尸。fork + execl 直传 argv，路径含空格/引号天然安全。
    const pid_t outer = ::fork();
    if (outer < 0) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "fork 失败: " + std::string(std::strerror(errno)));
    }
    if (outer == 0) {
        const pid_t inner = ::fork();
        if (inner < 0) {
            _exit(127);   // 孙进程派生失败（父进程凭 127 识别）
        }
        if (inner == 0) {
            ::execl("/usr/bin/open", "open", path.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);   // exec 失败（路径无效等）
        }
        _exit(0);
    }
    int status = 0;
    while (::waitpid(outer, &status, 0) < 0) {
        if (errno != EINTR) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "waitpid 失败: " + std::string(std::strerror(errno)));
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "/usr/bin/open 派生失败（中间子 fork/execl 阶段）");
    }
    return core::Result<void>::ok();
}

} // namespace platform::file_open
