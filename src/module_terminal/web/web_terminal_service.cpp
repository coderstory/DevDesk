#include "web_terminal_service.h"

#include "platform/http_server.h"
#include "platform/json.h"
#include "platform/net_interfaces.h"
#include "platform/web_crypto.h"

#include "core/logging.h"

#include "web_routes.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

namespace module::terminal::web {

namespace {

namespace wc = platform::web_crypto;
using platform::json::JsonObject;
using platform::json::JsonValue;

std::int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string stringifyOr(const JsonValue& value, const char* fallback) {
    auto text = platform::json::stringify(value);
    return text.isOk() ? text.value() : std::string(fallback);
}

std::string makeInner(int status, const JsonValue& body) {
    JsonObject obj;
    obj.set("s", JsonValue(status));
    if (!body.isNull()) obj.set("b", body);
    return stringifyOr(JsonValue(obj), "{\"s\":500}");
}

std::string makeInnerError(int status, const std::string& code) {
    JsonObject obj;
    obj.set("s", JsonValue(status));
    obj.set("e", JsonValue(code));
    return stringifyOr(JsonValue(obj), "{\"s\":500}");
}

std::string queryParam(const std::string& query, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t end = query.find('&', pos);
        const std::string item = query.substr(pos, end == std::string::npos ? std::string::npos
                                                                           : end - pos);
        if (item.rfind(needle, 0) == 0) return item.substr(needle.size());
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

bool isLoopbackClient(const std::string& address) {
    if (address == "::1" || address == "[::1]" || address == "::ffff:127.0.0.1") return true;
    return platform::isLoopbackAddress(address);
}

bool canEnterDirectory(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    std::filesystem::directory_iterator it(dir, ec);
    return !ec;
}

} // namespace

WebTerminalService::WebTerminalService()
    : devConfig_(readDevEntryConfig()) {
    cookieKey_ = wc::randomBytes(32);
    if (devConfig_.autostart) {
        if (devConfig_.port > 0) port_ = devConfig_.port;
        // Debug 测试入口：构造即开服；失败保留 error_ 供 snapshot 读取
        (void)start();
    }
}

WebTerminalService::~WebTerminalService() { (void)stop(); }

core::Result<void> WebTerminalService::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return core::Result<void>::ok();
    server_ = std::make_unique<platform::http::Server>(bindAddress_, port_);
    std::vector<std::string> hosts = {"127.0.0.1", "localhost", "[::1]", "::1"};
    for (const auto& addr : platform::localIpv4Addresses()) hosts.push_back(addr);
    server_->setAllowedHosts(hosts);
    registerWebRoutes(*server_, *this);
    if (!server_->start()) {
        bindState_ = core::WebBindState::Error;
        error_ = "端口被占用或无法监听";
        server_.reset();
        LOG_ERROR("网页终端服务启动失败: " + error_);
        return core::Result<void>::fail(core::ErrorCode::HttpError, error_);
    }
    port_ = server_->actualPort();
    running_ = true;
    bindState_ = core::WebBindState::Listening;
    bindTime_ = nowSec();
    error_.clear();
    if (cookieKey_.empty()) cookieKey_ = wc::randomBytes(32);
    if (!hasBindToken_) {
        auto token = makeBindToken(nowSec());
        if (token.isOk()) {
            bindToken_ = token.value();
            hasBindToken_ = true;
        }
    }
    return core::Result<void>::ok();
}

core::Result<void> WebTerminalService::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_) {
        server_->stop();
        server_.reset();
    }
    running_ = false;
    bindState_ = core::WebBindState::Stopped;
    sessions_.clear();
    return core::Result<void>::ok();
}

core::Result<void> WebTerminalService::refreshToken() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto token = makeBindToken(nowSec());
    if (!token.isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "token rng failed");
    }
    bindToken_ = token.value();
    hasBindToken_ = true;
    totpSecret_.clear();
    return core::Result<void>::ok();
}

core::Result<void> WebTerminalService::revokeBinding() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tickets_.revokeAll();
        auto token = makeBindToken(nowSec());
        if (token.isOk()) {
            bindToken_ = token.value();
            hasBindToken_ = true;
        }
        totpSecret_.clear();
    }
    bridge_.publish("revoke", "{}", /*control=*/true);
    return core::Result<void>::ok();
}

