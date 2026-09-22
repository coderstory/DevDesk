// src/platform/file_open_win.cpp
// Windows 实现：资源管理器「打开方式」/文件关联启动时路径走命令行。
// EUI-NEO 提供 main，DevDesk 拿不到形参，因此直接读进程命令行。
// 使用 GetCommandLineW + CommandLineToArgvW 拿宽字符 argv，再转 UTF-8：
// 非 ASCII 路径（如中文文件名）不再经 ANSI 代码页转换而乱码
//（旧实现用 MinGW CRT 的 ANSI __argv，该限制已在 2026-09-22 升级消除）。
#include "file_open.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace platform::file_open {

// UTF-16 → UTF-8（Windows API，无 locale 依赖）
static std::string wideToUtf8(const wchar_t* wstr, int wlen) {
    if (wstr == nullptr || wlen <= 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen,
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::string openFileFromArgv() {
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW == nullptr) return {};
    std::string result;
    // argv[0] 是 exe 自身路径；argv[1] 是文件路径（文件关联/命令行启动时传入）
    if (argc >= 2 && argvW[1] != nullptr && argvW[1][0] != L'\0' &&
        argvW[1][0] != L'-') {  // 跳过 flag 类参数
        result = wideToUtf8(argvW[1], lstrlenW(argvW[1]));
    }
    LocalFree(argvW);
    return result;
}

} // namespace platform::file_open
