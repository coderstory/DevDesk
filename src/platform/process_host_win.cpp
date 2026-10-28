// src/platform/process_host_win.cpp
// Windows 实现：CreateProcessW（CREATE_NO_WINDOW + 新进程组，环境块重写）+
// taskkill 树杀 + OpenProcess 探活。
#include "process_host.h"

#include <windows.h>

#include <string>
#include <system_error>
#include <vector>

namespace platform::process_host {

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

std::string lastErrorText(const char* what) {
    const DWORD code = GetLastError();
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text(what);
    text += " failed (" + std::to_string(static_cast<unsigned long>(code)) + ")";
    if (n > 0 && buf != nullptr) {
        text += ": ";
        const int need = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                            nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            std::string msg(static_cast<size_t>(need), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                msg.data(), need, nullptr, nullptr);
            text += msg;
        }
    }
    if (buf != nullptr) LocalFree(buf);
    return text;
}

// 命令行拼接：与 exec_capture_win 同一套 CommandLineToArgvW 逆规则
std::wstring joinArgsForCmdLine(const std::vector<std::string>& args) {
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd += L' ';
        const std::wstring w = utf8ToWide(args[i]);
        if (w.find_first_of(L" \t\"") == std::wstring::npos) {
            cmd += w;
            continue;
        }
        cmd += L'"';
        size_t pos = 0;
        while (pos < w.size()) {
            const size_t next = w.find(L'"', pos);
            if (next == std::wstring::npos) {
                cmd.append(w, pos, std::wstring::npos);
                break;
            }
            cmd.append(w, pos, next - pos);
            cmd += L"\"\"";
            pos = next + 1;
        }
        cmd += L'"';
    }
    return cmd;
}

// 继承环境 + 追加覆盖（"KEY=VALUE"），产出 CreateProcessW 环境块（UTF-16，\0 间隔、双 \0 结尾）
std::vector<wchar_t> buildEnvBlock(const std::vector<std::string>& envPairs) {
    std::vector<std::wstring> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block != nullptr) {
        LPWCH cursor = block;
        while (*cursor != L'\0') {
            entries.emplace_back(cursor);
            cursor += std::wcslen(cursor) + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    for (const std::string& pair : envPairs) {
        const std::wstring w = utf8ToWide(pair);
        const size_t eq = w.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;  // 非法项跳过
        const std::wstring key = w.substr(0, eq);
        bool replaced = false;
        for (std::wstring& e : entries) {
            if (e.compare(0, eq, key) == 0 && e[static_cast<size_t>(eq)] == L'=') {
                e = w;  // 同名覆盖
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back(w);
    }
    std::vector<wchar_t> out;
    for (const std::wstring& e : entries) {
        out.insert(out.end(), e.begin(), e.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

// 同步跑一条短命命令（taskkill）：不弹窗、输出丢弃、最多等 5s
void runSyncNoWindow(const std::wstring& cmdLine) {
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return;  // taskkill 缺失等环境异常：终 tree 失败由探活兜底，不抛
    }
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

} // namespace

core::Result<std::int64_t> spawnDetached(
    const std::vector<std::string>& argv,
    const std::vector<std::string>& envPairs,
    const std::string& workingDir) {
    if (argv.empty() || argv[0].empty()) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::Internal,
                                        "spawnDetached: argv 为空");
    }
    const std::wstring cmdLine = joinArgsForCmdLine(argv);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    const std::wstring cwdW = utf8ToWide(workingDir);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> envBlock = buildEnvBlock(envPairs);

    const BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
        envBlock.empty() ? nullptr : envBlock.data(),
        cwdW.empty() ? nullptr : cwdW.c_str(), &si, &pi);
    if (!ok) {
        return core::Result<std::int64_t>::fail(core::ErrorCode::Internal,
                                            lastErrorText("CreateProcessW"));
    }
    const std::int64_t pid = static_cast<std::int64_t>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return core::Result<std::int64_t>::ok(pid);
}

core::Result<void> terminateTree(std::int64_t pid) {
    if (pid <= 0) return core::Result<void>::ok();
    // 树杀必须 taskkill /T（直接 TerminateProcess 杀不掉 python→pip 子孙链）。
    // 退出码不核：目标已退出 / 已被杀都属幂等成功，残余进程由探活与重启路径兜底。
    runSyncNoWindow(L"taskkill.exe /T /F /PID " + std::to_wstring(pid));
    return core::Result<void>::ok();
}

bool isAlive(std::int64_t pid) {
    if (pid <= 0) return false;
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (h == nullptr) return false;
    DWORD code = 0;
    const BOOL hasCode = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!hasCode) return false;
    return code == STILL_ACTIVE;
}

} // namespace platform::process_host
