// tests/test_core/test_tool_registry.cpp
// ToolRegistry：注册/冲突/注销/find keep-alive/schema 拼装（spec §2.1）
#include "doctest.h"
#include "core/tool.h"

#include <utility>

namespace {
class FakeTool : public core::ITool {
public:
    FakeTool(std::string n, std::string schema) : n_(std::move(n)), s_(std::move(schema)) {}
    std::string name() const override { return n_; }
    std::string schemaJson() const override { return s_; }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        ++calls;
        return core::Result<core::ToolResult>::ok(core::ToolResult{false, "\"ok\""});
    }
    int calls = 0;
private:
    std::string n_, s_;
};
} // namespace

REGISTER_TEST(tool_registry_register_find, "tool: 注册后可 find，取到同一实例", {
    auto& reg = core::ToolRegistry::instance();
    auto t = std::make_shared<FakeTool>("t.a", R"({"name":"t.a"})");
    DOCTEST_REQUIRE(reg.registerTool(t).isOk());
    auto found = reg.find("t.a");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK(found.value() == t);
    reg.unregisterTool("t.a");
    DOCTEST_CHECK(reg.find("t.a").isFail());
    return true;
})

REGISTER_TEST(tool_registry_duplicate_name_rejected, "tool: 同名注册冲突拒（ValidationError）", {
    auto& reg = core::ToolRegistry::instance();
    auto a = std::make_shared<FakeTool>("t.dup", "{}");
    auto b = std::make_shared<FakeTool>("t.dup", "{}");
    DOCTEST_REQUIRE(reg.registerTool(a).isOk());
    auto r = reg.registerTool(b);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
    reg.unregisterTool("t.dup");
    return true;
})

REGISTER_TEST(tool_registry_shared_ptr_keepalive, "tool: find 持有 shared_ptr 期间 unregister 不悬垂，仍可 execute", {
    auto& reg = core::ToolRegistry::instance();
    auto t = std::make_shared<FakeTool>("t.keep", "{}");
    DOCTEST_REQUIRE(reg.registerTool(t).isOk());
    auto held = reg.find("t.keep");
    DOCTEST_REQUIRE(held.isOk());
    reg.unregisterTool("t.keep");   // 模拟模块运行中被拔
    auto r = held.value()->execute(core::ToolArgs{"{}"});   // 仍安全
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(t->calls, 1);
    return true;
})

REGISTER_TEST(tool_registry_all_schemas_json, "tool: allSchemasJson 拼数组，空表 = []", {
    auto& reg = core::ToolRegistry::instance();
    reg.unregisterTool("t.s1");
    reg.unregisterTool("t.s2");
    DOCTEST_CHECK_EQ(reg.allSchemasJson(), std::string("[]"));
    auto a = std::make_shared<FakeTool>("t.s1", R"({"name":"t.s1"})");
    auto b = std::make_shared<FakeTool>("t.s2", R"({"name":"t.s2"})");
    DOCTEST_REQUIRE(reg.registerTool(a).isOk());
    DOCTEST_REQUIRE(reg.registerTool(b).isOk());
    DOCTEST_CHECK_EQ(reg.allSchemasJson(),
                     std::string(R"([{"name":"t.s1"},{"name":"t.s2"}])"));
    reg.unregisterTool("t.s1");
    reg.unregisterTool("t.s2");
    return true;
})
