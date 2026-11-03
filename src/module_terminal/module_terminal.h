// src/module_terminal/module_terminal.h
// 终端模块（Router 页面模块，技术卡 §7）。
#pragma once

#include "core/module_interface.h"

#include "terminal_page.h"
#include "terminal_session_controller.h"

namespace terminal {

class TerminalModule : public core::Module {
public:
    TerminalModule();

    core::PageId pageId() const override;
    std::string displayName() const override;
    std::string shortName() const override;
    std::string iconGlyph() const override;

    void compose(eui::Ui& ui, const eui::Screen& screen) override;
    void onPageEnter() override;
    void onStart() override;
    void onStop() override;

    // 模块级键盘兜底（Cmd+C 选区复制）
    bool onKeyEvent(const eui::KeyEvent& event) override;

private:
    void ensureBackend();

    SessionController controller_;
    TerminalPage page_;
    platform::TerminalBackend* backend_ = nullptr;
    bool sidebarCollapsedApplied_ = false;
};

} // namespace terminal

extern "C" ::core::Module* create_terminal_module();
