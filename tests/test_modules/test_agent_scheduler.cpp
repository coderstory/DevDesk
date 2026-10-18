// tests/test_modules/test_agent_scheduler.cpp
// Scheduler 单测：cron 定时触发（tickNow 直调零墙钟）、同分钟防重、原子互斥
// 派发（撞闸不双跑）、跨路径单闸（手动 × cron 交叠）、常驻队列 + stopAll 收口。
// 跨文件前提：
//   - LlmEndpoint / ToolRegistry 全为注入桩，零网络。
//   - 时钟经 NowFn 注入：需要推进时间的用例传 [&nowSec]{ return nowSec; }，
//     不需要的一律传 {}（生产时钟）。
//   - 单闸在 AgentLoop::run 内：手动路径（独立线程直调 loop_.run）与 cron 路径
//     （tickNow → dispatchRun）撞的是同一把闸，被拒的一路不落 DB 行。
//   - activeRunCount 是 sched.runs_ 里的**条目数**，不是「在跑数」；被拒条目由
//     下次 tick / stopAll 的 reapDone 摘除。别拿它当跑数断言。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "module_agent/scheduler.h"
#include "core/llm_endpoint.h"
#include "core/logging.h"
#include "core/time.h"
#include "core/tool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace module::agent;

// 脚本化 mock：send 带 std::mutex——常驻队列下两跑并发 send，共享的 script
// 游标与记录容器必须自保
class MockLlm : public core::LlmEndpoint {
public:
    std::vector<core::LlmResponse> script;
    std::vector<std::vector<core::LlmMessage>> sentMessages;
    core::Result<core::LlmResponse> send(const std::vector<core::LlmMessage>& messages,
                                         const std::string&, const std::string&,
                                         const std::string&) override {
        std::lock_guard<std::mutex> lock(mu_);
        sentMessages.push_back(messages);
        if (cursor >= script.size())
            return core::Result<core::LlmResponse>::fail(core::ErrorCode::Internal,
                                                         "script 耗尽");
        return core::Result<core::LlmResponse>::ok(script[cursor++]);
    }
    size_t cursor = 0;
    std::mutex mu_;
};

core::LlmResponse endTurnResp() {
    core::LlmResponse r;
    r.stopReason = "end_turn";
    r.contentJson = R"([{"type":"text","text":"done"}])";
    r.inputTokens = 10;
    r.outputTokens = 5;
    return r;
}

// 单 tool_use block 的响应（id 固定 "tu1"：同一 run 内只有一次工具调用）
core::LlmResponse toolUseResp(const char* toolName) {
    core::LlmResponse r;
    r.stopReason = "tool_use";
    r.contentJson = std::string(R"([{"type":"tool_use","id":"tu1","name":")") +
                    toolName + R"(","input":{}}])";
    r.inputTokens = 10;
    r.outputTokens = 5;
    return r;
}

// 睡 300ms 的记账工具：把第一跑钉在闸内（撞闸用例的确定性来源——第二路必须在
// 这 300ms 窗口内起跑）。calls 用原子：跨线程读写。
class SlowToolForSched : public core::ITool {
public:
    explicit SlowToolForSched(std::string n) : n_(std::move(n)) {}
    std::string name() const override { return n_; }
    std::string schemaJson() const override {
        return R"({"name":")" + n_ + R"("})";
    }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return core::Result<core::ToolResult>::ok(core::ToolResult{false, R"("ok")"});
    }
    std::atomic<int> calls{0};

private:
    std::string n_;
};

// 立即返回的记账工具（常驻队列用例：两跑并行，不靠工具控时）
class FastToolForSched : public core::ITool {
public:
    explicit FastToolForSched(std::string n) : n_(std::move(n)) {}
    std::string name() const override { return n_; }
    std::string schemaJson() const override {
        return R"({"name":")" + n_ + R"("})";
    }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        calls.fetch_add(1);
        return core::Result<core::ToolResult>::ok(core::ToolResult{false, R"("ok")"});
    }
    std::atomic<int> calls{0};

private:
    std::string n_;
};

// 进程级 ToolRegistry 单例 + 借用栈对象。注册/注销必须成对，不能改成手写
// registerTool/unregisterTool 配对：doctest 断言失败即 `return false`（fail-fast），
// 配对的后半句被跳过 → 注册表留下悬垂裸指针，exe 崩在 exit。RAII 保证注销一定发生。
class ToolRegGuard {
public:
    ToolRegGuard(core::ToolRegistry& reg, core::ITool* tool)
        : reg_(reg), name_(tool->name()) {
        ok_ = reg_.registerTool(std::shared_ptr<core::ITool>(tool, [](core::ITool*) {}))
                  .isOk();
    }
    ~ToolRegGuard() {
        if (ok_) reg_.unregisterTool(name_);
    }
    ToolRegGuard(const ToolRegGuard&) = delete;
    ToolRegGuard& operator=(const ToolRegGuard&) = delete;

private:
    core::ToolRegistry& reg_;
    std::string name_;
    bool ok_ = false;
};

