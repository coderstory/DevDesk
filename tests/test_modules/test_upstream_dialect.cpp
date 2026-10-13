// tests/test_modules/test_upstream_dialect.cpp
// 上游方言（UpstreamDialect）单元测试：[1M] 后缀剥离 / baseUrl 分派 / 请求头方言化 /
// 请求体改写 / 流式路径端到端。
// 跨文件前提：
//   - 方言来自 baseUrl 的 host 推断，也可用 profile 的 dialectOverride 显式钉住；
//     未知或空值一律回落 host 推断，不静默换方言。
//   - applyHeaders 只做「覆写 + 清除大小写变体」，非方言所有的头原样透传。
#include "doctest.h"

#include "claude_service.h"
#include "platform/http_client.h"
#include "platform/http_server.h"
#include "streaming_proxy.h"
#include "test_support.h"
#include "upstream_dialect.h"

#include <cctype>
#include <chrono>
#include <map>
#include <string>
#include <thread>

namespace {

using module::claude::UpstreamDialect;
using test_support::countCI;
using test_support::headerCI;
using test_support::HeaderMap;

// 客户端（Claude Code / python-urllib 直测）发来的头部形态
HeaderMap clientHeaders() {
    HeaderMap h;
    h["user-agent"] = "Python-urllib/3.14";
    h["x-api-key"] = "sk-client-bogus";
    h["authorization"] = "Bearer sk-client-bogus";
    h["content-type"] = "application/json";
    h["accept"] = "text/event-stream";
    return h;
}

bool hasCI(const HeaderMap& h, const std::string& lowerKey) {
    return countCI(h, lowerKey) > 0;
}

} // namespace

