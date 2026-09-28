// tests/test_modules/test_proxy_router.cpp
// ProxyRouter 单元测试：转发 / 鉴权 / 上游失败 / 日志
//
// 测试环境隔离：
//   - 上游用 platform::http::Server 起一个本地 mock
//   - 数据库用临时文件
//   - Keychain 用 "DevDeskTestProxyRouter" service
//
// 覆盖：
//   1. 200 转发（body / status / x-api-key 注入 / 日志落库）
//   2. 401 上游透传（不吞错）
//   3. 404 上游透传
//   4. 上游不可达 → 502
//   5. 无激活 profile → 503
//   6. 代理 server 端到端（Bearer 鉴权 + 转发）
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

// 临时 db 文件名前缀（共享工厂 test_support::tempDbPath / TestDb 的第一实参）
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

// 头名大小写不敏感取头 / 计数：共享实现（原 test_claude_service /
// test_upstream_dialect 三份同款合流，TIN-06b）
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

// 原始 httplib mock 上游：伪头断言需要**未去重**的 Headers multimap
// （get_header_value_count / count）。platform::http::Server 的 ctx.headers
// 已按小写键去重（CP-06：httplib 的大小写不敏感 multimap 降级时键统一小写），
// 且它自己对每封收到的请求也注入同名伪头（httplib.h:7186-7192，键同样被降为
// 小写），无法区分「代理转发来的」与「mock 自己注入的」。
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

// cpp-httplib 服务端对**每封收到的请求**注入的本地连接伪头（httplib.h:7186-7192），
// 就是代理必须从客户端头里过滤掉的那 4 个名字
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

// ---- 1. 200 转发：status / body / 日志 ----

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
    // 上游收到的 x-api-key 应是 Keychain 里的明文（而不是客户端传来的）
    DOCTEST_CHECK_EQ(upstreamKey, std::string("sk-ant-upstream-key-9999"));
    DOCTEST_CHECK_EQ(upstreamBody, reqBody);

    // 应写入一条 claude_call_logs
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].method, std::string("POST"));
    DOCTEST_CHECK_EQ(logs.value()[0].path, std::string("/v1/messages"));
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 200);
    DOCTEST_CHECK_EQ(logs.value()[0].requestBytes,
                     static_cast<int64_t>(reqBody.size()));
    DOCTEST_CHECK(logs.value()[0].responseBytes > 0);

    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 2. 上游 401 透传（代理不吞错） ----

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

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 401);

    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 3. 上游 404 透传 ----

