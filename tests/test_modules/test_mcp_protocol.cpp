// MCP 协议分发的纯逻辑用例（不碰 socket）：判序、版本协商、会话、通知、错误码、
// JSON 转义与 id 回显。传输层（415/403/405）由服务级用例（B3）覆盖。
#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "module_mcp/mcp_protocol.h"
#include "module_mcp/mcp_session_store.h"
#include "platform/json.h"

namespace {

class StubTool : public core::mcp::IMcpTool {
public:
    StubTool(std::string name, std::string text, bool isError = false)
        : name_(std::move(name)), text_(std::move(text)), isError_(isError) {}

    void setDescription(std::string description) {
        description_ = std::move(description);
    }
    void setSchema(std::string schema) { schema_ = std::move(schema); }

    std::string name() const override { return name_; }
    std::string description() const override { return description_; }
    std::string inputSchemaJson() const override { return schema_; }
    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        lastArgs = argumentsJson;
        ++calls;
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{isError_, text_});
    }

    std::string lastArgs;
    int calls = 0;

private:
    std::string name_;
    std::string text_;
    bool isError_ = false;
    std::string description_ = "stub mcp tool";
    std::string schema_ = R"({"type":"object","properties":{}})";
};

class FailingMcpTool : public core::mcp::IMcpTool {
public:
    std::string name() const override { return "fail_tool"; }
    std::string description() const override { return "always fails"; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }
    core::Result<core::mcp::ToolResult> execute(
        const std::string&) override {
        return core::Result<core::mcp::ToolResult>::fail(
            core::ErrorCode::Internal, "boom: 工具内部错误");
    }
};

struct Fixture {
    module::mcp::McpSessionStore sessions;
    core::mcp::McpToolRegistry tools;
    module::mcp::McpProtocol protocol;

    Fixture() : protocol(sessions, tools, "9.9.9-test") {}

    // initialize 并返回新 session id（调用方自行断言非空）
    std::string openSession() {
        std::string id;
        protocol.handlePost(
            R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})",
            "", "", id);
        return id;
    }
};

platform::json::JsonValue parseBody(const std::string& body) {
    auto parsed = platform::json::parse(body);
    if (parsed.isFail()) return platform::json::JsonValue();
    return parsed.value();
}

std::int64_t errorCodeOf(const std::string& body) {
    const auto root = parseBody(body);
    if (!root.isObject()) return 1;
    const auto error = root.asObject().get("error");
    if (!error.isObject()) return 2;
    const auto code = error.asObject().get("code");
    return code.isNumber() ? code.asInt() : 3;
}

std::string strField(const platform::json::JsonObject& obj, const char* key) {
    const auto value = obj.get(key);
    return value.isString() ? value.asString() : std::string();
}

bool boolField(const platform::json::JsonObject& obj, const char* key) {
    const auto value = obj.get(key);
    return value.isBool() ? value.asBool() : false;
}

} // namespace

REGISTER_TEST(mcp_protocol_initialize_version_negotiation,
              "mcp protocol: initialize 版本协商（支持/不支持/缺失/非字符串）", {
    Fixture fixture;
    std::string newId;

    // 支持列表内的版本原样回显
    auto supported = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})",
        "", "", newId);
    DOCTEST_REQUIRE_EQ(supported.status, 200);
    DOCTEST_CHECK_FALSE(supported.notificationOnly);
    DOCTEST_CHECK_FALSE(newId.empty());
    {
        const auto root = parseBody(supported.body);
        DOCTEST_REQUIRE(root.isObject());
        const auto result = root.asObject().get("result");
        DOCTEST_REQUIRE(result.isObject());
        const auto resultObj = result.asObject();
        DOCTEST_CHECK_EQ(strField(resultObj, "protocolVersion"),
                         std::string("2025-03-26"));
        const auto serverInfo = resultObj.get("serverInfo");
        DOCTEST_REQUIRE(serverInfo.isObject());
        DOCTEST_CHECK_EQ(strField(serverInfo.asObject(), "name"),
                         std::string("devdesk"));
        DOCTEST_CHECK_EQ(strField(serverInfo.asObject(), "version"),
                         std::string("9.9.9-test"));
        const auto capabilities = resultObj.get("capabilities");
        DOCTEST_REQUIRE(capabilities.isObject());
        const auto toolsCap = capabilities.asObject().get("tools");
        DOCTEST_REQUIRE(toolsCap.isObject());
        DOCTEST_CHECK_FALSE(boolField(toolsCap.asObject(), "listChanged"));
        DOCTEST_CHECK_FALSE(strField(resultObj, "instructions").empty());
    }

    // 不支持的版本 / 缺失 params / 非字符串 → 回主版本
    const char* fallbackBodies[] = {
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2099-01-01"}})",
        R"({"jsonrpc":"2.0","id":3,"method":"initialize"})",
        R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":42}})",
    };
    for (const char* body : fallbackBodies) {
        auto outcome = fixture.protocol.handlePost(body, "", "", newId);
        DOCTEST_REQUIRE_EQ(outcome.status, 200);
        const auto root = parseBody(outcome.body);
        DOCTEST_REQUIRE(root.isObject());
        const auto result = root.asObject().get("result");
        DOCTEST_REQUIRE(result.isObject());
        DOCTEST_CHECK_EQ(strField(result.asObject(), "protocolVersion"),
                         std::string("2025-06-18"));
    }
    return true;
})

