// tests/test_modules/test_agent_store.cpp
// AgentStore 落库层用例（agent M1 Task 3）：任务→run→消息 回写读出一致 +
// sweep 把遗留 running 标 failed。TestDb 已 migrate 到 v12（含 agent 三表）。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_store.h"

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
