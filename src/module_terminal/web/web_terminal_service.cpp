#include "web_terminal_service.h"

#include "web_grid.h"
#include "web_session_bridge.h"

#include "module_terminal/terminal_session_controller.h"

#include "platform/http_server.h"
#include "platform/json.h"
#include "platform/net_interfaces.h"
#include "platform/web_crypto.h"

#include "core/logging.h"
#include "core/secrets_store.h"

#include "web_routes.h"
#include "qr/qr_encode.h"

#include "core/paths.h"

#include <openssl/crypto.h>

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

// cookie_key 在 app_secrets 中只存 hex 文本（value 列走 protect 后端，非明文文件）。
constexpr const char* kCookieKeyService = "devdesk.web_terminal";
constexpr const char* kCookieKeyAccount = "cookie_key";

std::string hexFromBytes(const std::vector<std::uint8_t>& bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<std::uint8_t> bytesFromHex(const std::string& text) {
    if (text.size() % 2 != 0) return {};
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexNibble(text[i]);
        const int lo = hexNibble(text[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
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

// 按 UTF-8 字符数截断（tab 改名上限 32 字符，不切半字符）。
std::string clampUtf8Chars(const std::string& in, std::size_t maxChars) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < in.size() && chars < maxChars) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = 1;
        if ((c >> 5) == 0x6) len = 2;
        else if ((c >> 4) == 0xE) len = 3;
        else if ((c >> 3) == 0x1E) len = 4;
        if (i + len > in.size()) break;  // 截断点落在多字节中间：丢弃残片
        i += len;
        ++chars;
    }
    return in.substr(0, i);
}

} // namespace

std::string WebTerminalService::settingsErrorReasonForTest(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return "not_exist";
    if (!std::filesystem::is_directory(dir, ec) || ec) return "not_dir";
    if (!canEnterDirectory(dir)) return "cannot_enter";
    return {};
}

WebTerminalService::WebTerminalService()
    : devConfig_(readDevEntryConfig()),
      gridEncoder_(std::make_unique<::terminal::web::WebGridEncoder>()) {
    // 票据落地：重启后浏览器凭本地票据免登录（P0-2 持久化）。损坏文件解析为空表。
    tickets_ = TicketStore::loadFromFile(core::paths::appDataDir() / "web_tickets.dat");
    // cookie_key 不在此生成：必须从 app_secrets 载入/落库（重启后旧票据仍可验签）。
    if (devConfig_.autostart) {
        if (devConfig_.port > 0) port_ = devConfig_.port;
        // Debug 测试入口：构造即开服；失败保留 error_ 供 snapshot 读取
        (void)start();
    }
}

WebTerminalService::~WebTerminalService() {
    (void)stop();
    std::lock_guard<std::mutex> lock(mutex_);
    // 会话密钥/TOTP 密钥即用即清：析构时 memset 不够可靠，走 OpenSSL 显式擦除。
    if (!cookieKey_.empty()) OPENSSL_cleanse(cookieKey_.data(), cookieKey_.size());
    if (!bindToken_.secret.empty())
        OPENSSL_cleanse(bindToken_.secret.data(), bindToken_.secret.size());
    if (!totpSecret_.empty()) OPENSSL_cleanse(totpSecret_.data(), totpSecret_.size());
}

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
    std::unique_ptr<platform::http::Server> server;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 锁序：mutex_ → server 生命周期。先把 server 摘出锁，再在外面 stop/join。
        // 若在 mutex_ 内调 server_->stop()，httplib 会 join 工作线程；在飞 handler
        // 正等 mutex_（如 state 分支）→ 互相等待，关服务/退出死锁。
        server = std::move(server_);
        running_ = false;
        bindState_ = core::WebBindState::Stopping;
        sessions_.clear();
    }
    if (server) server->stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) bindState_ = core::WebBindState::Stopped;  // 并发 start 已重开则不覆盖
    }
    return core::Result<void>::ok();
}

bool WebTerminalService::ensureCookieKeyLocked() {
    if (cookieKey_.size() == wc::kKeySize) return true;
    auto stored = core::secrets_store::get(kCookieKeyService, kCookieKeyAccount);
    if (stored.isOk()) {
        auto decoded = bytesFromHex(stored.value());
        if (decoded.size() != wc::kKeySize) {
            LOG_ERROR("网页终端: app_secrets 中 cookie_key 形态非法（期望 64 hex），拒绝启用票据");
            return false;
        }
        cookieKey_ = std::move(decoded);
        return true;
    }
    if (stored.errorCode() != core::ErrorCode::NotFound) {
        // 库不可用 / 解不开（被篡改）：不生成新密钥顶替——那会让旧票据无告知全失效，
        // 并把真实故障伪装成「首次运行」。
        LOG_ERROR("网页终端: cookie_key 读取失败，拒绝启用票据: " + stored.errorMessage());
        return false;
    }
    auto generated = wc::randomBytes(wc::kKeySize);
    if (generated.size() != wc::kKeySize) {
        LOG_ERROR("网页终端: cookie_key RNG 失败，拒绝启用票据");
        return false;
    }
    auto saved = core::secrets_store::set(kCookieKeyService, kCookieKeyAccount,
                                          hexFromBytes(generated));
    if (saved.isFail()) {
        LOG_ERROR("网页终端: cookie_key 写入 app_secrets 失败，拒绝启用票据: " +
                  saved.errorMessage());
        return false;
    }
    cookieKey_ = std::move(generated);
    return true;
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
        bound_ = false;
        boundAt_ = 0;
        lastLogin_ = 0;
        persistTicketsLocked();
    }
    bridge_.publish("revoke", "{}", /*control=*/true);
    return core::Result<void>::ok();
}

