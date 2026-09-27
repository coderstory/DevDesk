// tests/test_modules/test_streaming_proxy.cpp
// SSE 流式转发测试（spec §10.4 / P10a）
//
// 测试架构：
//   [mock 上游 httplib::Server]  ← libcurl(StreamingResponse) ← [ProxyRouter]
//                                                                     ↓
//                                                    [platform::http::Server]
//                                                                     ↓
//                                                       [httplib::Client 收]
//
// 覆盖：
//   1. stream:true 检测（JSON 顶层字段）
//   2. baseUrl + path 拼接
//   3. SSE 上游 chunk 透传（顺序 + 完整性 + Content-Type）
//   4. 真流式（非 buffer）：chunk 到达节奏符合上游
//   5. backpressure：大批 chunk 不丢不乱序
//   6. 上游错误状态码透传（429）
//   7. cancel()：立即停止上游且不阻塞
//   8. 端到端：ProxyRouter + SSE Content-Type
//   9. 端到端：错误状态码透传
//  10. 客户端断开 → 上游请求被取消
//  11. stream:false 走缓冲路径（不受流式改动影响）
//  12. 上游中途截断 → 流干净结束
//  13. StreamingContext 结构契约
//  14. 并发流互不串扰
//
// 全部使用 127.0.0.1 随机端口 + 短超时，不依赖外网。
//
// 注意：tests/doctest.h 是极简框架（DOCTEST_CHECK 失败即 `return false`），
// 且 REGISTER_TEST 的 body 是宏参数 —— **大括号不保护逗号**，所以测试体内
// 避免顶层逗号（聚合初始化用括号包裹或改成分步赋值）。
#include "doctest.h"

#include "claude_service.h"
#include "core/proxy_stats.h"
#include "core/sqlite_db.h"
#include "platform/http_client.h"
#include "platform/http_server.h"
#include "platform/json.h"
#include "proxy_router.h"
#include "streaming_proxy.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono;
using module::claude::Profile;

// ---------- 通用工具 ----------

// 轮询等待谓词成立
template <typename Fn>
bool waitUntil(Fn fn, int waitMs) {
    const auto deadline = steady_clock::now() + milliseconds(waitMs);
    while (steady_clock::now() < deadline) {
        if (fn()) return true;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return fn();
}

// 构造第 i 个 SSE chunk（Anthropic 风格 content_block_delta）
std::string sseChunk(int i) {
    return std::string("data: {\"type\":\"content_block_delta\",\"index\":") +
           std::to_string(i) + "}\n\n";
}

// 构造 n 个 delta + 末尾 [DONE]
std::vector<std::string> makeSseScript(size_t n, bool withDone) {
    std::vector<std::string> out;
    out.reserve(n + 1);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(sseChunk(static_cast<int>(i)));
    }
    if (withDone) out.push_back("data: [DONE]\n\n");
    return out;
}

// 把所有 chunk 拼成一个期望字符串
std::string joinChunks(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += s;
    return out;
}

// 验证 text 中 sseChunk(0..n-1) 依序出现（顺序 + 完整性）
bool sseOrderOk(const std::string& text, int n) {
    size_t lastPos = 0;
    for (int i = 0; i < n; ++i) {
        const std::string needle = sseChunk(i);
        const size_t pos = text.find(needle);
        if (pos == std::string::npos) return false;
        if (pos < lastPos) return false;
        lastPos = pos + needle.size();
    }
    return true;
}

// ---------- mock 上游 SSE 服务器 ----------

struct MockScript {
    std::vector<std::string> chunks;
    int status = 200;
    std::string contentType = "text/event-stream";
    long delayMsPerChunk = 0;
    size_t stopAfterChunks = 0;   // >0：发到第 N 个就结束（模拟截断）
};

class MockSseServer {
public:
    MockSseServer() = default;
    ~MockSseServer() { stop(); }
    MockSseServer(const MockSseServer&) = delete;
    MockSseServer& operator=(const MockSseServer&) = delete;

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

    // 注册 POST /v1/messages，按 script 逐 chunk 吐 SSE
    void onMessages(const MockScript& script) {
        auto scriptPtr = std::make_shared<MockScript>(script);
        auto emitted = emitted_;
        auto finished = finished_;
        auto lastKey = lastApiKey_;
        auto lastBody = lastRequestBody_;

        svr_.Post("/v1/messages",
                  [scriptPtr, emitted, finished, lastKey, lastBody](
                      const httplib::Request& req, httplib::Response& res) {
                      *lastBody = req.body;
                      *lastKey = req.get_header_value("x-api-key");

                      res.status = scriptPtr->status;
                      if (scriptPtr->status != 200) {
                          res.set_content("{\"error\":\"upstream error\"}",
                                          "application/json");
                          finished->store(true);
                          return;
                      }

                      auto idx = std::make_shared<size_t>(0);
                      // releaser 兜底：cpp-httplib 在两次 provider 调用之间
                      // 发现连接断开（is_writable 失败）时会直接退出写循环、
                      // 不再调用 provider——sink.write 失败路径没机会执行，
                      // finished 只能在 ~Response() 的 releaser 里置位。
                      // （与产品侧 http_server.cpp 的 releaser 模式一致。）
                      res.set_chunked_content_provider(
                          scriptPtr->contentType,
                          [scriptPtr, idx, emitted, finished](
                              size_t /*offset*/, httplib::DataSink& sink) -> bool {
                              if (*idx >= scriptPtr->chunks.size()) {
                                  finished->store(true);
                                  return false;
                              }
                              if (scriptPtr->stopAfterChunks > 0 &&
                                  *idx >= scriptPtr->stopAfterChunks) {
                                  finished->store(true);
                                  return false;
                              }
                              const std::string& c = scriptPtr->chunks[*idx];
                              if (!sink.write(c.data(), c.size())) {
                                  // 客户端（代理）断开
                                  finished->store(true);
                                  return false;
                              }
                              ++(*idx);
                              emitted->store(*idx);
                              if (scriptPtr->delayMsPerChunk > 0) {
                                  std::this_thread::sleep_for(
                                      milliseconds(scriptPtr->delayMsPerChunk));
                              }
                              return true;
                          },
                          [finished](bool /*success*/) {
                              finished->store(true);
                          });
                  });
    }