REGISTER_TEST(proxy_router_upstream_404,
              "proxy_router: 上游 404 原样透传", {
    platform::http::Server upstream("127.0.0.1", 0);
    // 只注册 /v1/messages；请求 /v1/unknown 会 404
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

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 404);
    DOCTEST_CHECK_EQ(logs.value()[0].path, std::string("/v1/unknown"));

    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 4. 上游不可达 → 502 ----

REGISTER_TEST(proxy_router_upstream_unreachable,
              "proxy_router: 上游不可达返回 502", {
    test_support::TestDb tdb(kDbPrefix, "fwd502");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 指向一个没有监听的端口
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

    // 日志应记录失败（status_code = 0 + error 非空）
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 0);
    DOCTEST_CHECK(!logs.value()[0].error.empty());

    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 5. 无激活 profile → 503 ----

REGISTER_TEST(proxy_router_no_active_profile,
              "proxy_router: 无激活 profile 返回 503", {
    test_support::TestDb tdb(kDbPrefix, "noactive");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    // 建一个 profile 但不激活
    auto created = svc.createProfile("Inactive", "http://127.0.0.1:1",
                                     "sk-ant-inactive", "m");
    DOCTEST_REQUIRE(created.isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages", "{}");
    DOCTEST_CHECK_EQ(r.status, 503);
    DOCTEST_CHECK(r.body.find("no_active_profile") != std::string::npos);

    // 无激活 profile 时不应写日志
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 0u);

    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 6. Keychain 缺 key → 两条路径统一 503 no_active_profile ----

REGISTER_TEST(proxy_router_missing_api_key,
              "proxy_router: Keychain 缺 key 缓冲/流式统一 503 no_active_profile", {
    test_support::TestDb tdb(kDbPrefix, "nokey");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);

    auto created = svc.createProfile("NoKey", "http://127.0.0.1:1",
                                     "sk-ant-will-delete", "m");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    // 故意删掉 Keychain 里的 key
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    // 同一失败（profile 已激活、Keychain 无 key）分别打两条转发路径：
    // 缓冲（body 无 stream）/ 流式（POST /v1/messages + "stream":true）
    auto buffered = directCall(router, "POST", "/v1/messages",
                               R"({"model":"m","messages":[]})");
    auto streaming = directCall(router, "POST", "/v1/messages",
                                R"({"model":"m","stream":true,"messages":[]})");

    // CPX-10：两条路径必须报同一状态码且响应体逐字一致（含 message）
    DOCTEST_CHECK_EQ(buffered.status, 503);
    DOCTEST_CHECK_EQ(streaming.status, 503);
    DOCTEST_CHECK_EQ(streaming.body, buffered.body);
    DOCTEST_CHECK(buffered.body.find("no_active_profile") != std::string::npos);
    // 错误类别统一了，但 message 仍点名 Keychain（排障不被抹平）
    DOCTEST_CHECK(buffered.body.find("读取 API key 失败") != std::string::npos);

    // 取 profile 失败不写调用日志（与「无激活 profile」同口径，两条路径一致）
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 0u);

    return true;
})

// ---- 7. 端到端：代理 server + Bearer 鉴权 + 转发 ----

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

    // (a) 不带 Authorization → 直接放行（代理不看该头）
    auto noAuth = syncSend("POST", proxyUrl + "/v1/messages", payload, "");
    DOCTEST_CHECK_EQ(noAuth.statusCode, 200);
    DOCTEST_CHECK(noAuth.body.find("msg_e2e") != std::string::npos);

    // (b) 带任意 Authorization → 同样放行
    auto anyAuth = syncSend("POST", proxyUrl + "/v1/messages", payload, "whatever");
    DOCTEST_CHECK_EQ(anyAuth.statusCode, 200);

    // 上游拿到的始终是 profile 的真实 key（代理覆盖客户端发来的值）
    DOCTEST_CHECK_EQ(seenApiKey, std::string("sk-ant-e2e-key-0001"));

    // (c) 两次请求都写进了日志
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 2u);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 200);

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 8. 错误响应是合法 JSON（message 含引号/反斜杠/换行时转义） ----

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

// ---- 9. 转发经上游方言层：客户端 UA 不泄漏、鉴权头被覆盖 ----

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

    // 客户端形态 = doc/scripts/proxy_smoke_test.py（python-urllib：
    // UA 会被 Cloudflare 判 browser_signature_banned，鉴权头是假 key）
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

// ---- 9b. profile.dialectOverride 接通转发路径（CPX-13 接线） ----
// 承载断言的必须是**转发路径的实际效果**（上游收到的头 / body），不是
// forBaseUrl 的返回值：host 是 loopback（host 推断 = 默认方言），profile 钉住
// opencode_zen → 上游必须收到会话头、模型名 [1M] 必须被剥。覆盖值经生产路径
//（createProfile 落库 → 激活 → resolveProfile 读回）注入，不绕过 DB。
// 后半段把覆盖清空（空串 = 未覆盖）→ 与改动前逐项一致：无会话头 + body 逐字节不变。