core::Result<void> WebTerminalService::writeSettings(const core::TerminalSettingsPatch& patch) {
    // need#452 契约（设计 §9）：保存不拦截——启动目录是快捷方式不是承诺，不存在/不可进入
    // 照存成功；可用性由启动侧严格校验（spawn 非空不可用 → 失败，不静默回退）。网页输入框
    // 的三态提示校验在 HTTP settings.set 边界（dispatchInner），不属于保存语义。
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
    snap.configuredPort = port_;  // W6 设置页：未运行时显示持久化配置端口
    snap.addresses = platform::localIpv4Addresses();
    snap.bindState = bindState_;
    snap.bindTime = bound_ ? boundAt_ : 0;
    snap.lastLogin = lastLogin_;
    // 在线数只算已登录会话（握手未登录的不占人数口径）。
    snap.onlineCount = static_cast<int>(sessions_.authenticatedCount());
    if (hasBindToken_) {
        const auto remaining = bindToken_.expireAt - nowSec();
        snap.tokenRemainingSec = remaining > 0 ? static_cast<int>(remaining) : 0;
        // P0-1：绑定二维码来自真实令牌 URL（http://<ip>:<port>/#b=<token>）。
        // 未绑定态每帧 snapshot 都会走到这里：按 URL 缓存矩阵，令牌/端口不变不重编。
        if (!bound_ && bindToken_.state != BindTokenState::Terminal &&
            snap.tokenRemainingSec > 0) {
            const std::string url = bindUrlLocked();
            if (!url.empty() && url != qrCacheUrl_) {
                QrMatrix matrix;
                ++qrEncodeCount_;
                if (encode(url, QrEcLevel::L, matrix)) {
                    qrCacheUrl_ = url;
                    qrCacheSize_ = matrix.size;
                    qrCacheModules_ = std::move(matrix.modules);
                } else {
                    qrCacheUrl_.clear();
                    qrCacheSize_ = 0;
                    qrCacheModules_.clear();
                }
            }
            if (!qrCacheUrl_.empty()) {
                snap.qrSize = qrCacheSize_;
                snap.qrMatrix = qrCacheModules_;
            }
        }
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
    // 两层限流（技术卡 §4）：全站 32/分 + 单客户端 12/分，任一超限即拒。
    bool allowed = globalHandshakeLimiter_.tryAcquire("__global__", now);
    if (!handshakeLimiter_.tryAcquire(address, now)) allowed = false;
    int retryAfter = 60;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handshakeHits_.size() > 256) {
            for (auto it = handshakeHits_.begin(); it != handshakeHits_.end();) {
                if (it->first != address &&
                    (it->second.empty() || it->second.back() <= now - kRateWindowSec)) {
                    it = handshakeHits_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto& hits = handshakeHits_[address];
        hits.push_back(now);
        while (!hits.empty() && hits.front() <= now - kRateWindowSec) hits.pop_front();
        if (!allowed && !hits.empty()) {
            retryAfter = static_cast<int>(kRateWindowSec - (now - hits.front()));
            if (retryAfter < 1) retryAfter = 1;
        }
        lastRetryAfter_ = allowed ? 0 : retryAfter;
    }
    if (!allowed) {
        logAuthFailure(address, "handshake_rate_limited", now);
        JsonObject out;
        out.set("e", JsonValue("too_many_requests"));
        out.set("retryAfter", JsonValue(retryAfter));
        rw.setHeader("Retry-After", std::to_string(retryAfter));
        replyJson(rw, 429, stringifyOr(JsonValue(out), "{\"e\":\"too_many_requests\"}"));
        return;
    }
    // P0-B：握手前清理 24h 空闲会话，避免 64 上限被死会话占死后永久 409。
    sessions_.prune(now);
    auto started = sessions_.beginHandshake(now);
    if (!started.isOk()) {
        rw.setHeader("Retry-After", "60");
        JsonObject out;
        out.set("e", JsonValue("too_many_sessions"));
        out.set("retryAfter", JsonValue(60));
        replyJson(rw, 409, stringifyOr(JsonValue(out), "{\"e\":\"too_many_sessions\"}"));
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

// 业务错误契约（老板裁决 2026-11-24，勿改成 HTTP 状态码）：票据满/登录锁定/会话满/
// 限流一律在密文 JSON 的 "s" 带 409/429 语义、HTTP 层仍 200；HTTP 只表达传输/协议级
// 错误（401/403/503）。前端按内层 "s"/"e" 分派。
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
        if (!ensureCookieKeyLocked()) return false;  // 密钥不可用 = 拒绝，不降级
        auto ticket = tickets_.validate(tokenValue.asString(), cookieKey_, now);
        if (!ticket.isOk()) return false;
        session->setTicketId(ticket.value().idHex);
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
        out.set("online", JsonValue(static_cast<int>(sessions_.authenticatedCount())));
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
        // 字段契约（W5 已按 b.text 接）：
        //   b.text  必填，UTF-8 文本（特殊键已由前端编码成转义序列）
        //   b.keys  可选，base64url 原始按键字节（优先级高于 text，给二进制键序）
        //   b.bp    可选，bool=ture 表示 bracketed paste（未自带 200~/201~ 包裹时服务端补）
        // HTTP 状态：无激活会话 = 200 + 密文内 409；命令队列满 = HTTP 503 + 密文内 503。
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        // 跨线程契约（P0-A）：HTTP 线程绝不触碰 SessionController；只看 UI tick
        // 同步的 activeSessions_（最多滞后一帧，可接受）。
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active = activeSessions_ > 0;
        }
        if (!active) return makeInnerError(409, "no_active_session");
        std::string bytes;
        const auto keysValue = body.get("keys");
        const auto textValue = body.get("text");
        if (keysValue.isString()) {
            auto decoded = wc::base64UrlDecode(keysValue.asString());
            if (!decoded.isOk()) return makeInnerError(400, "bad_keys");
            bytes.assign(decoded.value().begin(), decoded.value().end());
        } else if (textValue.isString()) {
            bytes = textValue.asString();
        } else {
            return makeInnerError(400, "bad_text");
        }
        const auto bpValue = body.get("bp");
        const bool bracketedPaste = bpValue.isBool() && bpValue.asBool();
        if (!enqueueCommand(makeInputCommandForTest(bytes, bracketedPaste))) {
            httpStatus = 503;
            return makeInnerError(503, "queue_full");
        }
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "resync") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        forceFull_.store(true);  // 下一帧 tick 由编码器重置出 full
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "new" || message == "close" || message == "select" ||
        message == "rename" || message == "restart" || message == "resize") {
        if (!requireTicket()) return makeInnerError(401, "bad_ticket");
        if (sessionBridge_ == nullptr) return makeInnerError(503, "no_bridge");
        const auto indexValue = body.get("index");
        const auto idValue = body.get("id");
        int index = -1;
        if (indexValue.isNumber()) {
            index = static_cast<int>(indexValue.asInt());
        } else if (idValue.isNumber()) {
            index = static_cast<int>(idValue.asInt());
        } else if (idValue.isString()) {
            try {
                index = std::stoi(idValue.asString());
            } catch (...) {
                index = -1;
            }
        }
        JsonObject command;
        command.set("op", JsonValue(message));
        command.set("index", JsonValue(index));
        if (message == "rename") {
            const auto titleValue = body.get("title");
            command.set("title", JsonValue(clampUtf8Chars(
                                         titleValue.isString() ? titleValue.asString()
                                                               : std::string(),
                                         32)));
        }
        int appliedRows = 0;
        int appliedCols = 0;
        if (message == "resize" || message == "new") {
            const auto rowsValue = body.get("rows");
            const auto colsValue = body.get("cols");
            const int rows = rowsValue.isNumber() ? static_cast<int>(rowsValue.asInt()) : 0;
            const int cols = colsValue.isNumber() ? static_cast<int>(colsValue.asInt()) : 0;
            const auto clamped = ::terminal::web::clampWebTerminalSize(rows, cols);
            appliedRows = clamped.rows;
            appliedCols = clamped.cols;
            command.set("rows", JsonValue(appliedRows));
            command.set("cols", JsonValue(appliedCols));
        }
        if (!enqueueCommand(stringifyOr(JsonValue(command), "{}"))) {
            httpStatus = 503;
            return makeInnerError(503, "queue_full");
        }
        JsonObject out;
        auto parsedTabs = platform::json::parse(tabSnapshotJson_);
        if (parsedTabs.isOk() && parsedTabs.value().isObject()) {
            out = parsedTabs.value().asObject();
        }
        // 命令在 UI 线程异步执行，此处的 tabSnapshotJson_ 是**命令前**快照。打 queued
        // 标让客户端丢弃这份旧列表；真正的新状态由 applyPendingCommands 执行后的
        // session 事件推回（客户端按事件刷新，不再被旧快照覆盖）。
        out.set("queued", JsonValue(true));
        if (message == "resize" || message == "new") {
            out.set("rows", JsonValue(appliedRows));
            out.set("cols", JsonValue(appliedCols));
        }
        return makeInner(200, JsonValue(out));
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
        // 跨线程契约（P0-A）：目录校验是纯函数，可在 HTTP 线程做（给出同步错误回包）；
        // 真正的 settings_ 写入 + 持久化 + 新会话参数只经命令队列在 UI tick 执行。
        if (body.has("startupDir")) {
            const auto dirValue = body.get("startupDir");
            if (dirValue.isString() && !dirValue.asString().empty()) {
                const std::string reason = settingsErrorReasonForTest(dirValue.asString());
                if (!reason.empty()) {
                    JsonObject err;
                    err.set("s", JsonValue(400));
                    err.set("e", JsonValue("settings_failed"));
                    err.set("reason", JsonValue(reason));
                    err.set("field", JsonValue("startupDir"));
                    return stringifyOr(JsonValue(err), "{\"s\":400,\"e\":\"settings_failed\"}");
                }
            }
        }
        JsonObject cmd;
        cmd.set("op", JsonValue("settings.set"));
        if (body.has("fontFamily")) cmd.set("fontFamily", JsonValue(body.get("fontFamily").asString()));
        if (body.has("fontSize")) cmd.set("fontSize", JsonValue(body.get("fontSize").asInt()));
        if (body.has("colorMode")) cmd.set("colorMode", JsonValue(body.get("colorMode").asString()));
        if (body.has("palette")) cmd.set("palette", JsonValue(body.get("palette").asString()));
        if (body.has("shell")) cmd.set("shell", JsonValue(body.get("shell").asString()));
        if (body.has("startupDir")) cmd.set("startupDir", JsonValue(body.get("startupDir").asString()));
        if (!enqueueCommand(stringifyOr(JsonValue(cmd), "{}"))) {
            httpStatus = 503;
            return makeInnerError(503, "queue_full");
        }
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "auth.login") {
        if (!loopback) {
            const auto codeValue = body.get("code");
            if (!codeValue.isString()) return makeInnerError(400, "bad_code");
            int retryAfter = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (loginLockout_.isLocked(clientAddress, now)) {
                    auto& hits = loginFailures_[clientAddress];
                    while (!hits.empty() && hits.front() <= now - kRateWindowSec) hits.pop_front();
                    retryAfter = hits.empty() ? 60
                                              : static_cast<int>(kRateWindowSec - (now - hits.front()));
                    if (retryAfter < 1) retryAfter = 1;
                }
            }
            if (retryAfter > 0) {
                JsonObject out;
                out.set("s", JsonValue(429));
                out.set("e", JsonValue("locked"));
                out.set("retryAfter", JsonValue(retryAfter));
                return stringifyOr(JsonValue(out), "{\"s\":429,\"e\":\"locked\"}");
            }
            if (totpSecret_.empty() || !verifyTotp(totpSecret_, codeValue.asString(), now)) {
                std::lock_guard<std::mutex> lock(mutex_);
                loginLockout_.recordFailure(clientAddress, now);
                if (loginFailures_.size() > 256) {
                    for (auto it = loginFailures_.begin(); it != loginFailures_.end();) {
                        if (it->first != clientAddress &&
                            (it->second.empty() || it->second.back() <= now - kRateWindowSec)) {
                            it = loginFailures_.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                auto& hits = loginFailures_[clientAddress];
                hits.push_back(now);
                while (!hits.empty() && hits.front() <= now - kRateWindowSec) hits.pop_front();
                return makeInnerError(401, "bad_code");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            loginLockout_.recordSuccess(clientAddress);
            loginFailures_.erase(clientAddress);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureCookieKeyLocked()) return makeInnerError(500, "key_unavailable");
        auto issued = tickets_.issue(cookieKey_, now);
        if (!issued.isOk()) {
            if (issued.errorCode() == core::ErrorCode::Conflict) {
                return makeInnerError(409, "too_many_tickets");
            }
            return makeInnerError(500, "ticket_failed");
        }
        lastLogin_ = now;
        session->setTicketId(issued.value().ticket.idHex);
        persistTicketsLocked();
        JsonObject out;
        out.set("t", JsonValue(issued.value().token));
        return makeInner(200, JsonValue(out));
    }
    if (message == "auth.logout") {
        if (loopback) return makeInner(200, JsonValue(nullptr));
        const auto tokenValue = body.get("t");
        if (!tokenValue.isString()) return makeInnerError(400, "bad_ticket");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureCookieKeyLocked()) return makeInnerError(500, "key_unavailable");
        auto ticket = tickets_.validate(tokenValue.asString(), cookieKey_, now);
        if (!ticket.isOk()) return makeInnerError(401, "bad_ticket");
        tickets_.revoke(ticket.value().idHex);
        persistTicketsLocked();
        return makeInner(200, JsonValue(nullptr));
    }
    if (message == "bind.claim") {
        // 契约：b.token = URL #b= 片段带来的 base64url 绑定令牌（前端读 location.hash）。
        // 回环可省略（桌面自测）；非回环必须带。错误码区分三态：no_token / bad_token /
        // bind_expired / bind_used / bind_terminal（PRD P-22 / app.js dirMsgs 同款映射）。
        const auto tokenValue = body.get("token");
        const std::string presented = tokenValue.isString() ? tokenValue.asString() : std::string();
        const std::string claimError = bindClaimErrorForTest(presented, loopback);
        if (!claimError.empty()) {
            const bool transport = claimError == "no_token" || claimError == "bad_token";
            return makeInnerError(transport ? 400 : 409, claimError);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto claim = claimBindToken(bindToken_, now);
        if (!claim.isOk()) return makeInnerError(409, "bind_terminal");
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
        if (outcome == ConfirmOutcome::InvalidCode) {
            // confirmBindToken 在累计第 5 次错码时已把令牌置终态；此时必须当场回
            // bind_invalid，前端据此立即禁用输入。只回 bad_code 会让用户继续输，
            // 直到下次 claim 才发现票已作废。
            if (bindToken_.state == BindTokenState::Terminal) {
                return makeInnerError(409, "bind_invalid");
            }
            return makeInnerError(400, "bad_code");
        }
        if (!ensureCookieKeyLocked()) return makeInnerError(500, "key_unavailable");
        totpSecret_ = bindToken_.secret;
        bound_ = true;
        boundAt_ = now;
        auto issued = tickets_.issue(cookieKey_, now);
        if (!issued.isOk()) return makeInnerError(409, "too_many_tickets");
        lastLogin_ = now;
        session->setTicketId(issued.value().ticket.idHex);
        persistTicketsLocked();
        JsonObject out;
        out.set("t", JsonValue(issued.value().token));
        return makeInner(200, JsonValue(out));
    }
    if (message == "bind.revoke") {  // 桌面/网页撤销验证器绑定
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tickets_.revokeAll();
            auto token = makeBindToken(now);
            if (token.isOk()) {
                bindToken_ = token.value();
                hasBindToken_ = true;
            }
            totpSecret_.clear();
            bound_ = false;
            boundAt_ = 0;
            lastLogin_ = 0;
            persistTicketsLocked();
        }
        // 与桌面 revokeBinding() 同一广播路径：其他在线端收到 revoke 后清票据并进 revoked 态。
        // 不能持 mutex_ 广播（订阅回调/流写可能在别的锁上），故先释放主锁。
        publishSessionEvent("revoke", "{}", true);
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
        JsonObject out;
        out.set("e", JsonValue("too_many_streams"));
        out.set("m", JsonValue("同时查看的连接数已达上限"));
        replyJson(rw, 503, stringifyOr(JsonValue(out), "{\"e\":\"too_many_streams\"}"));
        return;
    }
    forceFull_.store(true);
    pushSessionSnapshot_.store(true); // 晚连接补 tabs 快照（见头文件注释）
    if (sessionBridge_ != nullptr) sessionBridge_->addSubscriber();
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
            if (sessionBridge_ != nullptr) sessionBridge_->removeSubscriber();
            sessions_.finishStreamTicket(ticket, nowSec());
        });
}

