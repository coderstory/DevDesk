// tests/test_modules/test_mcp_service.cpp
// McpService 生命周期 + 真实 HTTP 行为（port 0 / 幂等 / 占用 / Host·Origin / 在途 stop）。
// 纪律：测试一律 port 0（OS 分临时端口），「端口占用」用例的被占端口来自排他
// holder bind(0) 后 getsockname 的实际端口——全文件零固定端口，防并发跑测试时
// 撞别人的监听。
// 防的事故：固定端口冲突、重启后旧监听残留、失败无提示、在途调用被硬杀、
// 会话表跨重启复活。

// 平台 socket 头最先包含（Windows 下早于任何可能拖入 windows.h 的头），仅供
// 排他 holder 使用；httplib 在别的 TU 自带 winsock2，此处同序不冲突。
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "doctest.h"
#include "mcp_service.h"
#include "core/mcp_tool.h"
#include "platform/http_client.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace {

using module::mcp::McpService;
using module::mcp::ServiceState;
using platform::http::Request;
using platform::http::Response;

// 真实 HTTP 往返（platform::http::send 已在 devdesk_tests 链接集内，零新依赖）。
// lambda/返回非 bool 的函数内不用 doctest 宏（宏展开含 return false）；失败用
// statusCode=0 表达，下游断言自然翻红。
Response httpCall(const std::string& method, int port,
                  const std::map<std::string, std::string>& headers,
                  const std::string& body) {
    Request req;
    req.method = method;
    req.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    req.headers = headers;
    req.body = body;
    req.timeoutMs = 5000;
    auto r = platform::http::send(req);
    if (r.isFail()) {
        Response failed;
        failed.networkError = true;
        failed.errorMessage = r.errorMessage();
        return failed;
    }
    return r.value();
}

Response postJson(int port, const std::string& body,
                  const std::map<std::string, std::string>& extra = {}) {
    std::map<std::string, std::string> headers{
        {"Content-Type", "application/json"}};
    for (const auto& kv : extra) headers[kv.first] = kv.second;
    return httpCall("POST", port, headers, body);
}

// initialize 握手并取 session id（响应头 Mcp-Session-Id）；失败返回空串。
std::string initializeSession(int port) {
    const Response resp = postJson(
        port, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    if (resp.statusCode != 200) return std::string();
    const auto it = resp.headers.find("Mcp-Session-Id");
    return it != resp.headers.end() ? it->second : std::string();
}

// 在途工具：execute 睡 300ms 后返回固定文本（验证 stop 等待收敛，不硬杀）。
class SlowTool final : public core::mcp::IMcpTool {
public:
    std::string name() const override { return "devdesk_slow_probe"; }
    std::string description() const override { return "在途收敛探针"; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }
    core::Result<core::mcp::ToolResult> execute(
        const std::string&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        core::mcp::ToolResult out;
        out.text = "slow-done";
        return core::Result<core::mcp::ToolResult>::ok(out);
    }
};

// ── 排他端口 holder（端口占用用例专用）────────────────────────────────
// 占口必须用「排他」语义的裸 socket，不能用第二个 McpService：httplib 服务端
// socket 默认带 SO_REUSEADDR（httplib.h `socket_options_ = default_socket_options`），
// Windows 语义下带 REUSEADDR 的第二个 bind 可直接落在已占用端口上（双绑定不报
// 错），占用检测必假阴性；POSIX 无 REUSEADDR 的活动 LISTEN 本就排他，第二个
// bind 仍 EADDRINUSE。Windows 侧设 SO_EXCLUSIVEADDRUSE 对带 REUSEADDR 的后来者
// 返回 WSAEACCES，才真正守住 spec §4.1「端口占用 → fail」语义。
#if defined(_WIN32)
using HolderSocket = SOCKET;
constexpr HolderSocket kInvalidHolderSocket = INVALID_SOCKET;
#else
using HolderSocket = int;
constexpr HolderSocket kInvalidHolderSocket = -1;
#endif

// bind(0) 占一个临时端口并把实际端口写入 actualPort（维持全文件零固定端口）。
// 占口姿势二选一（对应两份占用用例的探针语义）：
// - Exclusive：SO_EXCLUSIVEADDRUSE，挡一切后来者——「真占用 → fail」的正路；
// - ReuseAddr：与 httplib 服务端默认同款（httplib.h default_socket_options），
//   Windows 语义下两个都带 REUSEADDR 的 bind 可共存同一端口——这是修复前
//   「占用检测假成功」缺陷的复现姿势（等价第二个 DevDesk/httplib 实例）。
// 失败返回 kInvalidHolderSocket。Windows 进程内 WSAStartup 由含 httplib 的其它
// 编译单元的静态初始化完成（引用计数进程级生效），此处不重复初始化。
enum class HolderKind { Exclusive, ReuseAddr };
HolderSocket holdPort(int& actualPort, HolderKind kind) {
    actualPort = 0;
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kInvalidHolderSocket;
    if (kind == HolderKind::Exclusive) {
        BOOL exclusiveOpt = TRUE;
        if (::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<const char*>(&exclusiveOpt),
                         sizeof(exclusiveOpt)) != 0) {
            ::closesocket(s);
            return kInvalidHolderSocket;
        }
    }
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return kInvalidHolderSocket;
#endif
    if (kind == HolderKind::ReuseAddr) {
        const int one = 1;
        (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&one), sizeof(one));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;  // port 0 = OS 分临时端口
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 4) != 0) {
#if defined(_WIN32)
        ::closesocket(s);
#else
        ::close(s);
#endif
        return kInvalidHolderSocket;
    }
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
#if defined(_WIN32)
        ::closesocket(s);
