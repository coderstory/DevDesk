// src/platform/file_open_macos.cpp
// macOS 实现：_NSGetArgc/_NSGetArgv 读取进程命令行
// （EUI-NEO 的 main 不接 argc/argv；这是 Apple 自 10.0 起长期稳定的私有 API）
// ⚠️ 签名注意：这两个函数返回的是 argc/argv 的【地址】，必须解引用一次。
#include "file_open.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// macOS 私有 API：获取进程的 argc/argv 地址
extern "C" int* _NSGetArgc(void);
extern "C" char*** _NSGetArgv(void);

namespace platform::file_open {

std::string openFileFromArgv() {
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    if (argc < 2 || argv == nullptr) return {};

    // argv[0..1] 交共用纯函数选文件参数（macOS argv 按 UTF-8 处理）
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
    // 目录必须先存在（与 Windows 侧 ShellExecuteExW 对齐；旧实现靠
    // /usr/bin/open 的退出码兜这个错，改非阻塞后退出码不可见）
    if (::access(path.c_str(), F_OK) != 0) {
        return core::Result<void>::fail(
            core::ErrorCode::ValidationError,
            "目录不存在: " + path + "（" + std::strerror(errno) + "）");
    }
    // 审计 N2：原 std::system（经 /bin/sh 拼命令）会阻塞调用线程等整段
    // LaunchServices 握手，且只传回退出码。改为 fork + execl 非阻塞派生：
    //   - argv 直接传参、不经 shell：路径含空格/引号天然安全，
    //     不再需要 shellSingleQuote 转义；
    //   - 双 fork：中间子 fork 出真正跑 open 的孙进程后立即退出，由本函数
    //     waitpid 收口（微秒级）；孙进程随后被 launchd 收养、退出即回收，
    //     不产生僵尸进程；
    //   - 返回语义与 Windows 侧一致：「派生成功 = LaunchServices 已受理」，
    //     不承诺 Finder 窗口真的弹出（打开结果无法同步观察）。
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
