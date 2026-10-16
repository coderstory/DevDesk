// tests/test_modules/test_direct_llm_client.cpp
// DirectLlmClient 契约：直连上游（不经本地代理）的请求组装、槽位解析、鉴权头三态、
// 上游方言、错误分类，以及**与代理解耦**的硬证据。
//
// 跨文件前提：
//   - model 形参是槽位**键名**，客户端内部自行解析成实际 model id——形参原文不进 body。
//   - 配置来源 = core::LlmProfileSource（生产 = 激活 profile）；本文件一律注入
//     test_support::FakeProfileSource，不起代理、不发真实网络。
//   - 错误文案前缀「LLM 配置不可用」是 needsClaudePageHint 的匹配面（agent_module.h 契约），
//     文案一变 test_agent_ui.cpp 的前缀用例即红。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "core/claude_proxy_service.h"
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "core/service_locator.h"
#include "module_agent/direct_llm_client.h"
#include "platform/http_client.h"

#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace {

// 捕获一次请求并回固定响应；calls 计数供「一个包都不发」类断言
struct Capture {
    platform::http::Request req;
    int calls = 0;
    int status = 200;
    std::string body =
        R"({"id":"m1","stop_reason":"end_turn","content":[{"type":"text","text":"hi"}],)"
        R"("usage":{"input_tokens":11,"output_tokens":7}})";

    module::agent::DirectLlmClient::HttpFn fn() {
        return [this](const platform::http::Request& r) {
            ++calls;
            req = r;
            platform::http::Response res;
            res.statusCode = status;
            res.body = body;
            return core::Result<platform::http::Response>::ok(res);
        };
    }
};

const char* const kModelsJson = R"({"sonnet":{"model":"claude-sonnet-4-5"}})";

// 造坏配置后发一次（注入假发送），回「配置不可用」类错误文案；成功 = 空串。
// sentOut 供「一个包都不发」断言。只回数据不做断言：断言须平铺在用例体
std::string configErrorTextFor(
    const std::function<void(test_support::FakeProfileSource&)>& mutate, bool* sentOut) {
    test_support::FakeProfileSource profile;
    mutate(profile);
    Capture cap;
    module::agent::DirectLlmClient client(profile, cap.fn());
    auto r = client.send({{"user", "[]"}}, "[]", "", "default");
    *sentOut = cap.calls > 0;
    return r.isOk() ? std::string() : r.errorMessage();
}

} // namespace

REGISTER_TEST(direct_llm_client_posts_to_profile_upstream,
              "direct_llm_client: 直发 {baseUrl}/v1/messages（鉴权头 + 槽位解析 + 响应解析）", {
    test_support::FakeProfileSource profile;
    profile.profile.modelsConfigJson = kModelsJson;
    profile.profile.defaultModel = "claude-opus-4-1";
    Capture cap;
    module::agent::DirectLlmClient client(profile, cap.fn());

    std::vector<core::LlmMessage> msgs{{"user", R"([{"type":"text","text":"q"}])"}};
    auto r = client.send(msgs, R"([{"name":"t.a"}])", "sys-prompt", "sonnet");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(cap.calls, 1);
    // 直连 = 上游 baseUrl 原样，绝不指向 127.0.0.1 本地代理
    DOCTEST_CHECK_EQ(cap.req.url, std::string("https://api.anthropic.com/v1/messages"));
    DOCTEST_CHECK_EQ(cap.req.method, std::string("POST"));
    DOCTEST_CHECK(cap.req.verifyTls);
    // 鉴权头与上游契约头由本客户端注入（代理路径里这活是代理干的）
    DOCTEST_CHECK_EQ(cap.req.headers["x-api-key"], std::string("sk-test-key"));
    DOCTEST_CHECK_EQ(cap.req.headers["Authorization"],
                     std::string("Bearer sk-test-key"));
    DOCTEST_CHECK_EQ(cap.req.headers["anthropic-version"],
                     std::string("2023-06-01"));
    DOCTEST_CHECK_EQ(cap.req.headers["content-type"], std::string("application/json"));
    DOCTEST_CHECK(cap.req.headers["User-Agent"].find("claude-cli") != std::string::npos);
    // body：model 是解析后的 id（不是槽位键名），其余字段按契约
    DOCTEST_CHECK(cap.req.body.find("\"model\":\"claude-sonnet-4-5\"") != std::string::npos);
    // body 里的 model 是解析后的 id：槽位键名不单独出现在 model 字段
    DOCTEST_CHECK(cap.req.body.find("\"model\":\"sonnet\"") == std::string::npos);
    DOCTEST_CHECK(cap.req.body.find("\"max_tokens\":4096") != std::string::npos);
    DOCTEST_CHECK(cap.req.body.find("\"system\":\"sys-prompt\"") != std::string::npos);
    DOCTEST_CHECK(cap.req.body.find("\"tools\":[{\"name\":\"t.a\"}]") != std::string::npos);
    // 响应解析
    DOCTEST_CHECK_EQ(r.value().stopReason, std::string("end_turn"));
    DOCTEST_CHECK_EQ(r.value().inputTokens, 11);
    DOCTEST_CHECK_EQ(r.value().outputTokens, 7);
    DOCTEST_CHECK(r.value().contentJson.find("\"text\":\"hi\"") != std::string::npos);
    return true;
})