REGISTER_TEST(mcp_protocol_initialize_notification_no_session,
              "mcp protocol: initialize 作为通知（无 id）→ 202 且不建会话", {
    Fixture fixture;
    std::string newId = "stale";
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","method":"initialize","params":{}})", "", "",
        newId);
    DOCTEST_CHECK_EQ(outcome.status, 202);
    DOCTEST_CHECK(outcome.notificationOnly);
    DOCTEST_CHECK_EQ(outcome.body, std::string());
    DOCTEST_CHECK_EQ(newId, std::string());
    DOCTEST_CHECK_EQ(fixture.sessions.size(), std::size_t{0});
    return true;
})

REGISTER_TEST(mcp_protocol_session_unknown_404_and_missing_session_400,
              "mcp protocol: 未知 session 404；缺 session 头 400（收口批）；已知 session 放行", {
    Fixture fixture;
    std::string newId;

    // 未知 session 先于任何 JSON 解析判掉（body 故意用合法 ping）
    auto unknown = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "no-such-session", "",
        newId);
    DOCTEST_CHECK_EQ(unknown.status, 404);
    DOCTEST_CHECK_EQ(unknown.body, std::string());
    DOCTEST_CHECK_FALSE(unknown.notificationOnly);
    DOCTEST_CHECK_EQ(newId, std::string());

    // 不带 session 的非 initialize：400（收口批,原「无状态放行」口径废止）
    auto anonymous = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})", "", "", newId);
    DOCTEST_CHECK_EQ(anonymous.status, 400);
    DOCTEST_CHECK_EQ(anonymous.body, std::string());
    DOCTEST_CHECK_EQ(newId, std::string());

    // 已知 session 放行
    const std::string session = fixture.openSession();
    DOCTEST_REQUIRE(!session.empty());
    auto known = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":3,"method":"ping"})", session, "", newId);
    DOCTEST_CHECK_EQ(known.status, 200);
    DOCTEST_CHECK_EQ(newId, std::string());
    return true;
})

REGISTER_TEST(mcp_protocol_missing_session_400,
              "mcp protocol: 无 Mcp-Session-Id 的非 initialize 请求 → 400 空 body（通知同）；initialize 豁免", {
    // 收口批(spec SHOULD + 官方 SDK 400 "Missing session ID")：发了会话的服务,
    // 不带会话头的非 initialize 请求一律 400,防会话记账被绕过。
    Fixture fixture;
    std::string newId = "stale";
    auto ping = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "", "", newId);
    DOCTEST_CHECK_EQ(ping.status, 400);
    DOCTEST_CHECK_EQ(ping.body, std::string());
    DOCTEST_CHECK_FALSE(ping.notificationOnly);
    DOCTEST_CHECK_EQ(newId, std::string());

    // 通知（无 id）同样须带会话：门禁不因无 id 放行
    auto note = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "", "",
        newId);
    DOCTEST_CHECK_EQ(note.status, 400);
    DOCTEST_CHECK_EQ(fixture.sessions.size(), std::size_t{0});

    // initialize 豁免：无头照常协商并建会话
    auto init = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})", "",
        "", newId);
    DOCTEST_CHECK_EQ(init.status, 200);
    DOCTEST_CHECK_FALSE(newId.empty());
    DOCTEST_CHECK(fixture.sessions.has(newId));
    return true;
})