    // 注册 GET /v1/models（缓冲路径用）
    void onModels(const std::string& jsonBody) {
        svr_.Get("/v1/models",
                 [jsonBody](const httplib::Request&, httplib::Response& res) {
                     res.set_content(jsonBody, "application/json");
                 });
    }

    // 注册 POST /v1/messages 的「静默」处理器：请求收下后一字节都不回
    //（模拟「上游连着但不发数据」——取消只能靠 libcurl 进度回调叫醒的实现
    // 就是在这一档上翻车的，见用例 20）。releaseSilent() 放行以便 join。
    void onMessagesSilent() {
        auto gate = silentGate_;
        svr_.Post("/v1/messages",
                  [gate](const httplib::Request&, httplib::Response& res) {
                      while (!gate->load()) {
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(20));
                      }
                      res.set_content("{}", "application/json");
                  });
    }

    // 放行静默处理器（必须在 stop() 之前调用，否则 stop 会等 handler 收工）
    void releaseSilent() { silentGate_->store(true); }

    std::string lastRequestBody() const { return lastRequestBody_->get(); }
    std::string lastApiKey() const { return lastApiKey_->get(); }
    size_t emitted() const { return emitted_->load(); }
    bool upstreamFinished() const { return finished_->load(); }

private:
    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;

    // 共享状态（用 shared_ptr<atomic> 以便 lambda 捕获；atomic 不可拷贝）
    std::shared_ptr<std::atomic<size_t>> emitted_ =
        std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<bool>> finished_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> apiKeyHits_ =
        std::make_shared<std::atomic<int>>(0);
    // 字符串用 mutex 保护的共享槽（简化：单请求场景足够）
    struct StringSlot {
        std::mutex m;
        std::string v;
        void set(const std::string& s) {
            std::lock_guard<std::mutex> lock(m);
            v = s;
        }
        StringSlot& operator=(const std::string& s) {
            set(s);
            return *this;
        }
        std::string get() {
            std::lock_guard<std::mutex> lock(m);
            return v;
        }
    };
    std::shared_ptr<StringSlot> lastApiKey_ = std::make_shared<StringSlot>();
    std::shared_ptr<StringSlot> lastRequestBody_ = std::make_shared<StringSlot>();
    // 静默 handler 的放行闸（onMessagesSilent / releaseSilent）
    std::shared_ptr<std::atomic<bool>> silentGate_ =
        std::make_shared<std::atomic<bool>>(false);
};

// ---------- 测试用 ClaudeService（内存库，不触碰 Keychain）----------

struct TestEnv {
    core::Result<core::data::SqliteDb> dbResult =
        core::data::SqliteDb::open(":memory:");
    std::unique_ptr<module::claude::ClaudeService> service;

    TestEnv() {
        if (dbResult.isOk()) {
            service = std::make_unique<module::claude::ClaudeService>(
                dbResult.value());
        }
    }

    module::claude::ClaudeService& svc() const { return *service; }
};

Profile makeProfile(const std::string& baseUrl) {
    Profile p;
    p.id = 1;
    p.name = "streaming-test";
    p.baseUrl = baseUrl;
    p.apiKeyPlain = "sk-ant-test-key";
    p.model = "claude-sonnet-4-5";
    p.isActive = true;
    return p;
}

// 把 Result<unique_ptr<T>> 里的指针 move 出来（避免拷贝）
std::unique_ptr<platform::http::StreamingResponse> takeStream(
    core::Result<std::unique_ptr<platform::http::StreamingResponse>>& r) {
    if (r.isFail()) return nullptr;
    auto& ref = r.value();
    return std::unique_ptr<platform::http::StreamingResponse>(std::move(ref));
}

// 读完整个流；返回 false 表示读到 Error
bool drainStream(platform::http::StreamingResponse& sr,
                 std::string& textOut,
                 std::string& errOut) {
    std::string chunk;
    int guard = 0;
    while (guard++ < 100000) {
        const auto cr = sr.readChunk(chunk, errOut);
        if (cr == platform::http::ChunkResult::More) {
            textOut += chunk;
            continue;
        }
        if (cr == platform::http::ChunkResult::End) return true;
        return false;
    }
    return false;
}

// 用 cpp-httplib Client 发带 content receiver 的 POST（客户端流式接收）
struct ClientStreamResult {
    bool ok = false;
    int status = 0;
    std::string contentType;
    std::string body;
    std::vector<std::string> receiverCalls;   // 每次 receiver 回调收到的数据
};

