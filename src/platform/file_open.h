// src/platform/file_open.h
// 平台无关接口：①从进程命令行捕获「打开方式」传入的文件路径
//（EUI-NEO 提供 main，DevDesk 代码拿不到 argc/argv 形参，各平台用各自方式读取）
// ②在系统文件管理器中打开目录（产品审计 2026-09-23 [15]，设置页「打开目录」）
#pragma once

#include <string>

#include "core/result.h"

namespace platform::file_open {

// 返回 argv[1] 的文件路径（跳过以 '-' 开头的 flag 参数）；无则返回空串
//   macOS:   _NSGetArgc/_NSGetArgv（Apple 稳定私有 API）
//   Windows: 标准 argc/argv（资源管理器「打开方式」/文件关联启动走 argv）
//   其他:    不支持，返回空
std::string openFileFromArgv();

// 在系统文件管理器中打开目录（path 为 UTF-8 路径，目录存在与否由调用方
// 决定——Win/macOS 对不存在路径返回失败）。不抛异常，失败走 Result：
//   Windows: ShellExecuteW("open", dir)  → 资源管理器
//   macOS:   /usr/bin/open  → Finder
//   其他:    xdg-open
core::Result<void> openDirectory(const std::string& path);

} // namespace platform::file_open
