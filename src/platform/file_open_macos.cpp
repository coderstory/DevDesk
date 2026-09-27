// src/platform/file_open_macos.cpp
// macOS 实现：_NSGetArgc/_NSGetArgv 读取进程命令行
// （EUI-NEO 的 main 不接 argc/argv；这是 Apple 自 10.0 起长期稳定的私有 API）
// ⚠️ 签名注意：这两个函数返回的是 argc/argv 的【地址】，必须解引用一次。
#include "file_open.h"
#include "file_open_common.h"

#include <cstdlib>
#include <string>
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
    // /usr/bin/open 退出码 0 = Finder 接受请求；system 失败（无法 fork）= -1
    const int ret = std::system(("/usr/bin/open " + shellSingleQuote(path)).c_str());
    if (ret == 0) {
        return core::Result<void>::ok();
    }
    return core::Result<void>::fail(
        core::ErrorCode::Internal,
        "/usr/bin/open 打开目录失败 (exit=" + std::to_string(ret) + ")");
}

} // namespace platform::file_open
