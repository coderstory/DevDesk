// src/platform/single_instance_win.cpp
// Windows 单实例实现：命名互斥量 + 命名事件 + 监听线程 +（F-01）命名 section 路径送达。
// 第一实例持有互斥量句柄进程生命周期、不提前释放（锁随进程存活），同时创建命名事件
// 并启动监听线程，收到信号后调用 app::requestShow() 复用托盘「显示主窗口」代码路径，
// 再读命名 section（F-01：第二实例带来的文件路径）。
// 第二实例 CreateMutexW → ERROR_ALREADY_EXISTS → 返回 false，调用方再 notifyExistingInstance
// 带路径唤醒：整段事务持发送互斥 → 写 section → SetEvent 唤醒 → 等专名 ack。
//
// 线程契约（F-01 握手时序）：
//   发送方（第二实例任意线程）：持发送互斥的整段事务内 [写 section → SetEvent(唤醒) →
//     WaitForSingleObject(专名 ack, 封顶)]，映射句柄持到 ack/超时才关；
//   接收方（首实例监听线程）：WaitForSingleObject(唤醒事件) → ResetEvent →
//     consumeSectionSignalOnce()[读映射 → 整包校验 → SetEvent(载荷内专名 ack) → 路径回调]。
//   映射读取只发生在监听线程；发送互斥保证任一时刻 section 内容只属一个发送事务。
#include "platform/single_instance.h"

#include <eui/app.h>

#include "core/logging.h"

#include <atomic>
#include <cstring>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// ── 命名对象根名：默认 DevDesk（与既有单实例命名完全一致）；测试缝可覆写以隔离
// 开发机真实实例通道。生产路径从不调用覆写，根名恒默认。 ──
std::wstring g_nameRoot = L"DevDesk";

std::wstring mutexName() { return L"Local\\" + g_nameRoot + L".SingleInstance.Mutex"; }
std::wstring eventName() { return L"Local\\" + g_nameRoot + L".SingleInstance.Event"; }
std::wstring sendMutexName() {
    return L"Local\\" + g_nameRoot + L".SingleInstance.Send.Mutex";
}
std::wstring sectionName() { return L"Local\\" + g_nameRoot + L".SingleInstance.Section"; }
std::wstring ackPrefix() { return L"Local\\" + g_nameRoot + L".SingleInstance.Ack."; }

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            w.data(), n);
    }
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            s.data(), n, nullptr, nullptr);
    }
    return s;
}

// 互斥量句柄：进程生命周期持有，不 CloseHandle 到退出——锁随进程存活，
// 提前释放会让 OS 删掉命名互斥量，后续第二实例 CreateMutexW 会误判为「无实例」。
HANDLE g_mutexHandle = nullptr;

// 监听线程：WaitForSingleObject 等待事件 → 收到信号 → app::requestShow() 唤起主窗 +
// 读 section 送达路径，循环直到 g_listenerRunning=false（当前实现永不置 false，
// 进程退出时 OS 回收线程；线程启动即 detach，静态析构期不存在 joinable 线程）。
std::atomic<bool> g_listenerRunning{false};

// 路径接收回调（app::assembly 注册）：监听线程校验载荷后触发，见头文件线程契约
platform::single_instance::PathsHandler g_pathsHandler;

void listenerLoop() {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, eventName().c_str());
    if (!hEvent) {
        // CreateEventW 失败：事件已被第一实例的 acquireInstanceLock 创建，
        // 这里用 OpenEventW 兜底（理论上不会走到，因 acquireInstanceLock 先建事件再启线程）
        hEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName().c_str());
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
            // F-01：读映射 → 整包校验 → SetEvent(载荷内专名 ack) → 路径回调入
            // app_assembly 队列。校验失败整包丢弃（悬垂/半包永不误消费、不误 ack）
            platform::single_instance::consumeSectionSignalOnce();
        } else if (result == WAIT_FAILED) {
            break;
        }
    }

    CloseHandle(hEvent);
}

} // namespace