core::Result<void> WebTerminalService::writeSettings(const core::TerminalSettingsPatch& patch) {
    if (patch.startupDir && !patch.startupDir->empty() &&
        !canEnterDirectory(*patch.startupDir)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError, "bad_startup_dir");
    }
    SettingsState before;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        before = settings_;
        if (patch.fontFamily) settings_.fontFamily = *patch.fontFamily;
        if (patch.fontSize) settings_.fontSize = *patch.fontSize;
        if (patch.colorMode) settings_.colorMode = *patch.colorMode;
        if (patch.palette) settings_.palette = *patch.palette;
        if (patch.shell) settings_.shell = *patch.shell;
        if (patch.startupDir) settings_.startupDir = *patch.startupDir;
    }
    if (settingsPersist_) {
        auto persisted = settingsPersist_(patch);
        if (!persisted.isOk()) {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_ = before;  // 落库失败回滚内存值（单一真源不漂移）
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed, "persist failed");
        }
    }
    std::string json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        json = settingsJsonLocked();
    }
    bridge_.publish("settings", json);
    return core::Result<void>::ok();
}

core::WebAccessSnapshot WebTerminalService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshotLocked();
}

core::WebAccessSnapshot WebTerminalService::snapshotLocked() const {
    core::WebAccessSnapshot snap;
    snap.running = running_;
    snap.port = running_ && server_ ? server_->actualPort() : 0;
    snap.addresses = platform::localIpv4Addresses();
    snap.bindState = bindState_;
    snap.bindTime = bindTime_;
    snap.lastLogin = lastLogin_;
    snap.onlineCount = static_cast<int>(sessions_.size());
    if (hasBindToken_) {
        const auto remaining = bindToken_.expireAt - nowSec();
        snap.tokenRemainingSec = remaining > 0 ? static_cast<int>(remaining) : 0;
    }
    snap.error = error_;
    return snap;
}

std::string WebTerminalService::settingsJsonLocked() const {
    JsonObject obj;
    obj.set("fontFamily", JsonValue(settings_.fontFamily));
    obj.set("fontSize", JsonValue(settings_.fontSize));
    obj.set("colorMode", JsonValue(settings_.colorMode));
    obj.set("palette", JsonValue(settings_.palette));
    obj.set("shell", JsonValue(settings_.shell));
    obj.set("startupDir", JsonValue(settings_.startupDir));
    return stringifyOr(JsonValue(obj), "{}");
}

void WebTerminalService::replyJson(platform::http::ResponseWriter& rw, int status,
                                   const std::string& json) const {
    rw.setStatus(status);
    rw.setHeader("Content-Type", "application/json; charset=utf-8");
    rw.write(json);
}

void WebTerminalService::replyError(platform::http::ResponseWriter& rw, int status,
                                    const std::string& code) const {
    JsonObject obj;
    obj.set("e", JsonValue(code));
    replyJson(rw, status, stringifyOr(JsonValue(obj), "{\"e\":\"bad\"}"));
}

std::string WebTerminalService::clientAddressFor(const platform::http::RequestCtx& ctx) const {
    if (devConfig_.testHooks) {
        const auto it = ctx.headers.find("x-devdesk-test-addr");
        if (it != ctx.headers.end() && !it->second.empty()) return it->second;
    }
    return ctx.clientAddress;
}