void WebTerminalService::handleStatus(const platform::http::RequestCtx&,
                                      platform::http::ResponseWriter& rw) {
    replyJson(rw, 200, statusJsonForTest());
}

std::string WebTerminalService::statusJsonForTest() const {
    auto snap = snapshot();
    // 状态接口只回非秘密字段；hid/密钥/票据/绑定令牌绝不出现在明文
    JsonObject obj;
    obj.set("running", JsonValue(snap.running));
    obj.set("port", JsonValue(snap.port));
    obj.set("bindState", JsonValue(static_cast<int>(snap.bindState)));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obj.set("bound", JsonValue(bound_));
        // /api/status 未鉴权：只回登录态布尔与令牌倒计时，不暴露在线数/绑定时间/设备名。
        const auto remaining = hasBindToken_ ? bindToken_.expireAt - nowSec() : 0;
        obj.set("tokenRemainingSec", JsonValue(remaining > 0 ? static_cast<int>(remaining) : 0));
        if (desktopRestarted_) obj.set("emptyReason", JsonValue("restart"));
    }
    return stringifyOr(JsonValue(obj), "{}");
}

bool WebTerminalService::enqueueCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commands_.size() >= 256) return false;  // 有界队列，满 → HTTP 503
    commands_.push_back(command);
    return true;
}