REGISTER_TEST(mcp_protocol_new_session_only_on_initialize,
              "mcp protocol: 只有 initialize 成功才产出新 session id（其它方法保持空）", {
    Fixture fixture;
    std::string newId;
    // 缺会话头的 ping 在会话门禁 400（收口批）,同样不产出新会话
    auto anonymous = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "", "", newId);
    DOCTEST_CHECK_EQ(anonymous.status, 400);
    DOCTEST_CHECK_EQ(newId, std::string());
    DOCTEST_CHECK_EQ(fixture.sessions.size(), std::size_t{0});

    auto init = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})", "",
        "", newId);
    DOCTEST_CHECK_EQ(init.status, 200);
    DOCTEST_CHECK_FALSE(newId.empty());
    DOCTEST_CHECK(fixture.sessions.has(newId));

    // 带会话的非 initialize：200 且不另产新 session id（out 是草稿槽,入口即清,
    // 用独立 scratch 接,别吃掉待查的 newId）
    std::string scratch = "scratch";
    auto ping = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":3,"method":"ping"})", newId, "", scratch);
    DOCTEST_CHECK_EQ(ping.status, 200);
    DOCTEST_CHECK_EQ(scratch, std::string());
    DOCTEST_CHECK(fixture.sessions.has(newId));
    return true;
})

REGISTER_TEST(mcp_protocol_version_header_gate,
              "mcp protocol: 协议版本头不受支持 → 400 空 body；支持/缺失放行；initialize 豁免", {
    Fixture fixture;
    std::string newId;
    const std::string pingBody =
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    // 收口批起非 initialize 须带会话头（本用例测版本门禁,统一带合法会话）
    const std::string session = fixture.openSession();
    DOCTEST_REQUIRE(!session.empty());

    auto rejected =
        fixture.protocol.handlePost(pingBody, session, "2024-11-05", newId);
    DOCTEST_CHECK_EQ(rejected.status, 400);
    DOCTEST_CHECK_EQ(rejected.body, std::string());

    auto legacy =
        fixture.protocol.handlePost(pingBody, session, "2025-03-26", newId);
    DOCTEST_CHECK_EQ(legacy.status, 200);

    auto primary =
        fixture.protocol.handlePost(pingBody, session, "2025-06-18", newId);
    DOCTEST_CHECK_EQ(primary.status, 200);

    auto missing = fixture.protocol.handlePost(pingBody, session, "", newId);
    DOCTEST_CHECK_EQ(missing.status, 200);

    // initialize 的版本由 params.protocolVersion 协商，不受请求头限制
    auto init = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})", "",
        "bogus-version", newId);
    DOCTEST_CHECK_EQ(init.status, 200);
    DOCTEST_CHECK_FALSE(newId.empty());
    return true;
})

REGISTER_TEST(mcp_protocol_notifications_are_202,
              "mcp protocol: 通知与未知通知（带合法会话）一律 202、空 body、无会话副作用", {
    Fixture fixture;
    std::string newId = "stale";
    const std::string session = fixture.openSession();
    DOCTEST_REQUIRE(!session.empty());
    const char* notifications[] = {
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        R"({"jsonrpc":"2.0","method":"notifications/unknown"})",
    };
    for (const char* body : notifications) {
        auto outcome = fixture.protocol.handlePost(body, session, "", newId);
        DOCTEST_CHECK_EQ(outcome.status, 202);
        DOCTEST_CHECK(outcome.notificationOnly);
        DOCTEST_CHECK_EQ(outcome.body, std::string());
        DOCTEST_CHECK_EQ(newId, std::string());
    }
    DOCTEST_CHECK_EQ(fixture.sessions.size(), std::size_t{1});
    return true;
})

REGISTER_TEST(mcp_protocol_batch_array_rejected,
              "mcp protocol: JSON 数组（批处理）→ 400 -32600 且 id 为 null", {
    Fixture fixture;
    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])", "", "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 400);
    DOCTEST_CHECK_FALSE(outcome.notificationOnly);
    DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), std::int64_t{-32600});
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    DOCTEST_CHECK(root.asObject().get("id").isNull());
    return true;
})

