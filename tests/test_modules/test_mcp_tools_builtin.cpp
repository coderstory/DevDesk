// 内置 MCP 工具（B1 仅 devdesk_server_info）：注册、schema、输出固定五项、
// 注入注册表隔离、无参数可调用。web_fetch_markdown 在 B2 追加。
#include "doctest.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "core/mcp_tool.h"
#include "module_mcp/mcp_protocol.h"
#include "module_mcp/mcp_tools_builtin.h"
#include "platform/http_client.h"
#include "platform/json.h"

namespace {

class ExtraMcpTool : public core::mcp::IMcpTool {
public:
    explicit ExtraMcpTool(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "extra mcp tool"; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }
    core::Result<core::mcp::ToolResult> execute(const std::string&) override {
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, "ok"});
    }

private:
    std::string name_;
};

// 取 server_info 的文本；失败返回空串（断言由调用方做）
std::string serverInfoText(core::mcp::McpToolRegistry& registry,
                           const std::string& arguments) {
    auto found = registry.find("devdesk_server_info");
    if (found.isFail()) return std::string();
    auto result = found.value()->execute(arguments);
    if (result.isFail() || result.value().isError) return std::string();
    return result.value().text;
}

// 本机编译的预期平台名（空 = 不做平台断言）。#if 必须留在 REGISTER_TEST
// 实参之外：宏参数里嵌预处理指令在 -Wpedantic 下不可移植（B1 门禁实测）。
std::string expectedPlatformName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "Darwin";
#else
    return std::string();
#endif
}

} // namespace

REGISTER_TEST(mcp_tools_builtin_server_info_registered,
              "mcp tools builtin: registerBuiltinMcpTools 注册 devdesk_server_info 且 schema 合法", {
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry);
    // B2 起 registerBuiltinMcpTools 注册两个内置工具(spec §6);计数断言经主会话裁决对齐
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{2});

    auto found = registry.find("devdesk_server_info");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK_EQ(found.value()->name(), std::string("devdesk_server_info"));

    auto schema = platform::json::parse(found.value()->inputSchemaJson());
    DOCTEST_REQUIRE(schema.isOk());
    DOCTEST_REQUIRE(schema.value().isObject());
    const auto schemaObj = schema.value().asObject();
    const auto type = schemaObj.get("type");
    DOCTEST_REQUIRE(type.isString());
    DOCTEST_CHECK_EQ(type.asString(), std::string("object"));
    const auto properties = schemaObj.get("properties");
    DOCTEST_REQUIRE(properties.isObject());
    DOCTEST_CHECK(properties.asObject().keys().empty());
    return true;
})