REGISTER_TEST(direct_llm_client_slot_resolution,
              "direct_llm_client: 槽位解析（def≡default / default 回落 profile.model / 未配置→fail）", {
    test_support::FakeProfileSource profile;
    profile.profile.modelsConfigJson = kModelsJson;
    profile.profile.defaultModel = "claude-opus-4-1";

    // def ≡ default；models_config 缺 default 槽 → 回落 profile.model
    for (const char* slot : {"default", "def"}) {
        Capture cap;
        module::agent::DirectLlmClient client(profile, cap.fn());
        auto r = client.send({{"user", "[]"}}, "[]", "", slot);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(cap.req.body.find("\"model\":\"claude-opus-4-1\"") !=
                      std::string::npos);
    }
    // 已配置槽位取 models_config 里的 id
    {
        Capture cap;
        module::agent::DirectLlmClient client(profile, cap.fn());
        DOCTEST_REQUIRE(client.send({{"user", "[]"}}, "[]", "", "sonnet").isOk());
        DOCTEST_CHECK(cap.req.body.find("\"model\":\"claude-sonnet-4-5\"") !=
                      std::string::npos);
    }
    // 未配置槽位 / 未知槽位键 → fail 且不发包
    for (const char* slot : {"haiku", "bogus"}) {
        Capture cap;
        module::agent::DirectLlmClient client(profile, cap.fn());
        auto r = client.send({{"user", "[]"}}, "[]", "", slot);
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK_EQ(cap.calls, 0);
    }
    // models_config 空串：default 仍回落 profile.model，其余槽位无解析
    {
        profile.profile.modelsConfigJson.clear();
        Capture cap;
        module::agent::DirectLlmClient client(profile, cap.fn());
        DOCTEST_REQUIRE(client.send({{"user", "[]"}}, "[]", "", "default").isOk());
        DOCTEST_CHECK(cap.req.body.find("\"model\":\"claude-opus-4-1\"") !=
                      std::string::npos);
        auto sonnet = client.send({{"user", "[]"}}, "[]", "", "sonnet");
        DOCTEST_CHECK(sonnet.isFail());
    }
    return true;
})

REGISTER_TEST(direct_llm_client_auth_field_three_states,
              "direct_llm_client: authField 三态（both 双发 / bearer 只 Bearer / api_key 只 x-api-key）", {
    for (const auto& [field, wantApiKey, wantBearer] :
         std::vector<std::tuple<std::string, bool, bool>>{
             {"both", true, true}, {"bearer", false, true}, {"api_key", true, false}}) {
        test_support::FakeProfileSource profile;
        profile.profile.authField = field;
        Capture cap;
        module::agent::DirectLlmClient client(profile, cap.fn());
        DOCTEST_REQUIRE(client.send({{"user", "[]"}}, "[]", "", "default").isOk());
        DOCTEST_CHECK_EQ(cap.req.headers.count("x-api-key") != 0, wantApiKey);
        DOCTEST_CHECK_EQ(cap.req.headers.count("Authorization") != 0, wantBearer);
    }
    return true;
})

