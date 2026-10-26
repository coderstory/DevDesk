// 智能体页模块：生命周期 + UI（工具区 / 任务列表 / 任务弹窗 / 详情弹窗）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）；可点元素契约：id 元素
// 须带 text 与 onClick（button 的 onClick 落在 .bg 上）→ chip 手搓两层；弹窗字段标签
// 一律 .caption：dropdown 内部占用 <id>.label，同 id 会串位。

#include "agent_module.h"

#include "agent_module_internal.h"   // 拆分共享件：needsClaudePageHint / kModelSlot*（本 TU 消费方）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <system_error>
#include <utility>

#include <components/button.h>
#include <components/dialog.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/segmented.h>
#include <components/switch.h>
#include <core/window/window_backend.h>
#include <core/platform/platform.h>
#include <eui/dsl.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>
#include <ui_kit/button.h>
#include <ui_kit/card.h>
#include <ui_kit/wswitch.h>
#include <ui_kit/toast.h>

#include "model_slots.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/time.h"
#include "platform/notification.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_time_text.h"
#include "module_agent/agent_builtin_tools.h"
#include "module_agent/app_notify_tool.h"
#include "module_agent/app_now_tool.h"
#include "module_agent/app_stats_tool.h"
#include "module_agent/cron.h"
#include "module_agent/hot_tool.h"
#include "module_agent/message_blocks.h"
#include "module_agent/net_fetch_tool.h"
#include "module_agent/task_draft_parse.h"
#include "module_agent/tool_info.h"
#include "module_agent/trigger_config.h"
#include "module_agent/agent_system_prompt.h"
#include "module_agent/agent_ui_shared.h"

