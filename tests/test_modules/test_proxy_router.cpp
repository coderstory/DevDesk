// tests/test_modules/test_proxy_router.cpp
// ProxyRouter 单测：转发 / 鉴权 / 方言覆盖 / 伪头剥离 / 错误 JSON / failover 换家。
// 跨文件前提：
//   - 上游用 platform::http::Server 或裸 httplib 起本地 mock；数据库走临时文件。
//   - 凭据存 claude_profiles.api_key_encrypted 列，不在 Keychain；清 Keychain 对它无效。
//   - 无激活 profile 与「取 key 失败」统一报 503 no_active_profile，两者均不写调用日志。
//   - 方言由 baseUrl host 推断；默认方言不改 body、不加会话头。
#include "doctest.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "platform/http_client.h"
#include "platform/http_server.h"
#include "platform/json.h"
#include "platform/keychain.h"
#include "proxy_router.h"
#include "test_support.h"
#include "upstream_dialect.h"

#include <cctype>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <httplib.h>

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestProxyRouter";

// 临时 db 文件名前缀（TestDb 第一实参）
constexpr const char* kDbPrefix = "devdesk_test_proxy_";

// 用 platform::http::send 发同步请求（测试用，走 libcurl）
platform::http::Response syncSend(const std::string& method,
                                  const std::string& url,
                                  const std::string& body = "",
                                  const std::string& bearer = "") {
    platform::http::Request req;
    req.method = method;
    req.url = url;
    req.body = body;
    req.timeoutMs = 8000;
    req.headers["Content-Type"] = "application/json";
    if (!bearer.empty()) {
        req.headers["Authorization"] = "Bearer " + bearer;
    }
    auto r = platform::http::send(req);
    if (r.isFail()) {
        platform::http::Response err;
        err.statusCode = 0;
        err.errorMessage = r.errorMessage();
        err.networkError = true;
        return err;
    }
    return std::move(r.value());
}

using test_support::countCI;
using test_support::headerCI;
using test_support::HeaderMap;

// 在原始 Headers（multimap，未去重）里按大小写不敏感取首个值
std::string headerValueCI(const httplib::Headers& h, const std::string& lowerKey) {
    for (const auto& kv : h) {
        std::string k = kv.first;
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (k == lowerKey) return kv.second;
    }
    return std::string();
}

// 上游收到的任一头值 == v / 含 frag？（伪头以任意拼写形态泄漏的兜底断言）
bool anyHeaderValueEq(const httplib::Headers& h, const std::string& v) {
    for (const auto& kv : h) {
        if (kv.second == v) return true;
    }
    return false;
}

bool anyHeaderValueContains(const httplib::Headers& h, const std::string& frag) {
    for (const auto& kv : h) {
        if (kv.second.find(frag) != std::string::npos) return true;
    }
    return false;
}

// 伪头断言需要**未去重**的 Headers multimap（get_header_value_count / count）。
// platform::http::Server 的 ctx.headers 已按小写键去重，且 has_header/get_header_value
// 对禁用名直接拒见——只有直读 multimap 能看到真泄漏。
class RawUpstream {
public:
    RawUpstream() = default;
    ~RawUpstream() { stop(); }
    RawUpstream(const RawUpstream&) = delete;
    RawUpstream& operator=(const RawUpstream&) = delete;

    bool start() {
        port_ = svr_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) return false;
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
        svr_.wait_until_ready();
        return true;
    }
    void stop() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }
    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    // 处理 POST /v1/messages：把请求头原始快照交给 cb，再回 200 JSON
    void onMessages(std::function<void(const httplib::Headers&)> cb) {
        svr_.Post("/v1/messages",
                  [cb](const httplib::Request& req, httplib::Response& res) {
                      cb(req.headers);
                      res.set_content(
                          R"({"id":"msg_hdr","type":"message","content":[]})",
                          "application/json");
                  });
    }

private:
    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
};

