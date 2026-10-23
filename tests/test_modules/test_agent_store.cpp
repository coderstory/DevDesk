// tests/test_modules/test_agent_store.cpp
// AgentStore 落库层用例：任务 → run → 消息 的回写读出、sweep 收悬挂 run、listTasks 列映射、
// startRunExclusive 原子互斥起跑、updateTask/setTaskEnabled/getTask 往返、taskFromRow 行映射、
// listRuns 同秒并列的 tie-break，以及 trigger_config 编解码唯一真源。
// 跨文件前提：
//   - 每个用例经 test_support::TestDb 拿独立临时库（已 migrate 到含 agent 三表的版本），不碰用户真实库。
//   - trigger_config 的编解码只认 module::agent 的 encode/decodeTriggerConfig，UI 与调度器都不得手拼。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "module_agent/agent_time_text.h"
#include "module_agent/trigger_config.h"
#include "core/llm_endpoint.h"
#include "core/tool.h"
#include "platform/json.h"
#include <atomic>
#include <ctime>
#include <string>

namespace {

// 只会数调用次数的 LlmEndpoint：单闸用例断言它一次都没被调用（撞闸不发请求）。
class NeverSendLlm : public core::LlmEndpoint {
public:
    int calls = 0;
    core::Result<core::LlmResponse> send(const std::vector<core::LlmMessage>&,
                                         const std::string&, const std::string&,
                                         const std::string&) override {
        ++calls;
        return core::Result<core::LlmResponse>::fail(core::ErrorCode::Internal,
                                                     "撞闸的 run 不该发请求");
    }
};

} // namespace

REGISTER_TEST(agent_store_task_run_message_roundtrip, "agent_store: 任务→run→消息 回写读出一致", {
    test_support::TestDb tdb("agentstore", "rt");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto runId = store.startRun(taskId.value());
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE(store.appendMessage(runId.value(), 0, "user", R"([{"type":"text","text":"hi"}])", std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(runId.value(), 1, "assistant", R"([{"type":"tool_use","id":"t1"}])", std::nullopt).isOk());
    auto msgs = store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_CHECK_EQ(msgs.value().size(), 2u);
    DOCTEST_CHECK_EQ(msgs.value()[0].role, std::string("user"));
    DOCTEST_CHECK_EQ(msgs.value()[1].seq, 1);
    DOCTEST_REQUIRE(store.finishRun(runId.value(), "completed", 3, 120, 45, std::nullopt).isOk());
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(runs.value()[0].iterations, 3);
    DOCTEST_CHECK(runs.value()[0].endedAt.has_value());
    return true;
})

REGISTER_TEST(agent_store_sweep_stale_runs, "agent_store: sweep 把遗留 running 标 failed", {
    test_support::TestDb tdb("agentstore", "sw");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    auto runId = store.startRun(taskId.value());   // 不 finishRun，模拟崩溃
    DOCTEST_REQUIRE(store.sweepStaleRuns().isOk());
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("failed"));
    DOCTEST_REQUIRE(runs.value()[0].error.has_value());
    DOCTEST_CHECK(runs.value()[0].error->find("悬挂") != std::string::npos);
    return true;
})

