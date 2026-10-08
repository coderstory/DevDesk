// src/platform/file_watcher_macos.cpp
// macOS 实现：FSEvents（FSEventStreamCreate + 专属后台线程的 CFRunLoop）。
//
// 语义与错误码对齐 file_watcher_win.cpp（同一份头文件的契约）：
//   校验失败 → ValidationError / FileNotFound（逐条同文同码）
//   原生监听建不起来 → Internal
//   事件 → FileWatchAction 映射、按 debounce 静默窗口整批交付、回调在 watcher
//   后台线程执行、不抛异常。
//
// 线程模型（与 win 版同构）：
//   start()  参数校验 → 起 worker 线程 → **阻塞等 worker 完成武装**（见下），
//            失败即带原因返回（并收掉线程，无残留）
//   worker   建 FSEventStream → ScheduleWithRunLoop(自己的 CFRunLoop) →
//            FSEventStreamStart → 跑 run loop：回调累积 pending，静默
//            debounceMs 后整批交付；看到 running==false → 退出 run loop →
//            Stop/Invalidate/Release 流（流由本线程建、本线程收）
//   stop()   只置 running=false（原子）+ join；worker 自行收流退出。
//            回调线程内 stop 只请求停止（不 join、不碰流），join 由之后非回调
//            线程的 stop()/析构完成——与 win 版同一契约，消费方必须保证对象
//            活到回调返回（头文件「线程约定」）。
//
// 停止唤醒用**有界 tick** 而非跨线程 CFRunLoopStop：run loop 引用（CFRunLoopRef）
// 不是公开可 retain/release 的对象，线程退出即随线程销毁——跨线程拿它调
// CFRunLoopStop 存在「worker 已退、stop() 仍摸旧引用」的悬垂窗口。本实现让
// run loop 只被 worker 线程自己访问：每轮以 ≤kIdleTickSeconds 的有限超时睡眠
// （事件到达即被唤醒，空闲时每秒数次空转、成本可忽略），醒来看 running。
// 代价是 stop() 的线程退出时延上界 = 一个 tick，换来的是零跨线程悬垂面。
//
// 与 win 版的**唯一语义差异是改进项**：win 的 start() 只开目录句柄，首个
// ReadDirectoryChangesW 由 worker 异步发出，两端之间存在「改动不被上报」的
// 启动武装窗口（见头文件「已知边界」）。本实现 start() 阻塞到
// FSEventStreamStart() 成功才返回——start() 返回即可认为监听已生效，无该窗口。
//
// 事件流位掩码：
//   kFSEventStreamCreateFlagFileEvents —— 逐文件事件（而非目录级聚合），与 win
//       版逐条 FILE_NOTIFY_INFORMATION 对齐
//   kFSEventStreamCreateFlagNoDefer    —— 空闲后首个事件立即投递，不被流自身的
//       延迟窗口吃掉（「非延迟」语义与 win 的「事件到达即解析」对齐）
//   kFSEventStreamCreateFlagWatchRoot  —— 监听根被改名/删除时收到 RootChanged
//   kFSEventStreamCreateFlagUseCFTypes —— 回调收到 CFArray<CFString>（UTF-8 编码链
//       与消费方的 u8string 转换直连，不再过一遍 C 字符串）
// 流延迟取 0：批量交付完全由 worker 自己的 debounce 窗口决定（对应 win 的
// WaitForMultipleObjects(timeout = debounceMs)），流再叠一层延迟只会让交付时刻
// 不可预测。
//
// 事件 → 动作映射（对齐 win 的 FILE_ACTION_*）。APFS/macOS 27 实测的标志组合
// （一次性探针，见各行的注解）决定了判定顺序与判据，不能照文档字面直译：
//   建文件   ItemCreated ItemModified ItemXattrMod ItemIsFile
//   改内容   ItemCreated ItemInodeMetaMod ItemModified ItemXattrMod ItemIsFile
//   改名     (旧名) ItemCreated ItemInodeMetaMod ItemRenamed ItemModified …ItemIsFile
//            (新名) ItemRenamed ItemIsFile
//   删除     ItemRemoved ItemRenamed ItemIsFile          ← 注意：**同时带 Renamed**
//   —— ItemCreated 在「改动既有文件」时也会被置上（FSEvents 的已知行为：它表示
//      「该路径在事件流里出现」而非严格意义上被创建），故 Created 只在**没有**
//     ItemInodeMetaMod 时判定（真正的创建不带 InodeMetaMod；内容/size/mtime 变化
//     一定带）；删除恒带 ItemRemoved，故 Removed 判在 Renamed 之前。
//   1) ItemRemoved            → Deleted
//   2) ItemRenamed            → Renamed（旧名/新名各一条，消费方按磁盘存在性判定
//                               是新名还是旧名 —— 同 win 的 RENAMED_OLD/NEW 语义）
//   3) ItemCreated 且无
//      ItemInodeMetaMod       → Created
//   4) ItemModified / ItemInodeMetaMod / ItemFinderInfoMod / ItemChangeOwner /
//      ItemXattrMod           → Modified
//   5) 其余（无逐项标志 = 只知道该目录下有变化）→ Unknown（带路径）
//   另：路径不在监听根下或就是根自身 → relativePath 为空 → 一律 Unknown
//   （空路径 = 消费方全量重扫，见 file_watcher.h 的 Unknown 契约）；
//   MustScanSubDirs / UserDropped / KernelDropped / RootChanged / Mount / Unmount
//   同样交 Unknown（空路径）。
//
// 批内去重（macOS 侧合批细节，不影响 win 语义）：同一批内完全相同的
// (action, 相对路径) 只保留一条。FSEvents 的逐文件投递在突发（编辑器连续写、
// git checkout）下会给出大量重复的 Modified，而消费方对每条事件都要 force
// 同步一次索引（读文件 + 写库），去掉完全重复项不改变「哪些文件变了」的信息。
// 批内条目仍有上限：超过 kMaxPendingEvents 即折叠成单条 Unknown（消费方全量
// 重扫）——与 win 版「64KB 通知缓冲溢出 → Unknown」同语义，同时给内存封顶。
#include "platform/file_watcher.h"

