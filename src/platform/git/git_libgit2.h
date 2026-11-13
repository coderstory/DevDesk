// src/platform/git/git_libgit2.h
// libgit2 全局生命周期 RAII 作用域 + 错误码/路径转换小工具（git 管理器只读 v1，
// 池 need:8 批次一；方案 2026-11-13-git-manager-readonly-tech §5）。
// libgit2 是可移植 C 库：本层无 *_win/_macos 拆文件，平台差异封装在库内（方案 §1）。
// 纪律：git2.h 只允许出现在本目录 .cpp 内——头文件零 git2 类型，devdesk_platform
// 对 git2 是 PRIVATE 链接，include 路径不外泄。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/result.h"

namespace platform::git {

// 作用域：模块 onStart 构造、onStop 析构。git_libgit2_init/shutdown 是引用计数语义
// （v1.9 起线程安全），嵌套多作用域安全：构造 = init +1，析构 = shutdown -1。
// 构造失败（返回码非 0）只 LOG_ERROR 不中止——进程级全局初始化，失败留给后续每次
// open 走 Result 错误链上报，不在构造里 abort。
class Libgit2Scope {
public:
    Libgit2Scope();
    ~Libgit2Scope();
    Libgit2Scope(const Libgit2Scope&) = delete;
    Libgit2Scope& operator=(const Libgit2Scope&) = delete;
};

// libgit2 最近一次错误（git_error_last）→ 统一 ErrorCode（方案 §5 映射表）：
//   GIT_ENOTFOUND → FileNotFound；GIT_EBUFS / GIT_EINVALIDSPEC → ValidationError；
//   OS 类且 message 含权限语义（denied/Permission）→ FilePermissionDenied；其余 → Internal。
// rc = 最近一次 libgit2 调用的返回码，原始 message 由 libgit2ToMessage 拼装。
core::ErrorCode libgit2ToErrorCode(int rc);

// 带前缀的完整错误 message（"libgit2 <rc>: <原始 message>"），供 Result 携带后
// 在 UI 内联错误展示（F-13②）；无错误记录时给 "libgit2 <rc>: (no error message)"。
std::string libgit2ToMessage(int rc);

// std::filesystem::path ↔ UTF-8。libgit2 契约：入参路径 = UTF-8（Windows 侧库内
// 自行转宽字符 API）。窄字符串直接构造 fs::path 在 Windows 会按 ACP 误读中文
// （F-10③ 红线），必须走 char8_t 通道（C++20）。
std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path utf8ToPath(std::string_view utf8);

} // namespace platform::git
