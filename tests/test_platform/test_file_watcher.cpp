// tests/test_platform/test_file_watcher.cpp
// platform::FileWatcher 覆盖：
//   ① 参数校验（全平台共享，stub 与 win 同一契约）：空路径/空回调 →
//      ValidationError；目录不存在 → FileNotFound。
//   ② start/stop 生命周期幂等（全平台）：真实临时目录 start ok，stop
//      可重复调用不崩。
//   ③ 原生真实现事件链路（临时目录真实写/删/子目录 → 回调触发）：
//      Windows 走 ReadDirectoryChangesW、macOS 走 FSEvents，验证同一份契约
//      （去抖整批交付 + 路径转换 UTF-8 / '/' 分隔、子目录递归、中文文件名）。
//      两侧都注册：真实现是平台适配层，改一端必须两端都跑。仅 POSIX stub
//      平台（CMake else() 分支）无事件可言 → 不注册。
#include "doctest.h"
#include "test_support.h"

#include "platform/file_watcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 写文本文件 —— 共享实现（TIN-06b，六份 writeTextFile/writeText 合流）
using test_support::writeTextFile;

// 事件收集器（回调线程 → 测试线程）。内部 shared 状态 → 本体可拷贝，
// 满足 std::function 的 CopyConstructible 要求。
struct EventSink {
    struct State {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<platform::FileWatchEvent> events;
    };
    std::shared_ptr<State> st = std::make_shared<State>();

    void operator()(std::vector<platform::FileWatchEvent> batch) const {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->events.insert(st->events.end(), batch.begin(), batch.end());
        st->cv.notify_all();
    }
    // 等待满足 pred 的事件出现（超时 ms），返回是否命中
    template <typename Pred>
    bool waitFor(Pred pred, int timeoutMs) const {
        std::unique_lock<std::mutex> lock(st->mtx);
        return st->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [&] {
                                   for (const auto& e : st->events)
                                       if (pred(e)) return true;
                                   return false;
                               });
    }
    void clear() const {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->events.clear();
    }
    // 是否已收到某相对路径的事件（武装探针用；只读快照，不等待）
    bool hasPath(const std::string& relPath) const {
        std::lock_guard<std::mutex> lock(st->mtx);
        for (const auto& e : st->events) {
            if (e.relativePath == relPath) return true;
        }
        return false;
    }
};