void WebTerminalService::setSessionContext(::terminal::SessionController* controller,
                                           ::terminal::web::WebSessionBridge* bridge) {
    controller_ = controller;
    sessionBridge_ = bridge;
    if (sessionBridge_ != nullptr) {
        sessionBridge_->setListener([this](const ::terminal::web::WebEvent& ev) {
            switch (ev.type) {
                case ::terminal::web::WebEventType::Tabs: {
                    std::string snapshot;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        refreshTabSnapshotLocked();
                        snapshot = tabSnapshotJson_;
                    }
                    publishSessionEvent("session", snapshot, false);
                    break;
                }
                case ::terminal::web::WebEventType::Exit: {
                    JsonObject out;
                    out.set("tabIndex", JsonValue(ev.tabIndex));
                    out.set("exitCode", JsonValue(ev.exitCode));
                    publishSessionEvent("exit", stringifyOr(JsonValue(out), "{}"), true);
                    break;
                }
                case ::terminal::web::WebEventType::Failed: {
                    JsonObject out;
                    out.set("tabIndex", JsonValue(ev.tabIndex));
                    out.set("reason", JsonValue(ev.reason));
                    publishSessionEvent("exit", stringifyOr(JsonValue(out), "{}"), true);
                    break;
                }
                case ::terminal::web::WebEventType::Settings: {
                    std::string json;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        json = settingsJsonLocked();
                    }
                    publishSessionEvent("settings", json, false);
                    break;
                }
                case ::terminal::web::WebEventType::DesktopRestart: {
                    publishSessionEvent("desktop_restart", "{\"reason\":\"desktop_restart\"}", true);
                    break;
                }
            }
        });
    }
    forceFull_.store(true);
    publishSessionEvent("session", takeTabSnapshot(), false);
}