REGISTER_TEST(proxy_router_dialect_override_wired,
              "proxy_router: profile.dialectOverride 接通转发（空串等价未覆盖）", {
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
                       w.write(R"({"id":"msg_ovr","type":"message","content":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "dialect_ovr");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    // 第 8 个参数 = dialectOverride（值域见 upstream_dialect.h）
    auto created = svc.createProfile(
        "Ovr", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-ovr-key-0001", "deepseek-v4-flash",
        /*activateIfNoneActive=*/false, /*sharedConfigId=*/0, "both",
        "opencode_zen");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());
    auto active = svc.getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().dialectOverride, std::string("opencode_zen"));

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    const std::string reqBody =
        R"({"model":"deepseek-v4-flash[1M]","max_tokens":16,"messages":[{"role":"user","content":"hi"}]})";
    auto r = directCall(router, "POST", "/v1/messages", reqBody);
    DOCTEST_CHECK_EQ(r.status, 200);
    // loopback host 推断不出 zen → 会话头出现即证明覆盖值真的走进了转发路径
    DOCTEST_CHECK_EQ(headerCI(seen, "x-opencode-session"),
                     module::claude::claudeUpstreamSessionId());
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 1);
    // 方言全量生效（不只头）：模型名 [1M] 被剥掉
    DOCTEST_CHECK(seenBody.find("[1M]") == std::string::npos);
    DOCTEST_CHECK(seenBody.find("deepseek-v4-flash") != std::string::npos);
    DOCTEST_CHECK(seenBody.find("\"model\":\"deepseek-v4-flash\"") !=
                  std::string::npos);

    // ---- 空串 = 未覆盖：与改动前逐项一致（无会话头 + body 逐字节不变）----
    module::claude::Profile p = active.value();
    p.dialectOverride.clear();
    DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    auto cleared = svc.getProfile(created.value().id);
    DOCTEST_REQUIRE(cleared.isOk());
    DOCTEST_CHECK(cleared.value().dialectOverride.empty());

    seen.clear();
    seenBody.clear();
    auto r2 = directCall(router, "POST", "/v1/messages", reqBody);
    DOCTEST_CHECK_EQ(r2.status, 200);
    DOCTEST_CHECK_EQ(countCI(seen, "x-opencode-session"), 0);
    DOCTEST_CHECK_EQ(seenBody, reqBody);
    // 方言层其余行为照旧（鉴权 / UA / 版本头）
    DOCTEST_CHECK_EQ(headerCI(seen, "user-agent"),
                     std::string(module::claude::kProxyUserAgent));
    DOCTEST_CHECK_EQ(countCI(seen, "user-agent"), 1);
    DOCTEST_CHECK_EQ(headerCI(seen, "x-api-key"),
                     std::string("sk-ant-ovr-key-0001"));
    DOCTEST_CHECK_EQ(headerCI(seen, "authorization"),
                     std::string("Bearer sk-ant-ovr-key-0001"));
    DOCTEST_CHECK_EQ(headerCI(seen, "anthropic-version"), std::string("2023-06-01"));

    upstream.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(created.value().id));
    return true;
})

// ---- 10. httplib 注入的本地连接伪头不转发（缓冲路径：直接调 handleRequest） ----

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

    // 模拟路由收到的头：httplib 注入的大写伪头（值换成本机不可能出现的标记值）
    // + 客户端大小写变体 + 必须保留的正常头
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

    // 上游 mock 自己也对每封收到的请求注入一份伪头（httplib.h:7186-7192 服务端
    // 行为）；Headers 键大小写不敏感（httplib.h:506-508），所以每个伪头名在
    // 上游应**恰好 1 条**——就是 mock 自注入那条；≥2 即代理转发了伪头（任何拼写）
    for (const char* name : kPseudoHeaders) {
        DOCTEST_CHECK_EQ(seen.count(name), 1u);
    }
    // 兜底：标记值以任意拼写/键名出现 = 泄漏
    DOCTEST_CHECK_FALSE(anyHeaderValueContains(seen, "198.51.100."));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19861"));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19862"));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19863"));
    DOCTEST_CHECK_FALSE(anyHeaderValueEq(seen, "19864"));
    // 正常头必须继续透传
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

// ---- 11. 端到端（真 httplib 注入）：缓冲 + 流式两条路径都不转发伪头 ----

