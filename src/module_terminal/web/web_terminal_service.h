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
    // 设置页端口编辑（need#490 W6）：仅未运行时改写配置端口（运行中 UI 只读）。
    void setConfiguredPort(int port);
    // 桌面设置播种：服务 settings_ 的唯一初始来源（否则网页配置页回显默认值、shell 丢失）。
    void seedSettings(const std::string& fontFamily, int fontSize, const std::string& colorMode,
                      const std::string& palette, const std::string& shell,
                      const std::string& startupDir);
    // 桌面启动/会话全灭：标记并广播「桌面端已重启」，前端据此显示空态文案。
    void markDesktopRestart();

    // ---- 测试缝 ----
    CryptoSessionTable& sessionsForTest() { return sessions_; }
    WebBridge& bridgeForTest() { return bridge_; }
    bool handshakeRateAllowedForTest(const std::string& address, std::int64_t nowSec);
    int actualPortForTest() const;
    std::size_t pendingCommandCountForTest() const;
    std::vector<std::string> takeCommandsForTest();
    // 队列消费判据（W6 接线用例）：tick 未调用 → 命令滞留；tick 调用 → 排空。
    void enqueueCommandForTest(const std::string& command);
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
    // 确保 cookie_key 已从 app_secrets 载入（或首次生成并落库）；空返回 = 不可用。
    // 失败不生成临时密钥冒充可用——旧票据会全失效且故障被掩盖。
    std::vector<std::uint8_t> cookieKeyForTest();
    // QR 编码调用计数：证明未绑定态重复 snapshot 命中缓存、不重编。
    int qrEncodeCountForTest() const;
    TicketStore& ticketsForTest() { return tickets_; }
    // 测试缝：未运行时设监听端口；0 = 绑定任意空闲端口（测试免固定端口冲突）。
    void setPortForTest(int port);

private:
    core::WebAccessSnapshot snapshotLocked() const;
    // 调用方须持 mutex_。密钥只存 app_secrets（系统机密存储禁项后的唯一通道）。
    bool ensureCookieKeyLocked();
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
    std::string bindUrlLocked() const;
    void persistTicketsLocked() const;
    void refreshAllowedHostsLocked();

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
    // 晚连接补快照（2026-11-27）：新订阅者接入后，下一帧 UI tick 补推一次
    // 「session」事件（tabs+activeId），否则网页端直到用户点 tab 才拿到 tab 列表，
    // 空态遮罩会盖在活帧上（第二台设备打开网页端必现）。
    std::atomic<bool> pushSessionSnapshot_{false};
    bool gridCursorValid_ = false;
    int gridLastCursorRow_ = -1;
    int gridLastCursorCol_ = -1;
    std::string tabSnapshotJson_ = "{\"tabs\":[],\"activeId\":null}";
    // 未绑定态 QR 缓存：键 = 绑定 URL（含令牌）。snapshot 每帧调用，令牌不变就不重编。
    mutable std::string qrCacheUrl_;
    mutable int qrCacheSize_ = 0;
    mutable std::vector<std::uint8_t> qrCacheModules_;
    mutable int qrEncodeCount_ = 0;
    std::map<int, std::string> tabTitles_;
    int lastRetryAfter_ = 0;
    SlidingWindowLimiter globalHandshakeLimiter_{kHandshakeGlobalLimit, kRateWindowSec};
    bool desktopRestarted_ = false;
    bool settingsSeeded_ = false;
    std::int64_t lastHostRefresh_ = 0;
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