REGISTER_TEST(mcp_protocol_invalid_json_32700,
              "mcp protocol: 非法 JSON → 400 -32700 且 id 为 null", {
    Fixture fixture;
    std::string newId;
    auto outcome = fixture.protocol.handlePost("{not json", "", "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 400);
    DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), std::int64_t{-32700});
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    DOCTEST_CHECK(root.asObject().get("id").isNull());
    return true;
})

REGISTER_TEST(mcp_protocol_invalid_request_32600,
              "mcp protocol: 合法 JSON 但非 JSON-RPC 请求（无 method/响应对象/非对象）→ 400 -32600", {
    Fixture fixture;
    std::string newId;
    const char* bodies[] = {
        R"({"jsonrpc":"2.0","id":1})",                    // 无 method
        R"({"jsonrpc":"2.0","id":1,"result":{}})",        // 带 id 的响应对象
        R"("just a string")",                             // 非对象
        R"({"jsonrpc":"2.0","id":1,"method":42})",        // method 非字符串
    };
    for (const char* body : bodies) {
        auto outcome = fixture.protocol.handlePost(body, "", "", newId);
        DOCTEST_CHECK_EQ(outcome.status, 400);
        DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), std::int64_t{-32600});
        const auto root = parseBody(outcome.body);
        DOCTEST_REQUIRE(root.isObject());
        DOCTEST_CHECK(root.asObject().get("id").isNull());
    }
    return true;
})

REGISTER_TEST(mcp_protocol_unknown_method_32601_id_echo,
              "mcp protocol: 未知方法 → 200 -32601，数字 id 原样回显", {
    Fixture fixture;
    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/unknown"})", fixture.openSession(), "",
        newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), std::int64_t{-32601});
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto id = root.asObject().get("id");
    DOCTEST_REQUIRE(id.isNumber());
    DOCTEST_CHECK_EQ(id.asInt(), std::int64_t{7});
    return true;
})

REGISTER_TEST(mcp_protocol_ping_result_empty_object,
              "mcp protocol: ping → result 为空对象，字符串 id 原样回显", {
    Fixture fixture;
    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":"ping-1","method":"ping"})", fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    DOCTEST_CHECK(result.asObject().keys().empty());
    DOCTEST_CHECK_EQ(strField(root.asObject(), "id"), std::string("ping-1"));
    return true;
})

REGISTER_TEST(mcp_protocol_tools_list_sorted,
              "mcp protocol: tools/list 按 name 字典序返回 name/description/inputSchema", {
    Fixture fixture;
    auto zeta = std::make_shared<StubTool>("zeta_tool", "z");
    auto alpha = std::make_shared<StubTool>("alpha_tool", "a");
    auto mid = std::make_shared<StubTool>("mid_tool", "m");
    zeta->setSchema(R"({"type":"object","properties":{"x":{"type":"string"}}})");
    DOCTEST_REQUIRE(fixture.tools.registerTool(zeta).isOk());
    DOCTEST_REQUIRE(fixture.tools.registerTool(alpha).isOk());
    DOCTEST_REQUIRE(fixture.tools.registerTool(mid).isOk());

    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    const auto tools = result.asObject().get("tools");
    DOCTEST_REQUIRE(tools.isArray());
    DOCTEST_REQUIRE_EQ(tools.size(), 3);
    DOCTEST_CHECK_EQ(strField(tools[0].asObject(), "name"),
                     std::string("alpha_tool"));
    DOCTEST_CHECK_EQ(strField(tools[1].asObject(), "name"),
                     std::string("mid_tool"));
    DOCTEST_CHECK_EQ(strField(tools[2].asObject(), "name"),
                     std::string("zeta_tool"));
    const auto schema = tools[2].asObject().get("inputSchema");
    DOCTEST_REQUIRE(schema.isObject());
    DOCTEST_CHECK_EQ(strField(schema.asObject(), "type"), std::string("object"));
    return true;
})

