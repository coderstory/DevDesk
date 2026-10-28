// web_fetch_markdown 工具(B2,spec §6.1 与 §10 批次 2):mock fetcher 全分支。
// 硬约束逐条落测试:SSRF 字面量每跳、总时限跨跳、charset 白名单、2 MiB 截断、
// max_chars 钳制与 UTF-8 边界、file: 不触网。描述含 ASCII "mcp" 供定向。
#include "doctest.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/mcp_tool.h"
#include "module_mcp/mcp_tools_builtin.h"
#include "platform/http_client.h"
#include "platform/json.h"

namespace {

using Response = platform::http::Response;

Response makeResponse(int status, std::string body,
                      std::map<std::string, std::string> headers = {}) {
    Response r;
    r.statusCode = status;
    r.body = std::move(body);
    r.headers = std::move(headers);
    return r;
}

Response htmlOk(std::string body) {
    return makeResponse(200, std::move(body),
                        {{"Content-Type", "text/html; charset=utf-8"}});
}

// 脚本化 mock:按序回放响应,记录每个请求(URL / 每跳 timeout),可选每跳延迟
class ScriptedFetcher {
public:
    explicit ScriptedFetcher(std::vector<Response> responses)
        : responses_(std::move(responses)) {}

    core::Result<Response> operator()(const platform::http::Request& request) {
        requests_.push_back(request);
        if (sleepMs_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs_));
        }
        if (cursor_ >= responses_.size()) {
            Response exhausted;
            exhausted.networkError = true;
            exhausted.errorMessage = "mock script exhausted";
            return core::Result<Response>::ok(exhausted);
        }
        return core::Result<Response>::ok(responses_.at(cursor_++));
    }

    void setSleepMs(int ms) { sleepMs_ = ms; }
    const std::vector<platform::http::Request>& requests() const { return requests_; }
    std::size_t requestCount() const { return requests_.size(); }

private:
    std::vector<Response> responses_;
    std::vector<platform::http::Request> requests_;
    std::size_t cursor_ = 0;
    int sleepMs_ = 0;
};

// 注册到局部注册表后执行工具(与生产同一注册入口)
core::Result<core::mcp::ToolResult> runFetch(core::mcp::McpToolRegistry& registry,
                                             const std::string& argsJson) {
    const auto found = registry.find("web_fetch_markdown");
    if (found.isFail()) {
        return core::Result<core::mcp::ToolResult>::fail(core::ErrorCode::NotFound,
                                                         "web_fetch_markdown 未注册");
    }
    return found.value()->execute(argsJson);
}

std::string fetchText(core::mcp::McpToolRegistry& registry, const std::string& url,
                      const std::string& extraArgs = "") {
    const auto result =
        runFetch(registry, "{\"url\":\"" + url + "\"" + extraArgs + "}");
    if (result.isFail()) return "<infra-fail>";
    return result.value().text;
}

// ── 代理参数批(useProxy / proxy)的测试缝 ────────────────────────────

// 探针假实现:按候选精确应答,记录被探的 host:port(优先级序断言用)。
struct FakeProbe {
    std::map<std::string, bool> answers;
    std::vector<std::string> probed;
    bool operator()(const std::string& hostPort) {
        probed.push_back(hostPort);
        const auto it = answers.find(hostPort);
        return it != answers.end() && it->second;
    }
};

module::mcp::ProxyOptions proxyOptsOf(FakeProbe& probe) {
    module::mcp::ProxyOptions opts;
    opts.probe = std::ref(probe);
    return opts;
}

// 既有用例与代理参数正交:注入「首选命中」探针——零真端口探测、走代理路径
// 不追加直连附注,输出与代理批之前逐字节一致;专测代理语义的用例自带
// FakeProbe 独立组,不经此helper。
module::mcp::ProxyOptions legacyProxyOpts() {
    module::mcp::ProxyOptions opts;
    opts.probe = [](const std::string&) { return true; };
    return opts;
}

} // namespace