// runs 容器里指定状态的条目数（撞闸用例的 DB 侧观察面）
size_t countStatus(const std::vector<AgentRunRow>& rows, const std::string& status) {
    return static_cast<size_t>(std::count_if(
        rows.begin(), rows.end(),
        [&status](const AgentRunRow& r) { return r.status == status; }));
}

// 跨路径用例自持的 manual 线程：用例提前 return（断言失败）时也必须 cancel + join，
// 否则栈上 joinable 的 std::thread 析构即 std::terminate。
class ManualJoinGuard {
public:
    ManualJoinGuard(std::thread& th, std::atomic<bool>& cancel)
        : th_(th), cancel_(cancel) {}
    ~ManualJoinGuard() {
        cancel_.store(true);
        if (th_.joinable()) th_.join();
    }
    ManualJoinGuard(const ManualJoinGuard&) = delete;
    ManualJoinGuard& operator=(const ManualJoinGuard&) = delete;

private:
    std::thread& th_;
    std::atomic<bool>& cancel_;
};

} // namespace

REGISTER_TEST(scheduler_tick_fires_matching_task_once_per_minute,
              "scheduler: enabled+schedule+cron 命中 → 派发一次；同分钟不重复；下一分钟再触发", {
    test_support::TestDb tdb("agentsched", "tick");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };            // 立即 end_turn
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;                              // 任意整基点
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("每分钟", "心跳", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *"})");   // triggerConfig = 末位形参
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    // 有界等这一跑落终态**再** stopAll：只等「条目出现」就 stopAll 会与 run 线程
    // 赛跑——派发线程与 run 线程并发，cancel 先到就把本该 completed 的一跑记成
    // cancelled。终态断言必须建立在 DB 已定局之上。
    for (int i = 0; i < 500; ++i) {
        auto rs = store.listRuns(taskId.value(), 10);
        if (rs.isOk() && rs.value().size() == 1 && rs.value()[0].status != "running")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_EQ(sched.activeRunCount(), 1u);           // 条目仍在（未 reap）
    sched.stopAll();                                         // join，run 落 completed
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    // 同分钟：不重复派发
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    DOCTEST_CHECK(store.listRuns(taskId.value(), 10).value().size() == 1u);
    // 下一分钟：再触发
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && sched.activeRunCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 1u);
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_tick_skips_disabled_manual_and_bad_cron,
              "scheduler: disabled/manual/坏 cron 配置 → 不派发不崩", {
    test_support::TestDb tdb("agentsched", "skip");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, [] { return int64_t(3600 * 24); });
    auto dis = store.createTask("禁用", "g", "schedule", "default", 5, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(dis.isOk());
    DOCTEST_REQUIRE(store.setTaskEnabled(dis.value(), false).isOk());
    auto man = store.createTask("手动", "g", "manual", "default", 5, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(man.isOk());
    auto bad = store.createTask("坏配置", "g", "schedule", "default", 5, false, R"({"cron":"BROKEN"})");
    DOCTEST_REQUIRE(bad.isOk());
    auto noCfg = store.createTask("无配置", "g", "schedule", "default", 5, false);
    DOCTEST_REQUIRE(noCfg.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 不抛不崩
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    for (int64_t id : {dis.value(), man.value(), bad.value(), noCfg.value()})
        DOCTEST_CHECK(store.listRuns(id, 10).value().empty());
    sched.stopAll();
    return true;
})

// 「每天」触发 = 不指定时刻：本地当天第一次 tick 派发一次，当天后续 tick 不重复；
// 跨天后第一次 tick 恢复。判定读 agent_runs（跨重启也成立），不是内存标记。
// 时间基准取真实今天：agent_runs.started_at 由 AgentStore 用系统时钟落库，只有
// 「当天 / 次日」这种日历日边界断言才不受真实时钟的时/分影响。
REGISTER_TEST(scheduler_daily_runs_once_on_first_tick_of_local_day,
              "scheduler: 每天触发 → 本地当天首次 tick 跑一次、当天不重复、次日恢复", {
    test_support::TestDb tdb("agentsched", "daily");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);

    const int64_t realNow = static_cast<int64_t>(std::time(nullptr));
    std::tm base{};
    core::time::localTime(static_cast<std::time_t>(realNow), &base);
    auto atLocal = [](std::tm t, int hour, int minute) {
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;   // mktime 归一（月末/年末/夏令时都交给 libc）
        return static_cast<int64_t>(std::mktime(&t));
    };
    int64_t nowSec = atLocal(base, 10, 0);
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("每天", "g", "daily", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());

    auto waitFinishedRuns = [&](size_t want) {
        for (int i = 0; i < 500; ++i) {
            auto runs = store.listRuns(taskId.value(), 10);
            if (runs.isOk() && runs.value().size() >= want &&
                std::all_of(runs.value().begin(), runs.value().end(),
                            [](const AgentRunRow& r) { return r.status != "running"; }))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    DOCTEST_REQUIRE(sched.tickNow().isOk());
    DOCTEST_CHECK(waitFinishedRuns(1));
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 10).value().size(), 1u);

    nowSec += 61;   // 当天下一分钟：已有当天 run → 不再派发
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 10).value().size(), 1u);

    std::tm nextDay = base;
    nextDay.tm_mday += 1;   // 次日（mktime 归一）
    nowSec = atLocal(nextDay, 0, 5);
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    DOCTEST_CHECK(waitFinishedRuns(2));
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 10).value().size(), 2u);

    sched.stopAll();
    return true;
})

