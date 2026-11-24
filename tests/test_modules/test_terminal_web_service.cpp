// 网页终端服务级用例（need#490 W3）：握手/信封往返、篡改、重放、错 hid、会话与票据上限、
// SSE seq 与队列合并、per-client 限流、Host/Origin 拒绝、debug 启动入口。
#include "doctest.h"

#include "platform/json.h"
#include "platform/web_crypto.h"

#include "module_terminal/web/web_bridge.h"
#include "module_terminal/web/web_crypto_session.h"
#include "module_terminal/web/web_dev_entry.h"
#include "module_terminal/web/web_session_bridge.h"
#include "module_terminal/web/web_terminal_service.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/terminal_settings.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

void clearEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

namespace wc = platform::web_crypto;
using namespace module::terminal::web;

std::int64_t testNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 测试侧按服务端同一公式推导方向密钥（前端 JS 走同一 info 串）
std::vector<std::uint8_t> deriveClientKey(const char* direction,
                                          const std::string& hid,
                                          const std::vector<std::uint8_t>& csk,
                                          const std::vector<std::uint8_t>& cpk,
                                          const std::vector<std::uint8_t>& spk,
                                          const std::vector<std::uint8_t>& salt) {
    auto shared = wc::x25519SharedSecret(csk, spk);
    auto prk = wc::hkdfExtract(salt, shared.value());
    const std::string info = std::string("ddweb/v1/") + direction + "|" + hid + "|" +
                             wc::base64UrlEncode(cpk) + "|" + wc::base64UrlEncode(spk);
    return wc::hkdfExpand(prk.value(), info, wc::kKeySize).value();
}

struct Handshaken {
    std::shared_ptr<CryptoSession> session;
    std::vector<std::uint8_t> c2s;
    std::vector<std::uint8_t> s2c;
};

Handshaken doHandshake(CryptoSessionTable& table) {
    Handshaken h;
    auto started = table.beginHandshake(testNow());
    h.session = started.value();
    auto client = wc::x25519Generate().value();
    const auto cpk = client.publicKey;
    const auto spk = h.session->serverPublic;
    if (!h.session->completeHandshake(cpk).isOk()) std::abort();
    h.c2s = deriveClientKey("c2s", h.session->hid, client.privateKey, cpk, spk, h.session->salt);
    h.s2c = deriveClientKey("s2c", h.session->hid, client.privateKey, cpk, spk, h.session->salt);
    return h;
}

} // namespace

REGISTER_TEST(web_crypto_handshake_roundtrip, "web: 握手后双向往返 + 信封固定开销 29B", {
    CryptoSessionTable table;
    auto h = doHandshake(table);
    const std::string text = "hello";
    std::vector<std::uint8_t> plain(text.begin(), text.end());
    auto envelope = wc::sealEnvelope(h.c2s, wc::makeNonce(wc::kDirectionC2S, 0), plain).value();
    DOCTEST_CHECK_EQ(envelope.size(), plain.size() + wc::kEnvelopeOverhead);
    auto opened = h.session->openClientEnvelope(envelope);
    DOCTEST_CHECK(opened.isOk());
    DOCTEST_CHECK_EQ(std::string(opened.value().begin(), opened.value().end()), text);

    auto reply = h.session->sealServerEnvelope(plain).value();
    auto decoded = wc::openEnvelope(h.s2c, reply).value();
    DOCTEST_CHECK_EQ(std::string(decoded.begin(), decoded.end()), text);
    return true;
})

REGISTER_TEST(web_crypto_tamper_rejected, "web: 篡改信封/错 hid 一律拒绝且不可区分", {
    CryptoSessionTable table;
    auto h = doHandshake(table);
    std::vector<std::uint8_t> plain{'o', 'k'};
    auto envelope = wc::sealEnvelope(h.c2s, wc::makeNonce(wc::kDirectionC2S, 0), plain).value();
    envelope.back() ^= 0xFF;
    auto opened = h.session->openClientEnvelope(envelope);
    DOCTEST_CHECK(opened.isFail());
    DOCTEST_CHECK(opened.errorCode() == core::ErrorCode::ValidationError);
    DOCTEST_CHECK(table.find("unknown-hid") == nullptr);
    return true;
})

