// tests/test_modules/test_agent_preset_batch.cpp
// need 219 预设批新工具的断言级用例：schedule.runs / agent.runs / memory.stats /
// memory.health / mcp.health 各 1 条（临时库/临时目录夹具，不触真机数据）。
// 用例名前缀 agent_preset_（--filter agent 可定向命中）。
#include "doctest.h"
#include "test_support.h"

#include "core/time.h"
#include "core/tool.h"
#include "module_agent/agent_runs_tool.h"
#include "module_agent/agent_store.h"
#include "module_mcp/mcp_health_tool.h"
#include "module_memory/agent_tools.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/proposals/usage_ledger.h"
#include "module_schedule/schedule_runs_tool.h"
#include "module_schedule/schedule_store.h"
#include "platform/json.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

// schedule.runs（F-01）：failed/success 过滤互斥命中，错误与状态字段透出
REGISTER_TEST(agent_preset_schedule_runs_filter,
              "agent preset 工具 schedule.runs：failed 过滤与字段断言", {
    test_support::TestDb tdb("agentpreset", "schedruns");
    module::schedule::ScheduleStore store(tdb.handle());
    auto now = static_cast<int64_t>(core::time::nowUnix());
    module::schedule::RunRecord failedRow;
    failedRow.origin = "user";
    failedRow.originKey = "rule-1";
    failedRow.ruleId = 7;
    failedRow.plannedAt = now - 3600;
    failedRow.status = "failed";
    failedRow.attempt = 2;
    failedRow.error = "boom";
    DOCTEST_REQUIRE(store.insertRun(failedRow).isOk());
    module::schedule::RunRecord okRow;
    okRow.origin = "user";
    okRow.originKey = "rule-2";
    okRow.ruleId = 8;
    okRow.plannedAt = now - 60;
    okRow.status = "success";
    okRow.attempt = 1;
    DOCTEST_REQUIRE(store.insertRun(okRow).isOk());

    auto tool = module::schedule::makeScheduleRunsTool(store);
    auto r = tool->execute(core::ToolArgs{R"({"status":"failed"})"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().isError);
    DOCTEST_CHECK(r.value().contentJson.find("\"failed\"") != std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("boom") != std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("\"success\"") == std::string::npos);
    auto r2 = tool->execute(core::ToolArgs{R"({"status":"success"})"});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().contentJson.find("\"success\"") != std::string::npos);
    DOCTEST_CHECK(r2.value().contentJson.find("boom") == std::string::npos);
    // 输出恒为合法 JSON 数组
    DOCTEST_CHECK(platform::json::parse(r.value().contentJson).isOk());
    return true;
})

// agent.runs（F-02）：缺省排除 running（自指排除），显式 status=running 可见
REGISTER_TEST(agent_preset_agent_runs_exclude_running,
              "agent preset 工具 agent.runs：缺省排除 running + failed/triggerType 过滤", {
    test_support::TestDb tdb("agentpreset", "agentruns");
    module::agent::AgentStore store(tdb.handle());
    auto taskId =
        store.createTask("巡检任务", "目标", "schedule", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto run1 = store.startRunExclusive(taskId.value());
    DOCTEST_REQUIRE(run1.isOk());
    DOCTEST_REQUIRE(store
                        .finishRun(run1.value(), "failed", 1, std::nullopt,
                                   std::nullopt,
                                   std::optional<std::string>("boom"))
                        .isOk());
    auto run2 = store.startRunExclusive(taskId.value());
    DOCTEST_REQUIRE(run2.isOk());

    auto tool = module::agent::makeAgentRunsTool(store);
    auto r = tool->execute(core::ToolArgs{""});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().contentJson.find("boom") != std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("\"running\"") == std::string::npos);
    auto r2 = tool->execute(core::ToolArgs{R"({"status":"running"})"});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().contentJson.find("\"running\"") != std::string::npos);
    auto r3 = tool->execute(core::ToolArgs{R"({"triggerType":"manual"})"});
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK(r3.value().contentJson.find("boom") == std::string::npos);
    return true;
})

