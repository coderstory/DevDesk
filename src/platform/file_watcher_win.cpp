// src/platform/file_watcher_win.cpp
// Windows 实现：ReadDirectoryChangesW + OVERLAPPED + 专用后台线程。
//
// 线程模型：
//   start()  在调用线程打开目录句柄（失败即刻 Result 返回，无线程残留）
//   worker   常驻循环：保持一个未决 ReadDirectoryChangesW →
//            WaitForMultipleObjects(事件 | 停止) → 解析 → 去抖 → 回调
//   stop()   置 running=false + SetEvent 唤醒 worker，worker 自行
//            CancelIoEx 作废未决读并退出；stop() join。句柄只在 join 后关闭，
//            保证 worker 不会碰到失效 HANDLE。
//
// 去抖策略：pending 为空时无限期等待（零空转）；收到事件后解析、立刻重发
// 读请求（事件捕获无间隙），再按 debounceMs 静默窗口等待——窗口内再来事件
// 则重置窗口，窗口到点整批交付回调。
//
// 读请求「完成→重发」之间存在微秒级空窗，此间发生的改动不会被捕获——
// 消费方轮询兜底覆盖（见头文件「已知边界」）。
#include "platform/file_watcher.h"

#include "core/logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace platform {

namespace {

constexpr DWORD kWatchMask =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

// 事件链每条约 12B + 路径 UTF-16；64KB 足以让突发合并进一次交付
constexpr DWORD kBufBytes = 64 * 1024;

FileWatchAction mapAction(DWORD action) {
    switch (action) {
        case FILE_ACTION_ADDED:            return FileWatchAction::Created;
        case FILE_ACTION_REMOVED:          return FileWatchAction::Deleted;
        case FILE_ACTION_MODIFIED:         return FileWatchAction::Modified;
        case FILE_ACTION_RENAMED_OLD_NAME:
        case FILE_ACTION_RENAMED_NEW_NAME: return FileWatchAction::Renamed;
        default:                           return FileWatchAction::Unknown;
    }
}

// FILE_NOTIFY_INFORMATION.FileName（UTF-16，反斜杠）→ UTF-8 POSIX 分隔
std::string notifyPathToUtf8(const FILE_NOTIFY_INFORMATION& info,
                             DWORD bytesLeft) {
    // FileNameLength 是字节数；越界（损坏链）按截断处理
    const DWORD maxChars = bytesLeft >= sizeof(FILE_NOTIFY_INFORMATION)
                               ? (bytesLeft - FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName)) /
                                     sizeof(wchar_t)
                               : 0;
    DWORD chars = info.FileNameLength / sizeof(wchar_t);
    if (chars > maxChars) chars = maxChars;
    if (chars == 0) return {};

    const int need = WideCharToMultiByte(CP_UTF8, 0, info.FileName,
                                         static_cast<int>(chars),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, info.FileName, static_cast<int>(chars),
                        out.data(), need, nullptr, nullptr);
    for (char& ch : out) {
        if (ch == '\\') ch = '/';
    }
    return out;
}

// 解析一次交付的整条 FILE_NOTIFY_INFORMATION 链追加到 out
void parseNotificationChain(char* buffer, DWORD bytes,
                            std::vector<FileWatchEvent>& out) {
    DWORD offset = 0;
    while (offset < bytes) {
        if (bytes - offset < FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName)) {
            break;  // 残链，安全放弃
        }
        const auto* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
        FileWatchEvent ev;
        ev.action = mapAction(info->Action);
        ev.relativePath = notifyPathToUtf8(*info, bytes - offset);
        out.push_back(std::move(ev));
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
    }
}

std::string lastWinError(unsigned long code) {
    char* msg = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string out = "err=" + std::to_string(code);
    if (n > 0 && msg) {
        out += " ";
        out += msg;
    }
    if (msg) LocalFree(msg);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

} // namespace

struct FileWatcher::Impl {
    std::atomic<bool> running{false};
    std::thread worker;
    HANDLE dirHandle = nullptr;   // 被监听目录（OVERLAPPED 打开）
    HANDLE stopEvent = nullptr;   // 手动重置，stop() SetEvent 唤醒 worker
    FileWatchCallback callback;
    DWORD debounceMs = 200;

    void joinWorker() {
        running.store(false);
        if (stopEvent) SetEvent(stopEvent);
        if (worker.joinable()) {
            // 回调线程内自 stop → 跳过 join（服务析构发生在主线程，此处仅防御）
            if (worker.get_id() == std::this_thread::get_id()) {
                worker.detach();
            } else {
                worker.join();
            }
        }
        if (dirHandle) { CloseHandle(dirHandle); dirHandle = nullptr; }
        if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
    }

    // 后台线程主体（Impl 成员：匿名 ns 自由函数无权命名私有嵌套类型）
    void runWorker() {
        std::vector<char> buffer(kBufBytes);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            running.store(false);
            return;
        }

        std::vector<FileWatchEvent> pending;
        bool readIssued = false;

