// tests/test_modules/test_notify_tool.cpp
// app.notify 工具契约（spec §7.4：Receipt 状态 → 输出 JSON 映射）：
//   1) 受理 → status=accepted / deliveredVia=system / id / truncated；
//   2) 去重 / 限速 → deliveredVia=none，fallback 零调用（不算失败）；
//   3) 截断标志透传（truncated=true）；
//   4) 环境失败（Denied/NotDetermined/后端不可用）→ fallback 入队 + reason 映射；
//   5) 缺字段 / 空串 / 类型错 / 坏 JSON → isError=true，后端零调用。
// 三态断言在草案 Result<void> 接口下无法表达（无 Receipt）——即「无实现必红」。
#include "doctest.h"

#include "core/result.h"
#include "module_agent/app_notify_tool.h"
#include "platform/json.h"
#include "platform/notification.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace pj = platform::json;
using platform::notify::Authorization;
using platform::notify::Backend;
using platform::notify::Notification;
using platform::notify::Status;

struct FakeBackend final : Backend {
    int sendCalls = 0;
    int authorizationRequests = 0;
    Notification lastNotification;
    core::Result<Status> sendResult = core::Result<Status>::ok(Status::Accepted);
    Status sendStatus = Status::Accepted;
    Authorization status = Authorization::Authorized;

    void initialize() override {}

    core::Result<Status> send(const Notification& notification) override {
        ++sendCalls;
        lastNotification = notification;
        if (sendResult.isFail()) return sendResult;
        return core::Result<Status>::ok(sendStatus);
    }

    Authorization authorizationStatus() override { return status; }

    void requestAuthorization() override { ++authorizationRequests; }
};

// 用例 fail-fast 时析构仍会执行：全量复位（后端/去重/限速/时钟），避免串味。
struct ResetGuard {
    ~ResetGuard() { platform::notify::resetForTest(); }
};

bool parseOutput(const core::ToolResult& result, pj::JsonObject& out) {
    auto parsed = pj::parse(result.contentJson);
    if (parsed.isFail() || !parsed.value().isObject()) return false;
    out = parsed.value().asObject();
    return true;
}

} // namespace

REGISTER_TEST(app_notify_success_forwards_fields,
              "app.notify: 受理 → status=accepted/deliveredVia=system/id，字段透传", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    module::agent::NotifyTool tool;
    auto result = tool.execute(core::ToolArgs{
        R"({"title":"逾期待办提醒","body":"「交周报」已逾期 2 天","sound":false,"id":"todo-42"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("accepted"));
    DOCTEST_CHECK_EQ(out.get("deliveredVia").asString(), std::string("system"));
    DOCTEST_CHECK_EQ(out.get("id").asString(), std::string("todo-42"));
    DOCTEST_CHECK_FALSE(out.get("truncated").asBool());

    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    DOCTEST_CHECK_EQ(fake.lastNotification.title, std::string("逾期待办提醒"));
    DOCTEST_CHECK_EQ(fake.lastNotification.body, std::string("「交周报」已逾期 2 天"));
    DOCTEST_CHECK_EQ(fake.lastNotification.sound, false);
    DOCTEST_CHECK_EQ(fake.lastNotification.id, std::string("todo-42"));
    return true;
})

REGISTER_TEST(app_notify_truncated_flag_surfaces,
              "app.notify: 超长标题被 facade 截断 → truncated=true 透传给调用方", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    std::string longTitle;
    for (int i = 0; i < 80; ++i) longTitle += "字";
    module::agent::NotifyTool tool;
    auto result = tool.execute(core::ToolArgs{
        R"({"title":")" + longTitle + R"(","body":"正文"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("accepted"));
    DOCTEST_CHECK(out.get("truncated").asBool());
    return true;
})

REGISTER_TEST(app_notify_rate_limited_no_fallback,
              "app.notify: 第 4 条被限速 → status=rate_limited/retryAfterMs，fallback 零调用", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    int fallbackCalls = 0;
    module::agent::NotifyTool tool([&fallbackCalls](std::string) { ++fallbackCalls; });

    for (int i = 0; i < 3; ++i) {
        auto result = tool.execute(core::ToolArgs{
            R"({"title":"限速","body":"第)" + std::to_string(i) + R"(条"})"});
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK_FALSE(result.value().isError);
        pj::JsonObject out;
        DOCTEST_REQUIRE(parseOutput(result.value(), out));
        DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("accepted"));
    }

    auto result = tool.execute(core::ToolArgs{
        R"({"title":"限速","body":"第4条"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);   // 限速不算失败
    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("rate_limited"));
    DOCTEST_CHECK_EQ(out.get("deliveredVia").asString(), std::string("none"));
    DOCTEST_CHECK(out.get("retryAfterMs").asInt() > 0);
    DOCTEST_CHECK_EQ(fake.sendCalls, 3);
    DOCTEST_CHECK_EQ(fallbackCalls, 0);   // 限速不降级
    return true;
})

REGISTER_TEST(app_notify_deduped_no_fallback,
              "app.notify: 2s 内重复内容 → status=deduped，fallback 零调用", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    int fallbackCalls = 0;
    module::agent::NotifyTool tool([&fallbackCalls](std::string) { ++fallbackCalls; });

    const char* payload = R"({"title":"去重","body":"相同内容","id":"dup-tool"})";
    auto first = tool.execute(core::ToolArgs{payload});
    DOCTEST_REQUIRE(first.isOk());
    pj::JsonObject out1;
    DOCTEST_REQUIRE(parseOutput(first.value(), out1));
    DOCTEST_CHECK_EQ(out1.get("status").asString(), std::string("accepted"));

    auto second = tool.execute(core::ToolArgs{payload});
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_FALSE(second.value().isError);
    pj::JsonObject out2;
    DOCTEST_REQUIRE(parseOutput(second.value(), out2));
    DOCTEST_CHECK_EQ(out2.get("status").asString(), std::string("deduped"));
    DOCTEST_CHECK_EQ(out2.get("deliveredVia").asString(), std::string("none"));
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    DOCTEST_CHECK_EQ(fallbackCalls, 0);   // 去重不降级
    return true;
})

