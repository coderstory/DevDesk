#include "doctest.h"
#include "test_support.h"
#include "platform/http_client.h"
#include "platform/http_server.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

REGISTER_TEST(http_build_request, "http: build Request", {
    platform::http::Request req;
    req.method = "GET";
    req.url = "https://example.com";
    DOCTEST_CHECK_EQ(req.method, std::string("GET"));
    DOCTEST_CHECK_EQ(req.url, std::string("https://example.com"));
    DOCTEST_CHECK_EQ(req.timeoutMs, 30000);
    DOCTEST_CHECK(req.verifyTls);
    return true;
})

REGISTER_TEST(http_build_response, "http: build Response", {
    platform::http::Response resp;
    resp.statusCode = 200;
    resp.body = "OK";
    DOCTEST_CHECK_EQ(resp.statusCode, 200);
    DOCTEST_CHECK_EQ(resp.body, std::string("OK"));
    DOCTEST_CHECK_EQ(resp.elapsedMs, 0);
    DOCTEST_CHECK_FALSE(resp.networkError);
    return true;
})

// 原 http_chunk_result_enum（三个 ChunkResult 成员互不等）已删：enum class 成员
// 两两不等是语言保证的恒真式。ChunkResult 的真实行为由下方真实 HTTP 用例覆盖。

// ============ 真实 HTTP 行为（审计 TIN-11：原 3 例只回显结构体默认值，从不发送）============
// 全部打本地 127.0.0.1（Server 随机端口），不依赖外网、不依赖真实代理。

namespace {

// 作用域内把 no_proxy/NO_PROXY 指到回环：curl 从环境变量取代理
//（http_client.cpp 未显式设 CURLOPT_PROXY），公司代理环境下 127.0.0.1 的
// 请求可能被劫持。大小写两份都设（curl 先查小写；Windows 部分运行时只认
// 一份），析构还原原值——写法照 test_support.h 的 setEnvVar 作用域还原。
bool readEnvVar(const char* name, std::string& out) {
    const char* v = std::getenv(name);
    if (!v) return false;
    out = v;
    return true;
}

struct ScopedLoopbackNoProxy {
    bool hadLower = false;
    bool hadUpper = false;
    std::string savedLower;
    std::string savedUpper;

    ScopedLoopbackNoProxy() {
        hadLower = readEnvVar("no_proxy", savedLower);
        hadUpper = readEnvVar("NO_PROXY", savedUpper);
        test_support::setEnvVar("no_proxy", "127.0.0.1,localhost,::1");
        test_support::setEnvVar("NO_PROXY", "127.0.0.1,localhost,::1");
    }
    ~ScopedLoopbackNoProxy() {
        test_support::setEnvVar("no_proxy", hadLower ? savedLower.c_str() : "");
        test_support::setEnvVar("NO_PROXY", hadUpper ? savedUpper.c_str() : "");
    }
    ScopedLoopbackNoProxy(const ScopedLoopbackNoProxy&) = delete;
    ScopedLoopbackNoProxy& operator=(const ScopedLoopbackNoProxy&) = delete;
};

// 借一个空闲回环端口再立刻释放：之后该端口上「连接被拒」是确定行为
//（Server("127.0.0.1", 0) 走 bind_to_any_port 由内核分配，start() 返回前
// 已 wait_until_ready → 端口真实且没被别人占）。
int grabClosedLoopbackPort() {
    platform::http::Server srv("127.0.0.1", 0);
    if (!srv.start()) return -1;
    const int port = srv.actualPort();
    srv.stop();
    return port;
}

} // namespace

REGISTER_TEST(http_send_refused_local,
              "http: 连接被拒（本地已关闭端口）→ 失败 + 错误信息非空", {
    const int deadPort = grabClosedLoopbackPort();
    DOCTEST_REQUIRE(deadPort > 0);
    ScopedLoopbackNoProxy noProxy;

    platform::http::Request req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(deadPort) + "/nope";
    req.timeoutMs = 3000;

    const auto t0 = std::chrono::steady_clock::now();
    const auto res = platform::http::send(req);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();

    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK(!res.errorMessage().empty());
    DOCTEST_CHECK(elapsedMs < 5000);  // 被拒是立刻返回，不该等满超时
    return true;
})