REGISTER_TEST(mcp_tools_builtin_server_info_text_fields,
              "mcp tools builtin: server_info 文本含版本/构建类型/平台/两协议版本/工具数与名字典序", {
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry);

    const std::string text = serverInfoText(registry, "{}");
    DOCTEST_REQUIRE(!text.empty());
    DOCTEST_CHECK(text.find(std::string("DevDesk 版本：") + DEVDESK_VERSION) !=
                  std::string::npos);
    DOCTEST_CHECK(text.find("构建类型：") != std::string::npos);
    // 构建类型由 $<CONFIG> 注入，本构建下不该落到 unknown 回退
    DOCTEST_CHECK(text.find("构建类型：unknown") == std::string::npos);
    DOCTEST_CHECK(text.find("平台：") != std::string::npos);
    const std::string platformName = expectedPlatformName();
    if (!platformName.empty()) {
        DOCTEST_CHECK(text.find("平台：" + platformName) != std::string::npos);
    }
    DOCTEST_CHECK(text.find(module::mcp::kMcpProtocolVersion) !=
                  std::string::npos);
    DOCTEST_CHECK(text.find(module::mcp::kMcpProtocolVersionLegacy) !=
                  std::string::npos);
    DOCTEST_CHECK(text.find("已注册工具：2 个") != std::string::npos);
    DOCTEST_CHECK(text.find("web_fetch_markdown") != std::string::npos);
    DOCTEST_CHECK(text.find("devdesk_server_info") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_tools_builtin_server_info_uses_injected_registry,
              "mcp tools builtin: 工具清单来自构造注入的注册表（局部表互不串味，不读全局）", {
    core::mcp::McpToolRegistry localA;
    module::mcp::registerBuiltinMcpTools(localA);
    DOCTEST_REQUIRE(
        localA.registerTool(std::make_shared<ExtraMcpTool>("zeta_tool")).isOk());

    const std::string textA = serverInfoText(localA, "{}");
    DOCTEST_REQUIRE(!textA.empty());
    DOCTEST_CHECK(textA.find("已注册工具：3 个") != std::string::npos);
    DOCTEST_CHECK(textA.find("devdesk_server_info, web_fetch_markdown, zeta_tool") !=
                  std::string::npos);

    core::mcp::McpToolRegistry localB;
    module::mcp::registerBuiltinMcpTools(localB);
    const std::string textB = serverInfoText(localB, "{}");
    DOCTEST_REQUIRE(!textB.empty());
    DOCTEST_CHECK(textB.find("已注册工具：2 个") != std::string::npos);
    DOCTEST_CHECK(textB.find("zeta_tool") == std::string::npos);

    // 局部注册表的工具不能出现在全局表（串味防线）
    DOCTEST_CHECK(core::mcp::globalMcpTools().find("zeta_tool").isFail());
    return true;
})

REGISTER_TEST(mcp_tools_builtin_server_info_no_args,
              "mcp tools builtin: server_info 无参数可调用（{} / 空串 / null 均可）", {
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry);
    auto found = registry.find("devdesk_server_info");
    DOCTEST_REQUIRE(found.isOk());

    const char* args[] = {"{}", "", "null"};
    for (const char* argument : args) {
        auto result = found.value()->execute(argument);
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK_FALSE(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("DevDesk 版本：") !=
                      std::string::npos);
    }
    return true;
})

REGISTER_TEST(mcp_tools_builtin_fetcher_injection_signature,
              "mcp tools builtin: 可注入 Fetcher（B1 不抓取，注册不受影响）", {
    core::mcp::McpToolRegistry registry;
    bool fetcherCalled = false;
    module::mcp::Fetcher fetcher =
        [&fetcherCalled](const platform::http::Request&)
        -> core::Result<platform::http::Response> {
        fetcherCalled = true;
        return core::Result<platform::http::Response>::ok(
            platform::http::Response{});
    };
    module::mcp::registerBuiltinMcpTools(registry, fetcher);
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{2});
    DOCTEST_CHECK(registry.find("devdesk_server_info").isOk());
    // B1 只有 server_info：注册与调用都不触发 fetcher
    const std::string text = serverInfoText(registry, "{}");
    DOCTEST_CHECK_FALSE(text.empty());
    DOCTEST_CHECK_FALSE(fetcherCalled);
    return true;
})

REGISTER_TEST(mcp_tools_builtin_fetch_schema_has_proxy_params,
              "mcp tools builtin: web_fetch_markdown schema 含 useProxy/proxy 代理参数(代理参数批)", {
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry);
    auto found = registry.find("web_fetch_markdown");
    DOCTEST_REQUIRE(found.isOk());
    const auto schema = platform::json::parse(found.value()->inputSchemaJson());
    DOCTEST_REQUIRE(schema.isOk());
    DOCTEST_REQUIRE(schema.value().isObject());
    const auto props = schema.value().asObject().get("properties");
    DOCTEST_REQUIRE(props.isObject());
    DOCTEST_CHECK(props.asObject().has("useProxy"));
    DOCTEST_CHECK(props.asObject().has("proxy"));
    const auto useProxy = props.asObject().get("useProxy");
    DOCTEST_REQUIRE(useProxy.isObject());
    DOCTEST_CHECK_EQ(useProxy.asObject().get("type").asString(), std::string("boolean"));
    const auto proxy = props.asObject().get("proxy");
    DOCTEST_REQUIRE(proxy.isObject());
    DOCTEST_CHECK_EQ(proxy.asObject().get("type").asString(), std::string("string"));
    // required 仍只有 url:两参数均可选
    const auto required = schema.value().asObject().get("required");
    DOCTEST_REQUIRE(required.isArray());
    DOCTEST_REQUIRE_EQ(required.size(), std::size_t{1});
    return true;
})