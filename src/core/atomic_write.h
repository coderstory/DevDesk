// src/core/atomic_write.h
// 文件原子写（唯一实现）：仓内曾散着 3 份同模式拷贝且第 4 处漏改，故下沉 core 供 module_* 复用
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include "result.h"

namespace core {

// 原子写：content 逐字节写入 target。实现 = 同目录临时文件 <target>.devdesk-tmp → 覆盖式替换。
// 成功后内容完全一致、无残留；失败时目标保持原内容（不会被截断半截）、临时文件已清理。
// 父目录须调用方保证存在。label 仅用于错误信息前缀。不抛异常（项目铁律）
core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label = "文件");

// 覆盖式替换（平台适配实现，CMake 按平台选源）：把 tmp 移到 target，已存在时按平台语义覆盖；
// 失败填 ec。Windows remove(target) 再 rename（历史兼容路径，行为冻结）；POSIX 单次 rename
//（原子替换无窗口期）。仅供 atomicWriteFile 调用（core 内部细节）
namespace detail {

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec);

// 让 tmp 以「仅属主可读写」的权限就位（不写内容），供 atomicWriteFile 在打开写句柄前先调。
// POSIX 上 ofstream 建文件时权限位由 umask 决定（典型 0644）且在 open() 那刻定死，事后 chmod
// 追不回（别的用户已 open 的 fd 仍可读），故收紧必须在 open(O_CREAT) 之前。存在即 false 并填 ec。
// POSIX：open(O_CREAT|O_EXCL, S_IRUSR|S_IWUSR) 新建，已存在（上轮崩溃残留）则先 permissions(replace)
// 收紧再复用。Windows：权限模型是 ACL 不是模式位，数据根在 %USERPROFILE% 下、NTFS 继承默认 ACL
// 已是「仅本用户 + Administrators/SYSTEM」，此处无 chmod 可做，只固定「先建后写」次序（不做额外加固）
bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec);

} // namespace detail

} // namespace core
