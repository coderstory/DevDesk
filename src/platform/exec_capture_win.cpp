// src/platform/exec_capture_win.cpp
// Windows 实现：SearchPathW 探测 PATH + CreateProcessW 跑子进程（不弹窗，捕获退出码）。
#include "exec_capture.h"

#include <windows.h>

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace platform {

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

// 把 args 拼成 CreateProcessW 可接受的单条命令行（按 CommandLineToArgvW 逆规则加引号）。
// 约定：不含 " 或 \ 的简单 arg 不加引号；其余按双引号包裹 + 内部反斜杠转义。
std::wstring joinArgsForCmdLine(const std::vector<std::string>& args) {
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd += L' ';
        const std::wstring w = utf8ToWide(args[i]);
        // 不含空格/制表符/双引号 → 不加引号
        if (w.find_first_of(L" \t\"") == std::wstring::npos) {
            cmd += w;
            continue;
        }
        // 双引号包裹 + 内部双引号转义为 ""
        cmd += L'"';
        size_t pos = 0;
        while (pos < w.size()) {
            const size_t next = w.find(L'"', pos);
            if (next == std::wstring::npos) {
                cmd.append(w, pos, std::wstring::npos);
                break;
            }
            cmd.append(w, pos, next - pos);
            cmd += L"\"\"";  // 双引号 → 两个双引号
            pos = next + 1;
        }
        cmd += L'"';
    }
    return cmd;
}

// SearchPathW 把名字解析成可执行文件全路径（查当前目录 + PATH）。
// lpExtension 传 NULL 不会自动补 .exe（只有名字以句点结尾才补默认扩展）——
// 调用方传裸名（"git"）时必须显式补 .exe，否则 cmd 里明明能跑、这里却找不到。
std::wstring resolveOnPath(const std::string& name) {
    std::string probe = name;
    if (probe.find('.') == std::string::npos) probe += ".exe";
    const std::wstring wname = utf8ToWide(probe);
    wchar_t buf[MAX_PATH];
    const DWORD len = SearchPathW(nullptr, wname.c_str(), nullptr,
                                  MAX_PATH, buf, nullptr);
    if (len == 0 || len >= MAX_PATH) return std::wstring();
    return std::wstring(buf, len);
}

} // namespace

bool executableOnPath(const std::string& name) {
    return !resolveOnPath(name).empty();
}

int execCapture(const std::string& workingDir,
                const std::vector<std::string>& args) {
    if (args.empty()) return -1;

    const std::wstring cmdLine = joinArgsForCmdLine(args);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    // 程序名：含路径分隔符（绝对/相对路径）→ 原样；裸名 → 先经 resolveOnPath 解析
    // 成全路径再传。lpApplicationName 非空时既不搜 PATH 也不补 .exe（与 cmd 的
    // 语义相反；2026-10-14 实证：裸名 "git" 直接喂必失败 ERROR_FILE_NOT_FOUND，
    // md 自动提交整链路在 Windows 上因此全断）。
    std::wstring exe = utf8ToWide(args[0]);
    if (!exe.empty() && exe.find(L'\\') == std::wstring::npos &&
        exe.find(L'/') == std::wstring::npos) {
        exe = resolveOnPath(args[0]);
        if (exe.empty()) return -1;   // PATH 上找不到：与 executableOnPath 同口径
    }
    const std::wstring dir = utf8ToWide(workingDir);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;  // 我们不用管道 → 不设句柄，子进程继承空句柄
    PROCESS_INFORMATION pi{};

    const BOOL ok = CreateProcessW(
        exe.empty() ? nullptr : exe.c_str(),
        cmdBuf.data(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,  // 不弹控制台
        nullptr,
        dir.empty() ? nullptr : dir.c_str(),
        &si, &pi);

    if (ok == FALSE) {
        return -1;  // 启动失败
    }

    // 等待进程结束
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return static_cast<int>(exitCode);
}

} // namespace platform