namespace module::agent {

// 共享 UI helper（状态色/actionChip/导航/cronText/runLimitText/弹窗布局常量）
// 收在 module_agent/agent_ui_shared.h，与本 TU 的 agent_detail_dialog.cpp 共用真源。
// 本匿名空间保留本 TU 独有的助手。

namespace {

constexpr float kPageHeaderH = 40.0f;
constexpr float kNewTaskBtnW = 140.0f;
constexpr float kTaskRowH = 44.0f;
// UAT6 任务卡片化（用户真机反馈：列表 item 卡片展示、内容四边加内边距）：
// kTaskCardPad = 卡内内容四边内边距 12；kTaskCardInset = 卡列表在灰面板内的
// 四边留边 10（卡宽 = 列表宽 − 2×inset，弹性契约：无固定帽，随窗口缩放）；
// kTaskCardGap = 卡间距 8；kTaskCardH = 卡高 = 行高 + 上下内边距（step 同源，
// 一滚一格）
constexpr float kTaskCardPad = 12.0f;
constexpr float kTaskCardInset = 10.0f;
constexpr float kTaskCardGap = 8.0f;
constexpr float kTaskCardH = kTaskRowH + 2.0f * kTaskCardPad;
// 任务行「配置异常」标记宽：4 个 CJK 字 @12px = 48，留 8 余量（文案再长就换行/截断，
// 原因全文在详情弹窗配置摘要里）
constexpr float kTaskWarnW = 56.0f;
// UAT6 真机反馈「启动按钮比其他按钮大」的统一规格。差异源：原启动钮走
// components::button 派生字号 h×0.46≈14.7 且宽 72，其余 chip 字号 12、宽 48/56。
// 行内操作钮（详情/编辑/启停/删除/[入队]/启动）统一 56×32、字号 12——同排
// 同尺寸同级；「启动」的主操作语义用 Primary 色同尺寸表达，不用大一号。
constexpr float kTaskActionW = 56.0f;
constexpr float kPageErrorH = 16.0f;
// 页内错误行下的「去 Claude 页」引导行高（仅「LLM 配置不可用」类错误渲染）
constexpr float kPageHintH = 16.0f;

constexpr float kToolsHeaderH = 28.0f;
constexpr float kToolCardH = 40.0f;
// 工具列表高锚点 = 640 档（S4.1 最小窗 1000×640）下的列表高，也是弹性下限。
// H18-批3 起不再封顶：高窗按 body 可用高放大（公式见 compose 内 toolsListH）
constexpr float kToolsListAnchorH = 240.0f;

// 限次徽章文案（列表行内短标记，4 字以内）。
// runLimitText 在 agent_ui_shared.h——详情摘要与任务弹窗共用。
// 用「大白话」而不是 max_runs 术语——用户看到的是「一次性」不是「上限 1 次」。
// N > 1 只可能来自手改库 / 将来放开 N 次的 UI：文案跟着 N 走，不与「一次性」混
std::string runLimitBadge(int maxRuns) {
    if (maxRuns <= 1) return "一次性";
    return "限" + std::to_string(maxRuns) + "次";
}

} // namespace


AgentModule::AgentModule(Deps d) : deps_(d) {}

AgentModule::~AgentModule() {
    // 兜底：正常路径 onStop 已清空 runs_。只收线程不注销工具（工具可能仍被 run 线程的 shared_ptr 持有）
    for (auto& r : runs_) {
        r->cancel = true;
    }
    for (auto& r : runs_) {
        if (r->thread.joinable()) r->thread.join();
    }
    // AI 草稿线程同理先收（unique_ptr 析构遇 joinable thread 会 terminate）
    if (draftJob_) {
        if (draftJob_->thread.joinable()) draftJob_->thread.join();
        draftJob_.reset();
    }
    // OS 通知发送线程同口径收尾（onStop 漏调时的兜底，同款 terminate 风险）
    for (auto& item : notifySends_) {
        if (item->thread.joinable()) item->thread.join();
    }
    // 试运行线程兜底收尾
    if (dryRun_) {
        dryRun_->cancel.store(true);
        if (dryRun_->thread.joinable()) dryRun_->thread.join();
        dryRun_.reset();
    }
}

void AgentModule::onStart() {
    // 重入幂等：双 onStart 会重建服务、重复注册工具，清扫还会误伤重入期间新出现的 running run
    if (started_) return;
    started_ = true;
    // GitHub Sync 拉取会 DROP 重建整个库：订阅导入成功事件，导入线程同步回调里只置
    // 原子脏标记（不得摸 UI 线程成员），compose 收割置 tasksDirty_ 走既有重查路径。
    // 闸 = 存活守卫：onStop 先关闸再退订，挡在途 handler。
    dbImportHandlersEnabled_.store(true, std::memory_order_release);
    dbImportedSub_ = ::core::event::bus().subscribe(
        ::core::event::kDbImported, [this](const std::string&) {
            if (dbImportHandlersEnabled_.load(std::memory_order_acquire)) {
                dbImportedDirty_.store(true, std::memory_order_release);
            }
        });
    core::data::SqliteDb* db = deps_.db ? deps_.db : ::core::appDatabase();
    if (db == nullptr) {
        LOG_WARN("Agent: 数据库尚未注册，跳过服务装配");
        return;
    }
    store_ = std::make_unique<AgentStore>(*db);

    // 上次会话悬挂的 running run 清成 failed（异常退出恢复口径）
    auto swept = store_->sweepStaleRuns();
    if (swept.isFail()) {
        LOG_WARN("Agent: 悬挂 run 清扫失败: " + swept.errorMessage());
    }

    // 无服务 → 只 WARN，store/工具照常装配、手动启动禁用；首次使用时会再调本入口（见 ensureLlm）
    ensureLlm();

    // 注册自带工具（name 冲突属装配 bug，日志留痕不中断）
    statsTool_ = makeAppStatsTool(*db);
    auto reg = ::core::ToolRegistry::instance().registerTool(statsTool_);
    if (reg.isFail()) {
        LOG_ERROR("Agent: app.stats 注册失败: " + reg.errorMessage());
    }

    // app.notify 的系统通知失败降级：AgentLoop 工作线程调用，只做线程安全入队，
    // 由装配层每帧 takeRunEndNotice() 取走后经 app::showAppNotice 发 toast（不碰 UI）。
    notifyTool_ = std::make_shared<NotifyTool>([this](std::string text) {
        std::lock_guard<std::mutex> lock(pendingNoticesMu_);
        pendingNotices_.emplace_back("应用内提醒", std::move(text));
    });
    auto regNotify = ::core::ToolRegistry::instance().registerTool(notifyTool_);
    if (regNotify.isFail()) {
        LOG_ERROR("Agent: app.notify 注册失败: " + regNotify.errorMessage());
    }

    // hot.list：热榜聚合。默认构造 = 生产 fetcher/now；缓存与限频自持，无需模块状态。
    // 注册失败只留痕不中断，与 stats/notify 同口径。
    hotTool_ = std::make_shared<HotListTool>();
    auto regHot = ::core::ToolRegistry::instance().registerTool(hotTool_);
    if (regHot.isFail()) {
        LOG_ERROR("Agent: hot.list 注册失败: " + regHot.errorMessage());
    }

    // app.now：本地时间快照。纯读、无状态；注册失败只留痕不中断，同批口径。
    nowTool_ = makeAppNowTool();
    auto regNow = ::core::ToolRegistry::instance().registerTool(nowTool_);
    if (regNow.isFail()) {
        LOG_ERROR("Agent: app.now 注册失败: " + regNow.errorMessage());
    }

    // net.fetch：联网 GET 抓取（外部请求，仅 GET，10s 超时；单次调用语义，
    // 不做重放/并发限制）。非破坏性只读；注册失败只留痕不中断，同批口径。
    netTool_ = std::make_shared<NetFetchTool>();
    auto regNet = ::core::ToolRegistry::instance().registerTool(netTool_);
    if (regNet.isFail()) {
        LOG_ERROR("Agent: net.fetch 注册失败: " + regNet.errorMessage());
    }

    // 内置工具 14 件（UAT6）：fs.* / bash.* / git.* 集中注册，名称冲突只留痕不中断
    registerAgentBuiltinTools();

    tasksDirty_ = true;
    LOG_INFO("Agent 模块启动");
}

void AgentModule::onStop() {
    // 重入幂等：未启动 no-op。顺序：置取消 → join → 清容器 → 注销工具 → 放服务（注销须晚于线程退出）
    if (!started_) return;
    // 先关原子闸再退订：闸挡在途 handler，退订挡后续 emit。
    dbImportHandlersEnabled_.store(false, std::memory_order_release);
    dbImportedSub_ = {};
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        for (auto& r : runs_) {
            r->cancel = true;
        }
    }
    // join 不持锁：run 线程不碰 runsMu_，持锁会白堵并发访问者
    for (auto& r : runs_) {
        if (r->thread.joinable()) r->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        runs_.clear();
    }
    // AI 草稿生成线程：请求不可中断，join 等自然结束（DirectLlmClient 120s 超时兜底）。
    // join 必须先于 llm_.reset()——worker 持有 *llm_（同 runs_ 的「全 join 再放服务」序）
    if (draftJob_) {
        if (draftJob_->thread.joinable()) draftJob_->thread.join();
        draftJob_.reset();
    }
    // 试运行线程收尾：取消 + join（必须在 loop_/llm_ reset 之前——线程持有引用）
    if (dryRun_) {
        dryRun_->cancel.store(true);
        if (dryRun_->thread.joinable()) dryRun_->thread.join();
        dryRun_.reset();
    }
    testRunResult_.clear();
    // OS 通知发送线程收尾：等在飞的 send 结束（一次性，最坏 ≈ 后端阻塞上限），
    // 不留线程越过本函数——unique_ptr 析构遇 joinable thread 会 terminate
    {
        std::vector<std::unique_ptr<NotifySend>> pending;
        {
            std::lock_guard<std::mutex> lock(notifySendsMu_);
            pending.swap(notifySends_);
        }
        for (auto& item : pending) {
            if (item->thread.joinable()) item->thread.join();
        }
    }
    draftDialogOpen_ = false;
    draftPhase_ = DraftPhase::Idle;
    draftInput_.clear();
    draftNotice_.clear();
    draftWarnings_.clear();
    draftError_.clear();
    // Scheduler 收口：先退订事件（断 emit→onEvent 入队源），再 stopAll 停派发源并收在跑 run
    eventSubs_.clear();
    if (scheduler_) scheduler_->stopAll();
    ::core::ToolRegistry::instance().unregisterTool("app.notify");
    ::core::ToolRegistry::instance().unregisterTool("hot.list");
    ::core::ToolRegistry::instance().unregisterTool("app.stats");
    ::core::ToolRegistry::instance().unregisterTool("app.now");
    ::core::ToolRegistry::instance().unregisterTool("net.fetch");
    loop_.reset();
    llm_.reset();
    store_.reset();
    scheduler_.reset();
    notifyTool_.reset();
    hotTool_.reset();
    statsTool_.reset();
    nowTool_.reset();
    netTool_.reset();
    llmMissingWarned_ = false;
    tasks_.clear();
    latestRuns_.clear();
    taskRuns_.clear();
    runSummaries_.clear();
    runSummariesError_.clear();
    runSummariesSeenVersionValid_ = false;
    runSummariesSeenWindowStart_ = -1;
    detailMessages_.clear();
    detailMessagesError_.clear();
    detailPlan_.reset();
    detailPlanKey_.clear();
    nextRunText_.reset();
    nextRunMinute_ = -1;
    nextRunTaskId_ = 0;
    nextRunLatestRunId_ = 0;
    nextRunLatestStatus_.clear();
    detailTaskId_.reset();
    detailTaskRow_.reset();
    selectedRunId_.reset();
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    taskDialogOpen_ = false;
    editingTaskId_.reset();
    editError_.clear();
    pageError_.clear();
    uiPolling_ = false;
    started_ = false;
    LOG_INFO("Agent 模块停止");
}

