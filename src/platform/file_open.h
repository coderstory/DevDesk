// src/platform/file_open.h
// 平台无关接口：①从进程命令行捕获传入的文件路径（main 在 EUI-NEO，拿不到 argc/argv，各平台自读）；
// ②在系统文件管理器中打开目录。
#pragma once

#include <string>
#include <vector>

#include "core/result.h"

namespace platform::file_open {

// 返回 argv[1] 的文件路径（跳过 '-' 开头的 flag）；无则空串。
// macOS: _NSGetArgc/_NSGetArgv；Windows: GetCommandLineW + CommandLineToArgvW，宽字符 → UTF-8；其他平台返回空。
std::string openFileFromArgv();

// 纯函数：从 UTF-8 argv 里选出打开文件参数——argv[1] 非空且不以 '-' 开头则返回之，否则空串。
std::string selectOpenFileArg(const std::vector<std::string>& utf8Args);

// 在系统文件管理器中打开目录（path 为 UTF-8；目录不存在返回失败，由调用方判断）。
// Windows ShellExecuteW → 资源管理器；macOS /usr/bin/open → Finder；其他 xdg-open。
core::Result<void> openDirectory(const std::string& path);

} // namespace platform::file_open
