// 智能体页模块：生命周期 + UI（页头工具入口钮 / 任务列表 / 任务弹窗 / 详情弹窗 /
// 可用工具弹窗——池170，原页内工具区块已退役迁弹窗）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）；可点元素契约：id 元素
// 须带 text 与 onClick（button 的 onClick 落在 .bg 上）→ chip 手搓两层；弹窗字段标签
// 一律 .caption：dropdown 内部占用 <id>.label，同 id 会串位。

#include "agent_module.h"

#include "agent_module_internal.h"   // 拆分共享件：needsClaudePageHint / kModelSlot*（本 TU 消费方）

#include "module_agent/agent_runs_tool.h"

#include <algorithm>
#include <chrono>
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
#include <ui_kit/empty_state.h>
#include <ui_kit/grid.h>
#include <ui_kit/jelly.h>
#include <ui_kit/materials.h>
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
// 页头「可用工具」入口钮（池170）：工具区块退役后的唯一入口。宽不再固定
//（bug #6：原 132×40 次钮在工具数位数增长 9→12→108 时压叠图标/文本），
// 按 toolsEntryButtonWidth()（agent_ui_shared.h）流式自适应：图标 + 文字 +
// 计数 badge，右缘锚定、向左长（title 同步让宽）。
constexpr float kPageHeaderBtnGap = 8.0f;   // 设计 §3.1：两钮间距 8
// B3（F-66-2）任务卡网格化：单列每卡 = 内容宽 −2×inset，1600 窗下 1536×68 ≈ 22:1，
// 是横条不是卡。改多列网格锁 340–420 宽 × 120 高（≈3:1），窄窗自动退化为 1 列。
constexpr float kCardMinW = 340.0f;
constexpr float kCardMaxW = 420.0f;
constexpr int kCardMaxCols = 5;
constexpr float kTaskCardPad = 12.0f;
constexpr float kTaskCardInset = 10.0f;
constexpr float kTaskCardGap = 8.0f;
constexpr float kTaskCardH = 120.0f;   // 三行：22 + 18 + 18 + 行距 11×2 + 上下 pad 12×2
// 任务行「配置异常」标记宽：4 个 CJK 字 @12px = 48，留 8 余量（文案再长就换行/截断，
// 原因全文在详情弹窗配置摘要里）
constexpr float kTaskWarnW = 56.0f;
// hover 操作组（F-66-4）：五个 28×28 图标钮，间距 4 → 组宽 156。
// 组右缘必须 = 卡右缘 −14（探针 G2 断言），故 actionsX = cardW −14 − kTaskActionsW
constexpr float kTaskIconBtn = 28.0f;
constexpr float kTaskActionsW = kTaskIconBtn * 5.0f + 4.0f * 4.0f;
// 「启动」常驻主操作钮。原 UAT6 口径 56×32 五钮同排；B3 五钮转 hover 图标后，
// 行内只剩这一个文字钮，仍是 56×32（探针与既有契约按此验）
constexpr float kTaskActionW = 56.0f;
// 卡 bg 的果冻形变参数：复用 B2 默认档（jelly.h JellyOptions 默认值同值）
constexpr ui_kit::JellyOptions kJelly{};
constexpr float kPageErrorH = 16.0f;
// 页内错误行下的「去 Claude 页」引导行高（仅「LLM 配置不可用」类错误渲染）
constexpr float kPageHintH = 16.0f;

// 限次徽章文案（列表行内短标记，4 字以内）。
// runLimitText 在 agent_ui_shared.h——详情摘要与任务弹窗共用。
// 用「大白话」而不是 max_runs 术语——用户看到的是「一次性」不是「上限 1 次」。
// N > 1 只可能来自手改库 / 将来放开 N 次的 UI：文案跟着 N 走，不与「一次性」混
std::string runLimitBadge(int maxRuns) {
    if (maxRuns <= 1) return "一次性";
    return "限" + std::to_string(maxRuns) + "次";
}