#include "core/logging.h"

#include <CoreServices/CoreServices.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace platform {

namespace {

constexpr FSEventStreamCreateFlags kStreamFlags =
    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
    kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagUseCFTypes;

constexpr CFTimeInterval kStreamLatency = 0.0;

// 无法逐项枚举的事件（内核/用户态丢事件、需重扫子树）→ 交 Unknown，消费方全量重扫
constexpr FSEventStreamEventFlags kRescanFlags =
    kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
    kFSEventStreamEventFlagKernelDropped;

// 监听根本身出问题（被改名/删除/卷挂卸）→ 同样交 Unknown
constexpr FSEventStreamEventFlags kRootFlags =
    kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount |
    kFSEventStreamEventFlagUnmount;

// 单批 pending 上限（溢出折叠为 Unknown，见文件头）
constexpr size_t kMaxPendingEvents = 4096;

// run loop 单次睡眠上限（= stop() 退出时延上界）。空闲时事件到达即唤醒、
// 成本可忽略；有 pending 时取「距静默窗口到点的剩余时间」与它的小者，
// 故交付时刻精度不受影响。
constexpr CFTimeInterval kIdleTickSeconds = 0.25;

// 去抖静默窗口上限（与 win 版同一防呆上限）
constexpr long long kMaxDebounceMs = 60000;

// start() 等 worker 武装的上限：FSEventStreamStart 是同步调用，正常在毫秒级
// 返回；超时只作防御（避免 worker 异常路径漏发信号时 start() 永久挂住）。
constexpr int kStartHandshakeMs = 5000;

// 判定顺序与判据的依据见文件头「事件 → 动作映射」段（实测标志组合）
FileWatchAction mapFlags(FSEventStreamEventFlags f) {
    if ((f & kFSEventStreamEventFlagItemRemoved) != 0) return FileWatchAction::Deleted;
    if ((f & kFSEventStreamEventFlagItemRenamed) != 0) return FileWatchAction::Renamed;
    if ((f & kFSEventStreamEventFlagItemCreated) != 0 &&
        (f & kFSEventStreamEventFlagItemInodeMetaMod) == 0) {
        return FileWatchAction::Created;  // 真创建：Created 不带 InodeMetaMod
    }
    if ((f & (kFSEventStreamEventFlagItemModified |
              kFSEventStreamEventFlagItemInodeMetaMod |
              kFSEventStreamEventFlagItemFinderInfoMod |
              kFSEventStreamEventFlagItemChangeOwner |
              kFSEventStreamEventFlagItemXattrMod)) != 0) {
        return FileWatchAction::Modified;
    }
    return FileWatchAction::Unknown;  // 无逐项标志：仅知该目录下有变化（带路径）
}

// CFString（文件系统路径）→ UTF-8 std::string
std::string cfStringToUtf8(CFStringRef s) {
    if (!s) return {};
    const CFIndex len = CFStringGetLength(s);
    if (len <= 0) return {};
    const CFIndex maxBytes =
        CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(maxBytes), '\0');
    CFIndex used = 0;
    const CFIndex n =
        CFStringGetBytes(s, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false,
                         reinterpret_cast<UInt8*>(out.data()), maxBytes, &used);
    if (n <= 0 || used <= 0) return {};
    out.resize(static_cast<size_t>(used));
    return out;
}