REGISTER_TEST(mcp_protocol_tools_list_skips_invalid_schema,
              "mcp protocol: 单个工具 schema 非法只跳过它，其余照常返回（防单坏拖垮整表）", {
    Fixture fixture;
    auto good = std::make_shared<StubTool>("good_tool", "ok");
    auto badJson = std::make_shared<StubTool>("bad_json_tool", "x");
    badJson->setSchema("not-json");
    auto badArray = std::make_shared<StubTool>("bad_array_tool", "x");
    badArray->setSchema("[]");
    DOCTEST_REQUIRE(fixture.tools.registerTool(good).isOk());
    DOCTEST_REQUIRE(fixture.tools.registerTool(badJson).isOk());
    DOCTEST_REQUIRE(fixture.tools.registerTool(badArray).isOk());

    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    const auto tools = result.asObject().get("tools");
    DOCTEST_REQUIRE(tools.isArray());
    DOCTEST_REQUIRE_EQ(tools.size(), 1);
    DOCTEST_CHECK_EQ(strField(tools[0].asObject(), "name"),
                     std::string("good_tool"));
    return true;
})

REGISTER_TEST(mcp_protocol_tools_list_empty,
              "mcp protocol: 空注册表 tools/list 返回空数组", {
    Fixture fixture;
    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    const auto tools = result.asObject().get("tools");
    DOCTEST_REQUIRE(tools.isArray());
    DOCTEST_CHECK_EQ(tools.size(), 0);
    return true;
})

REGISTER_TEST(mcp_protocol_tools_list_escapes_strings,
              "mcp protocol: 工具 name/description 含引号/反斜杠/换行仍产出合法 JSON 且回读一致", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("escape_tool", "x");
    const std::string description = "他说\"hi\"\\路径\n第二行\t制表";
    tool->setDescription(description);
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());

    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    const auto tools = result.asObject().get("tools");
    DOCTEST_REQUIRE(tools.isArray());
    DOCTEST_REQUIRE_EQ(tools.size(), 1);
    DOCTEST_CHECK_EQ(strField(tools[0].asObject(), "description"), description);
    return true;
})

REGISTER_TEST(mcp_protocol_tools_call_success_and_escaping,
              "mcp protocol: tools/call 成功返回 content 文本且无 isError；正文转义回读一致", {
    Fixture fixture;
    const std::string text = "结果：\"引号\"\\反斜杠\n换行\t制表\x01控制";
    auto tool = std::make_shared<StubTool>("echo_tool", text);
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());

    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo_tool","arguments":{"a":1}}})",
        fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    const auto resultObj = result.asObject();
    DOCTEST_CHECK_FALSE(resultObj.has("isError"));
    const auto content = resultObj.get("content");
    DOCTEST_REQUIRE(content.isArray());
    DOCTEST_REQUIRE_EQ(content.size(), 1);
    const auto block = content[0].asObject();
    DOCTEST_CHECK_EQ(strField(block, "type"), std::string("text"));
    DOCTEST_CHECK_EQ(strField(block, "text"), text);
    DOCTEST_CHECK_EQ(tool->lastArgs, std::string(R"({"a":1})"));
    DOCTEST_CHECK_EQ(tool->calls, 1);
    return true;
})

REGISTER_TEST(mcp_protocol_tools_call_is_error,
              "mcp protocol: 工具业务失败（isError=true）仍 200，result 带 isError=true", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("failing_business_tool", "业务失败",
                                           true);
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());

    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"failing_business_tool"}})",
        fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    DOCTEST_CHECK(boolField(result.asObject(), "isError"));
    const auto content = result.asObject().get("content");
    DOCTEST_REQUIRE(content.isArray());
    DOCTEST_REQUIRE_EQ(content.size(), 1);
    DOCTEST_CHECK_EQ(strField(content[0].asObject(), "text"),
                     std::string("业务失败"));
    return true;
})

REGISTER_TEST(mcp_protocol_tools_call_arguments_default_empty_object,
              "mcp protocol: arguments 缺失或 null 按 {} 传给工具", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("args_tool", "ok");
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());

    std::string newId;
    auto missing = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"args_tool"}})",
        fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(missing.status, 200);
    DOCTEST_CHECK_EQ(tool->lastArgs, std::string("{}"));

    auto nullArgs = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"args_tool","arguments":null}})",
        fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(nullArgs.status, 200);
    DOCTEST_CHECK_EQ(tool->lastArgs, std::string("{}"));
    return true;
})

