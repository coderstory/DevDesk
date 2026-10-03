// src/platform/terminal_launch_win.cpp
// Windows 实现（2026-09-28 profile-run 设计 §4/§4.1、D-4/D-5）：
//   wt.exe 在（探测 %LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe）→
//     CreateProcessW(wt.exe 全路径, "wt.exe -w new -d <dir> --title <t> powershell…")
//   否则 → CreateProcessW(<SystemRoot>\System32\WindowsPowerShell\v1.0\powershell.exe,
//                        "powershell.exe -NoExit -NoProfile -EncodedCommand <b64>",
//                        CREATE_NEW_CONSOLE, lpCurrentDirectory = <起始目录>)
// 两条分支的 exe 都必须是**全路径**：CreateProcessW 的 lpApplicationName 传部分名
// （如裸名 "powershell.exe"）时只按「当前驱动器 + 当前目录」补全、**不查 PATH**
// → 实测 GetLastError=2，窗口起不来（见 terminal_launch_common.h 的说明）。
// 命令文本全部来自 terminal_launch_common.cpp 的纯函数（单测覆盖）；本文件只做
// 「探测 + UTF-8→UTF-16 转换 + CreateProcessW + 句柄清理」。
#include "terminal_launch.h"
#include "terminal_launch_common.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace platform::terminal {
namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), need);
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                         static_cast<int>(w.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

// %SystemRoot%（通常是 C:\Windows）：退化分支的 powershell 全路径由它铺开。
// 读不到返回空串 → 纯函数回落 C:\Windows 默认安装位置（不在此处硬编码兜底）。
std::string systemRootUtf8() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"SystemRoot", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    return wideToUtf8(std::wstring(buf, n));
}

// wt.exe 全路径：WindowsApps 下是 app execution alias，且 PATH 顺序不保证 →
// 探测与派发共用同一绝对路径
std::wstring windowsTerminalPath() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local == nullptr || *local == L'\0') return std::wstring();
    std::wstring p(local);
    p += L"\\Microsoft\\WindowsApps\\wt.exe";
    return p;
}

// 分离式派发：不等待退出、不继承句柄；进程/线程句柄用完即关（D-9：不持有会话）
core::Result<void> spawn(const std::wstring& exe,
                         const std::vector<std::string>& argv,
                         const std::wstring& currentDir) {
    std::wstring cmdLine = utf8ToWide(joinWindowsCommandLine(argv));
    // CreateProcessW 可能就地改写 lpCommandLine → 必须给可写缓冲
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const wchar_t* dir = currentDir.empty() ? nullptr : currentDir.c_str();
    const BOOL ok = CreateProcessW(exe.empty() ? nullptr : exe.c_str(),
                                   buf.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_CONSOLE, nullptr, dir, &si, &pi);
    if (ok == FALSE) {
        const DWORD err = GetLastError();
        return core::Result<void>::fail(core::ErrorCode::Internal,
            "启动终端进程失败（GetLastError=" + std::to_string(err) + "）");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return core::Result<void>::ok();
}

} // namespace

bool windowsTerminalMissing() {
    const std::wstring wt = windowsTerminalPath();
    if (wt.empty()) return true;   // 取不到 LOCALAPPDATA = 视为缺失
    // app execution alias 是重解析点：直接问 Win32 文件属性比 filesystem::exists
    // 更可靠（后者对别名不保证语义）
    return GetFileAttributesW(wt.c_str()) == INVALID_FILE_ATTRIBUTES;
}

std::string homeDir() {
    const char* p = std::getenv("USERPROFILE");
    if (p != nullptr && *p != '\0') return p;
    std::error_code ec;   // 环境变量缺失 → 进程当前目录兜底（保证非空）
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string() : cwd.string();
}

core::Result<void> launch(const LaunchSpec& spec) {
    const std::wstring wt = windowsTerminalPath();
    const bool hasWt = !wt.empty() &&
        GetFileAttributesW(wt.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (hasWt) {
        // wt -d 已带起始目录 → 不需要 lpCurrentDirectory（spec §4.1）
        return spawn(wt, buildWindowsLaunchArgs(spec), std::wstring());
    }
    // 退化分支（D-5）：没有 wt -d，**必须**显式设 lpCurrentDirectory；
    // 正文里的 Set-Location 是双保险（spec §4.1）
    // exe 走全路径（裸名 "powershell.exe" 不查 PATH → 实测 err=2 起不来）
    return spawn(utf8ToWide(fallbackPowerShellExe(systemRootUtf8())),
                 buildWindowsFallbackArgs(spec), utf8ToWide(spec.workingDir));
}

} // namespace platform::terminal