REGISTER_TEST(app_notify_permission_denied_falls_back_to_app,
              "app.notify: 授权拒绝 → fallback 收到 title/body，reason=permission_denied", {
    ResetGuard guard;
    FakeBackend fake;
    fake.status = Authorization::Denied;
    platform::notify::setBackendForTest(&fake);

    std::vector<std::string> fallbackTexts;
    module::agent::NotifyTool tool(
        [&fallbackTexts](std::string text) {
            fallbackTexts.push_back(std::move(text));
        });

    auto result = tool.execute(core::ToolArgs{
        R"({"title":"闹钟到点","body":"该起来活动一下了"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("fallback"));
    DOCTEST_CHECK_EQ(out.get("deliveredVia").asString(), std::string("app_toast"));
    DOCTEST_CHECK_EQ(out.get("reason").asString(), std::string("permission_denied"));
    DOCTEST_CHECK(out.get("detail").isString());

    DOCTEST_CHECK_EQ(fallbackTexts.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(fallbackTexts[0].find("闹钟到点") != std::string::npos);
    DOCTEST_CHECK(fallbackTexts[0].find("该起来活动一下了") != std::string::npos);
    DOCTEST_CHECK_EQ(fake.sendCalls, 0);   // facade 在授权检查拦截，后端零调用
    return true;
})

REGISTER_TEST(app_notify_not_determined_falls_back_and_requests_auth,
              "app.notify: 授权未决定 → fallback reason=not_determined 且已异步请求授权", {
    ResetGuard guard;
    FakeBackend fake;
    fake.status = Authorization::NotDetermined;
    platform::notify::setBackendForTest(&fake);

    int fallbackCalls = 0;
    module::agent::NotifyTool tool([&fallbackCalls](std::string) { ++fallbackCalls; });

    auto result = tool.execute(core::ToolArgs{
        R"({"title":"标题","body":"正文"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("fallback"));
    DOCTEST_CHECK_EQ(out.get("reason").asString(), std::string("not_determined"));
    DOCTEST_CHECK_EQ(fake.authorizationRequests, 1);
    DOCTEST_CHECK_EQ(fallbackCalls, 1);
    return true;
})

REGISTER_TEST(app_notify_backend_failure_maps_to_fallback,
              "app.notify: 后端内部失败 → facade 映射 backend_unavailable → fallback", {
    ResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<Status>::fail(core::ErrorCode::Internal,
                                                 "系统通知服务不可用");
    platform::notify::setBackendForTest(&fake);

    int fallbackCalls = 0;
    module::agent::NotifyTool tool([&fallbackCalls](std::string) { ++fallbackCalls; });

    auto result = tool.execute(core::ToolArgs{
        R"({"title":"标题","body":"正文"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK_EQ(out.get("status").asString(), std::string("fallback"));
    DOCTEST_CHECK_EQ(out.get("reason").asString(), std::string("backend_unavailable"));
    DOCTEST_CHECK(out.get("detail").asString().find("系统通知服务不可用") != std::string::npos);
    DOCTEST_CHECK_EQ(fallbackCalls, 1);
    return true;
})

REGISTER_TEST(app_notify_backend_failure_without_fallback_reports_error,
              "app.notify: 后端不可用且未注入 fallback → isError 保留原始 detail", {
    ResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<Status>::fail(core::ErrorCode::Internal,
                                                 "系统通知后端失败");
    platform::notify::setBackendForTest(&fake);

    module::agent::NotifyTool tool;
    auto result = tool.execute(core::ToolArgs{
        R"({"title":"标题","body":"正文"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK(out.get("error").asString().find("系统通知后端失败") != std::string::npos);
    return true;
})

REGISTER_TEST(app_notify_fallback_throw_reports_error,
              "app.notify: fallback 抛异常 → isError 保留原始错误与异常消息", {
    ResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<Status>::fail(core::ErrorCode::Internal,
                                                 "系统通知后端失败");
    platform::notify::setBackendForTest(&fake);

    int fallbackCalls = 0;
    module::agent::NotifyTool tool([&fallbackCalls](std::string) {
        ++fallbackCalls;
        throw std::runtime_error("降级队列不可用");
    });

    auto result = tool.execute(core::ToolArgs{
        R"({"title":"标题","body":"正文"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK_EQ(fallbackCalls, 1);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    const std::string error = out.get("error").asString();
    DOCTEST_CHECK(error.find("系统通知后端失败") != std::string::npos);
    DOCTEST_CHECK(error.find("降级队列不可用") != std::string::npos);
    return true;
})

REGISTER_TEST(app_notify_rejects_missing_and_blank_fields,
              "app.notify: 缺 title/body、空串、纯空白 → isError 且后端零调用", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    struct Case {
        const char* json;
        const char* field;
    };
    const Case cases[] = {
        {"", "title"},
        {R"({})", "title"},
        {R"({"title":"标题"})", "body"},
        {R"({"title":"   ","body":"正文"})", "title"},
        {R"({"title":"标题","body":" \t\n "})", "body"},
    };

    module::agent::NotifyTool tool;
    for (const Case& c : cases) {
        auto result = tool.execute(core::ToolArgs{c.json});
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        pj::JsonObject out;
        DOCTEST_REQUIRE(parseOutput(result.value(), out));
        DOCTEST_CHECK(out.get("error").isString());
        DOCTEST_CHECK(out.get("error").asString().find(c.field) != std::string::npos);
    }
    DOCTEST_CHECK_EQ(fake.sendCalls, 0);
    return true;
})

REGISTER_TEST(app_notify_rejects_wrong_types_and_bad_json,
              "app.notify: 字段类型错 / 坏 JSON → isError 且后端零调用", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    const char* cases[] = {
        R"({"title":123,"body":"正文"})",
        R"({"title":"标题","body":null})",
        R"({"title":"标题","body":"正文","sound":"true"})",
        R"({"title":"标题","body":"正文","id":7})",
        R"({"title":)",
        R"([])",
    };

    module::agent::NotifyTool tool;
    for (const char* json : cases) {
        auto result = tool.execute(core::ToolArgs{json});
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        pj::JsonObject out;
        DOCTEST_REQUIRE(parseOutput(result.value(), out));
        DOCTEST_CHECK(out.get("error").isString());
        DOCTEST_CHECK_FALSE(out.get("error").asString().empty());
    }
    DOCTEST_CHECK_EQ(fake.sendCalls, 0);
    return true;
})

REGISTER_TEST(app_notify_sound_defaults_true_and_false_passthrough,
              "app.notify: sound 缺省 true、显式 false 透传", {
    ResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    module::agent::NotifyTool tool;
    auto defaults = tool.execute(core::ToolArgs{
        R"({"title":"默认声音","body":"未传 sound"})"});
    DOCTEST_REQUIRE(defaults.isOk());
    DOCTEST_CHECK_FALSE(defaults.value().isError);
    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    DOCTEST_CHECK(fake.lastNotification.sound);

    auto silent = tool.execute(core::ToolArgs{
        R"({"title":"静音","body":"显式 sound=false","sound":false})"});
    DOCTEST_REQUIRE(silent.isOk());
    DOCTEST_CHECK_FALSE(silent.value().isError);
    DOCTEST_CHECK_EQ(fake.sendCalls, 2);
    DOCTEST_CHECK_FALSE(fake.lastNotification.sound);
    return true;
})

REGISTER_TEST(app_notify_schema_required_and_not_destructive,
              "app.notify: schema 可解析、required 含 title/body、非破坏性", {
    module::agent::NotifyTool tool;
    DOCTEST_CHECK_EQ(tool.name(), std::string("app.notify"));
    DOCTEST_CHECK_FALSE(tool.isDestructive());

    auto parsed = pj::parse(tool.schemaJson());
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    auto schema = parsed.value().asObject();
    DOCTEST_CHECK_EQ(schema.get("name").asString(), std::string("app.notify"));
    DOCTEST_CHECK(schema.get("description").asString().find("系统通知") !=
                  std::string::npos);

    auto input = schema.get("input_schema").asObject();
    DOCTEST_CHECK_EQ(input.get("type").asString(), std::string("object"));
    auto required = input.get("required");
    DOCTEST_REQUIRE(required.isArray());
    DOCTEST_CHECK_EQ(required.size(), 2);
    bool hasTitle = false;
    bool hasBody = false;
    for (int i = 0; i < required.size(); ++i) {
        const std::string field = required[i].asString();
        if (field == "title") hasTitle = true;
        if (field == "body") hasBody = true;
    }
    DOCTEST_CHECK(hasTitle);
    DOCTEST_CHECK(hasBody);

    auto properties = input.get("properties").asObject();
    DOCTEST_CHECK(properties.has("title"));
    DOCTEST_CHECK(properties.has("body"));
    DOCTEST_CHECK(properties.has("sound"));
    DOCTEST_CHECK(properties.has("id"));
    DOCTEST_CHECK_FALSE(properties.has("level"));   // 本工具固定 Info，不暴露级别
    return true;
})