// deleteTask 级联：任务 + 两个 run（一终态一悬挂）+ 各自消息一并消失；他任务数据零波及；
// 不存在的 id → NotFound 且三表零变化（事务回滚路径）
REGISTER_TEST(agent_store_delete_task_cascades_runs_messages,
              "agent_store: deleteTask 级联清 runs/messages，NotFound 三表零变化", {
    test_support::TestDb tdb("agentstore", "del");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("待删任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto keepId = store.createTask("保留任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(keepId.isOk());
    auto run1 = store.startRun(taskId.value());
    DOCTEST_REQUIRE(run1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(run1.value(), 0, "user",
                                        R"([{"type":"text","text":"hi"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(
        store.finishRun(run1.value(), "completed", 2, 10, 5, std::nullopt).isOk());
    auto run2 = store.startRun(taskId.value());   // 悬挂 run（不 finish，模拟进行中）
    DOCTEST_REQUIRE(run2.isOk());
    DOCTEST_REQUIRE(store.appendMessage(run2.value(), 0, "assistant",
                                        R"([{"type":"text","text":"x"}])",
                                        std::nullopt).isOk());
    auto keepRun = store.startRun(keepId.value());
    DOCTEST_REQUIRE(keepRun.isOk());

    DOCTEST_REQUIRE(store.deleteTask(taskId.value()).isOk());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK_EQ(tasks.value()[0].id, keepId.value());
    DOCTEST_CHECK(store.getTask(taskId.value()).isFail());
    auto runsGone = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runsGone.isOk());
    DOCTEST_CHECK(runsGone.value().empty());
    auto msgsGone = store.listMessages(run1.value());
    DOCTEST_REQUIRE(msgsGone.isOk());
    DOCTEST_CHECK(msgsGone.value().empty());
    // 他任务零波及：保留任务的 run 原样
    auto keepRuns = store.listRuns(keepId.value(), 10);
    DOCTEST_REQUIRE(keepRuns.isOk());
    DOCTEST_CHECK_EQ(keepRuns.value().size(), 1u);
    DOCTEST_CHECK_EQ(keepRuns.value()[0].id, keepRun.value());

    auto missing = store.deleteTask(999999);
    DOCTEST_REQUIRE(missing.isFail());
    DOCTEST_CHECK(missing.errorCode() == core::ErrorCode::NotFound);
    auto tasksAfter = store.listTasks();
    DOCTEST_REQUIRE(tasksAfter.isOk());
    DOCTEST_CHECK_EQ(tasksAfter.value().size(), 1u);
    return true;
})

// listTasks 的 9 列逐列还原：列名与 SELECT 拼写漂移是编译期看不见的错
// （只在运行期表现为 Result::fail(NotFound)），任务列表 UI 直接吃它。
// 入参取有区分度的值（modelSlot=sonnet / maxIterations=7 / allowDestructive=true），
// 与默认列（enabled=1、trigger_config='' 由 createTask 固定写空）各占一条。
REGISTER_TEST(agent_store_list_tasks_maps_columns, "agent_store: listTasks 列映射与值还原", {
    test_support::TestDb tdb("agentstore", "lt");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成",
                                   "manual", "sonnet", 7, true);
    DOCTEST_REQUIRE(taskId.isOk());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK_EQ(tasks.value()[0].id, taskId.value());
    DOCTEST_CHECK_EQ(tasks.value()[0].name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(tasks.value()[0].goal, std::string("把逾期 todo 全部完成"));
    DOCTEST_CHECK_EQ(tasks.value()[0].triggerType, std::string("manual"));
    DOCTEST_CHECK_EQ(tasks.value()[0].triggerConfig, std::string(""));
    DOCTEST_CHECK_EQ(tasks.value()[0].modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(tasks.value()[0].maxIterations, 7);
    DOCTEST_CHECK(tasks.value()[0].allowDestructive);
    DOCTEST_CHECK(tasks.value()[0].enabled);
    return true;
})

// 同 task 并发起跑的唯一闸。手动「启动」与 Scheduler/cron 派发都经 AgentLoop::run → 此处；
// 查+插在同一把 db_.mutex() 内 = 原子。终态（非 running）后必须能再次起跑——
// 闸是「同 task 不并发」，不是「只跑一次」。
REGISTER_TEST(agent_store_start_run_exclusive_rejects_concurrent,
              "agent_store: startRunExclusive 已有 running run → fail(AlreadyExists)", {
    test_support::TestDb tdb("agentstore", "exclusive");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "schedule", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto first = store.startRunExclusive(taskId.value());
    DOCTEST_REQUIRE(first.isOk());
    auto second = store.startRunExclusive(taskId.value());   // 并发第二次
    DOCTEST_REQUIRE(second.isFail());
    DOCTEST_CHECK(second.errorCode() == core::ErrorCode::AlreadyExists);
    // 拒的那次没有落行：第二路从未起跑，无悬挂 running 要收尾
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    // 终态后可再次起跑
    DOCTEST_REQUIRE(store.finishRun(first.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    DOCTEST_CHECK(store.startRunExclusive(taskId.value()).isOk());
    return true;
})

// updateTask 的七列覆盖（enabled 归 setTaskEnabled，updateTask 不碰）+ getTask
// 单查往返 + 不存在 id → fail(NotFound)。
REGISTER_TEST(agent_store_update_enable_get_roundtrip,
              "agent_store: updateTask/setTaskEnabled/getTask 往返 + createTask 带触发配置", {
    test_support::TestDb tdb("agentstore", "update");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("旧名", "旧目标", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(store.updateTask(taskId.value(), "整理逾期", "新目标", "schedule",
                                     R"({"cron":"0 9 * * 1-5"})", "sonnet", 20, true)
                        .isOk());
    auto got = store.getTask(taskId.value());
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(got.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(got.value().triggerConfig, std::string(R"({"cron":"0 9 * * 1-5"})"));
    DOCTEST_CHECK_EQ(got.value().modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(got.value().maxIterations, 20);
    DOCTEST_CHECK(got.value().allowDestructive);
    DOCTEST_CHECK(got.value().enabled);   // updateTask 不碰 enabled，见块首
    DOCTEST_REQUIRE(store.setTaskEnabled(taskId.value(), false).isOk());
    auto off = store.getTask(taskId.value());
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_CHECK_FALSE(off.value().enabled);
    DOCTEST_CHECK(store.getTask(99999).errorCode() == core::ErrorCode::NotFound);
    // 一类一规则：UPDATE 目标行不存在 → fail(NotFound)，不静默 no-op
    //（0 行变更判定，与 getTask 同口径）
    DOCTEST_CHECK(store.updateTask(99999, "n", "g", "manual", "", "default", 1, false)
                      .errorCode() == core::ErrorCode::NotFound);
    DOCTEST_CHECK(store.setTaskEnabled(99999, false).errorCode() == core::ErrorCode::NotFound);
    return true;
})

// createTask 的新尾参 triggerConfig 必须真落库：只加形参不绑库 = 静默丢配置，
// cron 任务会拿到空表达式。6 参调用点仍绑空串（由 listTasks 用例钉住）。
// taskFromRow 是 store 行 → AgentTask 的唯一映射（module / scheduler 共用），
// 逐字段搬运 id/name/goal/modelSlot/maxIterations/allowDestructive。
REGISTER_TEST(agent_store_create_task_trigger_config_and_task_from_row,
              "agent_store: createTask 带触发配置落库 + taskFromRow 逐字段搬运", {
    test_support::TestDb tdb("agentstore", "fromrow");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成", "schedule",
                                   "sonnet", 7, true, R"({"cron":"*/5 * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().triggerConfig, std::string(R"({"cron":"*/5 * * * *"})"));
    const module::agent::AgentTask t = module::agent::taskFromRow(row.value());
    DOCTEST_CHECK_EQ(t.id, taskId.value());
    DOCTEST_CHECK_EQ(t.name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(t.goal, std::string("把逾期 todo 全部完成"));
    DOCTEST_CHECK_EQ(t.modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(t.maxIterations, 7);
    DOCTEST_CHECK(t.allowDestructive);
    return true;
})

// 单闸的 loop 层钉子：task 已有 running 行时，AgentLoop::run 必须走 startRunExclusive
// 撞闸——原样上抛 fail(AlreadyExists)、不落行、不发请求（第二路从未起跑，库里不留
// 第二行 running）。若改回非互斥的 startRun，本用例三条断言全红。
REGISTER_TEST(agent_loop_gate_rejects_when_task_already_running,
              "agent_loop: 已有 running run → run() 撞闸 fail(AlreadyExists) 且不落行不发请求", {
    test_support::TestDb tdb("agentstore", "gate");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto held = store.startRunExclusive(taskId.value());   // 假装他路在跑
    DOCTEST_REQUIRE(held.isOk());
    NeverSendLlm llm;
    module::agent::AgentLoop loop{llm, core::ToolRegistry::instance(), store};
    module::agent::AgentTask task;
    task.id = taskId.value(); task.goal = "g"; task.modelSlot = "default";
    std::atomic<bool> cancel{false};
    auto r = loop.run(task, cancel);
    DOCTEST_REQUIRE(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::AlreadyExists);
    DOCTEST_CHECK_EQ(llm.calls, 0);                       // 撞闸不发请求
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);          // 也没多落一行
    DOCTEST_CHECK_EQ(runs.value()[0].id, held.value());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("running"));
    return true;
})

// 并列 tie-break：agent_runs.started_at 是秒精度，同一秒内连起两次 run 会并列——
// 只按 started_at DESC 时并列段顺序未定义，LIMIT 窗口本身就可能把较旧那条当「最近一次」
// 返回（消费方：状态列 / runlist[0] / 默认选中 run）。用例用原生 SQL 把两条 run 的
// started_at 钉成同值，直接钉住 tie-break。
REGISTER_TEST(agent_store_list_runs_tie_breaks_by_id,
              "agent_store: 同秒两次 run → listRuns 取最新（含 LIMIT 1 窗口）", {
    test_support::TestDb tdb("agentstore", "runorder");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto older = store.startRun(taskId.value());
    DOCTEST_REQUIRE(older.isOk());
    DOCTEST_REQUIRE(store.finishRun(older.value(), "limited", 7, int64_t(190000),
                                    int64_t(15000), std::string("limit")).isOk());
    auto newer = store.startRun(taskId.value());
    DOCTEST_REQUIRE(newer.isOk());
    DOCTEST_REQUIRE(store.finishRun(newer.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    // 把两条钉成同一秒（不依赖用例是否跨秒，杜绝负载敏感的假红/假绿）
    DOCTEST_REQUIRE(tdb.handle()
                        .exec("UPDATE agent_runs SET started_at = 1000")
                        .isOk());
    auto all = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(all.value()[0].startedAt, all.value()[1].startedAt);  // 并列前提
    DOCTEST_CHECK_EQ(all.value()[0].id, newer.value());   // 并列时最新在前
    DOCTEST_CHECK_EQ(all.value()[1].id, older.value());
    // LIMIT 1 窗口也必须返回较新那条：并列 + 无 tie-break 时这里会拿到旧 run
    auto latest = store.listRuns(taskId.value(), 1);
    DOCTEST_REQUIRE(latest.isOk());
    DOCTEST_REQUIRE_EQ(latest.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(latest.value()[0].id, newer.value());
    DOCTEST_CHECK_EQ(latest.value()[0].status, std::string("completed"));
    return true;
})

// 任务级 run 聚合：总数含未结束 run、成功率只认 completed、时长负差钳 0（与 runRowText
// 同口径）、全未结束 → lastEndedAt 空、无 run 的任务不在结果里。
// started_at/ended_at 用直接 SQL 钉死——finishRun 写墙钟，钉不住就断言不出确定时长。
REGISTER_TEST(agent_store_list_task_run_summaries,
              "agent_store: listTaskRunSummaries 任务级聚合（总数/成功/总时长/最近结果）", {
    test_support::TestDb tdb("agentstore", "summ");
    module::agent::AgentStore store(tdb.handle());
    auto a = store.createTask("A", "g", "manual", "default", 10, false);
    auto b = store.createTask("B", "g", "manual", "default", 10, false);
    auto c = store.createTask("C", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(a.isOk() && b.isOk() && c.isOk());
    // 返 bool：doctest REQUIRE 宏含 return，不能放进 lambda 内（-Wreturn-type）
    auto pin = [&](int64_t runId, int64_t started,
                   std::optional<int64_t> ended) -> bool {
        std::string sql = "UPDATE agent_runs SET started_at=" +
                          std::to_string(started) + ", ended_at=" +
                          (ended ? std::to_string(*ended) : std::string("NULL")) +
                          " WHERE id=" + std::to_string(runId);
        return tdb.handle().exec(sql).isOk();
    };
    // A：completed 60s + failed（ended<started，钳 0s）+ running（无 ended_at）
    auto a1 = store.startRun(a.value());
    DOCTEST_REQUIRE(a1.isOk());
    DOCTEST_REQUIRE(store.finishRun(a1.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    pin(a1.value(), 1000, int64_t(1060));
    auto a2 = store.startRun(a.value());
    DOCTEST_REQUIRE(a2.isOk());
    DOCTEST_REQUIRE(store.finishRun(a2.value(), "failed", 2, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    pin(a2.value(), 1100, int64_t(1090));   // 时钟回拨
    auto a3 = store.startRun(a.value());
    DOCTEST_REQUIRE(a3.isOk());
    pin(a3.value(), 1200, std::nullopt);    // 还在跑
    // B：一条 completed 60s
    auto b1 = store.startRun(b.value());
    DOCTEST_REQUIRE(b1.isOk());
    DOCTEST_REQUIRE(store.finishRun(b1.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    pin(b1.value(), 2000, int64_t(2060));
    // C：无 run

    // 窗口起点取真实 7d：钉死的 1970 年 run 全部落窗外，all-time 字段不变、
    // 7d 三字段必须为 0（7d 口径见 agent_store_run_summaries_7d 用例）
    const int64_t windowStart = static_cast<int64_t>(
        module::agent::sevenDayWindowStart(std::time(nullptr)));
    auto sums = store.listTaskRunSummaries(windowStart);
    DOCTEST_REQUIRE(sums.isOk());
    DOCTEST_REQUIRE_EQ(sums.value().size(), 2u);   // C 无 run 不在结果里
    const module::agent::AgentTaskRunSummary* sa = nullptr;
    const module::agent::AgentTaskRunSummary* sb = nullptr;
    for (const auto& s : sums.value()) {
        if (s.taskId == a.value()) sa = &s;
        if (s.taskId == b.value()) sb = &s;
    }
    DOCTEST_REQUIRE_MESSAGE(sa != nullptr && sb != nullptr, "聚合行缺任务 A/B");
    DOCTEST_CHECK_EQ(sa->totalRuns, 3);
    DOCTEST_CHECK_EQ(sa->okRuns, 1);
    DOCTEST_CHECK_EQ(sa->totalSeconds, 60);        // 60 + 回拨钳 0 + running 0
    DOCTEST_REQUIRE(sa->lastEndedAt.has_value());
    DOCTEST_CHECK_EQ(*sa->lastEndedAt, 1090);      // MAX 忽略 NULL（running）；回拨 run 的 ended_at 仍参与 MAX
    DOCTEST_CHECK_EQ(sb->totalRuns, 1);
    DOCTEST_CHECK_EQ(sb->okRuns, 1);
    DOCTEST_CHECK_EQ(sb->totalSeconds, 60);
    DOCTEST_CHECK_EQ(*sb->lastEndedAt, 2060);
    // 窗外（1970）run 不进 7d 计数
    DOCTEST_CHECK_EQ(sa->okRuns7d, 0);
    DOCTEST_CHECK_EQ(sa->failedRuns7d, 0);
    DOCTEST_CHECK_EQ(sa->limitedRuns7d, 0);
    DOCTEST_CHECK_EQ(sb->okRuns7d, 0);
    return true;
})

// 7d 窗口口径（spec §8.2/§12.5）：成功=completed、失败=failed+limited（limited
// 另给子计数）、cancelled/running 不计入；8 天前的 completed 落窗外。
// 夹具一律相对当前时间（now-1h/2h/1d/8d）——1970 钉死的旧夹具在 7d 窗口下全落空。
REGISTER_TEST(agent_store_run_summaries_7d,
              "agent_store: listTaskRunSummaries 7d 窗口（completed/failed/limited 计数、8天前排除）", {
    test_support::TestDb tdb("agentstore", "summ7d");
    module::agent::AgentStore store(tdb.handle());
    auto a = store.createTask("A", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(a.isOk());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto pin = [&](int64_t runId, int64_t started, int64_t ended) -> bool {
        return tdb.handle()
            .exec("UPDATE agent_runs SET started_at=" + std::to_string(started) +
                  ", ended_at=" + std::to_string(ended) +
                  " WHERE id=" + std::to_string(runId))
            .isOk();
    };
    struct Seed { const char* status; int64_t started; int64_t ended; };
    const Seed seeds[] = {
        {"completed", now - 3600, now - 3500},        // 1h 前：成功
        {"limited", now - 2 * 3600, now - 3400},      // 2h 前：受限（失败且子计数）
        {"failed", now - 86400, now - 86000},         // 1 天前：失败
        {"cancelled", now - 2 * 86400, now - 172000}, // 2 天前：cancelled 不计入
        {"completed", now - 8 * 86400, now - 863000}, // 8 天前：窗外
    };
    for (const auto& s : seeds) {
        auto r = store.startRun(a.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(
            store.finishRun(r.value(), s.status, 1, std::nullopt, std::nullopt,
                            std::nullopt)
                .isOk());
        DOCTEST_REQUIRE(pin(r.value(), s.started, s.ended));
    }
    const int64_t windowStart = static_cast<int64_t>(
        module::agent::sevenDayWindowStart(static_cast<std::time_t>(now)));
    auto sums = store.listTaskRunSummaries(windowStart);
    DOCTEST_REQUIRE(sums.isOk());
    DOCTEST_REQUIRE_EQ(sums.value().size(), 1u);
    const auto& s = sums.value()[0];
    DOCTEST_CHECK_EQ(s.taskId, a.value());
    DOCTEST_CHECK_EQ(s.okRuns7d, 1);
    DOCTEST_CHECK_EQ(s.failedRuns7d, 2);
    DOCTEST_CHECK_EQ(s.limitedRuns7d, 1);
    // all-time 字段不受窗口影响（cancelled 也进 total）
    DOCTEST_CHECK_EQ(s.totalRuns, 5);
    DOCTEST_CHECK_EQ(s.okRuns, 2);
    return true;
})

// 消息时间戳（spec §8.3）：appendMessage 已写 created_at，SELECT 补列后必须读回。
// UPDATE 把列钉成确定值——appendMessage 写的是墙钟，钉不住就没法断言。
REGISTER_TEST(agent_store_list_messages_created_at,
              "agent_store: listMessages 读回 created_at（钉值 12345）", {
    test_support::TestDb tdb("agentstore", "msgtime");
    module::agent::AgentStore store(tdb.handle());
    auto t = store.createTask("t", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(t.isOk());
    auto run = store.startRun(t.value());
    DOCTEST_REQUIRE(run.isOk());
    DOCTEST_REQUIRE(store.appendMessage(run.value(), 0, "user",
                                        R"([{"type":"text","text":"你好"}])",
                                        std::nullopt)
                        .isOk());
    DOCTEST_REQUIRE(
        tdb.handle().exec("UPDATE agent_messages SET created_at=12345").isOk());
    auto msgs = store.listMessages(run.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_REQUIRE_EQ(msgs.value().size(), 1u);
    DOCTEST_CHECK_EQ(msgs.value()[0].createdAt, 12345);
    return true;
})

// 本组钉住「store 行 ↔ TriggerConfig 结构」的往返与坏配置的可判定性：
// 编解码散在多处时谁都不能保证自己那份和别处同口径——这里以「encode 出来的字符串经
// createTask 落库、getTask 读出、decode 回来的结构必须逐字段相等」把两端钉在一起。
REGISTER_TEST(agent_store_trigger_config_roundtrip,
              "agent_store: trigger_config 往返（encode → createTask → getTask → decode 逐字段一致）", {
    test_support::TestDb tdb("agentstore", "trigcfg");
    module::agent::AgentStore store(tdb.handle());

    // 常规表达式 + 转义敏感表达式（引号 / 反斜杠 / 制表符——手拼 JSON 时必炸的字符；
    // json 层转义才对）。decode 回来的结构必须逐字相等
    for (const char* expr : {"0 9 * * 1-5", "* * * * *",
                             R"(a"b\c	d)"}) {
        module::agent::TriggerConfig cfg;
        cfg.cron = expr;
        auto encoded = module::agent::encodeTriggerConfig(cfg);
        DOCTEST_REQUIRE(encoded.isOk());
        // 编码产物必须是合法 JSON 对象（不是手拼的近似物）
        auto parsed = platform::json::parse(encoded.value());
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_CHECK(parsed.value().isObject());

        auto taskId = store.createTask("往返", "g", "schedule", "default", 5, false,
                                       encoded.value());
        DOCTEST_REQUIRE(taskId.isOk());
        auto row = store.getTask(taskId.value());
        DOCTEST_REQUIRE(row.isOk());
        DOCTEST_CHECK_EQ(row.value().triggerConfig, encoded.value());

        auto decoded = module::agent::decodeTriggerConfig(row.value().triggerConfig);
        DOCTEST_REQUIRE(decoded.isOk());
        DOCTEST_CHECK_EQ(decoded.value().cron, std::string(expr));
    }
    return true;
})

REGISTER_TEST(agent_store_trigger_config_decode_rejects_malformed,
              "trigger_config decode: 空/坏 JSON/非对象/缺 cron/空 cron → Result fail", {
    using module::agent::decodeTriggerConfig;
    // 空串 = 非 schedule（或未配置）任务的正常态，但对「要 cron 的人」是坏配置
    DOCTEST_CHECK(decodeTriggerConfig("").isFail());
    DOCTEST_CHECK(decodeTriggerConfig("{ not json !!!").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"(["cron"])").isFail());      // 非对象
    DOCTEST_CHECK(decodeTriggerConfig("{}").isFail());               // 缺 cron
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":123})").isFail());  // 类型不符
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":""})").isFail());   // 空表达式
    // 错误信息可读（调度器把它拼进点名 task id 的 WARN）
    auto bad = decodeTriggerConfig(R"({"cron":""})");
    DOCTEST_CHECK(!bad.errorMessage().empty());
    // 正常态：合法对象 + 非空字符串
    auto good = decodeTriggerConfig(R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(good.isOk());
    DOCTEST_CHECK_EQ(good.value().cron, std::string("* * * * *"));
    // 多余键不致命（向后兼容：以后加键，旧读取路径不会因此判坏）
    auto extra = decodeTriggerConfig(R"({"cron":"* * * * *","tz":"UTC"})");
    DOCTEST_REQUIRE(extra.isOk());
    DOCTEST_CHECK_EQ(extra.value().cron, std::string("* * * * *"));
    return true;
})