REGISTER_TEST(web_crypto_replay_rejected, "web: 同一 nonce 第二帧被重放窗口拒绝", {
    CryptoSessionTable table;
    auto h = doHandshake(table);
    std::vector<std::uint8_t> plain{'x'};
    auto nonce = wc::makeNonce(wc::kDirectionC2S, 0);
    auto first = wc::sealEnvelope(h.c2s, nonce, plain).value();
    auto second = wc::sealEnvelope(h.c2s, nonce, plain).value();
    DOCTEST_CHECK(h.session->openClientEnvelope(first).isOk());
    DOCTEST_CHECK(h.session->openClientEnvelope(second).isFail());
    return true;
})

REGISTER_TEST(web_session_cap_64, "web: 会话表 64 上限，超限返回 Conflict 且不动已有会话", {
    CryptoSessionTable table;
    for (std::size_t i = 0; i < kMaxSessions; ++i) {
        DOCTEST_CHECK(table.beginHandshake(testNow()).isOk());
    }
    auto overflow = table.beginHandshake(testNow());
    DOCTEST_CHECK(overflow.isFail());
    DOCTEST_CHECK(overflow.errorCode() == core::ErrorCode::Conflict);
    DOCTEST_CHECK_EQ(table.size(), kMaxSessions);
    return true;
})

REGISTER_TEST(web_stream_ticket_single_use, "web: 流票据 unused→in_use→consumed，重复使用拒绝", {
    CryptoSessionTable table;
    auto h = doHandshake(table);
    const auto now = testNow();
    auto ticket = table.issueStreamTicket(h.session->hid, now).value();
    auto first = table.consumeStreamTicket(ticket, now);
    DOCTEST_CHECK(first.isOk());
    DOCTEST_CHECK_EQ(first.value(), h.session->hid);
    DOCTEST_CHECK(table.consumeStreamTicket(ticket, now).isFail());
    table.finishStreamTicket(ticket, now);
    DOCTEST_CHECK(table.consumeStreamTicket(ticket, now).isFail());
    return true;
})

REGISTER_TEST(web_bridge_seq_and_control, "web: seq 单调；数据队列满丢最旧，控制事件不丢", {
    WebBridge bridge;
    auto sub = bridge.subscribe([](const std::string& text) { return text; });
    DOCTEST_CHECK(sub != nullptr);
    for (int i = 0; i < 10; ++i) bridge.publish("rows", "{}");
    std::uint64_t seqAfterData = 0;
    {
        std::lock_guard<std::mutex> lock(sub->mutex);
        DOCTEST_CHECK_EQ(sub->data.size(), kMaxQueuedFrames);
        DOCTEST_CHECK(sub->control.empty());
    }
    seqAfterData = bridge.lastSeq();
    DOCTEST_CHECK_EQ(seqAfterData, 10u);
    bridge.publish("revoke", "{}", /*control=*/true);
    {
        std::lock_guard<std::mutex> lock(sub->mutex);
        DOCTEST_CHECK_EQ(sub->control.size(), 1u);
        DOCTEST_CHECK_EQ(sub->data.size(), kMaxQueuedFrames);
    }
    DOCTEST_CHECK_EQ(bridge.lastSeq(), 11u);
    bridge.unsubscribe(sub);
    DOCTEST_CHECK_EQ(bridge.subscriberCount(), 0u);
    return true;
})

REGISTER_TEST(web_bridge_subscriber_cap, "web: SSE 订阅者 64 上限，第 65 个返回空", {
    WebBridge bridge;
    std::vector<std::shared_ptr<StreamSubscriber>> subs;
    for (std::size_t i = 0; i < kMaxSubscribers; ++i) {
        auto sub = bridge.subscribe([](const std::string& text) { return text; });
        DOCTEST_CHECK(sub != nullptr);
        subs.push_back(sub);
    }
    DOCTEST_CHECK(bridge.subscribe([](const std::string& text) { return text; }) == nullptr);
    for (auto& sub : subs) bridge.unsubscribe(sub);
    return true;
})