REGISTER_TEST(dialect_strip_one_m_suffix,
              "upstream_dialect: stripOneMSuffix 大小写/空白/未命中", {
    using module::claude::stripOneMSuffix;
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash[1M]"),
                     std::string("deepseek-v4-flash"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash[1m]"),
                     std::string("deepseek-v4-flash"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash [1m]"),
                     std::string("deepseek-v4-flash"));
    // 标记前带空格（上游配置里常见）
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash [1M]"),
                     std::string("deepseek-v4-flash"));
    // 尾部空白
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash[1M]   "),
                     std::string("deepseek-v4-flash"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash [1M]\t"),
                     std::string("deepseek-v4-flash"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("deepseek-v4-flash"),
                     std::string("deepseek-v4-flash"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("claude-opus-4-5[2M]"),
                     std::string("claude-opus-4-5[2M]"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("bailian/glm-5.2"),
                     std::string("bailian/glm-5.2"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("a[1M]b"), std::string("a[1M]b"));
    DOCTEST_CHECK_EQ(stripOneMSuffix("[1M]"), std::string(""));
    DOCTEST_CHECK_EQ(stripOneMSuffix(""), std::string(""));
    DOCTEST_CHECK_EQ(stripOneMSuffix("[1M"), std::string("[1M"));
    return true;
})

REGISTER_TEST(dialect_for_base_url,
              "upstream_dialect: forBaseUrl host 分派", {
    using module::claude::UpstreamDialect;
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://opencode.ai/zen/go").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://zen.opencode.ai/v1").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("HTTPS://OPENCODE.AI/zen/go").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("http://opencode.ai:8080/zen").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    // host 标签边界：形似但不同域不算
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://notopencode.ai/x").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://opencode.ai.evil.com/x").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://api.anthropic.com").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl(
                      "https://codingxrui.geely-test.com/bailian/anthropic").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("http://127.0.0.1:1234").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    // 只有 opencodeZen 剥模型标记
    DOCTEST_CHECK_FALSE(UpstreamDialect::anthropicDefault().stripModelSuffix());
    DOCTEST_CHECK_TRUE(UpstreamDialect::opencodeZen().stripModelSuffix());
    return true;
})

// 自建网关 / 内网镜像的 host 后缀推断不出正确方言（zen 镜像被判成 opencodeZen，
// 或内网反代被当成默认 → 模型名 [1M] 不剥被上游 400），profile 侧可显式钉住。
REGISTER_TEST(dialect_for_base_url_override,
              "upstream_dialect: forBaseUrl 覆盖参数优先于 host 推断", {
    using module::claude::UpstreamDialect;
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://api.anthropic.com",
                                              "opencode_zen").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl(
                      "https://codingxrui.geely-test.com/bailian/anthropic",
                      "opencode_zen").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    // 反向：host 像 opencode.ai（镜像 / 反代）但钉成通用方言
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://opencode.ai/zen/go",
                                              "anthropic_default").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://zen.opencode.ai/v1",
                                              "anthropic_default").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);

    // 覆盖后的**行为**随方言走（方言的价值在这两条上）
    const UpstreamDialect forcedZen =
        UpstreamDialect::forBaseUrl("https://api.anthropic.com", "opencode_zen");
    DOCTEST_CHECK_TRUE(forcedZen.stripModelSuffix());
    HeaderMap hz;
    forcedZen.applyHeaders(hz, "sk-x", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(countCI(hz, "x-opencode-session"), 1);
    std::string bodyZen = R"({"model":"deepseek-v4-flash[1M]"})";
    DOCTEST_CHECK_TRUE(forcedZen.normalizeBody(bodyZen));
    DOCTEST_CHECK_EQ(bodyZen, std::string(R"({"model":"deepseek-v4-flash"})"));

    const UpstreamDialect forcedDefault =
        UpstreamDialect::forBaseUrl("https://opencode.ai/zen/go",
                                    "anthropic_default");
    DOCTEST_CHECK_FALSE(forcedDefault.stripModelSuffix());
    HeaderMap hd;
    forcedDefault.applyHeaders(hd, "sk-x", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(countCI(hd, "x-opencode-session"), 0);
    std::string bodyDef = R"({"model":"deepseek-v4-flash[1M]"})";
    DOCTEST_CHECK_FALSE(forcedDefault.normalizeBody(bodyDef));
    DOCTEST_CHECK_EQ(bodyDef, std::string(R"({"model":"deepseek-v4-flash[1M]"})"));

    // 空串 = 未覆盖 → 与单参数调用逐项一致
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://opencode.ai/zen/go", "").kind ==
                  UpstreamDialect::forBaseUrl("https://opencode.ai/zen/go").kind);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://api.anthropic.com", "").kind ==
                  UpstreamDialect::forBaseUrl("https://api.anthropic.com").kind);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://zen.opencode.ai/v1", "").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    // 未知 / 拼写错误 = 未覆盖（回落到 host 推断，不是静默换方言）
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://zen.opencode.ai/v1",
                                              "Opencode_Zen").kind ==
                  UpstreamDialect::Kind::OpencodeZen);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("https://api.anthropic.com",
                                              "opencode-zen").kind ==
                  UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(UpstreamDialect::forBaseUrl("", "opencode_zen").kind ==
                  UpstreamDialect::Kind::OpencodeZen);   // 覆盖不依赖 baseUrl
    return true;
})

// 值域映射（写侧规范化 / 读侧兜底两处都用它，与 forBaseUrl 覆盖互为两面）

REGISTER_TEST(dialect_kind_text_mapping,
              "upstream_dialect: dialectKindFromText/ToText 值域", {
    using module::claude::UpstreamDialect;
    using module::claude::dialectKindFromText;
    using module::claude::dialectKindToText;

    const auto def = dialectKindFromText("anthropic_default");
    const auto zen = dialectKindFromText("opencode_zen");
    DOCTEST_REQUIRE(def.has_value());
    DOCTEST_REQUIRE(zen.has_value());
    DOCTEST_CHECK(*def == UpstreamDialect::Kind::AnthropicDefault);
    DOCTEST_CHECK(*zen == UpstreamDialect::Kind::OpencodeZen);
    // 未覆盖 / 未知 → nullopt（调用方回落 host 推断）
    DOCTEST_CHECK_FALSE(dialectKindFromText("").has_value());
    DOCTEST_CHECK_FALSE(dialectKindFromText("zen").has_value());
    DOCTEST_CHECK_FALSE(dialectKindFromText("opencodeZen").has_value());
    DOCTEST_CHECK_FALSE(dialectKindFromText(" anthropic_default").has_value());
    // 反向：Kind → 规范文本（写侧落库值）
    DOCTEST_CHECK_EQ(std::string(dialectKindToText(UpstreamDialect::Kind::AnthropicDefault)),
                     std::string("anthropic_default"));
    DOCTEST_CHECK_EQ(std::string(dialectKindToText(UpstreamDialect::Kind::OpencodeZen)),
                     std::string("opencode_zen"));
    return true;
})

