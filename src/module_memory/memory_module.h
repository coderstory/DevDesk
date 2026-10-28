// src/module_memory/memory_module.h
// 记忆页模块：telemem 记忆引擎托管面板三视图（主控台 / 记忆审核 / 配置）。
// 数据全部经 panel::PanelState 取数/操作（面板库随 DEVDESK_DATA_ROOT 隔离）；
// 引擎托管逻辑在 engine/ 子层，本类只做 UI 编排。密钥/记忆正文不进日志。
#pragma once

#include <memory>
#include <set>
#include <string>

#include "core/module_interface.h"
#include "module_memory/panel/panel_state.h"
#include "ui_kit/toast.h"

namespace core::theme {
struct Tokens;  // 前置声明：三视图 compose 按 const& 传 token，头不拖 theme 全量
}

namespace module::memory {

class MemoryModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override;
    std::string displayName() const override;
    std::string shortName() const override;
    std::string iconGlyph() const override;
    void compose(eui::Ui& ui, const eui::Screen& screen) override;
    void onStart() override;
    void onStop() override;

    // 测试/探针缝：视图切换不经点击注入（probe 直驱）
    void setActiveViewForTest(int view) { activeView_ = view; }

private:
    // 三视图 compose（在滚动容器 content 内绘制；y = 视口顶，x = 内容左，
    // 内部统一再减各自 scroll 偏移，同 settings 卡区模式）
    void composeConsole(eui::Ui& ui, const core::theme::Tokens& t, float x,
                        float w, float y, float viewH);
    void composeReview(eui::Ui& ui, const core::theme::Tokens& t, float x,
                       float w, float y, float viewH);
    void composeConfig(eui::Ui& ui, const core::theme::Tokens& t, float x,
                       float w, float y, float viewH);

    // 视图内部件
    void composeEngineCard(eui::Ui& ui, const core::theme::Tokens& t, float x,
                           float w, float y);
    void composeHealthGrid(eui::Ui& ui, const core::theme::Tokens& t, float x,
                           float w, float y);
    void composeFeed(eui::Ui& ui, const core::theme::Tokens& t, float x,
                     float w, float y);
    void composeProposalCard(eui::Ui& ui, const core::theme::Tokens& t,
                             float x, float w, float y,
                             const panel::PanelSnapshot& snap,
                             const proposals::Proposal& p, bool batchable);
    void composeDecidedCard(eui::Ui& ui, const core::theme::Tokens& t, float x,
                            float w, float y,
                            const panel::PanelSnapshot& snap,
                            const proposals::Proposal& p, bool undone);
    void composeEditDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& ct);
    void composeMaskModal(eui::Ui& ui, const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& ct);
    void composeClearModal(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& ct);
    void composeWizardModal(eui::Ui& ui, const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& ct);

    void showToast(const std::string& title, const std::string& body,
                   bool error = false);

    // 当前视图下标：0=主控台 1=记忆审核 2=配置
    int activeView_ = 0;

    // 数据中枢（面板库/设置/评测/引擎快照的唯一取数面）
    panel::PanelState panel_;
    ui_kit::ToastState toast_;

    // 各视图滚动偏移（settings 卡区同款手动偏移模式）
    float consoleScroll_ = 0;
    float reviewScroll_ = 0;
    float configScroll_ = 0;

    // 审核页：过滤 chips（下标即枚举序）/批量/展开/编辑
    int filterType_ = 0;   // 0=全部 1=合并 2=失效 3=遗忘 4=新增
    int filterSrc_ = 0;    // 0=全部 1=collect 2=整理 3=decay
    int filterState_ = 0;  // 0=待审 1=已处理
    bool batchMode_ = false;
    std::set<std::string> batchSel_;
    std::set<std::string> expanded_;
    std::string editingId_;  // 非空 = 编辑后接受弹窗打开
    std::string editText_;

    // 配置页弹窗态
    bool maskModalOpen_ = false;
    bool clearModalOpen_ = false;
    bool wizardOpen_ = false;
    std::string clearInput_;  // 输入「清除」解锁（设计稿契约）
};

} // namespace module::memory
