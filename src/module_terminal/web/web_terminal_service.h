// 网页终端服务骨架（need#490 W3，技术卡 §3 / §4 / §5）。
// 实现 core::IWebTerminalService：启停 HTTP、握手会话表、票据/限流、命令队列、SSE 广播、
// 设置单一写入入口。线程契约：HTTP 线程只碰本类的加锁状态与命令队列；会话状态/PTY 操作
// 由 UI 主线程通过 drainCommands() 排出（W4 接线）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/web_terminal_service.h"

#include "web_auth.h"
#include "web_bridge.h"
#include "web_dev_entry.h"
#include "web_crypto_session.h"

namespace terminal {
class SessionController;
namespace web {
class WebSessionBridge;
class WebGridEncoder;
} // namespace web
} // namespace terminal

namespace platform::http {
class Server;
struct RequestCtx;
class ResponseWriter;
} // namespace platform::http

namespace module::terminal::web {

class WebTerminalService : public core::IWebTerminalService {
public:
    WebTerminalService();
    ~WebTerminalService() override;

    // ---- core::IWebTerminalService ----
    core::WebAccessSnapshot snapshot() const override;
    core::Result<void> start() override;
    core::Result<void> stop() override;
    core::Result<void> refreshToken() override;
    core::Result<void> revokeBinding() override;
    core::Result<void> writeSettings(const core::TerminalSettingsPatch& patch) override;

    // ---- 路由入口（web_routes.cpp 调用）----
    void handleHandshakeGet(const platform::http::RequestCtx& ctx,
                            platform::http::ResponseWriter& rw);
    void handleHandshakePost(const platform::http::RequestCtx& ctx,
                             platform::http::ResponseWriter& rw);
    void handleEnvelope(const platform::http::RequestCtx& ctx,
                        platform::http::ResponseWriter& rw);
    void handleInput(const platform::http::RequestCtx& ctx,
                     platform::http::ResponseWriter& rw);
    void handleSettings(const platform::http::RequestCtx& ctx,
                        platform::http::ResponseWriter& rw);
    void handleStream(const platform::http::RequestCtx& ctx,
                      platform::http::ResponseWriter& rw);
    void handleStatus(const platform::http::RequestCtx& ctx,
                      platform::http::ResponseWriter& rw);

    // Host / Origin 校验（web_routes.cpp 在进 handler 前调用）
    bool isAllowedHost(const std::string& host) const;
    bool isAllowedOrigin(const std::string& origin) const;
    std::string clientAddressFor(const platform::http::RequestCtx& ctx) const;

    // ---- W4 接线（module_terminal 在 UI 主线程注入；本类不拥有二者）----
    void setSessionContext(::terminal::SessionController* controller,
                           ::terminal::web::WebSessionBridge* bridge);
    // UI 主线程每帧调用：pollStates → 排空命令 → 刷新 tab 快照 → 推网格帧
    void tick();
    // W4 事件（Exit/DesktopRestart/Failed）走不可丢控制队列
    void drainCommands();
    void setCommandHandler(std::function<void(const std::string&)> handler);
    void setSettingsPersist(
        std::function<core::Result<void>(const core::TerminalSettingsPatch&)> handler);
    // 激活会话数由 UI 会话控制器同步；0 → input/paste 返回密文内 409
    void setActiveSessionCount(int count);