REGISTER_TEST(http_send_timeout_local,
              "http: 超时（本地 /slow 睡 1.5s，timeoutMs=300）→ 失败且提前返回", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/slow",
              [](const platform::http::RequestCtx&, platform::http::ResponseWriter& w) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                  w.write("late");
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    ScopedLoopbackNoProxy noProxy;

    platform::http::Request req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(srv.actualPort()) + "/slow";
    req.timeoutMs = 300;

    const auto t0 = std::chrono::steady_clock::now();
    const auto res = platform::http::send(req);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();

    DOCTEST_CHECK(res.isFail());
    // 不断言文案（curl 错误串随版本变），只断言没等满 handler 的 1.5s
    DOCTEST_CHECK(elapsedMs < 1000);
    srv.stop();
    return true;
})

REGISTER_TEST(http_stream_error_terminal,
              "http: 流式 readChunk 终态（连接被拒 Error 粘滞 + cancelWithReason 带原因）", {
    ScopedLoopbackNoProxy noProxy;

    // ---- 1) 打已关闭端口：readChunk 立即 Error 且错误串非空，再读仍 Error ----
    const int deadPort = grabClosedLoopbackPort();
    DOCTEST_REQUIRE(deadPort > 0);
    platform::http::Request refused;
    refused.method = "GET";
    refused.url = "http://127.0.0.1:" + std::to_string(deadPort) + "/stream";
    refused.timeoutMs = 3000;
    auto refusedStream = platform::http::sendStreaming(refused);
    DOCTEST_REQUIRE(refusedStream.isOk());

    std::string chunk;
    std::string err;
    DOCTEST_CHECK(refusedStream.value()->readChunk(chunk, err) ==
                  platform::http::ChunkResult::Error);
    DOCTEST_CHECK(!err.empty());
    chunk.clear();
    err.clear();
    DOCTEST_CHECK(refusedStream.value()->readChunk(chunk, err) ==
                  platform::http::ChunkResult::Error);  // 终态粘滞：再读仍 Error

    // ---- 2) 长驻本地流 + cancelWithReason：readChunk 返回 Error + 该 reason ----
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/live",
              [](const platform::http::RequestCtx&, platform::http::ResponseWriter& w) {
                  w.setChunkProvider(
                      "text/event-stream",
                      [](platform::http::ResponseWriter::ChunkWriter writeChunk,
                         platform::http::ResponseWriter::IsWritableFn isWritable) -> bool {
                          static const char kChunk[] = "data: x\n\n";
                          // 长驻：每 50ms 一块、上限 5s（客户端会提前取消）
                          for (int i = 0; i < 100; ++i) {
                              if (!isWritable()) return false;
                              if (!writeChunk(kChunk, sizeof(kChunk) - 1)) return false;
                              std::this_thread::sleep_for(std::chrono::milliseconds(50));
                          }
                          return false;
                      });
              });
    DOCTEST_REQUIRE(srv.start());

    platform::http::Request live;
    live.method = "GET";
    live.url = "http://127.0.0.1:" + std::to_string(srv.actualPort()) + "/live";
    live.timeoutMs = 3000;
    auto liveStream = platform::http::sendStreaming(live);
    DOCTEST_REQUIRE(liveStream.isOk());
    (void)liveStream.value()->waitForStatus(2000);
    liveStream.value()->cancelWithReason("test");

    platform::http::ChunkResult r = platform::http::ChunkResult::More;
    for (int i = 0; i < 20 && r == platform::http::ChunkResult::More; ++i) {
        chunk.clear();
        err.clear();
        r = liveStream.value()->readChunk(chunk, err);  // 先排空取消前已入队的 chunk
    }
    DOCTEST_CHECK(r == platform::http::ChunkResult::Error);
    DOCTEST_CHECK_EQ(err, std::string("test"));
    srv.stop();
    return true;
})