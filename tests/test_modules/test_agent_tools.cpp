// tests/test_modules/test_agent_tools.cpp
// agent 工具用例（agent M1 Task 10：app.stats 只读统计；Task 12 的
// AgentModule 用例将共用本文件）。
//
// 错误回喂约定（spec §10 M1 统一）：工具级失败 → Result ok + isError=true +
// 中文说明；Result::fail 只留给编程不变量破坏。
#include "doctest.h"
#include "test_support.h"

#include "core/tool.h"
#include "module_agent/app_stats_tool.h"
#include "platform/json.h"

#include <string>

// ============ app.stats ============

// 直查三表 COUNT(*) 的只读工具：种子数据经原生 SQL 插入（不绕业务服务层，
// 断言的是工具对表行数的真实读数）。注意断言宏不能进 lambda（展开含
// return false，在 lambda 里会只退出 lambda 假装绿），故逐条平铺。
REGISTER_TEST(app_stats_counts_seeded_rows,
              "app.stats: 统计 todo_tasks/markdown_index/agent_tasks 三表行数", {
    test_support::TestDb tdb("agenttools", "stats");

    // 种数据：todo_tasks 2 行 / markdown_index 1 行 / agent_tasks 1 行
    //（todo_columns v2 种子保证 column_id=1 存在；created_at 等必填列给占位值）
    DOCTEST_REQUIRE(
        tdb.handle()
            .exec("INSERT INTO todo_tasks (column_id, title, position, "
                  "created_at, updated_at) VALUES "
                  "(1, 't1', 0, 1, 1), (1, 't2', 1, 1, 1)")
            .isOk());
    DOCTEST_REQUIRE(
        tdb.handle()
            .exec("INSERT INTO markdown_index (path, title, size_bytes, "
                  "modified_at, created_at) VALUES "
                  "('a.md', 'A', 10, 1, 1)")
            .isOk());
    DOCTEST_REQUIRE(
        tdb.handle()
            .exec("INSERT INTO agent_tasks (name, goal, trigger_type, "
                  "created_at, updated_at) VALUES "
                  "('整理逾期', '把逾期 todo 全部完成', 'manual', 1, 1)")
            .isOk());

    auto tool = module::agent::makeAppStatsTool(tdb.handle());
    DOCTEST_REQUIRE(tool != nullptr);
    DOCTEST_CHECK_EQ(tool->name(), std::string("app.stats"));

    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("todos").asInt(), 2);
    DOCTEST_CHECK_EQ(obj.get("notes").asInt(), 1);
    DOCTEST_CHECK_EQ(obj.get("agentTasks").asInt(), 1);
    return true;
})

REGISTER_TEST(app_stats_zero_state_and_schema,
              "app.stats: 空库全 0、schema 可读、只读非破坏性", {
    test_support::TestDb tdb("agenttools", "zero");

    auto tool = module::agent::makeAppStatsTool(tdb.handle());
    auto r = tool->execute(core::ToolArgs{""});  // 空参数也要能跑
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("todos").asInt(), 0);
    DOCTEST_CHECK_EQ(obj.get("notes").asInt(), 0);
    DOCTEST_CHECK_EQ(obj.get("agentTasks").asInt(), 0);

    // schema 抽查 + 只读
    const std::string schema = tool->schemaJson();
    DOCTEST_CHECK(schema.find("\"name\":\"app.stats\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("input_schema") != std::string::npos);
    DOCTEST_CHECK_FALSE(tool->isDestructive());
    return true;
})
