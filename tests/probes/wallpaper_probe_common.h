// tests/probes/wallpaper_probe_common.h
// wallpaper 三探针（image_rotation / rotation / yield）公共 Windows 脚手架
// （need:380 批C item7）：三份逐字节相同的 helper 收敛于此；其余段
// （VirtualClock / writeProbePng / MpvIpcClient / primaryMonitorRect /
// FakeYieldWatchers / wideToUtf8 / listOwnTopLevelWindows 等）为场景刻意
// 差异，保留在各探针文件内。include 需在 windows.h/tlhelp32.h 之后。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// 非 Windows：探针整体走 skip 存根，脚手架不编译；kSkipExitCode 三探针同值。
inline constexpr int kSkipExitCode = 77;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

using Clock = std::chrono::steady_clock;

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}
bool processStillRunning(std::uint32_t pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}
std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}
std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::vector<wchar_t> buffer(static_cast<std::size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), size);
    return std::wstring(buffer.data());
}
std::vector<std::uint32_t> pidsOfImageName(const std::wstring& imageName) {
    std::vector<std::uint32_t> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, imageName.c_str()) == 0) {
                pids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}
std::vector<std::uint32_t> setDifference(const std::vector<std::uint32_t>& left,
                                         const std::vector<std::uint32_t>& right) {
    std::vector<std::uint32_t> out;
    for (const std::uint32_t pid : left) {
        bool found = false;
        for (const std::uint32_t other : right) {
            if (pid == other) {
                found = true;
                break;
            }
        }
        if (!found) out.push_back(pid);
    }
    return out;
}
#endif  // _WIN32
