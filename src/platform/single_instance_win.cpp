// src/platform/single_instance_win.cpp
// Windows 单实例实现：命名互斥量 + 命名事件 + 监听线程。
// 第一实例持有互斥量句柄进程生命周期、不提前释放（锁随进程存活），同时创建命名事件
// 并启动监听线程，收到信号后调用 app::requestShow() 复用托盘「显示主窗口」代码路径。
// 第二实例 CreateMutexW → ERROR_ALREADY_EXISTS → 返回 false，调用方再 OpenEventW +
// SetEvent 通知第一实例。
#include "platform/single_instance.h"

#include <eui/app.h>

#include <atomic>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

constexpr const wchar_t* kMutexName = L"Local\\DevDesk.SingleInstance.Mutex";
constexpr const wchar_t* kEventName = L"Local\\DevDesk.SingleInstance.Event";

// 互斥量句柄：进程生命周期持有，不 CloseHandle 到退出——锁随进程存活，
// 提前释放会让 OS 删掉命名互斥量，后续第二实例 CreateMutexW 会误判为「无实例」。
HANDLE g_mutexHandle = nullptr;

// 监听线程：WaitForSingleObject 等待事件 → 收到信号 → app::requestShow() 唤起主窗，
// 循环直到 g_listenerRunning=false（当前实现永不置 false，进程退出时 OS 回收线程）。
std::atomic<bool> g_listenerRunning{false};
std::thread g_listenerThread;

void listenerLoop() {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, kEventName);
    if (!hEvent) {
        // CreateEventW 失败：事件已被第一实例的 acquireInstanceLock 创建，
        // 这里用 OpenEventW 兜底（理论上不会走到，因 acquireInstanceLock 先建事件再启线程）
        hEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kEventName);
    }
    if (!hEvent) return;

    while (g_listenerRunning.load(std::memory_order_relaxed)) {
        // 500ms 轮询以便响应 g_listenerRunning 变化（当前永不置 false，
        // 仅在进程退出时自然终止；保底设计便于将来加优雅停机）
        DWORD result = WaitForSingleObject(hEvent, 500);
        if (result == WAIT_OBJECT_0) {
            ResetEvent(hEvent);
            // 复用托盘「显示主窗口」代码路径：保证 hiddenToTray 等内部状态一致，
            // 不自写一套 ShowWindow
            app::requestShow();
        } else if (result == WAIT_FAILED) {
            break;
        }
    }

    CloseHandle(hEvent);
}

} // namespace

namespace platform::single_instance {

bool acquireInstanceLock() {
    HANDLE h = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!h) return false;

    bool isFirst = (GetLastError() != ERROR_ALREADY_EXISTS);

    if (isFirst) {
        // 句柄进程生命周期持有：锁随进程存活，不能提前释放
        g_mutexHandle = h;

        // 先建事件（信号通路），再启监听线程。保证 notifyExistingInstance 的 OpenEventW
        // 在监视线程执行到 WaitForSingleObject 之前就能命中事件对象。
        CreateEventW(nullptr, TRUE, FALSE, kEventName);

        g_listenerRunning.store(true, std::memory_order_relaxed);
        g_listenerThread = std::thread(listenerLoop);
    } else {
        // 第二实例：不持锁，不启监听
        CloseHandle(h);
    }

    return isFirst;
}

void notifyExistingInstance() {
    // 第一实例的 acquireInstanceLock 先建事件再启线程，事件一定先于监听端就绪。
    // 极端并发（两进程同时起、第二进程 execute 比第一进程的 CreateEventW 还快）
    // 概率极低，这里 retry 兜底。
    for (int attempt = 0; attempt < 10; ++attempt) {
        HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kEventName);
        if (hEvent) {
            SetEvent(hEvent);
            CloseHandle(hEvent);
            return;
        }
        Sleep(50);
    }
}

} // namespace platform::single_instance