ClientStreamResult clientPostStream(httplib::Client& cli,
                                    const std::string& path,
                                    const std::string& body,
                                    bool abortAfterFirstChunk) {
    ClientStreamResult out;

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.body = body;
    req.headers.emplace("Content-Type", "application/json");

    req.response_handler = [&out](const httplib::Response& r) {
        out.status = r.status;
        auto it = r.headers.find("Content-Type");
        if (it != r.headers.end()) out.contentType = it->second;
        return true;
    };
    req.content_receiver = [&out, abortAfterFirstChunk](
                               const char* data, size_t len,
                               uint64_t /*offset*/, uint64_t /*total*/) {
        out.receiverCalls.emplace_back(data, len);
        out.body.append(data, len);
        if (abortAfterFirstChunk && out.receiverCalls.size() >= 1) {
            return false;   // 主动中止读取 → 关闭连接
        }
        return true;
    };

    auto res = cli.send(req);
    out.ok = static_cast<bool>(res);
    return out;
}

} // namespace

// ==================== 1. stream:true 检测 ====================

REGISTER_TEST(streaming_wants_stream_detection,
              "streaming: detect stream:true in request body", {
    using module::claude::wantsStream;

    DOCTEST_CHECK(wantsStream(R"({"model":"m","stream":true})"));
    DOCTEST_CHECK(wantsStream(R"({"stream" : true , "model":"m"})"));
    DOCTEST_CHECK(wantsStream("{\n  \"stream\":\n  true\n}"));
    DOCTEST_CHECK(wantsStream(R"({"a":1,"stream":true,"b":[1,2,3]})"));

    // 不是 true → 不走流式
    DOCTEST_CHECK_FALSE(wantsStream(R"({"stream":false})"));
    DOCTEST_CHECK_FALSE(wantsStream(R"({"stream":"true"})"));
    DOCTEST_CHECK_FALSE(wantsStream(R"({"model":"m"})"));
    DOCTEST_CHECK_FALSE(wantsStream(std::string()));
    DOCTEST_CHECK_FALSE(wantsStream("not json at all"));
    // 只有顶层的 stream 生效（嵌套对象不算）
    DOCTEST_CHECK_FALSE(wantsStream(R"({"a":{"stream":true}})"));
    return true;
})

// ==================== 2. 路径判定 + URL 拼接 ====================

REGISTER_TEST(streaming_path_and_url_join,
              "streaming: streamable path check + upstream url join", {
    using module::claude::isStreamablePath;
    using module::claude::buildUpstreamUrl;

    DOCTEST_CHECK(isStreamablePath("/v1/messages"));
    DOCTEST_CHECK(isStreamablePath("/v1/messages?beta=1"));
    DOCTEST_CHECK_FALSE(isStreamablePath("/v1/models"));
    DOCTEST_CHECK_FALSE(isStreamablePath("/health"));

    DOCTEST_CHECK_EQ(buildUpstreamUrl("https://api.anthropic.com", "/v1/messages"),
                     std::string("https://api.anthropic.com/v1/messages"));
    DOCTEST_CHECK_EQ(buildUpstreamUrl("https://api.anthropic.com/", "/v1/messages"),
                     std::string("https://api.anthropic.com/v1/messages"));
    DOCTEST_CHECK_EQ(buildUpstreamUrl("https://api.anthropic.com/", "v1/messages"),
                     std::string("https://api.anthropic.com/v1/messages"));
    DOCTEST_CHECK_EQ(buildUpstreamUrl("http://127.0.0.1:8888", ""),
                     std::string("http://127.0.0.1:8888/"));

    // 已是绝对 URL → 原样返回
    DOCTEST_CHECK_EQ(buildUpstreamUrl("https://ignored.example",
                                      "http://127.0.0.1:1234/v1/messages"),
                     std::string("http://127.0.0.1:1234/v1/messages"));
    return true;
})

// ==================== 3. SSE chunk 透传（顺序 + 完整性） ====================

REGISTER_TEST(streaming_send_streaming_claude_passthrough,
              "streaming: sendStreamingClaude forwards upstream sse chunks", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.chunks = makeSseScript(5, true);
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 10000;

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    std::string all;
    std::string err;
    DOCTEST_REQUIRE_MESSAGE(drainStream(*sr, all, err),
                            "stream ended with error: " + err);

    // 完整 + 顺序
    DOCTEST_CHECK_EQ(all, joinChunks(script.chunks));
    DOCTEST_CHECK(sseOrderOk(all, 5));
    DOCTEST_CHECK(all.find("data: [DONE]\n\n") != std::string::npos);

    DOCTEST_CHECK_EQ(sr->statusCode(), 200);
    DOCTEST_CHECK_EQ(sr->contentType(), std::string("text/event-stream"));
    // 上游看到的 x-api-key 来自 profile
    DOCTEST_CHECK_EQ(upstream.lastApiKey(), std::string("sk-ant-test-key"));

    upstream.stop();
    return true;
})

// ==================== 4. 真流式（非 buffer）：到达节奏 ====================

REGISTER_TEST(streaming_is_incremental_not_buffered,
              "streaming: chunks arrive incrementally not buffered", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.chunks = makeSseScript(3, true);
    script.delayMsPerChunk = 250;
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 15000;

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    const auto t0 = steady_clock::now();
    std::string chunk;
    std::string err;
    std::vector<long> arrivalMs;

    // 读到第 3 个 chunk 的到达时间应该明显晚于第 1 个（证明没有整体缓冲）
    for (int i = 0; i < 3; ++i) {
        const auto cr = sr->readChunk(chunk, err);
        DOCTEST_REQUIRE_MESSAGE(cr == platform::http::ChunkResult::More, err);
        arrivalMs.push_back(
            duration_cast<milliseconds>(steady_clock::now() - t0).count());
    }

    DOCTEST_CHECK_EQ(arrivalMs.size(), 3u);
    DOCTEST_CHECK(arrivalMs[1] >= arrivalMs[0]);
    DOCTEST_CHECK(arrivalMs[2] >= arrivalMs[1]);
    // 上游 250ms/chunk → 第 3 个至少比第 1 个晚 ~400ms
    DOCTEST_CHECK(arrivalMs[2] - arrivalMs[0] >= 400);

    upstream.stop();
    return true;
})