#else
        ::close(s);
#endif
        return kInvalidHolderSocket;
    }
    actualPort = ntohs(bound.sin_port);
    return s;
}

void closeHolderSocket(HolderSocket s) {
    if (s == kInvalidHolderSocket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

} // namespace

REGISTER_TEST(mcp_service_start_port0_binds_actual_port,
              "mcp: start(0) 返回实际端口；isRunning/port/status 三态一致", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    DOCTEST_CHECK(port > 0);
    DOCTEST_CHECK(svc.isRunning());
    DOCTEST_CHECK(svc.port() == port);
    const auto st = svc.status();
    DOCTEST_CHECK(st.state == ServiceState::Running);
    DOCTEST_CHECK(st.port == port);
    DOCTEST_CHECK(st.lastError.empty());
    svc.stop();
    DOCTEST_CHECK(!svc.isRunning());
    DOCTEST_CHECK(svc.port() == 0);
    DOCTEST_CHECK(svc.status().state == ServiceState::Stopped);
    return true;
})

REGISTER_TEST(mcp_service_stop_idempotent_twice,
              "mcp: stop 幂等（连停两次不崩、不翻状态）", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    svc.stop();
    svc.stop();  // 二次 stop 必须是 no-op，不是崩溃或状态错乱
    DOCTEST_CHECK(!svc.isRunning());
    DOCTEST_CHECK(svc.status().state == ServiceState::Stopped);
    DOCTEST_CHECK(svc.status().lastError.empty());
    return true;
})

