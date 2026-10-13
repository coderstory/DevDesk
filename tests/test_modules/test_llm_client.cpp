// tests/test_modules/test_llm_client.cpp
// LlmClient 三条契约：正常链路（预检代理 → POST 本地代理 → 解析 LlmResponse）、代理未启、槽位解析失败。
// 两条失败契约都要求「一个包都不发」。
//
// 跨文件前提：
//   - model 形参是槽位**键名**，LlmClient 内部自行 resolveModelSlot 后才进 body——形参原文不进 body。
//     因此换形参值不改变结果，用例 ③ 传什么都是同一个解析失败。
//   - 请求体不夹带 anthropic-version：由代理侧补注入。
//   - HTTP 经 HttpFn 注入假发送，本文件零真实网络。
#include "doctest.h"
#include "core/claude_proxy_service.h"
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "module_agent/llm_client.h"
#include "platform/http_client.h"
#include <optional>
#include <string>
#include <vector>

namespace {

// 实现 core::IClaudeProxyService 全部虚方法（新增虚函数会编译失败——桩必须跟着补）
class FakeProxy : public core::IClaudeProxyService {
public:
    bool running = true;
    int port = 8787;
    std::optional<std::string> resolved = std::string("claude-sonnet-5-5");

    bool isProxyRunning() const override { return running; }
    int proxyPort() const override { return port; }
    void setConfiguredProxyPort(int) override {}
    void stopProxy() override {}
    core::Result<int> startProxy(const std::string&, int) override {
        return core::Result<int>::fail(core::ErrorCode::Internal, "unused");
    }
    bool isClaudeCodeManaged() const override { return false; }
    std::vector<std::string> lastSyncWarnings() const override { return {}; }
    std::string settingsFilePath() const override { return {}; }
    std::optional<std::string> resolveModelSlot(const std::string& slot) override {
        (void)slot;
        return resolved;
    }
};

} // namespace

REGISTER_TEST(llm_client_sends_via_local_proxy, "llm_client: 预检代理→POST 127.0.0.1:<port>/v1/messages→解析 LlmResponse", {
    FakeProxy proxy;
    platform::http::Request captured;
    int calls = 0;
    module::agent::LlmClient client(proxy, [&](const platform::http::Request& r) {
        ++calls;
        captured = r;
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[{"type":"text","text":"hi"}],"usage":{"input_tokens":11,"output_tokens":7}})";
        return core::Result<platform::http::Response>::ok(res);
    });
    std::vector<core::LlmMessage> msgs{{"user", R"([{"type":"text","text":"q"}])"}};
    // body 里的 model 是 FakeProxy.resolved 的返回值，不是形参原文
    auto r = client.send(msgs, R"([{"name":"t.a"}])", "sys-prompt", "default");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK(captured.url.find("http://127.0.0.1:8787/v1/messages") != std::string::npos);
    DOCTEST_CHECK(captured.method == "POST");
    DOCTEST_CHECK(captured.body.find("\"model\":\"claude-sonnet-5-5\"") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("\"max_tokens\":4096") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("\"system\":\"sys-prompt\"") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("\"tools\":[{\"name\":\"t.a\"}]") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("anthropic-version") == std::string::npos); // 代理补注入
    DOCTEST_CHECK_EQ(r.value().stopReason, std::string("end_turn"));
    DOCTEST_CHECK_EQ(r.value().inputTokens, 11);
    DOCTEST_CHECK_EQ(r.value().outputTokens, 7);
    // content 原文透传给 AgentLoop（含 text block）
    DOCTEST_CHECK(r.value().contentJson.find("\"text\":\"hi\"") != std::string::npos);
    return true;
})

REGISTER_TEST(llm_client_preflight_proxy_not_running, "llm_client: 代理未启→fail 不发包", {
    FakeProxy proxy;
    proxy.running = false;
    int calls = 0;
    module::agent::LlmClient client(proxy, [&](const platform::http::Request&) {
        ++calls;
        return core::Result<platform::http::Response>::fail(core::ErrorCode::NetworkUnreachable, "x");
    });
    auto r = client.send({{"user", "[]"}}, "[]", "", "m");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})

REGISTER_TEST(llm_client_slot_unresolved_fails_without_send, "llm_client: 槽位解析失败→fail 不发包", {
    FakeProxy proxy;
    proxy.resolved = std::nullopt;
    int calls = 0;
    module::agent::LlmClient client(proxy, [&](const platform::http::Request&) {
        ++calls;
        return core::Result<platform::http::Response>::fail(core::ErrorCode::NetworkUnreachable, "x");
    });
    auto r = client.send({{"user", "[]"}}, "[]", "", "does-not-matter");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})