// ==================== 5. backpressure：大批 chunk 不丢不乱序 ====================

REGISTER_TEST(streaming_backpressure_large_volume,
              "streaming: backpressure preserves all chunks under load", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    constexpr size_t kCount = 400;   // 远超队列上限 100
    MockScript script;
    script.chunks = makeSseScript(kCount, false);
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 30000;

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    std::string all;
    std::string err;
    DOCTEST_REQUIRE_MESSAGE(drainStream(*sr, all, err),
                            "stream ended with error: " + err);

    // 全部 chunk 都在，且字节级一致（顺序 + 无丢失）
    DOCTEST_CHECK_EQ(all, joinChunks(script.chunks));
    DOCTEST_CHECK(sseOrderOk(all, static_cast<int>(kCount)));

    upstream.stop();
    return true;
})

// ==================== 6. 上游错误状态码 ====================

REGISTER_TEST(streaming_upstream_error_status_forwarded,
              "streaming: upstream error status is surfaced to caller", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.status = 429;
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 10000;

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    DOCTEST_CHECK(sr->waitForStatus(5000));
    DOCTEST_CHECK_EQ(sr->statusCode(), 429);

    std::string all;
    std::string err;
    (void)drainStream(*sr, all, err);
    DOCTEST_CHECK(all.find("upstream error") != std::string::npos);

    upstream.stop();
    return true;
})

// ==================== 7. cancel() 立即停止上游 ====================

REGISTER_TEST(streaming_cancel_stops_upstream,
              "streaming: cancel unblocks reader and stops upstream", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    std::vector<std::string> many;
    for (int i = 0; i < 1000; ++i) many.push_back(sseChunk(i));

    MockScript script;
    script.chunks = many;
    script.delayMsPerChunk = 5;
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 30000;

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    std::string chunk;
    std::string err;
    for (int i = 0; i < 3; ++i) {
        DOCTEST_REQUIRE(sr->readChunk(chunk, err) ==
                        platform::http::ChunkResult::More);
    }

    sr->cancel();

    // cancel 后 readChunk 必须立即返回 Error（不能阻塞到 30s 超时）
    const auto t0 = steady_clock::now();
    const auto cr = sr->readChunk(chunk, err);
    const auto elapsed =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    DOCTEST_CHECK(cr == platform::http::ChunkResult::Error);
    DOCTEST_CHECK_EQ(err, std::string("cancelled"));
    DOCTEST_CHECK(elapsed < 2000);

    // 上游不应跑完 1000 个
    waitUntil([&] { return upstream.upstreamFinished(); }, 3000);
    DOCTEST_CHECK(upstream.emitted() < many.size());

    upstream.stop();
    return true;
})

// ==================== 8. 端到端：ProxyRouter + SSE Content-Type ====================

REGISTER_TEST(streaming_proxy_end_to_end_sse,
              "streaming: proxy relays sse end-to-end with correct content type", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.chunks = makeSseScript(4, true);
    upstream.onMessages(script);

    TestEnv env;
    DOCTEST_REQUIRE(env.service != nullptr);

    platform::http::Server proxy("127.0.0.1", 0);
    module::claude::ProxyRouter router(env.svc(), proxy);
    const std::string upstreamBase = upstream.baseUrl();
    router.setProfileProvider([upstreamBase]() {
        return core::Result<Profile>::ok(makeProfile(upstreamBase));
    });
    router.registerRoutes();
    DOCTEST_REQUIRE(proxy.start());

    httplib::Client cli("127.0.0.1", proxy.actualPort());
    cli.set_read_timeout(15, 0);

    auto res = clientPostStream(cli, "/v1/messages",
                                R"({"model":"claude-sonnet-4-5","stream":true})",
                                false);

    DOCTEST_CHECK(res.ok);
    DOCTEST_CHECK_EQ(res.status, 200);
    // Content-Type 必须是 text/event-stream（spec §10.4）
    DOCTEST_CHECK(res.contentType.find("text/event-stream") != std::string::npos);
    DOCTEST_CHECK_EQ(res.body, joinChunks(script.chunks));
    DOCTEST_CHECK(sseOrderOk(res.body, 4));
    DOCTEST_CHECK_EQ(upstream.lastApiKey(), std::string("sk-ant-test-key"));

    proxy.stop();
    upstream.stop();
    return true;
})

// ==================== 9. 端到端：错误状态码透传 ====================

REGISTER_TEST(streaming_proxy_forwards_error_status,
              "streaming: proxy forwards upstream error status to client", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.status = 401;
    upstream.onMessages(script);

    TestEnv env;
    DOCTEST_REQUIRE(env.service != nullptr);

    platform::http::Server proxy("127.0.0.1", 0);
    module::claude::ProxyRouter router(env.svc(), proxy);
    const std::string upstreamBase = upstream.baseUrl();
    router.setProfileProvider([upstreamBase]() {
        return core::Result<Profile>::ok(makeProfile(upstreamBase));
    });
    router.registerRoutes();
    DOCTEST_REQUIRE(proxy.start());

    httplib::Client cli("127.0.0.1", proxy.actualPort());
    cli.set_read_timeout(10, 0);

    auto res = clientPostStream(cli, "/v1/messages", R"({"stream":true})", false);

    DOCTEST_CHECK(res.ok);
    DOCTEST_CHECK_EQ(res.status, 401);
    DOCTEST_CHECK(res.body.find("upstream error") != std::string::npos);

    proxy.stop();
    upstream.stop();
    return true;
})