REGISTER_TEST(mcp_service_restart_rebinds_new_port,
              "mcp: restart 换端口（旧监听收束、新端口可服务）", {
    McpService svc;
    auto first = svc.start(0);
    DOCTEST_REQUIRE(first.isOk());
    auto second = svc.restart(0);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(svc.isRunning());
    DOCTEST_CHECK(second.value() > 0);
    // 新监听必须真能应答（防「状态说 Running 但旧 Server 已死」）
    const Response resp = postJson(
        second.value(),
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    DOCTEST_CHECK(resp.statusCode == 200);
    svc.stop();
    return true;
})

REGISTER_TEST(mcp_service_port_conflict_fails_with_last_error,
              "mcp: 端口被占用 → fail + lastError（不静默假成功）", {
    // holder 用排他裸 socket 占口（为何不能用第二个 McpService 见 helper 注释）
    int occupied = 0;
    HolderSocket holder = holdPort(occupied, HolderKind::Exclusive);
    DOCTEST_REQUIRE(holder != kInvalidHolderSocket);
    DOCTEST_REQUIRE(occupied > 0);

    McpService victim;
    auto conflict = victim.start(occupied);
    DOCTEST_REQUIRE(conflict.isFail());
    const auto st = victim.status();
    DOCTEST_CHECK(st.state == ServiceState::Failed);
    DOCTEST_CHECK(!st.lastError.empty());
    DOCTEST_CHECK(!victim.isRunning());
    closeHolderSocket(holder);
    return true;
})

REGISTER_TEST(mcp_service_start_fails_on_reuseaddr_occupied_port,
              "mcp: 端口被 REUSEADDR 型服务占用 → start 如实 fail（防双绑定假成功）", {
    // holder 与修复前的 httplib 服务端同款（默认 SO_REUSEADDR）：Windows 语义下
    // 两个都带 REUSEADDR 的 bind 共存同一端口、start 假成功——即第二个 DevDesk
    // 实例或任意 REUSEADDR 服务先占口时，本进程必须仍如实报「端口被占」。
    int occupied = 0;
    HolderSocket holder = holdPort(occupied, HolderKind::ReuseAddr);
    DOCTEST_REQUIRE(holder != kInvalidHolderSocket);
    DOCTEST_REQUIRE(occupied > 0);

    McpService victim;
    auto conflict = victim.start(occupied);
    DOCTEST_REQUIRE(conflict.isFail());
    DOCTEST_CHECK(!victim.isRunning());
    closeHolderSocket(holder);
    return true;
})

REGISTER_TEST(mcp_service_host_whitelist_rejects_foreign_host,
              "mcp: Host 头不在回环白名单 → 403（防本机网页借道伪造）", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    Response resp = httpCall("POST", port,
                             {{"Host", "evil.example.com"},
                              {"Content-Type", "application/json"}},
                             R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    DOCTEST_CHECK(resp.statusCode == 403);
    // 回环拼写放行（同值请求换白名单内 Host → 进协议层）
    Response ok = httpCall("POST", port,
                           {{"Host", "localhost"},
                            {"Content-Type", "application/json"}},
                           R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    DOCTEST_CHECK(ok.statusCode == 200);
    svc.stop();
    return true;
})

REGISTER_TEST(mcp_service_foreign_origin_rejected,
              "mcp: 非回环 Origin → 403；无 Origin 放行（非浏览器客户端）", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    Response evil = postJson(
        port, R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
        {{"Origin", "http://evil.example.com"}});
    DOCTEST_CHECK(evil.statusCode == 403);
    Response clean = postJson(
        port, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    DOCTEST_CHECK(clean.statusCode == 200);
    svc.stop();
    return true;
})

REGISTER_TEST(mcp_service_get_and_options_rejected_405,
              "mcp: GET/OPTIONS /mcp → 405 且 Allow: POST, DELETE（不提供 SSE）", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    for (const char* method : {"GET", "OPTIONS"}) {
        Response resp = httpCall(method, port, {}, "");
        DOCTEST_CHECK_EQ(resp.statusCode, 405);
        const auto allow = resp.headers.find("Allow");
        DOCTEST_REQUIRE(allow != resp.headers.end());
        DOCTEST_CHECK(allow->second.find("POST") != std::string::npos);
        DOCTEST_CHECK(allow->second.find("DELETE") != std::string::npos);
    }
    svc.stop();
    return true;
})

REGISTER_TEST(mcp_service_notification_202_no_body,
              "mcp: 通知（无 id）→ 202 空 body（不应答、不建会话）", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const Response resp = postJson(
        started.value(),
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    DOCTEST_CHECK_EQ(resp.statusCode, 202);
    DOCTEST_CHECK(resp.body.empty());
    svc.stop();
    return true;
})

REGISTER_TEST(mcp_service_unknown_session_404_and_cleared_on_restart,
              "mcp: 未知 session 404；重启后旧 session 复活被 404 拦下", {
    McpService svc;
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    const std::string session = initializeSession(port);
    DOCTEST_REQUIRE(!session.empty());

    // 未知 session id（格式合法但不在表内）→ 404
    Response unknown = postJson(
        port, R"({"jsonrpc":"2.0","id":2,"method":"ping"})",
        {{"Mcp-Session-Id", "00000000000000000000000000deadbeef"}});
    DOCTEST_CHECK(unknown.statusCode == 404);

    // 已知 session 可用 → restart 清表 → 同一 id 必 404（防旧会话复活）
    Response known = postJson(
        port, R"({"jsonrpc":"2.0","id":3,"method":"ping"})",
        {{"Mcp-Session-Id", session}});
    DOCTEST_CHECK(known.statusCode == 200);
    auto restarted = svc.restart(0);
    DOCTEST_REQUIRE(restarted.isOk());
    Response stale = postJson(
        restarted.value(), R"({"jsonrpc":"2.0","id":4,"method":"ping"})",
        {{"Mcp-Session-Id", session}});
    DOCTEST_CHECK(stale.statusCode == 404);
    svc.stop();
    return true;
})

REGISTER_TEST(mcp_service_stop_waits_inflight_tool_then_converges,
              "mcp: 在途工具（300ms）期间 stop 不崩且等待收敛（不硬杀 worker）", {
    core::mcp::McpToolRegistry localTools;
    auto tool = std::make_shared<SlowTool>();
    DOCTEST_REQUIRE(localTools.registerTool(tool).isOk());
    McpService svc(localTools);
    auto started = svc.start(0);
    DOCTEST_REQUIRE(started.isOk());
    const int port = started.value();
    const std::string callBody = R"({"jsonrpc":"2.0","id":9,"method":"tools/call",)"
                                 R"("params":{"name":"devdesk_slow_probe","arguments":{}}})";

    // 在途调用与 stop 并发：stop 必须等工具跑完（≈300ms）再返回，而非中断它
    bool callOk = false;
    std::thread caller([&] {
        Response resp = postJson(port, callBody);
        callOk = resp.statusCode == 200 &&
                 resp.body.find("slow-done") != std::string::npos;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 让请求先进入 execute
    const auto t0 = std::chrono::steady_clock::now();
    svc.stop();
    const auto stoppedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    caller.join();
    DOCTEST_CHECK(callOk);
    // stop 在在途调用期间发起 → 至少等到工具剩余时长（防「假 stop 硬杀」回归）
    DOCTEST_CHECK(stoppedMs.count() >= 150);
    DOCTEST_CHECK(!svc.isRunning());
    return true;
})