        while (running.load()) {
            if (!readIssued) {
                ResetEvent(ov.hEvent);
                DWORD transferredNow = 0;
                const BOOL ok = ReadDirectoryChangesW(
                    dirHandle, buffer.data(), kBufBytes, TRUE,
                    kWatchMask, &transferredNow, &ov, nullptr);
                if (!ok) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) {
                        LOG_WARN("FileWatcher: 监听读请求失败，退出监听（" +
                                 lastWinError(err) + "）");
                        break;
                    }
                }
                // 同步完成（目录里已有积压通知）时事件已置位，下面等待立即返回
                readIssued = true;
            }

            // pending 空 → INFINITE（零空转）；有货 → 去抖静默窗口
            const DWORD timeout = pending.empty() ? INFINITE : debounceMs;
            HANDLE waits[2] = {ov.hEvent, stopEvent};
            const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout);

            if (r == WAIT_OBJECT_0 + 1) {
                // stop()：作废未决读，等其真正结束后退出（本批 pending 丢弃）
                CancelIoEx(dirHandle, &ov);
                DWORD dummy = 0;
                GetOverlappedResult(dirHandle, &ov, &dummy, TRUE);
                break;
            }
            if (r == WAIT_TIMEOUT) {
                // 静默窗口到点：整批交付（读请求保持未决，捕获不中断）
                if (!pending.empty()) {
                    std::vector<FileWatchEvent> batch;
                    batch.swap(pending);
                    if (callback) callback(std::move(batch));
                }
                continue;
            }
            if (r != WAIT_OBJECT_0) {
                LOG_WARN("FileWatcher: 等待句柄异常（" +
                         lastWinError(static_cast<unsigned long>(r)) + "），退出监听");
                break;
            }

            DWORD transferred = 0;
            if (!GetOverlappedResult(dirHandle, &ov, &transferred, FALSE)) {
                const DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED) {
                    readIssued = false;  // 罕见：刚被 cancel，循环顶部处理 stop
                    continue;
                }
                LOG_WARN("FileWatcher: 取回通知失败，退出监听（" +
                         lastWinError(err) + "）");
                break;
            }
            if (transferred == 0) {
                // 缓冲区溢出（0 字节但确有改动）：无法逐项枚举，交 Unknown 让消费方全量重扫
                pending.push_back({FileWatchAction::Unknown, std::string()});
            } else {
                parseNotificationChain(buffer.data(), transferred, pending);
            }
            readIssued = false;  // 顶部重发下一个未决读
        }

        running.store(false);  // 线程退出 = 监听失效（active() 随之变 false）
        CloseHandle(ov.hEvent);
    }
};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() {
    stop();
}

core::Result<void> FileWatcher::start(const std::filesystem::path& dir,
                                      FileWatchCallback onChange,
                                      std::chrono::milliseconds debounceMs) {
    if (impl_->worker.get_id() == std::this_thread::get_id()) {
        // 回调线程内重启会让 joinWorker 走 detach 兜底、旧线程继续持旧句柄——
        // 属误用（消费方线程模型不允许），显式拒绝
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "FileWatcher: 不能在回调线程内重启监听");
    }
    stop();  // 幂等重启：先收掉旧线程

    if (dir.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "FileWatcher: 监听路径为空");
    }
    if (!onChange) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "FileWatcher: 回调为空");
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return core::Result<void>::fail(core::ErrorCode::FileNotFound,
                                        "FileWatcher: 目录不存在: " + dir.string());
    }

    // fs::path ←UTF-8 std::string 的窄→宽转换走 ACP，中文路径会乱码；
    // 显式 UTF-8 → UTF-16（与 file_open_win.cpp 同一编码链）
    const std::u8string u8 = dir.u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(u8.data()), u8.size());
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(),
                                         static_cast<int>(utf8Path.size()),
                                         nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(),
                        static_cast<int>(utf8Path.size()), wpath.data(), wlen);

    HANDLE h = CreateFileW(wpath.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return core::Result<void>::fail(core::ErrorCode::FileNotFound,
                                        "FileWatcher: 无法打开监听目录（" +
                                            lastWinError(GetLastError()) + "）");
    }
    HANDLE stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEv) {
        const DWORD err = GetLastError();
        CloseHandle(h);
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "FileWatcher: 创建停止事件失败（" +
                                            lastWinError(err) + "）");
    }

    impl_->dirHandle = h;
    impl_->stopEvent = stopEv;
    impl_->callback = std::move(onChange);
    impl_->debounceMs = static_cast<DWORD>(
        std::chrono::milliseconds(debounceMs).count() > 0
            ? static_cast<long long>(debounceMs.count())
            : 0);
    if (impl_->debounceMs > 60000) impl_->debounceMs = 60000;  // 防呆上限

    impl_->running.store(true);
    try {
        impl_->worker = std::thread([this] { impl_->runWorker(); });
    } catch (const std::system_error& e) {
        impl_->running.store(false);
        CloseHandle(h);
        CloseHandle(stopEv);
        impl_->dirHandle = nullptr;
        impl_->stopEvent = nullptr;
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        std::string("FileWatcher: 无法启动监听线程: ") +
                                            e.what());
    }
    LOG_INFO("FileWatcher: 已启动监听 " + dir.string());
    return core::Result<void>::ok();
}

void FileWatcher::stop() {
    impl_->joinWorker();
}

bool FileWatcher::active() const {
    return impl_->running.load();
}

} // namespace platform