// ==================== 10. 客户端断开 → 取消上游 ====================

REGISTER_TEST(streaming_proxy_client_disconnect_cancels_upstream,
              "streaming: client disconnect cancels upstream request", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    std::vector<std::string> many;
    for (int i = 0; i < 2000; ++i) many.push_back(sseChunk(i));

    MockScript script;
    script.chunks = many;
    script.delayMsPerChunk = 3;
    upstream.onMessages(script);

    TestEnv env;
    DOCTEST_REQUIRE(env.service != nullptr);

    platform::http::Server proxy("127.0.0.1", 0);
    module::claude::ProxyRouter router(env.svc(), proxy);
    const std::string upstreamBase = upstream.baseUrl();
    router.setProfileProvider([upstreamBase]() {
        return core::Result<Profile>::ok(makeProfile(upstreamBase));
    });
    router.registerRoutes();
    DOCTEST_REQUIRE(proxy.start());

    httplib::Client cli("127.0.0.1", proxy.actualPort());
    cli.set_read_timeout(15, 0);

    // 收到第一个 chunk 后主动中止读取 → 客户端关闭连接
    auto res = clientPostStream(cli, "/v1/messages", R"({"stream":true})",
                                /*abortAfterFirstChunk=*/true);

    // 客户端中止不是干净成功，这里只关心服务端反应
    DOCTEST_CHECK(res.receiverCalls.size() >= 1u);

    // 客户端断开后上游应在短时间内被取消（不跑完 2000 个）
    const bool stopped = waitUntil([&] { return upstream.upstreamFinished(); },
                                   5000);
    DOCTEST_CHECK(stopped);
    DOCTEST_CHECK(upstream.emitted() < many.size());

    proxy.stop();
    upstream.stop();
    return true;
})

// ==================== 11. stream:false 走缓冲路径 ====================

REGISTER_TEST(streaming_proxy_buffered_path_unaffected,
              "streaming: non-streaming requests still use buffered path", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());
    upstream.onMessages(MockScript());
    upstream.onModels(R"({"data":[{"id":"claude-sonnet-4-5"}]})");

    TestEnv env;
    DOCTEST_REQUIRE(env.service != nullptr);

    platform::http::Server proxy("127.0.0.1", 0);
    module::claude::ProxyRouter router(env.svc(), proxy);
    const std::string upstreamBase = upstream.baseUrl();
    router.setProfileProvider([upstreamBase]() {
        return core::Result<Profile>::ok(makeProfile(upstreamBase));
    });
    router.registerRoutes();
    DOCTEST_REQUIRE(proxy.start());

    httplib::Client cli("127.0.0.1", proxy.actualPort());
    cli.set_read_timeout(10, 0);

    // 非 /v1/messages 路径 → 缓冲转发
    auto res = cli.Get("/v1/models");
    DOCTEST_REQUIRE(res);
    DOCTEST_CHECK_EQ(res->status, 200);
    DOCTEST_CHECK(res->body.find("claude-sonnet-4-5") != std::string::npos);

    // 路由判定
    DOCTEST_CHECK(module::claude::ProxyRouter::shouldUseStreaming(
        "/v1/messages", R"({"stream":true})"));
    DOCTEST_CHECK_FALSE(module::claude::ProxyRouter::shouldUseStreaming(
        "/v1/messages", R"({"stream":false})"));
    DOCTEST_CHECK_FALSE(module::claude::ProxyRouter::shouldUseStreaming(
        "/v1/models", R"({"stream":true})"));

    proxy.stop();
    upstream.stop();
    return true;
})

// ==================== 12. 上游中途截断 ====================

REGISTER_TEST(streaming_upstream_truncation_ends_cleanly,
              "streaming: upstream truncation ends the stream cleanly", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.chunks = makeSseScript(10, false);
    script.stopAfterChunks = 3;
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 10000;

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    std::string all;
    std::string err;
    DOCTEST_CHECK(drainStream(*sr, all, err));
    DOCTEST_CHECK_EQ(all, sseChunk(0) + sseChunk(1) + sseChunk(2));

    upstream.stop();
    return true;
})

// ==================== 13. StreamingContext 结构契约 ====================

REGISTER_TEST(streaming_context_contract,
              "streaming: StreamingContext backpressure contract", {
    module::claude::StreamingContext ctx;
    DOCTEST_CHECK_EQ(module::claude::StreamingContext::kMaxQueueDepth, 100u);
    DOCTEST_CHECK_EQ(module::claude::StreamingContext::kResumeQueueDepth, 50u);
    DOCTEST_CHECK_FALSE(ctx.done.load());
    DOCTEST_CHECK_FALSE(ctx.error.load());
    DOCTEST_CHECK_EQ(ctx.depth(), 0u);

    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ctx.queue.push_back("a");
        ctx.queue.push_back("b");
    }
    DOCTEST_CHECK_EQ(ctx.depth(), 2u);

    ctx.done.store(true);
    DOCTEST_CHECK(ctx.done.load());

    DOCTEST_CHECK_EQ(std::string(module::claude::sse::kContentType),
                     std::string("text/event-stream"));
    DOCTEST_CHECK_EQ(std::string(module::claude::sse::kDoneMarker),
                     std::string("data: [DONE]\n\n"));
    DOCTEST_CHECK_EQ(std::string(module::claude::sse::kEventSeparator),
                     std::string("\n\n"));
    return true;
})

