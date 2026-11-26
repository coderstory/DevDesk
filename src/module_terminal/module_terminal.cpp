#include "module_terminal.h"

#include <eui/app.h>

#include "terminal_quit_guard.h"

#include "core/database.h"
#include "core/logging.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "platform/terminal/terminal_backend.h"
#include "terminal_fullscreen_bridge.h"
#include <core/platform/platform.h>  // core::platform::requestUiUpdate（bug249 输出唤醒出口）

namespace terminal {

namespace {
// M2 Task 3：退出守卫需要一个「当前终端模块」入口（EUI quit guard 是无参回调，
// 先例 = notepad detail::sessionRegistry）。
TerminalModule* g_terminalModule = nullptr;
} // namespace

TerminalModule::TerminalModule(platform::TerminalBackend* injectedBackend)
    // 设置单一真源经桥收口：网页与桌面「完成」都走 bridge_.writeSettings；重绘用
    // requestUiUpdate 与 bug249 同一出口。
    : bridge_(controller_, &settings_,
              [](const TerminalSettings& s) {
                  return saveTerminalSettings(::core::appDatabase(), s);
              },
              [] { core::platform::requestUiUpdate(); }) {
    backend_ = injectedBackend != nullptr ? injectedBackend : platform::terminalBackendMac();
    controller_.setBackend(backend_);
    // bug249：reader 线程产出新输出/子进程退出 → 请求一帧（非 UI 线程安全路径；
    // 常驻 onFrame 泵因此可摘，空闲终端零出帧）
    controller_.setOutputWake([] { core::platform::requestUiUpdate(); });
    // need250：载入持久化设置并铺到新会话参数（db 未就绪/键缺失 → 默认值）。
    // C-4（need380）：目录/回滚 UI 已砍，固定默认（空=主目录 / 1000 行）
    settings_ = loadTerminalSettings(::core::appDatabase());
    controller_.setNewSessionOptions(settings_.shell, settings_.startupDir, kScrollbackDefault);
    // bug278④：面板结果通道装配（完成/× 由页面转发到 finishSettingsPanel）
    page_.setPanelResultHandler([this](bool commit) { finishSettingsPanel(commit); });
    // M2：Cmd/Ctrl + = / - / 0 字号快捷缩放——delta=0 回默认；改值即持久化并下发网页
    g_terminalModule = this;
    // M2 Task 3：关标签走模块裁定——Running 先弹确认，其余直关
    page_.setCloseTabHandler([this](int index) {
        const Session* s = controller_.at(index);
        if (s != nullptr && stateNeedsCloseConfirm(s->state)) {
            page_.requestCloseConfirm(index);
        } else {
            controller_.closeSession(index);
        }
    });
    page_.setConfirmHandler([this](TerminalPage::ConfirmKind kind, int index, bool ok) {
        if (!ok) return;
        if (kind == TerminalPage::ConfirmKind::QuitAll) {
            while (!controller_.empty()) controller_.closeSession(0);
            app::requestExit();
        } else if (kind == TerminalPage::ConfirmKind::CloseOne) {
            controller_.closeSession(index);
        }
    });
    page_.setFontSizeHandler([this](int delta) {
        const int next = delta == 0 ? kFontSizeDefault : settings_.fontSize + delta;
        settings_.fontSize = clampedFontSize(next);
        (void)writeSettings(settings_);
        core::platform::requestUiUpdate();
    });
}

TerminalModule::~TerminalModule() {
    stopWebService();
    if (g_terminalModule == this) g_terminalModule = nullptr;
}

bool TerminalModule::needsQuitConfirm() const {
    return anyRunningSession(controller_);
}

void TerminalModule::requestQuitConfirm() { page_.requestQuitConfirm(); }

bool terminalQuitGuard() {
    if (g_terminalModule == nullptr) return true;
    if (!g_terminalModule->needsQuitConfirm()) return true;
    // 有 Running 会话：弹页内确认并拦截本次退出；确认后关全部会话再 requestExit
    g_terminalModule->requestQuitConfirm();
    return false;
}

core::PageId TerminalModule::pageId() const { return core::PageId::Terminal; }
std::string TerminalModule::displayName() const { return "终端"; }
std::string TerminalModule::shortName() const { return "终端"; }
std::string TerminalModule::iconGlyph() const { return "\xEF\x84\xA0"; }  // U+F120 fa-terminal

void TerminalModule::ensureBackend() {
    // bug406：后台建会话——进页帧不被 ConPTY 创建（实测 14.4ms）阻塞；会话以 Starting
    // 占位（页面骨架），成功/失败由 compose → pollAll 收割。Starting 也是会话，幂等不变。
    if (controller_.empty()) controller_.startSessionAsync();
}

// need#490 W6/W3 接线：服务实例由模块独占（设置页经 ServiceLocator 消费同一实例），
// UI 主线程每帧 tick 才能排空网页命令队列（input/resize/tab 操作）+ 推网格帧。
void TerminalModule::startWebService() {
    if (!webService_)
        webService_ = std::make_unique<::module::terminal::web::WebTerminalService>();
    // P1-F：服务 settings_ 的唯一初始来源 = 当前会话设置（否则网页配置页回显默认值）。
    webService_->seedSettings(
        settings_.fontFamily, settings_.fontSize,
        settings_.colorMode == TerminalColorMode::Independent ? "independent" : "followApp",
        settings_.colorPalette == TerminalColorPalette::Dark ? "dark" : "light", settings_.shell,
        settings_.startupDir);
    if (settings_.webPort >= 1024) webService_->setConfiguredPort(settings_.webPort);  // P2-8
    webService_->setSessionContext(&controller_, &bridge_);
    webService_->setSettingsPersist(
        [this](const core::TerminalSettingsPatch& patch) {
            return persistWebSettings(patch);
        });
    webService_->setActiveSessionCount(controller_.count());
    ::core::ServiceLocator::set<::core::IWebTerminalService>(webService_.get());
    // P1-3：桌面启动无任何会话 → 标记「桌面端已重启」，网页重连读空态文案。
    if (controller_.count() == 0) webService_->markDesktopRestart();
    if (webTickerHandle_ == 0) {
        webTickerHandle_ = ::core::ticker::subscribe(
            {.name = "terminal.web.tick",
             .fn =
                 [](::core::ticker::Now, void* user) {
                     static_cast<TerminalModule*>(user)->tickWeb();
                 },
             .user = this,
             .firstAt = 0,
             .intervalSec = 0,  // 每次 pump（主循环每帧）
             .expireAt = 0});
    }
}

void TerminalModule::tickWeb() {
    if (!webService_) return;
    // 会话帧与后台建会话结果只由 pollAll 收割；终端页不可见时 compose 不跑，网页端
    // 会一直拿到 0x0 空帧（W7 E2E 实测）。这里补一次幂等 pollAll（按 generation 去重），
    // 页面可见时重复调用无副作用。
    controller_.pollAll();
    webService_->setActiveSessionCount(controller_.count());
    webService_->tick();
}

void TerminalModule::stopWebService() {
    // 顺序不可换：先摘 ticker（此后不再被 pump），再停服务并注销，最后析构。
    ::core::ticker::unsubscribe(webTickerHandle_);
    webTickerHandle_ = 0;
    ::core::ServiceLocator::set<::core::IWebTerminalService>(nullptr);
    if (webService_) {
        (void)webService_->stop();
        webService_.reset();
    }
}

core::Result<void> TerminalModule::persistWebSettings(
    const core::TerminalSettingsPatch& patch) {
    // 网页 settings.set 与桌面面板「完成」落到同一 store：以库内已存值为基底打补丁，
    // 避免把打开中的桌面草稿写进库。端口为服务级配置（不进 terminal_settings）。
    TerminalSettings next = loadTerminalSettings(::core::appDatabase());
    if (patch.fontFamily) next.fontFamily = *patch.fontFamily;
    if (patch.fontSize) next.fontSize = *patch.fontSize;
    if (patch.colorMode) {
        next.colorMode = *patch.colorMode == "independent"
                             ? TerminalColorMode::Independent
                             : TerminalColorMode::FollowApp;
    }
    if (patch.palette) {
        next.colorPalette = *patch.palette == "dark"
                                ? TerminalColorPalette::Dark
                                : TerminalColorPalette::Light;
    }
    if (patch.shell) next.shell = *patch.shell;
    if (patch.startupDir) next.startupDir = *patch.startupDir;
    if (patch.webPort) {
        next.webPort = *patch.webPort;
        settings_.webPort = *patch.webPort;
        if (webService_) webService_->setConfiguredPort(*patch.webPort);
    }
    return writeSettings(next);
}

void TerminalModule::onPageEnter() { ensureBackend(); }
// bug247：开机不建会话——onStart 不再 ensureBackend（应用启动不占 ConPTY 进程/
// 不出「终端 1」）；首个会话由 onPageEnter 建（ensureBackend 内 empty 检查即幂等）
void TerminalModule::onStart() { startWebService(); }
void TerminalModule::onStop() {
    // 先停网页服务（摘 ticker → 服务 stop → 注销 ServiceLocator），避免 tick 打在
    // 随后被销毁的会话/桥对象上。
    stopWebService();
    // 全会话终止（模块停用=会话生命周期终点）；v2 多会话无单例句柄语义
    while (!controller_.empty()) {
        controller_.closeSession(0);
    }
    if (page_.fullscreen()) {
        page_.toggleFullscreen();
        app::terminal_fullscreen::setCollapsed(false);
    }
}

// 模块级键盘兜底：Esc 关设置面板（need250 关闭通道之一；settings/release 模块同款
// module 级 Esc 先例）→ Cmd+C（encodeKey 对 super 系返回 false）→ 选区文本 → 剪贴板（F-04）
bool TerminalModule::onKeyEvent(const eui::KeyEvent& event) {
    if (event.isDown() && event.key == eui::InputKey::Escape && page_.settingsOpen()) {
        finishSettingsPanel(false);  // bug278④：Esc=取消（丢弃草稿）
        return true;
    }
    // need#452：目录弹层 Esc 关闭（面板优先于本层，见上；不改既有键序）
    if (event.isDown() && event.key == eui::InputKey::Escape && page_.dirMenuOpen()) {
        page_.closeDirMenu();
        return true;
    }
    if (event.isDown() && event.modifiers.super && event.key == eui::InputKey::C) {
        std::string text;
        // 行列=page 记录的拖选范围（视口系，Selection.text 同系）
        int r0 = page_.selARow(), c0 = page_.selACol();
        int r1 = page_.selHRow(), c1 = page_.selHCol();
        if (page_.hasSelection() &&
            controller_.activeSelectionText(r0, c0, r1, c1, text)) {
            platform::clipboardCopyText(reinterpret_cast<const uint8_t*>(text.data()),
                                        static_cast<int32_t>(text.size()));
            return true;
        }
    }
    return false;
}

void TerminalModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    // bug278④：面板开合沿——开帧打草稿基线快照（本帧面板刚出现、用户尚未编辑，
    // settings_ 即打开时刻原值；编辑发生在后续帧）。关闭沿由 finishSettingsPanel 处理。
    if (page_.settingsOpen() && !panelWasOpen_) panelSnapshot_ = settings_;
    panelWasOpen_ = page_.settingsOpen();

