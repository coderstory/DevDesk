// 文件原子写（唯一实现），供 module_* 复用
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include "result.h"

namespace core {

// 原子写：content 逐字节写入 target = 同目录临时文件 <target>.devdesk-tmp → 覆盖式替换。
// 成功后内容完全一致、无残留；失败时目标保持原内容、临时文件已清理。父目录须调用方保证存在。
// label 仅用于错误信息前缀。不抛异常（项目铁律）
core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label = "文件");

// 覆盖式替换（平台适配实现，CMake 按平台选源）：把 tmp 移到 target，已存在时按平台语义覆盖；
// 失败填 ec。Windows 先 remove 再 rename；POSIX 单次 rename（原子替换无窗口期）。仅供 atomicWriteFile 调用
namespace detail {

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec);

// 让 tmp 以「仅属主可读写」就位（不写内容），必须在打开写句柄（open O_CREAT）之前调：POSIX 权限位
// 由 umask 在 open 那刻定死，事后 chmod 追不回（已 open 的 fd 仍可读）；已存在（崩溃残留）则先收紧
// 再复用。Windows 是 ACL 模型（NTFS 继承默认已收紧），只固定「先建后写」次序。存在即 false 并填 ec
bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec);

} // namespace detail

} // namespace core
