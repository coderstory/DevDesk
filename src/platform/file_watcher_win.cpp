// src/platform/file_watcher_win.cpp
// Windows：ReadDirectoryChangesW + OVERLAPPED + 专用后台线程。
// 线程模型：start() 在调用线程开目录句柄（失败即刻返回，无线程残留）；worker 常驻循环（未决读 →
// WaitForMultipleObjects(事件|停止) → 解析 → 去抖 → 回调）；stop() 置 running=false + SetEvent，
// worker 自行 CancelIoEx 作废未决读并退出。句柄关闭责任归 worker 退出路径（回调线程内 stop 不 join、
// 不关句柄），不存在「句柄已关、worker 仍在用」的失效 HANDLE。
// 去抖：pending 空时无限期等待（零空转）；收到事件后解析、立刻重发读请求（捕获无间隙），再按
// debounceMs 静默窗口等待，窗口内来事件则重置，到点整批交付。
// 坑：①启动武装窗口——start() 开句柄与 worker 首读之间发生的改动不上报也不补发（平台语义），需
// 零丢失者应确认已产生过事件或依赖上层轮询；②读请求「完成→重发」间有微秒级空窗，同靠轮询兜底。
#include "platform/file_watcher.h"

#include "core/logging.h"
#include "platform/file_open_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
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

// 解析一次交付的整条 FILE_NOTIFY_INFORMATION 链，追加到 out
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
    // 句柄归属：start() 写入，关闭责任在 worker 退出路径或 join 后的 closeHandles()。
    // atomic：stop() 可能与 worker 退出并发读写（回调线程内 stop 不 join），故用原子。
    std::atomic<HANDLE> dirHandle{nullptr};   // 被监听目录（OVERLAPPED 打开）
    std::atomic<HANDLE> stopEvent{nullptr};   // 手动重置，stop() SetEvent 唤醒 worker
    FileWatchCallback callback;
    DWORD debounceMs = 200;

    // 关句柄（判空幂等；exchange 保证不重复关）。仅 join 成功之后调用才安全。
    void closeHandles() {
        if (HANDLE h = dirHandle.exchange(nullptr)) CloseHandle(h);
        if (HANDLE e = stopEvent.exchange(nullptr)) CloseHandle(e);
    }

    // worker 退出路径：先把仍指向自己副本的 Impl 字段置空再关副本（判空幂等）；字段可能
    // 已被 start() 换成新句柄——只在相等时清，不动新句柄。
    void releaseOwnHandles(HANDLE dh, HANDLE se) {
        if (dirHandle.load() == dh) dirHandle.store(nullptr);
        if (stopEvent.load() == se) stopEvent.store(nullptr);
        if (dh) CloseHandle(dh);
        if (se) CloseHandle(se);
    }

    void joinWorker() {
        running.store(false);
        if (HANDLE e = stopEvent.load()) SetEvent(e);
        if (worker.joinable()) {
            // 回调线程内自 stop：只请求停止——不 detach（worker 仍在 Impl 成员里跑，析构即 UAF）、
            // 不 join、不关句柄（worker 自己会退出并收句柄）。join 由之后非回调线程的 stop()/析构完成。
            if (worker.get_id() == std::this_thread::get_id()) return;
            worker.join();
        }
        closeHandles();
    }

    // 后台线程主体（Impl 成员：匿名 ns 自由函数无权命名私有嵌套类型）
    void runWorker() {
        // 局部句柄副本：循环内只用副本，stop() 并发置空 Impl 字段也不影响本线程；副本关闭由本线程负责
        const HANDLE dh = dirHandle.load();
        const HANDLE se = stopEvent.load();
        std::vector<char> buffer(kBufBytes);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            running.store(false);
            releaseOwnHandles(dh, se);
            return;
        }

        std::vector<FileWatchEvent> pending;
        bool readIssued = false;

        while (running.load()) {
            if (!readIssued) {
                ResetEvent(ov.hEvent);
                DWORD transferredNow = 0;
                const BOOL ok = ReadDirectoryChangesW(
                    dh, buffer.data(), kBufBytes, TRUE,
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
            HANDLE waits[2] = {ov.hEvent, se};
            const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout);

            if (r == WAIT_OBJECT_0 + 1) {
                // stop()：作废未决读，等其真正结束后退出（本批 pending 丢弃）
                CancelIoEx(dh, &ov);
                DWORD dummy = 0;
                GetOverlappedResult(dh, &ov, &dummy, TRUE);
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
            if (!GetOverlappedResult(dh, &ov, &transferred, FALSE)) {
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
        releaseOwnHandles(dh, se);  // 退出前收自己的句柄副本（判空幂等）
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
        // 回调线程内重启是误用：旧 worker 未 join 就赋新 std::thread = 对 joinable thread 赋值 →
        // std::terminate，且旧线程仍持旧句柄
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

    // fs::path ← UTF-8 std::string 的窄→宽转换走 ACP、中文路径乱码；显式 UTF-8 → UTF-16
    //（与 file_open_win.cpp 共用编码链）
    const std::u8string u8 = dir.u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(u8.data()), u8.size());
    const std::wstring wpath = detail::utf8ToWide(utf8Path);

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

    impl_->dirHandle.store(h);
    impl_->stopEvent.store(stopEv);
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
        impl_->dirHandle.store(nullptr);
        impl_->stopEvent.store(nullptr);
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