void WebTerminalService::tick() {
    if (sessionBridge_ == nullptr || controller_ == nullptr) return;
    sessionBridge_->pollStates();
    sessions_.prune(nowSec());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (desktopRestarted_ && controller_->count() > 0) desktopRestarted_ = false;
        if (nowSec() - lastHostRefresh_ > 30) {
            lastHostRefresh_ = nowSec();
            refreshAllowedHostsLocked();
        }
    }
    applyPendingCommands();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshTabSnapshotLocked();
    }
    publishFrame();
}

void WebTerminalService::drainCommands() { applyPendingCommands(); }

void WebTerminalService::applyPendingCommands() {
    std::deque<std::string> pending;
    std::function<void(const std::string&)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(commands_);
        handler = commandHandler_;
    }
    bool tabsChanged = false;
    for (const auto& command : pending) {
        if (handler) {
            handler(command);
            continue;
        }
        auto parsed = platform::json::parse(command);
        if (!parsed.isOk() || !parsed.value().isObject()) continue;
        const auto obj = parsed.value().asObject();
        const auto opValue = obj.get("op");
        if (!opValue.isString()) continue;
        const std::string op = opValue.asString();
        const auto indexValue = obj.get("index");
        const int index = indexValue.isNumber() ? static_cast<int>(indexValue.asInt()) : -1;
        if (op == "new" || op == "close" || op == "select" || op == "rename" ||
            op == "restart") {
            tabsChanged = true;
        }
        if (op == "input" && controller_ != nullptr) {
            const auto dataValue = obj.get("data");
            if (!dataValue.isString()) continue;
            auto bytes = wc::base64UrlDecode(dataValue.asString());
            if (!bytes.isOk()) continue;
            const auto pasteValue = obj.get("bp");
            const bool paste = pasteValue.isBool() && pasteValue.asBool();
            if (!bytes.value().empty()) {
                controller_->writeToActive(bytes.value().data(),
                                           static_cast<int32_t>(bytes.value().size()), paste);
            }
        } else if (op == "resize" && sessionBridge_ != nullptr) {
            const auto rowsValue = obj.get("rows");
            const auto colsValue = obj.get("cols");
            sessionBridge_->webResize(rowsValue.isNumber() ? static_cast<int>(rowsValue.asInt()) : 0,
                                      colsValue.isNumber() ? static_cast<int>(colsValue.asInt()) : 0);
        } else if (op == "new" && sessionBridge_ != nullptr) {
            sessionBridge_->createTab();
            const auto rowsValue = obj.get("rows");
            const auto colsValue = obj.get("cols");
            if (rowsValue.isNumber() && colsValue.isNumber()) {
                sessionBridge_->webResize(static_cast<int>(rowsValue.asInt()),
                                          static_cast<int>(colsValue.asInt()));
            }
        } else if (op == "close" && sessionBridge_ != nullptr) {
            sessionBridge_->closeTab(index);
        } else if (op == "select" && sessionBridge_ != nullptr) {
            sessionBridge_->activateTab(index);
        } else if (op == "restart" && sessionBridge_ != nullptr) {
            sessionBridge_->restartTab(index);
        } else if (op == "rename") {
            const auto titleValue = obj.get("title");
            if (index >= 0 && titleValue.isString()) {
                tabTitles_[index] = clampUtf8Chars(titleValue.asString(), 32);
                // 同步到桌面会话（P1-4）：桌面 tab 与网页显示同一名字。
                if (sessionBridge_ != nullptr) {
                    sessionBridge_->renameTab(index, titleValue.asString());
                }
            }
        } else if (op == "settings.set") {
            core::TerminalSettingsPatch patch;
            if (obj.has("fontFamily")) patch.fontFamily = obj.get("fontFamily").asString();
            if (obj.has("fontSize")) patch.fontSize = static_cast<int>(obj.get("fontSize").asInt());
            if (obj.has("colorMode")) patch.colorMode = obj.get("colorMode").asString();
            if (obj.has("palette")) patch.palette = obj.get("palette").asString();
            if (obj.has("shell")) patch.shell = obj.get("shell").asString();
            if (obj.has("startupDir")) patch.startupDir = obj.get("startupDir").asString();
            (void)writeSettings(patch);  // UI 线程：持久化 + 广播
        }
    }
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshTabSnapshotLocked();
        snapshot = tabSnapshotJson_;
    }
    // 命令回包是命令前快照（见 dispatchInner 的 queued 说明）；执行后再推一次
    // session 事件，客户端以此刷新 tab 列表/标题。
    if (tabsChanged) publishSessionEvent("session", snapshot, false);
}

