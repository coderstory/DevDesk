// tests/probes/wallpaper_yield_probe.cpp
// 壁纸「让路 watcher 全接线」端到端真机探针：真桌面播视频 → 全屏窗让路 → 关窗
// 恢复 → 电池/锁屏事件注入裁决 → 停止无残留。
//
// 断言（每条一行 PASS/FAIL）：
//   §1 资源与服务：真 WorkerW 宿主 + 真 mpv（默认装配，不注入 player），%TEMP%
//      独立目录两支 clip 入库 Playing；yield watcher 消息窗建立（本进程顶层窗口
//      = 宿主 1 + watcher 1）。
//   §2 全屏让路（决定性）：探针自开一个可见、恰好覆盖主屏的顶层窗 → 有界窗口内
//      快照 paused && systemHolds == Fullscreen，且 mpv IPC pause 属性 == true
//      （机判信号：暂停确实下达到播放器，而非只改快照）→ 销毁全屏窗 → 快照恢复
//      且 IPC pause == false。锁屏/换屏无法程序化模拟——事件通道由 §3 注入回调
//      实证服务侧裁决，真广播路径由单测与人工验收覆盖。
//   §3 watcher 事件注入（接口注入）：独立服务（测试装配 + 假 watcher）——
//      锁屏回调 → 暂停 + ScreenLocked 位；解锁 → 恢复；电池轮询置位 → 暂停 +
//      Battery 位；「电池供电时继续」豁免 → 即便持续上报也恢复；重开 → 位回来。
//   §4 停止无残留：service.stop() < 3s；mpv 退出无残留；IPC 管道不可再连；
//      本进程顶层窗口归零（宿主消息窗 + watcher 消息窗都已销毁 = 桌面还原）。
// 退出码：0 通过 / 77 非 Windows 跳过 / 非零失败（stderr 中文原因）。
// 数据隔离：设置走 %TEMP% 独立 settings.json；探针不杀任何进程，mpv 由服务自启自收。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "core/paths.h"
#include "module_wallpaper/core/settings.h"
#include "module_wallpaper/core/video_library.h"
#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/platform/yield_watchers.h"
#include "module_wallpaper/wallpaper_service.h"
#endif

namespace {

constexpr int kSkipExitCode = 77;

#if defined(_WIN32)

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

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<std::size_t>(size), char{0});
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size) - 1);
    return out;
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::vector<wchar_t> buffer(static_cast<std::size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), size);
    return std::wstring(buffer.data());
}

// ---- mpv 进程盘点（只观察，不杀） ----

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

// ---- 本进程顶层窗口计数：壁纸宿主 + yield watcher 的消息窗是本进程仅有的
//      顶层窗口来源；计数归零 = 两窗都已销毁 = 桌面还原 ----

struct WindowCountContext {
    DWORD pid = 0;
    int count = 0;
};

BOOL CALLBACK countWindowProc(HWND window, LPARAM lParam) {
    WindowCountContext* context = reinterpret_cast<WindowCountContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != context->pid) return TRUE;
    // 不变量：IME「Default IME」伴生窗由 imm32 按持有窗口的线程自动生灭，
    // 是系统输入上下文的卫星窗，不属本探针的装配状态，必须排除。
    wchar_t cls[64] = {};
    if (GetClassNameW(window, cls, 64) > 0 && lstrcmpW(cls, L"IME") == 0) return TRUE;
    ++context->count;
    return TRUE;
}

int countOwnTopLevelWindows() {
    WindowCountContext context;
    context.pid = GetCurrentProcessId();
    EnumWindows(countWindowProc, reinterpret_cast<LPARAM>(&context));
    return context.count;
}