// 坏 trigger_config 曾是**静默** continue：用户看到任务在列表里存在、永不触发、
// 没有任何提示（UI 用同一份 JSON 渲染，两边对「配置是否有效」的判断还会各自漂移）。
// 契约：解码失败 = 一条带 task id 的 LOG_WARN + 本分钟不派发。
REGISTER_TEST(scheduler_bad_trigger_config_logs_and_skips,
              "scheduler: trigger_config 坏 → LOG_WARN 点名 task id 后跳过（不再静默）", {
    test_support::TestDb tdb("agentsched", "badcfg");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, [] { return int64_t(3600 * 24); });

    auto bad = store.createTask("坏配置", "g", "schedule", "default", 5, false,
                                R"({"cron":"BROKEN"})");     // 有值但非法
    DOCTEST_REQUIRE(bad.isOk());
    auto empty = store.createTask("空配置", "g", "schedule", "default", 5, false);
    DOCTEST_REQUIRE(empty.isOk());                           // trigger_config = ''
    auto badJson = store.createTask("非JSON", "g", "schedule", "default", 5, false,
                                    "{ not json !!!");
    DOCTEST_REQUIRE(badJson.isOk());
    auto nonObj = store.createTask("非对象", "g", "schedule", "default", 5, false,
                                   R"(["cron"])");
    DOCTEST_REQUIRE(nonObj.isOk());

    // 日志捕获：core::log 只有 addFileOutput（无关闭接口）——捕获后本进程后续
    // 日志都写这个临时文件，不影响用例正确性
    const auto logPath = test_temp::tempPath("devdesk_test_sched_badcfg.log");
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    core::log::addFileOutput(logPath);

    DOCTEST_REQUIRE(sched.tickNow().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);            // 一个都不派发
    for (const auto& t : {bad, empty, badJson, nonObj})
        DOCTEST_CHECK(store.listRuns(t.value(), 10).value().empty());
    sched.stopAll();

    std::ifstream in(logPath, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string logText = oss.str();
    for (const auto& t : {bad, empty, badJson, nonObj}) {
        DOCTEST_CHECK_MESSAGE(
            logText.find("任务 " + std::to_string(t.value())) != std::string::npos,
            logText);
        DOCTEST_CHECK(logText.find("trigger_config") != std::string::npos);
    }
    return true;
})

REGISTER_TEST(scheduler_tick_skips_running_task,
              "scheduler: 任务在跑时下一分钟 tick → startRunExclusive 拒绝，不双跑", {
    test_support::TestDb tdb("agentsched", "concurrent");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.tick");
    SlowToolForSched tool{"test.tick"};   // 文件内定义：execute 睡 300ms 的 test.tick 工具
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.tick"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("慢任务", "g", "schedule", "default", 5, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && tool.calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE(tool.calls.load() == 1);                 // 第一跑在途（睡 300ms）
    nowSec += 61;                                            // 下一分钟
    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 起线程 → loop_.run 撞闸
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 被拒那一路不落行 → DB 仍只有 1 行 running（sched.runs_ 里会多一条已收尾的被拒
    // 条目，由下次 tick / stopAll 的 reapDone() 摘除）
    auto mid = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(mid.isOk());
    DOCTEST_CHECK_EQ(countStatus(mid.value(), "running"), 1u);
    DOCTEST_CHECK_EQ(tool.calls.load(), 1);                  // 未起第二跑
    sched.stopAll();                                         // cancel → run 落 cancelled
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    return true;
})

// 跨路径钉子：手动路径（AgentModule 同款：独立线程直调 loop_.run）× cron 路径
// （tickNow → dispatchRun）同任务交叠。单闸在 AgentLoop::run 内，第二路撞闸即拒
// （WARN 在 run 线程内，无行可落）：两套 run 线程表合计 2 个在跑条目，DB 恰 1 行
// running。若两路各自预插行，会落 2 行 running、同一任务双跑。
REGISTER_TEST(scheduler_cross_path_single_gate_no_double_run,
              "scheduler: 手动 × cron 同任务交叠 → 单闸拒第二路（DB 恰 1 行 running）", {
    test_support::TestDb tdb("agentsched", "crossgate");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.cross");
    SlowToolForSched tool{"test.cross"};   // 睡 300ms（Task 4 定义复用）：把第一跑钉在闸内
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.cross"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("交叠", "g", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    // 路径 A：手动（AgentModule 的 run 线程同款——独立线程直调 loop_.run）
    auto rowA = store.getTask(taskId.value());
    DOCTEST_REQUIRE(rowA.isOk());
    AgentTask manualTask = module::agent::taskFromRow(rowA.value());
    std::atomic<bool> manualCancel{false};
    std::thread manual([&] { (void)loop.run(manualTask, manualCancel); });
    ManualJoinGuard manualGuard{manual, manualCancel};
    for (int i = 0; i < 500 && tool.calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE(tool.calls.load() == 1);                 // A 在跑（闸被占）
    // 路径 B：cron tick 派发同任务 → 起线程 → loop_.run 撞闸（WARN 在 run 线程内）
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && sched.activeRunCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 1u);            // B 条目在（线程已起）
    DOCTEST_CHECK_EQ(tool.calls.load(), 1);                  // 第二路从未执行
    auto mid = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(mid.isOk());
    DOCTEST_CHECK_EQ(countStatus(mid.value(), "running"), 1u);   // DB 恰 1 行 running
    manualCancel.store(true);                                // A 收尾（在跑的 run 落 cancelled）
    manual.join();
    sched.stopAll();
    auto after = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_REQUIRE_EQ(after.value().size(), 1u);            // 第二路从未落行
    DOCTEST_CHECK_EQ(after.value()[0].status, std::string("cancelled"));
    return true;
})