// ==================== 14. 并发流互不串扰 ====================

REGISTER_TEST(streaming_concurrent_requests_isolated,
              "streaming: concurrent streams do not cross chunks", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.chunks = makeSseScript(20, true);
    upstream.onMessages(script);

    const Profile profile = makeProfile(upstream.baseUrl());
    const std::string expected = joinChunks(script.chunks);

    auto runOne = [&profile](std::string* out) {
        platform::http::Request req;
        req.method = "POST";
        req.url = "/v1/messages";
        req.body = R"({"stream":true})";
        req.timeoutMs = 20000;

        auto r = module::claude::sendStreamingClaude(req, profile);
        if (r.isFail()) return;
        auto sr = takeStream(r);
        if (!sr) return;
        std::string err;
        (void)drainStream(*sr, *out, err);
    };

    std::string a;
    std::string b;
    std::thread t1([&] { runOne(&a); });
    std::thread t2([&] { runOne(&b); });
    t1.join();
    t2.join();

    DOCTEST_CHECK_EQ(a, expected);
    DOCTEST_CHECK_EQ(b, expected);

    upstream.stop();
    return true;
})

// ==================== 15. SSE usage 重组器（首页 Token 速率卡片） ====================

REGISTER_TEST(streaming_sse_usage_parser,
              "SseUsageParser: 跨 chunk 切断重组 / 多事件 / CRLF / 缺 usage / DONE / 畸形", {
    using module::claude::SseUsageParser;
    std::vector<int64_t> v;

    // 1. 事件从中间切断：TCP 分段边界 ≠ 事件边界
    SseUsageParser p1;
    v = p1.feed("event: message_delta\ndata: {\"type\":\"message_delta\"");
    DOCTEST_CHECK(v.empty());
    v = p1.feed(",\"delta\":{},\"usage\":{\"output_tokens\":42}}\n\n");
    DOCTEST_CHECK_EQ(v.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(v[0], 42);

    // 2. 一个 chunk 多事件（message_start 嵌套 + delta 顶层 + 无 usage 事件跳过）
    SseUsageParser p2;
    v = p2.feed("data: {\"type\":\"message_start\",\"message\":{\"usage\":"
                "{\"input_tokens\":10,\"output_tokens\":3}}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,"
                "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n"
                "data: {\"type\":\"message_delta\",\"delta\":{},"
                "\"usage\":{\"output_tokens\":15}}\n\n");
    DOCTEST_CHECK_EQ(v.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(v[0], 3);
    DOCTEST_CHECK_EQ(v[1], 15);

    // 3. \r\n 分隔符（含 \r\n\r\n 事件边界 + 行尾 \r）
    SseUsageParser p3;
    v = p3.feed("data: {\"type\":\"message_delta\",\"usage\":"
                "{\"output_tokens\":7}}\r\n\r\n");
    DOCTEST_CHECK_EQ(v.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(v[0], 7);

    // 4. [DONE] / 畸形 JSON / 非 data 行 / 无分隔符截断 → 全部安全跳过
    SseUsageParser p4;
    DOCTEST_CHECK(p4.feed("data: [DONE]\n\n").empty());
    DOCTEST_CHECK(p4.feed("data: {not json at all\n\n").empty());
    DOCTEST_CHECK(p4.feed("event: ping\n\n").empty());
    DOCTEST_CHECK(p4.feed("data: {\"type\":\"message_delta\"").empty());
    // 补完剩余字节后正常出值
    v = p4.feed(",\"usage\":{\"output_tokens\":9}}\n\n");
    DOCTEST_CHECK_EQ(v.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(v[0], 9);

    // 5. usage 缺 output_tokens / 非数字 → 跳过
    SseUsageParser p5;
    DOCTEST_CHECK(p5.feed("data: {\"type\":\"message_delta\",\"usage\":{}}\n\n")
                     .empty());
    DOCTEST_CHECK(p5.feed("data: {\"type\":\"message_delta\"}\n\n").empty());
    return true;
})

// ==================== 16. 端到端：流式统计接线 ====================

REGISTER_TEST(streaming_stats_end_to_end,
              "streaming: 代理流式请求驱动 proxy_stats（usage 累计 + 流结束回收）", {
    core::proxy_stats::resetForTest();
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    // message_start + 2 个 message_delta（累计 3 → 18 → 40）+ [DONE]
    MockScript script;
    script.chunks.push_back(
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":"
        "{\"input_tokens\":10,\"output_tokens\":3}}}\n\n");
    script.chunks.push_back(
        "data: {\"type\":\"message_delta\",\"delta\":{},"
        "\"usage\":{\"output_tokens\":18}}\n\n");
    script.chunks.push_back(
        "data: {\"type\":\"message_delta\",\"delta\":{},"
        "\"usage\":{\"output_tokens\":40}}\n\n");
    script.chunks.push_back("data: [DONE]\n\n");
    upstream.onMessages(script);

    TestEnv env;
    DOCTEST_REQUIRE(env.service != nullptr);

    platform::http::Server proxy("127.0.0.1", 0);
    module::claude::ProxyRouter router(env.svc(), proxy);
    const std::string upstreamBase = upstream.baseUrl();
    router.setProfileProvider([upstreamBase]() {
        return core::Result<Profile>::ok(makeProfile(upstreamBase));
    });
    router.registerRoutes();
    DOCTEST_REQUIRE(proxy.start());

    httplib::Client cli("127.0.0.1", proxy.actualPort());
    cli.set_read_timeout(15, 0);
    auto res = clientPostStream(cli, "/v1/messages",
                                R"({"model":"claude-sonnet-4-5","stream":true})",
                                false);
    DOCTEST_CHECK(res.ok);
    DOCTEST_CHECK_EQ(res.status, 200);

    // 流结束后（releaser 必触发）：累计 = 40，活动流归零
    DOCTEST_CHECK(waitUntil([] {
        const auto s = core::proxy_stats::snapshot();
        return s.activeStreams == 0 && s.sessionTokens == 40;
    }, 5000));
    {
        const auto s = core::proxy_stats::snapshot();
        DOCTEST_CHECK_EQ(s.sessionTokens, 40);
        DOCTEST_CHECK_EQ(s.activeStreams, 0);
    }

    proxy.stop();
    upstream.stop();
    core::proxy_stats::resetForTest();
    return true;
})

// ==================== 17. 上游断流 error 帧合成（W7 P0-2）====================

REGISTER_TEST(streaming_error_frame_synthesis,
              "streaming: 上游断流合成 event: error SSE 帧（合法 JSON + 转义）", {
    using module::claude::buildUpstreamErrorFrame;

    const std::string frame = buildUpstreamErrorFrame("Timeout was reached");
    DOCTEST_CHECK(frame.compare(0, 12, "event: error") == 0);
    DOCTEST_CHECK(frame.size() >= 4 &&
                  frame.compare(frame.size() - 2, 2, "\n\n") == 0);

    // data 行必须是合法 JSON（客户端要能解析出错误信息）
    const size_t dataPos = frame.find("data: ");
    DOCTEST_REQUIRE(dataPos != std::string::npos);
    const std::string payload = frame.substr(
        dataPos + 6, frame.find("\n\n", dataPos) - (dataPos + 6));
    auto r = platform::json::parse(payload);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(r.value().isObject());
    DOCTEST_CHECK_EQ(r.value().asObject().get("type").asString(),
                     std::string("error"));
    const auto errObj = r.value().asObject().get("error");
    DOCTEST_REQUIRE(errObj.isObject());
    DOCTEST_CHECK_EQ(errObj.asObject().get("message").asString(),
                     std::string("Timeout was reached"));

    // 转义：引号/反斜杠/换行 → 合法 JSON；\r 丢弃
    const std::string esc = buildUpstreamErrorFrame("a\"b\\c\nd\re");
    const size_t d2 = esc.find("data: ");
    const std::string p2 = esc.substr(d2 + 6, esc.find("\n\n", d2) - (d2 + 6));
    auto r2 = platform::json::parse(p2);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().asObject().get("error")
                         .asObject()
                         .get("message")
                         .asString(),
                     std::string("a\"b\\c\nde"));
    return true;
})

// ==================== 18. 低速空闲超时（W7 P0-3）====================

REGISTER_TEST(streaming_low_speed_idle_timeout,
              "streaming: 低速空闲超时中止（无总墙钟上限，无新 chunk 才判死）", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    MockScript script;
    script.chunks = makeSseScript(3, false);
    script.delayMsPerChunk = 15000;   // 第 1 个 chunk 之后挂 15s（模拟断流）
    upstream.onMessages(script);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 0;                // 无总墙钟上限
    req.lowSpeedBytesPerSec = 1;      // 速率 < 1 B/s
    req.lowSpeedTimeSec = 3;          // 持续 3s → curl 中止（CURLE_OPERATION_TIMEDOUT）

    auto r = module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    // 第 1 个 chunk 正常到达（空闲超时不影响活跃数据）
    std::string chunk;
    std::string err;
    DOCTEST_REQUIRE(sr->readChunk(chunk, err) ==
                    platform::http::ChunkResult::More);

    // 之后无新数据 → 低速超时触发 Error。libcurl 的低速检测基于滑动窗口
    // 速率采样，实测要么 3-10s 内自行中止、要么等下一次 socket 事件
    // （mock 的 15s chunk 到达）时才中止；个别轮次还会先把那个 chunk
    // 交付出来再中止。所以这里读到终态为止（Error/End），只断言
    // "远早于无超时的无限挂死"，不卡精确时刻。
    const auto t0 = steady_clock::now();
    platform::http::ChunkResult cr = platform::http::ChunkResult::More;
    while (cr == platform::http::ChunkResult::More &&
           steady_clock::now() - t0 < milliseconds(25000)) {
        cr = sr->readChunk(chunk, err);
    }
    const auto elapsed =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();
    DOCTEST_CHECK(cr == platform::http::ChunkResult::Error);
    DOCTEST_CHECK(elapsed >= 1500);    // 不是立即失败（确认真在等数据）
    DOCTEST_CHECK(elapsed < 25000);    // 两条中止路径都覆盖

    upstream.stop();
    return true;
})

// ==================== 19. 在途流进行中停服（审计 2026-09-23 [9]）====================
// 回归：stopProxy 先 cancelActiveStreams() 再 server stop()。旧实现
// Server::stop() 只关监听 socket 就 join worker，在途流的 worker 阻塞在
// readChunk（上游持续产出时空闲超时永不触发），「停止代理」/应用退出会
// 挂到流自然结束（幽灵进程）。本测试上游脚本约 1000 chunk × 10ms ≈ 10s，
// 取消生效后 stop 必须秒级返回。
REGISTER_TEST(streaming_proxy_stop_unblocks_inflight_stream,
              "streaming: server stop with an in-flight stream returns promptly", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());

    std::vector<std::string> many;
    for (int i = 0; i < 1000; ++i) many.push_back(sseChunk(i));
    MockScript script;
    script.chunks = many;
    script.delayMsPerChunk = 10;
    upstream.onMessages(script);

    TestEnv env;
    DOCTEST_REQUIRE(env.service != nullptr);

    platform::http::Server proxy("127.0.0.1", 0);
    module::claude::ProxyRouter router(env.svc(), proxy);
    const std::string upstreamBase = upstream.baseUrl();
    router.setProfileProvider([upstreamBase]() {
        return core::Result<Profile>::ok(makeProfile(upstreamBase));
    });
    router.registerRoutes();
    DOCTEST_REQUIRE(proxy.start());

    // 客户端线程：持续流式接收（content_receiver 一直 true）
    std::atomic<bool> gotFirstChunk{false};
    const int proxyPort = proxy.actualPort();
    std::thread client([proxyPort, &gotFirstChunk] {
        httplib::Client cli("127.0.0.1", proxyPort);
        cli.set_read_timeout(30, 0);
        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/messages";
        req.body = R"({"stream":true})";
        req.headers.emplace("Content-Type", "application/json");
        req.response_handler = [](const httplib::Response&) { return true; };
        req.content_receiver = [&gotFirstChunk](
            const char* /*data*/, size_t len, uint64_t /*off*/, uint64_t /*total*/) {
            if (len > 0) gotFirstChunk.store(true);
            return true;
        };
        cli.send(req);
    });

    DOCTEST_CHECK(waitUntil([&] { return gotFirstChunk.load(); }, 10000));

    // 与 stopProxy 相同顺序：先取消在途流，再停 server
    const auto t0 = steady_clock::now();
    env.svc().cancelActiveStreams();
    proxy.stop();
    const auto stopMs = duration_cast<milliseconds>(
        steady_clock::now() - t0).count();
    DOCTEST_CHECK(stopMs < 2000);

    client.join();
    DOCTEST_CHECK(waitUntil([&] { return upstream.upstreamFinished(); }, 3000));
    DOCTEST_CHECK(upstream.emitted() < many.size());

    upstream.stop();
    return true;
})

