#include "doctest.h"
#include "test_support.h"
#include "platform/http_client.h"
#include "platform/http_client_ca.h"
#include "platform/http_server.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <thread>

REGISTER_TEST(http_build_request, "http: build Request", {
    platform::http::Request req;
    req.method = "GET";
    req.url = "https://example.com";
    DOCTEST_CHECK_EQ(req.method, std::string("GET"));
    DOCTEST_CHECK_EQ(req.url, std::string("https://example.com"));
    // 默认值不可省：timeoutMs / verifyTls 的缺省值就是生产侧的静默契约，
    // 改了会直接改变所有未显式设置的请求行为（超时、TLS 校验）。
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

// ChunkResult 的 enum class 成员两两不等是语言保证的恒真式，不另立用例；
// 真实行为由下方真实 HTTP 用例覆盖。

// 真实 HTTP 行为：全部打本地 127.0.0.1（Server 随机端口），不依赖外网、不依赖
// 真实代理。上面两个 build_* 用例只回显结构体默认值，从不发送。

namespace {

// 作用域内把 no_proxy/NO_PROXY 指到回环：curl 从环境变量取代理，而
// http_client.cpp 未显式设 CURLOPT_PROXY，公司代理环境下 127.0.0.1 的请求会被
// 劫持（测试就会假红）。大小写两份都设（curl 先查小写；Windows 部分运行时只认
// 一份），析构还原原值——照 test_support.h 的 setEnvVar 作用域还原。
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

// 借一个空闲回环端口再立刻释放：之后该端口上「连接被拒」是确定行为。Server
// 构造传 0 = bind_to_any_port 由内核分配，start() 返回前已 wait_until_ready，
// 故实际端口真实且当下没被别人占；stop() 之后才可能被抢走。
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
    DOCTEST_CHECK(elapsedMs < 5000);  // 被拒是立刻返回；放宽到 5s 容忍调度抖动，但不等于等满 3s 超时
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

    // 1) 打已关闭端口：readChunk 立即 Error 且错误串非空，再读仍 Error
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

    // 2) 长驻本地流 + cancelWithReason：readChunk 返回 Error + 该 reason
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/live",
              [](const platform::http::RequestCtx&, platform::http::ResponseWriter& w) {
                  w.setChunkProvider(
                      "text/event-stream",
                      [](platform::http::ResponseWriter::ChunkWriter writeChunk,
                         platform::http::ResponseWriter::IsWritableFn isWritable) -> bool {
                          static const char kChunk[] = "data: x\n\n";
                          // 长驻：每 50ms 一块、上限 5s；客户端会提前 cancelWithReason，
                          // 所以正常路径下循环走不满 100 轮。
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
        r = liveStream.value()->readChunk(chunk, err);  // 先排空 cancel 前已入队的 chunk，
                                                      // 否则会在 More 上耗尽 20 轮预算
    }
    DOCTEST_CHECK(r == platform::http::ChunkResult::Error);
    DOCTEST_CHECK_EQ(err, std::string("test"));
    srv.stop();
    return true;
})
// ── CA 策略(CA 批:DEVDESK_CA_BUNDLE 覆盖口,DevDesk 自有约定)────────

REGISTER_TEST(http_ca_policy_env_unset_defaults_to_native,
              "http ca: DEVDESK_CA_BUNDLE 未设 → 平台信任根策略(Windows NATIVE_CA 现状不变)", {
    const auto policy = platform::http::resolveCaPolicy("");
    DOCTEST_REQUIRE(policy.isOk());
    DOCTEST_CHECK(policy.value().useNative);
    DOCTEST_CHECK_EQ(policy.value().caInfo, std::string());
    return true;
})

REGISTER_TEST(http_ca_policy_bundle_file_activates_cainfo,
              "http ca: DEVDESK_CA_BUNDLE 指向存在文件 → Bundle 校验(CAINFO),不叠加 NATIVE", {
    // 防的事故:NATIVE_CA 依赖本机 ROOT 库,缺根证书(如 example.com 链)即 CURLE 60;
    // 显式 bundle 是完整校验的替代信任根,不是关校验。
    const auto bundlePath = test_support::tempConfigFile("http_ca", "bundle");
    {
        std::ofstream o(bundlePath, std::ios::binary | std::ios::trunc);
        o << "-----BEGIN CERTIFICATE-----\nMIIBtest\n-----END CERTIFICATE-----\n";
    }
    const auto policy = platform::http::resolveCaPolicy(bundlePath.string());
    DOCTEST_REQUIRE(policy.isOk());
    DOCTEST_CHECK_FALSE(policy.value().useNative);
    DOCTEST_CHECK_EQ(policy.value().caInfo, bundlePath.string());
    std::filesystem::remove(bundlePath);
    return true;
})

REGISTER_TEST(http_ca_policy_missing_file_fails_readable,
              "http ca: DEVDESK_CA_BUNDLE 指向不存在路径 → 可读失败(含变量名与路径)", {
    const std::string missing =
        (std::filesystem::temp_directory_path() /
         "devdesk_ca_no_such_bundle.pem")
            .string();
    std::filesystem::remove(missing);  // 确保不存在
    const auto policy = platform::http::resolveCaPolicy(missing);
    DOCTEST_REQUIRE(policy.isFail());
    DOCTEST_CHECK(policy.errorMessage().find(
                      std::string(platform::http::kCaBundleEnv)) !=
                  std::string::npos);
    DOCTEST_CHECK(policy.errorMessage().find("devdesk_ca_no_such_bundle.pem") !=
                  std::string::npos);
    return true;
})

// ── 代理/CA 批审计修复(findings = docs/superpowers/plans/2026-10-27-mcp-proxy-ca-audit.md)──

REGISTER_TEST(http_tcp_probe_pins_env_proxy,
              "http: tcpReachable 钉掉 env 代理——http_proxy 指向死端口时探测仍直达本机(F1/F4)", {
    // 活监听 + 死代理:修前探测经死代理连「代理的回环」→ false;修后 NOPROXY "*"
    // 直达 → true。控制组:已关闭端口必须如实 false。
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/ping",
              [](const platform::http::RequestCtx&, platform::http::ResponseWriter& w) {
                  w.write("ok");
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int deadProxy = grabClosedLoopbackPort();
    DOCTEST_REQUIRE(deadProxy > 0);
    const std::string dead = "http://127.0.0.1:" + std::to_string(deadProxy);
    const test_support::ScopedEnv lowerHttp("http_proxy", dead);
    const test_support::ScopedEnv allLower("all_proxy", dead);
    const test_support::ScopedEnv allUpper("ALL_PROXY", dead);
    DOCTEST_CHECK(platform::http::tcpReachable(
        "127.0.0.1:" + std::to_string(srv.actualPort()),
        std::chrono::milliseconds(3000)));
    srv.stop();
    DOCTEST_CHECK_FALSE(platform::http::tcpReachable(
        "127.0.0.1:" + std::to_string(deadProxy), std::chrono::milliseconds(3000)));
    return true;
})

REGISTER_TEST(http_send_streaming_honors_request_proxy,
              "http: sendStreaming 遵循 Request.proxy——死代理必败,不得静默直连(F3)", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/live",
              [](const platform::http::RequestCtx&, platform::http::ResponseWriter& w) {
                  w.write("hello");
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int deadProxy = grabClosedLoopbackPort();
    DOCTEST_REQUIRE(deadProxy > 0);
    platform::http::Request req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(srv.actualPort()) + "/live";
    req.timeoutMs = 3000;
    req.proxy = "127.0.0.1:" + std::to_string(deadProxy);
    auto stream = platform::http::sendStreaming(req);
    DOCTEST_REQUIRE(stream.isOk());
    std::string chunk;
    std::string err;
    // 显式代理是死端口 → 连接失败 Error;修前忽略 proxy 直连本地活服务 → More(红)
    DOCTEST_CHECK(stream.value()->readChunk(chunk, err) ==
                  platform::http::ChunkResult::Error);
    DOCTEST_CHECK(!err.empty());
    srv.stop();
    return true;
})

REGISTER_TEST(http_ca_bundle_missing_send_fails_readable,
              "http ca: bundle 指向缺失路径时 send 先于网络可读失败——接线入口断言(F2)", {
    const std::string missing =
        (std::filesystem::temp_directory_path() / "devdesk_ca_missing_send.pem")
            .string();
    std::filesystem::remove(missing);
    const test_support::ScopedEnv bundle(platform::http::kCaBundleEnv, missing);
    const int deadPort = grabClosedLoopbackPort();
    DOCTEST_REQUIRE(deadPort > 0);
    platform::http::Request req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(deadPort) + "/nope";
    req.timeoutMs = 1000;
    const auto res = platform::http::send(req);
    // 失败串只能产自 CA 解析路径(先于 curl 句柄创建)——删掉 send 内的解析
    // 接线此断言即红
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK(res.errorMessage().find(std::string(platform::http::kCaBundleEnv)) !=
                  std::string::npos);
    DOCTEST_CHECK(res.errorMessage().find("devdesk_ca_missing_send.pem") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(http_ca_bundle_missing_send_streaming_fails_readable,
              "http ca: sendStreaming 同款先于网络可读失败——接线入口断言(F2)", {
    const std::string missing =
        (std::filesystem::temp_directory_path() / "devdesk_ca_missing_stream.pem")
            .string();
    std::filesystem::remove(missing);
    const test_support::ScopedEnv bundle(platform::http::kCaBundleEnv, missing);
    const int deadPort = grabClosedLoopbackPort();
    DOCTEST_REQUIRE(deadPort > 0);
    platform::http::Request req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(deadPort) + "/nope";
    req.timeoutMs = 1000;
    const auto res = platform::http::sendStreaming(req);
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK(res.errorMessage().find(std::string(platform::http::kCaBundleEnv)) !=
                  std::string::npos);
    DOCTEST_CHECK(res.errorMessage().find("devdesk_ca_missing_stream.pem") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(http_ca_policy_directory_path_fails_readable,
              "http ca: bundle 指向目录 → 提前可读报错,不留给 perform 期 libcurl 英文错(F6)", {
    const auto dir = std::filesystem::temp_directory_path() / "devdesk_ca_dir_probe";
    std::filesystem::create_directories(dir);
    const auto policy = platform::http::resolveCaPolicy(dir.string());
    DOCTEST_REQUIRE(policy.isFail());
    DOCTEST_CHECK(policy.errorMessage().find(std::string(platform::http::kCaBundleEnv)) !=
                  std::string::npos);
    DOCTEST_CHECK(policy.errorMessage().find(dir.string()) != std::string::npos);
    std::filesystem::remove(dir);
    return true;
})