// 常驻 worker 队列 + stopAll 收口

REGISTER_TEST(scheduler_resident_queue_runs_enqueued_jobs,
              "scheduler: 常驻队列两个不同任务各起一跑（确定 2 行 run）；空队列不空转", {
    test_support::TestDb tdb("agentsched", "resident");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;   // send 带 mutex：并行派发下两 run 并发 send
    core::ToolRegistry::instance().unregisterTool("test.res");
    FastToolForSched tool{"test.res"};
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.res"), endTurnResp(),
                   toolUseResp("test.res"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});         // 生产时钟即可
    sched.start();                                           // 起常驻线程
    // 两个**不同** taskId：闸不互相拒 → 跑数确定。常驻线程 pop 即 spawn、不等上一跑
    // 结束，两跑响应的游标交错顺序不定 → 只断言「每任务恰 1 跑 + 终态 completed」，
    // 不断言工具调用次数。
    auto a = store.createTask("常驻A", "g", "resident", "default", 5, false);
    auto b = store.createTask("常驻B", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    sched.enqueueResident(a.value());
    sched.enqueueResident(b.value());
    for (int i = 0; i < 1000; ++i) {                         // 有界等两跑落终态
        auto ra = store.listRuns(a.value(), 10);
        auto rb = store.listRuns(b.value(), 10);
        if (ra.isOk() && rb.isOk() && ra.value().size() == 1 && rb.value().size() == 1 &&
            ra.value()[0].status != "running" && rb.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto ra = store.listRuns(a.value(), 10);
    auto rb = store.listRuns(b.value(), 10);
    DOCTEST_REQUIRE(ra.isOk());
    DOCTEST_REQUIRE(rb.isOk());
    DOCTEST_REQUIRE_EQ(ra.value().size(), 1u);               // 每任务恰 1 跑（无第三跑）
    DOCTEST_REQUIRE_EQ(rb.value().size(), 1u);
    DOCTEST_CHECK_EQ(ra.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(rb.value()[0].status, std::string("completed"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 空队列不空转
    DOCTEST_CHECK_EQ(store.listRuns(a.value(), 10).value().size(), 1u);
    DOCTEST_CHECK_EQ(store.listRuns(b.value(), 10).value().size(), 1u);
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_resident_same_task_twice_single_gate,
              "scheduler: 同 task 连发两单 → 单闸只放一跑（1 行 run；第二单 WARN 后收尾）", {
    test_support::TestDb tdb("agentsched", "residentdup");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.slow");
    SlowToolForSched tool{"test.slow"};   // 第一跑钉在闸内，第二单必撞闸
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.slow"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    sched.start();
    auto taskId = store.createTask("常驻重发", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());
    sched.enqueueResident(taskId.value());
    sched.enqueueResident(taskId.value());                   // 同 task 连发两单
    for (int i = 0; i < 1000; ++i) {                         // 有界等唯一一跑落终态
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 1 &&
            runs.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);             // 第二单从未落行
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(tool.calls.load(), 1);                  // 单闸只放一跑（第二单未执行）
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_stopall_cancels_inflight_resident_run,
              "scheduler: stopAll 取消在跑的常驻 run 并 join（teardown 契约）", {
    test_support::TestDb tdb("agentsched", "stop");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.slow");
    SlowToolForSched tool{"test.slow"};   // 在跑期间 stopAll
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.slow") };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    sched.start();
    auto taskId = store.createTask("常驻慢", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());
    sched.enqueueResident(taskId.value());
    for (int i = 0; i < 500 && tool.calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE(tool.calls.load() == 1);                 // 在跑（睡 300ms 中）
    sched.stopAll();                                         // 阻塞到 cancel+join 完成
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    return true;
})

// enqueueResident 的 stopFlag 检查与 push 必须在同一把锁内：stopAll 窗口里 push 的
// 条目会被常驻线程消费不到（线程已退出）。本用例钉住可观察后果：stopAll 之后入队
// **不产生任何 run**（有界观察窗，无时序余量——幽灵单若被消费，10ms 内必见 run 行）。
REGISTER_TEST(scheduler_resident_enqueue_after_stop_is_dropped,
              "scheduler: stopAll 后 enqueueResident 被丢弃（不产生 run）", {
    test_support::TestDb tdb("agentsched", "stopped");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { toolUseResp("test.res"), endTurnResp() };
    core::ToolRegistry::instance().unregisterTool("test.res");
    FastToolForSched tool{"test.res"};
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    auto taskId = store.createTask("停后入队", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());
    sched.start();                                           // 起常驻线程
    sched.stopAll();                                         // 先停（常驻线程已 join）
    sched.enqueueResident(taskId.value());                   // 停止后入队 → 丢弃
    DOCTEST_CHECK_EQ(sched.residentQueueSize(), 0u);         // 不留幽灵单（杀「摘守卫」变异）
    for (int i = 0; i < 50; ++i) {                           // 有界观察窗（500ms 上限）
        DOCTEST_CHECK(store.listRuns(taskId.value(), 10).value().empty());
        DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
})

// ---- 事件触发 + 冷却（M4 Task 2）。用例名前缀 event_trigger_（--filter event_trigger
// 可定向跑，避开中文过滤词的传参编码坑）。驱动方式与 tickNow 用例同构：不起事件线程，
// onEvent 入队 + drainEventQueue 直调（零等待、确定性），冷却用注入时钟推走。 ----

// 事件路由：event 型任务在事件到达后被起跑；manual/schedule/resident 任务对同一事件
// 无感；不匹配的事件名不触发
REGISTER_TEST(event_trigger_routing_fires_matching_event_task_only,
              "event_trigger: 事件路由只派发 enabled+event 型+events 含该名的任务", {
    test_support::TestDb tdb("agentsched", "evtroute");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});         // 生产时钟即可（无冷却断言）
    auto evOk = store.createTask("事件", "g", "event", "default", 5, false,
                                 R"({"events":["todo.created","md.saved"]})");
    auto manual = store.createTask("手动", "g", "manual", "default", 5, false,
                                   R"({"events":["todo.created"]})");
    auto cronT = store.createTask("定时", "g", "schedule", "default", 5, false,
                                  R"({"cron":"0 9 * * 1-5"})");
    auto other = store.createTask("别的事件", "g", "event", "default", 5, false,
                                  R"({"events":["md.saved"]})");
    DOCTEST_REQUIRE(evOk.isOk() && manual.isOk() && cronT.isOk() && other.isOk());
    // 不匹配的事件名：没有任何任务起跑（队列清空、零 run）
    sched.onEvent("todo.completed");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (const auto& t : {evOk, manual, cronT, other})
        DOCTEST_CHECK(store.listRuns(t.value(), 10).value().empty());
    // 匹配：只有第一个任务派发
    sched.onEvent("todo.created");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    DOCTEST_CHECK_EQ(sched.eventQueueSize(), 0u);
    for (int i = 0; i < 500; ++i) {                          // 有界等唯一一跑落终态
        auto runs = store.listRuns(evOk.value(), 10);
        if (runs.isOk() && runs.value().size() == 1 &&
            runs.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto fired = store.listRuns(evOk.value(), 10);
    DOCTEST_REQUIRE(fired.isOk());
    DOCTEST_REQUIRE_EQ(fired.value().size(), 1u);
    DOCTEST_CHECK_EQ(fired.value()[0].status, std::string("completed"));
    // 其它三类任务零 run（schedule 不因事件被起，manual/resident 不消费事件）
    for (const auto& t : {manual, cronT, other})
        DOCTEST_CHECK(store.listRuns(t.value(), 10).value().empty());
    sched.stopAll();
    return true;
})

// 任务级冷却默认 300s：窗口内第二次事件被**丢弃**（不排队不延后——eventQueueSize 归
// 零、无第二行 run）；窗口过后再触发
REGISTER_TEST(event_trigger_cooldown_default_300s_drops_within_window,
              "event_trigger: 冷却默认 300s——窗口内事件丢弃不排队，窗口过后再派发", {
    test_support::TestDb tdb("agentsched", "evtcd");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 100000;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("冷却", "g", "event", "default", 5, false,
                                    R"({"events":["todo.created"]})");
    DOCTEST_REQUIRE(taskId.isOk());
    // 第一次事件 → 派发（冷却起点 = nowSec）
    sched.onEvent("todo.created");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 1 &&
            runs.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_EQ(store.listRuns(taskId.value(), 10).value().size(), 1u);
    // 窗口内（+100s < 300s）：第二次事件被丢弃——不排队（队列已清）、不产生第二行
    nowSec += 100;
    sched.onEvent("todo.created");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    DOCTEST_CHECK_EQ(sched.eventQueueSize(), 0u);             // 丢弃不是延后
    for (int i = 0; i < 50; ++i) {                            // 有界观察窗（500ms）
        DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 10).value().size(), 1u);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 窗口边界过后（+300s 相对冷却起点已过）：再触发第二跑
    nowSec += 200;                                           // 累计 +300s
    sched.onEvent("todo.created");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 2 &&
            runs.value()[0].status != "running" &&
            runs.value()[1].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 2u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(runs.value()[1].status, std::string("completed"));
    sched.stopAll();
    return true;
})

// cooldown_sec 覆盖默认：60s 冷却下 +100s 的第二次事件照常派发
REGISTER_TEST(event_trigger_cooldown_override_by_config,
              "event_trigger: cooldown_sec=60 覆盖默认——+100s 的第二次事件再派发", {
    test_support::TestDb tdb("agentsched", "evtcdovr");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 200000;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("短冷却", "g", "event", "default", 5, false,
                                    R"({"events":["md.saved"],"cooldown_sec":60})");
    DOCTEST_REQUIRE(taskId.isOk());
    sched.onEvent("md.saved");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 1 &&
            runs.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_EQ(store.listRuns(taskId.value(), 10).value().size(), 1u);
    nowSec += 100;                                           // > 60s 冷却窗口
    sched.onEvent("md.saved");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 2 &&
            runs.value()[1].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 10).value().size(), 2u);
    sched.stopAll();
    return true;
})

// 冷却键粒度 = 任务×事件组合（R-5）：同任务订阅两个事件时，事件一触发后、默认 300s
// 窗口内事件二**仍可派发**。键若退化为 taskId 单键，事件一会压制同任务的事件二——此
// 用例必红（第二跑永不出现）；修法是键含事件名，逐组合计时
REGISTER_TEST(event_trigger_cooldown_key_is_task_x_event,
              "event_trigger: 冷却键=任务×事件——同任务第二事件在默认 300s 窗口内仍派发", {
    test_support::TestDb tdb("agentsched", "evtcdkey");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 300000;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("双事件", "g", "event", "default", 5, false,
                                    R"({"events":["ev.a","ev.b"]})");
    DOCTEST_REQUIRE(taskId.isOk());
    // 事件一 → 派发（(task, ev.a) 冷却起点 = nowSec）
    sched.onEvent("ev.a");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 1 &&
            runs.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_EQ(store.listRuns(taskId.value(), 10).value().size(), 1u);
    // +100s（仍在 300s 默认窗口内）事件二到达：不同事件名 → 不受事件一冷却压制
    nowSec += 100;
    sched.onEvent("ev.b");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 2 &&
            runs.value()[1].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 10).value().size(), 2u);
    sched.stopAll();
    return true;
})

// 坏 trigger_config（两者皆缺 / events 空数组 / events 非数组）→ 点名 task id 的
// WARN + 跳过（不静默）；disabled 的 event 任务不派发。日志捕获同 badcfg 用例
REGISTER_TEST(event_trigger_bad_config_logs_and_skips,
              "event_trigger: 坏 events 配置 → WARN 点名 task id 后跳过；disabled 不派发", {
    test_support::TestDb tdb("agentsched", "evtbad");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, [] { return int64_t(3600 * 24); });
    auto both = store.createTask("两者皆缺", "g", "event", "default", 5, false, R"({})");
    auto emptyArr = store.createTask("空数组", "g", "event", "default", 5, false,
                                     R"({"events":[]})");
    auto notArr = store.createTask("非数组", "g", "event", "default", 5, false,
                                   R"({"events":"todo.created"})");
    auto dis = store.createTask("禁用", "g", "event", "default", 5, false,
                                R"({"events":["todo.created"]})");
    DOCTEST_REQUIRE(both.isOk() && emptyArr.isOk() && notArr.isOk() && dis.isOk());
    DOCTEST_REQUIRE(store.setTaskEnabled(dis.value(), false).isOk());

    const auto logPath = test_temp::tempPath("devdesk_test_sched_evtbad.log");
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    core::log::addFileOutput(logPath);

    for (const char* ev : {"todo.created", "md.saved"}) {
        sched.onEvent(ev);
        DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (const auto& t : {both, emptyArr, notArr, dis})
        DOCTEST_CHECK(store.listRuns(t.value(), 10).value().empty());

    std::ifstream in(logPath, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string logText = oss.str();
    for (const auto& t : {both, emptyArr, notArr}) {
        DOCTEST_CHECK_MESSAGE(
            logText.find("任务 " + std::to_string(t.value())) != std::string::npos,
            logText);
        DOCTEST_CHECK(logText.find("trigger_config") != std::string::npos);
    }
    // disabled 无 WARN、无 run（用例自身的 listRuns 空断言已钉）
    sched.stopAll();
    return true;
})

// ---- 事件线程真起（共享 cv_ 丢唤醒的离线守卫）----
// 上面的事件用例全走 drainEventQueue 直调：不起事件线程，覆盖不到「等待线程被
// 唤醒」这一段。本用例 start() 起真线程——常驻 + 事件两个等待者共用同一把 cv_，
// onEvent 的通知摇中错误等待者（谓词假、重睡）时事件滞留队列、永不消费。每轮
// onEvent 后有界轮询 eventQueueSize()==0 断言抽干。零 LLM：库里无任务，
// processEvent 的 listTasks 为空，dispatchRun 永不触达 MockLlm；零 store 写。
// notify_one 形态下每轮约 50% 红，20 轮全绿 ≈ 1-0.5^20。
REGISTER_TEST(event_trigger_real_thread_wakeup_not_lost,
              "event_trigger: 真事件线程抽干 onEvent×20（共享 cv_ 丢唤醒回归守卫，零 LLM）", {
    test_support::TestDb tdb("agentsched", "evtwake");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;   // 空 script：本用例任何 send 都说明事件被误派发了
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    sched.start();                                           // 常驻 + 事件两个等待线程真起
    for (int round = 0; round < 20; ++round) {
        sched.onEvent("todo.created");
        bool drained = false;
        for (int i = 0; i < 400 && !drained; ++i) {          // 有界轮询（2s 上限）
            drained = sched.eventQueueSize() == 0;
            if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        DOCTEST_CHECK_MESSAGE(drained,
                              "第 " + std::to_string(round) +
                                  " 轮事件 2s 未被事件线程抽干（共享 cv_ 丢唤醒）");
        if (!drained) break;
    }
    DOCTEST_CHECK_EQ(llm.sentMessages.size(), 0u);           // 零 LLM：只入队不派发
    sched.stopAll();
    return true;
})

// ---- 限次 / 一次性任务（max_runs = trigger_config 里的可选上限键）----
// 语义（缺省不限，保持今日「恒循环」）：派发前数 agent_runs 里该任务已起过跑的 run，
// 达上限 → 不派发 + setTaskEnabled(false) 置停用。计数含 running/failed/cancelled/
// limited ——记账单位是「起过跑」而不是「跑成功」（用户要的是「别再反复跑」）。
// 驱动方式与上方用例同构：假时钟 + tickNow / drainEventQueue 直调，零墙钟零网络。

namespace {
// 有界等「该任务恰有 n 行 run 且全部落终态」：run 线程异步，终态断言必须等定局
bool waitRunsSettled(module::agent::AgentStore& store, int64_t taskId, size_t n) {
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId, 20);
        if (runs.isOk() && runs.value().size() == n) {
            bool settled = true;
            for (const auto& r : runs.value())
                settled = settled && r.status != "running";
            if (settled) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

// 判据 ① + ③：上限 1 的任务跑完一次后不再派发、并被置停用；此后每分钟扫描都不再起跑。
// 同分钟那次 tick 是关键：同分钟防重也会「不派发」，但它**不置停用**——enabled 转 false
// 才是额度判定生效的证据（防「防重顶替成停用」的误判）
REGISTER_TEST(scheduler_max_runs_once_disables_and_stops_dispatch,
              "scheduler: max_runs=1 —— 首跑后置停用；此后各分钟不再派发（停用不占 run 队列）", {
    test_support::TestDb tdb("agentsched", "maxruns1");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp() };   // 第二跑若发生必被抓
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("只跑一次", "g", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *","max_runs":1})");
    DOCTEST_REQUIRE(taskId.isOk());

    // 日志是自动停用对用户的可见通道（列表徽章之外唯一）
    const auto logPath = test_temp::tempPath("devdesk_test_sched_maxruns.log");
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    core::log::addFileOutput(logPath);

    DOCTEST_REQUIRE(sched.tickNow().isOk());
    DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), 1u),
                            "首跑未落终态（有界等待超时）");
    auto afterFirst = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterFirst.isOk());
    DOCTEST_CHECK(afterFirst.value().enabled);               // 首跑期间仍启用（跑完才停）

    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 同分钟：额度判定应置停用
    auto afterSecond = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterSecond.isOk());
    DOCTEST_CHECK_FALSE(afterSecond.value().enabled);        // 自动停用
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 1u);

    // 停用后：后续每分钟照常扫描，但不再派发（判据 ③）
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 1u);
    sched.stopAll();

    std::ifstream in(logPath, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string logText = oss.str();
    DOCTEST_CHECK_MESSAGE(logText.find("自动停用") != std::string::npos, logText);
    DOCTEST_CHECK(logText.find(std::to_string(taskId.value())) != std::string::npos);
    return true;
})

// 对照组（防「一刀切把所有任务停用」）：旧配置串（无 max_runs）跨三分钟照旧反复派发、
// 且永不被自动停用——旧行没有该键 → decode 恒不解出上限 → Scheduler 恒派发
REGISTER_TEST(scheduler_max_runs_unlimited_keeps_repeating,
              "scheduler: 无 max_runs（旧配置串）—— 连续三分钟反复派发且不被停用", {
    test_support::TestDb tdb("agentsched", "maxruns0");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("每分钟", "g", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    for (size_t n = 1; n <= 3; ++n) {
        DOCTEST_REQUIRE(sched.tickNow().isOk());
        DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), n),
                                "第 " + std::to_string(n) + " 跑未落终态");
        auto row = store.getTask(taskId.value());
        DOCTEST_REQUIRE(row.isOk());
        DOCTEST_CHECK_MESSAGE(row.value().enabled, "无上限任务被误停用（第 " +
                                                       std::to_string(n) + " 跑后）");
        nowSec += 61;
    }
    sched.stopAll();
    return true;
})