// hover 操作组里的图标按钮（F-66-4b）：components::button 没有 zIndex，
// 而操作组按钮必须压在透明命中面之上才点得到——所以这里手搓两层
// （.bg 命中面 + 图标文本穿透），由外层 stack 的 zIndex 抬层级。
// onHover 必须由**真正接住指针的那个元素**上报（hover 全局唯一、只给最上层），
// 否则指针从卡身移到按钮上时卡会以为 hover 结束 → 材质/果冻闪一下。
void iconActionChip(::eui::Ui& ui, const std::string& id, unsigned int codepoint,
                    float size, const components::theme::ThemeColorTokens& tokens,
                    bool enabled, std::function<void()> onClick, float x,
                    std::function<void(bool)> onHover) {
    const ::core::Color base = tokens.surface;
    ::core::Color iconColor = tokens.text;
    if (!enabled) iconColor = components::theme::withOpacity(iconColor, 0.40f);
    // disabled 只置灰不摘 onClick：调用方/测试按 id 直调回调时靠回调内守卫
    //（与既有 actionChip 同口径，点击禁用钮无副作用），避免「元素在但不可点」的歧义
    auto bg = ui.rect(id + ".bg")
                  .position(x, 0.0f)
                  .size(size, size)
                  .states(base, components::theme::buttonHover(tokens, base),
                          components::theme::buttonPressed(tokens, base))
                  .radius(tokens.metrics.radius.overlay)
                  .border(components::theme::buttonBorder(tokens, false))
                  .disabled(!enabled)
                  .onClick(onClick);
    if (onHover) bg.onHover(onHover);
    bg.build();
    // 契约「id 元素带 onClick」（与 actionChip 同口径）：图标文本本身穿透命中
    //（hitTestMode None）
    ui.text(id)
        .position(x, 0.0f)
        .size(size, size)
        .icon(::eui::utf8(codepoint))
        .fontSize(size * 0.5f)
        .lineHeight(size)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(iconColor)
        .hitTestMode(::core::dsl::HitTestMode::None)
        .onClick(onClick)
        .build();
}

// 页头「可用工具」入口钮（池170 v2 §关键决策 3，bug #6）：图标 −6− 文字 −8−
// 计数 badge（primarySoftBg pill）。components::button 无 badge 槽位，按
// iconActionChip 同款手搓两层：.bg 命中面（三态/描边/禁用/onClick）+ 内容穿透。
// 宽度一律 toolsEntryButtonWidth()（agent_ui_shared.h）；badge 右缘贴按钮右内边距
// ——位数增长只向左扩，页头 title 同步让宽，按钮右缘锚定不变。
// 配色全走 token：次钮底 surface + buttonHover/Pressed（与 components 次钮同源）、
// badge 底 primarySoftBg / 字 primary（与 Outline 变体同配对）；禁用统一 40% 置灰。
void toolsEntryButton(::eui::Ui& ui, const std::string& id, int toolCount,
                      const components::theme::ThemeColorTokens& tokens,
                      bool enabled, std::function<void()> onClick) {
    const float w = toolsEntryButtonWidth(toolCount);
    const float h = 40.0f;
    const float font = core::theme::scaled(14.0f);
    const float iconW = core::theme::scaled(16.0f);
    const float textW = 4.0f * font;
    const float padX = core::theme::scaled(10.0f);
    // badge 宽由同一公式反解（右缘贴 padX），改宽公式只动 toolsEntryButtonWidth 一处
    const float badgeW = w - 2.0f * padX - iconW - 6.0f - textW - 8.0f;
    const float badgeH = core::theme::scaled(20.0f);
    ::core::Color fg = tokens.text;
    if (!enabled) fg = components::theme::withOpacity(fg, 0.40f);
    const ::core::Color base = tokens.surface;
    const std::string countText = std::to_string(toolCount < 0 ? 0 : toolCount);

    ui.stack(id)
        .size(w, h)
        .visualStateFrom(id + ".bg", 0.965f)  // 与 components::button pressScale 同档
        .content([&] {
            ui.rect(id + ".bg")
                .size(w, h)
                .states(base, components::theme::buttonHover(tokens, base),
                        components::theme::buttonPressed(tokens, base))
                .radius(6.0f)
                .border(components::theme::buttonBorder(tokens, false))
                .disabled(!enabled)
                .onClick(onClick)
                .build();
            ui.text(id + ".icon")
                .position(padX, 0.0f)
                .size(iconW, h)
                .icon(::eui::utf8(0xF1E6))  // fa-plug，与原 components 版同图标
                .fontSize(font)
                .lineHeight(font)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(fg)
                .build();
            ui.text(id + ".text")
                .position(padX + iconW + 6.0f, 0.0f)
                .size(textW, h)
                .text("可用工具")
                .fontSize(font)
                .lineHeight(font)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(fg)
                .build();
            ui.stack(id + ".badge")
                .position(w - padX - badgeW, (h - badgeH) * 0.5f)
                .size(badgeW, badgeH)
                .content([&] {
                    ui.rect(id + ".badge.pill")
                        .size(badgeW, badgeH)
                        .color(tokens.primarySoftBg)
                        .radius(badgeH * 0.5f)  // 全圆角 = pill
                        .build();
                    ui.text(id + ".badge.count")
                        .size(badgeW, badgeH)
                        .text(countText)
                        .fontSize(font)
                        .lineHeight(font)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(enabled ? tokens.primary
                                       : components::theme::withOpacity(
                                             tokens.primary, 0.40f))
                        .build();
                })
                .build();
        })
        .build();
}