REGISTER_TEST(web_bridge_write_failure_ends, "web: 写失败立即结束流并标记死亡", {
    WebBridge bridge;
    auto sub = bridge.subscribe([](const std::string& text) { return text; });
    bridge.publish("full", "{}");
    bool ok = bridge.runStream(
        sub, [](const char*, std::size_t) { return false; }, []() { return true; });
    DOCTEST_CHECK_FALSE(ok);
    std::lock_guard<std::mutex> lock(sub->mutex);
    DOCTEST_CHECK(sub->dead);
    return true;
})

REGISTER_TEST(web_rate_limit_per_client, "web: 握手限流按客户端地址，A 打满不影响 B", {
    WebTerminalService service;
    const auto now = testNow();
    for (int i = 0; i < kHandshakeClientLimit; ++i) {
        DOCTEST_CHECK(service.handshakeRateAllowedForTest("10.0.0.1", now));
    }
    DOCTEST_CHECK_FALSE(service.handshakeRateAllowedForTest("10.0.0.1", now));
    DOCTEST_CHECK(service.handshakeRateAllowedForTest("10.0.0.2", now));
    return true;
})

REGISTER_TEST(web_origin_host_rejected, "web: 伪造 Host/Origin 被判定拒绝", {
    WebTerminalService service;
    DOCTEST_CHECK(service.isAllowedHost("127.0.0.1:7878"));
    DOCTEST_CHECK(service.isAllowedHost("localhost"));
    DOCTEST_CHECK_FALSE(service.isAllowedHost("evil.example.com:7878"));
    DOCTEST_CHECK(service.isAllowedOrigin("http://127.0.0.1:7878"));
    DOCTEST_CHECK_FALSE(service.isAllowedOrigin("https://127.0.0.1:7878"));
    DOCTEST_CHECK_FALSE(service.isAllowedOrigin("null"));
    return true;
})

REGISTER_TEST(web_settings_startup_dir_validation, "web: 设置唯一入口校验启动目录可进入", {
    WebTerminalService service;
    core::TerminalSettingsPatch bad;
    bad.startupDir = std::string("/definitely/not/a/dir/need490");
    auto rejected = service.writeSettings(bad);
    DOCTEST_CHECK(rejected.isFail());
    DOCTEST_CHECK(rejected.errorCode() == core::ErrorCode::ValidationError);

    core::TerminalSettingsPatch good;
    good.startupDir = std::filesystem::temp_directory_path().string();
    DOCTEST_CHECK(service.writeSettings(good).isOk());
    return true;
})

REGISTER_TEST(web_status_plaintext_fields, "web: /api/status 回绑定态/设备/时间且无秘密", {
    WebTerminalService service;
    auto parsed = platform::json::parse(service.statusJsonForTest());
    DOCTEST_CHECK(parsed.isOk());
    const auto obj = parsed.value().asObject();
    DOCTEST_CHECK(obj.get("bound").isBool());
    DOCTEST_CHECK_FALSE(obj.get("bound").asBool());
    // P2：未鉴权 /api/status 不再暴露在线数/绑定时间/设备名
    DOCTEST_CHECK_FALSE(obj.has("deviceName"));
    DOCTEST_CHECK_FALSE(obj.has("bindTime"));
    DOCTEST_CHECK_FALSE(obj.has("lastLogin"));
    DOCTEST_CHECK_FALSE(obj.has("online"));
    DOCTEST_CHECK_FALSE(obj.has("hid"));
    DOCTEST_CHECK_FALSE(obj.has("token"));
    return true;
})

REGISTER_TEST(web_settings_error_tristate, "web: 启动目录三态 not_exist/not_dir/cannot_enter", {
    DOCTEST_CHECK_EQ(WebTerminalService::settingsErrorReasonForTest(
                         "/definitely/not/here/need490"),
                     std::string("not_exist"));
    const auto file = std::filesystem::temp_directory_path() / "need490_not_dir_probe";
    {
        std::ofstream out(file);
        out << "x";
    }
    DOCTEST_CHECK_EQ(WebTerminalService::settingsErrorReasonForTest(file.string()),
                     std::string("not_dir"));
    std::filesystem::remove(file);
    DOCTEST_CHECK_EQ(WebTerminalService::settingsErrorReasonForTest(
                         std::filesystem::temp_directory_path().string()),
                     std::string());
    return true;
})