void WebTerminalService::refreshTabSnapshotLocked() {
    tabSnapshotJson_ = takeTabSnapshot();
}

std::string WebTerminalService::takeTabSnapshot() const {
    std::vector<::terminal::web::WebTab> tabs;
    if (sessionBridge_ != nullptr) tabs = sessionBridge_->tabs();
    auto array = platform::json::makeArray();
    int activeIndex = -1;
    for (const auto& tab : tabs) {
        JsonObject item;
        item.set("id", JsonValue(std::to_string(tab.index)));
        item.set("index", JsonValue(tab.index));
        auto titleIt = tabTitles_.find(tab.index);
        item.set("title", JsonValue(titleIt != tabTitles_.end() ? titleIt->second : tab.title));
        item.set("active", JsonValue(tab.active));
        item.set("state", JsonValue(tab.state));
        item.set("exitCode", JsonValue(tab.exitCode));
        if (tab.active) activeIndex = tab.index;
        array.append(JsonValue(item));
    }
    JsonObject root;
    root.set("tabs", array);
    if (activeIndex >= 0) {
        root.set("activeId", JsonValue(std::to_string(activeIndex)));
    } else {
        root.set("activeId", JsonValue(nullptr));
    }
    return stringifyOr(JsonValue(root), "{\"tabs\":[],\"activeId\":null}");
}