void AgentModule::onPageEnter() {
    tasksDirty_ = true;
}

void AgentModule::compose(::eui::Ui& ui, const ::eui::Screen& screen) {
    // AI 草稿生成收割：worker 置 done 后 requestUiUpdate 主动要帧，这里每帧先收
    //（join 已完成线程 → 预填 / 报错 / 丢弃过期结果），再画 UI。
    reapDraftJob();
    // 库被 GitHub Sync 导入覆盖 → 任务/运行缓存指向旧行；置脏走既有重查路径。
    if (dbImportedDirty_.exchange(false, std::memory_order_acq_rel)) {
        tasksDirty_ = true;
    }
    const auto& t = ::core::theme::current();
    const auto themeTokens = ::core::theme::componentTokensWeChat(t);
    const float pad = t.sizes.spacingLarge * 2.0f;  // 32 —— S4.2 弹性内边距（spacingLarge=16×2=32）
    const float contentW = screen.width - pad * 2.0f;   // 宽 = 窗口宽 − 双侧内边距

    // 页面帧惰性解析：否则 loop_ 恒空、「启动」永久禁用，惰性解析在真实 UI 上不可达
    if (store_ != nullptr && loop_ == nullptr && tasksDirty_) {
        ensureLlm();
    }

    // 数据刷新：脏标记或有 run 在身（每帧跟进）；查询在主线程，量级可控
    if (store_ != nullptr && (tasksDirty_ || uiPolling_)) {
        refreshData();
    }

    ensureToolsCache(/*forceReload=*/false);
    // 工具列表纵向弹性（H18-批3 / S4.1「锚点 1000×640 + 伸缩公式」）：
    //   toolsListH = min(max(240 锚点, body 可用高 × 0.3), 内容自然高)
    // ① body 可用高 = screen.height − pad：与下方 listH 同口径（页 column 底 padding
    //    为 0，只在顶部扣一次 pad），即工具区所在 body 的真实可用高度；
    // ② 内容自然高 = 行数 × kToolCardH：沿用既有算式（scrollView 无额外内边距，
    //    卡间 gap 已含在 step 节奏里）；
    // ③ 240 = 最小窗锚点（原封顶值）：大窗按真实页高 960 算 (960−32)×0.3 = 278.4
    //    > 240，工具区随窗拉高；×0.2 时 928×0.2 = 185.6 会被锚点封死，故取 0.3
    //   （最小窗 body 可用高 568×0.3 = 170.4 仍落 240，两档同时成立）。
    const float toolsBodyAvailH = screen.height - pad;
    const float toolsNaturalH =
        static_cast<float>(toolsCache_.size()) * kToolCardH;
    const float toolsListH =
        toolsOpen_ ? std::min(std::max(kToolsListAnchorH, toolsBodyAvailH * 0.3f),
                              toolsNaturalH)
                   : 0.0f;
    const float toolsH = kToolsHeaderH + (toolsOpen_ ? kMsgGap + toolsListH : 0.0f);
    // 页 column 子项：标题行 / [错误行] / [引导行] / tools / tasklist；子项数或行高一变，kPageGapCount 与减项必须同步
    const bool hasPageError = !pageError_.empty();
    const bool hasPageHint = hasPageError && needsClaudePageHint(pageError_);
    const int kPageGapCount = 2 + (hasPageError ? 1 : 0) + (hasPageHint ? 1 : 0);
    // listH S4.2 弹性：底 padding 已为 0，高仅扣 pad 一次（顶内边距）
    const float listH = screen.height - pad - kPageHeaderH - toolsH -
                        (hasPageError ? kPageErrorH : 0.0f) -
                        (hasPageHint ? kPageHintH : 0.0f) -
                        static_cast<float>(kPageGapCount) * t.sizes.spacingLarge;

    ui.column("agent.page")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)  // 上=0 标题顶格（2026-10-25 拍板）——EUI padding 序=(left,top,right,bottom)
        .gap(t.sizes.spacingLarge)
                .content([&] {
            ui.row("agent.page.header")
                .size(contentW, kPageHeaderH)
                .gap(kDialogGap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    ui.text("agent.title")
                        .size(tasks_.empty() ? contentW : (contentW - kNewTaskBtnW - kDialogGap), 32.0f)
                        .text("智能体")
                        .fontSize(t.sizes.fontSizeTitle)
                        .lineHeight(32.0f)
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                    if (!tasks_.empty()) {
                        components::button(ui, "agent.page.newTask")
                            .size(kNewTaskBtnW, 40.0f)
                            .text("+ 新建任务")
                            .theme(themeTokens)
                            .disabled(store_ == nullptr)
                            .onClick([this] { openNewTaskDialog(); })
                            .build();
                    }
                })
                .build();

            // 页内错误行（有错才渲染）：弹窗之外失败的唯一可见出口
            if (hasPageError) {
                ui.text("agent.page.error")
                    .size(contentW, kPageErrorH)
                    .text(pageError_)
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(kPageErrorH)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.errorText)
                    .build();
                // 「去 Claude 页」引导：仅「LLM 配置不可用」类错误渲染（needsClaudePageHint 前缀钉死）
                if (hasPageHint) {
                    ui.text("agent.page.error.hint")
                        .size(contentW, kPageHintH)
                        .text("去 Claude 页 →")
                        .fontSize(core::theme::scaled(12.0f))
                        .lineHeight(kPageHintH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.primary)
                        .onClick([] { navigateToClaudePage(); })
                        .build();
                }
            }

            // 可用工具区块：收起默认、头部点击展开；卡多了由 scrollView 内滚
            ui.column("agent.tools")
                .size(contentW, toolsH)
                .gap(kMsgGap)
                .content([&] {
                    ui.row("agent.tools.header")
                        .size(contentW, kToolsHeaderH)
                        .interactive(true)
                        .gap(8.0f)
                        .alignItems(eui::Align::CENTER)
                        .onClick([this] {
                            toolsOpen_ = !toolsOpen_;
                            if (toolsOpen_) {
                                ensureToolsCache(/*forceReload=*/true);
                            }
                        })
                        .content([&] {
                            ui.text("agent.tools.header.label")
                                .size(contentW - 72.0f, 20.0f)
                                .text("可用工具（" +
                                      std::to_string(toolsCache_.size()) + "）")
                                .fontSize(core::theme::scaled(13.0f))
                                .lineHeight(core::theme::scaled(20.0f))
                                .fontWeight(700)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.text("agent.tools.header.hint")
                                .size(64.0f, 20.0f)
                                .text(toolsOpen_ ? "收起 ▲" : "展开 ▼")
                                .fontSize(core::theme::scaled(12.0f))
                                .lineHeight(core::theme::scaled(20.0f))
                                .color(t.colors.textTertiary)
                                .build();
                        })
                        .build();
                    if (toolsOpen_ && !toolsCache_.empty()) {
                        // 工具列表容器：实底白卡面板（S6.1 玻璃退役）
                        ui.stack("agent.tools.list.wrap")
                            .size(contentW, toolsListH)
                            .content([&] {
                                ui.rect("agent.tools.list.panel")
                                    .position(0.0f, 0.0f)
                                    .size(contentW, toolsListH)
                                    .color(ui_kit::cardSurface(t))
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                components::scrollView(ui, "agent.tools.scroll")
                                    .position(0.0f, 0.0f)
                                    .size(contentW, toolsListH)
                                    .gap(kMsgGap)
                                    .step(kToolCardH)
                                    .theme(themeTokens)
                                    .contentKey("agent.tools.scroll.n" +
                                                std::to_string(toolsCache_.size()))
                                    .content([&](eui::Ui& inner, float cw, float) {
                                        for (size_t k = 0; k < toolsCache_.size(); ++k) {
                                            const ToolInfo& tool = toolsCache_[k];
                                            const std::string cid =
                                                "agent.tools.card." + std::to_string(k);
                                            inner.stack(cid)
                                                .size(cw, kToolCardH)
                                                .content([&] {
                                                    // 白卡背景（S6.1 实底）
                                                    inner.rect(cid + ".bg")
                                                        .position(0.0f, 0.0f)
                                                        .size(cw, kToolCardH)
                                                        .color(ui_kit::cardSurface(t))
                                                        .radius(t.sizes.radius)
                                                        .border(1.0f, t.colors.borderDefault)
                                                        .build();
                                                    // 行1：工具名（14px 600 一级字）
                                                    inner.text(cid + ".name")
                                                        .position(12.0f, 3.0f)
                                                        .size(cw - 24.0f, 17.0f)
                                                        .text(tool.name)
                                                        .fontSize(core::theme::scaled(14.0f))
                                                        .lineHeight(core::theme::scaled(17.0f))
                                                        .fontWeight(600)
                                                        .wrap(false)
                                                        .color(t.colors.textPrimary)
                                                        .build();
                                                    // 行2：描述+参数（12px 三级字）
                                                    std::string descShort;
                                                    if (!tool.description.empty())
                                                        descShort = utf8Truncate(tool.description, 80);
                                                    std::string paramsText;
                                                    if (!tool.params.empty()) {
                                                        paramsText = "参数：";
                                                        for (size_t p = 0; p < tool.params.size(); ++p) {
                                                            if (p > 0) paramsText += ", ";
                                                            paramsText += tool.params[p];
                                                        }
                                                    }
                                                    std::string line2;
                                                    if (!descShort.empty() && !paramsText.empty())
                                                        line2 = descShort + " · " + paramsText;
                                                    else if (!descShort.empty())
                                                        line2 = descShort;
                                                    else if (!paramsText.empty())
                                                        line2 = paramsText;
                                                    else
                                                        line2 = "（无描述）";
                                                    inner.text(cid + ".desc")
                                                        .position(12.0f, 22.0f)
                                                        .size(cw - 24.0f, 15.0f)
                                                        .text(line2)
                                                        .fontSize(core::theme::scaled(12.0f))
                                                        .lineHeight(core::theme::scaled(15.0f))
                                                        .wrap(false)
                                                        .color(t.colors.textTertiary)
                                                        .build();
                                                })
                                        .build();
                                }
                            })
                            .build();
                            })   // agent.tools.list.wrap 的 content 收口
                            .build();
                        // 滚动提示：工具数多于可见行数时显示"还有 N 件"。
                        // 可见行数按本帧弹性列表高推（H18-批3：窗口拉高后可见行变多，
                        // 提示要跟着变，不能再用固定 240 锚点算）
                        if (!toolsCache_.empty()) {
                            const float visibleRows = toolsListH / kToolCardH;
                            const int total = static_cast<int>(toolsCache_.size());
                            const int visible = static_cast<int>(visibleRows);
                            if (total > visible) {
                                ui.text("agent.tools.scrollHint")
                                    .size(contentW, 16.0f)
                                    .text("还有 " + std::to_string(total - visible) + " 件")
                                    .fontSize(core::theme::scaled(11.0f))
                                    .lineHeight(core::theme::scaled(16.0f))
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();
                            }
                        }
                    }
                })
                .build();

            // 任务列表面板（S6.1 实底白卡、玻璃退役）
            // 行级交互不变：点卡开详情、hover 加深一档；列表无选中
            // 语义（点行是开弹窗不是选中），不加绿圈白勾
            ui.stack("agent.tasklist.wrap")
                .size(contentW, std::max(0.0f, listH))
                .content([&] {
                    ui.rect("agent.tasklist.panel")
                        .position(0.0f, 0.0f)
                        .size(contentW, std::max(0.0f, listH))
                        .color(ui_kit::cardSurface(t))
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .build();
                    // 卡片列表：面板内四边留 kTaskCardInset，卡宽 = 列表宽 − 边距；
                    // 卡高 kTaskCardH（step 同源）；行内动作：启动/详情/编辑/禁用（常驻多一个入队）
                    components::scrollView(ui, "agent.tasklist")
                        .position(kTaskCardInset, kTaskCardInset)
                        .size(std::max(0.0f, contentW - 2.0f * kTaskCardInset),
                              std::max(0.0f, listH - 2.0f * kTaskCardInset))
                        .gap(kTaskCardGap)
                        .step(kTaskCardH)
                        .theme(themeTokens)
                        .contentKey("agent.tasklist.n" + std::to_string(tasks_.size()))
                        .content([&](eui::Ui& inner, float cw, float) {
                    if (store_ == nullptr) {
                        inner.text("agent.error")
                            .size(cw, 20.0f)
                            .text("数据库未就绪（Agent 模块服务未装配）")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(20.0f)
                            .color(t.colors.errorText)
                            .build();
                        return;
                    }
                    if (tasks_.empty()) {
                        // S2.8 空状态标准化：图标 + 标题 + 说明 + 操作按钮
                        const float chipW = 120.0f, chipH = 40.0f;
                        inner.text("agent.empty.icon")
                            .size(cw, 24.0f)
                            .text("+")
                            .fontSize(core::theme::scaled(20.0f))
                            .lineHeight(core::theme::scaled(24.0f))
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        inner.text("agent.empty.title")
                            .size(cw, 24.0f)
                            .text("暂无任务")
                            .fontSize(core::theme::scaled(16.0f))
                            .lineHeight(core::theme::scaled(24.0f))
                            .fontWeight(600)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textPrimary)
                            .build();
                        inner.text("agent.empty.desc")
                            .size(cw, 20.0f)
                            .text("创建智能任务，让 AI 自动执行工作流")
                            .fontSize(core::theme::scaled(12.0f))
                            .lineHeight(core::theme::scaled(20.0f))
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        inner.stack("agent.empty.btn.chip")
                            .size(chipW, chipH)
                            .content([&] {
                                inner.rect("agent.empty.btn.bg")
                                    .size(chipW, chipH)
                                    .states(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                                    .radius(t.sizes.radius)
                                    .onClick([this] { openNewTaskDialog(); })
                                    .build();
                                inner.text("agent.empty.btn")
                                    .size(chipW, chipH)
                                    .text("创建第一个任务")
                                    .fontSize(core::theme::scaled(14.0f))
                                    .lineHeight(core::theme::scaled(40.0f))
                                    .fontWeight(600)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.onPrimary)
                                    .hitTestMode(core::dsl::HitTestMode::None)
                                    .onClick([this] { openNewTaskDialog(); })
                                    .build();
                            })
                            .build();
                        return;
                    }
                    for (const auto& task : tasks_) {
                        const std::string id =
                            "agent.task." + std::to_string(task.id);
                        std::string status = "—";
                        eui::Color statusColor = t.colors.textTertiary;
                        auto it = latestRuns_.find(task.id);
                        if (it != latestRuns_.end()) {
                            status = runStatusText(it->second.status);
                            statusColor = statusDotColor(it->second.status, t);
                        }
                        // 运行中禁再启动（防双起 run）：startTask 的并发闸只
                        // LOG 静默返回，按钮态必须先于点击反映真实可用性
                        const bool runStarted =
                            it != latestRuns_.end() &&
                            it->second.status == "running";
                        // 宽度分配（卡内行）：状态 80（圆点 8 + gap 12 + 文字 60）/
                        // 行内操作钮统一 kTaskActionW×32（详情·编辑·启停·删除·
                        // 启动 + [常驻入队]，UAT6 同排同尺寸）/ [配置异常·限次·
                        // 已停用 各 kTaskWarnW]；childCount 须与实际子元素数一致
                        const bool resident = task.triggerType == "resident";
                        const bool taskOff = !task.enabled;
                        const bool hasProblem =
                            taskTriggerProblems_.find(task.id) !=
                            taskTriggerProblems_.end();
                        const auto limitIt = taskRunLimits_.find(task.id);
                        const bool limited = limitIt != taskRunLimits_.end();
                        // 运行中禁删（E1）：DB running 行或在跑 ActiveRun 任一即算——删除必须
                        // 「确认即终局」，先停后删会把破坏性动作拆成难收口的中间态
                        const bool runBusy =
                            isTaskRunActive(task.id) || runStarted;
                        const float rowGap = 12.0f;
                        const int childCount = 8 + (resident ? 1 : 0) +
                                               (hasProblem ? 1 : 0) +
                                               (limited ? 1 : 0) +
                                               (taskOff ? 1 : 0);
                        const float fixedW = 80.0f + kTaskActionW *
                                                         (5.0f + (resident ? 1.0f : 0.0f)) +
                                             (hasProblem ? kTaskWarnW : 0.0f) +
                                             (limited ? kTaskWarnW : 0.0f) +
                                             (taskOff ? kTaskWarnW : 0.0f);
                        const float rowW = cw - 2.0f * kTaskCardPad;
                        const float titleW =
                            rowW - fixedW - rowGap * static_cast<float>(childCount - 1);
                        // S6.1 实底白卡（玻璃退役）：白底 + borderDefault 细边 + radius 圆角
                        // 上层 row 内缩 kTaskCardPad 四边内边距只做布局；非交互的
                        // row 不拦指针，子 chip 各自消费点击
                        inner.stack(id)
                            .size(cw, kTaskCardH)
                            .content([&] {
                                inner.rect(id + ".bg")
                                    .position(0.0f, 0.0f)
                                    .size(cw, kTaskCardH)
                                    .states(t.colors.surface,
                                            t.colors.hover,
                                            t.colors.primarySoftBg)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this, taskId = task.id] {
                                        openTaskDetail(taskId);
                                    })
                                    .build();
                                inner.row(id + ".row")
                            .position(kTaskCardPad, kTaskCardPad)
                            .size(rowW, kTaskRowH)
                            .alignItems(eui::Align::CENTER)
                            .gap(rowGap)
                            .content([&] {
                                inner.text(id + ".title")
                                    .size(titleW, 20.0f)
                                    .text(task.name)
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .color(taskOff
                                               ? components::theme::withOpacity(
                                                     t.colors.textPrimary, 0.45f)
                                               : t.colors.textPrimary)
                                    .build();
                                // 配置异常标记：Scheduler 每跑必跳过的任务在这里必须显形
                                //（点行本体开详情，原因在配置摘要里；标记不消费点击）
                                if (hasProblem) {
                                    inner.text(id + ".warn")
                                        .size(kTaskWarnW, 20.0f)
                                        .text("配置异常")
                                        .fontSize(core::theme::scaled(12.0f))
                                        .lineHeight(core::theme::scaled(20.0f))
                                        .color(t.colors.errorText)
                                        .build();
                                }
                                // 限次标记：看得出「这是一次性任务」（跑满会自动停用）
                                if (limited) {
                                    inner.text(id + ".limit")
                                        .size(kTaskWarnW, 20.0f)
                                        .text(runLimitBadge(limitIt->second))
                                        .fontSize(core::theme::scaled(12.0f))
                                        .lineHeight(core::theme::scaled(20.0f))
                                        .color(t.colors.textTertiary)
                                        .build();
                                }
                                // 已停用徽章：与标题置灰双通道并存（启用后两者同消）
                                if (taskOff) {
                                    inner.text(id + ".off")
                                        .size(kTaskWarnW, 20.0f)
                                        .text("已停用")
                                        .fontSize(core::theme::scaled(12.0f))
                                        .lineHeight(core::theme::scaled(20.0f))
                                        .color(t.colors.textTertiary)
                                        .build();
                                }
                                inner.rect(id + ".status.dot")
                                    .size(8.0f, 8.0f)
                                    .radius(8.0f)
                                    .color(statusColor)
                                    .build();
                                inner.text(id + ".status")
                                    .size(60.0f, 20.0f)
                                    .text(status)
                                    .fontSize(core::theme::scaled(12.0f))
                                    .lineHeight(core::theme::scaled(20.0f))
                                    .color(t.colors.textSecondary)
                                    .build();
                                actionChip(inner, id + ".detail", "详情", kTaskActionW,
                                           /*primary=*/false, /*enabled=*/true,
                                               themeTokens, [this, taskId = task.id] {
                                                   openTaskDetail(taskId);
                                               });
                                actionChip(inner, id + ".edit", "编辑", kTaskActionW,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this, taskId = task.id] {
                                               openTaskEditor(taskId);
                                           });
                                actionChip(inner, id + ".toggle",
                                           task.enabled ? "禁用" : "启用", kTaskActionW,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this, taskCopy = task] {
                                               toggleTaskEnabled(taskCopy);
                                           });
                                actionChip(inner, id + ".delete", "删除", kTaskActionW,
                                           /*primary=*/false, /*enabled=*/!runBusy,
                                           themeTokens,
                                           [this, taskId = task.id, busy = runBusy] {
                                               if (busy) return;   // 禁用态点击无效果（与入队 chip 同守卫法）
                                               deleteConfirmTaskId_ = taskId;
                                           });
                                if (resident) {
                                    // Scheduler 未装配时置灰；入队跑手动子任务
                                    actionChip(inner, id + ".enqueue", "入队", kTaskActionW,
                                               /*primary=*/false,
                                               /*enabled=*/scheduler_ != nullptr,
                                               themeTokens, [this, taskId = task.id] {
                                                   if (scheduler_ == nullptr) {
                                                       LOG_WARN("Agent: Scheduler 未"
                                                                "就绪，常驻入队被忽略");
                                                       return;
                                                   }
                                                   scheduler_->enqueueResident(taskId);
                                               });
                                }
                                // 启动 = 行内唯一主操作：Primary 绿表达主次，几何
                                // 与其他 chip 同规格（kTaskActionW×32、字号 12——
                                // UAT6 真机反馈「启动比其他按钮大」的统一收口）
                                components::button(inner, id + ".run")
                                    .size(kTaskActionW, 32.0f)
                                    .text(runStarted ? "运行中…" : "启动")
                                    .fontSize(core::theme::scaled(12.0f))
                                    .theme(themeTokens)
                                    .disabled(loop_ == nullptr || runStarted)
                                    .onClick([this, taskId = task.id] {
                                        startTask(taskId);
                                        // 启动 run 后立即开详情弹窗，随 uiPolling_ 实时更新
                                        openTaskDetail(taskId);
                                    })
                                    .build();
                            })
                            .build();
                            })   // stack content
                            .build();
                    }
                })
                .build();
                })   // agent.tasklist.wrap 的 content 收口
                .build();
        })
        .build();

    if (taskDialogOpen_) {
        composeTaskDialog(ui, screen, t, themeTokens);
    }
    if (taskDialogOpen_ && draftDialogOpen_) {
        composeDraftDialog(ui, screen, t, themeTokens);
    }
    if (detailTaskId_) {
        composeTaskDetailDialog(ui, screen, t, themeTokens);
    }
    if (deleteConfirmTaskId_) {
        composeDeleteConfirmDialog(ui, screen, t, themeTokens);
    }
    // 页级复制 toast 最后画（spec §7.3：z 1100 > dialog 1000，盖在弹窗之上）；
    // 每帧无条件调用，显隐由 toast_.open 控制
    ui_kit::drawToast(ui, screen, "agent.detail.toast", toast_, themeTokens);
}