// 事件绝对路径 → 监听根下的相对路径（POSIX 分隔、UTF-8）。
// roots 同时给「原样路径」与「规范化路径」两种前缀：macOS 的 /tmp、
// /var/folders 等是符号链接（真实路径 /private/...），FSEvents 回的路径带哪一侧
// 前缀取决于内核视角，两种都匹配才不至于把整个笔记树都判成「无法定位」。
// 不在任何根下 → 空串（消费方按「空串 = 无法定位」走全量重扫兜底）。
std::string relativeUnderRoot(const std::string& absPath,
                              const std::vector<std::string>& roots) {
    for (const auto& root : roots) {
        if (root.empty()) continue;
        if (absPath == root) return {};  // 根自身（RootChanged 等）
        if (absPath.size() > root.size() &&
            absPath.compare(0, root.size(), root) == 0 &&
            absPath[root.size()] == '/') {
            return absPath.substr(root.size() + 1);
        }
    }
    return {};
}

// 去掉尾部 '/'（保留单独的 "/"）
std::string trimTrailingSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

} // namespace

struct FileWatcher::Impl {
    std::atomic<bool> running{false};
    std::thread worker;
    FileWatchCallback callback;
    long long debounceMs = 200;

    // start() ←→ worker 的武装握手：worker 完成 FSEventStreamStart 后放行 start()
    std::mutex startMu;
    std::condition_variable startCv;
    bool startSignaled = false;
    bool startOk = false;
    core::ErrorCode startCode = core::ErrorCode::Internal;
    std::string startError;

    // ---- 以下字段 worker 线程独占（run loop 回调与主循环同线程，无需加锁）----
    std::string watchRoot;            // 原样形式：建流路径 + 日志
    std::vector<std::string> roots;   // 前缀匹配用（原样 + 规范化）
    FSEventStreamRef stream = nullptr;
    bool streamStarted = false;       // FSEventStreamStop 只对已 Start 的流调用
    std::vector<FileWatchEvent> pending;
    std::set<std::string> pendingKeys;  // 批内去重键（action + 路径）
    bool overflowed = false;            // 本批已折叠为 Unknown（不再累积，见 pushEvent）
    std::chrono::steady_clock::time_point lastEventAt{};

    // ---------- worker → start() 的握手 ----------
    void signalStart(bool ok, core::ErrorCode code, std::string msg) {
        {
            std::lock_guard<std::mutex> lock(startMu);
            startSignaled = true;
            startOk = ok;
            startCode = code;
            startError = std::move(msg);
        }
        startCv.notify_all();
    }

    // ---------- 批次累积与交付（仅 worker 线程） ----------
    void pushEvent(FileWatchAction action, std::string relPath) {
        if (overflowed) return;  // 本批已折叠为 Unknown：静默丢弃余量（见下）
        if (pending.size() >= kMaxPendingEvents) {
            // 突发规模超上限：整批折叠为单条 Unknown 并**停止累积**。消费方收到
            // 空路径即做「以磁盘为准」的全量重扫，交付时刻在突发之后，故被丢弃
            // 的余量不会漏（重扫看的是最终磁盘状态）——与 win 版缓冲溢出同语义，
            // 同时给内存封顶（FSEvents 逐文件投递在 git checkout 下可达数万条）
            overflowed = true;
            pendingKeys.clear();
            pending.clear();
            pending.push_back({FileWatchAction::Unknown, std::string()});
            LOG_WARN("FileWatcher: 批内事件数超上限（" +
                     std::to_string(kMaxPendingEvents) +
                     "），交 Unknown 由消费方全量重扫: " + watchRoot);
            return;
        }
        const std::string key = std::to_string(static_cast<int>(action)) + ":" + relPath;
        if (!pendingKeys.insert(key).second) return;  // 批内完全重复 → 丢（见文件头）
        pending.push_back({action, std::move(relPath)});
    }

    // 无法逐项枚举（内核丢事件 / 需重扫子树 / 根自身变化 / 路径不在根下）：
    // 追加一条空路径 Unknown（= 消费方全量重扫）。已在批内的逐项事件保留——
    // 它们仍然有效，丢弃只会白丢信息
    void pushRescan() { pushEvent(FileWatchAction::Unknown, std::string()); }

