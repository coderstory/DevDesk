// src/platform/file_open_win.cpp
// Windows：GetCommandLineW + CommandLineToArgvW 拿宽字符 argv 再转 UTF-8，非 ASCII
// 路径（中文文件名）不绕 ANSI 代码页而乱码。EUI-NEO 提供 main，DevDesk 拿不到形参。
#include "file_open.h"
#include "file_open_common.h"
#include "file_open_win.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace platform::detail {

// UTF-16 → UTF-8（Windows API，无 locale 依赖）。定义在本 Windows-only TU
// （共用声明见 file_open_common.h）
std::string wideToUtf8(const wchar_t* wstr, int wlen) {
    if (wstr == nullptr || wlen <= 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen,
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, out.data(), needed,
                        nullptr, nullptr);
    return out;
}

// UTF-8 → UTF-16（openDirectory / 监听目录打开用；空串或非法输入返回空 wstring）
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                         static_cast<int>(s.size()),
                                         nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), wlen);
    return out;
}

} // namespace platform::detail

namespace platform::file_open {

// 测试缝：暴露 wideToUtf8 编码链供回归用例直接调用（与 openFileFromArgv 是同一
// 函数，锁的就是真实转换路径）。声明见 win 专用头 file_open_win.h，不进平台无关头。
std::string wideToUtf8ForTest(const std::wstring& w) {
    return detail::wideToUtf8(w.data(), static_cast<int>(w.size()));
}

std::string openFileFromArgv() {
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW == nullptr) return {};
    // 解码 argv[0..1] 为 UTF-8（只这两项够选参数），交共用纯函数选 argv[1]
    std::vector<std::string> utf8Args;
    for (int i = 0; i < argc && i <= 1; ++i) {
        utf8Args.push_back(argvW[i] != nullptr
                               ? detail::wideToUtf8(argvW[i], lstrlenW(argvW[i]))
                               : std::string{});
    }
    LocalFree(argvW);
    return selectOpenFileArg(utf8Args);
}

core::Result<void> openDirectory(const std::string& path) {
    if (path.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "目录路径为空");
    }
    const std::wstring wpath = detail::utf8ToWide(path);
    if (wpath.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "目录路径 UTF-8 转换失败");
    }
    // ShellExecuteExW + SEE_MASK_FLAG_NO_UI：失败只回错误码、不弹系统错误框（旧
    // ShellExecuteW 的 ≤32 返回值语义 + 失败弹窗在自动化测试里都是坑；同值别名不叠加）
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = wpath.c_str();
    info.nShow = SW_SHOW;
    if (ShellExecuteExW(&info)) {
        return core::Result<void>::ok();
    }
    return core::Result<void>::fail(
        core::ErrorCode::Internal,
        "ShellExecuteExW 打开目录失败 (GetLastError=" +
            std::to_string(static_cast<long>(GetLastError())) + ")");
}

} // namespace platform::file_open