// memory.stats（F-03）：提案分组计数 + 账面 token 与存储层直查对齐
REGISTER_TEST(agent_preset_memory_stats_counts,
              "agent preset 工具 memory.stats：分组计数与账面口径断言", {
    auto dir = std::filesystem::temp_directory_path() /
               "devdesk-agent-preset-stats";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    auto now = static_cast<int64_t>(core::time::nowUnix());
    auto prop =
        module::memory::proposals::ProposalStore::open(dir / "proposals.db");
    DOCTEST_REQUIRE(prop.isOk());
    DOCTEST_REQUIRE(prop.value()
                        .create("add", {}, "正文", "text", "", {}, 0.5,
                                "collect", "user_stated", now)
                        .isOk());
    DOCTEST_REQUIRE(prop.value()
                        .create("merge", {}, "正文2", "text", "", {}, 0.6,
                                "dedupe", "assistant_derived", now)
                        .isOk());
    auto led =
        module::memory::proposals::UsageLedger::open(dir / "usage_ledger.db");
    DOCTEST_REQUIRE(led.isOk());
    DOCTEST_REQUIRE(led.value()
                        .record(module::memory::proposals::kKindLlmInput, 1000,
                                now)
                        .isOk());
    DOCTEST_REQUIRE(led.value()
                        .record(module::memory::proposals::kKindEmbed, 500, now)
                        .isOk());

    auto tool = module::memory::makeMemoryStatsTool(dir / "proposals.db",
                                                    dir / "usage_ledger.db");
    auto r = tool->execute(core::ToolArgs{""});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().isError);
    auto v = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(v.isOk());
    auto obj = v.value().asObject();
    auto proposals = obj.get("proposals").asObject();
    DOCTEST_CHECK(proposals.get("pending").asInt() == 2);
    DOCTEST_CHECK(proposals.get("total").asInt() == 2);
    auto usage = obj.get("usage").asObject();
    DOCTEST_CHECK(usage.get("todayTokens").asInt() == 1500);
    DOCTEST_CHECK(obj.get("engine").asObject().get("state").isString());
    std::filesystem::remove_all(dir, ec);
    return true;
})

// memory.health（F-04）：快照 + 探活 shape；引擎未跑时探活给明确原因不挂起
REGISTER_TEST(agent_preset_memory_health_shape,
              "agent preset 工具 memory.health：快照与探活 shape 断言", {
    auto tool = module::memory::makeMemoryHealthTool();
    auto r = tool->execute(core::ToolArgs{""});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().isError);
    auto v = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(v.isOk());
    auto obj = v.value().asObject();
    DOCTEST_CHECK(obj.get("state").isString());
    auto probe = obj.get("probe").asObject();
    DOCTEST_CHECK(probe.get("ok").isBool());
    DOCTEST_CHECK(probe.get("reason").isString());
    return true;
})

// mcp.health（F-05）：stopped 态明确报错（port=0、探活即败）不挂起
REGISTER_TEST(agent_preset_mcp_health_shape,
              "agent preset 工具 mcp.health：状态 shape + stopped 不挂起断言", {
    auto tool = module::mcp::makeMcpHealthTool();
    auto r = tool->execute(core::ToolArgs{""});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().isError);
    auto v = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(v.isOk());
    auto obj = v.value().asObject();
    auto state = obj.get("state").asString();
    DOCTEST_CHECK(state == "stopped" || state == "running" ||
                  state == "failed");
    if (state == "stopped") {
        DOCTEST_CHECK(obj.get("port").asInt() == 0);
        DOCTEST_CHECK(obj.get("probe").asObject().get("ok").asBool() == false);
    }
    DOCTEST_CHECK(obj.get("tools").asInt() >= 0);
    return true;
})
