// tests/test_modules/test_notify_tool.cpp
// app.notify 工具契约（平台无关层 + fake backend，不触发真实系统通知）：
//   1) 合法参数原样透传 title/body/sound/id，成功输出 ok/accepted；
//   2) 缺字段 / 空串 / 纯空白 / 类型错 / 坏 JSON → isError=true，后端零调用；
//   3) 系统通知失败 + 注入 fallback → fallback 收到 title/body，输出 ok/fallback=app；
//   4) 系统通知失败 + 无 fallback 或 fallback 抛异常 → isError=true，保留原始错误；
//   5) sound 缺省 true、显式 false 透传；schema 必填 title/body。
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

struct FakeBackend final : Backend {
    int sendCalls = 0;
    Notification lastNotification;
    core::Result<void> sendResult = core::Result<void>::ok();
    Authorization status = Authorization::NotDetermined;

    core::Result<void> send(const Notification& notification) override {
        ++sendCalls;
        lastNotification = notification;
        return sendResult;
    }

    Authorization authorizationStatus() override { return status; }

    void requestAuthorization() override {}
};

// 用例 fail-fast 时析构仍会执行：拉回默认后端，避免栈上 fake 悬垂。
struct BackendResetGuard {
    ~BackendResetGuard() { platform::notify::resetBackendForTest(); }
};

bool parseOutput(const core::ToolResult& result, pj::JsonObject& out) {
    auto parsed = pj::parse(result.contentJson);
    if (parsed.isFail() || !parsed.value().isObject()) return false;
    out = parsed.value().asObject();
    return true;
}

} // namespace

REGISTER_TEST(app_notify_success_forwards_fields,
              "app.notify: 合法参数透传 title/body/sound/id 并返回 accepted", {
    BackendResetGuard guard;
    FakeBackend fake;
    platform::notify::setBackendForTest(&fake);

    module::agent::NotifyTool tool;
    auto result = tool.execute(core::ToolArgs{
        R"({"title":"逾期待办提醒","body":"「交周报」已逾期 2 天","sound":false,"id":"todo-42"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK(out.get("ok").asBool());
    DOCTEST_CHECK(out.get("accepted").asBool());
    DOCTEST_CHECK(out.get("note").isString());

    DOCTEST_CHECK_EQ(fake.sendCalls, 1);
    DOCTEST_CHECK_EQ(fake.lastNotification.title, std::string("逾期待办提醒"));
    DOCTEST_CHECK_EQ(fake.lastNotification.body, std::string("「交周报」已逾期 2 天"));
    DOCTEST_CHECK_EQ(fake.lastNotification.sound, false);
    DOCTEST_CHECK_EQ(fake.lastNotification.id, std::string("todo-42"));
    return true;
})

REGISTER_TEST(app_notify_rejects_missing_and_blank_fields,
              "app.notify: 缺 title/body、空串、纯空白 → isError 且后端零调用", {
    BackendResetGuard guard;
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
    BackendResetGuard guard;
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

REGISTER_TEST(app_notify_backend_failure_falls_back_to_app,
              "app.notify: 后端失败 + fallback → fallback 收到 title/body，输出 ok/fallback=app", {
    BackendResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<void>::fail(core::ErrorCode::Internal,
                                               "系统通知服务不可用");
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
    DOCTEST_CHECK(out.get("ok").asBool());
    DOCTEST_CHECK_EQ(out.get("fallback").asString(), std::string("app"));
    DOCTEST_CHECK_EQ(out.get("warning").asString(),
                     std::string("系统通知不可用，已改为应用内提示"));

    DOCTEST_CHECK_EQ(fallbackTexts.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(fallbackTexts[0].find("闹钟到点") != std::string::npos);
    DOCTEST_CHECK(fallbackTexts[0].find("该起来活动一下了") != std::string::npos);
    return true;
})

REGISTER_TEST(app_notify_backend_failure_without_fallback_reports_error,
              "app.notify: 后端失败且未注入 fallback → isError 保留原始错误", {
    BackendResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<void>::fail(core::ErrorCode::Internal,
                                               "系统通知后端失败");
    platform::notify::setBackendForTest(&fake);

    module::agent::NotifyTool tool;
    auto result = tool.execute(core::ToolArgs{
        R"({"title":"标题","body":"正文"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);

    pj::JsonObject out;
    DOCTEST_REQUIRE(parseOutput(result.value(), out));
    DOCTEST_CHECK(out.get("error").isString());
    DOCTEST_CHECK(out.get("error").asString().find("系统通知后端失败") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(app_notify_fallback_throw_reports_error,
              "app.notify: 后端失败 + fallback 抛异常 → isError 保留原始错误", {
    BackendResetGuard guard;
    FakeBackend fake;
    fake.sendResult = core::Result<void>::fail(core::ErrorCode::Internal,
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

REGISTER_TEST(app_notify_sound_defaults_true_and_false_passthrough,
              "app.notify: sound 缺省 true、显式 false 透传", {
    BackendResetGuard guard;
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