    void deliver() {
        std::vector<FileWatchEvent> batch;
        batch.swap(pending);
        pendingKeys.clear();
        overflowed = false;
        if (callback) callback(std::move(batch));
    }

    // ---------- FSEvents 回调（worker 的 run loop 上执行） ----------
    static void streamCallback(ConstFSEventStreamRef /*streamRef*/, void* info,
                               size_t numEvents, void* eventPaths,
                               const FSEventStreamEventFlags* eventFlags,
                               const FSEventStreamEventId* /*eventIds*/) {
        auto* self = static_cast<Impl*>(info);
        if (!self || !eventPaths || !eventFlags) return;
        // UseCFTypes → CFArray<CFString>（所有权归框架，回调返回后失效）
        CFArrayRef paths = static_cast<CFArrayRef>(eventPaths);
        const CFIndex n = CFArrayGetCount(paths);
        self->lastEventAt = std::chrono::steady_clock::now();
        for (size_t i = 0; i < numEvents; ++i) {
            const FSEventStreamEventFlags f = eventFlags[i];
            if ((f & kRescanFlags) != 0 || (f & kRootFlags) != 0) {
                self->pushRescan();
                continue;
            }
            if (static_cast<CFIndex>(i) >= n) break;  // 数组短于事件数：安全放弃
            const auto* s = static_cast<CFStringRef>(
                CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
            std::string rel = relativeUnderRoot(cfStringToUtf8(s), self->roots);
            if (rel.empty()) {
                // 根自身变化 / 路径不在根下：无法定位到具体文件 → 全量重扫兜底
                // （保持「空 relativePath ⟺ Unknown」的契约）
                self->pushRescan();
                continue;
            }
            self->pushEvent(mapFlags(f), std::move(rel));
        }
    }

    // ---------- 流生命周期（仅 worker 线程；本线程调度 → 本线程撤销） ----------
    void releaseStream() {
        if (!stream) return;
        if (streamStarted) {
            FSEventStreamStop(stream);
            streamStarted = false;
        }
        FSEventStreamInvalidate(stream);  // 要求已 Schedule（本实现成立）
        FSEventStreamRelease(stream);
        stream = nullptr;
    }

    // ---------- worker 主体 ----------
    void runWorker() {
        CFStringRef cfRoot = CFStringCreateWithFileSystemRepresentation(
            nullptr, watchRoot.c_str());
        CFArrayRef paths = nullptr;
        if (cfRoot) {
            const void* one[1] = {cfRoot};
            paths = CFArrayCreate(nullptr, one, 1, &kCFTypeArrayCallBacks);
        }
        FSEventStreamContext ctx{};
        ctx.info = this;
        if (paths) {
            stream = FSEventStreamCreate(nullptr, &Impl::streamCallback, &ctx, paths,
                                         kFSEventStreamEventIdSinceNow, kStreamLatency,
                                         kStreamFlags);
        }
        if (paths) CFRelease(paths);
        if (cfRoot) CFRelease(cfRoot);
        if (!stream) {
            signalStart(false, core::ErrorCode::Internal,
                        "FileWatcher: 无法创建 FSEvents 流: " + watchRoot);
            LOG_WARN("FileWatcher: 无法创建 FSEvents 流: " + watchRoot);
            running.store(false);
            return;
        }

        {
            // FSEventStreamScheduleWithRunLoop 自 macOS 13 起弃用（推荐
            // SetDispatchQueue）；本实现按接口约定走「专属后台线程 + CFRunLoop」，
            // 该 API 仍可用，显式压掉这条弃用告警（仓库 -Werror 全档开启）。
            const CFRunLoopRef rl = CFRunLoopGetCurrent();  // 只在本线程使用/传参
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            FSEventStreamScheduleWithRunLoop(stream, rl, kCFRunLoopDefaultMode);
#pragma clang diagnostic pop
        }
        if (!FSEventStreamStart(stream)) {
            signalStart(false, core::ErrorCode::Internal,
                        "FileWatcher: FSEvents 启动失败: " + watchRoot);
            LOG_WARN("FileWatcher: FSEvents 启动失败: " + watchRoot);
            running.store(false);
            releaseStream();
            return;
        }
        streamStarted = true;
        LOG_INFO("FileWatcher: 已启动监听（FSEvents）" + watchRoot);
        signalStart(true, core::ErrorCode::Ok, std::string());

        while (running.load()) {
            CFTimeInterval wait = kIdleTickSeconds;
            if (!pending.empty()) {
                // 有货：等到静默窗口（debounceMs 内无新事件）再整批交付，与 win 的
                // WaitForMultipleObjects(timeout=debounceMs) 一一对应（分片等待，
                // 使 stop 时延上界仍是一个 tick）
                const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - lastEventAt)
                                        .count();
                const auto remain = debounceMs - static_cast<long long>(idleMs);
                if (remain <= 0) {
                    deliver();
                    continue;
                }
                const CFTimeInterval remainSec =
                    static_cast<CFTimeInterval>(remain) / 1000.0;
                if (remainSec < wait) wait = remainSec;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, wait, true);
        }

        releaseStream();
        running.store(false);  // 线程退出 = 监听失效（active() 随之变 false）
    }

