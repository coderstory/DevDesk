// src/module_terminal/module_terminal.h
// 终端模块（Router 页面模块，技术卡 §7）。
#pragma once

#include "core/module_interface.h"
#include "core/router.h"
#include "core/ticker.h"
#include "core/web_terminal_service.h"

#include "terminal_page.h"
#include "terminal_session_controller.h"
#include "terminal_settings.h"
#include "web/web_session_bridge.h"
#include "web/web_terminal_service.h"

#include <memory>

namespace terminal {

class TerminalModule : public core::Module {
public:
    // injectedBackend=测试缝（复审 P2：doctest 注入 fake，防构造即接真后端在测试里
    // 真 spawn pwsh）；缺省空指针 → 平台真后端
    explicit TerminalModule(platform::TerminalBackend* injectedBackend = nullptr);
    ~TerminalModule() override;

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

    // M2 Task 3：退出守卫（app::setQuitGuard 组合入口）
    bool needsQuitConfirm() const;
    void requestQuitConfirm();

    // 测试缝（bug247 doctest）：当前会话数（只读透传 controller）
    int sessionCount() const { return controller_.count(); }
    // 测试缝（bug406 doctest）：后台建会话结果要靠 pollAll 收割；生产走 compose → pollAll
    SessionController& controllerForTest() { return controller_; }

    // 设置单一真源（技术卡 §3.2 / P-24）：网页 settings.set 与桌面面板「完成」同一入口。
    // 校验目录 → 写 settings_ → 持久化（失败回滚）→ 新会话参数 → 广播/重绘。
    core::Result<void> writeSettings(const TerminalSettings& next);

private:
    void ensureBackend();
    // need#490 W6/W3 接线：服务实例生命周期 + 每帧 tick（全局 ticker，页面不可见也跑）。
    void startWebService();
    void stopWebService();
    void tickWeb();
    core::Result<void> persistWebSettings(const core::TerminalSettingsPatch& patch);
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
    // need#490 W4：网页会话桥（tab/尺寸/退出态/设置单一入口）。settings_ 指针必须在
    // 本成员之前声明（桥持其地址）。
    web::WebSessionBridge bridge_;
    // 网页终端服务（W3）：模块独占持有并注册为 core::IWebTerminalService（设置页消费）。
    std::unique_ptr<::module::terminal::web::WebTerminalService> webService_;
    core::ticker::Handle webTickerHandle_ = 0;
};

} // namespace terminal

extern "C" ::core::Module* create_terminal_module();
