// tests/live/mcp_live.cpp
// B4 live 驱动：MCP Streamable HTTP 服务的进程内实连验收 + 实连供进 Claude Code。
//
//   mcp_live --check   进程内起 McpService(port 0)，经真实回环 HTTP 跑完整传输
//                      断言（initialize→tools/list→tools/call→DELETE + 404/415/
//                      405/202/重启后旧 session 404），并对真网页抓取做可用性
//                      探测——不可用（离线/出口受限）时打 [skip] 记录，不算失败。
//   mcp_live --serve   globalMcpService().start(0) 后向 stdout 打印 PORT=<实际>
//                      并 fflush，随后阻塞——供 scripts/run-mcp-live.sh 解析。
//
// 写法纪律（台账 B4-01/B4-02）：零平台宏、纯标准库 + 仓库头；include 一律
// "module_mcp/…" 前缀形态（不依赖 -I src/module_mcp）——本文件经
// if(DEVDESK_BUILD_LIVE_TOOLS) 裸块进 Windows 交叉编译受编集合，写法不洁会
// 污染后续所有门禁。
#include "module_mcp/mcp_service.h"
#include "module_mcp/mcp_tools_builtin.h"
#include "platform/http_client.h"
#include "platform/json.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

namespace {

using module::mcp::McpService;
using platform::http::Request;
using platform::http::Response;

int g_failures = 0;

void check(const bool ok, const std::string& what) {
    std::fprintf(stderr, "  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

Response httpCall(const std::string& method, const int port,
                  const std::map<std::string, std::string>& headers,
                  const std::string& body) {
    Request req;
    req.method = method;
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    req.headers = headers;
    req.body = body;
    req.timeoutMs = 15000;
    auto r = platform::http::send(req);
    if (r.isFail()) {
        Response failed;
        failed.networkError = true;
        failed.errorMessage = r.errorMessage();
        return failed;
    }
    return r.value();
}

Response post(const int port, const std::map<std::string, std::string>& extra,
              const std::string& body) {
    std::map<std::string, std::string> headers{
        {"Content-Type", "application/json"}};
    for (const auto& kv : extra) headers[kv.first] = kv.second;
    return httpCall("POST", port, headers, body);
}

// initialize 响应的 Mcp-Session-Id；缺失返回空串。
std::string sessionOf(const Response& resp) {
    const auto it = resp.headers.find("Mcp-Session-Id");
    return it == resp.headers.end() ? std::string() : it->second;
}

platform::json::JsonValue parseJsonBody(const std::string& body) {
    auto parsed = platform::json::parse(body);
    return parsed.isOk() ? parsed.value() : platform::json::JsonValue();
}

// tools/call 的 result.content[0].text；解析失败 isError=true 且返回空串。
std::string callText(const Response& resp, bool* isErrorOut) {
    *isErrorOut = true;
    const auto root = parseJsonBody(resp.body);
    if (!root.isObject()) return std::string();
    const auto result = root.asObject().get("result");
    if (!result.isObject()) return std::string();
    const auto errorFlag = result.asObject().get("isError");
    *isErrorOut = errorFlag.isBool() && errorFlag.asBool();
    const auto content = result.asObject().get("content");
    if (!content.isArray() || content.size() == 0) return std::string();
    const auto block = content[0];
    if (!block.isObject()) return std::string();
    const auto text = block.asObject().get("text");
    return text.isString() ? text.asString() : std::string();
}

int runCheck() {
    std::fprintf(stderr,
                 "mcp_live --check：进程内起 McpService(port 0) 跑传输断言\n");

    core::mcp::McpToolRegistry tools;
    // 默认 fetcher = platform::http::send（curl）——真网页抓取不经 mock。
    module::mcp::registerBuiltinMcpTools(tools);
    McpService svc(tools);
    const auto started = svc.start(0);
    check(started.isOk(), "McpService.start(0)");
    if (!started.isOk()) return 1;
    int port = started.value();

    // ── 传输层判序断言 ──────────────────────────────────────────────
    // 未知 session：先于任何 JSON 解析判 404
    {
        const Response r = post(
            port, {{"Mcp-Session-Id", "00000000000000000000000000deadbeef"}},
            R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        check(r.statusCode == 404, "未知 session → 404");
    }
    // Content-Type 缺失 / 非 JSON → 415
    {
        const Response missing =
            httpCall("POST", port, {},
                     R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
        check(missing.statusCode == 415, "缺 Content-Type → 415");
        const Response wrong = httpCall(
            "POST", port, {{"Content-Type", "text/plain"}},
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
        check(wrong.statusCode == 415, "Content-Type 非 JSON → 415");
    }
    // GET / OPTIONS → 405 且 Allow: POST, DELETE（不提供 SSE）
    {
        const Response get = httpCall("GET", port, {}, "");
        check(get.statusCode == 405, "GET → 405");
        const auto allow = get.headers.find("Allow");
        check(allow != get.headers.end() &&
                  allow->second.find("POST") != std::string::npos &&
                  allow->second.find("DELETE") != std::string::npos,
              "GET 405 带 Allow: POST, DELETE");
        const Response options = httpCall("OPTIONS", port, {}, "");
        check(options.statusCode == 405, "OPTIONS → 405");
    }

    // ── initialize → tools/list → tools/call server_info ────────────
    std::string session;
    {
        const Response r = post(
            port, {},
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp_live","version":"0"}}})");
        check(r.statusCode == 200, "initialize → 200");
        session = sessionOf(r);
        check(!session.empty(), "initialize 下发 Mcp-Session-Id");
        const auto root = parseJsonBody(r.body);
        check(root.isObject() && root.asObject().has("result"),
              "initialize 响应含 result");
    }
    {
        const Response r =
            post(port, {{"Mcp-Session-Id", session}},
                 R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        check(r.statusCode == 200, "tools/list → 200");
        check(r.body.find("devdesk_server_info") != std::string::npos &&
                  r.body.find("web_fetch_markdown") != std::string::npos,
              "tools/list 含 devdesk_server_info 与 web_fetch_markdown");
    }
    {
        const Response r = post(
            port, {{"Mcp-Session-Id", session}},
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"devdesk_server_info","arguments":{}}})");
        check(r.statusCode == 200, "tools/call devdesk_server_info → 200");
        bool isError = true;
        const std::string text = callText(r, &isError);
        check(!isError, "server_info 非 isError");
        check(text.find("DevDesk 版本") != std::string::npos &&
                  text.find("MCP 协议版本") != std::string::npos,
              "server_info 输出含版本与协议信息");
    }
    // 通知（无 id）→ 202 空 body
    {
        const Response r = post(
            port, {{"Mcp-Session-Id", session}},
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        check(r.statusCode == 202 && r.body.empty(), "通知 → 202 空 body");
    }

    // ── 真网页抓取可用性探测（不可用 → [skip]，不计失败）──────────
    {
        const Response r = post(
            port, {{"Mcp-Session-Id", session}},
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"web_fetch_markdown","arguments":{"url":"https://example.com/","max_chars":2000}}})");
        if (r.statusCode != 200) {
            std::fprintf(stderr,
                         "  [skip] 真网页抓取不可用：tools/call HTTP %d\n",
                         r.statusCode);
        } else {
            bool isError = true;
            const std::string text = callText(r, &isError);
            if (isError && text.find("网络请求失败") != std::string::npos) {
                std::fprintf(stderr,
                             "  [skip] 真网页抓取不可用（离线/出口受限）：%s\n",
                             text.c_str());
            } else if (isError) {
                check(false, "真网页抓取报错（非网络类，视为失败）: " + text);
            } else if (text.empty()) {
                check(false, "真网页抓取成功但输出为空");
            } else {
                std::fprintf(stderr,
                             "  [ok] 真网页抓取可用（example.com 抓得 %zu 字）\n",
                             text.size());
            }
        }
    }

    // ── DELETE → 200；再 DELETE → 404 ───────────────────────────────
    {
        const Response ok =
            httpCall("DELETE", port, {{"Mcp-Session-Id", session}}, "");
        check(ok.statusCode == 200, "DELETE 已知 session → 200");
        const Response again =
            httpCall("DELETE", port, {{"Mcp-Session-Id", session}}, "");
        check(again.statusCode == 404, "DELETE 已删 session → 404");
    }

    // ── 重启（清会话表）→ 旧 session 404 ────────────────────────────
    {
        const auto restarted = svc.restart(0);
        check(restarted.isOk(), "svc.restart(0)");
        if (restarted.isOk()) {
            port = restarted.value();
            const Response stale = post(
                port, {{"Mcp-Session-Id", session}},
                R"({"jsonrpc":"2.0","id":9,"method":"ping"})");
            check(stale.statusCode == 404, "重启后旧 session → 404");
        }
    }
    svc.stop();
    check(!svc.isRunning(), "stop 后不再运行");

    std::fprintf(stderr, "mcp_live --check：%s（失败 %d 项）\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

int runServe() {
    // 与 main.cpp 装配段同口径：内置工具注册进全局注册表（常驻），服务挂
    // globalMcpService（单例永不析构）。
    module::mcp::registerBuiltinMcpTools(core::mcp::globalMcpTools());
    const auto started = module::mcp::globalMcpService().start(0);
    if (!started.isOk()) {
        std::fprintf(stderr, "mcp_live --serve：start 失败 — %s\n",
                     started.errorMessage().c_str());
        return 1;
    }
    std::printf("PORT=%d\n", started.value());
    std::fflush(stdout);  // 脚本轮询依赖：必须先于阻塞可见
    // 供进外部 Claude Code 进程：永久阻塞，由调用方按 PID 结束
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(3600));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check" || arg == "--serve") mode = arg;
    }
    if (mode == "--check") return runCheck();
    if (mode == "--serve") return runServe();
    std::fprintf(stderr, "用法: mcp_live --check | --serve\n");
    return 2;
}