void WebTerminalService::publishSessionEvent(const std::string& eventName,
                                             const std::string& payloadJson, bool control) {
    bridge_.publish(eventName, payloadJson, control);
}

void WebTerminalService::publishFrame() {
    if (sessionBridge_ == nullptr || controller_ == nullptr || gridEncoder_ == nullptr) return;
    if (bridge_.subscriberCount() == 0) return;
    // 晚连接补一次 tabs 快照（UI 线程读 controller，安全）
    if (pushSessionSnapshot_.exchange(false)) {
        publishSessionEvent("session", takeTabSnapshot(), false);
    }
    const ::terminal::Session* session = controller_->active();
    if (session == nullptr) return;
    if (forceFull_.exchange(false)) {
        gridEncoder_->reset();
        gridCursorValid_ = false;
    }
    bool dark = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dark = settings_.palette == "dark";
    }
    auto frame = gridEncoder_->encode(session->frame, session->lastGeneration,
                                      session->scrollOffset, controller_->activeScrollbackCount(),
                                      true, dark);
    const bool cursorMoved = !gridCursorValid_ ||
                             frame.header.cursorRow != gridLastCursorRow_ ||
                             frame.header.cursorCol != gridLastCursorCol_;
    if (frame.rows.empty() && !cursorMoved) return;
    gridCursorValid_ = true;
    gridLastCursorRow_ = frame.header.cursorRow;
    gridLastCursorCol_ = frame.header.cursorCol;
    const auto bytes = ::terminal::web::encodeWebFrame(frame);
    const std::string b64 = wc::base64UrlEncode(bytes);
    bridge_.publish(frame.header.full ? "full" : "rows", "{\"f\":\"" + b64 + "\"}");
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
    const bool allowed = handshakeLimiter_.tryAcquire(address, now);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& hits = handshakeHits_[address];
    hits.push_back(now);
    while (!hits.empty() && hits.front() <= now - kRateWindowSec) hits.pop_front();
    if (!allowed && !hits.empty()) {
        lastRetryAfter_ = static_cast<int>(kRateWindowSec - (now - hits.front()));
        if (lastRetryAfter_ < 1) lastRetryAfter_ = 1;
    } else {
        lastRetryAfter_ = 0;
    }
    return allowed;
}

