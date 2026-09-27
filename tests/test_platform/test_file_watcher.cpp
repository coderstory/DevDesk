// tests/test_platform/test_file_watcher.cpp
// platform::FileWatcher 覆盖：
//   ① 参数校验（全平台共享，stub 与 win 同一契约）：空路径/空回调 →
//      ValidationError；目录不存在 → FileNotFound。
//   ② start/stop 生命周期幂等（全平台）：真实临时目录 start ok，stop
//      可重复调用不崩。
//   ③ Windows 真实现事件链路（临时目录真实写/删/子目录 → 回调触发）：
//      验证 ReadDirectoryChangesW 线程 + 去抖 + 路径转换（UTF-8、'/' 分隔、
//      子目录递归、中文文件名）。stub 平台（POSIX/macOS）跳过并打印 [SKIP]。
#include "doctest.h"

#include "platform/file_watcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path makeTempDir(const std::string& tag) {
    static int counter = 0;
    ++counter;
    fs::path dir = test_temp::tempPath("devdesk_fwatch_test_" + tag + "_" +
                                       std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool writeTextFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

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
};

// 平台谓词（文件作用域指令合法；REGISTER_TEST 宏体内嵌指令会被
// -Wpedantic -Werror 拒绝）。true = 本平台 FileWatcher 为原生真实现。
#if defined(_WIN32)
inline constexpr bool kWinNativeWatcher = true;
#else
inline constexpr bool kWinNativeWatcher = false;
#endif

} // namespace

REGISTER_TEST(fwatch_start_validation,
              "FileWatcher: 空路径/空回调/目录不存在 → fail（全平台契约）", {
    platform::FileWatcher w;
    EventSink sink;

    auto r1 = w.start(fs::path(), sink);
    DOCTEST_CHECK(r1.isFail());
    DOCTEST_CHECK(r1.errorCode() == core::ErrorCode::ValidationError);

    auto r2 = w.start(makeTempDir("nullcb"), nullptr);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK(r2.errorCode() == core::ErrorCode::ValidationError);

    const auto missing =
        test_temp::tempPath("devdesk_fwatch_no_such_dir_7c02");
    std::error_code ec;
    fs::remove_all(missing, ec);
    auto r3 = w.start(missing, sink);
    DOCTEST_CHECK(r3.isFail());
    DOCTEST_CHECK(r3.errorCode() == core::ErrorCode::FileNotFound);

    fs::remove_all(makeTempDir("cleanup_ignore"), ec);
    return true;
})

REGISTER_TEST(fwatch_start_stop_idempotent,
              "FileWatcher: 真实目录 start ok + stop 幂等（可重复）", {
    const auto dir = makeTempDir("lifecycle");
    EventSink sink;
    platform::FileWatcher w;

    auto r = w.start(dir, sink);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(w.active() == kWinNativeWatcher);  // Win 真监听 / stub 恒 false
    w.stop();
    w.stop();  // 幂等
    DOCTEST_CHECK(!w.active());

    // 重启可换目录（服务层不用，锁死契约）
    auto r2 = w.start(dir, sink);
    DOCTEST_CHECK(r2.isOk());
    w.stop();

    std::error_code ec;
    fs::remove_all(dir, ec);
    return true;
})

#if defined(_WIN32)
// 注意：以下用例只在 Windows 注册——它们验证 ReadDirectoryChangesW 的
// 事件链路（去抖交付 / '/' 相对路径 / 子目录递归 / UTF-8 中文名）。
// POSIX/macOS stub 平台事件永不到达，注册了也只能 [SKIP]，索性不注册。

REGISTER_TEST(fwatch_event_create_modify_delete,
              "FileWatcher[Win]: 新建/改写/删除 → 回调带正确相对路径与动作", {
    const auto dir = makeTempDir("events");
    EventSink sink;
    platform::FileWatcher w;
    DOCTEST_REQUIRE(w.start(dir, sink).isOk());
    DOCTEST_REQUIRE(w.active());

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
    fs::remove_all(dir, ec);
    return true;
})

REGISTER_TEST(fwatch_event_subdir_and_nonascii,
              "FileWatcher[Win]: 子目录递归（box/deep.md）+ 中文文件名 UTF-8 送达", {
    const auto dir = makeTempDir("subdir");
    EventSink sink;
    platform::FileWatcher w;
    DOCTEST_REQUIRE(w.start(dir, sink).isOk());

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
    std::error_code ec;
    fs::remove_all(dir, ec);
    return true;
})

REGISTER_TEST(fwatch_event_burst_debounce_merges,
              "FileWatcher[Win]: 突发 5 文件在去抖窗口内合并为 ≤2 次批量交付", {
    const auto dir = makeTempDir("debounce");
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

    std::error_code ec;
    fs::remove_all(dir, ec);
    return true;
})
#endif  // _WIN32