    // 可用性（OS<1809 → startSession 前置检查进 Failed 态占位，技术卡 v2 决策 6）
    const bool available = backend_ != nullptr && backend_->available();

    GridLayout layout = page_.compose(
        ui, screen, controller_,
        [this](const uint8_t* bytes, int32_t n) { controller_.writeToActive(bytes, n); },
        [this](const std::string& text, bool paste) {
            // paste=true → 核心带 bracketed paste 语义（writePaste，技术卡 §5.4）
            controller_.writeToActive(reinterpret_cast<const uint8_t*>(text.data()),
                                       static_cast<int32_t>(text.size()), paste);
        },
        [](const std::string& text) {
            platform::clipboardCopyText(reinterpret_cast<const uint8_t*>(text.data()),
                                        static_cast<int32_t>(text.size()));
        },
        // bug278④：settings_=草稿态单一数据源——改动直写即 live preview；落库与
        // 生效只在「完成」（finishSettingsPanel），×/Esc 回滚快照（无逐改动回调）
        settings_);
    // 全屏态变化 → 侧栏全隐缝（F-05；v2 终裁 clue92：全屏=侧栏 0 宽全隐、不画 logo，
    // 「折叠 72px 非隐藏」方案已作废）。主通道 = TerminalPage::toggleFullscreen 内的
    // 当帧打桥（bug252 根修）；此处差值检查保留，兜 DEVDESK_TERMINAL_FULLSCREEN env
    // 注入初始化（不经 toggle）的态同步，幂等无害。
    if (page_.fullscreen() != sidebarCollapsedApplied_) {
        sidebarCollapsedApplied_ = page_.fullscreen();
        app::terminal_fullscreen::setCollapsed(page_.fullscreen());
    }
    // widget 量化 → resize 激活会话（先 reflow 后 TIOCSWINSZ，核心语义）；
    // 切换会话后尺寸不一致时 page 会重新挂 pending（自动补 resize）
    if (available && page_.wantsResize()) {
        bridge_.observeDesktopResize(page_.pendingRows(), page_.pendingCols());
        controller_.resizeActive(page_.pendingRows(), page_.pendingCols());
        page_.clearPendingResize();
    }
    (void)layout;
}

// bug278④：面板结果落地（老板口径：持久化/生效只在「完成」）。commit=草稿即终态
// （新会话参数同步 + 落库；db 未就绪 save fail 只记日志不阻断）；取消=回滚打开时刻
// 快照（草稿丢弃，live preview 的改动下一帧随 settings_ 还原）。
void TerminalModule::finishSettingsPanel(bool commit) {
    if (commit) {
        // 单一入口：与网页 settings.set 同一条路径（目录校验/回灌/持久化/新会话参数）。
        auto r = writeSettings(settings_);
        if (r.isFail()) LOG_WARN("Terminal: 设置保存失败: " + r.errorMessage());
    } else {
        settings_ = panelSnapshot_;
    }
    page_.closeSettings();
}

core::Result<void> TerminalModule::writeSettings(const TerminalSettings& next) {
    return bridge_.writeSettings(next);
}

} // namespace terminal

extern "C" ::core::Module* create_terminal_module() {
    return new terminal::TerminalModule();
}