REGISTER_TEST(direct_llm_client_config_errors_do_not_send,
              "direct_llm_client: 四类配置不可用 → fail 且一个包都不发（前缀供「去 Claude 页」引导）", {
    // ① 无激活 profile（来源自身失败，文案原样带出）
    {
        test_support::FakeProfileSource profile;
        profile.failWith = "没有激活的 Claude profile，请在 DevDesk 的 Claude 页选中 "
                           "Profile 并点击「启用」";
        Capture cap;
        module::agent::DirectLlmClient client(profile, cap.fn());
        auto r = client.send({{"user", "[]"}}, "[]", "", "default");
        DOCTEST_REQUIRE(r.isFail());
        DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
        DOCTEST_CHECK(r.errorMessage().rfind("LLM 配置不可用", 0) == 0);
        DOCTEST_CHECK(r.errorMessage().find("没有激活的 Claude profile") !=
                      std::string::npos);
        DOCTEST_CHECK_EQ(cap.calls, 0);
    }
    // ② 未配 Base URL ③ 未配 API Key ④ 槽位未配置。断言平铺（本框架 DOCTEST_CHECK
    // 失败展开为 return false，进 lambda 会退成「lambda 内绿」，故只回传数据）
    bool sent = true;   // 起点 true：任何一次误发包都会把下方断言打红
    const std::string noBaseUrl = configErrorTextFor(
        [](test_support::FakeProfileSource& p) { p.profile.baseUrl.clear(); }, &sent);
    DOCTEST_REQUIRE(noBaseUrl.size() > 0);
    DOCTEST_CHECK(noBaseUrl.find("Base URL") != std::string::npos);
    DOCTEST_CHECK_FALSE(sent);
    sent = true;
    const std::string noKey = configErrorTextFor(
        [](test_support::FakeProfileSource& p) { p.profile.apiKeyPlain.clear(); }, &sent);
    DOCTEST_REQUIRE(noKey.size() > 0);
    DOCTEST_CHECK(noKey.find("API Key") != std::string::npos);
    DOCTEST_CHECK_FALSE(sent);
    sent = true;
    const std::string noSlot = configErrorTextFor(
        [](test_support::FakeProfileSource& p) { p.profile.defaultModel.clear(); }, &sent);
    DOCTEST_REQUIRE(noSlot.size() > 0);
    DOCTEST_CHECK(noSlot.find("无法解析模型槽位") != std::string::npos);
    DOCTEST_CHECK_FALSE(sent);
    return true;
})

REGISTER_TEST(direct_llm_client_upstream_status_and_network_errors,
              "direct_llm_client: 上游 401 → HttpError（带状态码）；网络失败 → 原样透传错误码", {
    {
        test_support::FakeProfileSource profile;
        Capture cap;
        cap.status = 401;
        cap.body = R"({"error":{"type":"authentication_error"}})";
        module::agent::DirectLlmClient client(profile, cap.fn());
        auto r = client.send({{"user", "[]"}}, "[]", "", "default");
        DOCTEST_REQUIRE(r.isFail());
        DOCTEST_CHECK(r.errorCode() == core::ErrorCode::HttpError);
        DOCTEST_CHECK(r.errorMessage().find("401") != std::string::npos);
    }
    {
        test_support::FakeProfileSource profile;
        module::agent::DirectLlmClient client(
            profile, [](const platform::http::Request&) {
                return core::Result<platform::http::Response>::fail(
                    core::ErrorCode::NetworkUnreachable, "connection refused");
            });
        auto r = client.send({{"user", "[]"}}, "[]", "", "default");
        DOCTEST_REQUIRE(r.isFail());
        DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NetworkUnreachable);
        DOCTEST_CHECK(r.errorMessage().find("connection refused") != std::string::npos);
    }
    return true;
})