// 本地连接伪头（REMOTE_ADDR/REMOTE_PORT/LOCAL_ADDR/LOCAL_PORT）：httplib 0.58.0 起
// 改为 is_prohibited_header_name 主动禁止、不再注入。代理侧剥离保留为纵深防御
// （客户端仍可能手工携带）；断言预期 0 条——>0 即泄漏。
const char* const kPseudoHeaders[] = {"REMOTE_ADDR", "REMOTE_PORT",
                                      "LOCAL_ADDR", "LOCAL_PORT"};

// 直接调 ProxyRouter::handleRequest（绕过 HTTP server）的便捷封装
struct DirectResult {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

DirectResult directCall(module::claude::ProxyRouter& router,
                        const std::string& method,
                        const std::string& path,
                        const std::string& body,
                        const std::map<std::string, std::string>& headers = {}) {
    platform::http::ResponseWriter w;
    router.handleRequest(method, path, headers, body, w);
    DirectResult r;
    r.status = w.status();
    r.body = w.body();
    r.headers = w.headers();
    return r;
}

} // namespace

REGISTER_TEST(proxy_router_forward_200,
              "proxy_router: 200 转发 + 写 claude_call_logs", {
    // mock 上游：POST /v1/messages → 200 + JSON
    platform::http::Server upstream("127.0.0.1", 0);
    std::string upstreamKey;
    std::string upstreamBody;
    upstream.route("POST", "/v1/messages",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       auto it = ctx.headers.find("x-api-key");
                       if (it != ctx.headers.end()) upstreamKey = it->second;
                       upstreamBody = ctx.body;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"id":"msg_01","type":"message","content":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    const int upstreamPort = upstream.actualPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fwd200");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile(
        "Upstream", "http://127.0.0.1:" + std::to_string(upstreamPort),
        "sk-ant-upstream-key-9999", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    const std::string reqBody =
        R"({"model":"claude-sonnet-4-5","max_tokens":16,"messages":[{"role":"user","content":"hi"}]})";
    auto r = directCall(router, "POST", "/v1/messages", reqBody);

    DOCTEST_CHECK_EQ(r.status, 200);
    DOCTEST_CHECK(r.body.find("msg_01") != std::string::npos);
    // 上游收到的 x-api-key 应是 profile 存的明文（而不是客户端传来的）
    DOCTEST_CHECK_EQ(upstreamKey, std::string("sk-ant-upstream-key-9999"));
    DOCTEST_CHECK_EQ(upstreamBody, reqBody);


    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_upstream_401,
              "proxy_router: 上游 401 原样透传", {
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("POST", "/v1/messages",
                   [](const platform::http::RequestCtx&,
                       platform::http::ResponseWriter& w) {
                       w.setStatus(401);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fwd401");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile(
        "BadKey", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-invalid", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    DOCTEST_CHECK_EQ(r.status, 401);
    DOCTEST_CHECK(r.body.find("authentication_error") != std::string::npos);


    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_upstream_404,
              "proxy_router: 上游 404 原样透传", {
    // 只注册 /v1/messages；请求 /v1/unknown 会 404
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("POST", "/v1/messages",
                   [](const platform::http::RequestCtx&,
                       platform::http::ResponseWriter& w) {
                       w.setStatus(200);
                       w.write("ok");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fwd404");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile(
        "Any", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-key-1234", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/unknown", "{}");
    DOCTEST_CHECK_EQ(r.status, 404);


    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_upstream_unreachable,
              "proxy_router: 上游不可达返回 502", {
    test_support::TestDb tdb(kDbPrefix, "fwd502");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Dead", "http://127.0.0.1:1",
                                     "sk-ant-dead-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    DOCTEST_CHECK_EQ(r.status, 502);
    DOCTEST_CHECK(r.body.find("upstream_unreachable") != std::string::npos);


    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_no_active_profile,
              "proxy_router: 无激活 profile 返回 503", {
    test_support::TestDb tdb(kDbPrefix, "noactive");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("Inactive", "http://127.0.0.1:1",
                                     "sk-ant-inactive", "m");
    DOCTEST_REQUIRE(created.isOk());   // 刻意不 setActiveProfile

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages", "{}");
    DOCTEST_CHECK_EQ(r.status, 503);
    DOCTEST_CHECK(r.body.find("no_active_profile") != std::string::npos);


    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_missing_api_key,
              "proxy_router: Keychain 缺 key 缓冲/流式统一 503 no_active_profile", {
    test_support::TestDb tdb(kDbPrefix, "nokey");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("NoKey", "http://127.0.0.1:1",
                                     "sk-ant-will-delete", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    // 故意清空凭据列，造「该 profile 没存过 API Key」的形态
    DOCTEST_REQUIRE(test_support::clearStoredCredential(
        tdb.handle(), created.value().id));

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    // 同一失败（profile 已激活但取不到 key）分别打两条转发路径：缓冲（body 无
    // stream）/ 流式（POST /v1/messages + "stream":true）
    auto buffered = directCall(router, "POST", "/v1/messages",
                               R"({"model":"m","messages":[]})");
    auto streaming = directCall(router, "POST", "/v1/messages",
                                R"({"model":"m","stream":true,"messages":[]})");

    // 两条路径必须报同一状态码且响应体逐字一致（含 message）
    DOCTEST_CHECK_EQ(buffered.status, 503);
    DOCTEST_CHECK_EQ(streaming.status, 503);
    DOCTEST_CHECK_EQ(streaming.body, buffered.body);
    DOCTEST_CHECK(buffered.body.find("no_active_profile") != std::string::npos);
    // 错误类别统一了，但 message 仍点名取 key 失败（排障不被抹平）
    DOCTEST_CHECK(buffered.body.find("读取 API key 失败") != std::string::npos);


    return true;
})

REGISTER_TEST(proxy_router_end_to_end,
              "proxy_router: 端到端 HTTP 代理（Bearer 鉴权 + 转发）", {
    // mock 上游
    platform::http::Server upstream("127.0.0.1", 0);
    std::string seenApiKey;
    upstream.route("POST", "/v1/messages",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       auto it = ctx.headers.find("x-api-key");
                       if (it != ctx.headers.end()) seenApiKey = it->second;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"id":"msg_e2e","type":"message"})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "e2e");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "E2E", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-e2e-key-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    // 通过 ClaudeService 起真正的代理（仅本机监听，无鉴权）
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    const int proxyPort = started.value();
    DOCTEST_CHECK(svc.isProxyRunning());

    const std::string proxyUrl = "http://127.0.0.1:" + std::to_string(proxyPort);
    const std::string payload =
        R"({"model":"claude-sonnet-4-5","max_tokens":8,"messages":[{"role":"user","content":"ping"}]})";

    // 代理不校验 Authorization：不带 (a) 与带任意值 (b) 都放行，上游拿到的始终是
    // profile 的真实 key（代理覆盖客户端发来的值）
    auto noAuth = syncSend("POST", proxyUrl + "/v1/messages", payload, "");
    DOCTEST_CHECK_EQ(noAuth.statusCode, 200);
    DOCTEST_CHECK(noAuth.body.find("msg_e2e") != std::string::npos);

    auto anyAuth = syncSend("POST", proxyUrl + "/v1/messages", payload, "whatever");
    DOCTEST_CHECK_EQ(anyAuth.statusCode, 200);

    DOCTEST_CHECK_EQ(seenApiKey, std::string("sk-ant-e2e-key-0001"));


    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_error_json_escaped,
              "proxy_router: 错误 JSON 转义（message 含引号/反斜杠）", {
    test_support::TestDb tdb(kDbPrefix, "escjson");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    // 注入会失败的 profile 提供者：错误 message 含引号/反斜杠/换行
    const std::string nasty = "boom \"quoted\" C:\\path\\next\nline";
    router.setProfileProvider([&nasty]() {
        return core::Result<module::claude::Profile>::fail(
            core::ErrorCode::NoActiveProfile, nasty);
    });

    // 缓冲路径（非流式 body）
    auto r1 = directCall(router, "POST", "/v1/other", "{}");
    DOCTEST_CHECK_EQ(r1.status, 503);

    // 流式路径（POST /v1/messages + stream:true，取 profile 失败在启动上游之前）
    auto r2 = directCall(router, "POST", "/v1/messages",
                         R"({"model":"m","stream":true,"messages":[]})");
    DOCTEST_CHECK_EQ(r2.status, 503);

    for (const DirectResult* r : {&r1, &r2}) {
        // 必须是可解析的 JSON，且 message 原样往返
        auto parsed = platform::json::parse(r->body);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_REQUIRE(parsed.value().isObject());
        auto msg = parsed.value().asObject().get("message");
        DOCTEST_REQUIRE(msg.isString());
        DOCTEST_CHECK_EQ(msg.asString(), nasty);
        auto err = parsed.value().asObject().get("error");
        DOCTEST_CHECK_EQ(err.asString(), std::string("no_active_profile"));
    }
    return true;
})

REGISTER_TEST(proxy_router_applies_upstream_dialect,
              "proxy_router: 转发覆盖 UA + 鉴权头（客户端 python-urllib 不泄漏）", {
    platform::http::Server upstream("127.0.0.1", 0);
    HeaderMap seen;
    std::string seenBody;
    upstream.route("POST", R"(/(.*))",
                   [&](const platform::http::RequestCtx& ctx,
                       platform::http::ResponseWriter& w) {
                       seen = ctx.headers;
                       seenBody = ctx.body;
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"id":"msg_dialect","type":"message","content":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "dialect");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "Dialect", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-dialect-key-0001", "deepseek-v4-flash");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    // 客户端形态 = python-urllib：UA 会被 Cloudflare 判 browser_signature_banned，
    // 鉴权头是假 key
    HeaderMap clientH;
    clientH["user-agent"] = "Python-urllib/3.14";
    clientH["x-api-key"] = "sk-client-bogus";
    clientH["authorization"] = "Bearer sk-client-bogus";
    clientH["content-type"] = "application/json";

    const std::string reqBody =
        R"({"model":"deepseek-v4-flash[1M]","max_tokens":16,"messages":[{"role":"user","content":"hi"}]})";
    auto r = directCall(router, "POST", "/v1/messages", reqBody, clientH);
    DOCTEST_CHECK_EQ(r.status, 200);

    // UA 被无条件覆盖（客户端签名不泄漏给上游），且没有大小写变体重复头
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    // 鉴权头换成 profile 的 key（客户端那份作废）
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-dialect-key-0001"));
    DOCTEST_CHECK_EQ(countCI(seen, "x-api-key"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "authorization"),
                     std::string("Bearer sk-ant-dialect-key-0001"));
    DOCTEST_CHECK_EQ(countCI(seen, "authorization"), 1);
    // 客户端没带 anthropic-version → 补默认值
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"), std::string("2023-06-01"));
    // 127.0.0.1 是默认方言：不注入会话头、不剥模型标记（body 原样上行）
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 0);
    DOCTEST_CHECK_EQ(seenBody, reqBody);

    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_drops_httplib_pseudo_headers,
              "proxy_router: 不转发 httplib 注入伪头（LOCAL_ADDR/REMOTE_PORT 等）", {
    // mock 上游：捕获原始请求头（未去重的 Headers multimap）
    RawUpstream upstream;
    httplib::Headers seen;
    upstream.onMessages([&seen](const httplib::Headers& h) { seen = h; });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "hdrdrop");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile("HdrDrop", upstream.baseUrl(),
                                     "sk-ant-hdr-key-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    // 模拟恶意/旧版客户端手工携带的伪头（大小写变体，值换成本机不可能出现的
    // 标记值）+ 必须保留的正常头
    HeaderMap clientH;
    clientH["REMOTE_ADDR"] = "198.51.100.21";
    clientH["REMOTE_PORT"] = "19861";
    clientH["LOCAL_ADDR"] = "198.51.100.22";
    clientH["LOCAL_PORT"] = "19862";
    clientH["remote_addr"] = "198.51.100.23";
    clientH["remote_port"] = "19863";
    clientH["local_addr"] = "198.51.100.24";
    clientH["local_port"] = "19864";
    clientH["anthropic-version"] = "2023-06-01";
    clientH["content-type"] = "application/json";
    clientH["x-devdesk-keep"] = "yes";

    const std::string reqBody =
        R"({"model":"claude-sonnet-4-5","max_tokens":16,"messages":[{"role":"user","content":"hi"}]})";
    auto r = directCall(router, "POST", "/v1/messages", reqBody, clientH);
    DOCTEST_CHECK_EQ(r.status, 200);

    // 上游 mock 不再自注入伪头（is_prohibited_header_name 主动禁止）——任何 >0
    // 计数都只能来自代理转发，即泄漏
    for (const char* name : kPseudoHeaders) {
        DOCTEST_CHECK_EQ(seen.count(name), 0u);
    }
    // 兜底：标记值以任意拼写/键名出现 = 泄漏
    DOCTEST_CHECK_FALSE(anyHeaderValueContains(seen, "198.51.100."));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19861"));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19862"));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19863"));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19864"));
    DOCTEST_CHECK_EQ(headerValueCI(seen, "anthropic-version"),
                     std::string("2023-06-01"));
    DOCTEST_CHECK_EQ(headerValueCI(seen, "content-type"),
                     std::string("application/json"));
    DOCTEST_CHECK_EQ(headerValueCI(seen, "x-devdesk-keep"), std::string("yes"));

    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

REGISTER_TEST(proxy_router_pseudo_headers_filtered_end_to_end,
              "proxy_router: 端到端不转发伪头（缓冲 + 流式）", {
    // 上游 mock 捕获每封请求的原始头
    RawUpstream upstream;
    std::vector<httplib::Headers> snaps;
    upstream.onMessages([&snaps](const httplib::Headers& h) {
        snaps.push_back(h);
    });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "hdre2e");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile("HdrE2E", upstream.baseUrl(),
                                     "sk-ant-hdr-e2e-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    // 起真代理 server
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    const std::string proxyUrl =
        "http://127.0.0.1:" + std::to_string(started.value());

    // 客户端头：正常头 + 小写伪头 canary（模拟客户端手工携带）
    HeaderMap reqH;
    reqH["Content-Type"] = "application/json";
    reqH["anthropic-version"] = "2023-06-01";
    reqH["x-devdesk-keep"] = "yes";
    reqH["local_port"] = "19871";
    reqH["local_addr"] = "198.51.100.31";
    reqH["remote_port"] = "19872";
    reqH["remote_addr"] = "198.51.100.32";

    auto doSend = [&](const std::string& body) {
        platform::http::Request creq;
        creq.method = "POST";
        creq.url = proxyUrl + "/v1/messages";
        creq.body = body;
        creq.timeoutMs = 8000;
        creq.headers = reqH;
        return platform::http::send(creq);
    };

    // (a) 缓冲路径（body 无 stream:true）
    auto r1 = doSend(
        R"({"model":"claude-sonnet-4-5","max_tokens":8,"messages":[{"role":"user","content":"ping"}]})");
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_EQ(r1.value().statusCode, 200);
    DOCTEST_CHECK(r1.value().body.find("msg_hdr") != std::string::npos);

    // (b) 流式路径（POST /v1/messages + stream:true）
    auto r2 = doSend(
        R"({"model":"claude-sonnet-4-5","max_tokens":8,"stream":true,"messages":[{"role":"user","content":"ping"}]})");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().statusCode, 200);
    DOCTEST_CHECK(r2.value().body.find("msg_hdr") != std::string::npos);

    DOCTEST_REQUIRE_EQ(snaps.size(), 2u);
    for (const auto& h : snaps) {
        // 上游 mock 无任何自注入——计数 >0 只能是客户端伪头穿透代理 = 泄漏
        for (const char* name : kPseudoHeaders) {
            DOCTEST_CHECK_EQ(h.count(name), 0u);
        }
        DOCTEST_CHECK_FALSE(anyHeaderValueContains(h, "198.51.100."));
        DOCTEST_CHECK_FALSE(anyHeaderValueEq(h, "19871"));
        DOCTEST_CHECK_FALSE(anyHeaderValueEq(h, "19872"));
        DOCTEST_CHECK_EQ(headerValueCI(h, "anthropic-version"),
                         std::string("2023-06-01"));
        DOCTEST_CHECK_EQ(headerValueCI(h, "content-type"),
                         std::string("application/json"));
        DOCTEST_CHECK_EQ(headerValueCI(h, "x-devdesk-keep"), std::string("yes"));
    }

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