REGISTER_TEST(proxy_router_pseudo_headers_filtered_end_to_end,
              "proxy_router: 端到端不转发伪头（缓冲 + 流式，真 httplib 注入）", {
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

    // 起真代理 server（其 httplib 会对每封收到的请求注入 LOCAL_ADDR 等伪头）
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_CHECK(svc.isProxyRunning());
    const std::string proxyUrl =
        "http://127.0.0.1:" + std::to_string(started.value());

    // 客户端头：正常头 + 小写伪头 canary（与代理端注入的大写伪头同属 CI 同名）
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
        // 每个伪头名恰好 1 条（上游自注入）——代理注入的（大写）与客户端
        // canary（小写）都被过滤；≥2 即泄漏
        for (const char* name : kPseudoHeaders) {
            DOCTEST_CHECK_EQ(h.count(name), 1u);
        }
        DOCTEST_CHECK_FALSE(anyHeaderValueContains(h, "198.51.100."));
        DOCTEST_CHECK_FALSE(anyHeaderValueEq(h, "19871"));
        DOCTEST_CHECK_FALSE(anyHeaderValueEq(h, "19872"));
        // 正常头仍在
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

// ---- 12. failover：上游 5xx → 换备用重试一次，成功回备用的结果（设计 §3.8）----
// 取数 = seed profile 行（决议：既有注入用例断言不换家；「注入 + 换家」场景
// seed 行——不走 profileProvider_ 注入，测试走真实 DB 路径，双 profile 落库：
// 激活 A + A.fallbackProfileId = B）

REGISTER_TEST(proxy_router_failover_switch_on_5xx,
              "proxy_router: 上游 500 → 换备用重试成功（fallback_profile_id）", {
    platform::http::Server upA("127.0.0.1", 0);
    platform::http::Server upB("127.0.0.1", 0);
    upA.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(500);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"type":"error","error":{"type":"overloaded_error","message":"A down"}})");
                  w.end();
              });
    upB.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(200);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"id":"msg_fb_b","type":"message","content":[]})");
                  w.end();
              });
    DOCTEST_REQUIRE(upA.start());
    DOCTEST_REQUIRE(upB.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fb_5xx");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto a = svc.createProfile("主家", "http://127.0.0.1:" +
                               std::to_string(upA.actualPort()),
                               "sk-ant-fb-a-key-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "http://127.0.0.1:" +
                               std::to_string(upB.actualPort()),
                               "sk-ant-fb-b-key-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    {
        module::claude::Profile p = a.value();
        p.fallbackProfileId = b.value().id;
        DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    }

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    // 主家 500 → 换备家 → 200 + 备家 body
    DOCTEST_CHECK_EQ(r.status, 200);
    DOCTEST_CHECK(r.body.find("msg_fb_b") != std::string::npos);

    // 日志只有 1 条，profile = 实际服务的那家（备家）
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_REQUIRE_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].profileId, b.value().id);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 200);

    upA.stop();
    upB.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})

// ---- 13. failover 负列表：400 不换家（即使配了可用备用） ----

REGISTER_TEST(proxy_router_failover_negative_list_no_switch,
              "proxy_router: failover 上游 400（负列表）不换家，原样透传", {
    platform::http::Server upA("127.0.0.1", 0);
    platform::http::Server upB("127.0.0.1", 0);
    std::atomic<int> hitsB{0};
    upA.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(400);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"type":"error","error":{"type":"invalid_request_error","message":"bad model"}})");
                  w.end();
              });
    upB.route("POST", "/v1/messages",
              [&hitsB](const platform::http::RequestCtx&,
                       platform::http::ResponseWriter& w) {
                  hitsB.fetch_add(1);
                  w.setStatus(200);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"id":"msg_fb_never","type":"message"})");
                  w.end();
              });
    DOCTEST_REQUIRE(upA.start());
    DOCTEST_REQUIRE(upB.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fb_neg");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto a = svc.createProfile("主家", "http://127.0.0.1:" +
                               std::to_string(upA.actualPort()),
                               "sk-ant-fb-neg-a-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "http://127.0.0.1:" +
                               std::to_string(upB.actualPort()),
                               "sk-ant-fb-neg-b-0001", "m");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    {
        module::claude::Profile p = a.value();
        p.fallbackProfileId = b.value().id;   // 备用可用，但负列表不该用
        DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    }

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    DOCTEST_CHECK_EQ(r.status, 400);
    DOCTEST_CHECK(r.body.find("invalid_request_error") != std::string::npos);
    DOCTEST_CHECK_EQ(hitsB.load(), 0);   // 备家一次都没被叫

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_REQUIRE_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].profileId, a.value().id);

    upA.stop();
    upB.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})

