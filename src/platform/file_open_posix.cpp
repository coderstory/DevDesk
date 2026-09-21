// src/platform/file_open_posix.cpp
// 其他平台（Linux 等）实现：暂不支持，返回空（与原行为一致）
#include "file_open.h"

namespace platform::file_open {

std::string openFileFromArgv() {
    return {};
}

} // namespace platform::file_open