REGISTER_TEST(web_handshake_retry_after, "web: 握手限流暴露剩余秒（不再固定 60）", {
    WebTerminalService service;
    const auto now = testNow();
    for (int i = 0; i < kHandshakeClientLimit; ++i) {
        DOCTEST_CHECK(service.handshakeRateAllowedForTest("10.9.9.9", now));
    }
    DOCTEST_CHECK_FALSE(service.handshakeRateAllowedForTest("10.9.9.9", now));
    const int retry = service.lastRetryAfterForTest();
    DOCTEST_CHECK(retry >= 1);
    DOCTEST_CHECK(retry <= 60);
    return true;
})

REGISTER_TEST(web_input_command_contract, "web: /api/input 契约 b.text/b.keys/b.bp", {
    WebTerminalService service;
    auto parsed = platform::json::parse(service.makeInputCommandForTest("ab", true));
    DOCTEST_CHECK(parsed.isOk());
    const auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("op").asString(), std::string("input"));
    DOCTEST_CHECK(obj.get("bp").asBool());
    auto decoded = platform::web_crypto::base64UrlDecode(obj.get("data").asString());
    DOCTEST_CHECK(decoded.isOk());
    DOCTEST_CHECK_EQ(std::string(decoded.value().begin(), decoded.value().end()), std::string("ab"));
    return true;
})

REGISTER_TEST(web_tab_snapshot_default, "web: 无会话桥时 tab 快照为空且 activeId=null", {
    WebTerminalService service;
    auto parsed = platform::json::parse(service.tabSnapshotJsonForTest());
    DOCTEST_CHECK(parsed.isOk());
    const auto obj = parsed.value().asObject();
    DOCTEST_CHECK(obj.get("tabs").isArray());
    DOCTEST_CHECK_EQ(obj.get("tabs").size(), 0);
    DOCTEST_CHECK(obj.get("activeId").isNull());
    return true;
})

REGISTER_TEST(web_bind_token_validation, "web: bind.claim 令牌校验区分 no_token/bad_token", {
    WebTerminalService service;
    DOCTEST_CHECK(service.refreshToken().isOk());
    const std::string token = service.bindTokenForTest();
    DOCTEST_CHECK_FALSE(token.empty());
    DOCTEST_CHECK_EQ(service.bindClaimErrorForTest(token, /*loopback=*/false), std::string());
    DOCTEST_CHECK_EQ(service.bindClaimErrorForTest(token, /*loopback=*/true), std::string());
    DOCTEST_CHECK_EQ(service.bindClaimErrorForTest("wrong-token", false), std::string("bad_token"));
    DOCTEST_CHECK_EQ(service.bindClaimErrorForTest("", false), std::string("no_token"));
    DOCTEST_CHECK_EQ(service.bindClaimErrorForTest("", true), std::string());
    return true;
})

#ifndef NDEBUG
REGISTER_TEST(web_dev_entry_available_debug, "web: 开发测试入口仅 Debug 可用", {
    DOCTEST_CHECK(devEntryAvailable());
    return true;
})

REGISTER_TEST(web_dev_entry_autostart, "web: DEVDESK_WEB_AUTOSTART/PORT 构造即开服（debug）", {
    setEnv("DEVDESK_WEB_AUTOSTART", "1");
    setEnv("DEVDESK_WEB_PORT", "47883");
    {
        WebTerminalService service;
        DOCTEST_CHECK(service.snapshot().running);
        DOCTEST_CHECK_EQ(service.actualPortForTest(), 47883);
        (void)service.stop();
        DOCTEST_CHECK_FALSE(service.snapshot().running);
    }
    clearEnv("DEVDESK_WEB_AUTOSTART");
    clearEnv("DEVDESK_WEB_PORT");
    return true;
})
#else
REGISTER_TEST(web_dev_entry_absent_release, "web: 开发测试入口在 Release 编译期不可用", {
    DOCTEST_CHECK_FALSE(devEntryAvailable());
    return true;
})
#endif