// ---- 14. failover 传输失败（statusCode==0）：主家不可达 → 换备家 ----

REGISTER_TEST(proxy_router_failover_transport_failure_switch,
              "proxy_router: failover 主家不可达（传输失败）→ 换备用成功", {
    platform::http::Server upB("127.0.0.1", 0);
    upB.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(200);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"id":"msg_fb_trans","type":"message"})");
                  w.end();
              });
    DOCTEST_REQUIRE(upB.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fb_trans");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto a = svc.createProfile("主家", "http://127.0.0.1:1",   // 无监听的端口
                               "sk-ant-fb-trans-a-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "http://127.0.0.1:" +
                               std::to_string(upB.actualPort()),
                               "sk-ant-fb-trans-b-0001", "m");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    {
        module::claude::Profile p = a.value();
        p.fallbackProfileId = b.value().id;
        DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    }

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    DOCTEST_CHECK_EQ(r.status, 200);
    DOCTEST_CHECK(r.body.find("msg_fb_trans") != std::string::npos);

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_REQUIRE_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].profileId, b.value().id);

    upB.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})

// ---- 15. 两次都失败 → 回第二次的结果（错误更相关），日志 = 备家 ----

REGISTER_TEST(proxy_router_failover_both_fail_second_result,
              "proxy_router: failover 主备都失败回第二次结果（错误更相关）", {
    platform::http::Server upA("127.0.0.1", 0);
    platform::http::Server upB("127.0.0.1", 0);
    upA.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(500);
                  w.write("A-overloaded");
                  w.end();
              });
    upB.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(529);   // 上游过载档，同样可换但这是第二次
                  w.write("B-overloaded");
                  w.end();
              });
    DOCTEST_REQUIRE(upA.start());
    DOCTEST_REQUIRE(upB.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fb_both");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto a = svc.createProfile("主家", "http://127.0.0.1:" +
                               std::to_string(upA.actualPort()),
                               "sk-ant-fb-both-a-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "http://127.0.0.1:" +
                               std::to_string(upB.actualPort()),
                               "sk-ant-fb-both-b-0001", "m");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    {
        module::claude::Profile p = a.value();
        p.fallbackProfileId = b.value().id;
        DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    }

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    // 第二次的结果（备家 529），不换第三家
    DOCTEST_CHECK_EQ(r.status, 529);
    DOCTEST_CHECK(r.body.find("B-overloaded") != std::string::npos);

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_REQUIRE_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].profileId, b.value().id);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 529);

    upA.stop();
    upB.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})

// ---- 16. 注入模式守卫：profileProvider_ 已设 → 不换家（设计 §3.8 显式
//      声明，不依赖「内存库无 profile → 恒 nullopt」的巧合）----
// 取数：seed 一条**可用**的备家行（若守卫缺失，resolveFallbackProfile 能读到
// 它并换家成功——用例断言「没换」才是守卫在起作用）

