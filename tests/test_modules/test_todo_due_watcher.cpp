// 待办到期提醒（need453）测试：到期聚合投递 / 同任务同截止不重投 / 完成与归档
// 排除 / 未到期不投且扫描节流 / 超单轮上限下轮补投。
// 假时钟直驱 watcher.tick（不经真实 ticker）；渠道走真实 ToastChannel + 假 sink，
// flush 显式调用（生产由引擎 tick 末 flush）。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "module_schedule/channel_registry.h"
#include "module_schedule/todo_due_watcher.h"
#include "module_todo/todo_service.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_tododue_";

core::Database openMigrated(const std::string& tag) {
    auto path = test_support::tempDbPath(kDbPrefix, tag);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto opened = core::Database::open(path);
    if (opened.isFail()) {
        std::fprintf(stderr, "openMigrated(%s): %s\n", tag.c_str(),
                     opened.errorMessage().c_str());
        std::exit(1);
    }
    auto migrated = opened.value().migrate();
    if (migrated.isFail()) {
        std::fprintf(stderr, "openMigrated(%s) migrate: %s\n", tag.c_str(),
                     migrated.errorMessage().c_str());
        std::exit(1);
    }
    return std::move(opened.value());
}

struct Fixture {
    core::Database db;
    module::todo::TodoService todo;
    module::schedule::ChannelRegistry channels;
    module::schedule::TodoDueWatcher watcher;
    std::vector<std::pair<std::string, std::string>> toasts;  // flush 落 sink 的快照

    explicit Fixture(const std::string& tag)
        : db(openMigrated(tag)), todo(db.db()), watcher(db.db(), channels) {
        auto reg = channels.registerChannel(module::schedule::makeToastChannelForTest(
            [this](const std::string& title, const std::string& body) {
                toasts.emplace_back(title, body);
            }));
        if (reg.isFail()) std::exit(1);
    }

    int64_t columnId(const std::string& kind) {
        auto cols = todo.listColumns();
        if (cols.isFail()) std::exit(1);
        for (const auto& c : cols.value()) {
            if (c.kind == kind) return c.id;
        }
        std::fprintf(stderr, "column kind=%s not found\n", kind.c_str());
        std::exit(1);
    }

    int64_t addTask(const std::string& title, int64_t dueAt) {
        module::todo::NewTaskAttributes attrs;
        attrs.dueAt = dueAt;
        auto created = todo.createTask(columnId("todo"), title, attrs);
        if (created.isFail()) std::exit(1);
        return created.value().id;
    }

    // 一次扫描 + 落 UI：生产路径 = 引擎 tick 末 flushAllToMainThread
    void scan(int64_t now) {
        watcher.tick(now);
        channels.flushAllToMainThread();
    }
};

}  // namespace

REGISTER_TEST(todo_due_watcher_fires_once,
              "待办到期提醒: 到期任务经 toast 渠道投递一条, 正文带任务标题", {
    Fixture f("fire");
    f.addTask("写周报", 1000);
    f.scan(1000);
    DOCTEST_CHECK(f.toasts.size() == 1);
    DOCTEST_CHECK(f.toasts[0].second.find("写周报") != std::string::npos);
    return true;
})

// 防：去重账失效 → 每 60s 重投同任务（刷屏）
REGISTER_TEST(todo_due_watcher_dedupes_same_due,
              "待办到期提醒: 同任务同截止时刻只投一次, 后续扫描不重投", {
    Fixture f("dedupe");
    f.addTask("写周报", 1000);
    f.scan(1000);
    DOCTEST_CHECK(f.toasts.size() == 1);
    f.scan(1061);  // 跨过一个扫描周期
    f.scan(1122);
    DOCTEST_CHECK(f.toasts.size() == 1);
    return true;
})

// 防：完成/归档任务仍被到期扫描捞出来提醒
REGISTER_TEST(todo_due_watcher_skips_completed_and_archived,
              "待办到期提醒: 已完成任务与归档列任务不投递", {
    Fixture f("exclude");
    const int64_t doneTask = f.addTask("已完成的", 900);
    const int64_t archivedTask = f.addTask("已归档的", 900);
    if (f.todo.completeTask(doneTask).isFail()) std::exit(1);
    auto moved = f.todo.moveTask(archivedTask, f.columnId("archive"), 0);
    if (moved.isFail()) std::exit(1);
    f.scan(1000);
    DOCTEST_CHECK(f.toasts.empty());
    return true;
})

// 防：未到期就投 / 节流失效导致每秒查库（五维预算 CPU 轴）
REGISTER_TEST(todo_due_watcher_waits_due_and_throttles,
              "待办到期提醒: 未到期不投, 距上轮不足 60s 不扫描, 到期后扫描才投", {
    Fixture f("throttle");
    f.addTask("下午交", 2000);
    f.scan(1000);  // 未到期
    DOCTEST_CHECK(f.toasts.empty());
    f.scan(1030);  // 节流窗内（距上轮 30s）：不扫描
    DOCTEST_CHECK(f.toasts.empty());
    f.scan(2060);  // 距上轮 1060s ≥ 60s：扫描且已到期
    DOCTEST_CHECK(f.toasts.size() == 1);
    return true;
})

// 防：超出单轮上限的任务被记账吞掉（永不投递）
REGISTER_TEST(todo_due_watcher_caps_per_notice_and_retries_next_scan,
              "待办到期提醒: 单轮最多聚合 5 个到期任务, 第 6 个下轮补投", {
    Fixture f("cap");
    for (int i = 1; i <= 6; ++i) f.addTask("任务" + std::to_string(i), 1000 + i);
    f.scan(1100);
    DOCTEST_CHECK(f.toasts.size() == 1);
    DOCTEST_CHECK(f.toasts[0].first.find("5") != std::string::npos);
    f.scan(1161);
    DOCTEST_CHECK(f.toasts.size() == 2);  // 第 6 个补投
    return true;
})