// src/platform/file_open_posix.cpp
// 其他平台（Linux 等）实现：openFileFromArgv 暂不支持返回空（与原行为一致）；
// openDirectory 走 xdg-open
#include "file_open.h"

#include <cstdlib>

namespace platform::file_open {

std::string openFileFromArgv() {
    return {};
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
    const int ret = std::system(("xdg-open " + shellSingleQuote(path)).c_str());
    if (ret == 0) {
        return core::Result<void>::ok();
    }
    return core::Result<void>::fail(
        core::ErrorCode::Internal,
        "xdg-open 打开目录失败 (exit=" + std::to_string(ret) + ")");
}

} // namespace platform::file_open