// 上限 N>1 的半程语义：额度未满照常派发（第 2 跑真的起），第三次扫描才置停用。
// 只按 max_runs 做了「上限裁剪样式」的实现（例如把 2 当 1）会在这里红
REGISTER_TEST(scheduler_max_runs_two_runs_then_disables,
              "scheduler: max_runs=2 —— 第 2 跑照常派发，第 3 次扫描置停用不再派发", {
    test_support::TestDb tdb("agentsched", "maxruns2");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp(), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("跑两次", "g", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *","max_runs":2})");
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), 1u), "首跑未落终态");
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 额度剩 1 → 照常派发
    DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), 2u),
                            "第 2 跑未发生（额度未满却停发）");
    auto afterTwo = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterTwo.isOk());
    DOCTEST_CHECK(afterTwo.value().enabled);                 // 满额前不停用
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 满额 → 置停用 + 不派发
    auto afterThree = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterThree.isOk());
    DOCTEST_CHECK_FALSE(afterThree.value().enabled);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 2u);
    sched.stopAll();
    return true;
})

// 失败语义的钉子：首跑失败也算用掉额度（记账单位 = 起过跑）。若改成「只数 completed」，
// 失败的一次性任务会每分钟重试到成功为止，最坏情况无限消耗 LLM 配额——正是本单要消灭的
REGISTER_TEST(scheduler_max_runs_failed_run_counts_toward_limit,
              "scheduler: max_runs=1 —— 首跑 failed 也算用掉额度（不重试到成功）", {
    test_support::TestDb tdb("agentsched", "maxrunsfail");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;                                             // 空 script：send 必失败
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("失败也算", "g", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *","max_runs":1})");
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), 1u), "首跑未落终态");
    auto first = store.listRuns(taskId.value(), 20);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_REQUIRE_EQ(first.value().size(), 1u);
    DOCTEST_REQUIRE_EQ(first.value()[0].status, std::string("failed"));
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_FALSE(row.value().enabled);                // 失败也把额度用掉了
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 1u);
    sched.stopAll();
    return true;
})

