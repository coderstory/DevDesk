// src/platform/folder_picker_posix.cpp
// POSIX 通用 stub：无 GUI 环境返回「当前平台不支持目录选择」
#include "folder_picker.h"

namespace platform::folderpicker {

core::Result<std::string> pickFolder(const std::string& /*title*/,
                                     const std::string& /*startDir*/) {
    return core::Result<std::string>::fail(
        core::ErrorCode::NotImplemented,
        "当前平台不支持目录选择");
}

} // namespace platform::folderpicker