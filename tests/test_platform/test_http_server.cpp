#include "doctest.h"
#include "platform/http_server.h"

#include <chrono>
#include <thread>
#include <utility>

#include <curl/curl.h>

namespace {

// libcurl write callback：把所有响应字节追加到 out
size_t curlWriteCb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    out->append(static_cast<const char*>(contents), total);
    return total;
}

struct CurlGlobalInit {
    CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};

// 在测试进程内做一次全局 init（每个测试共享）
const CurlGlobalInit& curlInit() {
    static CurlGlobalInit g;
    return g;
}

// 同步 GET，返回 statusCode（输出参数：body）
long httpGet(const std::string& url, std::string& body,
             const std::string& bearerToken = "") {
    curlInit();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    struct curl_slist* headers = nullptr;
    if (!bearerToken.empty()) {
        std::string h = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, h.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

// 同步 POST，返回 statusCode（输出参数：body）
long httpPost(const std::string& url, const std::string& payload,
              std::string& body, const std::string& bearerToken = "") {
    curlInit();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    struct curl_slist* headers = nullptr;
    std::string ctype = "Content-Type: application/json";
    headers = curl_slist_append(headers, ctype.c_str());
    if (!bearerToken.empty()) {
        std::string h = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

// 等待服务最多 maxMs 启动可连接
bool waitForReady(int port, int maxMs = 2000) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(maxMs)) {
        std::string body;
        long status = httpGet("http://127.0.0.1:" + std::to_string(port) +
                              "/__ping__", body);
        if (status != 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

// ---- Server on random port responds with handler output ----

REGISTER_TEST(http_server_basic_get, "http_server: GET handler responds", {
    platform::http::Server srv("127.0.0.1", 0);
    int capturedStatus = 0;
    std::string capturedPath;
    srv.route("GET", "/hello", [&](const platform::http::RequestCtx& ctx,
                                  platform::http::ResponseWriter& w) {
        capturedStatus = (ctx.method == "GET") ? 1 : 0;
        capturedPath = ctx.path;
        w.setStatus(200);
        w.setHeader("Content-Type", "text/plain");
        w.write("OK");
        w.end();
    });
    DOCTEST_CHECK(srv.start());
    DOCTEST_CHECK(srv.isRunning());
    int port = srv.actualPort();
    DOCTEST_CHECK(port > 0);
    DOCTEST_CHECK(waitForReady(port));

    std::string body;
    long status = httpGet("http://127.0.0.1:" + std::to_string(port) +
                          "/hello", body);
    DOCTEST_CHECK_EQ(status, 200L);
    DOCTEST_CHECK_EQ(body, std::string("OK"));
    DOCTEST_CHECK_EQ(capturedStatus, 1);
    DOCTEST_CHECK_EQ(capturedPath, std::string("/hello"));

    srv.stop();
    DOCTEST_CHECK_FALSE(srv.isRunning());
    return true;
})

// ---- POST handler reads body ----

REGISTER_TEST(http_server_post_body, "http_server: POST handler reads body", {
    platform::http::Server srv("127.0.0.1", 0);
    std::string receivedBody;
    srv.route("POST", "/echo", [&](const platform::http::RequestCtx& ctx,
                                   platform::http::ResponseWriter& w) {
        receivedBody = ctx.body;
        w.setStatus(201);
        w.write("got:" + ctx.body);
        w.end();
    });
    DOCTEST_CHECK(srv.start());
    int port = srv.actualPort();
    DOCTEST_CHECK(waitForReady(port));

    std::string body;
    long status = httpPost("http://127.0.0.1:" + std::to_string(port) +
                           "/echo", "ping", body, "");
    DOCTEST_CHECK_EQ(status, 201L);
    DOCTEST_CHECK_EQ(body, std::string("got:ping"));
    DOCTEST_CHECK_EQ(receivedBody, std::string("ping"));

    srv.stop();
    return true;
})

// ---- Unknown route returns 404 ----

REGISTER_TEST(http_server_404_unknown, "http_server: unknown path returns 404", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/known", [](const platform::http::RequestCtx&,
                                 platform::http::ResponseWriter& w) {
        w.write("known");
        w.end();
    });
    DOCTEST_CHECK(srv.start());
    int port = srv.actualPort();
    DOCTEST_CHECK(waitForReady(port));

    std::string body;
    long status = httpGet("http://127.0.0.1:" + std::to_string(port) +
                          "/unknown", body);
    DOCTEST_CHECK_EQ(status, 404L);

    srv.stop();
    return true;
})

// ---- 无鉴权：带不带 Authorization 都直接放行（2026-09-27 起代理仅本机访问） ----

REGISTER_TEST(http_server_no_auth_required,
              "http_server: no auth required, any request passes through", {
    platform::http::Server srv("127.0.0.1", 0);

    int handledCount = 0;
    srv.route("GET", "/v1/chat",
              [&](const platform::http::RequestCtx&,
                  platform::http::ResponseWriter& w) {
                  ++handledCount;
                  w.setStatus(200);
                  w.write("ok");
                  w.end();
              });
    DOCTEST_CHECK(srv.start());
    int port = srv.actualPort();
    DOCTEST_CHECK(waitForReady(port));

    // 不带 Authorization：放行
    std::string body;
    long statusNoToken = httpGet("http://127.0.0.1:" +
                                 std::to_string(port) + "/v1/chat", body);
    DOCTEST_CHECK_EQ(statusNoToken, 200L);
    DOCTEST_CHECK_EQ(body, std::string("ok"));

    // 带任意 Authorization：同样放行（代理不看该头）
    std::string bodyAny;
    long statusAnyToken = httpGet(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat", bodyAny,
        "whatever-token");
    DOCTEST_CHECK_EQ(statusAnyToken, 200L);
    DOCTEST_CHECK_EQ(bodyAny, std::string("ok"));
    DOCTEST_CHECK_EQ(handledCount, 2);

    srv.stop();
    return true;
})

// ---- Custom status code and multi header pass through ----

REGISTER_TEST(http_server_custom_status_and_headers,
              "http_server: custom status code and headers", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/x", [](const platform::http::RequestCtx&,
                               platform::http::ResponseWriter& w) {
        w.setStatus(418);
        w.setHeader("X-Custom", "yes");
        w.setHeader("Content-Type", "text/plain");
        w.write("teapot");
        w.end();
    });
    DOCTEST_CHECK(srv.start());
    int port = srv.actualPort();
    DOCTEST_CHECK(waitForReady(port));

    std::string body;
    long status =
        httpGet("http://127.0.0.1:" + std::to_string(port) + "/x", body);
    DOCTEST_CHECK_EQ(status, 418L);
    DOCTEST_CHECK_EQ(body, std::string("teapot"));

    srv.stop();
    return true;
})