// 事件型同一语义（票面明说 cron/事件触发恒为循环）：首跑后置停用、后续事件不再派发。
// 事件型没有周期扫描——唯一的「下一次调度扫描」就是下一次事件到达，故额度判定放在
// events 比对之前（不匹配的事件也会把已耗尽的一次性任务置停用，但绝不派发任何 run）
REGISTER_TEST(event_trigger_max_runs_once_disables_and_stops_dispatch,
              "event_trigger: max_runs=1 —— 事件首跑后置停用；后续事件（含不匹配）不再派发", {
    test_support::TestDb tdb("agentsched", "evtmaxruns");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp(), endTurnResp() };   // 第二跑若发生必被抓
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    auto taskId = store.createTask("事件一次", "g", "event", "default", 5, false,
                                   R"({"events":["todo.created"],"max_runs":1})");
    DOCTEST_REQUIRE(taskId.isOk());
    sched.onEvent("todo.created");
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), 1u),
                            "事件首跑未落终态");
    // 事件型无周期扫描：此刻还没人做额度判定，任务仍是启用态（下次事件到达才停用）
    auto afterFirst = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterFirst.isOk());
    DOCTEST_CHECK(afterFirst.value().enabled);

    sched.onEvent("md.saved");                               // 不匹配本任务的事件
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    auto afterUnrelated = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterUnrelated.isOk());
    DOCTEST_CHECK_FALSE(afterUnrelated.value().enabled);     // 额度耗尽 → 扫描即停用
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 1u);

    sched.onEvent("todo.created");                           // 匹配事件也不再有 run（已停用）
    DOCTEST_REQUIRE(sched.drainEventQueue().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 1u);
    sched.stopAll();
    return true;
})

