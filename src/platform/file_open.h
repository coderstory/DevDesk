// src/platform/file_open.h
// 平台无关接口：①从进程命令行捕获「打开方式」传入的文件路径（EUI-NEO 提供 main，
// DevDesk 拿不到 argc/argv，各平台各自读取）；②在系统文件管理器中打开目录。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"

namespace platform::file_open {

// 返回 argv[1] 的文件路径（跳过 '-' 开头的 flag）；无则空串。
//   macOS: _NSGetArgc/_NSGetArgv（Apple 稳定私有 API）；Windows: GetCommandLineW +
//   CommandLineToArgvW，宽字符 → UTF-8（非 ASCII 路径不绕 ANSI 代码页）；其他平台返回空。
std::string openFileFromArgv();

// 纯函数：从「已解码为 UTF-8 的 argv」里选出打开文件参数——argv[1] 非空且不以 '-'
// 开头则返回之，否则空串。各平台负责原始命令行 → UTF-8，选择逻辑共用本函数。
std::string selectOpenFileArg(const std::vector<std::string>& utf8Args);

// Windows 专用测试缝 wideToUtf8ForTest 的声明在 file_open_win.h，不放本平台无关头
// （declaration 在无关头、定义只在 win.cpp 会打破契约）；仅 Windows 编译单元包含它。

// 在系统文件管理器中打开目录（path 为 UTF-8；目录不存在由调用方判断，Win/macOS 对
// 不存在路径返回失败）。不抛异常，失败走 Result：Windows ShellExecuteW → 资源管理器；
// macOS /usr/bin/open → Finder；其他 xdg-open。
core::Result<void> openDirectory(const std::string& path);

} // namespace platform::file_open
