// src/platform/file_open_win.cpp
// Windows 实现：标准 argc/argv（资源管理器「打开方式」/文件关联启动时路径走 argv）
// EUI-NEO 提供 main，DevDesk 拿不到形参；MinGW CRT 提供 __argc/__argv 全局变量（stdlib.h）。
// 已知边界：__argv 是 ANSI 编码，非 ASCII 文件路径可能乱码；
// 完整做法是 GetCommandLineW + CommandLineToArgvW 再转 UTF-8，留待后续任务。
#include "file_open.h"

#include <stdlib.h>
#include <string>

namespace platform::file_open {

std::string openFileFromArgv() {
    if (__argc < 2 || __argv == nullptr) return {};

    // argv[1] 是文件路径（文件关联/命令行启动时传入）
    const char* path = __argv[1];
    if (path == nullptr || path[0] == '\0') return {};

    // 跳过 flag 类参数（以 '-' 开头）
    if (path[0] == '-') return {};

    return std::string(path);
}

} // namespace platform::file_open