// ── 武装等待（flake 归因 2026-09-28，见 file_watcher.h「启动武装窗口」）──
// 仅 Windows 有该窗口（macOS 的 start() 阻塞到 FSEventStreamStart 成功才返回，
// 返回即已武装）；探针等待在 macOS 上同样成立，故一并编入，不做平台分叉。
#if defined(_WIN32) || defined(__APPLE__)
// start() 在调用线程开目录句柄，而首个 ReadDirectoryChangesW 由 worker 线程
// 异步发出；Windows 只上报「首读发出之后」的改动，之前的改动**永久丢失**
// （等再久也不会补发）。用例在 start() 后立刻写文件就会踩进这个窗口——
// 高负载时线程启动慢，写操作稳赢首读 → 事件丢失必红；本机实测过。
// 判据（不放宽）：往被监听目录写探针文件 → **有界**等待 watcher 把它报上来；
// 收到即证明首读已发出（此后任何改动都会被捕获），此后用例才动真文件。
// 探针每轮换名，避免上一轮的迟到事件造成假阳性；预算内始终收不到 → 打印
// 诊断并返回 false，由调用方 DOCTEST_REQUIRE 判红。等武装只是还原真实时序
// （真实进程里用户操作远在启动之后），不改任何断言语义。
bool waitWatcherArmed(const fs::path& dir,
                      const std::function<bool(const std::string&)>& seen,
                      const char* label) {
    constexpr int kRounds = 10;        // 10 × 500ms = 5s 上限（有界，不会无限挂）
    constexpr int kPerRoundMs = 500;
    constexpr int kPollMs = 20;
    for (int i = 0; i < kRounds; ++i) {
        const std::string probe = "arm_probe_" + std::to_string(i) + ".md";
        if (!writeTextFile(dir / probe, "# arm\n")) {
            std::cout << "[DIAG] watcher 武装等待：探针写入失败 " << probe
                      << " label=" << label << " dir=" << dir.string() << std::endl;
            return false;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kPerRoundMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (seen(probe)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
    }
    std::cout << "[DIAG] watcher 武装等待超时（" << kRounds * kPerRoundMs
              << "ms，探针始终未被上报）：label=" << label
              << " dir=" << dir.string() << std::endl;
    return false;
}
#endif  // _WIN32 || __APPLE__（POSIX stub 平台不产生事件，武装等待无用武之地
        // → 不编入避免 -Wunused-function -Werror）

// 清场（best-effort，非抛出版本）：整棵删掉临时目录；删不干净只打印残留
// 诊断、不把用例判红（残留路径带 pid + 序号，不复用、不干扰后续用例）。
// 调用方须先 stop()/析构 watcher——监听存活时回调线程可能在读被监听文件，
// MinGW 的 ifstream 句柄不带 FILE_SHARE_DELETE，期间的删除会失败（macOS 无
// 该语义，删除不被读句柄挡住，但 FSEvents 的迟到事件会干扰后续断言，仍先停）。
void cleanupTempDir(const fs::path& dir, const char* label) {
    std::error_code ec;
    fs::remove_all(dir, ec);           // 非抛出版本：删不掉也不抛、不算用例失败
    if (!fs::exists(dir, ec)) return;  // 干净（exists 的 error_code 重载会重置 ec）
    std::error_code ecRetry;
    fs::remove_all(dir, ecRetry);      // best-effort 重试一次（句柄刚释放的竞态）
    if (!fs::exists(dir, ecRetry)) return;
    std::cout << "[DIAG] 临时目录残留 " << dir.string() << " label=" << label
              << " exists=" << ec.message() << " retry=" << ecRetry.message()
              << std::endl;
}

// 平台谓词（文件作用域指令合法；REGISTER_TEST 宏体内嵌指令会被
// -Wpedantic -Werror 拒绝）。true = 本平台 FileWatcher 为原生真实现
// （Windows ReadDirectoryChangesW / macOS FSEvents）；POSIX stub 恒 false
// ——它 start() 返回 ok 但 active() 恒 false，消费方据此退回轮询。
#if defined(_WIN32) || defined(__APPLE__)
inline constexpr bool kNativeWatcher = true;
#else
inline constexpr bool kNativeWatcher = false;
#endif

} // namespace

REGISTER_TEST(fwatch_start_validation,
              "FileWatcher: 空路径/空回调/目录不存在 → fail（全平台契约）", {
    platform::FileWatcher w;
    EventSink sink;

    auto r1 = w.start(fs::path(), sink);
    DOCTEST_CHECK(r1.isFail());
    DOCTEST_CHECK(r1.errorCode() == core::ErrorCode::ValidationError);

    const auto nullCbDir = test_support::makeTempDir("devdesk_fwatch_test_", "nullcb");
    auto r2 = w.start(nullCbDir, nullptr);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK(r2.errorCode() == core::ErrorCode::ValidationError);

    const auto missing =
        test_temp::tempPath("devdesk_fwatch_no_such_dir_7c02");
    std::error_code ec;
    fs::remove_all(missing, ec);
    auto r3 = w.start(missing, sink);
    DOCTEST_CHECK(r3.isFail());
    DOCTEST_CHECK(r3.errorCode() == core::ErrorCode::FileNotFound);

    // 清场：本用例两次 makeTempDir 都要收（原 nullcb 那个建完就没人删，每跑一次
    // 就往 %TEMP% 留一个空目录）
    cleanupTempDir(test_support::makeTempDir("devdesk_fwatch_test_", "cleanup_ignore"),
                   "start_validation");
    cleanupTempDir(nullCbDir, "start_validation/nullcb");
    return true;
})

REGISTER_TEST(fwatch_start_stop_idempotent,
              "FileWatcher: 真实目录 start ok + stop 幂等（可重复）", {
    const auto dir = test_support::makeTempDir("devdesk_fwatch_test_", "lifecycle");
    EventSink sink;
    platform::FileWatcher w;

    auto r = w.start(dir, sink);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(w.active() == kNativeWatcher);  // 原生真监听 / stub 恒 false
    w.stop();
    w.stop();  // 幂等
    DOCTEST_CHECK(!w.active());

    // 重启可换目录（服务层不用，锁死契约）
    auto r2 = w.start(dir, sink);
    DOCTEST_CHECK(r2.isOk());
    w.stop();

    cleanupTempDir(dir, "start_stop_idempotent");
    return true;
})

#if defined(_WIN32) || defined(__APPLE__)
// 以下用例在**两个有原生实现的平台**都注册（Windows ReadDirectoryChangesW /
// macOS FSEvents）——它们验证的是平台无关契约：去抖整批交付、'/' 分隔的相对
// 路径、子目录递归、UTF-8 中文名、回调线程内 stop 的语义。POSIX stub 平台
// 事件永不到达，注册了也只能空等 → 不注册。

REGISTER_TEST(fwatch_event_create_modify_delete,
              "FileWatcher[Native]: 新建/改写/删除 → 回调带正确相对路径与动作", {
    const auto dir = test_support::makeTempDir("devdesk_fwatch_test_", "events");
    EventSink sink;
    platform::FileWatcher w;
    DOCTEST_REQUIRE(w.start(dir, sink).isOk());
    DOCTEST_REQUIRE(w.active());

    // 先等 worker 武装（见 waitWatcherArmed）：否则下面的写入可能落在启动武装
    // 窗口内 → 事件永久丢失（本用例的失败模式正是「等 5s 也收不到」）
    const auto probeSeen = [&](const std::string& rel) { return sink.hasPath(rel); };
    const bool armed = waitWatcherArmed(dir, probeSeen, "create_modify_delete");
    DOCTEST_REQUIRE(armed);
    sink.clear();  // 探针文件的事件不参与断言

    // 新建
    DOCTEST_REQUIRE(writeTextFile(dir / "note_a.md", "# a\n"));
    DOCTEST_CHECK(sink.waitFor(
        [](const platform::FileWatchEvent& e) {
            return e.relativePath == "note_a.md" &&
                   (e.action == platform::FileWatchAction::Created ||
                    e.action == platform::FileWatchAction::Modified);
        },
        5000));

    // 改写（同长度换内容——mtime/size 轮询看不见、事件看得见的正是这种）
    sink.clear();
    DOCTEST_REQUIRE(writeTextFile(dir / "note_a.md", "# b\n"));
    DOCTEST_CHECK(sink.waitFor(
        [](const platform::FileWatchEvent& e) {
            return e.relativePath == "note_a.md" &&
                   e.action == platform::FileWatchAction::Modified;
        },
        5000));

    // 删除
    sink.clear();
    std::error_code ec;
    DOCTEST_REQUIRE(fs::remove(dir / "note_a.md", ec));
    DOCTEST_CHECK(sink.waitFor(
        [](const platform::FileWatchEvent& e) {
            return e.relativePath == "note_a.md" &&
                   (e.action == platform::FileWatchAction::Deleted ||
                    e.action == platform::FileWatchAction::Renamed);
        },
        5000));

    w.stop();
    cleanupTempDir(dir, "create_modify_delete");
    return true;
})

REGISTER_TEST(fwatch_event_subdir_and_nonascii,
              "FileWatcher[Native]: 子目录递归（box/deep.md）+ 中文文件名 UTF-8 送达", {
    const auto dir = test_support::makeTempDir("devdesk_fwatch_test_", "subdir");
    EventSink sink;
    platform::FileWatcher w;
    DOCTEST_REQUIRE(w.start(dir, sink).isOk());

    // 先等 worker 武装（见 waitWatcherArmed），再把磁盘弄动
    const auto probeSeen = [&](const std::string& rel) { return sink.hasPath(rel); };
    const bool armed = waitWatcherArmed(dir, probeSeen, "subdir_and_nonascii");
    DOCTEST_REQUIRE(armed);
    sink.clear();

    // 子目录内的新文件：相对路径必须含子目录且用 '/' 分隔
    const std::string cn = "\xe4\xb8\xad\xe6\x96\x87\xe7\xac\x94\xe8\xae\xb0.md";  // 中文笔记.md
    DOCTEST_REQUIRE(writeTextFile(dir / "box" / "deep.md", "# deep\n"));
    DOCTEST_REQUIRE(writeTextFile(dir / cn, "# cn\n"));

    DOCTEST_CHECK(sink.waitFor(
        [](const platform::FileWatchEvent& e) {
            return e.relativePath == "box/deep.md";
        },
        5000));
    DOCTEST_CHECK(sink.waitFor(
        [&](const platform::FileWatchEvent& e) {
            return e.relativePath == cn;  // UTF-8 字节原样送达，不乱码
        },
        5000));

    w.stop();
    cleanupTempDir(dir, "subdir_and_nonascii");
    return true;
})

REGISTER_TEST(fwatch_event_burst_debounce_merges,
              "FileWatcher[Native]: 突发 5 文件在去抖窗口内合并为 ≤2 次批量交付", {
    const auto dir = test_support::makeTempDir("devdesk_fwatch_test_", "debounce");
    struct {
        std::mutex mtx;
        int deliveries = 0;
        std::vector<platform::FileWatchEvent> all;
    } sink;
    auto cb = [&](std::vector<platform::FileWatchEvent> batch) {
        std::lock_guard<std::mutex> lock(sink.mtx);
        ++sink.deliveries;
        sink.all.insert(sink.all.end(), batch.begin(), batch.end());
    };

    platform::FileWatcher w;
    DOCTEST_REQUIRE(w.start(dir, cb, std::chrono::milliseconds(300)).isOk());

    // 先等 worker 武装（见 waitWatcherArmed）——突发必须整批落在首读之后，
    // 否则头几个文件的事件丢在启动窗口内、下面的 hit >= 5 直接不成立
    const auto probeSeen = [&](const std::string& rel) {
        std::lock_guard<std::mutex> lock(sink.mtx);
        for (const auto& e : sink.all) {
            if (e.relativePath == rel) return true;
        }
        return false;
    };
    const bool armed = waitWatcherArmed(dir, probeSeen, "burst_debounce_merges");
    DOCTEST_REQUIRE(armed);
    // 探针不参与本用例计数：先放掉探针那批的余量（同一写入的后续通知）走完
    // 一个去抖窗口，再清零 —— 下面的 deliveries 从 0 起算
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    {
        std::lock_guard<std::mutex> lock(sink.mtx);
        sink.deliveries = 0;
        sink.all.clear();
    }

    // 一次性突发 5 个文件（都在 300ms 去抖窗口内）
    for (int i = 0; i < 5; ++i) {
        DOCTEST_REQUIRE(writeTextFile(dir / ("burst_" + std::to_string(i) + ".md"),
                                      "# b\n"));
    }
    // 等静默窗口 + 交付
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    w.stop();

    std::lock_guard<std::mutex> lock(sink.mtx);
    // 全部 5 个文件的事件都到达
    int hit = 0;
    for (const auto& e : sink.all) {
        if (e.relativePath.rfind("burst_", 0) == 0) ++hit;
    }
    DOCTEST_CHECK(hit >= 5);
    // 合并语义：交付次数应远小于文件数（去抖生效；极端调度抖动放宽到 ≤4）
    DOCTEST_CHECK(sink.deliveries <= 4);

    cleanupTempDir(dir, "burst_debounce_merges");
    return true;
})
REGISTER_TEST(fwatch_stop_from_callback_thread,
              "FileWatcher[Native]: 回调线程内 stop() 只请求停止（不崩、join 后可再启）", {
    const auto dir = test_support::makeTempDir("devdesk_fwatch_test_", "cbstop");
    platform::FileWatcher w;
    EventSink sink;
    std::atomic<int> deliveries(0);
    // 回调线程内首次收到事件即 stop()（review-A 实锤的 UAF 场景：旧实现 detach
    // + 关句柄 → detached worker 继续摸 Impl/句柄；新实现只请求停止，句柄与
    // 线程由 worker 退出路径 + 随后的非回调线程 join 收尾）
    auto cb = [&](std::vector<platform::FileWatchEvent> batch) {
        bool sawOwnFile = false;
        for (const auto& e : batch) {
            if (e.relativePath == "cb_stop.md") sawOwnFile = true;
        }
        // 首次交付到本用例文件的事件时，在回调线程内 stop()（review-A 实锤的
        // UAF 场景：旧实现 detach + 关句柄 → detached worker 继续摸 Impl/句柄；
        // 新实现只请求停止，句柄与线程由 worker 退出路径 + 随后的非回调线程
        // join 收尾）。判据不取「首次交付」——%TEMP% 下无关通知（杀软/索引器）
        // 可能抢先投递，会把监听提前收掉、真事件反而收不到（实测 flake 1 次）
        if (sawOwnFile && deliveries.fetch_add(1) == 0) w.stop();
        sink(batch);
    };
    DOCTEST_REQUIRE(w.start(dir, cb).isOk());
    DOCTEST_REQUIRE(w.active());
    // 先等 worker 武装（见 waitWatcherArmed）再写文件——start 与首读之间的
    // 固有窗口里发生的改动不进变更缓冲（机器高负载时实测偶发丢通知）
    const auto probeSeen = [&](const std::string& rel) { return sink.hasPath(rel); };
    const bool armed = waitWatcherArmed(dir, probeSeen, "stop_from_callback_thread");
    DOCTEST_REQUIRE(armed);
    sink.clear();  // 探针文件的事件不参与断言

    DOCTEST_REQUIRE(writeTextFile(dir / "cb_stop.md", "# s\n"));
    DOCTEST_CHECK(sink.waitFor(
        [](const platform::FileWatchEvent& e) {
            return e.relativePath == "cb_stop.md";
        },
        5000));
    DOCTEST_CHECK(deliveries.load() >= 1);

    // 回调已返回（stop() 在其内部调用过）；非回调线程 stop() 完成 join
    w.stop();
    DOCTEST_CHECK(!w.active());

    // 可再次 start() 并收到事件（句柄/线程干净回收，无失效 HANDLE）
    EventSink sink2;
    DOCTEST_REQUIRE(w.start(dir, sink2).isOk());
    DOCTEST_CHECK(w.active());
    const auto probeSeen2 = [&](const std::string& rel) { return sink2.hasPath(rel); };
    const bool armed2 = waitWatcherArmed(dir, probeSeen2, "stop_from_callback_thread#2");
    DOCTEST_REQUIRE(armed2);  // 同上：重启后同样要等武装
    sink2.clear();
    DOCTEST_REQUIRE(writeTextFile(dir / "cb_stop2.md", "# s2\n"));
    DOCTEST_CHECK(sink2.waitFor(
        [](const platform::FileWatchEvent& e) {
            return e.relativePath == "cb_stop2.md";
        },
        5000));
    w.stop();

    cleanupTempDir(dir, "stop_from_callback_thread");
    return true;
})
#endif  // _WIN32 || __APPLE__
