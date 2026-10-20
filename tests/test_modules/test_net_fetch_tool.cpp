// tests/test_modules/test_net_fetch_tool.cpp
// net.fetch 行为契约。全程离线：本地 platform::http::Server（127.0.0.1 随机口）当
// 假上游，绝不真外网请求（公司网环境 + 测试稳定性红线）。
// 覆盖面（每条对应一个真实事故面）：
//   - 本地固定响应 → body 命中（含 http→JSON 字符串回喂形状）
//   - 超长响应按 32KB 截断（对齐 agent_loop kMaxToolResultBytes 的上下文护栏，
//     防「长页面烧爆上下文致上游 400」复发）
//   - 非 http/https scheme 直接拒绝且不触网（scheme 白名单在 fetcher 之前）
//   - 缺 url 参数 → 可读错误
//   - 超时路径：短超时注入 + 本地慢 handler → 可读中文超时文案（含 curl 原文）
//   - 非 2xx → 错误文本含状态码
//   - 二进制 Content-Type → 「二进制/非文本」前缀 + 32KB 预算截断
// 跨文件前提：
//   - tests/doctest.h 极简框架：DOCTEST_CHECK 失败展开为 return false，断言必须平铺
//     （不进 lambda）。
//   - Server::start 同步 bind 但 listen 线程异步起——首连可能撞上未就绪，用例内
//     以「重试窗口内反复 execute」宽限（同 test_http_server.cpp 的 waitForReady 动机）。
#include "doctest.h"

#include "core/tool.h"
#include "module_agent/net_fetch_tool.h"
#include "platform/http_client.h"
#include "platform/http_server.h"
#include "platform/json.h"

#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace {

// contentJson 是 JSON 字符串（toolResultText 对 JSON 字符串原样透传）——
// 解出字符串即 LLM 实际看到的文本
bool decodeText(const core::Result<core::ToolResult>& r, std::string& out) {
    if (r.isFail()) return false;
    auto parsed = platform::json::parse(r.value().contentJson);
    if (parsed.isFail() || !parsed.value().isString()) return false;
    out = parsed.value().asString();
    return true;
}

// 用 JSON 字符串字面量拼 {"url": "..."} 参数（url 内是纯 ASCII，无转义问题）
std::string urlArgs(const std::string& url) {
    return "{\"url\":\"" + url + "\"}";
}

} // namespace

REGISTER_TEST(net_fetch_local_server_fixed_response,
              "net.fetch: 本地固定响应 → body 命中（离线，不碰外网）", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/hello",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setHeader("Content-Type", "text/plain");
                  w.write("NETFETCH-MARKER-OK");
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int port = srv.actualPort();
    module::agent::NetFetchTool tool;
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/hello";

    // listen 就绪宽限：2s 窗口内反复 execute，出现首个非 error 结果即收
    core::Result<core::ToolResult> r;
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
        r = tool.execute(core::ToolArgs{urlArgs(url)});
        ok = r.isOk() && !r.value().isError;
        if (!ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    srv.stop();
    std::string text;
    DOCTEST_REQUIRE_MESSAGE(ok && decodeText(r, text),
                            "本地 fetch 2s 内未成功: " +
                                (r.isOk() ? r.value().contentJson
                                          : r.errorMessage()));
    DOCTEST_CHECK(text.find("NETFETCH-MARKER-OK") != std::string::npos);
    return true;
})