REGISTER_TEST(proxy_router_failover_injected_mode_no_switch,
              "proxy_router: failover 注入模式（profileProvider_）下不换家（守卫）", {
    platform::http::Server upB("127.0.0.1", 0);
    std::atomic<int> hitsB{0};
    upB.route("POST", "/v1/messages",
              [&hitsB](const platform::http::RequestCtx&,
                       platform::http::ResponseWriter& w) {
                  hitsB.fetch_add(1);
                  w.setStatus(200);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"id":"msg_inj_b","type":"message"})");
                  w.end();
              });
    DOCTEST_REQUIRE(upB.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fb_inj");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    // seed 可用备家行（id 供注入 profile 的 fallbackProfileId 引用）
    auto b = svc.createProfile("备家", "http://127.0.0.1:" +
                               std::to_string(upB.actualPort()),
                               "sk-ant-fb-inj-b-0001", "m");
    DOCTEST_REQUIRE(b.isOk());

    platform::http::Server proxyServer("127.0.0.1", 0);
    module::claude::ProxyRouter router(svc, proxyServer);
    router.registerRoutes();

    // 注入完整 profile（绕过 DB/Keychain）：主家 = 死端口；fallbackProfileId
    // 指向 seed 的可用备家（守卫缺失时会换过去 → 200）
    router.setProfileProvider([fbId = b.value().id]() {
        module::claude::Profile p;
        p.id = 999;
        p.name = "注入主家";
        p.baseUrl = "http://127.0.0.1:1";   // 传输失败（可换档）
        p.apiKeyPlain = "sk-injected";
        p.fallbackProfileId = fbId;
        return core::Result<module::claude::Profile>::ok(std::move(p));
    });

    auto r = directCall(router, "POST", "/v1/messages",
                        R"({"model":"m","messages":[]})");
    // 注入模式：不 resolveFallbackProfile、不换家 → 主家的 502 原样回
    DOCTEST_CHECK_EQ(r.status, 502);
    DOCTEST_CHECK(r.body.find("upstream_unreachable") != std::string::npos);
    DOCTEST_CHECK_EQ(hitsB.load(), 0);   // 备家一次都没被叫 = 守卫生效

    upB.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})

// ---- 17. 流式路径 failover：waitForStatus 后写响应头前换家（设计 §3.8）----
// 走真代理 server（cpp-httplib 驱动 chunked provider），主家 500 → 废弃本次
// 流 → 备用起新流 → 客户端拿到备家的 200 与 body

REGISTER_TEST(proxy_router_failover_streaming_switch,
              "proxy_router: 流式主家 500 → cancelWithReason(failover) 换备用", {
    platform::http::Server upA("127.0.0.1", 0);
    platform::http::Server upB("127.0.0.1", 0);
    upA.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(500);
                  w.setHeader("Content-Type", "application/json");
                  w.write(R"({"type":"error","error":{"type":"overloaded_error","message":"A down"}})");
                  w.end();
              });
    upB.route("POST", "/v1/messages",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(200);
                  w.setHeader("Content-Type", "text/event-stream");
                  w.write("data: {\"type\":\"message_start\"}\n\ndata: [DONE]\n\n");
                  w.end();
              });
    DOCTEST_REQUIRE(upA.start());
    DOCTEST_REQUIRE(upB.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    test_support::TestDb tdb(kDbPrefix, "fb_stream");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto a = svc.createProfile("主家", "http://127.0.0.1:" +
                               std::to_string(upA.actualPort()),
                               "sk-ant-fb-s-a-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    auto b = svc.createProfile("备家", "http://127.0.0.1:" +
                               std::to_string(upB.actualPort()),
                               "sk-ant-fb-s-b-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(a.value().id).isOk());
    {
        module::claude::Profile p = a.value();
        p.fallbackProfileId = b.value().id;
        DOCTEST_REQUIRE(svc.updateProfile(p).isOk());
    }

    // 起真代理（驱动 chunked provider）
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    const std::string proxyUrl = "http://127.0.0.1:" +
                                 std::to_string(started.value());

    auto resp = syncSend("POST", proxyUrl + "/v1/messages",
                         R"({"model":"m","stream":true,"messages":[]})");
    DOCTEST_CHECK_EQ(resp.statusCode, 200);
    DOCTEST_CHECK(resp.body.find("message_start") != std::string::npos);
    DOCTEST_CHECK(resp.body.find("[DONE]") != std::string::npos);
    // 备家的 500 body（A down）不该出现在客户端（已随废弃流销毁）
    DOCTEST_CHECK(resp.body.find("A down") == std::string::npos);

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_REQUIRE_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].profileId, b.value().id);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 200);

    svc.stopProxy();
    DOCTEST_CHECK_FALSE(svc.isProxyRunning());
    upA.stop();
    upB.stop();
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(a.value().id));
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(b.value().id));
    return true;
})
