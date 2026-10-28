// src/platform/window_visibility_win.cpp
// Windows 实现：原 app_sidebar.cpp 内的 Win32 可见态查询原样迁入，判定逻辑、
// 阈值与降级语义逐行不变（行为等价）。
//
// 主窗 HWND 一次性捕获（首次门控查询时执行一次，static 缓存终身复用）：
// EnumWindows 扫本进程顶层窗口，取「可见 + 非 WS_EX_TOOLWINDOW」命中——
// styledMenu 弹层窗带 TOOLWINDOW、single_instance 辅助窗隐藏，首帧时主窗是
// 唯一命中；标题精确等于 "DevDesk" 者优先（将来有第二个可见顶窗也保命）。
// 捕获不到（无头单测/窗口未就绪）返回空 → 门控按可见降级（fail-open），
// 零成本兜底仍由 EUI 主循环的 hidden/iconified 跳帧保证。
#include "window_visibility.h"

#include "core/logging.h"

// 宏守卫与 EUI-NEO detail 头同款（LEAN_AND_MEAN + NOMINMAX），本 TU 不引
// EUI 头，仍统一防止 min/max 宏污染。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>

namespace platform::window_visibility {

namespace {

struct MainHwndSearch {
    HWND fallback = nullptr;
    HWND titled = nullptr;
};

BOOL CALLBACK enumMainWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<MainHwndSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (ctx->fallback == nullptr) ctx->fallback = hwnd;
    WCHAR title[64] = {};
    if (GetWindowTextW(hwnd, title, 64) > 0 &&
        wcscmp(title, L"DevDesk") == 0 && ctx->titled == nullptr) {
        ctx->titled = hwnd;
    }
    return TRUE;
}

HWND mainWindowHwnd() {
    static const HWND cached = [] {
        MainHwndSearch search;
        EnumWindows(enumMainWindowsProc, reinterpret_cast<LPARAM>(&search));
        const HWND picked =
            search.titled != nullptr ? search.titled : search.fallback;
        if (picked == nullptr) {
            LOG_WARN("动态背景：主窗 HWND 一次性捕获未命中（无头环境或窗口未就绪），"
                     "可见态门控本轮按可见降级（EUI 主循环跳帧兜底不变）");
        }
        return picked;
    }();
    return cached;
}

} // namespace

bool mainWindowVisible() {
    const HWND hwnd = mainWindowHwnd();
    if (hwnd == nullptr) return true;  // 捕获失败按可见降级
    return !IsIconic(hwnd) && IsWindowVisible(hwnd);
}

} // namespace platform::window_visibility