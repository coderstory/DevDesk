// 编译目标的 OS / 架构名（展示用）：平台分叉的唯一合法落点。
// 业务代码需要 OS/架构名时调这里，禁止再写 _WIN32 / __APPLE__ 判 OS——
// 接口隔离：头文件平台无关，实现按平台拆 *_win / *_posix。
#pragma once

#include <string>

namespace core::platform_info {

// 操作系统名："Windows" / "macOS" / "Linux"
std::string osName();

// 系统架构名："x86_64" / "arm64" / "未知架构"
std::string archName();

} // namespace core::platform_info
