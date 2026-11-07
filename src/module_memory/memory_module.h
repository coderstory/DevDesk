// src/module_memory/memory_module.h
// 记忆页模块：telemem 记忆引擎托管面板三视图（主控台 / 记忆审核 / 配置）。
// 数据全部经 panel::PanelState 取数/操作（面板库随 DEVDESK_DATA_ROOT 隔离）；
// 引擎托管逻辑在 engine/ 子层，本类只做 UI 编排。密钥/记忆正文不进日志。
#pragma once

#include <array>
#include <memory>
#include <set>
#include <string>

#include "core/module_interface.h"
#include "core/tool.h"
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

    // 需-9 用量告警判定驱动（装配层每帧调，M3 pollRunEnd 同款）：判定与投递
    // 不依赖记忆页可见；出口在 PanelState（toast 桥 + platform::notify）。
    void pollUsageAlerts();

    // 测试/探针缝：视图切换不经点击注入（probe 直驱）
    void setActiveViewForTest(int view) { activeView_ = view; }
    // 测试/探针缝：配置折叠组展开态直驱（140% 档组头会被推到视口外，
    // 点击注入不可达——probe 组 C 用它展开后再断言组内控件）
    void setConfigGroupOpenForTest(int gi, bool v) {
        if (gi >= 0 && gi < static_cast<int>(cfgOpen_.size()))
            cfgOpen_[static_cast<std::size_t>(gi)] = v;
    }
    // 测试/探针缝：审核视图滚动偏移直驱（bug190 探针要把视口滚到渐进加载
    // 尾行才能点「再显示 50 条」——按钮内容位深在 50 卡 × 244px ≈ 12k px）
    void setReviewScrollForTest(float v) { reviewScroll_ = v; }

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
    // need:181 三 TAB 分层（2026-11-07 设计 §2.6/§3）：引擎详情弹窗 + 审核卡
    // 全文只读弹窗（#183：多行只读滚动可复制，改动不落状态）
    void composeEngineDetailModal(eui::Ui& ui, const eui::Screen& screen,
                                  const core::theme::Tokens& t,
                                  const components::theme::ThemeColorTokens& ct);
    void composeFullTextDialog(eui::Ui& ui, const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& ct);

    void showToast(const std::string& title, const std::string& body,
                   bool error = false);

    // 当前视图下标：0=主控台 1=记忆审核 2=配置
    int activeView_ = 0;

    // 数据中枢（面板库/设置/评测/引擎快照的唯一取数面）
    panel::PanelState panel_;
    ui_kit::ToastState toast_;

    // agent 只读诊断工具（need 219 F-03/F-04）：onStart 注册 / onStop 注销，
    // 跟随模块启用（拍板 3）；路径取 engine::memoryDataDir()，不依赖页面开过
    std::shared_ptr<::core::ITool> memStatsTool_;
    std::shared_ptr<::core::ITool> memHealthTool_;
    bool agentToolsRegistered_ = false;

    // 各视图滚动偏移（settings 卡区同款手动偏移模式）
    float consoleScroll_ = 0;
    float reviewScroll_ = 0;
    float configScroll_ = 0;

    // 审核页：过滤 chips（下标即枚举序）/批量/展开/编辑
    int filterType_ = 0;   // 0=全部 1=合并 2=失效 3=遗忘 4=新增
    int filterSrc_ = 0;    // 0=全部 1=collect 2=整理 3=decay
    int filterState_ = 0;  // 0=待审 1=已处理
    // 待审卡流渐进加载上限（bug190，2026-11-04）：无上限逐卡渲染时单帧
    // compose 的元素数与文本度量量随 pending 线性膨胀（真实库曾 1630 条
    // pending → ~2.1 万元素 + 数百万 CJK 字形度量/帧，主线程被单帧 compose
    // 饿死 = UI 冻结）。默认渲染前 50 张，卡片流尾「再显示 50 条」按需增长；
    // 筛选条件变化时重置。
    static constexpr int kReviewShowLimit = 50;
    int reviewShowLimit_ = kReviewShowLimit;
    bool batchMode_ = false;
    std::set<std::string> batchSel_;
    std::string editingId_;  // 非空 = 编辑后接受弹窗打开（全文出口，bug234）
    std::string editText_;

    // need:181 三 TAB 分层状态：主控台「运行明细」折叠区（默认收起，L3）/
    // 引擎详情弹窗（L3）/ 配置页三折叠组（0 提炼端点 1 触发线 2 用量告警）
    bool consoleDetailsOpen_ = false;
    bool engineDetailOpen_ = false;
    std::array<bool, 3> cfgOpen_ = {};
    // need:183：审核筛选同类收进下拉（类型/来源），开合态显式托管
    bool rvTypeDdOpen_ = false;
    bool rvSrcDdOpen_ = false;
    // need:183：查看全文只读弹窗（多行滚动可复制；onChange 不挂 = 只读）
    bool textDialogOpen_ = false;
    std::string textDialogTitle_;
    std::string textDialogBody_;

    // 配置页弹窗态
    bool maskModalOpen_ = false;
    bool clearModalOpen_ = false;
    bool wizardOpen_ = false;
    std::string clearInput_;  // 输入「清除」解锁（设计稿契约）
};

} // namespace module::memory
