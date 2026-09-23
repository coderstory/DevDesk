// src/platform/file_open_macos.cpp
// macOS 实现：_NSGetArgc/_NSGetArgv 读取进程命令行
// （EUI-NEO 的 main 不接 argc/argv；这是 Apple 自 10.0 起长期稳定的私有 API）
// ⚠️ 签名注意：这两个函数返回的是 argc/argv 的【地址】，必须解引用一次。
#include "file_open.h"

#include <cstdlib>
#include <string>

// macOS 私有 API：获取进程的 argc/argv 地址
extern "C" int* _NSGetArgc(void);
extern "C" char*** _NSGetArgv(void);

namespace platform::file_open {

std::string openFileFromArgv() {
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    if (argc < 2 || argv == nullptr) return {};

    // argv[1] 是文件路径（命令行直接调用二进制时传入）
    const char* path = argv[1];
    if (path == nullptr || path[0] == '\0') return {};

    // 跳过 flag 类参数（以 '-' 开头）
    if (path[0] == '-') return {};

    return std::string(path);
}

// shell 单引号转义（' 拆成 '\''）：路径含空格/引号不破坏命令边界
static std::string shellSingleQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
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
