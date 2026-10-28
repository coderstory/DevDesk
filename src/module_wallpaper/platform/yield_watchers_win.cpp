// src/module_wallpaper/platform/yield_watchers_win.cpp
// Win32 让路事件源：隐藏顶层消息窗（WTS 会话通知 + WM_DISPLAYCHANGE 广播）
// + GetSystemPowerStatus / EnumWindows 轮询。
//
// 线程模型（同 wallpaper_host_win 的消息窗模式）：
//   - start() 起唯一 watcher 线程；窗口类注册、CreateWindowEx、WTS 注册、消息泵、
//     DestroyWindow 全部只在该线程执行（Win32 窗口有线程亲和性）。
//   - stop() 投递 WM_CLOSE 并 join 线程；与回调并发安全（回调先拷贝再调用）。
//   - 轮询方法无窗口依赖，任意线程直接查系统状态。
//
// 为什么不建 message-only 窗口：WM_WTSSESSION_CHANGE / WM_DISPLAYCHANGE 是系统
// 广播，只投递给顶层窗口——message-only（HWND_MESSAGE 父）收不到，必须同
// wallpaper_host 一样建 WS_POPUP 隐藏工具窗。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>

#include "module_wallpaper/platform/yield_watchers.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "core/logging.h"

namespace module::wallpaper {
namespace {

constexpr wchar_t kWatcherClass[] = L"DevDeskWallpaperYieldWatcher";

// 壁纸自身窗口类名（host 的四类 + watcher 自己）：全屏扫描排除。
// 契约见 yield_watchers.h 的 pollFullscreenOtherProcess 注释——按类名排除而非
// 按进程排除：宿主层是 WS_CHILD 不会被 EnumWindows 枚举，而同进程的全屏测试窗
// （探针）必须能触发让路。
const wchar_t* const kExcludedClasses[] = {
    L"Progman",                       // 桌面本体（覆盖整屏，永远不该算全屏应用）
    L"WorkerW",                       // 壁纸宿主背后的 WorkerW 层
    L"Shell_TrayWnd",                 // 任务栏
    L"Shell_SecondaryTrayWnd",        // 副屏任务栏
    L"DevDeskWallpaperHost",
    L"DevDeskWallpaperVideo",
    L"DevDeskWallpaperImage",
    L"DevDeskWallpaperMessage",
    kWatcherClass,
};

// DWMWA_CLOAKED 的取值（dwmapi.h；动态加载避免为单一标志引入链接依赖）。
constexpr DWORD kDwmwaCloaked = 14;

using DwmGetWindowAttributeFn = LONG(WINAPI*)(HWND, DWORD, void*, DWORD);

DwmGetWindowAttributeFn loadDwmGetWindowAttribute() {
    static DwmGetWindowAttributeFn cached = nullptr;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm != nullptr) {
            // FARPROC 与目标签名类型不同（long long int(*)() vs 带参函数指针），
            // 直接 cast 触发 -Wcast-function-type；经 void* 中转是 Win32 动态加载
            // 的标准消警写法，语义不变。
            cached = reinterpret_cast<DwmGetWindowAttributeFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(dwm, "DwmGetWindowAttribute")));
        }
    }
    return cached;
}

// UWP 挂起 / 虚拟桌面隐藏的窗口会被 cloak：肉眼不可见却 IsWindowVisible 为真，
// 不排除会把后台 UWP 误判成全屏应用。
bool isCloaked(HWND window) {
    DwmGetWindowAttributeFn dwm = loadDwmGetWindowAttribute();
    if (dwm == nullptr) return false;
    DWORD cloaked = 0;
    if (dwm(window, kDwmwaCloaked, &cloaked, sizeof(cloaked)) != 0) return false;
    return cloaked != 0;
}

bool isExcludedClass(HWND window) {
    wchar_t className[64] = {};
    if (GetClassNameW(window, className, 64) == 0) return true;
    for (const wchar_t* excluded : kExcludedClasses) {
        if (lstrcmpW(className, excluded) == 0) return true;
    }
    return false;
}

struct FullscreenScan {
    bool found = false;
};

BOOL CALLBACK fullscreenScanProc(HWND window, LPARAM param) {
    FullscreenScan* scan = reinterpret_cast<FullscreenScan*>(param);
    if (scan->found) return TRUE;
    if (!IsWindowVisible(window)) return TRUE;
    if (IsIconic(window)) return TRUE;
    const LONG_PTR exStyles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyles & WS_EX_TOOLWINDOW) != 0) return TRUE;
    if (isExcludedClass(window)) return TRUE;
    if (isCloaked(window)) return TRUE;

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) return TRUE;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return TRUE;
    // 契约：rect 恰好覆盖整个显示器才算全屏（最大化窗口带不可用区偏移时不算）。
    if (rect.left == info.rcMonitor.left && rect.top == info.rcMonitor.top &&
        rect.right == info.rcMonitor.right && rect.bottom == info.rcMonitor.bottom) {
        scan->found = true;
        return FALSE;
    }
    return TRUE;
}

} // namespace

