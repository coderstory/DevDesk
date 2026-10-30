// tests/test_modules/test_project_info_tool.cpp
// 需-11 project_info MCP 工具用例。防的事故：
// - F-02：中文名卡片按名命中，六字段带中文标签回给客户端（Claude Code 直接可读）。
// - F-03：缺 name / 空串 / 非字符串 / 非 JSON 对象 → isError 明确文案，进程不崩
//   （返回的是 ok(Result) 包 isError 业务失败，不是 Result::fail 基建错）。
// - 未知名：错误里附已登记清单（agent 可自纠重试），空库给登记入口指引。
// - 装配契约：注册后 registry 可按名找到；注销后 no-op 不炸。
#include "doctest.h"

#include "core/mcp_tool.h"
#include "module_settings/project_card_store.h"
#include "module_settings/project_info_tool.h"
#include "project_cards_test_util.h"

#include <string>

namespace {

namespace ms = module::settings;

ms::ProjectCard sampleCard() {
    ms::ProjectCard c;
    c.name = "订单中心";
    c.repoPath = "/srv/git/order-center";
    c.startCommand = "make dev";
    c.testCommand = "make test";
    c.deployMethod = "k8s 滚动升级";
    c.testDbUrl = "mysql://order:pwd@test-db:3306/order_center";
    c.keyModules = "order-api / order-worker";
    return c;
}

}  // namespace

REGISTER_TEST(project_info_tool_hits_chinese_card,
              "project_info: 中文名卡片命中（F-02），六字段全量回传", {
    devdesk::ProjectCardsDbEnv env("tool");
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, sampleCard()).isOk());

    ms::ProjectInfoTool tool(env.db.get());
    auto r = tool.execute(R"({"name":"订单中心"})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(!r.value().isError);
    // 标签 + 值都在：客户端拿到的是可直接引用的档案，不是裸行转储
    DOCTEST_CHECK(r.value().text.find("项目卡「订单中心」") != std::string::npos);
    DOCTEST_CHECK(r.value().text.find("仓库路径：/srv/git/order-center") !=
                  std::string::npos);
    DOCTEST_CHECK(r.value().text.find("启动命令：make dev") != std::string::npos);
    DOCTEST_CHECK(r.value().text.find("测试命令：make test") != std::string::npos);
    DOCTEST_CHECK(r.value().text.find("部署方式：k8s 滚动升级") != std::string::npos);
    DOCTEST_CHECK(r.value().text.find(
                      "测试库地址：mysql://order:pwd@test-db:3306/order_center") !=
                  std::string::npos);
    DOCTEST_CHECK(r.value().text.find("关键模块：order-api / order-worker") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(project_info_tool_missing_name_is_clear_error,
              "project_info: 缺 name 返回明确错误不崩（F-03）", {
    devdesk::ProjectCardsDbEnv env("toolerr");
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, sampleCard()).isOk());
    ms::ProjectInfoTool tool(env.db.get());

    // 四种坏参形态全是业务级 isError（基建层 Result 恒 ok = 服务不会被 -32603 拖垮）
    for (const char* bad : {"{}",
                            R"({"name":""})",
                            R"({"name":123})",
                            "not-json"}) {
        auto r = tool.execute(bad);
        DOCTEST_CHECK_MESSAGE(r.isOk(), bad);
        if (r.isOk()) {
            DOCTEST_CHECK_MESSAGE(r.value().isError, bad);
            DOCTEST_CHECK_MESSAGE(!r.value().text.empty(), bad);
        }
    }
    // 缺 name 的文案点名参数（对照：命中路径 !isError，防「恒错误」假绿）
    auto missing = tool.execute("{}");
    DOCTEST_REQUIRE(missing.isOk());
    DOCTEST_CHECK(missing.value().isError);
    DOCTEST_CHECK(missing.value().text.find("缺少 name") != std::string::npos);
    return true;
})

REGISTER_TEST(project_info_tool_unknown_name_lists_registered,
              "project_info: 未知名错误附已登记清单；空库给登记指引", {
    devdesk::ProjectCardsDbEnv env("toolmiss");
    ms::ProjectInfoTool tool(env.db.get());

    // 空库：指引去设置页登记
    auto empty = tool.execute(R"({"name":"不存在"})");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK(empty.value().isError);
    DOCTEST_CHECK(empty.value().text.find("未找到") != std::string::npos);
    DOCTEST_CHECK(empty.value().text.find("登记") != std::string::npos);

    // 有卡：错误里带可重试的名字清单
    DOCTEST_CHECK(ms::upsertProjectCard(*env.db, sampleCard()).isOk());
    auto miss = tool.execute(R"({"name":"不存在"})");
    DOCTEST_REQUIRE(miss.isOk());
    DOCTEST_CHECK(miss.value().isError);
    DOCTEST_CHECK(miss.value().text.find("未找到") != std::string::npos);
    DOCTEST_CHECK(miss.value().text.find("订单中心") != std::string::npos);
    return true;
})

REGISTER_TEST(project_info_tool_null_db_reports_not_crash,
              "project_info: 库未就绪（nullptr）明确报错不崩", {
    ms::ProjectInfoTool tool(nullptr);
    auto r = tool.execute(R"({"name":"任意"})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().isError);
    DOCTEST_CHECK(r.value().text.find("数据库未就绪") != std::string::npos);
    return true;
})

REGISTER_TEST(project_info_tool_registry_wiring,
              "project_info: 注册/注销接线（按名可查、注销后 no-op）", {
    devdesk::ProjectCardsDbEnv env("toolreg");
    core::mcp::McpToolRegistry registry;
    auto registered = ms::registerProjectInfoTool(registry, env.db.get());
    DOCTEST_REQUIRE(registered.isOk());
    auto found = registry.find(ms::kProjectInfoToolName);
    DOCTEST_CHECK(found.isOk());
    DOCTEST_CHECK_EQ(found.value()->name(), std::string("project_info"));
    // 注册表名全小写下划线（MCP 内置工具命名约束）
    DOCTEST_CHECK(ms::ProjectInfoTool(env.db.get()).inputSchemaJson().find(
                      "\"required\":[\"name\"]") != std::string::npos);

    ms::unregisterProjectInfoTool(registry);
    DOCTEST_CHECK(registry.find(ms::kProjectInfoToolName).isFail());
    // 重复注销 = no-op 不炸（unregisterTool 既有语义）
    ms::unregisterProjectInfoTool(registry);
    return true;
})