// 诊断：列出本进程全部顶层窗口（类名 + 标题），供装配计数断言失败时定位多出的窗。
std::string listOwnTopLevelWindows() {
    std::string out;
    EnumWindows([](HWND window, LPARAM lParam) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(window, cls, 64);
        wchar_t title[128] = {};
        GetWindowTextW(window, title, 128);
        auto* outp = reinterpret_cast<std::string*>(lParam);
        *outp += " [" + wideToUtf8(std::wstring(cls)) + "|"
               + wideToUtf8(std::wstring(title)) + "]";
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

// ---- mpv IPC 第二客户端（同 wallpaper_rotation_probe 的有界 OVERLAPPED 纪律） ----

bool overlappedTransfer(HANDLE handle, bool isWrite, void* buffer, DWORD size, DWORD timeoutMs,
                        DWORD* transferred) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    const BOOL queued =
        isWrite ? WriteFile(handle, buffer, size, nullptr, &overlapped)
                : ReadFile(handle, buffer, size, nullptr, &overlapped);
    bool ok = false;
    if (queued == 0 && GetLastError() != ERROR_IO_PENDING) {
        // 立即失败（断管等）：无 IRP 可等。
    } else if (WaitForSingleObject(overlapped.hEvent, timeoutMs) == WAIT_OBJECT_0) {
        DWORD got = 0;
        ok = GetOverlappedResult(handle, &overlapped, &got, FALSE) != 0;
        if (isWrite) ok = ok && got == size;
        if (transferred != nullptr) *transferred = got;
    } else {
        (void)CancelIoEx(handle, &overlapped);
        DWORD dropped = 0;
        (void)GetOverlappedResult(handle, &overlapped, &dropped, TRUE);
    }
    CloseHandle(overlapped.hEvent);
    return ok;
}

bool extractReplyData(const std::string& line, int requestId, std::string* value) {
    std::string compact;
    compact.reserve(line.size());
    for (const char raw : line) {
        if (raw == ' ' || raw == '\t' || raw == '\r' || raw == '\n') continue;
        compact += raw;
    }
    const std::string idKey = "\"request_id\":" + std::to_string(requestId);
    const std::size_t idAt = compact.find(idKey);
    if (idAt == std::string::npos) return false;
    const std::size_t idEnd = idAt + idKey.size();
    if (idEnd >= compact.size() || (compact[idEnd] != ',' && compact[idEnd] != '}')) return false;
    if (compact.find("\"error\":\"success\"") == std::string::npos) return false;
    const std::size_t dataAt = compact.find("\"data\":");
    if (dataAt == std::string::npos) return false;
    const std::size_t valueStart = dataAt + 7;
    if (valueStart >= compact.size()) return false;
    if (valueStart < compact.size() && compact[valueStart] == '"') return false;  // 只要布尔
    const std::size_t numberEnd = compact.find_first_of(",}", valueStart);
    if (numberEnd == std::string::npos) return false;
    if (value != nullptr) *value = compact.substr(valueStart, numberEnd - valueStart);
    return true;
}

class MpvIpcClient {
public:
    ~MpvIpcClient() { disconnect(); }

    bool connect(const std::string& pipeName, std::chrono::milliseconds timeout) {
        const std::wstring wide = utf8ToWide(pipeName);
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                pipe_ = handle;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    void disconnect() {
        if (pipe_ == nullptr) return;
        (void)CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }

    bool getProperty(const std::string& name, int requestId, std::string* value,
                     std::chrono::milliseconds timeout) {
        if (pipe_ == nullptr) return false;
        const std::string command = "{\"command\":[\"get_property\",\"" +
                                    module::wallpaper::mpvJsonEscape(name) +
                                    "\"],\"request_id\":" + std::to_string(requestId) + "}\n";
        if (!overlappedTransfer(pipe_, true, const_cast<char*>(command.c_str()),
                                static_cast<DWORD>(command.size()), 1000, nullptr)) {
            return false;
        }
        const auto deadline = Clock::now() + timeout;
        std::string pending;
        char buffer[4096];
        while (Clock::now() < deadline) {
            DWORD got = 0;
            if (!overlappedTransfer(pipe_, false, buffer, sizeof(buffer), 200, &got)) continue;
            if (got == 0) continue;
            pending.append(buffer, got);
            std::size_t newline = pending.find('\n');
            while (newline != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                std::string data;
                if (extractReplyData(line, requestId, &data)) {
                    if (value != nullptr) *value = data;
                    return true;
                }
                newline = pending.find('\n');
            }
        }
        return false;
    }

private:
    HANDLE pipe_ = nullptr;
};

// ---- §2 的全屏测试窗：可见、无工具窗样式、恰好覆盖主屏（满足 watcher 的
//      覆盖判据；类名不在壁纸排除表内） ----

constexpr wchar_t kProbeFullscreenClass[] = L"DevDeskYieldProbeFullscreen";

bool primaryMonitorRect(RECT* rect) {
    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr) return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    *rect = info.rcMonitor;
    return true;
}

HWND createFullscreenWindow() {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kProbeFullscreenClass;
    if (RegisterClassW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    RECT rect{};
    if (!primaryMonitorRect(&rect)) return nullptr;
    // 注意：不得加 WS_EX_TOOLWINDOW——watcher 的全屏扫描会排除工具窗。
    return CreateWindowExW(0, kProbeFullscreenClass, L"devdesk yield probe", WS_POPUP,
                           rect.left, rect.top, rect.right - rect.left,
                           rect.bottom - rect.top, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

// ---- §3 的假 watcher（接口注入；与单测同构，事件手动点火、轮询读原子开关） ----

class FakeYieldWatchers final : public module::wallpaper::YieldWatchers {
public:
    void setHoldCallback(HoldCallback callback) override { hold_ = std::move(callback); }
    void setDisplayChangedCallback(DisplayChangedCallback callback) override {
        (void)callback;  // 探针不断言换屏重建（无宿主装配）；通道保持接通即可
    }
    bool start(std::string* error) override {
        if (error != nullptr) *error = "fake";
        return true;
    }
    void stop() override {}
    bool pollOnBattery() override { return onBattery_.load(); }
    bool pollFullscreenOtherProcess() override { return false; }

    void fireHold(module::wallpaper::HoldReason reason, bool active) {
        if (hold_) hold_(reason, active);
    }
    void setBattery(bool on) { onBattery_.store(on); }

private:
    HoldCallback hold_;
    std::atomic<bool> onBattery_{false};
};

#endif  // _WIN32

}  // namespace

int main(int argc, char* argv[]) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    std::cout << "wallpaper_yield_probe: 非 Windows 平台，跳过（WorkerW/WTS 仅 Windows）"
              << " exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#else
    (void)argc;
    (void)argv;

    // 探针独立进程：对齐 DevDesk 的 PerMonitorV2（rcMonitor 必须是物理像素）。
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool passed = true;
    auto check = [&passed](bool condition, const std::string& what) {
        if (condition) {
            std::cout << "[PASS] " << what << "\n";
        } else {
            std::cerr << "[FAIL] " << what << "\n";
            passed = false;
        }
    };

    const std::filesystem::path fixtureDir =
        "D:/Data/Downloads/dynamic-wallpaper-master/fixtures";
    const std::filesystem::path clipA = fixtureDir / "clip-a.mp4";
    if (!std::filesystem::is_regular_file(clipA)) {
        std::cerr << "wallpaper_yield_probe: 测试视频不存在: " << fixtureDir.string() << "\n";
        return 3;
    }

    // mpv 预检（服务内部同款解析优先级）。
    bool usedDevFallback = false;
    const std::string mpvPath = module::wallpaper::resolveMpvExecutable(
        envValue(module::wallpaper::kMpvPathEnvVar), core::paths::executableDir().string(),
        module::wallpaper::kMpvDevFallbackPath, true, &usedDevFallback);
    if (mpvPath.empty()) {
        std::cerr << "wallpaper_yield_probe: 找不到 mpv.exe（DEVDESK_MPV_PATH / "
                     "<exeDir>/assets/mpv/mpv.exe / 开发兜底）\n";
        return 4;
    }
    if (usedDevFallback) {
        std::cout << "[INFO] 使用开发兜底 mpv: " << mpvPath << "\n";
    }
    const std::wstring mpvImageName =
        utf8ToWide(std::filesystem::path(mpvPath).filename().string());

    // ── §1 资源与服务 ──
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() /
                                          ("devdesk-yield-probe-" +
                                           std::to_string(GetCurrentProcessId()));
    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }
    std::error_code fsEc;
    std::filesystem::create_directories(tempDir, fsEc);
    const std::filesystem::path tempClipA = tempDir / "clip-a.mp4";
    std::filesystem::copy_file(clipA, tempClipA,
                               std::filesystem::copy_options::overwrite_existing, fsEc);
    const bool resourcesOk = std::filesystem::is_regular_file(tempClipA);
    check(resourcesOk, "§1 资源：%TEMP% 真文件夹就绪（clip-a 拷贝）: " + tempDir.string());
    if (!resourcesOk) {
        std::error_code cleanupEc;
        std::filesystem::remove_all(tempDir, cleanupEc);
        return 6;
    }

    const std::vector<std::uint32_t> mpvBaseline = pidsOfImageName(mpvImageName);
    const int windowsBefore = countOwnTopLevelWindows();

    // 真装配：真 WorkerW 宿主 + 真 mpv + 真 yield watcher（不注入 player/watcher）。
    module::wallpaper::WallpaperService service;
    service.setSettingsPathForTest((tempDir / "settings.json").string());
    std::string folderError;
    if (!service.selectFolder(tempDir.string(), &folderError)) {
        std::cerr << "wallpaper_yield_probe: selectFolder 失败: " << folderError << "\n";
        std::error_code cleanupEc;
        std::filesystem::remove_all(tempDir, cleanupEc);
        return 5;
    }
    service.start();

    auto cleanup = [&service, &tempDir]() {
        service.stop();
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    };

    const bool ingested = waitUntil(
        [&service]() {
            const module::wallpaper::WallpaperSnapshot snap = service.snapshot();
            return !snap.scanning &&
                   snap.availability == module::wallpaper::Availability::Playing;
        },
        std::chrono::milliseconds(10000));
    check(ingested, "§1 入库：服务 Playing（真宿主 + 真 mpv + 真 watcher 装配）");
    if (!ingested) {
        cleanup();
        return 7;
    }

    // 顶层窗口 = 宿主消息窗 + watcher 消息窗（恰好 +2：两窗都建立才接得全四路）。
    const int windowsDuringPlay = countOwnTopLevelWindows();
    check(windowsDuringPlay == windowsBefore + 2,
          "§1 装配：本进程顶层窗口 " + std::to_string(windowsDuringPlay) + " = 启动前 " +
              std::to_string(windowsBefore) + " + 宿主消息窗 1 + watcher 消息窗 1"
              + (windowsDuringPlay == windowsBefore + 2
                     ? std::string()
                     : "；窗口清单:" + listOwnTopLevelWindows()));

    MpvIpcClient ipc;
    const std::string pipeName = module::wallpaper::makeMpvPipeName(GetCurrentProcessId(), 1);
    const bool pipeUp = ipc.connect(pipeName, std::chrono::milliseconds(10000));
    check(pipeUp, "§1 首播：mpv IPC 管道可连（" + pipeName + "）");
    if (!pipeUp) {
        cleanup();
        return 8;
    }

    // ── §2 全屏让路 → 恢复（决定性）──
    const HWND fullscreenWindow = createFullscreenWindow();
    check(fullscreenWindow != nullptr, "§2 全屏：探针全屏测试窗创建成功");
    if (fullscreenWindow != nullptr) {
        ShowWindow(fullscreenWindow, SW_SHOW);
        const bool held = waitUntil(
            [&service]() {
                const module::wallpaper::WallpaperSnapshot snap = service.snapshot();
                return snap.paused && snap.systemHolds ==
                                          module::wallpaper::holdReasonBit(
                                              module::wallpaper::HoldReason::Fullscreen);
            },
            std::chrono::milliseconds(6000));
        check(held, "§2 让路：全屏窗出现后快照 paused 且 systemHolds == Fullscreen"
                    "（watcher 轮询 250ms/拍，有界 6s）");

        std::string pauseValue;
        const bool mpvPaused = waitUntil(
            [&]() {
                return ipc.getProperty("pause", 4001, &pauseValue,
                                       std::chrono::milliseconds(800)) &&
                       pauseValue == "true";
            },
            std::chrono::milliseconds(4000));
        check(mpvPaused, "§2 让路：mpv IPC pause 属性 == true（暂停下达到播放器，实测 \"" +
                             pauseValue + "\"）");

        DestroyWindow(fullscreenWindow);
        const bool released = waitUntil(
            [&service]() {
                const module::wallpaper::WallpaperSnapshot snap = service.snapshot();
                return !snap.paused &&
                       (snap.systemHolds &
                        module::wallpaper::holdReasonBit(
                            module::wallpaper::HoldReason::Fullscreen)) == 0;
            },
            std::chrono::milliseconds(6000));
        check(released, "§2 恢复：全屏窗销毁后快照解除暂停且 Fullscreen 位清零");

        std::string resumeValue;
        const bool mpvResumed = waitUntil(
            [&]() {
                return ipc.getProperty("pause", 4002, &resumeValue,
                                       std::chrono::milliseconds(800)) &&
                       resumeValue == "false";
            },
            std::chrono::milliseconds(4000));
        check(mpvResumed, "§2 恢复：mpv IPC pause 属性 == false（续播下达到播放器，实测 \"" +
                              resumeValue + "\"）");
    }

    // ── §3 watcher 事件注入（接口注入；锁屏/换屏无法程序化模拟，事件通道在此实证
    //    服务侧裁决；电池为轮询通道）──
    {
        auto player = std::make_unique<module::wallpaper::NullPlayerBackend>();
        auto watchers = std::make_unique<FakeYieldWatchers>();
        FakeYieldWatchers* watchersRaw = watchers.get();
        module::wallpaper::WallpaperService fakeService;
        fakeService.setSettingsPathForTest((tempDir / "settings-fake.json").string());
        fakeService.setVideoScannerForTest([&tempDir](const std::string&) {
            module::wallpaper::VideoScanResult result;
            result.status = module::wallpaper::LibraryScanStatus::Ok;
            result.files = {(tempDir / "clip-a.mp4").string()};
            return result;
        });
        fakeService.setYieldWatchersForTest(std::move(watchers));
        fakeService.setPlayerForTest(std::move(player));
        fakeService.start();
        std::string fakeError;
        if (fakeService.selectFolder(tempDir.string(), &fakeError)) {
            const bool fakePlaying = waitUntil(
                [&fakeService]() {
                    const module::wallpaper::WallpaperSnapshot snap = fakeService.snapshot();
                    return !snap.scanning &&
                           snap.availability == module::wallpaper::Availability::Playing;
                },
                std::chrono::milliseconds(8000));

            // 锁屏事件：锁 → 暂停 + ScreenLocked 位；解锁 → 恢复。
            watchersRaw->fireHold(module::wallpaper::HoldReason::ScreenLocked, true);
            const bool lockHeld = waitUntil(
                [&fakeService]() {
                    const module::wallpaper::WallpaperSnapshot snap = fakeService.snapshot();
                    return snap.paused && snap.systemHolds ==
                                              module::wallpaper::holdReasonBit(
                                                  module::wallpaper::HoldReason::ScreenLocked);
                },
                std::chrono::milliseconds(3000));
            check(fakePlaying && lockHeld,
                  "§3 锁屏事件：锁屏回调 → 暂停 + ScreenLocked 位（事件通道裁决）");
            watchersRaw->fireHold(module::wallpaper::HoldReason::ScreenLocked, false);
            const bool lockReleased = waitUntil(
                [&fakeService]() {
                    return !fakeService.snapshot().paused;
                },
                std::chrono::milliseconds(3000));
            check(lockReleased, "§3 锁屏事件：解锁回调 → 解除暂停");

            // 电池轮询 + 豁免：上报电池 → 暂停；「电池供电时继续」→ 即便持续上报也恢复。
            fakeService.setPauseOnBattery(true);
            watchersRaw->setBattery(true);
            const bool batteryHeld = waitUntil(
                [&fakeService]() {
                    const module::wallpaper::WallpaperSnapshot snap = fakeService.snapshot();
                    return snap.paused && snap.systemHolds ==
                                              module::wallpaper::holdReasonBit(
                                                  module::wallpaper::HoldReason::Battery);
                },
                std::chrono::milliseconds(4000));
            check(batteryHeld, "§3 电池轮询：电池上报 → 暂停 + Battery 位（≈1s 分频轮询）");
            fakeService.setPauseOnBattery(false);
            const bool batteryExempt = waitUntil(
                [&fakeService]() { return !fakeService.snapshot().paused; },
                std::chrono::milliseconds(3000));
            check(batteryExempt,
                  "§3 电池豁免：「电池供电时继续」开关 → 持续上报电池仍解除暂停");
            fakeService.setPauseOnBattery(true);
            const bool batteryBack = waitUntil(
                [&fakeService]() {
                    const module::wallpaper::WallpaperSnapshot snap = fakeService.snapshot();
                    return snap.paused && snap.systemHolds ==
                                              module::wallpaper::holdReasonBit(
                                                  module::wallpaper::HoldReason::Battery);
                },
                std::chrono::milliseconds(4000));
            check(batteryBack, "§3 电池豁免：重开门控 → 下一拍电池位回来（持续上报在位）");
        } else {
            check(false, "§3 事件注入：fake 服务 selectFolder 失败: " + fakeError);
        }
        fakeService.stop();
    }

    // ── §4 停止无残留 ──
    const std::vector<std::uint32_t> spawned =
        setDifference(pidsOfImageName(mpvImageName), mpvBaseline);
    const std::uint32_t mpvPid = spawned.empty() ? 0 : spawned[0];

    const auto stopStarted = Clock::now();
    service.stop();
    const double stopElapsed =
        std::chrono::duration<double>(Clock::now() - stopStarted).count();
    check(stopElapsed < 3.0, "§4 停止：service.stop() 耗时 " + std::to_string(stopElapsed) +
                                 "s < 3s（stop 梯子契约，含 watcher join）");

    const bool exited = waitUntil([&]() { return !processStillRunning(mpvPid); },
                                  std::chrono::milliseconds(3000));
    check(exited && !processStillRunning(mpvPid), "§4 停止：mpv pid " +
                                                      std::to_string(mpvPid) +
                                                      " 已退出且无残留（≤3s）");

    ipc.disconnect();
    bool pipeGone = true;
    {
        HANDLE probe = CreateFileW(utf8ToWide(pipeName).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (probe != INVALID_HANDLE_VALUE) {
            CloseHandle(probe);
            pipeGone = false;
        }
    }
    check(pipeGone, "§4 停止：IPC 管道不可再连（服务端已随 mpv 退出）");

    const int windowsAfterStop = countOwnTopLevelWindows();
    check(windowsAfterStop == windowsBefore,
          "§4 停止：桌面还原（本进程顶层窗口 " + std::to_string(windowsAfterStop) +
              " == 启动前 " + std::to_string(windowsBefore) +
              "，宿主消息窗 + watcher 消息窗都已销毁）");

    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }
    if (!passed) {
        std::cerr << "wallpaper_yield_probe: FAIL\n";
        return 1;
    }
    std::cout << "wallpaper_yield_probe: PASS\n";
    return 0;
#endif
}
