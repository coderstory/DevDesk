// src/module_terminal/module_terminal.h
// 终端模块（Router 页面模块，技术卡 §7）。
#pragma once

#include "core/module_interface.h"
#include "core/router.h"

#include "terminal_page.h"
#include "terminal_session_controller.h"
#include "terminal_settings.h"

namespace terminal {

class TerminalModule : public core::Module {
public:
    // injectedBackend=测试缝（复审 P2：doctest 注入 fake，防构造即接真后端在测试里
    // 真 spawn pwsh）；缺省空指针 → 平台真后端
    explicit TerminalModule(platform::TerminalBackend* injectedBackend = nullptr);

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

    // 测试缝（bug247 doctest）：当前会话数（只读透传 controller）
    int sessionCount() const { return controller_.count(); }
    // 测试缝（bug406 doctest）：后台建会话结果要靠 pollAll 收割；生产走 compose → pollAll
    SessionController& controllerForTest() { return controller_; }

private:
    void ensureBackend();
    // bug278④：设置面板结果通道（完成=true 提交落库+新会话参数生效；×/Esc=false
    // 丢弃草稿回滚快照）。老板口径 2026-11-16：弹窗内改动只在「完成」落库/生效。
    void finishSettingsPanel(bool commit);

    SessionController controller_;
    TerminalPage page_;
    platform::TerminalBackend* backend_ = nullptr;
    bool sidebarCollapsedApplied_ = false;
    // need250：终端设置单一数据源（面板开着=草稿态：改动即时预览但不落库，
    // bug278④；「完成」才提交）
    TerminalSettings settings_;
    TerminalSettings panelSnapshot_;   // bug278④：面板打开时刻快照（草稿基线）
    bool panelWasOpen_ = false;        // bug278④：开合沿侦测（开帧打快照）
};

} // namespace terminal

extern "C" ::core::Module* create_terminal_module();