bool WebTerminalService::isAllowedHost(const std::string& host) const {
    std::string key = host;
    while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    if (!key.empty() && key.front() == '[') {
        const auto end = key.find(']');
        if (end != std::string::npos) key = key.substr(0, end + 1);
    } else {
        const auto colon = key.find(':');
        if (colon != std::string::npos) key = key.substr(0, colon);
    }
    for (auto& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (key == "127.0.0.1" || key == "localhost" || key == "::1" || key == "[::1]") return true;
    for (const auto& addr : platform::localIpv4Addresses()) {
        if (addr == key) return true;
    }
    return false;
}

bool WebTerminalService::isAllowedOrigin(const std::string& origin) const {
    std::string value = origin;
    const auto scheme = value.find("://");
    if (scheme == std::string::npos) return false;
    if (value.compare(0, 7, "http://") != 0) return false;  // HTTP 明文传输，HTTPS 来源不可能合法
    return isAllowedHost(value.substr(7));
}

void WebTerminalService::handleHandshakeGet(const platform::http::RequestCtx& ctx,
                                            platform::http::ResponseWriter& rw) {
    const auto address = clientAddressFor(ctx);
    const auto now = nowSec();
    if (!handshakeLimiter_.tryAcquire(address, now)) {
        logAuthFailure(address, "handshake_rate_limited", now);
        replyError(rw, 429, "too_many_requests");
        return;
    }
    auto started = sessions_.beginHandshake(now);
    if (!started.isOk()) {
        replyError(rw, 409, "too_many_sessions");
        return;
    }
    const auto& session = started.value();
    JsonObject obj;
    obj.set("hid", JsonValue(session->hid));
    obj.set("spk", JsonValue(wc::base64UrlEncode(session->serverPublic)));
    obj.set("salt", JsonValue(wc::base64UrlEncode(session->salt)));
    obj.set("alg", JsonValue("x25519-hkdf-sha256-chacha20poly1305"));
    obj.set("v", JsonValue(1));
    replyJson(rw, 200, stringifyOr(JsonValue(obj), "{}"));
}

void WebTerminalService::handleHandshakePost(const platform::http::RequestCtx& ctx,
                                             platform::http::ResponseWriter& rw) {
    auto parsed = platform::json::parse(ctx.body);
    if (!parsed.isOk() || !parsed.value().isObject()) {
        replyError(rw, 400, "bad");
        return;
    }
    const auto obj = parsed.value().asObject();
    const auto hidValue = obj.get("hid");
    const auto cpkValue = obj.get("cpk");
    if (!hidValue.isString() || !cpkValue.isString()) {
        replyError(rw, 400, "bad");
        return;
    }
    auto session = sessions_.find(hidValue.asString());
    if (!session) {
        replyError(rw, 400, "bad");
        return;
    }
    auto clientPublic = wc::base64UrlDecode(cpkValue.asString());
    if (!clientPublic.isOk() || !session->completeHandshake(clientPublic.value()).isOk()) {
        replyError(rw, 400, "bad");
        return;
    }
    JsonObject ok;
    ok.set("ok", JsonValue(true));
    replyJson(rw, 200, stringifyOr(JsonValue(ok), "{}"));
}

std::shared_ptr<CryptoSession> WebTerminalService::decryptRequest(
    const platform::http::RequestCtx& ctx, std::string& plaintext) {
    const auto hidIt = ctx.headers.find("x-devdesk-session");
    if (hidIt == ctx.headers.end() || hidIt->second.empty()) {
        return nullptr;
    }
    auto session = sessions_.find(hidIt->second);
    if (!session || !session->ready) return nullptr;
    std::vector<std::uint8_t> envelope(ctx.body.begin(), ctx.body.end());
    auto opened = session->openClientEnvelope(envelope);
    if (!opened.isOk()) {
        // 兼容 base64url 承载的信封（前端 SSE 侧同一编码）
        auto decoded = wc::base64UrlDecode(ctx.body);
        if (decoded.isOk()) opened = session->openClientEnvelope(decoded.value());
    }
    if (!opened.isOk()) return nullptr;
    session->lastSeen = nowSec();
    plaintext.assign(opened.value().begin(), opened.value().end());
    return session;
}

void WebTerminalService::replySealed(platform::http::ResponseWriter& rw,
                                     const std::shared_ptr<CryptoSession>& session,
                                     const std::string& plaintext, int httpStatus) const {
    std::vector<std::uint8_t> bytes(plaintext.begin(), plaintext.end());
    auto sealed = session->sealServerEnvelope(bytes);
    if (!sealed.isOk()) {
        replyError(rw, 400, "bad");
        return;
    }
    rw.setStatus(httpStatus);
    rw.setHeader("Content-Type", "application/octet-stream");
    rw.setHeader("X-DevDesk-Envelope", "v1");
    rw.write(std::string(sealed.value().begin(), sealed.value().end()));
}

void WebTerminalService::handleEnvelopeImpl(const platform::http::RequestCtx& ctx,
                                            platform::http::ResponseWriter& rw,
                                            const std::string& hint) {
    std::string plaintext;
    auto session = decryptRequest(ctx, plaintext);
    if (!session) {
        replyError(rw, 400, "bad");  // 不可区分错误：错 hid / 篡改 / 无握手同码
        return;
    }
    const auto address = clientAddressFor(ctx);
    int httpStatus = 200;
    const auto response =
        dispatchInner(session, plaintext, isLoopbackClient(address), address, hint, httpStatus);
    replySealed(rw, session, response, httpStatus);
}

void WebTerminalService::handleEnvelope(const platform::http::RequestCtx& ctx,
                                        platform::http::ResponseWriter& rw) {
    handleEnvelopeImpl(ctx, rw, {});
}

void WebTerminalService::handleInput(const platform::http::RequestCtx& ctx,
                                     platform::http::ResponseWriter& rw) {
    handleEnvelopeImpl(ctx, rw, "input");
}

void WebTerminalService::handleSettings(const platform::http::RequestCtx& ctx,
                                        platform::http::ResponseWriter& rw) {
    handleEnvelopeImpl(ctx, rw, "settings.set");
}

std::string WebTerminalService::dispatchInner(const std::shared_ptr<CryptoSession>& session,
                                              const std::string& plaintext, bool loopback,
                                              const std::string& clientAddress,
                                              const std::string& hint, int& httpStatus) {
    auto parsed = platform::json::parse(plaintext);
    if (!parsed.isOk() || !parsed.value().isObject()) {
        return makeInnerError(400, "bad");
    }
    const auto obj = parsed.value().asObject();
    std::string message = hint;
    if (message.empty()) {
        const auto mValue = obj.get("m");
        if (mValue.isString()) message = mValue.asString();
    }
    const auto bodyValue = obj.get("b");
    const JsonObject body = bodyValue.isObject() ? bodyValue.asObject() : JsonObject{};
    const auto now = nowSec();

    auto requireTicket = [&]() {
        if (loopback) return true;  // 回环免验只免票据/TOTP，不免握手与加密
        const auto tokenValue = body.get("t");
        if (!tokenValue.isString() || tokenValue.asString().empty()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto ticket = tickets_.validate(tokenValue.asString(), cookieKey_, now);
        if (!ticket.isOk()) return false;
        session->ticketId = ticket.value().idHex;
        return true;
    };

    if (message == "info") {
        JsonObject out;
        out.set("v", JsonValue(1));
        out.set("alg", JsonValue("x25519-hkdf-sha256-chacha20poly1305"));
        return makeInner(200, JsonValue(out));
    }
    if (message == "state") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        JsonObject out;
        out.set("online", JsonValue(static_cast<int>(sessions_.size())));
        std::lock_guard<std::mutex> lock(mutex_);
        out.set("port", JsonValue(port_));
        JsonObject st;
        st.set("fontFamily", JsonValue(settings_.fontFamily));
        st.set("fontSize", JsonValue(settings_.fontSize));
        st.set("colorMode", JsonValue(settings_.colorMode));
        st.set("palette", JsonValue(settings_.palette));
        st.set("shell", JsonValue(settings_.shell));
        st.set("startupDir", JsonValue(settings_.startupDir));
        out.set("settings", JsonValue(st));
        return makeInner(200, JsonValue(out));
    }
    if (message == "stream.ticket") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        auto ticket = sessions_.issueStreamTicket(session->hid, now);
        if (!ticket.isOk()) return makeInnerError(409, "too_many_tickets");
        JsonObject out;
        out.set("t", JsonValue(ticket.value()));
        return makeInner(200, JsonValue(out));
    }
    if (message == "input" || message == "paste") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeSessions_ <= 0) return makeInnerError(409, "no_active_session");
        }
        // 命令队列满 = HTTP 503（技术卡 §3.1），不是密文内错误
        if (!enqueueCommand(plaintext)) {
            httpStatus = 503;
            return makeInnerError(503, "queue_full");
        }
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "resync") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        bridge_.publish("full", "{}");
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "settings.get") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        std::lock_guard<std::mutex> lock(mutex_);
        JsonObject st;
        st.set("fontFamily", JsonValue(settings_.fontFamily));
        st.set("fontSize", JsonValue(settings_.fontSize));
        st.set("colorMode", JsonValue(settings_.colorMode));
        st.set("palette", JsonValue(settings_.palette));
        st.set("shell", JsonValue(settings_.shell));
        st.set("startupDir", JsonValue(settings_.startupDir));
        return makeInner(200, JsonValue(st));
    }
    if (message == "settings.set") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        core::TerminalSettingsPatch patch;
        if (body.has("fontFamily")) patch.fontFamily = body.get("fontFamily").asString();
        if (body.has("fontSize")) patch.fontSize = static_cast<int>(body.get("fontSize").asInt());
        if (body.has("colorMode")) patch.colorMode = body.get("colorMode").asString();
        if (body.has("palette")) patch.palette = body.get("palette").asString();
        if (body.has("shell")) patch.shell = body.get("shell").asString();
        if (body.has("startupDir")) patch.startupDir = body.get("startupDir").asString();
        auto written = writeSettings(patch);
        if (!written.isOk()) {
            return makeInnerError(written.errorCode() == core::ErrorCode::ValidationError ? 400 : 500,
                                  "settings_failed");
        }
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "auth.login") {
        if (!loopback) {
            const auto codeValue = body.get("code");
            if (!codeValue.isString()) return makeInnerError(400, "bad_code");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (loginLockout_.isLocked(clientAddress, now)) return makeInnerError(429, "locked");
            }
            if (totpSecret_.empty() || !verifyTotp(totpSecret_, codeValue.asString(), now)) {
                std::lock_guard<std::mutex> lock(mutex_);
                loginLockout_.recordFailure(clientAddress, now);
                return makeInnerError(401, "bad_code");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            loginLockout_.recordSuccess(clientAddress);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto issued = tickets_.issue(cookieKey_, now);
        if (!issued.isOk()) {
            if (issued.errorCode() == core::ErrorCode::Conflict) {
                return makeInnerError(409, "too_many_tickets");
            }
            return makeInnerError(500, "ticket_failed");
        }
        lastLogin_ = now;
        session->ticketId = issued.value().ticket.idHex;
        JsonObject out;
        out.set("t", JsonValue(issued.value().token));
        return makeInner(200, JsonValue(out));
    }
    if (message == "auth.logout") {
        if (loopback) return makeInner(200, JsonValue(nullptr));
        const auto tokenValue = body.get("t");
        if (!tokenValue.isString()) return makeInnerError(400, "bad_ticket");
        std::lock_guard<std::mutex> lock(mutex_);
        auto ticket = tickets_.validate(tokenValue.asString(), cookieKey_, now);
        if (!ticket.isOk()) return makeInnerError(401, "bad_ticket");
        tickets_.revoke(ticket.value().idHex);
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "bind.claim") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasBindToken_) return makeInnerError(409, "no_token");
        auto claim = claimBindToken(bindToken_, now);
        if (!claim.isOk()) return makeInnerError(409, "bad_token");
        totpSecret_ = bindToken_.secret;
        JsonObject out;
        out.set("secret", JsonValue(claim.value().secret));
        out.set("otpauth", JsonValue(claim.value().otpauth));
        return makeInner(200, JsonValue(out));
    }
    if (message == "bind.confirm") {
        const auto codeValue = body.get("code");
        if (!codeValue.isString()) return makeInnerError(400, "bad_code");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasBindToken_) return makeInnerError(409, "no_token");
        const auto outcome = confirmBindToken(bindToken_, codeValue.asString(), now);
        if (outcome == ConfirmOutcome::NotClaimed) return makeInnerError(409, "bad_token");
        if (outcome == ConfirmOutcome::InvalidCode) return makeInnerError(400, "bad_code");
        totpSecret_ = bindToken_.secret;
        auto issued = tickets_.issue(cookieKey_, now);
        if (!issued.isOk()) return makeInnerError(409, "too_many_tickets");
        lastLogin_ = now;
        session->ticketId = issued.value().ticket.idHex;
        JsonObject out;
        out.set("t", JsonValue(issued.value().token));
        return makeInner(200, JsonValue(out));
    }
    if (message == "bind.revoke") {  // 桌面/网页撤销验证器绑定
        tickets_.revokeAll();
        auto token = makeBindToken(now);
        if (token.isOk()) {
            bindToken_ = token.value();
            hasBindToken_ = true;
        }
        totpSecret_.clear();
        return makeInner(200, JsonValue(nullptr));
    }
    return makeInnerError(400, "unknown_message");
}

