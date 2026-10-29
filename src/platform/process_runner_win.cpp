// src/platform/process_runner_win.cpp
// Windows 进程执行器：CreateProcess + 匿名管道全捕获（stdin/stdout/stderr）。
// 超时用 WaitForSingleObject；超时到期 TerminateProcess 强杀。
// 线程安全：每次调用独立进程，runner 自身无共享状态。
#include "process_runner.h"
#include "file_open_common.h"  // platform::detail::wideToUtf8 / utf8ToWide

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace platform {

namespace {

// 排空管道当前可读数据，不阻塞：无数据或管道坏即返回（供读/等待交错轮询）
void drainPipe(HANDLE h, std::string& out) {
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) return;
        out.append(buf, n);
    }
}

// 进程终结后阻塞读到 EOF：写端随进程退出关闭，ReadFile 返回 0/失败即止。
// 孙进程继承写端未关时会拖住本调用——与旧实现的同一已知边界，不在本函数处理。
void drainToEof(HANDLE h, std::string& out) {
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) return;
        out.append(buf, n);
    }
}

void closeHandleSafe(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = nullptr;
    }
}

// 将命令行参数拼成 CreateProcess 的 lpCommandLine（MSVC CRT 式引号规则）
std::wstring buildCommandLine(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd += L' ';
        std::wstring wide = platform::detail::utf8ToWide(argv[i]);
        // 参数含空格或为空 → 双引号包裹；内部双引号转义成 \"
        if (wide.empty() || wide.find(L' ') != std::wstring::npos ||
            wide.find(L'\t') != std::wstring::npos) {
            // 简单引号规则：成对双引号包裹，内部双引号转义
            std::wstring escaped;
            escaped += L'"';
            for (wchar_t c : wide) {
                if (c == L'"') escaped += L'\\';
                escaped += c;
            }
            escaped += L'"';
            cmd += escaped;
        } else {
            cmd += wide;
        }
    }
    return cmd;
}

} // namespace

bool processRunnerSupported() { return true; }

std::string defaultShellPath() {
    // 优先级：PATH 里的 bash.exe（排除 WSL 的 System32\bash.exe）
    // 兜底：常见安装路径
    constexpr const wchar_t* kCandidates[] = {
        L"bash.exe",                                          // PATH，最高优先
        L"C:\\msys64\\usr\\bin\\bash.exe",                    // MSYS2
        L"C:\\Program Files\\Git\\bin\\bash.exe",             // Git for Windows
        L"C:\\Program Files\\Git\\usr\\bin\\bash.exe",
    };

    for (const wchar_t* cand : kCandidates) {
        // 对于第一个候选（bash.exe），走 SearchPathW；其余走 GetFileAttributes
        std::wstring path;
        std::wstring fullPath;
        if (cand == std::wstring(L"bash.exe")) {
            wchar_t buf[MAX_PATH] = {};
            DWORD len = SearchPathW(nullptr, cand, L".exe", MAX_PATH, buf, nullptr);
            if (len > 0 && len < MAX_PATH) {
                fullPath = buf;
                // 排除 System32\bash.exe（WSL 壳，非 Git Bash）
                std::wstring lower = fullPath;
                for (wchar_t& c : lower) c = static_cast<wchar_t>(::towlower(c));
                if (lower.find(L"system32") != std::wstring::npos) {
                    continue;  // 跳过 WSL，试下一个
                }
                return platform::detail::wideToUtf8(fullPath.c_str(), static_cast<int>(fullPath.size()));
            }
        } else {
            DWORD attr = GetFileAttributesW(cand);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return platform::detail::wideToUtf8(cand, static_cast<int>(std::wcslen(cand)));
            }
        }
    }
    return "bash.exe";  // 最终兜底
}