    // ---- 测试缝 ----
    CryptoSessionTable& sessionsForTest() { return sessions_; }
    WebBridge& bridgeForTest() { return bridge_; }
    bool handshakeRateAllowedForTest(const std::string& address, std::int64_t nowSec);
    int actualPortForTest() const;
    std::size_t pendingCommandCountForTest() const;
    std::vector<std::string> takeCommandsForTest();
    void setSessionActiveForTest(bool active);
    std::string statusJsonForTest() const;
    std::string tabSnapshotJsonForTest() const;
    std::string makeInputCommandForTest(const std::string& text, bool bracketedPaste) const;
    static std::string settingsErrorReasonForTest(const std::string& dir);
    int lastRetryAfterForTest() const;
    // bind.claim 令牌校验（三态错误码）：空串 = 通过；否则 no_token/bad_token/
    // bind_expired/bind_used/bind_terminal。dispatch 与测试共用，防两处漂移。
    std::string bindClaimErrorForTest(const std::string& token, bool loopback) const;
    std::string bindTokenForTest() const;

private:
    core::WebAccessSnapshot snapshotLocked() const;
    void replyJson(platform::http::ResponseWriter& rw, int status, const std::string& json) const;
    void replyError(platform::http::ResponseWriter& rw, int status, const std::string& code) const;
    void replySealed(platform::http::ResponseWriter& rw,
                     const std::shared_ptr<CryptoSession>& session,
                     const std::string& plaintext, int httpStatus = 200) const;
    void handleEnvelopeImpl(const platform::http::RequestCtx& ctx,
                            platform::http::ResponseWriter& rw, const std::string& hint);
    // 解密 `/api/e` 请求体并定位会话；失败已直接回 400，返回 nullptr。
    std::shared_ptr<CryptoSession> decryptRequest(const platform::http::RequestCtx& ctx,
                                                  std::string& plaintext);
    std::string dispatchInner(const std::shared_ptr<CryptoSession>& session,
                              const std::string& plaintext, bool loopback,
                              const std::string& clientAddress, const std::string& hint,
                              int& httpStatus);
    bool enqueueCommand(const std::string& command);
    void applyPendingCommands();
    void refreshTabSnapshotLocked();
    void publishFrame();
    void publishSessionEvent(const std::string& eventName, const std::string& payloadJson,
                             bool control);
    std::string settingsJsonLocked() const;
    void broadcastSettingsLocked();
    std::string takeTabSnapshot() const;

    mutable std::mutex mutex_;
    std::unique_ptr<platform::http::Server> server_;
    bool running_ = false;
    int port_ = 7878;
    std::string bindAddress_ = "0.0.0.0";
    core::WebBindState bindState_ = core::WebBindState::Stopped;
    std::string error_;
    std::int64_t bindTime_ = 0;
    std::int64_t lastLogin_ = 0;
    int activeSessions_ = 0;

    DevEntryConfig devConfig_;
    CryptoSessionTable sessions_;
    WebBridge bridge_;
    SlidingWindowLimiter handshakeLimiter_{kHandshakeLimit, kRateWindowSec};
    LoginLockout loginLockout_;

    std::vector<std::uint8_t> cookieKey_;
    TicketStore tickets_;
    BindToken bindToken_;
    bool hasBindToken_ = false;
    std::vector<std::uint8_t> totpSecret_;

    std::deque<std::string> commands_;
    std::function<void(const std::string&)> commandHandler_;
    ::terminal::SessionController* controller_ = nullptr;
    ::terminal::web::WebSessionBridge* sessionBridge_ = nullptr;
    std::unique_ptr<::terminal::web::WebGridEncoder> gridEncoder_;
    std::atomic<bool> forceFull_{true};
    bool gridCursorValid_ = false;
    int gridLastCursorRow_ = -1;
    int gridLastCursorCol_ = -1;
    std::string tabSnapshotJson_ = "{\"tabs\":[],\"activeId\":null}";
    std::map<int, std::string> tabTitles_;
    int lastRetryAfter_ = 0;
    std::map<std::string, std::deque<std::int64_t>> handshakeHits_;
    std::map<std::string, std::deque<std::int64_t>> loginFailures_;
    bool bound_ = false;
    std::int64_t boundAt_ = 0;
    std::function<core::Result<void>(const core::TerminalSettingsPatch&)> settingsPersist_;

    struct SettingsState {
        std::string fontFamily;
        int fontSize = 13;
        std::string colorMode = "followApp";
        std::string palette = "light";
        std::string shell;
        std::string startupDir;
    } settings_;
};

} // namespace module::terminal::web