REGISTER_TEST(net_fetch_truncates_long_response,
              "net.fetch: 超长响应按 32KB 截断（含尾注，防长页面烧爆上下文）", {
    const std::string big(100u * 1024u, 'x');   // 100KB 文本响应
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/big",
              [&big](const platform::http::RequestCtx&,
                     platform::http::ResponseWriter& w) {
                  w.setHeader("Content-Type", "text/plain");
                  w.write("HEAD-");
                  w.write(big);
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int port = srv.actualPort();
    module::agent::NetFetchTool tool;
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/big";

    core::Result<core::ToolResult> r;
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
        r = tool.execute(core::ToolArgs{urlArgs(url)});
        ok = r.isOk() && !r.value().isError;
        if (!ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    srv.stop();
    std::string text;
    DOCTEST_REQUIRE_MESSAGE(ok && decodeText(r, text),
                            "本地 fetch 2s 内未成功: " +
                                (r.isOk() ? r.value().contentJson
                                          : r.errorMessage()));
    DOCTEST_CHECK(text.find("HEAD-") == 0);
    // 预算含尾注：最终文本恰 ≤ 32KB，loop 侧不再二切
    DOCTEST_CHECK_MESSAGE(text.size() <= 32u * 1024u,
                          "截断后应 ≤ 32KB，实测 " + std::to_string(text.size()));
    DOCTEST_CHECK(text.find("已截断") != std::string::npos);
    DOCTEST_CHECK(text.size() < big.size());
    return true;
})

REGISTER_TEST(net_fetch_rejects_non_http_scheme,
              "net.fetch: 非 http/https scheme 拒绝且不触网（fetcher 零调用）", {
    int fetcherCalls = 0;
    module::agent::NetFetchTool tool(
        [&fetcherCalls](const platform::http::Request&) {
            ++fetcherCalls;   // scheme 校验在前，走到这里 = 校验失效
            platform::http::Response res;
            res.statusCode = 200;
            return core::Result<platform::http::Response>::ok(res);
        });

    auto r = tool.execute(core::ToolArgs{urlArgs("ftp://example.com/file")});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(r.value().isError);
    std::string text;
    DOCTEST_REQUIRE(decodeText(r, text));
    DOCTEST_CHECK(text.find("http/https") != std::string::npos);
    DOCTEST_CHECK_EQ(fetcherCalls, 0);   // 拒绝发生在网络层之前

    // 缺 url 参数同批：可读错误 + 不触网
    auto r2 = tool.execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().isError);
    std::string text2;
    DOCTEST_REQUIRE(decodeText(r2, text2));
    DOCTEST_CHECK(text2.find("url") != std::string::npos);
    DOCTEST_CHECK_EQ(fetcherCalls, 0);
    return true;
})

REGISTER_TEST(net_fetch_timeout_error_text,
              "net.fetch: 超时 → 可读中文错误（短超时对本地慢 handler）", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/slow",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  // 1.5s 响应 > 300ms 超时：curl 必走 CURLE_OPERATION_TIMEDOUT
                  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                  w.write("late");
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int port = srv.actualPort();
    // 生产 fetcher + 注入短超时（生产默认 10s，见 kNetFetchDefaultTimeoutMs）
    module::agent::NetFetchTool tool(module::agent::defaultNetFetcher, 300);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/slow";

    // listen 就绪宽限：起动竞态期的「连接失败」错误不算数，等真正的超时错误
    core::Result<core::ToolResult> r;
    std::string text;
    bool sawTimeout = false;
    for (int i = 0; i < 40 && !sawTimeout; ++i) {
        r = tool.execute(core::ToolArgs{urlArgs(url)});
        if (r.isOk() && r.value().isError && decodeText(r, text)) {
            sawTimeout = text.find("超时") != std::string::npos;
        }
        if (!sawTimeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    srv.stop();
    DOCTEST_REQUIRE_MESSAGE(sawTimeout,
                            "未观测到超时错误文本，实测: " + text);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK(r.value().isError);
    DOCTEST_CHECK(text.find("net.fetch 失败") == 0);
    return true;
})

REGISTER_TEST(net_fetch_non_2xx_error_text,
              "net.fetch: 非 2xx → 错误文本含状态码与响应片段", {
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/missing",
              [](const platform::http::RequestCtx&,
                 platform::http::ResponseWriter& w) {
                  w.setStatus(404);
                  w.write("nope-here");
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int port = srv.actualPort();
    module::agent::NetFetchTool tool;
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/missing";

    core::Result<core::ToolResult> r;
    std::string text;
    bool got404 = false;
    // listen 就绪宽限：起动竞态期是「连接失败」而非 404，重试直到出现真正的 404 文本
    for (int i = 0; i < 40 && !got404; ++i) {
        r = tool.execute(core::ToolArgs{urlArgs(url)});
        if (r.isOk() && r.value().isError && decodeText(r, text)) {
            got404 = text.find("404") != std::string::npos;
        }
        if (!got404) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    srv.stop();
    DOCTEST_REQUIRE_MESSAGE(got404, "2s 内未观测到 404 错误文本，实测: " + text);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK(r.value().isError);
    DOCTEST_CHECK(text.find("非 2xx") != std::string::npos);
    DOCTEST_CHECK(text.find("nope-here") != std::string::npos);   // 原因线索
    return true;
})

REGISTER_TEST(net_fetch_binary_content_type_prefix,
              "net.fetch: 二进制 Content-Type → 「二进制/非文本」前缀 + 32KB 截断", {
    const std::string bin(40u * 1024u, 'B');   // 40KB「二进制」（ASCII 字节即够断言）
    platform::http::Server srv("127.0.0.1", 0);
    srv.route("GET", "/bin",
              [&bin](const platform::http::RequestCtx&,
                     platform::http::ResponseWriter& w) {
                  w.setHeader("Content-Type", "application/octet-stream");
                  w.write("BIN-HEAD");
                  w.write(bin);
                  w.end();
              });
    DOCTEST_REQUIRE(srv.start());
    const int port = srv.actualPort();
    module::agent::NetFetchTool tool;
    const std::string url =
        "http://127.0.0.1:" + std::to_string(port) + "/bin";

    core::Result<core::ToolResult> r;
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
        r = tool.execute(core::ToolArgs{urlArgs(url)});
        ok = r.isOk() && !r.value().isError;
        if (!ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    srv.stop();
    std::string text;
    DOCTEST_REQUIRE_MESSAGE(ok && decodeText(r, text),
                            "本地 fetch 2s 内未成功: " +
                                (r.isOk() ? r.value().contentJson
                                          : r.errorMessage()));
    DOCTEST_CHECK(text.find("二进制/非文本，已截断") == 0);   // spec 前缀提示
    DOCTEST_CHECK(text.find("BIN-HEAD") != std::string::npos);
    DOCTEST_CHECK_MESSAGE(text.size() <= 32u * 1024u,
                          "截断后应 ≤ 32KB，实测 " + std::to_string(text.size()));
    DOCTEST_CHECK(text.size() < 7u + bin.size());   // 确实截了
    return true;
})
