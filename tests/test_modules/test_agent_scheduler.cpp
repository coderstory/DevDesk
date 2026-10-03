// tests/test_modules/test_agent_scheduler.cpp
// Scheduler 用例（agent M2 Task 4）：cron 定时触发（tickNow 直调零等待）、同分钟防重、
// 原子互斥派发（撞闸不双跑）、跨路径单闸（手动 module 路径 × cron 路径交叠）。
// Task 5 追加：常驻 worker 队列 + stopAll 收口契约。
// LlmEndpoint / ToolRegistry 均为注入桩，零网络；时钟经 NowFn 注入，断言无墙钟余量。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "module_agent/scheduler.h"
#include "core/llm_endpoint.h"
#include "core/logging.h"
#include "core/tool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace module::agent;

// 脚本化 mock（test_agent_loop.cpp:22-45 同款，**加 std::mutex**）：常驻 / 多派发下
// 两个 run 会并发 send，共享的 script 游标与记录容器必须自保。
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

// 进程级 ToolRegistry 单例 + 借用栈对象：注册/注销必须成对。本仓库 CHECK 失败即
// `return false`（fail-fast），手写注销会被跳过 → 注册表留下悬垂裸指针（RED 期
// 实测整个 exe 崩在 exit 138）。RAII 保证注销一定发生（test_agent_loop.cpp:129 同款）。
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
    // cancelled（实测 3/3 命中）。终态断言必须建立在 DB 已定局之上（零墙钟余量）。
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

// ARCH-02（M2 静态审计，high）：坏 trigger_config 此前是**静默** continue——
// 用户看到任务在列表里存在、永不触发、没有任何提示（UI 用同一份 JSON 渲染，
// 两边对「配置是否有效」的判断还会各自漂移）。修复后：解码失败 = 一条带
// task id 的 LOG_WARN + 本分钟不派发。
// 红（修复前）：日志里没有该 task id 的 WARN（断言失败）；
// 绿（修复后）：日志含 task id + 「trigger_config」，且仍然不派发。
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

    // 日志捕获：core::log 只有 addFileOutput（无关闭接口，test_logging.cpp 同款
    // 先例）——捕获后本进程后续日志都写这个临时文件，不影响用例正确性
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
        // 修复前：整段日志里连 task id 都不出现（静默 continue）
        DOCTEST_CHECK_MESSAGE(
            logText.find("任务 " + std::to_string(t.value())) != std::string::npos,
            logText);
        DOCTEST_CHECK(logText.find("trigger_config") != std::string::npos);
    }
    // 恢复进程日志落点：后续用例（含 test_logging）各自管理自己的输出
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
    // 条目，由下次 tick / stopAll 的 reapDone() 摘除——activeRunCount 是条目数、
    // 不是「在跑数」，别拿它当跑数断言）
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

// Review Focus 1 跨路径钉子：手动路径（AgentModule 同款：独立线程直调 loop_.run）×
// cron 路径（tickNow → dispatchRun）同任务交叠。单闸在 AgentLoop::run 内，第二路
// 撞闸即拒（run 线程内 LOG_WARN，无行可落）：两套 run 线程表合计 2 个在跑条目，
// DB 恰 1 行 running——旧「两路各自预插」案会落 2 行 running、同一任务双跑。
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

// ==================== Task 5：常驻 worker 队列 + stopAll 收口 ====================

REGISTER_TEST(scheduler_resident_queue_runs_enqueued_jobs,
              "scheduler: 常驻队列两个不同任务各起一跑（确定 2 行 run）；空队列不空转", {
    test_support::TestDb tdb("agentsched", "resident");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;   // 文件内副本：send 带 std::mutex（并行派发下两 run 并发 send）
    core::ToolRegistry::instance().unregisterTool("test.res");
    FastToolForSched tool{"test.res"};   // 立即返回（文件内定义）
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.res"), endTurnResp(),
                   toolUseResp("test.res"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});         // 生产时钟即可
    sched.start();                                           // 起常驻线程
    // 两个**不同** taskId：闸不互相拒 → 跑数确定（同 task 连发的闸语义见下一用例）。
    // 常驻线程 pop 即 spawn、不等上一跑结束，两跑响应的游标交错顺序不定 → 只断言
    // 「每任务恰 1 跑 + 终态 completed」，不断言工具调用次数。
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
    SlowToolForSched tool{"test.slow"};   // 睡 300ms：第一跑钉在闸内，第二单必撞闸
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
    SlowToolForSched tool{"test.slow"};                      // 睡 300ms（Task 4 定义复用）
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

// 评审修复轮（2026-10-01）：enqueueResident 的 stopFlag 检查与 push 原本不在同一把
// 锁内 → stopAll 窗口里 push 的条目永不被消费（常驻线程已退出）。修复后「已停则不接单」
// 在 mu_ 内判定。本用例钉住可观察后果：stopAll 之后入队**不产生任何 run**（有界观察窗，
// 无时序余量——幽灵单若被消费，10ms 内必见 run 行）。
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
