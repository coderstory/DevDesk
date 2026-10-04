// src/platform/file_open_macos.cpp
// macOS：_NSGetArgc/_NSGetArgv 读取进程命令行（EUI-NEO 的 main 不接 argc/argv）。
// ⚠️ 二者返回的是 argc/argv 的**地址**，必须解引用一次。
#include "file_open.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" int* _NSGetArgc(void);
extern "C" char*** _NSGetArgv(void);

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
