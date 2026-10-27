// MCP 工具注册表（core 能力接缝）：除单例身份外全部用局部注册表，避免跨用例
// 串味；单例用例只注册唯一名字并在尾部注销。
#include "doctest.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/mcp_tool.h"

namespace {

class FakeMcpTool : public core::mcp::IMcpTool {
public:
    explicit FakeMcpTool(std::string name,
                         std::string schema = R"({"type":"object"})")
        : name_(std::move(name)), schema_(std::move(schema)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake mcp tool"; }
    std::string inputSchemaJson() const override { return schema_; }
    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        lastArgs = argumentsJson;
        calls.fetch_add(1);
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, "ok"});
    }

    std::string lastArgs;
    std::atomic<int> calls{0};

private:
    std::string name_;
    std::string schema_;
};

} // namespace

REGISTER_TEST(mcp_tool_registry_register_find, "mcp tool registry: 注册后可按名 find 到同一实例（size=1）", {
    core::mcp::McpToolRegistry registry;
    auto tool = std::make_shared<FakeMcpTool>("demo_tool");
    DOCTEST_REQUIRE(registry.registerTool(tool).isOk());
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{1});
    auto found = registry.find("demo_tool");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK(found.value() == tool);
    return true;
})

REGISTER_TEST(mcp_tool_registry_rejects_null_and_bad_names,
              "mcp tool registry: 空指针/空名/超长名/非法字符名一律 ValidationError", {
    core::mcp::McpToolRegistry registry;

    auto nullResult = registry.registerTool(nullptr);
    DOCTEST_CHECK(nullResult.isFail());
    DOCTEST_CHECK(nullResult.errorCode() == core::ErrorCode::ValidationError);

    // 空名
    auto emptyNamed = std::make_shared<FakeMcpTool>("");
    auto emptyResult = registry.registerTool(emptyNamed);
    DOCTEST_CHECK(emptyResult.isFail());
    DOCTEST_CHECK(emptyResult.errorCode() == core::ErrorCode::ValidationError);

    // 128 合法、129 非法（边界）
    auto maxNamed = std::make_shared<FakeMcpTool>(std::string(128, 'a'));
    DOCTEST_CHECK(registry.registerTool(maxNamed).isOk());
    registry.unregisterTool(maxNamed->name());
    auto overNamed = std::make_shared<FakeMcpTool>(std::string(129, 'a'));
    auto overResult = registry.registerTool(overNamed);
    DOCTEST_CHECK(overResult.isFail());
    DOCTEST_CHECK(overResult.errorCode() == core::ErrorCode::ValidationError);

    // 字符集：空格 / 斜杠 / 中文都不允许
    const std::vector<std::string> illegalNames = {"has space", "path/name",
                                                   "中文名"};
    for (const std::string& name : illegalNames) {
        auto bad = std::make_shared<FakeMcpTool>(name);
        auto result = registry.registerTool(bad);
        DOCTEST_CHECK(result.isFail());
        DOCTEST_CHECK(result.errorCode() == core::ErrorCode::ValidationError);
    }
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{0});
    return true;
})

REGISTER_TEST(mcp_tool_registry_duplicate_name_rejected,
              "mcp tool registry: 重名注册失败（含同一实例二次注册），不静默覆盖", {
    core::mcp::McpToolRegistry registry;
    auto first = std::make_shared<FakeMcpTool>("dup_tool");
    auto second = std::make_shared<FakeMcpTool>("dup_tool");
    DOCTEST_REQUIRE(registry.registerTool(first).isOk());
    auto conflict = registry.registerTool(second);
    DOCTEST_CHECK(conflict.isFail());
    DOCTEST_CHECK(conflict.errorCode() == core::ErrorCode::ValidationError);
    // 同一 shared_ptr 重复注册也按重名拒绝（本注册表契约：不是 no-op）
    auto same = registry.registerTool(first);
    DOCTEST_CHECK(same.isFail());
    DOCTEST_CHECK(same.errorCode() == core::ErrorCode::ValidationError);
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{1});
    return true;
})

REGISTER_TEST(mcp_tool_registry_find_missing_not_found,
              "mcp tool registry: find 未注册名字返回 NotFound", {
    core::mcp::McpToolRegistry registry;
    auto missing = registry.find("no_such_tool");
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK(missing.errorCode() == core::ErrorCode::NotFound);
    return true;
})