std::string WebTerminalService::bindClaimErrorForTest(const std::string& token,
                                                     bool loopback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasBindToken_) return "no_token";
    if (token.empty()) {
        if (!loopback) return "no_token";
    } else if (!wc::constantTimeEquals(token, bindToken_.token)) {
        return "bad_token";
    }
    const auto now = nowSec();
    if (bindToken_.state == BindTokenState::Terminal) {
        if (bindToken_.terminal == BindTokenTerminal::Used) return "bind_used";
        if (bindToken_.terminal == BindTokenTerminal::Expired || now > bindToken_.expireAt) {
            return "bind_expired";
        }
        return "bind_terminal";
    }
    if (now > bindToken_.expireAt) return "bind_expired";
    return {};
}

std::vector<std::uint8_t> WebTerminalService::cookieKeyForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureCookieKeyLocked()) return {};
    return cookieKey_;
}

int WebTerminalService::qrEncodeCountForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return qrEncodeCount_;
}

void WebTerminalService::setPortForTest(int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) port_ = port;
}

int WebTerminalService::actualPortForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_ ? server_->actualPort() : 0;
}

void WebTerminalService::setConfiguredPort(int port) {
    if (port < 1024 || port > 65535) {
        LOG_WARN("网页终端: 忽略非法端口 " + std::to_string(port) + "（允许 1024-65535）");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) port_ = port;  // 运行中禁改：UI 端口行只读，重启后生效
}

void WebTerminalService::seedSettings(const std::string& fontFamily, int fontSize,
                                      const std::string& colorMode, const std::string& palette,
                                      const std::string& shell, const std::string& startupDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.fontFamily = fontFamily;
    settings_.fontSize = fontSize;
    settings_.colorMode = colorMode;
    settings_.palette = palette;
    settings_.shell = shell;
    settings_.startupDir = startupDir;
    settingsSeeded_ = true;
}

void WebTerminalService::markDesktopRestart() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desktopRestarted_ = true;
    }
    if (sessionBridge_ != nullptr) sessionBridge_->markDesktopRestart();
}

std::string WebTerminalService::bindUrlLocked() const {
    if (!hasBindToken_) return {};
    std::string host;
    for (const auto& addr : platform::localIpv4Addresses()) {
        if (!platform::isLoopbackAddress(addr)) {
            host = addr;
            break;
        }
    }
    if (host.empty()) {
        const auto addrs = platform::localIpv4Addresses();
        if (!addrs.empty()) host = addrs.front();
    }
    const int port = (running_ && server_) ? server_->actualPort() : port_;
    if (host.empty() || port <= 0) return {};
    return "http://" + host + ":" + std::to_string(port) + "/#b=" + bindToken_.token;
}

void WebTerminalService::persistTicketsLocked() const {
    const auto path = core::paths::appDataDir() / "web_tickets.dat";
    if (!tickets_.saveToFile(path)) {
        LOG_ERROR("网页终端: 票据写入失败 " + core::paths::toUtf8(path));
    }
}

void WebTerminalService::refreshAllowedHostsLocked() {
    if (!server_) return;
    std::vector<std::string> hosts = {"127.0.0.1", "localhost", "[::1]", "::1"};
    for (const auto& addr : platform::localIpv4Addresses()) hosts.push_back(addr);
    server_->setAllowedHosts(hosts);
}

std::size_t WebTerminalService::pendingCommandCountForTest() const {    std::lock_guard<std::mutex> lock(mutex_);
    return commands_.size();
}

std::vector<std::string> WebTerminalService::takeCommandsForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out(commands_.begin(), commands_.end());
    commands_.clear();
    return out;
}

void WebTerminalService::enqueueCommandForTest(const std::string& command) {
    (void)enqueueCommand(command);
}

std::string WebTerminalService::makeInputCommandForTest(const std::string& bytes,
                                                       bool bracketedPaste) const {
    JsonObject command;
    command.set("op", JsonValue("input"));
    command.set("data", JsonValue(wc::base64UrlEncode(
                            std::vector<std::uint8_t>(bytes.begin(), bytes.end()))));
    command.set("bp", JsonValue(bracketedPaste));
    return stringifyOr(JsonValue(command), "{}");
}

std::string WebTerminalService::bindTokenForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasBindToken_ ? bindToken_.token : std::string();
}

std::string WebTerminalService::tabSnapshotJsonForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tabSnapshotJson_;
}

int WebTerminalService::lastRetryAfterForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastRetryAfter_;
}
void WebTerminalService::setSessionActiveForTest(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSessions_ = active ? 1 : 0;
}

} // namespace module::terminal::web