    // ---------- 停止 ----------
    void joinWorker() {
        running.store(false);  // worker 在一个 tick 内看到并自行收流退出
        if (worker.joinable()) {
            // 回调线程内自 stop：只请求停止——不 detach（worker 继续在 Impl 成员
            // 函数里跑，对象析构即 UAF）、不 join、不碰流（worker 自己退出并收尾）。
            // join 由之后非回调线程的 stop()/析构完成；对象必须活到回调返回。
            if (worker.get_id() == std::this_thread::get_id()) return;
            worker.join();
        }
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
        // 回调线程内重启是误用（与 win 版同判据）：旧 worker 尚未 join 就给它赋
        // 新 std::thread = 对 joinable thread 赋值 → std::terminate
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

    // 事件路径前缀匹配的两侧：原样（native()，字节保真）与规范化（解符号链接，
    // /tmp → /private/tmp）。规范化失败时退化为只用原样路径。
    std::error_code cec;
    const std::filesystem::path canon = std::filesystem::weakly_canonical(dir, cec);
    impl_->watchRoot = trimTrailingSlash(dir.native());
    impl_->roots.clear();
    impl_->roots.push_back(impl_->watchRoot);
    if (!cec && !canon.empty()) {
        const std::string canonStr = trimTrailingSlash(canon.native());
        if (canonStr != impl_->watchRoot) impl_->roots.push_back(canonStr);
    }
    // 清批内状态（审计 F-01）：pending/pendingKeys/overflowed/lastEventAt 挂在 Impl 上
    // 而非 worker 局部（win 版是局部变量）。stop() 时未交付的旧批会被 worker 丢弃，
    // 但**留在 Impl 里** —— 若不清，下次 start() 换根后首个循环迭代因 lastEventAt
    // 陈旧而立刻 deliver()，把**旧根的事件**当成新根的交给新回调。
    impl_->pending.clear();
    impl_->pendingKeys.clear();
    impl_->overflowed = false;
    impl_->lastEventAt = {};
    impl_->callback = std::move(onChange);
    impl_->debounceMs = debounceMs.count() > 0
                            ? static_cast<long long>(debounceMs.count())
                            : 0;
    if (impl_->debounceMs > kMaxDebounceMs) impl_->debounceMs = kMaxDebounceMs;  // 防呆上限

    {
        std::lock_guard<std::mutex> lock(impl_->startMu);
        impl_->startSignaled = false;
        impl_->startOk = false;
        impl_->startCode = core::ErrorCode::Internal;
        impl_->startError.clear();
    }
    impl_->running.store(true);
    try {
        impl_->worker = std::thread([this] { impl_->runWorker(); });
    } catch (const std::system_error& e) {
        impl_->running.store(false);
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        std::string("FileWatcher: 无法启动监听线程: ") +
                                            e.what());
    }

    // 等武装：start() 返回 = FSEvents 已在跑（无 win 版启动武装窗口）
    bool ok = false;
    core::ErrorCode code = core::ErrorCode::Internal;
    std::string errMsg;
    {
        std::unique_lock<std::mutex> lock(impl_->startMu);
        impl_->startCv.wait_for(lock, std::chrono::milliseconds(kStartHandshakeMs),
                                [this] { return impl_->startSignaled; });
        ok = impl_->startSignaled && impl_->startOk;
        code = impl_->startCode;
        errMsg = impl_->startError;
        if (!impl_->startSignaled) {
            errMsg = "FileWatcher: 启动监听超时（FSEvents 未在 " +
                     std::to_string(kStartHandshakeMs / 1000) + "s 内武装）: " +
                     impl_->watchRoot;
        }
    }
    if (!ok) {
        impl_->running.store(false);
        if (impl_->worker.joinable()) impl_->worker.join();  // 收掉失败线程，无残留
        return core::Result<void>::fail(code, errMsg);
    }
    return core::Result<void>::ok();
}

void FileWatcher::stop() {
    impl_->joinWorker();
}

bool FileWatcher::active() const {
    return impl_->running.load();
}

} // namespace platform