namespace platform::single_instance {

bool acquireLockOnce() {
    HANDLE h = CreateMutexW(nullptr, TRUE, mutexName().c_str());
    if (!h) return false;

    bool isFirst = (GetLastError() != ERROR_ALREADY_EXISTS);

    if (isFirst) {
        // 句柄进程生命周期持有：锁随进程存活，不能提前释放
        g_mutexHandle = h;

        // 先建事件（信号通路），再启监听线程。保证 notifyExistingInstance 的 OpenEventW
        // 在监视线程执行到 WaitForSingleObject 之前就能命中事件对象。
        CreateEventW(nullptr, TRUE, FALSE, eventName().c_str());

        g_listenerRunning.store(true, std::memory_order_relaxed);
        std::thread(listenerLoop).detach();
    } else {
        // 第二实例：不持锁，不启监听
        CloseHandle(h);
    }

    return isFirst;
}

bool acquireInstanceLock() {
    // 进程内幂等（magic static 只判一次）：EUI 框架会多次调 dslAppConfig（启动早期一次、
    // 窗口高度计算经 initialWindowHeight 又一次），而命名 mutex 对同进程的第二次
    // CreateMutexW 同样返回 ERROR_ALREADY_EXISTS——不防重入会把已运行的唯一实例误判成
    // 第二实例，启动期直接 std::exit。任何后续调用沿用首判结果。
    static const bool isFirst = acquireLockOnce();
    return isFirst;
}

void setOpenPathsHandler(PathsHandler handler) { g_pathsHandler = std::move(handler); }

bool consumeSectionSignalOnce() {
    HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName().c_str());
    if (!section) return false;  // 无载荷（普通托盘式唤起）→ 静默返回
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(section);
        return false;
    }
    // 先整块快照再解码：把「读映射」压成一次 memcpy，缩短与发送方覆写交叠的窗口；
    // 快照若被写穿，解码按整包校验拒收
    std::string snapshot(static_cast<const char*>(view), kMaxPayloadBytes);
    UnmapViewOfFile(view);
    CloseHandle(section);

    PathPayload payload;
    if (!decodePathPayload(snapshot, payload)) {
        LOG_WARN("单实例：收到非法路径载荷（悬垂 section / 半包 / 魔数错），整包丢弃");
        return false;
    }

    // ack 载荷内专名（配对防错配）：发送方超时退出后事件对象可能已消失，打不开只 LOG，
    // 合法载荷仍投递（用户打开意图优先）
    HANDLE ack = OpenEventW(EVENT_MODIFY_STATE, FALSE, utf8ToWide(payload.ackName).c_str());
    if (ack) {
        SetEvent(ack);
        CloseHandle(ack);
    } else {
        LOG_WARN("单实例：载荷专名 ack 事件不存在（发送方已超时退出），路径仍投递");
    }

    if (g_pathsHandler) {
        g_pathsHandler(payload.paths);
    } else {
        LOG_WARN("单实例：路径接收回调未注册，本次送达的 " +
                 std::to_string(payload.paths.size()) + " 个路径丢弃");
    }
    return true;
}

bool notifyExistingInstance(const std::vector<std::string>& paths,
                            unsigned ackTimeoutMs) {
    // ack 专名：前缀 + pid + tick + 进程内序号——同 pid 同 tick 重入也不撞名
    static std::atomic<uint32_t> ackSeq{0};
    const std::wstring ackName =
        ackPrefix() + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." +
        std::to_wstring(ackSeq.fetch_add(1, std::memory_order_relaxed));
    HANDLE ackEvent = CreateEventW(nullptr, TRUE, FALSE, ackName.c_str());
    if (!ackEvent) return false;
    const std::string payload =
        encodePathPayload(PathPayload{wideToUtf8(ackName), paths});
    if (payload.empty()) {
        // 超上界（整包 > 32KB / 路径数 > 256）：不可投递
        CloseHandle(ackEvent);
        return false;
    }

    // 整段事务持发送互斥：串行化并发第二实例，消除同名 section「后写覆先写」。
    // 每个持有者最长 = 唤醒事件打开重试(≤500ms) + ack 封顶，锁等待有界。
    // WAIT_ABANDONED（前任持有者崩溃）照常接手：本事务写满整个载荷后才唤醒
    HANDLE sendMutex = CreateMutexW(nullptr, FALSE, sendMutexName().c_str());
    if (!sendMutex) {
        CloseHandle(ackEvent);
        return false;
    }
    const DWORD acquired = WaitForSingleObject(sendMutex, INFINITE);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
        CloseHandle(sendMutex);
        CloseHandle(ackEvent);
        return false;
    }

    bool acked = false;
    HANDLE wake = nullptr;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE, 0, kMaxPayloadBytes,
                                        sectionName().c_str());
    if (section) {
        // 首实例未就绪兜底（acquireLockOnce 先建事件后启线程，正常时一次命中）；
        // 重试封顶 500ms 后放弃整段事务
        for (int attempt = 0; attempt < 10 && !wake; ++attempt) {
            wake = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName().c_str());
            if (!wake) Sleep(50);
        }
    }
    if (section && wake) {
        void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
        if (view) {
            memcpy(view, payload.data(), payload.size());
            UnmapViewOfFile(view);  // 视图即用即还；映射对象靠 section 句柄存活
            SetEvent(wake);
            // 持映射句柄等到 ack/超时才关：首实例读取期映射对象必须存活
            acked = (WaitForSingleObject(ackEvent, ackTimeoutMs) == WAIT_OBJECT_0);
        }
    }
    if (!acked) {
        LOG_WARN("单实例：路径送达失败（首实例未就绪/确认超时/映射不可写），退出前放弃");
    }
    if (wake) CloseHandle(wake);
    if (section) CloseHandle(section);
    ReleaseMutex(sendMutex);
    CloseHandle(sendMutex);
    CloseHandle(ackEvent);
    return acked;
}

// 测试缝实现
void setSingleInstanceRootForTest(const std::string& root) {
    g_nameRoot.assign(root.begin(), root.end());  // 测试根名为 ASCII，逐字符展宽
}

bool writeSectionForTest(const std::string& bytes) {
    if (bytes.size() > kMaxPayloadBytes) return false;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE, 0, kMaxPayloadBytes,
                                        sectionName().c_str());
    if (!section) return false;
    void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
    if (!view) {
        CloseHandle(section);
        return false;
    }
    if (!bytes.empty()) memcpy(view, bytes.data(), bytes.size());
    UnmapViewOfFile(view);
    CloseHandle(section);
    return true;
}

} // namespace platform::single_instance