REGISTER_TEST(mcp_protocol_tools_call_param_errors_32602,
              "mcp protocol: params 缺失/name 非法/arguments 非对象/未知工具 → -32602", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("args_tool", "ok");
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());
    std::string newId;

    const char* badBodies[] = {
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})",  // 无 params
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{}})",  // 无 name
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":""}})",  // 空 name
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":7}})",  // name 非字符串
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"args_tool","arguments":"x"}})",  // arguments 非对象
    };
    for (const char* body : badBodies) {
        auto outcome = fixture.protocol.handlePost(body, fixture.openSession(), "", newId);
        DOCTEST_CHECK_EQ(outcome.status, 200);
        DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), std::int64_t{-32602});
    }
    DOCTEST_CHECK_EQ(tool->calls, 0);  // 参数非法不触达工具

    auto unknown = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"ghost_tool"}})",
        fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(unknown.status, 200);
    DOCTEST_CHECK_EQ(errorCodeOf(unknown.body), std::int64_t{-32602});
    const auto root = parseBody(unknown.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto error = root.asObject().get("error");
    DOCTEST_REQUIRE(error.isObject());
    DOCTEST_CHECK(strField(error.asObject(), "message").find("ghost_tool") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(mcp_protocol_tools_call_internal_error_32603,
              "mcp protocol: 工具 execute 返回 fail → 200 -32603（业务失败必须用 isError）", {
    Fixture fixture;
    DOCTEST_REQUIRE(
        fixture.tools.registerTool(std::make_shared<FailingMcpTool>()).isOk());
    std::string newId;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"fail_tool"}})",
        fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(outcome.status, 200);
    DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), std::int64_t{-32603});
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto error = root.asObject().get("error");
    DOCTEST_REQUIRE(error.isObject());
    DOCTEST_CHECK(strField(error.asObject(), "message").find("boom") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(mcp_protocol_id_echo_raw,
              "mcp protocol: id 解析后原样回显（大整数/含引号反斜杠换行控制字符的字符串）", {
    Fixture fixture;

    // 大整数 id（超出 32 位范围）逐位回显
    std::string newId;
    auto numeric = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":9007199254740993,"method":"ping"})", fixture.openSession(), "",
        newId);
    DOCTEST_CHECK_EQ(numeric.status, 200);
    {
        const auto root = parseBody(numeric.body);
        DOCTEST_REQUIRE(root.isObject());
        const auto id = root.asObject().get("id");
        DOCTEST_REQUIRE(id.isNumber());
        DOCTEST_CHECK_EQ(id.asInt(), std::int64_t{9007199254740993LL});
    }

    // 字符串 id 含转义字符：请求经 JSON 工厂构造，响应回读必须逐字符相等
    const std::string specialId = "id\"\\\n\t\x01 中文";
    platform::json::JsonObject request;
    request.set("jsonrpc", platform::json::makeString("2.0"));
    request.set("id", platform::json::makeString(specialId));
    request.set("method", platform::json::makeString("ping"));
    auto body = platform::json::stringify(request);
    DOCTEST_REQUIRE(body.isOk());
    auto stringIdOutcome =
        fixture.protocol.handlePost(body.value(), fixture.openSession(), "", newId);
    DOCTEST_CHECK_EQ(stringIdOutcome.status, 200);
    const auto root = parseBody(stringIdOutcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto id = root.asObject().get("id");
    DOCTEST_REQUIRE(id.isString());
    DOCTEST_CHECK_EQ(id.asString(), specialId);
    return true;
})

REGISTER_TEST(mcp_protocol_delete_session_semantics,
              "mcp protocol: DELETE 带已知 session 移除并 200；未知 404；不带 200", {
    Fixture fixture;
    const std::string session = fixture.openSession();
    DOCTEST_REQUIRE(!session.empty());
    DOCTEST_REQUIRE(fixture.sessions.has(session));

    auto removed = fixture.protocol.handleDelete(session);
    DOCTEST_CHECK_EQ(removed.status, 200);
    DOCTEST_CHECK_FALSE(fixture.sessions.has(session));

    auto again = fixture.protocol.handleDelete(session);
    DOCTEST_CHECK_EQ(again.status, 404);

    auto unknown = fixture.protocol.handleDelete("never-created");
    DOCTEST_CHECK_EQ(unknown.status, 404);

    auto empty = fixture.protocol.handleDelete("");
    DOCTEST_CHECK_EQ(empty.status, 200);
    return true;
})