// ---- Multiple sequential stop/start cycles don't crash ----

REGISTER_TEST(http_server_stop_restart, "http_server: stop and start cycles", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/ping", [](const platform::http::RequestCtx&,
                                 platform::http::ResponseWriter& w) {
        w.write("pong");
        w.end();
    });

    for (int i = 0; i < 3; ++i) {
        DOCTEST_CHECK(srv.start());
        int port = srv.actualPort();
        DOCTEST_CHECK(waitForReady(port));
        std::string body;
        long status =
            httpGet("http://127.0.0.1:" + std::to_string(port) + "/ping", body);
        DOCTEST_CHECK_EQ(status, 200L);
        DOCTEST_CHECK_EQ(body, std::string("pong"));
        srv.stop();
        DOCTEST_CHECK_FALSE(srv.isRunning());
    }
    return true;
})

// ---- Request body size limit（spec §10.4：>10MB 拒绝） ----

REGISTER_TEST(http_server_body_size_limit,
              "http_server: 请求体 >10MB 拒绝（413），边界 10MB 通过", {
    platform::http::Server srv("127.0.0.1", 0);
    int handlerCalls = 0;
    srv.route("POST", "/echo",
              [&](const platform::http::RequestCtx& ctx,
                  platform::http::ResponseWriter& w) {
                  ++handlerCalls;
                  w.setStatus(200);
                  w.write("len:" + std::to_string(ctx.body.size()));
                  w.end();
              });
    DOCTEST_CHECK(srv.start());
    int port = srv.actualPort();
    DOCTEST_CHECK(waitForReady(port));

    // 恰好 10MB（边界，含）→ 200，handler 收到完整 body
    const std::string exact(platform::http::kMaxBodyBytes, 'x');
    std::string body;
    long statusExact = httpPost(
        "http://127.0.0.1:" + std::to_string(port) + "/echo", exact, body);
    DOCTEST_CHECK_EQ(statusExact, 200L);
    DOCTEST_CHECK_EQ(body,
                     std::string("len:") + std::to_string(exact.size()));

    // 10MB + 1 → 413，handler 不执行
    const std::string over(platform::http::kMaxBodyBytes + 1, 'x');
    const int callsBefore = handlerCalls;
    long statusOver = httpPost(
        "http://127.0.0.1:" + std::to_string(port) + "/echo", over, body);
    DOCTEST_CHECK_EQ(statusOver, 413L);
    DOCTEST_CHECK_EQ(handlerCalls, callsBefore);

    srv.stop();
    return true;
})