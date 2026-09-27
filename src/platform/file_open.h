// src/platform/file_open.h
// 平台无关接口：①从进程命令行捕获「打开方式」传入的文件路径
//（EUI-NEO 提供 main，DevDesk 代码拿不到 argc/argv 形参，各平台用各自方式读取）
// ②在系统文件管理器中打开目录（产品审计 2026-09-23 [15]，设置页「打开目录」）
#pragma once

#include <string>
#include <vector>

#include "core/result.h"

namespace platform::file_open {

// 返回 argv[1] 的文件路径（跳过以 '-' 开头的 flag 参数）；无则返回空串
//   macOS:   _NSGetArgc/_NSGetArgv（Apple 稳定私有 API）
//   Windows: GetCommandLineW + CommandLineToArgvW 宽字符 argv → UTF-8
//            （2026-09-22 W5 起，非 ASCII 路径不再经 ANSI 代码页乱码）
//   其他:    不支持，返回空
std::string openFileFromArgv();

// 纯函数（审计 [17]）：从「已解码为 UTF-8 的 argv 数组」里选出打开文件参数
// —— argv[0]=exe，argv[1] 非空且不以 '-' 开头则返回之，否则空串。
// 各平台 openFileFromArgv 负责原始命令行 → UTF-8 的解码，选择逻辑共用本函数。
std::string selectOpenFileArg(const std::vector<std::string>& utf8Args);

// Windows 专用测试缝 wideToUtf8ForTest 的声明**不在这里**：平台无关头不放
// win-only 符号（审计：契约被打破——声明在平台无关头、定义只在 file_open_win.cpp）。
// 改由 Windows 侧专用头 src/platform/file_open_win.h 承载，只有 Windows 编译单元
// （file_open_win.cpp）与 _WIN32 守卫内的用例包含它。

// 在系统文件管理器中打开目录（path 为 UTF-8 路径，目录存在与否由调用方
// 决定——Win/macOS 对不存在路径返回失败）。不抛异常，失败走 Result：
//   Windows: ShellExecuteW("open", dir)  → 资源管理器
//   macOS:   /usr/bin/open  → Finder
//   其他:    xdg-open
core::Result<void> openDirectory(const std::string& path);

} // namespace platform::file_open