void WebTerminalService::handleStream(const platform::http::RequestCtx& ctx,
                                      platform::http::ResponseWriter& rw) {
    std::string ticket = queryParam(ctx.query, "t");
    if (ticket.empty()) ticket = queryParam(ctx.query, "ticket");
    const auto now = nowSec();
    auto hid = sessions_.consumeStreamTicket(ticket, now);
    if (!hid.isOk()) {
        replyError(rw, 401, "bad_ticket");
        return;
    }
    auto session = sessions_.find(hid.value());
    if (!session) {
        replyError(rw, 401, "bad_ticket");
        return;
    }
    if (bridge_.subscriberCount() >= kMaxSubscribers) {
        sessions_.finishStreamTicket(ticket, now);
        replyError(rw, 503, "too_many_streams");
        return;
    }
    auto sub = bridge_.subscribe([session](const std::string& text) -> std::string {
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        auto sealed = session->sealServerEnvelope(bytes);
        if (!sealed.isOk()) return {};
        return wc::base64UrlEncode(sealed.value());
    });
    if (!sub) {
        sessions_.finishStreamTicket(ticket, now);
        replyError(rw, 503, "too_many_streams");
        return;
    }
    bridge_.publish("full", "{}");
    rw.setStatus(200);
    rw.setHeader("Content-Type", "text/event-stream");
    rw.setHeader("Cache-Control", "no-store");
    rw.setHeader("Connection", "keep-alive");
    rw.setChunkProvider(
        "text/event-stream",
        [this, sub](platform::http::ResponseWriter::ChunkWriter writeChunk,
                    platform::http::ResponseWriter::IsWritableFn isWritable) -> bool {
            return bridge_.runStream(
                sub,
                [&writeChunk](const char* data, std::size_t len) {
                    return writeChunk(data, len);
                },
                [&isWritable]() { return isWritable(); });
        },
        [this, sub, ticket](bool) {
            bridge_.unsubscribe(sub);
            sessions_.finishStreamTicket(ticket, nowSec());
        });
}