ProcessRunner systemProcessRunner() {
    return [](const std::vector<std::string>& argv, const std::string& stdinText,
              const std::string& cwd, long timeoutMs) -> core::Result<ProcessResult> {
        if (argv.empty()) {
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::ValidationError, "进程执行器：argv 为空");
        }

        ProcessResult result;

        // 创建 stdin 管道（如果调用方传了 stdin 内容才创建写端）
        HANDLE stdinRead = nullptr, stdinWrite = nullptr;
        SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!stdinText.empty()) {
            if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0)) {
                return core::Result<ProcessResult>::fail(
                    core::ErrorCode::Internal,
                    "进程执行器：CreatePipe stdin 失败 (GetLastError=" +
                        std::to_string(GetLastError()) + ")");
            }
            // 不让子进程继承写端
            SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        }

        // stdout 管道
        HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
        if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
            closeHandleSafe(stdinRead);
            closeHandleSafe(stdinWrite);
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：CreatePipe stdout 失败 (GetLastError=" +
                    std::to_string(GetLastError()) + ")");
        }
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

        // stderr 管道
        HANDLE stderrRead = nullptr, stderrWrite = nullptr;
        if (!CreatePipe(&stderrRead, &stderrWrite, &sa, 0)) {
            closeHandleSafe(stdinRead);
            closeHandleSafe(stdinWrite);
            closeHandleSafe(stdoutRead);
            closeHandleSafe(stdoutWrite);
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：CreatePipe stderr 失败 (GetLastError=" +
                    std::to_string(GetLastError()) + ")");
        }
        SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

        // STARTUPINFO：重定向标准句柄
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = stdoutWrite;
        si.hStdError = stderrWrite;
        si.hStdInput = stdinRead ? stdinRead : GetStdHandle(STD_INPUT_HANDLE);

        std::wstring cmdLine = buildCommandLine(argv);

        // cwd 转换
        std::wstring wideCwd;
        const wchar_t* cwdPtr = nullptr;
        if (!cwd.empty()) {
            wideCwd = platform::detail::utf8ToWide(cwd);
            cwdPtr = wideCwd.c_str();
        }

        PROCESS_INFORMATION pi = {};
        BOOL created = CreateProcessW(
            nullptr,                     // lpApplicationName：从命令行解析
            cmdLine.data(),              // lpCommandLine（可写缓冲）
            nullptr, nullptr,            // 不继承进程属性
            TRUE,                        // 继承句柄（管道）
            CREATE_NO_WINDOW,            // 不弹控制台窗口
            nullptr,                     // 继承环境块
            cwdPtr,                      // 工作目录
            &si, &pi);

        // 子进程侧管道端不在此进程用，立即关
        closeHandleSafe(stdoutWrite);
        closeHandleSafe(stderrWrite);
        if (stdinWrite) {
            // 写完 stdin 立即关写端，子进程看到 EOF
            if (!stdinText.empty()) {
                DWORD written = 0;
                WriteFile(stdinWrite, stdinText.data(),
                          static_cast<DWORD>(stdinText.size()), &written, nullptr);
            }
            closeHandleSafe(stdinWrite);
        }

        if (!created) {
            closeHandleSafe(stdinRead);
            closeHandleSafe(stdoutRead);
            closeHandleSafe(stderrRead);
            closeHandleSafe(pi.hProcess);
            closeHandleSafe(pi.hThread);
            return core::Result<ProcessResult>::fail(
                core::ErrorCode::Internal,
                "进程执行器：CreateProcess 失败 (GetLastError=" +
                    std::to_string(GetLastError()) + ")");
        }

        closeHandleSafe(pi.hThread);  // 不需要线程句柄

        // 读排空与等待交错轮询：管道缓冲满而子进程未退出时，「先 wait 后读」
        // 双向死锁（子进程写管道阻塞、本进程等退出）。每拍先排空两根管道再探退出；
        // 超时杀进程；退出后阻塞排空残余到 EOF。timeoutMs<=0 即无限等（仍每拍排空）。
        constexpr long kPollMs = 20;
        long elapsed = 0;
        while (true) {
            drainPipe(stdoutRead, result.stdoutText);
            drainPipe(stderrRead, result.stderrText);
            const DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(kPollMs));
            if (wait == WAIT_OBJECT_0) break;
            if (timeoutMs > 0) {
                elapsed += kPollMs;
                if (elapsed >= timeoutMs) {
                    result.timedOut = true;
                    TerminateProcess(pi.hProcess, 1);
                    WaitForSingleObject(pi.hProcess, 5000);  // 给 5 秒清理
                    break;
                }
            }
        }
        drainToEof(stdoutRead, result.stdoutText);
        drainToEof(stderrRead, result.stderrText);

        // 取退出码
        DWORD exitCode = 0;
        if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
            result.exitCode = static_cast<int>(exitCode);
        }
        closeHandleSafe(pi.hProcess);

        closeHandleSafe(stdoutRead);
        closeHandleSafe(stderrRead);
        closeHandleSafe(stdinRead);

        return core::Result<ProcessResult>::ok(result);
    };
}

} // namespace platform