class YieldWatchersWin final : public YieldWatchers {
public:
    YieldWatchersWin() = default;
    ~YieldWatchersWin() override { stop(); }

    YieldWatchersWin(const YieldWatchersWin&) = delete;
    YieldWatchersWin& operator=(const YieldWatchersWin&) = delete;

    void setHoldCallback(HoldCallback callback) override {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        holdCallback_ = std::move(callback);
    }

    void setDisplayChangedCallback(DisplayChangedCallback callback) override {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        displayChangedCallback_ = std::move(callback);
    }

    bool start(std::string* error) override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (thread_.joinable()) return true;  // 幂等
        stopRequested_.store(false);
        try {
            thread_ = std::thread(&YieldWatchersWin::threadMain, this);
        } catch (const std::system_error& failure) {
            if (error != nullptr) {
                *error = std::string("创建让路 watcher 线程失败: ") + failure.what();
            }
            return false;
        }
        return true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (!thread_.joinable()) return;
        stopRequested_.store(true);
        {
            // WM_CLOSE → DefWindowProc → DestroyWindow → WM_DESTROY → PostQuitMessage：
            // 消息泵在 GetMessage 阻塞中也能被确定性地唤醒退出。
            std::lock_guard<std::mutex> windowLock(windowMutex_);
            if (window_ != nullptr) PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        // watcher 线程不取 lifecycleMutex_，持锁 join 无死锁面。
        thread_.join();
        std::lock_guard<std::mutex> windowLock(windowMutex_);
        window_ = nullptr;
    }

    bool pollOnBattery() override {
        SYSTEM_POWER_STATUS status{};
        if (GetSystemPowerStatus(&status) == 0) return false;  // 查询失败：不让路
        // ACLineStatus：1 = AC 供电；0 = 电池；255 = 未知（按 AC 处理，宁可不暂停）。
        return status.ACLineStatus == 0;
    }

    bool pollFullscreenOtherProcess() override {
        FullscreenScan scan;
        EnumWindows(&fullscreenScanProc, reinterpret_cast<LPARAM>(&scan));
        return scan.found;
    }

private:
    static LRESULT CALLBACK watcherProc(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<YieldWatchersWin*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
            case WM_WTSSESSION_CHANGE:
                if (self != nullptr) {
                    // 只认本会话锁/解锁；断连/重连语义不属于壁纸让路。
                    if (wparam == WTS_SESSION_LOCK) {
                        self->fireHold(HoldReason::ScreenLocked, true);
                    } else if (wparam == WTS_SESSION_UNLOCK) {
                        self->fireHold(HoldReason::ScreenLocked, false);
                    }
                }
                return 0;
            case WM_DISPLAYCHANGE:
                if (self != nullptr) self->fireDisplayChanged();
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);  // 消息泵唯一退出通道（GetMessage 只认 WM_QUIT）
                return 0;
            default:
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void fireHold(HoldReason reason, bool active) {
        HoldCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = holdCallback_;
        }
        if (callback) callback(reason, active);
    }

    void fireDisplayChanged() {
        DisplayChangedCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = displayChangedCallback_;
        }
        if (callback) callback();
    }

    void threadMain() {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &YieldWatchersWin::watcherProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWatcherClass;
        if (RegisterClassW(&windowClass) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_WARN("壁纸: 注册让路 watcher 窗口类失败");
            return;
        }
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kWatcherClass,
                                      L"DevDeskWallpaperYieldWatcher", WS_POPUP, 0, 0, 0, 0,
                                      nullptr, nullptr, instance, this);
        if (window == nullptr) {
            LOG_WARN("壁纸: 创建让路 watcher 消息窗失败");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(windowMutex_);
            window_ = window;
        }
        if (WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) == 0) {
            // 注册失败只失去锁屏让路：换屏/电池/全屏三路不受影响，不作致命处理。
            LOG_WARN("壁纸: WTS 会话通知注册失败（锁屏让路不可用）");
        }
        // stop 与建窗竞态：stop 投 WM_CLOSE 时 window_ 尚空则无人接收、GetMessage
        // 将永久阻塞——开泵前补判一次，保证 stop() 的 join 有限时返回。
        if (!stopRequested_.load()) {
            MSG message{};
            while (!stopRequested_.load()) {
                const BOOL got = GetMessageW(&message, nullptr, 0, 0);
                if (got <= 0) break;  // WM_QUIT（WM_DESTROY 链）或错误 → 退出
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        WTSUnRegisterSessionNotification(window);
        if (IsWindow(window)) DestroyWindow(window);
    }

    std::mutex lifecycleMutex_;  // 串行化 start/stop
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};

    std::mutex windowMutex_;  // window_ 由 watcher 线程写、stop 线程读
    HWND window_ = nullptr;

    std::mutex callbackMutex_;
    HoldCallback holdCallback_;
    DisplayChangedCallback displayChangedCallback_;
};

std::unique_ptr<YieldWatchers> createYieldWatchers() {
    return std::make_unique<YieldWatchersWin>();
}

} // namespace module::wallpaper
