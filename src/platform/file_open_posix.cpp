// src/platform/file_open_posix.cpp
// 其他平台（Linux 等）：openFileFromArgv 不支持返回空；openDirectory 走 xdg-open
#include "file_open.h"
#include "file_open_common.h"

#include <cstdlib>

namespace platform::file_open {

std::string openFileFromArgv() {
    return {};
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
