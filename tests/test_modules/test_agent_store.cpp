// tests/test_modules/test_agent_store.cpp
// AgentStore 落库层用例（agent M1 Task 3）：任务→run→消息 回写读出一致 +
// sweep 把遗留 running 标 failed。TestDb 已 migrate 到 v12（含 agent 三表）。
// M2 Task 2 追加：原子互斥起跑（startRunExclusive）+ 任务编辑/启停/单查 +
// taskFromRow 唯一映射。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "core/llm_endpoint.h"
#include "core/tool.h"
#include <atomic>

namespace {

// 只会数调用次数的 LlmEndpoint：单闸用例断言它**一次都没被调用**（撞闸不发请求）。
// 放在本文件是因为 Task 2 的提交面 = store 两文件 + loop 两文件 + 本用例文件
// （plan task-2-brief Step 5 pathspec），loop 的单闸契约不能在别处钉。
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

// listTasks 的 9 列逐列还原：列名与 SELECT 拼写漂移是编译期看不见的错
//（只在运行期表现为 Result::fail(NotFound)），Task 12 的任务列表 UI 直接吃它。
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

// ===== M2 Task 2：原子互斥起跑 / 任务编辑 / 单查 / 行映射 =====

// Review Focus 1：同 task 并发起跑的唯一闸。手动「启动」与 Scheduler/cron
// 派发都经 AgentLoop::run → 此处；查+插在同一把 db_.mutex() 内 = 原子。
// 终态（非 running）后必须能再次起跑——闸是「同 task 不并发」，不是「只跑一次」。
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
    // 拒的那次**没有落行**（第二路从未起跑，无悬挂 running 要收尾）
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
    // updateTask 全列覆盖 + triggerConfig 落库
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
    DOCTEST_CHECK(got.value().enabled);          // updateTask 不碰 enabled
    // setTaskEnabled 翻转 + getTask 回读；不存在 id → NotFound
    DOCTEST_REQUIRE(store.setTaskEnabled(taskId.value(), false).isOk());
    auto off = store.getTask(taskId.value());
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_CHECK_FALSE(off.value().enabled);
    DOCTEST_CHECK(store.getTask(99999).errorCode() == core::ErrorCode::NotFound);
    // 一类一规则（评审裁定 a）：UPDATE 目标行不存在 → fail(NotFound)，
    // 不静默 no-op（0 行变更判定，与 getTask 同口径）
    DOCTEST_CHECK(store.updateTask(99999, "n", "g", "manual", "", "default", 1, false)
                      .errorCode() == core::ErrorCode::NotFound);
    DOCTEST_CHECK(store.setTaskEnabled(99999, false).errorCode() == core::ErrorCode::NotFound);
    return true;
})

// createTask 的新尾参 triggerConfig 必须真落库（只加形参不绑库 = 静默丢配置，
// cron 任务会拿到空表达式)；6 参调用点仍绑空串（由既有 listTasks 用例钉住）。
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

// 单闸的 loop 层钉子（Review Focus 1）：task 已有 running 行时，AgentLoop::run
// 必须走 startRunExclusive 撞闸——原样上抛 fail(AlreadyExists)、**不落行、
// 不发请求**（第二路从未起跑，库里不留第二行 running）。
// 这条用例也是 Task 2「起跑换单闸」那行改动的唯一红灯面：改回 startRun 即红。
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
