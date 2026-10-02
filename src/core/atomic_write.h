// src/core/atomic_write.h
// 文件原子写（唯一实现）。此前仓内散着 3 份同模式拷贝（module_claude_proxy
// settings_sync::atomicWrite / module_markdown atomicWriteFile / module_settings
// atomicWriteConfig），第 4 处调用点（module_todo 的视图状态落盘）漏改仍用
// ofstream(trunc) 原地写——2026-09-27 审计 THS-03 据此下沉 core，供 module_*
// 复用（module_* 互不依赖，公共 util 只能落在 core）。
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include "result.h"

namespace core {

// 原子写：把 content 逐字节写入 target。
// 实现 = 同目录临时文件 <target>.devdesk-tmp → 覆盖式替换（core::detail::replaceFile：
// 平台语义差异收在该函数的两个平台实现里，见下方声明）。
// 成功后目标内容与 content 完全一致、无临时文件残留；失败时目标保持原有内容
// （不会被截断成半截），临时文件已清理。父目录需调用方自行保证存在（与本工具的
// 前身拷贝语义一致）。
// label 仅用于错误信息前缀（如 "配置文件" → "配置文件写入失败: ..."）。
// 不抛异常（项目铁律），全部走 std::error_code 重载。
core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label = "文件");

// 覆盖式替换（平台适配实现：本头文件平台无关，实现按平台拆文件，CMake 按平台选源）：
// 把 tmp 移到 target，target 已存在时按平台语义覆盖；失败填 ec（成功清空）。
//   Windows: atomic_write_win.cpp   → remove(target) 再 rename（历史兼容路径，行为冻结）
//   POSIX:   atomic_write_posix.cpp → 单次 rename（POSIX 语义 = 原子替换，无窗口期）
// 仅供 core::atomicWriteFile 调用（core 内部细节，不是对外契约）。
namespace detail {

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec);

// 让 tmp **以「仅属主可读写」的权限就位**（不写内容），供 atomicWriteFile 在
// 打开写句柄之前先调。
//
// 为什么需要它：直接 `ofstream(tmp, trunc)` 建文件时，POSIX 上权限位由 umask 决定
//（典型 0644），且**在 open() 那一刻就定死**。事后 chmod 追不回来——chmod 之前
// 若已被别的用户 open 住，那把 fd 之后仍可读，而我们会往同一个 inode 里写密钥。
// 所以收紧必须发生在 open(O_CREAT) 之前：让文件从出生起就是 0600。
// 存在即失败返回 false 且填 ec。
//
// POSIX（atomic_write_posix.cpp）：open(O_CREAT|O_EXCL, S_IRUSR|S_IWUSR) 新建；
//   若已存在（上一轮崩溃的残留）→ 先 permissions(replace) 收紧再复用。
// Windows（atomic_write_win.cpp）：权限模型是 ACL 而非 POSIX 模式位，数据根在
//   %USERPROFILE% 下、NTFS 继承的默认 ACL 已是「仅本用户 + Administrators/SYSTEM」，
//   故此处无 chmod 可做；只把「先建后写」的次序固定下来。**诚实说明**：本函数在
//   Windows 上不构成额外的安全加固，那边的凭据落点保护由目录 ACL 承担。
bool prepareRestrictedFile(const std::filesystem::path& tmp, std::error_code& ec);

} // namespace detail

} // namespace core