REGISTER_TEST(mcp_tool_registry_list_sorted_and_unregister,
              "mcp tool registry: list 按 name 字典序，unregister 后 size 收缩", {
    core::mcp::McpToolRegistry registry;
    DOCTEST_REQUIRE(registry.registerTool(std::make_shared<FakeMcpTool>("zeta_tool")).isOk());
    DOCTEST_REQUIRE(registry.registerTool(std::make_shared<FakeMcpTool>("alpha_tool")).isOk());
    DOCTEST_REQUIRE(registry.registerTool(std::make_shared<FakeMcpTool>("mid_tool")).isOk());

    auto tools = registry.list();
    DOCTEST_REQUIRE_EQ(tools.size(), std::size_t{3});
    DOCTEST_CHECK_EQ(tools[0]->name(), std::string("alpha_tool"));
    DOCTEST_CHECK_EQ(tools[1]->name(), std::string("mid_tool"));
    DOCTEST_CHECK_EQ(tools[2]->name(), std::string("zeta_tool"));

    registry.unregisterTool("mid_tool");
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{2});
    DOCTEST_CHECK(registry.find("mid_tool").isFail());
    return true;
})

REGISTER_TEST(mcp_tool_registry_keepalive_after_unregister,
              "mcp tool registry: find 持有的 shared_ptr 在 unregister 后仍可 execute（不悬垂）", {
    core::mcp::McpToolRegistry registry;
    auto tool = std::make_shared<FakeMcpTool>("keep_tool");
    DOCTEST_REQUIRE(registry.registerTool(tool).isOk());
    auto held = registry.find("keep_tool");
    DOCTEST_REQUIRE(held.isOk());
    registry.unregisterTool("keep_tool");
    DOCTEST_CHECK(registry.find("keep_tool").isFail());
    auto executed = held.value()->execute(R"({"x":1})");
    DOCTEST_REQUIRE(executed.isOk());
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);
    DOCTEST_CHECK_EQ(tool->lastArgs, std::string(R"({"x":1})"));
    return true;
})

REGISTER_TEST(mcp_tool_registry_concurrent_unregister_keepalive,
              "mcp tool registry: 并发 execute 期间反复 unregister/register 不崩（keep-alive shared_ptr）", {
    core::mcp::McpToolRegistry registry;
    auto tool = std::make_shared<FakeMcpTool>("race_tool");
    DOCTEST_REQUIRE(registry.registerTool(tool).isOk());
    auto held = registry.find("race_tool");
    DOCTEST_REQUIRE(held.isOk());
    // 先同步执行一次：确保并发线程即使抢不到 CPU 也有执行证据
    DOCTEST_REQUIRE(held.value()->execute("{}").isOk());

    std::atomic<bool> stop{false};
    std::thread caller([&] {
        const auto keep = held.value();  // 线程内独立持有
        while (!stop.load()) {
            auto result = keep->execute("{}");
            if (result.isFail()) break;
        }
    });
    bool reRegisterOk = true;
    for (int i = 0; i < 200; ++i) {
        registry.unregisterTool("race_tool");
        if (registry.registerTool(tool).isFail()) {
            reRegisterOk = false;
            break;
        }
    }
    // 必须先 join 再断言：断言失败会 fail-fast 返回，joinable 线程析构即 terminate
    stop.store(true);
    caller.join();

    DOCTEST_CHECK(reRegisterOk);
    DOCTEST_CHECK(tool->calls.load() > 0);
    DOCTEST_CHECK(held.value() != nullptr);
    registry.unregisterTool("race_tool");
    return true;
})

REGISTER_TEST(mcp_tool_registry_global_singleton_stable,
              "mcp tool registry: globalMcpTools 是同一进程级实例，可注册/注销", {
    DOCTEST_CHECK(&core::mcp::globalMcpTools() == &core::mcp::globalMcpTools());
    auto& global = core::mcp::globalMcpTools();
    global.unregisterTool("global_probe_tool");
    auto tool = std::make_shared<FakeMcpTool>("global_probe_tool");
    DOCTEST_REQUIRE(global.registerTool(tool).isOk());
    DOCTEST_CHECK(global.find("global_probe_tool").isOk());
    global.unregisterTool("global_probe_tool");
    DOCTEST_CHECK(global.find("global_probe_tool").isFail());
    return true;
})