// ==================== 20. 上游静默（连着不发数据）时取消必须秒级生效 ====================
// 回归（H1，2026-09-27）：cancelWithReason 只置 cancelled 标志 + notify_all，
// 旧实现的两条唤醒路径都要求「有人正好堵在队列/回调上」——上游**连着但不发
// 一个字节**（请求已送出、响应头未到）时 write callback 从未进入，消费者
// 醒来也叫不动 libcurl 线程，于是 libcurl 一直等到低速空闲超时
//（生产 lowSpeedTimeSec=60s → 实测 60~70s）才退；而 stopProxy / 退出应用要
// 等 worker join，就被拖着挂同样时长。
// 修复：进度回调（CURLOPT_XFERINFOFUNCTION + NOPROGRESS=0）在上游静默期也按
// 秒级节拍被调用，回调里读 cancelled 原子量 → 立即 CURLE_ABORTED_BY_CALLBACK。
// 本用例把低速超时设为 5s（生产值 60s）以限制回归时用例自身的挂起时长；
// 断言 cancel → libcurl 线程 join 完成 ≤ 2s。取消原因语义一并断言。
// 实测（2026-09-27，本机）：带修复 ~500ms 稳定复现；把进度回调关掉
//（NOPROGRESS=1，等价修复前）同一用例 10592ms —— 即旧实现挂在低速空闲
// 超时上（生产 60s 配置对应 60~70s）。
REGISTER_TEST(streaming_cancel_silent_upstream_prompt,
              "streaming: cancel is prompt when upstream stays silent", {
    MockSseServer upstream;
    DOCTEST_REQUIRE(upstream.start());
    upstream.onMessagesSilent();

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = R"({"stream":true})";
    req.timeoutMs = 0;               // 无总墙钟上限（与生产一致）
    req.lowSpeedBytesPerSec = 1;
    req.lowSpeedTimeSec = 5;         // 生产 60；测试缩到 5 以限制回归挂起时长

    auto r =
        module::claude::sendStreamingClaude(req, makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    // 静默 500ms：确保请求已送出、libcurl 真卡在「等响应头」而不是还没连上
    std::this_thread::sleep_for(milliseconds(500));

    std::string chunk;
    std::string err;
    const auto t0 = steady_clock::now();
    sr->cancelWithReason("proxy stopping");
    const auto cr = sr->readChunk(chunk, err);   // 消费者路径：立即 Error
    sr.reset();   // 析构 = cancel + join：join 返回即 libcurl 线程真的退出了
    const auto cancelMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    upstream.releaseSilent();
    upstream.stop();

    // 实测值打出来（同 [stress] 系列风格）：回归时一眼看出是从 1s 级退化到
    // 低速超时量级，还是别的路径变慢
    std::cout << "[stream] 静默上游 cancel→libcurl 线程退出: " << cancelMs
              << "ms" << std::endl;

    DOCTEST_CHECK(cr == platform::http::ChunkResult::Error);
    DOCTEST_CHECK_EQ(err, std::string("proxy stopping"));
    DOCTEST_CHECK_MESSAGE(cancelMs < 2000,
                          "cancel→libcurl 线程退出实测 " +
                              std::to_string(cancelMs) +
                              "ms（要求 <2000ms；修复前挂在低速超时上，"
                              "本用例配置实测 ~10.6s）");
    return true;
})
