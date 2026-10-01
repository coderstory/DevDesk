// tests/test_modules/test_llm_client.cpp
// LlmClient 用例（agent M1 Task 7）：经本地代理发 /v1/messages 的生产
// LlmEndpoint。三个契约各一条用例：
//   1. 正常链路：预检代理 → POST http://127.0.0.1:<port>/v1/messages →
//      请求体形状（model=槽位解析结果 / max_tokens / messages / tools /
//      system；不夹带 anthropic-version——由代理补注入）→ 响应解析成
//      LlmResponse。HTTP 经 HttpFn 注入假发送，零真实网络。
//   2. 代理未启：send 失败（ValidationError）且**一个包都不发**。
//   3. 槽位解析失败：send 失败且不发包（model 形参传什么都一样——形参语义
//      是槽位键名，LlmClient 内重新解析，H1 定案）。
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

// FakeProxy：实现当前实际的 core::IClaudeProxyService 全部虚方法
//（8 个既有 + resolveModelSlot）。与 FakeProxy 草图的差异以头文件为准。
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
    // F2：接口改按值返回快照（M2 静态审计），桩同口径
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
    // model 形参 = 槽位键名（H1 定案）：LlmClient 内 resolveModelSlot 解析，
    // 进 body 的 model 是 FakeProxy.resolved 的返回值，不是形参原文
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
    // 形参传什么都 fail：解析在 LlmClient 内做，槽位解析不出就没有请求
    auto r = client.send({{"user", "[]"}}, "[]", "", "does-not-matter");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})