REGISTER_TEST(direct_llm_client_opencode_dialect,
              "direct_llm_client: opencode 方言（x-opencode-session + 剥 [1M]）；override 可覆盖 host 推断", {
    const auto sendOnce = [](test_support::FakeProfileSource& profile,
                             Capture& cap) {
        module::agent::DirectLlmClient client(profile, cap.fn());
        return client.send({{"user", "[]"}}, "[]", "", "default");
    };
    // host 推断：zen.opencode.ai → 会话头 + 模型名剥 [1M]
    {
        test_support::FakeProfileSource profile;
        profile.profile.baseUrl = "https://zen.opencode.ai";
        profile.profile.defaultModel = "claude-sonnet-4-5[1M]";
        Capture cap;
        DOCTEST_REQUIRE(sendOnce(profile, cap).isOk());
        DOCTEST_CHECK_EQ(cap.req.headers.count("x-opencode-session"), size_t(1));
        DOCTEST_CHECK(cap.req.headers["x-opencode-session"].size() == 32);
        DOCTEST_CHECK(cap.req.body.find("\"model\":\"claude-sonnet-4-5\"") !=
                      std::string::npos);
    }
    // 显式覆盖：自建网关 host 推断不出 → profile 钉 opencode_zen
    {
        test_support::FakeProfileSource profile;
        profile.profile.baseUrl = "https://gw.internal.example";
        profile.profile.dialectOverride = "opencode_zen";
        Capture cap;
        DOCTEST_REQUIRE(sendOnce(profile, cap).isOk());
        DOCTEST_CHECK_EQ(cap.req.headers.count("x-opencode-session"), size_t(1));
    }
    // 显式覆盖成 anthropic_default：opencode host 也不加会话头、不剥标记
    {
        test_support::FakeProfileSource profile;
        profile.profile.baseUrl = "https://zen.opencode.ai";
        profile.profile.dialectOverride = "anthropic_default";
        profile.profile.defaultModel = "claude-sonnet-4-5[1M]";
        Capture cap;
        DOCTEST_REQUIRE(sendOnce(profile, cap).isOk());
        DOCTEST_CHECK_EQ(cap.req.headers.count("x-opencode-session"), size_t(0));
        DOCTEST_CHECK(cap.req.body.find("[1M]") != std::string::npos);
    }
    // 非 opencode 的通用网关：无会话头，模型名原样（[1M] 不剥）
    {
        test_support::FakeProfileSource profile;
        profile.profile.defaultModel = "claude-sonnet-4-5[1M]";
        Capture cap;
        DOCTEST_REQUIRE(sendOnce(profile, cap).isOk());
        DOCTEST_CHECK_EQ(cap.req.headers.count("x-opencode-session"), size_t(0));
        DOCTEST_CHECK(cap.req.body.find("[1M]") != std::string::npos);
    }
    return true;
})

// 解耦硬证据：代理「未运行」的最强形态 = 进程内连服务对象都不存在
// （ClaudeService 单例 null，test_main 每用例复位）+ ServiceLocator 未注册
// IClaudeProxyService。此状态下直连端点仍能组出完整请求并走到 HTTP 发送缝。
REGISTER_TEST(direct_llm_client_sends_without_any_proxy_service,
              "direct_llm_client: 无 ClaudeService 单例 / 无 IClaudeProxyService 注册时仍能组请求并走到发送缝", {
    DOCTEST_REQUIRE_MESSAGE(::module::claude::ClaudeService::global() == nullptr,
                            "前提不成立：进程内存在 ClaudeService 单例（代理服务对象在）");
    DOCTEST_REQUIRE_MESSAGE(
        ::core::ServiceLocator::requireService<::core::IClaudeProxyService>().isFail(),
        "前提不成立：IClaudeProxyService 已注册（代理服务可达）");

    test_support::FakeProfileSource profile;
    Capture cap;
    module::agent::DirectLlmClient client(profile, cap.fn());
    auto r = client.send({{"user", R"([{"type":"text","text":"q"}])"}}, "[]", "",
                         "default");
    DOCTEST_REQUIRE_MESSAGE(r.isOk(), "代理缺席时直连端点失败: " + r.errorMessage());
    DOCTEST_CHECK_EQ(cap.calls, 1);
    DOCTEST_CHECK_EQ(cap.req.url, std::string("https://api.anthropic.com/v1/messages"));
    DOCTEST_CHECK(cap.req.headers.count("x-api-key") == 1);
    return true;
})