void WebTerminalService::handleStatus(const platform::http::RequestCtx&,
                                      platform::http::ResponseWriter& rw) {
    auto snap = snapshot();
    // 状态接口只回非秘密字段；hid/密钥/票据绝不出现在明文
    JsonObject obj;
    obj.set("running", JsonValue(snap.running));
    obj.set("port", JsonValue(snap.port));
    obj.set("online", JsonValue(snap.onlineCount));
    obj.set("bindState", JsonValue(static_cast<int>(snap.bindState)));
    replyJson(rw, 200, stringifyOr(JsonValue(obj), "{}"));
}

bool WebTerminalService::enqueueCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commands_.size() >= 256) return false;  // 有界队列，满 → HTTP 503
    commands_.push_back(command);
    return true;
}

void WebTerminalService::drainCommands() {
    std::deque<std::string> pending;
    std::function<void(const std::string&)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(commands_);
        handler = commandHandler_;
    }
    if (!handler) return;
    for (const auto& command : pending) handler(command);
}

void WebTerminalService::setCommandHandler(std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    commandHandler_ = std::move(handler);
}

void WebTerminalService::setSettingsPersist(
    std::function<core::Result<void>(const core::TerminalSettingsPatch&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    settingsPersist_ = std::move(handler);
}

void WebTerminalService::setActiveSessionCount(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSessions_ = count < 0 ? 0 : count;
}

bool WebTerminalService::handshakeRateAllowedForTest(const std::string& address,
                                                     std::int64_t now) {
    return handshakeLimiter_.tryAcquire(address, now);
}

int WebTerminalService::actualPortForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_ ? server_->actualPort() : 0;
}

std::size_t WebTerminalService::pendingCommandCountForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commands_.size();
}

std::vector<std::string> WebTerminalService::takeCommandsForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out(commands_.begin(), commands_.end());
    commands_.clear();
    return out;
}

void WebTerminalService::setSessionActiveForTest(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSessions_ = active ? 1 : 0;
}

} // namespace module::terminal::web