// schedule 型的额度判定必须**与 cron 是否匹配无关**：每分钟都重判，跑满即停（≤1 分钟
// 就在库里可见）。否则「每天 18:00 只跑一次」的任务跑完后会一直显示「启用」直到第二天
// 18:00 才被停用 —— 用户看不见停，与 scheduler.cpp 额度闸块注释写的契约相反。
// 只把闸放在 cronMatches **之后**的实现会在这里红：不匹配的分钟永远走不到额度判定。
REGISTER_TEST(scheduler_max_runs_disables_on_non_matching_minute,
              "scheduler: 一次性任务跑满后，cron 不匹配的分钟也会被置停用（≤1 分钟可见）", {
    test_support::TestDb tdb("agentsched", "maxrunsoff");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };   // 不匹配的分钟若仍派发必被抓（脚本为空会红）
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });

    // 用**本机本地时刻**拼「每天这一分钟」的 cron：用例不依赖运行机器的时区设置
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(nowSec), &local);
    const std::string cron = std::to_string(local.tm_min) + " " +
                             std::to_string(local.tm_hour) + " * * *";
    auto taskId = store.createTask("每天一次", "g", "schedule", "default", 5, false,
                                   "{\"cron\":\"" + cron + "\",\"max_runs\":1}");
    DOCTEST_REQUIRE(taskId.isOk());

    // 匹配分钟：派发首跑（= 额度用尽），当分钟仍是启用态
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    DOCTEST_REQUIRE_MESSAGE(waitRunsSettled(store, taskId.value(), 1u),
                            "首跑未落终态");
    auto afterFirst = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterFirst.isOk());
    DOCTEST_CHECK(afterFirst.value().enabled);

    // 5 分钟后（同一天、cron 不匹配）：额度判定照跑 → 置停用，且不肯多派一发
    nowSec += 5 * 60;
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto afterOffMinute = store.getTask(taskId.value());
    DOCTEST_REQUIRE(afterOffMinute.isOk());
    DOCTEST_CHECK_FALSE(afterOffMinute.value().enabled);
    DOCTEST_CHECK_EQ(store.listRuns(taskId.value(), 20).value().size(), 1u);
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    sched.stopAll();
    return true;
})
