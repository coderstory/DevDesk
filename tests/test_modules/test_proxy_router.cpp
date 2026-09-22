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

#include <chrono>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#include <curl/curl.h>

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestProxyRouter";

std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_proxy_" + tag + "_" +
                std::to_string(counter) + ".db";
    return std::filesystem::temp_directory_path() / name;
}

struct TestDb {
    std::filesystem::path path;
    core::Database db;

    explicit TestDb(const std::string& tag) {
        path = tempDbPath(tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " + r.errorMessage());
        }
        db = std::move(r.unsafeValue());
        auto m = db.migrate();
        if (m.isFail()) {
            throw std::runtime_error("migrate failed: " + m.errorMessage());
        }
    }

    ~TestDb() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }

    core::data::SqliteDb& handle() { return db.db(); }
};

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
    return std::move(r.unsafeValue());
}

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

    TestDb tdb("fwd200");
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

    TestDb tdb("fwd401");
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

    TestDb tdb("fwd404");
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
    TestDb tdb("fwd502");
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
    TestDb tdb("noactive");
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

// ---- 6. Keychain 缺 key → 500 ----

REGISTER_TEST(proxy_router_missing_api_key,
              "proxy_router: Keychain 缺 key 返回 500", {
    TestDb tdb("nokey");
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

    auto r = directCall(router, "POST", "/v1/messages", "{}");
    DOCTEST_CHECK_EQ(r.status, 500);
    DOCTEST_CHECK(r.body.find("keychain_read_failed") != std::string::npos);

    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 1u);
    DOCTEST_CHECK_EQ(logs.value()[0].statusCode, 500);
    DOCTEST_CHECK_EQ(logs.value()[0].error,
                     std::string("keychain_read_failed"));

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

    TestDb tdb("e2e");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto created = svc.createProfile(
        "E2E", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-e2e-key-0001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(created.value().id).isOk());

    // 通过 ClaudeService 起真正的代理（含 auth token）
    const std::string token = "e2e-test-token-xyz";
    auto started = svc.startProxy("127.0.0.1", 0, token);
    DOCTEST_REQUIRE(started.isOk());
    const int proxyPort = started.value();
    DOCTEST_CHECK(svc.isProxyRunning());

    const std::string proxyUrl = "http://127.0.0.1:" + std::to_string(proxyPort);
    const std::string payload =
        R"({"model":"claude-sonnet-4-5","max_tokens":8,"messages":[{"role":"user","content":"ping"}]})";

    // (a) 不带 token → 401（pre_routing_handler 拦截）
    auto noAuth = syncSend("POST", proxyUrl + "/v1/messages", payload, "");
    DOCTEST_CHECK_EQ(noAuth.statusCode, 401);

    // (b) 错误 token → 401
    auto badAuth = syncSend("POST", proxyUrl + "/v1/messages", payload, "wrong");
    DOCTEST_CHECK_EQ(badAuth.statusCode, 401);

    // (c) 正确 token → 200 + 转发
    auto ok = syncSend("POST", proxyUrl + "/v1/messages", payload, token);
    DOCTEST_CHECK_EQ(ok.statusCode, 200);
    DOCTEST_CHECK(ok.body.find("msg_e2e") != std::string::npos);
    DOCTEST_CHECK_EQ(seenApiKey, std::string("sk-ant-e2e-key-0001"));

    // (d) 只有 (c) 写进了日志
    auto logs = svc.recentCalls(10);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_CHECK_EQ(logs.value().size(), 1u);
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
    TestDb tdb("escjson");
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
