#include "module_terminal.h"

#include "core/database.h"
#include "core/logging.h"
#include "core/theme.h"
#include "platform/terminal/terminal_backend.h"
#include "terminal_fullscreen_bridge.h"

namespace terminal {

TerminalModule::TerminalModule(platform::TerminalBackend* injectedBackend) {
    backend_ = injectedBackend != nullptr ? injectedBackend : platform::terminalBackendMac();
    controller_.setBackend(backend_);
    // need250：载入持久化设置并铺到新会话参数（db 未就绪/键缺失 → 默认值）
    settings_ = loadTerminalSettings(::core::appDatabase());
    controller_.setNewSessionOptions(settings_.shell, settings_.workDir, settings_.scrollback);
}

core::PageId TerminalModule::pageId() const { return core::PageId::Terminal; }
std::string TerminalModule::displayName() const { return "终端"; }
std::string TerminalModule::shortName() const { return "终端"; }
std::string TerminalModule::iconGlyph() const { return "\xEF\x84\xA0"; }  // U+F120 fa-terminal

void TerminalModule::ensureBackend() {
    if (controller_.empty()) controller_.startSession();
}

void TerminalModule::onPageEnter() { ensureBackend(); }
// bug247：开机不建会话——onStart 不再 ensureBackend（应用启动不占 ConPTY 进程/
// 不出「终端 1」）；首个会话由 onPageEnter 建（ensureBackend 内 empty 检查即幂等）
void TerminalModule::onStart() {}
void TerminalModule::onStop() {
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
        page_.closeSettings();
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
        settings_,
        // need250：面板任意改动 → 新会话参数同步 + 落库（即时生效项靠 settings_ 单一
        // 数据源在下一帧 compose 直接可见；db 未就绪 save fail 只记日志不阻断）
        [this]() {
            controller_.setNewSessionOptions(settings_.shell, settings_.workDir,
                                             settings_.scrollback);
            auto r = saveTerminalSettings(::core::appDatabase(), settings_);
            if (r.isFail()) LOG_WARN("Terminal: 设置保存失败: " + r.errorMessage());
        });
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
        controller_.resizeActive(page_.pendingRows(), page_.pendingCols());
        page_.clearPendingResize();
    }
    (void)layout;
}

} // namespace terminal

extern "C" ::core::Module* create_terminal_module() {
    return new terminal::TerminalModule();
}