// 卡内常驻主操作钮（「启动」）：Primary 底色 + 文字，56×32。文本节点沿用
// `<id>.text.normal` 命名，与 button 的逐态文本 id 契约保持一致。
// shortcut: 只做「常态/hover/pressed 三态 + disabled 置灰」，上限即本函数 ~30 行；
// 升级触发＝需要图标/按下缩放时改用 components::button（其 onHover 已可用，
// 原「.bg 抢走 hover 拿不到回调」障碍已消；升级仍有行为差异：按下缩放果冻为
// 新增、zIndex 需另行透传、文案 id 变为 button 逐态契约，升级时一并处理）。
void textActionChip(::eui::Ui& ui, const std::string& id, float x, float y,
                    float w, float h, const std::string& label, float fontSize,
                    const components::theme::ThemeColorTokens& tokens,
                    bool enabled, std::function<void()> onClick,
                    std::function<void(bool)> onHover, int zIndex) {
    const ::core::Color base = tokens.primary;
    const ::core::Color hover = components::theme::buttonHover(tokens, base);
    const ::core::Color pressed = components::theme::buttonPressed(tokens, base);
    const ::core::Color textColor =
        enabled ? ::core::Color{0.94f, 0.97f, 1.0f, 1.0f}
                : ::core::Color{0.94f, 0.97f, 1.0f, 0.40f};
    // 根 stack 保留 `<id>` 语义（可点 + 带文案）并承载 zIndex：命中面压在上面时
    // 靠它把整组抬到命中面之上（同 components::button 的「外层 stack 抬层级」口径）
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .zIndex(zIndex)
        .content([&] {
            auto bg = ui.rect(id + ".bg")
                          .size(w, h)
                          .states(base, hover, pressed)
                          .radius(tokens.metrics.radius.overlay)
                          .disabled(!enabled)
                          .onClick(onClick);
            if (onHover) bg.onHover(onHover);
            bg.build();
            ui.text(id + ".text.normal")
                .size(w, h)
                .text(label)
                .fontSize(fontSize)
                .lineHeight(h)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(textColor)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .onClick(onClick)
                .build();
        })
        .build();
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

    // agent.runs：运行历史只读查询（need 219 F-02）；status 缺省排除 running =
    // 自指排除。注册失败只留痕不中断，同批口径。
    runsTool_ = makeAgentRunsTool(*store_);
    auto regRuns = ::core::ToolRegistry::instance().registerTool(runsTool_);
    if (regRuns.isFail()) {
        LOG_ERROR("Agent: agent.runs 注册失败: " + regRuns.errorMessage());
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
    ::core::ToolRegistry::instance().unregisterTool("agent.runs");
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
    pressedMsgSeqs_.clear();
    toolsDialogOpen_ = false;
    toolsSearch_.clear();
    toolsExpandedNames_.clear();
    toolsPressedCards_.clear();
    toolsHoverCard_.clear();
    toolsSearchFocusPending_ = false;
    hoverHolder_.clear();
    hoverHideDueMs_.clear();
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
    // 池170：页内工具区块整体退役（迁弹窗），其高度预算归还任务列表。
    // 页 column 子项：标题行 / [错误行] / [引导行] / tasklist；子项数或行高一变，kPageGapCount 与减项必须同步
    const bool hasPageError = !pageError_.empty();
    const bool hasPageHint = hasPageError && needsClaudePageHint(pageError_);
    const int kPageGapCount = 1 + (hasPageError ? 1 : 0) + (hasPageHint ? 1 : 0);
    // 错误/引导行行盒随字号派生（B6）：行盒不缩 = 字变大被裁。
    const float pageErrorH = core::theme::scaled(kPageErrorH);
    const float pageHintH = core::theme::scaled(kPageHintH);
    // listH S4.2 弹性：底 padding 已为 0，高仅扣 pad 一次（顶内边距）
    const float listH = screen.height - pad - kPageHeaderH -
                        (hasPageError ? pageErrorH : 0.0f) -
                        (hasPageHint ? pageHintH : 0.0f) -
                        static_cast<float>(kPageGapCount) * t.sizes.spacingLarge;

    ui.column("agent.page")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)  // 上=0 标题顶格（2026-10-25 拍板）——EUI padding 序=(left,top,right,bottom)
        .gap(t.sizes.spacingLarge)
                .content([&] {
            ui.row("agent.page.header")
                .size(contentW, kPageHeaderH)
                .gap(kPageHeaderBtnGap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    // 入口钮宽随工具数位数流式生长（toolsEntryButtonWidth）；title
                    // 吃剩余宽 → 钮右缘锚定（不含新建任务时贴 contentW 右缘），位数
                    // 增长只把左缘向左推
                    const float toolsBtnW =
                        toolsEntryButtonWidth(static_cast<int>(toolsCache_.size()));
                    ui.text("agent.title")
                        .size(contentW - toolsBtnW - kPageHeaderBtnGap -
                                  (tasks_.empty() ? 0.0f
                                                  : kNewTaskBtnW + kPageHeaderBtnGap),
                              core::theme::scaled(32.0f))
                        .text("智能体")
                        .fontSize(t.sizes.fontSizeTitle)
                        .lineHeight(core::theme::scaled(32.0f))
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                    // 「可用工具」入口钮（池170 v2 §关键决策 3）：图标 + 文字 + 计数
                    // badge；store 未装配时禁用（40% 置灰）。弹窗打开时刷新注册表快照
                    toolsEntryButton(ui, "agent.page.tools",
                                     static_cast<int>(toolsCache_.size()),
                                     themeTokens, store_ != nullptr,
                                     [this] {
                                         // 池170 v2 §3.6：弹窗每次打开搜索态重置
                                         //（空输入 + 全量列表 + 卡片全部收起）
                                         toolsSearch_.clear();
                                         toolsExpandedNames_.clear();
                                         toolsHoverCard_.clear();
                                         toolsPressedCards_.clear();
                                         toolsSearchFocusPending_ = false;
                                         toolsDialogOpen_ = true;
                                         ensureToolsCache(/*forceReload=*/true);
                                     });
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
                    .size(contentW, pageErrorH)
                    .text(pageError_)
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(pageErrorH)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.errorText)
                    .build();
                // 「去 Claude 页」引导：仅「LLM 配置不可用」类错误渲染（needsClaudePageHint 前缀钉死）
                if (hasPageHint) {
                    ui.text("agent.page.error.hint")
                        .size(contentW, pageHintH)
                        .text("去 Claude 页 →")
                        .fontSize(core::theme::scaled(12.0f))
                        .lineHeight(pageHintH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.primary)
                        .onClick([] { navigateToClaudePage(); })
                        .build();
                }
            }

            // 任务列表面板（S6.1 实底白卡、玻璃退役）
            // 行级交互不变：点卡开详情、hover 加深一档；列表无选中
            // 语义（点行是开弹窗不是选中），不加绿圈白勾
            ui.stack("agent.tasklist.wrap")
                .size(contentW, std::max(0.0f, listH))
                .content([&] {
                    ui.rect("agent.tasklist.panel")
                        .position(0.0f, 0.0f)
                        .size(contentW, std::max(0.0f, listH))
                        .color(ui_kit::containerSurface(t))
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .build();
                    // 任务卡网格（2026-11-02 F-66-2）：多列流式网格取代单列列表——
                    // 1600 窗下单列 1536×68（22:1）是横条不是卡；网格锁 340–420×120
                    // （≈3:1）。外层 scrollView 保留做溢出滚动（卡数超面板高时可滚），
                    // 内层 flow 负责换行；step = 行距，一滚一格。
                    components::scrollView(ui, "agent.tasklist")
                        .position(kTaskCardInset, kTaskCardInset)
                        .size(std::max(0.0f, contentW - 2.0f * kTaskCardInset),
                              std::max(0.0f, listH - 2.0f * kTaskCardInset))
                        .gap(kTaskCardGap)
                        .step(core::theme::scaled(kTaskCardH) + kTaskCardGap)
                        .scrollbarOverlay()
                        .theme(themeTokens)
                        .contentKey("agent.tasklist.n" + std::to_string(tasks_.size()) +
                                    "." + std::to_string(static_cast<int>(contentW)))
                        .content([&](eui::Ui& inner, float cw, float) {
                    if (store_ == nullptr) {
                        inner.text("agent.error")
                            .size(cw, core::theme::scaled(20.0f))
                            .text("数据库未就绪（Agent 模块服务未装配）")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(core::theme::scaled(20.0f))
                            .color(t.colors.errorText)
                            .build();
                        return;
                    }
                    if (tasks_.empty()) {
                        // S2.8 空状态标准化：ui_kit::emptyState 极简文字 + 就近按钮（§4.12 单一规格，与 todo/schedule 同源）
                        // 2026-11-01 R3：补图标 + 引导句，空态形态全站统一。
                        constexpr float kEmptyWrapH = 200.0f;
                        inner.stack("agent.empty.wrap")
                            .width(cw)
                            .height(core::theme::scaled(kEmptyWrapH))
                            .content([&] {
                                ui_kit::EmptyStateOptions opts;
                                opts.message = "暂无任务";
                                opts.guide =
                                    "新建一个任务，让智能体按你的规则盯这件事。";
                                opts.icon = 0xF544;  // fa-robot
                                opts.action.emplace("新建任务",
                                                    [this] { openNewTaskDialog(); });
                                ui_kit::emptyState(inner, "agent.empty", opts, 0.0f,
                                                   0.0f, cw,
                                                   core::theme::scaled(kEmptyWrapH),
                                                   t);
                            })
                            .build();
                        return;
                    }
                    // 网格列宽：cw 是滚动内容宽（可能已扣滚动条），留 0.5 余量避免
                    // 恰好整除时浮点误差触发提前换行
                    const auto [gridCols, cardW] =
                        ui_kit::gridColumns(std::max(0.0f, cw - 0.5f), kTaskCardGap,
                                            kCardMinW, kCardMaxW, kCardMaxCols);
                    (void)gridCols;
                    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
                    const int64_t nowSec = core::time::nowUnix();
                    inner.flow("agent.tasklist.grid")
                        .width(cw)
                        .gap(kTaskCardGap)
                        .lineGap(kTaskCardGap)
                        .content([&] {
                    // bug169 §8 离场去抖账目时钟（steady ms，每帧取一次）
                    const int64_t hoverNowMs = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
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
                        // hover 态（F-66-4/5 + bug169 §8 状态机）：hover 目标全局唯一，
                        // 每卡记持有槽位；到期去抖账目先收割再读域。派发序实证（EUI
                        // runtime）：低 z 元素的 onHover(false) 可晚于高 z 元素的
                        // onHover(true)——单集合在按钮间缝隙穿越时被抹空（菜单即消失
                        // 的根因）；持有者槽位互检 + leave 只在「持有者=自己」时入
                        // 去抖账，任何派发序都收敛。
                        {
                            auto dueIt = hoverHideDueMs_.find(task.id);
                            if (dueIt != hoverHideDueMs_.end() &&
                                hoverNowMs >= dueIt->second) {
                                hoverHideDueMs_.erase(dueIt);
                                hoverHolder_.erase(task.id);
                            }
                        }
                        const bool hovered = hoverHolder_.count(task.id) > 0;
                        const bool pressed = pressedTaskIds_.count(task.id) > 0;

                        // 三行内容（F-66-3）：标题+状态药丸 / 徽章+最近运行 / 触发条件。
                        // 卡内一律绝对定位 → 必须包一层 stack 根，flow 会接管子元素 x/y。
                        // 三行行盒与卡高随字号成对派生（B6/F-69-2）：只缩字号不缩行盒
                        // = 字变大被裁；卡高不跟着缩放 = 第三行被挤出卡外。
                        const auto stTitle = core::theme::scaledText(14.0f, 22.0f);
                        const auto stSmall = core::theme::scaledText(11.0f, 18.0f);
                        const float cardPad = core::theme::scaled(kTaskCardPad);
                                                // bug154①：行距 11→12——22+18+32（启动钮）+ 行距 12×2
                        // + pad 12×2 = 120 正好填满卡高，消除行间/底部空白袋
                        const float rowGap = core::theme::scaled(12.0f);
                        const float cardH = core::theme::scaled(kTaskCardH);
                        const float pillTextW = core::TextPrimitive::measureTextWidth(
                            status, "", stSmall.fontSize, 500);
                        const float pillW = pillTextW + core::theme::scaled(30.0f);
                        const float titleW =
                            std::max(0.0f, cardW - cardPad * 2.0f -
                                               kTaskActionsW - 8.0f);
                        const float titleRowY = cardPad;
                        const float statusRowY =
                            titleRowY + stTitle.lineHeight + rowGap;
                        const float triggerRowY =
                            statusRowY + stSmall.lineHeight + rowGap;
                        const float actionsX = cardW - 14.0f - kTaskActionsW;
                        const float runW = kTaskActionW;
                        const float runX = cardW - cardPad - runW;

                        inner.stack(id)
                            .size(cardW, cardH)
                            .content([&] {
                                // 卡底（F-66-5 材质×果冻合体）：B1 三态材质吃 hover/press
                                // 变色，need201 v2 每帧写 radius/scale 目标值吃
                                // 压扁-拉伸-回弹时间线（jellyFrame 公开缝供值——
                                // 材质走 states 不能整卡换 jellyCard；kJelly 默认档
                                // radius=12 与缝的基线口径无缝）。pressed 优先级
                                // 维持 pressedRadius/pressedScale，时间线在缝内
                                // 后台推进不中断。
                                // ⚠️ states() 只改颜色改不了圆角，圆角/缩放必须每帧
                                // compose 出新值，动画只把上一帧的值平滑过去。
                                const eui::Color cardSurfaceColor =
                                    pressed ? ui_kit::itemSurfacePressed(t)
                                            : (hovered ? ui_kit::itemSurfaceHover(t)
                                                       : ui_kit::itemSurface(t));
                                const ui_kit::JellyPose jelly =
                                    ui_kit::jellyFrame(id, hovered);
                                auto cardBg = inner.rect(id + ".bg")
                                    .size(cardW, cardH)
                                    .states(cardSurfaceColor, cardSurfaceColor,
                                            cardSurfaceColor)
                                    .radius(pressed ? kJelly.pressedRadius
                                                    : jelly.radius)
                                    .transformOrigin(0.5f, 0.5f)
                                    .scale(pressed ? kJelly.pressedScale : jelly.sx,
                                           pressed ? kJelly.pressedScale : jelly.sy)
                                    .border(1.0f, t.colors.borderDefault)
                                    .shadow(components::theme::panelShadow(
                                        core::theme::componentTokensWeChat(t)))
                                    .transition(jelly.active
                                                    ? core::Transition::make(
                                                          0.035f, core::Ease::Linear)
                                                    : core::Transition::make(
                                                          0.18f, core::Ease::OutBack))
                                    .animate(core::AnimProperty::Color |
                                             core::AnimProperty::Radius |
                                             core::AnimProperty::Transform);
                                if (jelly.active) {
                                    cardBg.zIndex(20);  // 动画进行中盖过邻卡
                                }
                                cardBg.build();

                                // 行 1 左：任务名（悬停时让位给右侧操作组）
                                inner.text(id + ".title")
                                    .position(cardPad, titleRowY)
                                    .size(titleW, stTitle.lineHeight)
                                    .text(task.name)
                                    .fontSize(stTitle.fontSize)
                                    .lineHeight(stTitle.lineHeight)
                                    .fontWeight(600)
                                    .wrap(false)
                                    .maxWidth(titleW)
                                    .overflow(core::TextOverflow::Ellipsis)
                                    .color(taskOff
                                               ? components::theme::withOpacity(
                                                     t.colors.textPrimary, 0.45f)
                                               : t.colors.textPrimary)
                                    .build();
                                // 行 1 右：状态药丸（点 + 文案，软底）——hover 时让位
                                inner.rect(id + ".pill.bg")
                                    .position(cardW - cardPad - pillW,
                                              titleRowY + core::theme::scaled(2.0f))
                                    .size(pillW, stSmall.lineHeight)
                                    .radius(t.sizes.radiusPill)
                                    .color(components::theme::withOpacity(
                                        statusColor, 0.16f))
                                    .opacity(hovered ? 0.0f : 1.0f)
                                    .build();
                                inner.rect(id + ".status.dot")
                                    .position(cardW - cardPad - pillW +
                                                  core::theme::scaled(6.0f),
                                              titleRowY + core::theme::scaled(7.0f))
                                    .size(8.0f, 8.0f)
                                    .radius(8.0f)
                                    .color(statusColor)
                                    .opacity(hovered ? 0.0f : 1.0f)
                                    .build();
                                inner.text(id + ".status")
                                    .position(cardW - cardPad - pillW +
                                                  core::theme::scaled(18.0f),
                                              titleRowY + core::theme::scaled(2.0f))
                                    .size(std::max(0.0f,
                                                   pillW - core::theme::scaled(24.0f)),
                                          stSmall.lineHeight)
                                    .text(status)
                                    .fontSize(stSmall.fontSize)
                                    .lineHeight(stSmall.lineHeight)
                                    .fontWeight(500)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(statusColor)
                                    .opacity(hovered ? 0.0f : 1.0f)
                                    .build();
                                // 行 1 右：hover 操作组（F-66-4）。zIndex 2 高于命中面，
                                // 按钮才点得到；stack 必须给 size 才有布局盒。
                                // ⚠️ opacity 0 不挡命中（hitTest 不看 opacity）→ 必须配合
                                // disabled：否则卡右上角会藏着看不见的「删除」可点区。
                                inner.stack(id + ".actions")
                                    .position(actionsX,
                                              titleRowY +
                                                  (stTitle.lineHeight - 28.0f) *
                                                      0.5f)
                                    .size(kTaskActionsW, 28.0f)
                                    .zIndex(2)
                                    .opacity(hovered ? 1.0f : 0.0f)
                                    .disabled(!hovered)
                                    // bug169 §6 动效表：操作组显隐淡入/淡出 120ms
                                    .transition(core::Transition::make(
                                        0.12f, core::Ease::OutCubic))
                                    .animate(core::AnimProperty::Opacity)
                                    .content([&] {
                                        const float step = kTaskIconBtn + 4.0f;
                                        // 指针在按钮上 = 仍在联合命中域内（bug169 §8）：
                                        // enter 覆写持有槽位 + 清去抖账；leave 仅在
                                        // 持有者=自己时擦槽位 + 入 120ms 去抖账
                                        auto keepHover = [this,
                                                          taskId = task.id](
                                                             int slot) {
                                            return [this, taskId, slot](bool h) {
                                                // 时刻在回调现场取（帧期捕获的
                                                // nowMs 会因跨帧陈旧把去抖窗口算短）
                                                const int64_t nowMs =
                                                    std::chrono::duration_cast<
                                                        std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::
                                                            now()
                                                            .time_since_epoch())
                                                        .count();
                                                if (h) {
                                                    hoverHolder_[taskId] = slot;
                                                    hoverHideDueMs_.erase(taskId);
                                                } else {
                                                    // leave 不擦持有者（去抖窗口内
                                                    // 仍须显），只入账；到期由 compose
                                                    // 收割擦槽位。槽位互检挡陈旧 leave
                                                    auto hIt =
                                                        hoverHolder_.find(taskId);
                                                    if (hIt != hoverHolder_.end() &&
                                                        hIt->second == slot) {
                                                        hoverHideDueMs_.emplace(
                                                            taskId, nowMs + 120);
                                                    }
                                                }
                                            };
                                        };
                                        iconActionChip(inner, id + ".detail", 0xF05A,
                                                       kTaskIconBtn, themeTokens, true,
                                                       [this, taskId = task.id] {
                                                           openTaskDetail(taskId);
                                                       }, 0.0f, keepHover(1));
                                        iconActionChip(inner, id + ".edit", 0xF044,
                                                       kTaskIconBtn, themeTokens, true,
                                                       [this, taskId = task.id] {
                                                           openTaskEditor(taskId);
                                                       }, step, keepHover(2));
                                        iconActionChip(inner, id + ".toggle", 0xF011,
                                                       kTaskIconBtn, themeTokens, true,
                                                       [this, taskCopy = task] {
                                                           toggleTaskEnabled(taskCopy);
                                                       }, step * 2.0f, keepHover(3));
                                        // 「入队」对非常驻任务无意义（Scheduler 只对常驻
                                        // 派生子任务），非常驻/Scheduler 未装配 → 置灰；
                                        // 五钮恒渲染保操作组宽度恒定（G2 右缘对齐判据）
                                        iconActionChip(inner, id + ".enqueue", 0xF03A,
                                                       kTaskIconBtn, themeTokens,
                                                       resident && scheduler_ != nullptr,
                                                       [this, taskId = task.id] {
                                                           if (scheduler_ == nullptr) {
                                                               LOG_WARN("Agent: Scheduler 未"
                                                                        "就绪，常驻入队被忽略");
                                                               return;
                                                           }
                                                           scheduler_->enqueueResident(taskId);
                                                       }, step * 3.0f, keepHover(4));
                                        iconActionChip(inner, id + ".delete", 0xF2ED,
                                                       kTaskIconBtn, themeTokens, !runBusy,
                                                       [this, taskId = task.id, busy = runBusy] {
                                                           if (busy) return;   // 禁用态点击无效果（与入队 chip 同守卫法）
                                                           deleteConfirmTaskId_ = taskId;
                                                       }, step * 4.0f, keepHover(5));
                                    })
                                    .build();

                                // 行 2：徽章组（配置异常 / 限次 / 已停用）+ 最近运行时间右对齐
                                float badgeX = cardPad;
                                auto badge = [&](const std::string& suffix,
                                                 const std::string& text,
                                                 const eui::Color& color) {
                                    inner.text(id + suffix)
                                        .position(badgeX, statusRowY)
                                        .size(kTaskWarnW, stSmall.lineHeight)
                                        .text(text)
                                        .fontSize(stSmall.fontSize)
                                        .lineHeight(stSmall.lineHeight)
                                        .color(color)
                                        .build();
                                    badgeX += kTaskWarnW + core::theme::scaled(6.0f);
                                };
                                if (hasProblem) {
                                    badge(".warn", "配置异常", t.colors.errorText);
                                }
                                if (limited) {
                                    badge(".limit", runLimitBadge(limitIt->second),
                                          t.colors.warning);
                                }
                                if (taskOff) {
                                    badge(".off", "已停用", t.colors.textTertiary);
                                }
                                const std::string lastRunText =
                                    it == latestRuns_.end()
                                        ? std::string("未运行过")
                                        : "最近运行 " + relativeTimeText(
                                                            it->second.startedAt, nowSec);
                                // 起点避开左侧徽章（badgeX 已被上面的徽章推进），
                                // 否则三枚徽章齐挂时右对齐文字会压到徽章上
                                const float lastRunX = badgeX;
                                const float lastRunW = std::max(
                                    0.0f, cardW - cardPad - lastRunX);
                                inner.text(id + ".lastrun")
                                    .position(lastRunX, statusRowY)
                                    .size(lastRunW, stSmall.lineHeight)
                                    .text(lastRunText)
                                    .fontSize(stSmall.fontSize)
                                    .lineHeight(stSmall.lineHeight)
                                    .wrap(false)
                                    .maxWidth(lastRunW)
                                    .overflow(core::TextOverflow::Ellipsis)
                                    .horizontalAlign(eui::HorizontalAlign::Right)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 行 3：触发条件（cron 原文 / 常驻 / 一次性类型标签），
                                // 右侧给常驻「启动」让位
                                const std::string cron = cronTextOf(task);
                                std::string triggerText =
                                    cron.empty() ? triggerLabelOf(task.triggerType)
                                                 : cron;
                                inner.text(id + ".trigger")
                                    .position(cardPad, triggerRowY)
                                    .size(std::max(0.0f, runX - cardPad - 8.0f),
                                          stSmall.lineHeight)
                                    .text(triggerText)
                                    .fontSize(stSmall.fontSize)
                                    .lineHeight(stSmall.lineHeight)
                                    .wrap(false)
                                    .maxWidth(std::max(0.0f, runX - cardPad - 8.0f))
                                    .overflow(core::TextOverflow::Ellipsis)
                                    .color(t.colors.textSecondary)
                                    .build();

                                // 「启动」常驻主操作（不随 hover；Primary 表达主次），zIndex 2
                                // 压在命中面之上。手搓而非 components::button 的取舍见
                                // textActionChip shortcut 注释（其 onHover 障碍已消，
                                // 剩余行为差异：果冻新增/zIndex 透传/文案 id 契约）。
                                textActionChip(
                                    inner, id + ".run", runX,
                                    triggerRowY - core::theme::scaled(7.0f), runW,
                                    core::theme::scaled(32.0f),
                                    runStarted ? "运行中…" : "启动",
                                    core::theme::scaled(12.0f), themeTokens,
                                    loop_ != nullptr && !runStarted,
                                    [this, taskId = task.id] {
                                        startTask(taskId);
                                        // 启动 run 后立即开详情弹窗，随 uiPolling_ 实时更新
                                        openTaskDetail(taskId);
                                    },
                                    [this, taskId = task.id](bool h) {
                                        if (h) {
                                            hoverHolder_[taskId] = 6;   // 启动钮槽位
                                            hoverHideDueMs_.erase(taskId);
                                        } else {
                                            auto hIt = hoverHolder_.find(taskId);
                                            if (hIt != hoverHolder_.end() &&
                                                hIt->second == 6) {
                                                // 同上：leave 只入账不擦持有者
                                                hoverHideDueMs_.emplace(
                                                    taskId,
                                                    std::chrono::duration_cast<
                                                        std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::
                                                            now()
                                                            .time_since_epoch())
                                                        .count() +
                                                        120);
                                            }
                                        }
                                    },
                                    2);

                                // 命中面（F-66-4a）：铺满整卡、三态同色、zIndex 1（高于卡内容、
                                // 低于操作组/启动），写 hover/press 集合 + 点击开详情。
                                // 卡底压在命中面下拿不到 hover（hover 全局唯一给最上层），
                                // 所以 B1 三态材质由这里代报：press 由 onPress/onRelease 维护。
                                inner.rect(id + ".hit")
                                    .position(0.0f, 0.0f)
                                    .size(cardW, kTaskCardH)
                                    .zIndex(1)
                                    .states(kTransparent, kTransparent, kTransparent)
                                    .onHover([this, taskId = task.id](bool h) {
                                        if (h) {
                                            hoverHolder_[taskId] = 0;   // 命中面槽位
                                            hoverHideDueMs_.erase(taskId);
                                        } else {
                                            auto hIt = hoverHolder_.find(taskId);
                                            if (hIt != hoverHolder_.end() &&
                                                hIt->second == 0) {
                                                // 同上：leave 只入账不擦持有者
                                                hoverHideDueMs_.emplace(
                                                    taskId,
                                                    std::chrono::duration_cast<
                                                        std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::
                                                            now()
                                                            .time_since_epoch())
                                                        .count() +
                                                        120);
                                            }
                                        }
                                    })
                                    .onPress([this, taskId = task.id](const core::PointerEvent&,
                                                                      const core::Rect&) {
                                        pressedTaskIds_.insert(taskId);
                                    })
                                    .onRelease([this, taskId = task.id](const core::PointerEvent&,
                                                                        const core::Rect&) {
                                        pressedTaskIds_.erase(taskId);
                                    })
                                    .onClick([this, taskId = task.id] {
                                        openTaskDetail(taskId);
                                    })
                                    .build();
                            })
                            .build();
                    }
                        })   // agent.tasklist.grid
                        .build();
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
    if (toolsDialogOpen_) {
        composeToolsDialog(ui, screen, t, themeTokens);
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
    if (toolsDialogOpen_) {
        // 池170 v2 §3.6 Esc 优先级：搜索框有输入先清搜索（弹窗不关，焦点回输入框，
        // compose 帧内 requestFocus），空输入才关弹窗。输入框聚焦时 Esc 在组件内
        // 消费（onCancel 同语义），本分支兜的是焦点在卡面/页面的情况。
        if (!toolsSearch_.empty()) {
            toolsSearch_.clear();
            toolsSearchFocusPending_ = true;
            return true;
        }
        toolsDialogOpen_ = false;   // 工具弹窗与页面弹窗互不并存（backdrop 挡页）
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