// 详情弹窗布局壳（UAT6 方案 A）：弹性宽 max(720, 窗口/2) + 头部（任务名/状态/动作
// chips）→ 摘要三卡（常驻横排）→ 顶部 segment 三档（执行记录|运行详情|任务配置）→
// 单列 tab 内容 → 底部 关闭|编辑任务。所有区域坐标从实际钳制后的 w/h 推导，禁用屏幕
// 绝对坐标；消息区默认折叠（messagesOpen_），复制反馈走页级 toast
bool AgentModule::onKeyEvent(const ::eui::KeyEvent& event) {
    if (!event.isDown()) return false;
    if (event.key != eui::InputKey::Escape) return false;
    if (deleteConfirmTaskId_) {
        deleteConfirmTaskId_.reset();   // 确认弹窗是最上层：Esc 只关它，不碰底下弹窗
        return true;
    }
    if (detailTaskId_) {
        closeTaskDetail();
        return true;
    }
    if (taskDialogOpen_ && draftDialogOpen_) {
        closeDraftDialog();   // Esc 关最上层：先子弹窗，外层留待下一次 Esc
        return true;
    }
    if (taskDialogOpen_) {
        closeTaskDialog();
        return true;
    }
    return false;
}
std::string AgentModule::iconGlyph() const {
    return ::eui::utf8(0xF544);  // fa-robot
}

} // namespace module::agent

extern "C" ::core::Module* create_agent_module() {
    // 生产装配：Deps 全空，onStart 内回退 appDatabase / ServiceLocator
    return new module::agent::AgentModule(module::agent::AgentModule::Deps{});
}
