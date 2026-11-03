#include "module_terminal.h"

#include "core/logging.h"
#include "core/theme.h"
#include "platform/terminal/terminal_backend.h"
#include "terminal_fullscreen_bridge.h"

namespace terminal {

TerminalModule::TerminalModule() {
    backend_ = platform::terminalBackendMac();
    controller_.setBackend(backend_);
}

core::PageId TerminalModule::pageId() const { return core::PageId::Terminal; }
std::string TerminalModule::displayName() const { return "终端"; }
std::string TerminalModule::shortName() const { return "终端"; }
std::string TerminalModule::iconGlyph() const { return "\xEF\x84\xA0"; }  // U+F120 fa-terminal

void TerminalModule::ensureBackend() {
    if (controller_.state() == terminal::SessionState::Idle) controller_.startNew();
}

void TerminalModule::onPageEnter() { ensureBackend(); }
void TerminalModule::onStart() { ensureBackend(); }
void TerminalModule::onStop() {
    controller_.stop();
    if (page_.fullscreen()) {
        page_.toggleFullscreen();
        app::terminal_fullscreen::setCollapsed(false);
    }
}

// 模块级键盘兜底：Cmd+C（encodeKey 对 super 系返回 false）→ 选区文本 → 剪贴板（F-04）
bool TerminalModule::onKeyEvent(const eui::KeyEvent& event) {
    if (event.isDown() && event.modifiers.super && event.key == eui::InputKey::C) {
        std::string text;
        // 行列=page 记录的拖选范围（视口系，Selection.text 同系）
        int r0 = page_.selARow(), c0 = page_.selACol();
        int r1 = page_.selHRow(), c1 = page_.selHCol();
        if (page_.hasSelection() &&
            controller_.selectionText(r0, c0, r1, c1, text)) {
            platform::clipboardCopyText(reinterpret_cast<const uint8_t*>(text.data()),
                                        static_cast<int32_t>(text.size()));
            return true;
        }
    }
    return false;
}

void TerminalModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    // 可用性（OS<1809 → controller.startNew 前置检查进 Failed 态，技术卡 v2 决策 6）
    const bool available = backend_ != nullptr && backend_->available();

    GridLayout layout = page_.compose(
        ui, screen, controller_,
        [this](const uint8_t* bytes, int32_t n) { controller_.writeToSession(bytes, n); },
        [this](const std::string& text, bool paste) {
            // paste=true → 核心带 bracketed paste 语义（writePaste，技术卡 §5.4）
            controller_.writeToSession(reinterpret_cast<const uint8_t*>(text.data()),
                                       static_cast<int32_t>(text.size()), paste);
        },
        [](const std::string& text) {
            platform::clipboardCopyText(reinterpret_cast<const uint8_t*>(text.data()),
                                        static_cast<int32_t>(text.size()));
        });
    // 全屏态变化 → 侧栏折叠缝（F-05；近似语义：折叠非隐藏，偏差已记录）
    if (page_.fullscreen() != sidebarCollapsedApplied_) {
        sidebarCollapsedApplied_ = page_.fullscreen();
        app::terminal_fullscreen::setCollapsed(page_.fullscreen());
    }
    // widget 量化 → resize 会话（先 reflow 后 TIOCSWINSZ，核心语义）
    if (available && page_.wantsResize()) {
        controller_.resizeGrid(page_.pendingRows(), page_.pendingCols());
        page_.clearPendingResize();
    }
    (void)layout;
}

} // namespace terminal

extern "C" ::core::Module* create_terminal_module() {
    return new terminal::TerminalModule();
}