REGISTER_TEST(dialect_apply_headers_default,
              "upstream_dialect: 默认方言覆盖 UA + 鉴权头（无重复头）", {
    HeaderMap h = clientHeaders();
    UpstreamDialect::anthropicDefault().applyHeaders(h, "sk-profile-key", module::claude::AuthField::Both);

    // UA 无条件覆盖：客户端是 python-urllib（会被 Cloudflare 403 封）也不能泄漏
    DOCTEST_CHECK_EQ(headerCI(h, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(h, "user-agent"), 1);

    DOCTEST_CHECK_EQ(headerCI(h, "x-api-key"), std::string("sk-profile-key"));
    DOCTEST_CHECK_EQ(countCI(h, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(headerCI(h, "authorization"), std::string("Bearer sk-profile-key"));
    DOCTEST_CHECK_EQ(countCI(h, "authorization"), 1);

    DOCTEST_CHECK_EQ(headerCI(h, "anthropic-version"), std::string("2023-06-01"));

    DOCTEST_CHECK_FALSE(hasCI(h, "x-opencode-session"));

    DOCTEST_CHECK_EQ(headerCI(h, "content-type"), std::string("application/json"));
    DOCTEST_CHECK_EQ(headerCI(h, "accept"), std::string("text/event-stream"));
    return true;
})

REGISTER_TEST(dialect_apply_headers_client_variants,
              "upstream_dialect: 客户端头大小写变体被清干净 + 不覆盖显式版本", {
    HeaderMap h = {
        {"User-Agent", "curl/8.22.0"},
        {"X-Api-Key", "sk-client"},
        {"Authorization", "Bearer sk-client"},
        {"Anthropic-Version", "2024-06-01"},
    };
    UpstreamDialect::anthropicDefault().applyHeaders(h, "sk-profile-key", module::claude::AuthField::Both);

    DOCTEST_CHECK_EQ(countCI(h, "user-agent"), 1);
    DOCTEST_CHECK_EQ(headerCI(h, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(h, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(countCI(h, "authorization"), 1);
    DOCTEST_CHECK_EQ(headerCI(h, "x-api-key"), std::string("sk-profile-key"));
    // 客户端显式给的版本优先于代理默认值
    DOCTEST_CHECK_EQ(headerCI(h, "anthropic-version"), std::string("2024-06-01"));
    DOCTEST_CHECK_EQ(countCI(h, "anthropic-version"), 1);

    // 空 key：客户端带来的鉴权头也一并清掉，不能留着裸奔上游
    HeaderMap h2 = clientHeaders();
    UpstreamDialect::anthropicDefault().applyHeaders(h2, "", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(countCI(h2, "x-api-key"), 0);
    DOCTEST_CHECK_EQ(countCI(h2, "authorization"), 0);
    return true;
})

REGISTER_TEST(dialect_apply_headers_opencode_session,
              "upstream_dialect: opencodeZen 注入会话头但不覆盖客户端已有值", {
    // 客户端没带 → 注入代理实例级 id（进程内稳定值，靠它保上游缓存亲和）
    HeaderMap h = clientHeaders();
    UpstreamDialect::opencodeZen().applyHeaders(h, "sk-profile-key", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(headerCI(h, "x-opencode-session"),
                     module::claude::claudeUpstreamSessionId());
    DOCTEST_CHECK_EQ(countCI(h, "x-opencode-session"), 1);

    // 客户端带了 → 原样保留（尊重其会话亲和意图）
    HeaderMap h2 = clientHeaders();
    h2["x-opencode-session"] = "client-session-42";
    UpstreamDialect::opencodeZen().applyHeaders(h2, "sk-profile-key", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(headerCI(h2, "x-opencode-session"),
                     std::string("client-session-42"));
    DOCTEST_CHECK_EQ(countCI(h2, "x-opencode-session"), 1);

    // 大写变体也算"客户端已带"
    HeaderMap h3 = clientHeaders();
    h3["X-Opencode-Session"] = "client-session-43";
    UpstreamDialect::opencodeZen().applyHeaders(h3, "sk-profile-key", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(headerCI(h3, "x-opencode-session"),
                     std::string("client-session-43"));
    DOCTEST_CHECK_EQ(countCI(h3, "x-opencode-session"), 1);
    return true;
})

// 认证字段三态

REGISTER_TEST(dialect_apply_headers_auth_field_three_states,
              "upstream_dialect: 认证字段三态（both/bearer/api_key × 有 key/空 key）", {
    const std::string key = "sk-test";
    HeaderMap both;
    HeaderMap bearer;
    HeaderMap apiKeyOnly;
    HeaderMap emptyKey;
    const auto dialect = UpstreamDialect::anthropicDefault();
    dialect.applyHeaders(both, key, module::claude::AuthField::Both);
    dialect.applyHeaders(bearer, key, module::claude::AuthField::Bearer);
    dialect.applyHeaders(apiKeyOnly, key, module::claude::AuthField::ApiKey);
    dialect.applyHeaders(emptyKey, "", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(countCI(both, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(countCI(both, "authorization"), 1);
    // bearer：只有 Authorization（负断言是核心）
    DOCTEST_CHECK_EQ(countCI(bearer, "x-api-key"), 0);
    DOCTEST_CHECK_EQ(countCI(bearer, "authorization"), 1);
    DOCTEST_CHECK_EQ(countCI(apiKeyOnly, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(countCI(apiKeyOnly, "authorization"), 0);
    // 空 key：两个都不注入
    DOCTEST_CHECK_EQ(countCI(emptyKey, "x-api-key"), 0);
    DOCTEST_CHECK_EQ(countCI(emptyKey, "authorization"), 0);
    DOCTEST_CHECK_EQ(countCI(bearer, "user-agent"), 1);
    DOCTEST_CHECK_EQ(countCI(bearer, "anthropic-version"), 1);
    return true;
})

REGISTER_TEST(dialect_apply_headers_auth_field_clears_client_creds,
              "upstream_dialect: 三态都清客户端凭据（大小写变体）", {
    const auto dialect = UpstreamDialect::anthropicDefault();
    HeaderMap hb = {
        {"x-api-key", "sk-client-bogus"},
        {"X-Api-Key", "sk-client-bogus-2"},
        {"authorization", "Bearer sk-client-bogus"},
    };
    dialect.applyHeaders(hb, "sk-profile-key", module::claude::AuthField::Bearer);
    DOCTEST_CHECK_EQ(countCI(hb, "x-api-key"), 0);
    DOCTEST_CHECK_EQ(countCI(hb, "authorization"), 1);
    DOCTEST_CHECK_EQ(headerCI(hb, "authorization"),
                     std::string("Bearer sk-profile-key"));
    HeaderMap ha = {
        {"Authorization", "Bearer sk-client-bogus"},
        {"X-Api-Key", "sk-client-bogus"},
    };
    dialect.applyHeaders(ha, "sk-profile-key", module::claude::AuthField::ApiKey);
    DOCTEST_CHECK_EQ(countCI(ha, "authorization"), 0);
    DOCTEST_CHECK_EQ(countCI(ha, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(headerCI(ha, "x-api-key"), std::string("sk-profile-key"));
    HeaderMap hboth = clientHeaders();
    dialect.applyHeaders(hboth, "sk-profile-key", module::claude::AuthField::Both);
    DOCTEST_CHECK_EQ(countCI(hboth, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(countCI(hboth, "authorization"), 1);
    DOCTEST_CHECK_EQ(headerCI(hboth, "x-api-key"), std::string("sk-profile-key"));
    DOCTEST_CHECK_EQ(headerCI(hboth, "authorization"),
                     std::string("Bearer sk-profile-key"));
    return true;
})

REGISTER_TEST(dialect_normalize_body_strips_model,
              "upstream_dialect: normalizeBody 剥顶层 model，其余字节不动", {
    const std::string body =
        R"({"model":"deepseek-v4-flash[1M]","max_tokens":32,"stream":true,"messages":[{"role":"user","content":"hi"}]})";
    const std::string expected =
        R"({"model":"deepseek-v4-flash","max_tokens":32,"stream":true,"messages":[{"role":"user","content":"hi"}]})";

    std::string b = body;
    DOCTEST_CHECK_TRUE(UpstreamDialect::opencodeZen().normalizeBody(b));
    DOCTEST_CHECK_EQ(b, expected);

    // 默认方言不碰 body（bailian 带 [1M] 能正常返回，剥了反而改行为）
    std::string b2 = body;
    DOCTEST_CHECK_FALSE(UpstreamDialect::anthropicDefault().normalizeBody(b2));
    DOCTEST_CHECK_EQ(b2, body);
    return true;
})

REGISTER_TEST(dialect_normalize_body_edge_cases,
              "upstream_dialect: normalizeBody 只认顶层 model / 不改坏 body", {
    const UpstreamDialect d = UpstreamDialect::opencodeZen();

    // 嵌套 model（tool schema 里的同名键）不动，只动顶层
    std::string nested =
        R"({"model":"m[1M]","tools":[{"input_schema":{"properties":{"model":{"type":"string"}}}}]})";
    DOCTEST_CHECK_TRUE(d.normalizeBody(nested));
    DOCTEST_CHECK_EQ(nested,
                     std::string(R"({"model":"m","tools":[{"input_schema":{"properties":{"model":{"type":"string"}}}}]})"));

    // 字符串值里出现的 "model": "x[1M]" 不能误伤
    std::string inString =
        R"({"system":"write \"model\": \"z[1M]\" here","model":"m[1M]"})";
    DOCTEST_CHECK_TRUE(d.normalizeBody(inString));
    DOCTEST_CHECK_EQ(inString,
                     std::string(R"({"system":"write \"model\": \"z[1M]\" here","model":"m"})"));

    // 无标记 → 一个字节都不动
    const std::string plain = R"({"model":"deepseek-v4-flash","stream":false})";
    std::string p1 = plain;
    DOCTEST_CHECK_FALSE(d.normalizeBody(p1));
    DOCTEST_CHECK_EQ(p1, plain);

    // 无 model 字段 / model 非字符串 / body 非 JSON → 一个字节都不动
    const std::string noModel = R"({"max_tokens":1,"stream":true})";
    std::string p2 = noModel;
    DOCTEST_CHECK_FALSE(d.normalizeBody(p2));
    DOCTEST_CHECK_EQ(p2, noModel);

    const std::string numModel = R"({"model":123})";
    std::string p3 = numModel;
    DOCTEST_CHECK_FALSE(d.normalizeBody(p3));
    DOCTEST_CHECK_EQ(p3, numModel);

    const std::string notJson = "not a json body [1M]";
    std::string p4 = notJson;
    DOCTEST_CHECK_FALSE(d.normalizeBody(p4));
    DOCTEST_CHECK_EQ(p4, notJson);

    std::string empty;
    DOCTEST_CHECK_FALSE(d.normalizeBody(empty));
    DOCTEST_CHECK_EQ(empty, std::string(""));
    return true;
})

REGISTER_TEST(dialect_streaming_path_applies_dialect,
              "upstream_dialect: sendStreamingClaude 覆盖 UA / 注入 key", {
    platform::http::Server upstream("127.0.0.1", 0);
    HeaderMap seen;  // mock 上游实际收到的头
    upstream.route("POST", R"(/(.*))",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       seen = ctx.headers;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "text/event-stream");
                       w.write("data: [DONE]\n\n");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    module::claude::Profile p;
    p.id = 1;
    p.name = "dialect-stream-test";
    p.baseUrl = "http://127.0.0.1:" + std::to_string(upstream.actualPort());
    p.apiKeyPlain = "sk-profile-key";

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"model":"deepseek-v4-flash[1M]","stream":true,"max_tokens":8})";
    req.headers = clientHeaders();   // 客户端 UA = python-urllib
    req.timeoutMs = 5000;

    auto r = module::claude::sendStreamingClaude(req, p);
    DOCTEST_REQUIRE(r.isOk());
    auto sr = std::move(r.value());
    DOCTEST_REQUIRE(sr != nullptr);
    DOCTEST_REQUIRE(sr->waitForStatus(5000));
    DOCTEST_CHECK_EQ(sr->statusCode(), 200);
    sr->cancel();

    // 上游看到的是代理的 UA，不是客户端的 python-urllib
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    // 鉴权头换成 profile 的 key（客户端那份不算数）
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"), std::string("sk-profile-key"));
    DOCTEST_CHECK_EQ(countCI(seen, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(countCI(seen, "authorization"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"), std::string("2023-06-01"));
    // 127.0.0.1 是默认方言 → 不注入会话头
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 0);

    upstream.stop();
    return true;
})