REGISTER_TEST(mcp_fetch_registered_with_server_info,
              "mcp fetch: web_fetch_markdown 与 server_info 一同注册、schema 合法", {
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(
        registry, [](const platform::http::Request&) {
            return core::Result<Response>::ok(Response{});
        });
    DOCTEST_CHECK_EQ(registry.size(), std::size_t{2});
    const auto found = registry.find("web_fetch_markdown");
    DOCTEST_REQUIRE(found.isOk());
    const auto schema = platform::json::parse(found.value()->inputSchemaJson());
    DOCTEST_REQUIRE(schema.isOk());
    DOCTEST_REQUIRE(schema.value().isObject());
    const auto properties = schema.value().asObject().get("properties").asObject();
    DOCTEST_CHECK(properties.has("url"));
    DOCTEST_CHECK(properties.has("max_chars"));
    DOCTEST_CHECK(properties.has("main_content"));
    const auto required = schema.value().asObject().get("required");
    DOCTEST_REQUIRE(required.isArray());
    DOCTEST_CHECK(required.size() == 1);
    return true;
})

REGISTER_TEST(mcp_fetch_html_converted_to_markdown,
              "mcp fetch: text/html 转 Markdown;文本含引号原样返回(转义归协议层)", {
    ScriptedFetcher script({htmlOk("<h1>标题</h1><p>说 \"引号\" & <b>粗</b></p>")});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const std::string text = fetchText(registry, "https://e.test/page");
    DOCTEST_CHECK(text.find("# 标题") != std::string::npos);
    DOCTEST_CHECK(text.find("说 \"引号\" & **粗**") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_text_plain_passthrough_exact,
              "mcp fetch: text/plain 原文返回,工具层不做二次加工", {
    ScriptedFetcher script(
        {makeResponse(200, "line1\nline2", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://a.test/t.txt"), std::string("line1\nline2"));
    return true;
})

REGISTER_TEST(mcp_fetch_missing_content_type_treated_as_text,
              "mcp fetch: 缺失 Content-Type 按文本(spec §6.1-7)", {
    ScriptedFetcher script({makeResponse(200, "raw body")});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://a.test/n"), std::string("raw body"));
    return true;
})

REGISTER_TEST(mcp_fetch_unsupported_content_type_rejected,
              "mcp fetch: 二进制/未知 Content-Type 拒绝(spec §6.1-7)", {
    for (const char* type : {"application/octet-stream", "image/png"}) {
        ScriptedFetcher script({makeResponse(200, "binary", {{"Content-Type", type}})});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result = runFetch(registry, "{\"url\":\"http://a.test/b\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("不支持的内容类型") != std::string::npos);
    }
    return true;
})

REGISTER_TEST(mcp_fetch_charset_gbk_rejected_without_retry,
              "mcp fetch: charset=gbk 白名单外拒绝且不重试(防非 UTF-8 字节进 JSON)", {
    ScriptedFetcher script(
        {makeResponse(200, "<p>x</p>", {{"Content-Type", "text/html; charset=gbk"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"http://a.test/g\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("不支持非 UTF-8 编码") != std::string::npos);
    DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{1});
    return true;
})

REGISTER_TEST(mcp_fetch_charset_whitelist_passes,
              "mcp fetch: charset=us-ascii / 带引号 UTF8(去引号归一)放行(spec §6.1-8)", {
    ScriptedFetcher script(
        {makeResponse(200, "asc", {{"Content-Type", "text/plain; charset=us-ascii"}}),
         makeResponse(200, "u8", {{"Content-Type", "text/html; charset=\"UTF8\""}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://a.test/1"), std::string("asc"));
    DOCTEST_CHECK(fetchText(registry, "http://a.test/2").find("u8") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_invalid_utf8_sanitized,
              "mcp fetch: charset 缺失时非法字节 sanitize 成 U+FFFD(防坏字节进 JSON)", {
    std::string body = "ok";
    body += "\xFF\xFE";
    body += "end";
    ScriptedFetcher script({makeResponse(200, body)});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const std::string text = fetchText(registry, "http://a.test/bad");
    DOCTEST_CHECK(text.find("ok") != std::string::npos);
    DOCTEST_CHECK(text.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD
    DOCTEST_CHECK(text.find("\xFF") == std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_non_http_scheme_never_touches_network,
              "mcp fetch: file:/ftp:/javascript: 非 http(s) 拒绝且不触网(spec §6.1-1)", {
    for (const char* url : {"file:///etc/passwd", "ftp://h/x", "javascript:alert(1)"}) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"" + std::string(url) + "\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("仅支持 http/https") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_literals_blocked_without_network,
              "mcp fetch: SSRF 字面地址全表拒绝且不触网(spec §6.1-2)", {
    const char* blocked[] = {"localhost",    "LOCALHOST",    "127.0.0.1",
                             "127.255.0.9",  "0.0.0.0",      "10.1.2.3",
                             "172.16.0.9",   "172.31.255.1", "192.168.1.1",
                             "169.254.9.9",  "[::1]",        "[::ffff:127.0.0.1]",
                             "[fe80::1]",    "[fc00::1234]"};
    for (const char* host : blocked) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_numeric_ip_encodings_blocked,
              "mcp fetch: IPv4 数字编码形态(十进制大整数/八进制/十六进制/缩写/填充零)一律还原查表拦截，不得按域名放行", {
    // 防的事故：inet_aton 语义的数字变体被系统解析器还原成回环/私网,
    // 而字面校验只认点分十进制 → 编码形态绕过(spec §14.2 的 DNS 前提被架空)
    const char* blockedHosts[] = {"2130706433", "0177.0.0.1", "0x7f.0.0.1",
                                  "127.1", "127.000000000000000.1", "0177.1",
                                  "0x7f.1", "0x7f000001"};
    for (const char* host : blockedHosts) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_numeric_public_forms_pass,
              "mcp fetch: 公网数字形态(134744072/0x08080808)与纯十六进制词域名不误杀", {
    ScriptedFetcher script(
        {makeResponse(200, "ok1", {{"Content-Type", "text/plain"}}),
         makeResponse(200, "ok2", {{"Content-Type", "text/plain"}}),
         makeResponse(200, "ok3", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://134744072/"),   // = 8.8.8.8
                     std::string("ok1"));
    DOCTEST_CHECK_EQ(fetchText(registry, "http://0x08080808/"), std::string("ok2"));
    // 字母域名（含十六进制词）不落数字形态判定,仍按域名放行
    DOCTEST_CHECK_EQ(fetchText(registry, "http://deadbeef.ca/"), std::string("ok3"));
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_ipv4_compatible_v6_blocked,
              "mcp fetch: ::/96 IPv4-compatible IPv6(如 ::127.0.0.1)还原 v4 查表拦截", {
    // ::127.0.0.1 前七组全零、末段非 0/1、非 ffff 映射——修复前四条判定全不中而放行
    const char* blocked[] = {"[::127.0.0.1]", "[::10.1.2.3]", "[::0.0.0.2]"};
    for (const char* host : blocked) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_userinfo_rejected,
              "mcp fetch: 带 userinfo 的目标(a@127.0.0.1)一律拒，无 userinfo 正常通过", {
    // 防的事故：libcurl 按 RFC 3986 把 @ 后段当真主机实连回环,而自研解析
    // 取到的 host 段含 @ → 判「非字面 IP」放行
    const char* blocked[] = {"a@127.0.0.1", "b@10.1.2.3", "a@localhost"};
    for (const char* host : blocked) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    // 无 userinfo 的普通目标不受影响
    ScriptedFetcher script({makeResponse(200, "ok", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://example.com/"), std::string("ok"));
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_trailing_dot_host_rejected,
              "mcp fetch: host 尾点(127.0.0.1./10.0.0.1./example.com.)一律 fail-closed 拒绝", {
    // 防的事故：WHATWG/curl 剥尾点后按 IPv4 实连,三道字面解析却都判
    // 「非字面 IP」落域名放行;公网域名尾点同拒(保守口径,不给绕过留口)
    const char* blocked[] = {"127.0.0.1.", "10.0.0.1.", "example.com."};
    for (const char* host : blocked) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_multicast_denied,
              "mcp fetch: 组播段 224.0.0.0/4 与 ff00::/8 入 deny 表(OWASP 点名段补齐)", {
    const char* blocked[] = {"224.0.0.1", "[ff02::1]"};
    for (const char* host : blocked) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_unparseable_host_fail_closed,
              "mcp fetch: v6 解析失败(zone-id/畸形)/数字形态解析失败(越界/超段)一律 fail-closed，合法公网 v6 放行", {
    // 防的事故：解析失败走「放行当域名」分支（fe80::1%25eth0 等 zone-id 形态
    // 系统解析器认得,字面校验不认 → 绕过）
    const char* rejected[] = {"[fe80::1%25eth0]",   // zone-id
                              "[fe80::1%eth0]",     // 未转义 zone-id
                              "[1:2:3:4:5:6:7:8:9]",  // 段数超限
                              "1.2.3.4.5.6",          // 数字形态段数超限
                              "999.1.1.1",            // 段值越界
                              "0x"};                  // 裸十六进制前缀
    for (const char* host : rejected) {
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const auto result =
            runFetch(registry, "{\"url\":\"http://" + std::string(host) + "/x\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    // 合法公网 v6 不误杀
    ScriptedFetcher script({makeResponse(200, "ok6", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://[2606:4700::6810:85e5]/"),
                     std::string("ok6"));
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_auto_probe_hits_first_candidate,
              "mcp fetch: useProxy 默认开——自动探测命中首选 7897 即短路并走代理(优先级序)", {
    FakeProbe probe;
    probe.answers["127.0.0.1:7897"] = true;
    ScriptedFetcher script({makeResponse(200, "ok", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, proxyOptsOf(probe));
    const std::string out = fetchText(registry, "http://a.test/x");
    DOCTEST_REQUIRE(script.requestCount() == 1);
    // 命中首选:代理生效且不再探后续候选(优先级短路)
    DOCTEST_CHECK_EQ(script.requests()[0].proxy, std::string("127.0.0.1:7897"));
    DOCTEST_CHECK_EQ(probe.probed.size(), std::size_t{1});
    // 命中路径无直连附注
    DOCTEST_CHECK_EQ(out, std::string("ok"));
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_auto_all_miss_direct_with_note,
              "mcp fetch: 自动探测全不命中 → 直连且响应附注「未检测到本地代理,已直连」", {
    FakeProbe probe;  // 全 false
    ScriptedFetcher script({makeResponse(200, "ok", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, proxyOptsOf(probe));
    const std::string out = fetchText(registry, "http://a.test/x");
    DOCTEST_REQUIRE(script.requestCount() == 1);
    DOCTEST_CHECK_EQ(script.requests()[0].proxy, std::string());
    DOCTEST_CHECK_EQ(probe.probed.size(), std::size_t{3});  // 三候选逐个探尽
    DOCTEST_CHECK_EQ(out, std::string("ok\n未检测到本地代理,已直连"));
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_explicit_ok,
              "mcp fetch: 显式 proxy 预检通过 → 强制走该代理(不经自动探测)", {
    FakeProbe probe;
    probe.answers["127.0.0.1:7897"] = true;
    ScriptedFetcher script({makeResponse(200, "ok", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, proxyOptsOf(probe));
    const std::string out =
        fetchText(registry, "http://a.test/x", ",\"proxy\":\"127.0.0.1:7897\"");
    DOCTEST_REQUIRE(script.requestCount() == 1);
    DOCTEST_CHECK_EQ(script.requests()[0].proxy, std::string("127.0.0.1:7897"));
    DOCTEST_CHECK_EQ(out, std::string("ok"));
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_explicit_unreachable_is_error,
              "mcp fetch: 显式 proxy 预检失败 → isError「显式指定的代理 … 不可用」且不触网", {
    FakeProbe probe;  // 无服务
    ScriptedFetcher script({});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, proxyOptsOf(probe));
    const auto result =
        runFetch(registry, "{\"url\":\"http://a.test/x\",\"proxy\":\"127.0.0.1:7897\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("显式指定的代理 127.0.0.1:7897 不可用") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_conflict_with_use_proxy_false,
              "mcp fetch: proxy 与 useProxy=false 同给 → 参数冲突错误(-32602 口径),不探测不触网", {
    FakeProbe probe;
    ScriptedFetcher script({});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, proxyOptsOf(probe));
    const auto result = runFetch(
        registry,
        "{\"url\":\"http://a.test/x\",\"proxy\":\"127.0.0.1:7897\",\"useProxy\":false}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("参数冲突") != std::string::npos);
    DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    DOCTEST_CHECK(probe.probed.empty());
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_use_proxy_false_direct_no_note,
              "mcp fetch: useProxy=false → 直连、不探测、无附注", {
    FakeProbe probe;
    ScriptedFetcher script({makeResponse(200, "ok", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, proxyOptsOf(probe));
    const std::string out =
        fetchText(registry, "http://a.test/x", ",\"useProxy\":false");
    DOCTEST_REQUIRE(script.requestCount() == 1);
    DOCTEST_CHECK_EQ(script.requests()[0].proxy, std::string());
    DOCTEST_CHECK(probe.probed.empty());
    DOCTEST_CHECK_EQ(out, std::string("ok"));
    return true;
})

REGISTER_TEST(mcp_fetch_proxy_explicit_format_restricted,
              "mcp fetch: proxy 仅接受 IP:port 或 localhost:port(控 SSRF 面),其余形态报错", {
    FakeProbe probe;
    const char* bad[] = {"myproxy.example:8080", "127.0.0.1", "127.0.0.1:0",
                         "127.0.0.1:99999", "[::1]:7897", "user@127.0.0.1:7897"};
    for (const char* p : bad) {
        FakeProbe fresh;
        ScriptedFetcher script({});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                             module::mcp::FetchLimits{},
                                             proxyOptsOf(fresh));
        const auto result = runFetch(
            registry,
            "{\"url\":\"http://a.test/x\",\"proxy\":\"" + std::string(p) + "\"}");
        DOCTEST_REQUIRE(result.isOk());
        DOCTEST_CHECK(result.value().isError);
        DOCTEST_CHECK(result.value().text.find("仅支持") != std::string::npos);
        DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{0});
    }
    return true;
})

REGISTER_TEST(mcp_fetch_public_literals_and_domains_pass,
              "mcp fetch: 公网边界(172.32/ fec0:: / 域名)放行(防 SSRF 误杀)", {
    ScriptedFetcher script(
        {makeResponse(200, "ok1", {{"Content-Type", "text/plain"}}),
         makeResponse(200, "ok2", {{"Content-Type", "text/plain"}}),
         makeResponse(200, "ok3", {{"Content-Type", "text/plain"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    DOCTEST_CHECK_EQ(fetchText(registry, "http://172.32.0.1/"), std::string("ok1"));
    DOCTEST_CHECK_EQ(fetchText(registry, "http://[fec0::1]/"), std::string("ok2"));
    DOCTEST_CHECK_EQ(fetchText(registry, "http://example.com/"), std::string("ok3"));
    return true;
})

REGISTER_TEST(mcp_fetch_ssrf_checked_on_every_hop,
              "mcp fetch: 重定向每一跳都过 SSRF 校验(防重定向绕过)", {
    ScriptedFetcher script(
        {makeResponse(302, "", {{"Location", "http://127.0.0.1/x"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"https://e.test/redirect\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("SSRF") != std::string::npos);
    DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{1});  // 第一跳后止步
    return true;
})

REGISTER_TEST(mcp_fetch_redirect_location_resolution,
              "mcp fetch: 相对/绝对路径/协议相对 Location 解析(spec §6.1-4)", {
    {  // 相对路径:相对当前目录
        ScriptedFetcher script({makeResponse(301, "", {{"Location", "b/c.html"}}),
                                htmlOk("<p>ok</p>")});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        DOCTEST_CHECK(fetchText(registry, "http://a.test/dir/page").find("ok") !=
                      std::string::npos);
        DOCTEST_REQUIRE(script.requestCount() == 2);
        DOCTEST_CHECK_EQ(script.requests()[1].url,
                         std::string("http://a.test/dir/b/c.html"));
    }
    {  // 绝对路径:补 scheme+host
        ScriptedFetcher script({makeResponse(302, "", {{"Location", "/root"}}),
                                htmlOk("<p>ok</p>")});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        DOCTEST_CHECK(fetchText(registry, "https://a.test/deep/x").find("ok") !=
                      std::string::npos);
        DOCTEST_CHECK_EQ(script.requests()[1].url, std::string("https://a.test/root"));
    }
    {  // 协议相对:补当前 scheme
        ScriptedFetcher script({makeResponse(302, "", {{"Location", "//other.test/z"}}),
                                htmlOk("<p>ok</p>")});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        DOCTEST_CHECK(fetchText(registry, "https://a.test/y").find("ok") !=
                      std::string::npos);
        DOCTEST_CHECK_EQ(script.requests()[1].url, std::string("https://other.test/z"));
    }
    return true;
})

REGISTER_TEST(mcp_fetch_redirect_hop_cap,
              "mcp fetch: 超过 5 跳报『重定向次数过多』(防重定向死循环)", {
    std::vector<Response> responses;
    for (int i = 0; i < 7; ++i) {
        responses.push_back(makeResponse(302, "", {{"Location", "/next"}}));
    }
    ScriptedFetcher script(std::move(responses));
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"http://a.test/loop\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("重定向次数过多") != std::string::npos);
    DOCTEST_CHECK_EQ(script.requestCount(), std::size_t{6});  // 首跳 + 5 跳
    return true;
})

REGISTER_TEST(mcp_fetch_redirect_missing_location_and_bad_target,
              "mcp fetch: 缺 Location / 非法目标均报错(spec §6.1-4)", {
    ScriptedFetcher script({makeResponse(302, "")});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto missing = runFetch(registry, "{\"url\":\"http://a.test/m\"}");
    DOCTEST_CHECK(missing.value().text.find("重定向缺少 Location") != std::string::npos);

    ScriptedFetcher badTarget({makeResponse(302, "", {{"Location", "ftp://h/x"}})});
    core::mcp::McpToolRegistry registry2;
    module::mcp::registerBuiltinMcpTools(registry2, std::ref(badTarget),
                                         module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto bad = runFetch(registry2, "{\"url\":\"http://a.test/n\"}");
    DOCTEST_CHECK(bad.value().text.find("无法解析为 http/https") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_non_2xx_reports_status_and_body_prefix,
              "mcp fetch: 非 2xx 报状态码 + 响应体前 200 字符(UTF-8 边界安全)", {
    const std::string body = std::string(195, 'a') + "MMMM" + std::string(300, 'z');
    ScriptedFetcher script({makeResponse(404, body)});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"http://a.test/gone\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("HTTP 404") != std::string::npos);
    DOCTEST_CHECK(result.value().text.find("MMMM") != std::string::npos);
    DOCTEST_CHECK(result.value().text.find("zzzzzzzzzz") == std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_non_2xx_cjk_prefix_counts_chars_not_bytes,
              "mcp fetch: 非 2xx 错误页前缀按 UTF-8 码点截 200 字符(防 CJK 页面按字节只显 66 字)", {
    // 300 个三字节汉字 = 900 字节：按字节截 200 只剩 66 字(修复前),按码点应满 200 字
    const std::string cjk = "\xE9\x94\x99";  // 「错」U+9519
    std::string body;
    for (int i = 0; i < 300; ++i) body += cjk;
    ScriptedFetcher script({makeResponse(500, body)});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"http://a.test/err\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    std::string want;
    for (int i = 0; i < 200; ++i) want += cjk;
    DOCTEST_CHECK(result.value().text.find(want) != std::string::npos);  // 满 200 字连续出现
    DOCTEST_CHECK(result.value().text.find(want + cjk) == std::string::npos);  // 不超 200 字
    return true;
})

REGISTER_TEST(mcp_fetch_body_over_2mib_truncation_note,
              "mcp fetch: 原始响应超 2 MiB 截断并附注(spec §6.1-9)", {
    std::string body = "<p>hello</p><!--";
    body += std::string(2 * 1024 * 1024 + 50, 'a');
    body += "-->";
    ScriptedFetcher script(
        {makeResponse(200, body, {{"Content-Type", "text/html"}})});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const std::string text = fetchText(registry, "http://a.test/big");
    DOCTEST_CHECK(text.find("hello") != std::string::npos);
    DOCTEST_CHECK(text.find("（原始响应超过 2 MiB，已截断）") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_max_chars_clamped_and_utf8_boundary_cut,
              "mcp fetch: max_chars 钳制 1000..200000、按码点 UTF-8 边界截断(spec §6.1-10)", {
    {  // 下溢钳到 1000
        ScriptedFetcher script({makeResponse(200, std::string(1500, 'a'),
                                             {{"Content-Type", "text/plain"}})});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const std::string text =
            fetchText(registry, "http://a.test/lo", ",\"max_chars\":1");
        DOCTEST_CHECK(text.substr(0, 1000) == std::string(1000, 'a'));
        DOCTEST_CHECK(text.find("（已截断到 1000 字符）") != std::string::npos);
    }
    {  // CJK 按码点截断,不产生半个字符(max_chars 合法下界 1000)
        std::string body;
        for (int i = 0; i < 1500; ++i) body += "好";
        ScriptedFetcher script(
            {makeResponse(200, body, {{"Content-Type", "text/plain"}})});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const std::string text =
            fetchText(registry, "http://a.test/cjk", ",\"max_chars\":1000");
        DOCTEST_CHECK(text.find("（已截断到 1000 字符）") != std::string::npos);
        // 截断点 = 第 1000 个字符(3000 字节)处,附注紧跟其后,无半个字符
        DOCTEST_CHECK(text.compare(0, 12, "\xE5\xA5\xBD\xE5\xA5\xBD\xE5\xA5\xBD\xE5\xA5\xBD") == 0);
        DOCTEST_CHECK(text.size() == std::string("（已截断到 1000 字符）").size() + 3000);
    }
    {  // 上溢钳到 200000
        ScriptedFetcher script({makeResponse(200, std::string(200001, 'c'),
                                             {{"Content-Type", "text/plain"}})});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const std::string text =
            fetchText(registry, "http://a.test/hi", ",\"max_chars\":999999");
        DOCTEST_CHECK(text.find("（已截断到 200000 字符）") != std::string::npos);
    }
    {  // 非数字 max_chars 忽略 → 默认 60000
        ScriptedFetcher script({makeResponse(200, std::string(60001, 'b'),
                                             {{"Content-Type", "text/plain"}})});
        core::mcp::McpToolRegistry registry;
        module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
        const std::string text =
            fetchText(registry, "http://a.test/def", ",\"max_chars\":\"x\"");
        DOCTEST_CHECK(text.find("（已截断到 60000 字符）") != std::string::npos);
    }
    return true;
})

REGISTER_TEST(mcp_fetch_empty_visible_text_hint,
              "mcp fetch: HTML 无可见文本非错误,返回提示(spec §6.1-11)", {
    ScriptedFetcher script({htmlOk("<html><body>   </body></html>")});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"https://e.test/js\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);
    DOCTEST_CHECK_EQ(result.value().text,
                     std::string("页面未提取到文本（可能是 JS 渲染页面）"));
    return true;
})

REGISTER_TEST(mcp_fetch_total_timeout_across_hops,
              "mcp fetch: 所有跳合计总时限耗尽 → 超时错误(测试注入小预算,生产默认 10s)", {
    std::vector<Response> responses;
    for (int i = 0; i < 10; ++i) {
        responses.push_back(makeResponse(302, "", {{"Location", "/next"}}));
    }
    ScriptedFetcher script(std::move(responses));
    script.setSleepMs(80);
    module::mcp::FetchLimits limits;
    limits.totalBudget = std::chrono::milliseconds(200);
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), limits,
                                         legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"http://a.test/slow\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("超时") != std::string::npos);
    DOCTEST_CHECK(result.value().text.find("次数过多") == std::string::npos);
    DOCTEST_CHECK(script.requestCount() < std::size_t{10});
    return true;
})

REGISTER_TEST(mcp_fetch_network_error_readable_message,
              "mcp fetch: 网络失败返回可读中文 + 底层信息(spec §6.1-12)", {
    Response failure;
    failure.networkError = true;
    failure.errorMessage = "connection reset";
    ScriptedFetcher script({failure});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script), module::mcp::FetchLimits{}, legacyProxyOpts());
    const auto result = runFetch(registry, "{\"url\":\"http://a.test/dead\"}");
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK(result.value().text.find("网络请求失败") != std::string::npos);
    DOCTEST_CHECK(result.value().text.find("connection reset") != std::string::npos);
    return true;
})

// ── 正文主干参数(CE 批):main_content 默认 true;false 规则全关断保底 ──

REGISTER_TEST(mcp_fetch_main_content_default_strips_boilerplate,
              "mcp fetch: main_content 缺省 true——导航/页脚样板不入输出,正文保留", {
    const std::string page =
        "<html><body><header><a href=\"/\">站头导航</a></header>"
        "<main><h1>正文标题</h1><p>正文段落内容。</p></main>"
        "<footer><a href=\"/f\">页脚链接</a></footer></body></html>";
    ScriptedFetcher script({htmlOk(page)});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, legacyProxyOpts());
    const std::string text = fetchText(registry, "https://e.test/doc");
    DOCTEST_CHECK(text.find("# 正文标题") != std::string::npos);
    DOCTEST_CHECK(text.find("正文段落内容") != std::string::npos);
    DOCTEST_CHECK(text.find("站头导航") == std::string::npos);
    DOCTEST_CHECK(text.find("页脚链接") == std::string::npos);
    return true;
})

REGISTER_TEST(mcp_fetch_main_content_false_keeps_everything,
              "mcp fetch: main_content=false——规则全关断,输出与提取功能引入前逐字节同口径", {
    const std::string page =
        "<html><body><header><a href=\"/\">站头导航</a></header>"
        "<main><h1>正文标题</h1><p>正文段落内容。</p></main>"
        "<footer><a href=\"/f\">页脚链接</a></footer></body></html>";
    ScriptedFetcher script({htmlOk(page)});
    core::mcp::McpToolRegistry registry;
    module::mcp::registerBuiltinMcpTools(registry, std::ref(script),
                                         module::mcp::FetchLimits{}, legacyProxyOpts());
    const std::string text =
        fetchText(registry, "https://e.test/doc", ",\"main_content\":false");
    DOCTEST_CHECK(text.find("# 正文标题") != std::string::npos);
    DOCTEST_CHECK(text.find("正文段落内容") != std::string::npos);
    DOCTEST_CHECK(text.find("[站头导航](/)") != std::string::npos);
    DOCTEST_CHECK(text.find("[页脚链接](/f)") != std::string::npos);
    return true;
})