// need#490 W6 接线判据：网页输入/resize/tab 命令进服务端队列后，只有主循环每帧
// tick() 才排空；未接会话上下文或未 tick 时命令滞留（防「接上了但静默不工作」）。
REGISTER_TEST(web_terminal_service_tick_drains_commands,
              "web: tick 未调用命令滞留 / 调用后消费（主循环队列执行依赖）", {
    clearEnv("DEVDESK_WEB_AUTOSTART");
    clearEnv("DEVDESK_WEB_PORT");
    WebTerminalService svc;
    terminal::TerminalSettings settings;
    terminal::SessionController controller;
    terminal::web::WebSessionBridge bridge(
        controller, &settings,
        [](const terminal::TerminalSettings&) {
            return core::Result<void>::ok();
        });
    std::vector<std::string> handled;
    svc.setCommandHandler(
        [&handled](const std::string& c) { handled.push_back(c); });

    svc.enqueueCommandForTest("{\"op\":\"new\"}");
    DOCTEST_CHECK_EQ(svc.pendingCommandCountForTest(), std::size_t{1});
    // 未接会话上下文时 tick 是 no-op：命令必须滞留（不能假装消费）
    svc.tick();
    DOCTEST_CHECK_EQ(svc.pendingCommandCountForTest(), std::size_t{1});
    DOCTEST_CHECK(handled.empty());

    // 接上下文后 tick 排空并交给命令处理器
    svc.setSessionContext(&controller, &bridge);
    svc.tick();
    DOCTEST_CHECK_EQ(svc.pendingCommandCountForTest(), std::size_t{0});
    DOCTEST_CHECK_EQ(handled.size(), std::size_t{1});
    return true;
})


REGISTER_TEST(web_qr_matrix_from_token,
              "web: 绑定二维码由令牌 URL 生成（P0-1：快照 qrSize/qrMatrix 非空）", {
    setEnv("DEVDESK_WEB_PORT", "28787");
    WebTerminalService service;
    clearEnv("DEVDESK_WEB_PORT");
    auto started = service.start();
    DOCTEST_REQUIRE(started.isOk());
    auto snap = service.snapshot();
    DOCTEST_CHECK(snap.running);
    DOCTEST_CHECK(snap.tokenRemainingSec > 0);
    DOCTEST_CHECK(snap.qrSize >= 21);
    DOCTEST_CHECK_EQ(snap.qrMatrix.size(),
                     static_cast<std::size_t>(snap.qrSize) *
                         static_cast<std::size_t>(snap.qrSize));
    int dark = 0;
    for (auto m : snap.qrMatrix) dark += m ? 1 : 0;
    DOCTEST_CHECK(dark > 0);
    // 令牌刷新 → 矩阵随之更新（同一主机/端口，只有 token 变了）
    const auto before = snap.qrMatrix;
    DOCTEST_REQUIRE(service.refreshToken().isOk());
    auto after = service.snapshot();
    DOCTEST_CHECK(after.qrSize >= 21);
    DOCTEST_CHECK(after.qrMatrix != before);
    (void)service.stop();
    return true;
});

REGISTER_TEST(web_session_prune_reopens_capacity,
              "web: 64 空闲会话清理后新握手成功（P0-B prune 生效）", {
    WebTerminalService service;
    auto& table = service.sessionsForTest();
    const std::int64_t t = testNow();
    for (int i = 0; i < 64; ++i) {
        DOCTEST_REQUIRE(table.beginHandshake(t).isOk());
    }
    DOCTEST_CHECK_FALSE(table.beginHandshake(t).isOk());  // 满 64 → 拒绝
    table.prune(t + kSessionIdleSec + 1);                 // 全部超 24h 空闲
    DOCTEST_CHECK(table.beginHandshake(t + kSessionIdleSec + 2).isOk());
    return true;
});
