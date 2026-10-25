// 智能体页模块：生命周期 + UI（工具区 / 任务列表 / 任务弹窗 / 详情弹窗）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）；可点元素契约：id 元素
// 须带 text 与 onClick（button 的 onClick 落在 .bg 上）→ chip 手搓两层；弹窗字段标签
// 一律 .caption：dropdown 内部占用 <id>.label，同 id 会串位。

#include "agent_module.h"

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

bool hasLiveRun(const std::map<int64_t, AgentRunRow>& latestRuns,
                const std::set<int64_t>& orphanedRunIds) {
    for (const auto& [taskId, run] : latestRuns) {
        (void)taskId;
        if (run.status != "running") continue;
        // 孤儿 = 收尾写库失败永停 running；按 run.id（非 map 键 taskId）比对，FinishFailureFn 回报的是 runId
        if (orphanedRunIds.count(run.id) != 0) continue;
        return true;
    }
    return false;
}

bool needsClaudePageHint(const std::string& errorText) {
    // 前缀必须与 direct_llm_client.cpp 的 kConfigErrorPrefix 原文逐字一致（见头文件注释）
    return errorText.rfind("LLM 配置不可用", 0) == 0;
}

std::vector<int64_t> detectRunEndNotifications(
    const std::map<int64_t, std::string>& prevStatus,
    const std::map<int64_t, std::string>& nowStatus,
    const std::set<int64_t>& notifiedRunIds) {
    std::vector<int64_t> toNotify;
    for (const auto& [runId, status] : nowStatus) {
        const auto prevIt = prevStatus.find(runId);
        if (prevIt == prevStatus.end() || prevIt->second != "running") continue;
        if (status != "failed" && status != "limited") continue;
        if (notifiedRunIds.count(runId) != 0) continue;
        toNotify.push_back(runId);
    }
    return toNotify;   // map 迭代升序，无需再排
}

void pruneNotifiedRunIds(const std::map<int64_t, std::string>& window,
                         std::set<int64_t>& notifiedRunIds) {
    for (auto it = notifiedRunIds.begin(); it != notifiedRunIds.end();) {
        if (window.count(*it) == 0) {
            it = notifiedRunIds.erase(it);   // 掉出窗口：不会再被观测到
        } else {
            ++it;
        }
    }
}

// 共享 UI helper（状态色/actionChip/导航/cronText/runLimitText/弹窗布局常量）
// 收在 module_agent/agent_ui_shared.h，与本 TU 的 agent_detail_dialog.cpp 共用真源。
// 本匿名空间保留本 TU 独有的助手。

namespace {

// 弹窗字段标签：显式垂直居中（光靠 lineHeight 不居中）+ 显式定位；id 用 .caption 避开 dropdown 的 .label
void fieldCaption(::eui::Ui& ui, const std::string& id, const std::string& text,
                  float x, float y, float width, float height,
                  const ::core::Color& color) {
    ui.text(id)
        .position(x, y)
        .size(width, height)
        .text(text)
        .fontSize(12.0f)
        .lineHeight(20.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(color)
        .build();
}

// 消息收起态预览：解码纯文本前 120 字节，统一走 message_blocks
std::string messagePreview(const AgentMessageRow& m) {
    return messagePreviewFromJson(m.contentJson, 120);
}

// 任务名派生：goal 前 24 字节（UTF-8 边界回退）、换行压空格；goal 非空故结果非空
std::string deriveTaskName(const std::string& goal) {
    constexpr size_t kMaxNameBytes = 24;
    std::string flat;
    flat.reserve(goal.size());
    for (const char c : goal) {
        flat += (c == '\n' || c == '\r') ? ' ' : c;
    }
    if (flat.size() <= kMaxNameBytes) return flat;
    size_t cut = kMaxNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(flat[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return flat.substr(0, cut) + "…";
}

// 真源 = src/model_slots.h 的 X 宏表，新增槽位只改那里；键集进库（agent_tasks.model_slot），显示名只上屏
#define DEVDESK_MODEL_SLOT_KEY(name, jsonKey, label, formLabel, envKey, nameKey, oneM) jsonKey,
constexpr const char* kModelSlotKeys[] = {DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_KEY)};
#undef DEVDESK_MODEL_SLOT_KEY
#define DEVDESK_MODEL_SLOT_LABEL(name, jsonKey, label, formLabel, envKey, nameKey, oneM) label,
constexpr const char* kModelSlotLabels[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_LABEL)};
#undef DEVDESK_MODEL_SLOT_LABEL
constexpr int kModelSlotCount = static_cast<int>(
    sizeof(kModelSlotKeys) / sizeof(kModelSlotKeys[0]));

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

// 任务弹窗几何（UAT5 重设计：弹性宽 = max(720, 窗口/2)，demo 智能体面板的紧凑行高）
// kDialogGap/kDialogLabelW/kDialogRowH 已收进 agent_ui_shared.h（与详情弹窗共用真源）
constexpr float kTaskDialogH = 520.0f;        // UAT6 竖向导航案：标题栏/底栏各 44 带 + 内容区 432
constexpr float kGoalInputH = 64.0f;          // demo 指令 textarea rows≈3
constexpr float kGroupTitleH = 20.0f;        // 分组标题高（14px 加粗 + 行高 20）

// AI 草稿子弹窗几何（Standard 460 宽；y 均为本弹窗面板内坐标，与任务弹窗互不影响）。
// UAT6 重排：输入加高 88→140、结果行让位下移、按钮组迁底部按钮带（行顶 y=h-60、
// 行底距弹窗底 20，与 dialog_form 同口径）并整体居中。
constexpr float kDraftDialogH = 380.0f;
constexpr float kDraftInputH = 140.0f;
constexpr float kDraftBtnH = 40.0f;          // 底部按钮带按钮高（dialog_form 同档）
constexpr float kDraftBtnW = 110.0f;
constexpr float kDraftBtnGap = 12.0f;
constexpr float kDraftResultY = 244.0f;      // 结果行顶（输入底 232 + 12 呼吸）

// 任务列表每任务只查最近 1 条（列表状态/通知足够，spec §8.6 D8）。
// kDetailRunListLimit（=20）在 agent_ui_shared.h——列表查询与详情 UI 共用同一上限
constexpr int kRunListLimit = 1;

constexpr float kToolsHeaderH = 28.0f;
constexpr float kToolCardH = 40.0f;
// 工具列表高锚点 = 640 档（S4.1 最小窗 1000×640）下的列表高，也是弹性下限。
// H18-批3 起不再封顶：高窗按 body 可用高放大（公式见 compose 内 toolsListH）
constexpr float kToolsListAnchorH = 240.0f;

int slotIndexOf(const std::string& slotKey) {
    for (int i = 0; i < kModelSlotCount; ++i) {
        if (slotKey == kModelSlotKeys[i]) return i;
    }
    return 0;
}

// 迭代上限容错解析：非数字忽略（输入中间态）、>1000 截断防溢出、无数字 → 0（保存时 clamp）
int parseIterInput(const std::string& text) {
    int value = 0;
    bool any = false;
    for (const char c : text) {
        if (c < '0' || c > '9') continue;
        any = true;
        value = value * 10 + (c - '0');
        if (value > 1000) return 1000;
    }
    return any ? value : 0;
}

// 限次徽章文案（列表行内短标记，4 字以内）。
// runLimitText 在 agent_ui_shared.h——详情摘要与任务弹窗共用。
// 用「大白话」而不是 max_runs 术语——用户看到的是「一次性」不是「上限 1 次」。
// N > 1 只可能来自手改库 / 将来放开 N 次的 UI：文案跟着 N 走，不与「一次性」混
std::string runLimitBadge(int maxRuns) {
    if (maxRuns <= 1) return "一次性";
    return "限" + std::to_string(maxRuns) + "次";
}

// 闹钟时间文本 ↔ epoch 的纯函数集（trimCopy / parseOnceAtLocalText /
// formatOnceAtText / defaultOnceAtText / onceTextOf）已搬至 task_draft_parse.h：
// 草稿解析层（task_draft.cpp）需要同一份闹钟校验，留在本文件私有会造成
// 「模型草稿说合法、UI 说非法」的双真源。搬移是行为等价的，判定规则与文案逐字未改。


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

// 收割已结束的 run：摘除后 join，避免「同 task 不并发」判定被已完成 run 误伤。仅主线程调用。
// join 在锁外（与 onStop 同纪律）：锁内摘出、锁外 join——join 耗时期间不该堵死
// hasActiveRun/isTaskRunActive 等并发访问者
void AgentModule::reapFinishedRuns() {
    std::vector<std::unique_ptr<ActiveRun>> finished;
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        for (auto it = runs_.begin(); it != runs_.end();) {
            if ((*it)->done.load()) {
                finished.push_back(std::move(*it));
                it = runs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& run : finished) {
        if (run->thread.joinable()) run->thread.join();
    }
}

bool AgentModule::hasActiveRun(int64_t taskId) {
    std::lock_guard<std::mutex> lock(runsMu_);
    for (auto& r : runs_) {
        if (r->taskId == taskId && !r->done.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

// 锁内置位：ActiveRun 指针不出 runsMu_（erase 与解引用之间无锁隔断即 use-after-free）
void AgentModule::requestStopRun(int64_t taskId) {
    std::lock_guard<std::mutex> lock(runsMu_);
    for (auto& r : runs_) {
        if (r->taskId == taskId && !r->done.load(std::memory_order_acquire)) {
            r->cancel.store(true, std::memory_order_release);
            LOG_INFO("Agent: 已请求停止任务 " + std::to_string(taskId) +
                     " 的 run（当前轮结束后生效）");
            return;
        }
    }
    LOG_WARN("Agent: 停止请求落空（任务 " + std::to_string(taskId) +
             " 无在跑 run），忽略");
}

bool AgentModule::isTaskRunActive(int64_t taskId) {
    return hasActiveRun(taskId);
}

bool AgentModule::isStopRequested(int64_t taskId) {
    std::lock_guard<std::mutex> lock(runsMu_);
    for (auto& r : runs_) {
        if (r->taskId == taskId && !r->done.load(std::memory_order_acquire)) {
            return r->cancel.load(std::memory_order_acquire);
        }
    }
    return false;
}

// OS 通知发送线程收割：锁内摘出 done 的、锁外 join（join 立即返回，不堵帧）。
// 在飞的留在表里下帧再收；onStop/析构才等在飞的（一次性，换来无 detach 的退出期安全）
void AgentModule::reapNotifySends() {
    std::vector<std::unique_ptr<NotifySend>> finished;
    {
        std::lock_guard<std::mutex> lock(notifySendsMu_);
        for (auto it = notifySends_.begin(); it != notifySends_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = notifySends_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& item : finished) {
        if (item->thread.joinable()) item->thread.join();
    }
}

// 惰性装配单入口：启动时 claude 关、会话内拨开无须重载本模块。幂等：loop_ 非空即返回；
// scheduler_ 须在此与 loop_ 同建（否则惰性路径下 cron 永不触发）
void AgentModule::ensureLlm() {
    if (loop_ != nullptr) return;       // 已就绪（llm_/loop_/scheduler_ 三者同生共死）
    if (store_ == nullptr) return;
    // 只解析配置来源（激活 profile），不碰代理运行态：代理开不开与智能体能不能跑无关
    core::LlmProfileSource* source = deps_.profileSource;
    if (source == nullptr) {
        auto located =
            ::core::ServiceLocator::requireService<::core::LlmProfileSource>();
        if (located.isOk()) source = located.value();
    }
    if (source == nullptr) {
        // 每段「无服务」只提示一次——本函数被页面帧调用，不节流会刷屏
        if (!llmMissingWarned_) {
            llmMissingWarned_ = true;
            LOG_WARN("Agent: Claude 配置来源未装配，LLM 通道不可用"
                     "（请在设置页开启 Claude 模块后重试）");
        }
        return;
    }
    llm_ = std::make_unique<DirectLlmClient>(*source, deps_.http);
    loop_ = std::make_unique<AgentLoop>(*llm_, ::core::ToolRegistry::instance(),
                                        *store_, AgentLoop::NowMsFn{},
                                        [this](int64_t runId, std::string reason) {
                                            // 收尾写库失败 → 记孤儿供轮询判停排除（否则 uiPolling_ 永不复位）
                                            std::lock_guard<std::mutex> lock(runsMu_);
                                            orphanedRunIds_.insert(runId);
                                            LOG_ERROR("Agent: run " + std::to_string(runId) +
                                                      " 收尾写库失败，已记为孤儿 run（"
                                                      "不计入 UI 轮询判停）：" + reason);
                                        });
    if (scheduler_ == nullptr) {
        scheduler_ = std::make_unique<Scheduler>(*store_, *loop_, deps_.schedulerNow);
        // 事件触发接线：订阅 core EventBus 三事件 → Scheduler::onEvent。emit 在 UI
        // 主线程同步执行，onEvent 契约「轻入队」不阻塞 UI。token 存 eventSubs_（RAII），
        // onStop 清空即退订——漏退订会让 bus() 单例持有悬垂 this 的回调
        auto& bus = ::core::event::bus();
        eventSubs_.clear();
        for (const char* ev : kEventNames) {
            eventSubs_.push_back(bus.subscribe(ev, [this](const std::string& name) {
                if (scheduler_) scheduler_->onEvent(name);
            }));
        }
        scheduler_->start();
    }
    llmMissingWarned_ = false;
    LOG_INFO("Agent: LLM 通道就绪（惰性解析）");
}

void AgentModule::startTask(int64_t taskId) {
    ensureLlm();   // 首次使用路径：会话内装配的配置来源在此解析
    if (store_ == nullptr || loop_ == nullptr) {
        LOG_WARN("Agent: 服务未就绪（数据库 / LLM 通道），无法启动任务");
        return;
    }
    reapFinishedRuns();
    const AgentTaskRow* row = nullptr;
    for (const auto& t : tasks_) {
        if (t.id == taskId) {
            row = &t;
            break;
        }
    }
    if (row == nullptr) {
        LOG_WARN("Agent: 任务不存在或缓存未刷新（id=" + std::to_string(taskId) +
                 "），忽略启动");
        return;
    }
    ActiveRun* raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        for (const auto& r : runs_) {
            if (r->taskId == taskId) {
                LOG_INFO("Agent: 任务 " + std::to_string(taskId) +
                         " 已在运行，忽略本次启动");
                return;
            }
        }
        auto run = std::make_unique<ActiveRun>();
        run->taskId = taskId;
        runs_.push_back(std::move(run));
        raw = runs_.back().get();
    }
    const AgentTaskRow taskRecord = *row;   // 值拷贝，线程不摸 tasks_
    try {
        raw->thread = std::thread([this, raw, task = taskRecord] {
            // 生命周期由 onStop 的 join 先于 reset 保证；taskFromRow = store 行 → AgentTask 唯一映射
            const AgentTask t = taskFromRow(task);
            auto started = loop_->run(t, raw->cancel);
            if (started.isFail()) {
                LOG_ERROR("Agent: run 启动失败（task " +
                          std::to_string(task.id) + "）: " +
                          started.errorMessage());
            }
            raw->done.store(true);
            // 通知驱动的关键一步（fix round）：run 结束必须主动请求一帧——
            // EUI-NEO 帧循环是事件驱动（无动画/输入时不 compose），不请求则
            // 迁移检测（装配层 pollRunEnd / 页内 compose 的 refreshData）都要
            // 等到下一次用户输入才发生，通知延迟不可控。模块内调 core API 合法
            //（先例：settings_module.cpp:780 / claude_module.cpp:502 同款）。
            ::core::platform::requestUiUpdate();
        });
    } catch (const std::system_error& e) {
        // 线程创建失败：摘除占位（线程未启动，无需 join），状态列不打「运行中」
        std::lock_guard<std::mutex> lock(runsMu_);
        for (auto it = runs_.begin(); it != runs_.end(); ++it) {
            if (it->get() == raw) {
                runs_.erase(it);
                break;
            }
        }
        LOG_ERROR(std::string("Agent: run 线程创建失败: ") + e.what());
        return;
    }
    uiPolling_ = true;    // 状态列每帧跟进，直到全部 run 终止
    tasksDirty_ = true;
}

void AgentModule::openNewTaskDialog() {
    taskDialogOpen_ = true;
    pageError_.clear();
    editingTaskId_.reset();
    closeDraftDialog();   // AI 草稿会话随弹窗重置（在途结果一并作废）
    draftInput_.clear();
    editName_.clear();
    editGoal_.clear();
    editCron_.clear();
    editSlot_ = kModelSlotKeys[0];
    editIter_ = 10;
    editDestructive_ = false;
    // 上下文记忆与自动记录两字段已停读写（并入系统提示词；DB 列删除归 W6）
    // 新建时生成默认系统提示词（环境上下文：工作目录、git 分支、OS、架构）
    editSystemPrompt_ = buildDefaultSystemPrompt(
        std::filesystem::current_path().string());
    editEvents_ = {};
    editMaxRuns_ = 0;   // 新建缺省 = 不限次数（与旧任务同语义：恒循环）
    editTriggerIdx_ = 0;
    editOnceAtText_ = defaultOnceAtText(
        static_cast<int64_t>(core::time::nowUnix()));   // 切到「闹钟」时就有可用缺省，不逼用户从零拼格式
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
    currentStep_ = 0;
    testRunResult_.clear();
    refreshCronPreview();
    refreshOncePreview();
}

// 关闭任务弹窗（多条路径共用）：不落库、不碰任务列表，只退弹窗态与草稿
void AgentModule::closeTaskDialog() {
    taskDialogOpen_ = false;
    editingTaskId_.reset();
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
    closeDraftDialog();   // 子弹窗随外层一起退（在途结果一并作废）
    // 试运行线程仍在飞：请求取消 + join 回收（不 detach，不留悬垂线程）
    if (dryRun_) {
        dryRun_->cancel.store(true);
        if (dryRun_->thread.joinable()) dryRun_->thread.join();
        dryRun_.reset();
    }
    testRunResult_.clear();
}

void AgentModule::setEditCronDraft(const std::string& cron) {
    editCron_ = cron;
    refreshCronPreview();
}

void AgentModule::setEditOnceDraft(const std::string& text) {
    editOnceAtText_ = text;
    refreshOncePreview();
}

void AgentModule::setDialogIterDraft(int iter) {
    editIter_ = iter;
}

// 预览行重算：只在 editCron_ 变化点调用（不在 compose 每帧调——无匹配表达式
// 的 366*1440 步前探最坏数十万次 localTime，每帧算会拖垮弹窗）。
// 「现在」取 deps_.schedulerNow（测试可注入假时钟），缺省真实系统时间。
void AgentModule::refreshCronPreview() {
    cronPreview_.clear();
    cronPreviewIsError_ = false;
    const std::string cron = trimCopy(editCron_);
    if (cron.empty()) return;
    auto expr = parseCron(cron);
    if (expr.isFail()) {
        cronPreview_ = "cron 表达式非法：" + expr.errorMessage();
        cronPreviewIsError_ = true;
        return;
    }
    const std::time_t now = deps_.schedulerNow ? deps_.schedulerNow()
                                               : core::time::nowUnix();
    auto next = nextCronMatch(expr.value(), now);
    if (next.isFail()) {
        cronPreview_ = "一年内无触发时间";
        cronPreviewIsError_ = true;
        return;
    }
    const std::tm& t = next.value();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "下次触发：%04d-%02d-%02d %02d:%02d",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                  t.tm_min);
    cronPreview_ = buf;
}

// 闹钟预览：合法未来时刻 → 常态文案；格式非法 / 归一化失败 / 已过去 → 红字
//（与保存同一份解析函数，预览说能存就一定存得进去）
void AgentModule::refreshOncePreview() {
    oncePreview_.clear();
    oncePreviewIsError_ = false;
    if (editTriggerIdx_ != kTriggerOnceIdx) return;
    const std::string text = trimCopy(editOnceAtText_);
    if (text.empty()) {
        oncePreview_ = "请填写闹钟时间（YYYY-MM-DD HH:MM）";
        oncePreviewIsError_ = true;
        return;
    }
    auto parsed = parseOnceAtLocalText(text);
    if (parsed.isFail()) {
        oncePreview_ = parsed.errorMessage();
        oncePreviewIsError_ = true;
        return;
    }
    const int64_t now = static_cast<int64_t>(core::time::nowUnix());
    if (parsed.value() <= now) {
        oncePreview_ = "闹钟时间必须晚于当前时间";
        oncePreviewIsError_ = true;
        return;
    }
    oncePreview_ =
        "将在 " + formatOnceAtText(parsed.value()) + " 提醒（需应用运行；错过不补）";
}

// 编辑态：字段以库为准回填（tasks_ 可能滞后，故直接 getTask）；读库失败 → 页内错误行
void AgentModule::openTaskEditor(int64_t taskId) {
    if (store_ == nullptr) return;
    auto row = store_->getTask(taskId);
    if (row.isFail()) {
        pageError_ = "读取任务失败：" + row.errorMessage();
        LOG_WARN("Agent: 打开任务弹窗失败: " + row.errorMessage());
        return;
    }
    const AgentTaskRow& r = row.value();
    pageError_.clear();
    taskDialogOpen_ = true;
    editingTaskId_ = r.id;
    closeDraftDialog();   // AI 草稿会话随弹窗重置（在途结果一并作废）
    draftInput_.clear();
    editName_ = r.name;
    editGoal_ = r.goal;
    editCron_ = cronTextOf(r);
    editOnceAtText_.clear();
    editSlot_ = r.modelSlot;
    editIter_ = r.maxIterations;
    editDestructive_ = r.allowDestructive;
    editSystemPrompt_ = r.systemPrompt;  // 编辑已有任务：保持用户现有值（不覆盖）
    editTriggerIdx_ = triggerIndexOf(r.triggerType);
    // 「闹钟」分支回填：按 once_at 格式化成同一份 YYYY-MM-DD HH:MM 文本；坏配置
    // 留空 + 弹窗内点名异常，绝不回落到「手动」（回落会让一次保存把类型改写成手动）。
    // triggerConfigProblem 与列表「配置异常」同源，坏配置的原因两处口径一致。
    std::string onceProblem;
    if (r.triggerType == "once") {
        auto cfg = decodeTriggerConfig(r.triggerConfig);
        if (cfg.isOk() && cfg.value().onceAt) {
            editOnceAtText_ = formatOnceAtText(*cfg.value().onceAt);
        } else {
            std::string problem =
                triggerConfigProblem(r.triggerType, r.triggerConfig);
            if (problem.empty()) problem = "缺少 once_at（闹钟不会触发）";
            onceProblem = "触发配置异常：" + problem;
            LOG_WARN("Agent: 任务 " + std::to_string(r.id) +
                     " 的闹钟配置无法回填（输入框留空）：" + problem);
        }
    }
    // 限次草稿回填：只有 schedule / event 消费 trigger_config 的 max_runs（manual /
    // resident 恒 0——给不消费该列的任务显示「只跑一次」等于骗人）。坏配置按 0（不限次）
    // 显示：解码失败的点名 WARN 归 cronTextOf（schedule）/ 下面的分支（event），
    // 同一份配置只报一次
    editMaxRuns_ = 0;
    if (r.triggerType == "schedule") {
        auto cfg = decodeTriggerConfig(r.triggerConfig);
        if (cfg.isOk() && cfg.value().maxRuns) editMaxRuns_ = *cfg.value().maxRuns;
    }
    // 「事件」分支回填：按 decode 结果逐项勾选；坏配置不静默（与 cronTextOf 同口径）
    editEvents_ = {};
    if (r.triggerType == "event") {
        auto cfg = decodeTriggerConfig(r.triggerConfig);
        if (cfg.isOk()) {
            if (cfg.value().maxRuns) editMaxRuns_ = *cfg.value().maxRuns;
            for (int i = 0; i < kEventCount; ++i) {
                const std::string name = kEventNames[i];
                if (std::find(cfg.value().events.begin(), cfg.value().events.end(),
                               name) != cfg.value().events.end()) {
                    editEvents_[i] = true;
                }
            }
        } else if (!r.triggerConfig.empty()) {
            LOG_WARN("Agent: 任务 " + std::to_string(r.id) +
                     " 的 trigger_config 无法解析（事件表单按全不勾显示）：" +
                     cfg.errorMessage());
        }
    }
    editError_.clear();
    if (!onceProblem.empty()) editError_ = onceProblem;
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
    currentStep_ = 0;
    testRunResult_.clear();
    refreshCronPreview();
    refreshOncePreview();
}

// 保存任务弹窗（新建/编辑同一路径）：定时先 parseCron 校验、事件校验 events 至少
// 勾选 1 个；非法 → 弹窗内报错不关窗
void AgentModule::saveTaskDialog() {
    if (store_ == nullptr) {
        editError_ = "数据库未就绪";
        return;
    }
    const std::string goal = trimCopy(editGoal_);
    if (goal.empty()) {
        editError_ = "请先填写任务目标";
        return;
    }
    const std::string triggerType = kTriggerKeys[editTriggerIdx_];
    std::string triggerConfig;
    if (triggerType == std::string("schedule")) {
        const std::string cron = trimCopy(editCron_);
        auto expr = parseCron(cron);
        if (expr.isFail()) {
            editError_ = "cron 表达式非法：" + expr.errorMessage();
            return;
        }
        // 编码唯一落点 = trigger_config.h；不手拼 JSON（字符集一变就得逐个改）
        TriggerConfig cfg;
        cfg.cron = cron;
        if (editMaxRuns_ > 0) cfg.maxRuns = editMaxRuns_;   // 0 = 不限次数 → 不写该键
        auto encoded = encodeTriggerConfig(cfg);
        if (encoded.isFail()) {
            editError_ = "保存失败：" + encoded.errorMessage();
            LOG_ERROR("Agent: 触发配置编码失败: " + encoded.errorMessage());
            return;
        }
        triggerConfig = encoded.value();
    } else if (triggerType == std::string("event")) {
        // 编码唯一落点 = trigger_config.h；events 至少 1 个（全不勾 = 永不触发的死配置）
        TriggerConfig cfg;
        for (int i = 0; i < kEventCount; ++i) {
            if (editEvents_[i]) cfg.events.push_back(kEventNames[i]);
        }
        if (cfg.events.empty()) {
            editError_ = "事件触发需至少勾选一个事件";
            return;
        }
        if (editMaxRuns_ > 0) cfg.maxRuns = editMaxRuns_;   // 事件型同样可配一次性
        auto encoded = encodeTriggerConfig(cfg);
        if (encoded.isFail()) {
            editError_ = "保存失败：" + encoded.errorMessage();
            LOG_ERROR("Agent: 触发配置编码失败: " + encoded.errorMessage());
            return;
        }
        triggerConfig = encoded.value();
    } else if (triggerType == std::string("once")) {
        // 闹钟：严格本地时间解析 + 必须晚于现在（Scheduler 对错过窗口不补发，
        // 存一个过去时刻等于建了个永不触发的任务）。不写 cron / events / max_runs：
        // once_at 本身就是「只跑一次」，多余字段只会让 encode 的互斥校验失败。
        auto parsed = parseOnceAtLocalText(editOnceAtText_);
        if (parsed.isFail()) {
            editError_ = parsed.errorMessage();
            return;
        }
        if (parsed.value() <= static_cast<int64_t>(core::time::nowUnix())) {
            editError_ = "闹钟时间必须晚于当前时间";
            return;
        }
        TriggerConfig cfg;
        cfg.onceAt = parsed.value();
        auto encoded = encodeTriggerConfig(cfg);
        if (encoded.isFail()) {
            editError_ = "保存失败：" + encoded.errorMessage();
            LOG_ERROR("Agent: 触发配置编码失败: " + encoded.errorMessage());
            return;
        }
        triggerConfig = encoded.value();
    }
    const int iter = editIter_ < 1 ? 1 : (editIter_ > 50 ? 50 : editIter_);
    // UAT5：任务名称独立输入（TaskDef.name 一一对应）；留空 = 既有行为按目标取名
    const std::string name = [&] {
        const std::string trimmed = trimCopy(editName_);
        return trimmed.empty() ? deriveTaskName(goal) : trimmed;
    }();
    if (editingTaskId_) {
        auto updated = store_->updateTask(*editingTaskId_, name, goal,
                                          triggerType, triggerConfig, editSlot_, iter,
                                          editDestructive_, editSystemPrompt_);
        if (updated.isFail()) {
            editError_ = "保存失败：" + updated.errorMessage();
            LOG_ERROR("Agent: 保存任务失败: " + updated.errorMessage());
            return;
        }
    } else {
        auto created = store_->createTask(name, goal, triggerType,
                                          editSlot_, iter, editDestructive_,
                                          triggerConfig, editSystemPrompt_);
        if (created.isFail()) {
            editError_ = "创建失败：" + created.errorMessage();
            LOG_ERROR("Agent: 创建任务失败: " + created.errorMessage());
            return;
        }
    }
    closeTaskDialog();
    tasksDirty_ = true;
}

// 试运行：不落正式任务列表——用当前表单建临时任务 → 起一次 run → 跑完即删。
// 线程借 ActiveRun 模式（cancel/done 原子 + reap join）；完成结果经 dryRun_.done 回收。
void AgentModule::testRunDialog() {
    if (dryRun_ != nullptr || store_ == nullptr || loop_ == nullptr) return;
    const std::string goal = trimCopy(editGoal_);
    if (goal.empty()) {
        editError_ = "请先填写任务目标"; currentStep_ = 2; return;
    }
    const std::string triggerType = kTriggerKeys[editTriggerIdx_];
    std::string triggerConfig;
    // 构建 triggerConfig（与 saveTaskDialog 同源）
    if (triggerType == std::string("schedule")) {
        const std::string cron = trimCopy(editCron_);
        auto expr = parseCron(cron);
        if (expr.isFail()) {
            editError_ = "cron 表达式非法：" + expr.errorMessage(); currentStep_ = 1; return;
        }
        TriggerConfig cfg;
        cfg.cron = cron;
        if (editMaxRuns_ > 0) cfg.maxRuns = editMaxRuns_;
        auto encoded = encodeTriggerConfig(cfg);
        if (encoded.isFail()) {
            editError_ = "试运行失败：" + encoded.errorMessage(); return;
        }
        triggerConfig = encoded.value();
    } else if (triggerType == std::string("event")) {
        TriggerConfig cfg;
        for (int i = 0; i < kEventCount; ++i)
            if (editEvents_[i]) cfg.events.push_back(kEventNames[i]);
        if (cfg.events.empty()) {
            editError_ = "事件触发需至少勾选一个事件"; currentStep_ = 1; return;
        }
        if (editMaxRuns_ > 0) cfg.maxRuns = editMaxRuns_;
        auto encoded = encodeTriggerConfig(cfg);
        if (encoded.isFail()) {
            editError_ = "试运行失败：" + encoded.errorMessage(); return;
        }
        triggerConfig = encoded.value();
    } else if (triggerType == std::string("once")) {
        auto parsed = parseOnceAtLocalText(editOnceAtText_);
        if (parsed.isFail()) {
            editError_ = parsed.errorMessage(); currentStep_ = 1; return;
        }
        if (parsed.value() <= static_cast<int64_t>(core::time::nowUnix())) {
            editError_ = "闹钟时间必须晚于当前时间"; currentStep_ = 1; return;
        }
        TriggerConfig cfg;
        cfg.onceAt = parsed.value();
        auto encoded = encodeTriggerConfig(cfg);
        if (encoded.isFail()) {
            editError_ = "试运行失败：" + encoded.errorMessage(); return;
        }
        triggerConfig = encoded.value();
    }
    const int iter = editIter_ < 1 ? 1 : (editIter_ > 50 ? 50 : editIter_);
    const std::string name = [&] {
        const std::string trimmed = trimCopy(editName_);
        return trimmed.empty() ? deriveTaskName(goal) : trimmed;
    }();
    // 建临时任务
    auto created = store_->createTask(name + "（试运行）", goal, triggerType,
                                      editSlot_, iter, editDestructive_,
                                      triggerConfig, editSystemPrompt_);
    if (created.isFail()) {
        editError_ = "试运行失败：" + created.errorMessage();
        return;
    }
    int64_t tempTaskId = created.value();
    editError_.clear();
    testRunResult_.clear();
    // 起线程：走 loop_->run 直通（不经过 startTask 的 tasks_ 缓存依赖）
    auto run = std::make_unique<ActiveRun>();
    run->taskId = tempTaskId;
    try {
        run->thread = std::thread([this, raw = run.get(), tempTaskId] {
            auto row = store_->getTask(tempTaskId);
            if (row.isOk()) {
                AgentTask t = taskFromRow(row.value());
                loop_->run(t, raw->cancel);
            }
            // 清理临时任务（级联删 runs / messages）
            auto del = store_->deleteTask(tempTaskId);
            if (del.isFail()) {
                LOG_WARN("Agent: 试运行后删除临时任务失败（id=" +
                         std::to_string(tempTaskId) + "）: " + del.errorMessage());
            }
            raw->done.store(true, std::memory_order_release);
            ::core::platform::requestUiUpdate();
        });
    } catch (const std::system_error& e) {
        store_->deleteTask(tempTaskId);
        editError_ = "试运行启动失败：" + std::string(e.what());
        LOG_ERROR(std::string("Agent: 试运行线程创建失败: ") + e.what());
        return;
    }
    dryRun_ = std::move(run);
}

// 启用/禁用取反（独立于任务弹窗，不碰其它列）。失败 → 页内错误行，不静默
void AgentModule::toggleTaskEnabled(const AgentTaskRow& task) {
    if (store_ == nullptr) return;
    auto r = store_->setTaskEnabled(task.id, !task.enabled);
    if (r.isFail()) {
        pageError_ = "启停切换失败：" + r.errorMessage();
        LOG_WARN("Agent: 启停切换失败（task " + std::to_string(task.id) +
                 "）: " + r.errorMessage());
        return;
    }
    pageError_.clear();
    tasksDirty_ = true;
}

// 打开详情弹窗：每开一次回到「最近一次 run」（selectedRunId_ 清空，refreshData 回落首条）。
// 布局态一并清零（回 tab 0「执行记录」、消息区收起），弹窗不记忆上一次的分支状态
void AgentModule::openTaskDetail(int64_t taskId) {
    detailTaskId_ = taskId;
    selectedRunId_.reset();
    detailMessages_.clear();
    detailMessagesError_.clear();
    detailPlan_.reset();
    detailPlanKey_.clear();
    // 消息展开态按 seq 定位且只在 run 内有意义：换任务即清
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    detailTab_ = 0;
    messagesOpen_ = false;
    ++msgScrollVersion_;
    tasksDirty_ = true;
}

// 关详情弹窗：只退弹窗态，不触发重查
void AgentModule::closeTaskDetail() {
    detailTaskId_.reset();
    detailTaskRow_.reset();
    selectedRunId_.reset();
    detailMessages_.clear();
    detailMessagesError_.clear();
    detailPlan_.reset();
    detailPlanKey_.clear();
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    detailTab_ = 0;
    messagesOpen_ = false;
    ++msgScrollVersion_;
}

// 「下一次」缓存（spec §8.5）：分钟/任务/最近 run 任一变化才重算。
// schedule 才有 nextCronMatch 回扫成本；其余类型廉价，但统一走键判等防任务
// 配置热改后展示陈旧语义。UI 线程专用（compose 每帧先判等后早退，均摊零查询）
void AgentModule::updateNextRunCache(const AgentTaskRow& task,
                                     const AgentRunRow* latest,
                                     int64_t nowSec) {
    const int64_t minute = nowSec / 60;
    const int64_t latestId = latest != nullptr ? latest->id : 0;
    const std::string latestStatus = latest != nullptr ? latest->status : std::string();
    if (nextRunText_.has_value() && nextRunMinute_ == minute &&
        nextRunTaskId_ == task.id && nextRunLatestRunId_ == latestId &&
        nextRunLatestStatus_ == latestStatus) {
        return;
    }
    nextRunText_ = describeNextRun(task, latest, nowSec);
    nextRunMinute_ = minute;
    nextRunTaskId_ = task.id;
    nextRunLatestRunId_ = latestId;
    nextRunLatestStatus_ = latestStatus;
}

// 「计划 vs 实际」缓存（spec §8.4）：键 = taskId+runId+startedAt+triggerType+
// triggerConfig。previousCronMatch 最坏回扫一年分钟，只在键变化时付一次
void AgentModule::updateDetailPlanCache(const AgentTaskRow& task,
                                        const AgentRunRow& run) {
    const std::string key = std::to_string(task.id) + "|" + std::to_string(run.id) +
                            "|" + std::to_string(run.startedAt) + "|" +
                            task.triggerType + "|" + task.triggerConfig;
    if (detailPlan_.has_value() && detailPlanKey_ == key) return;
    detailPlan_ = describeRunPlan(task, run, core::time::nowUnix());
    detailPlanKey_ = key;
}

// 默认展开最近一条带 text 的 assistant 消息；只在 seq 变化时改展开集，手动收起不被每帧顶回
void AgentModule::applyDefaultMessageExpansion() {
    std::optional<int64_t> target;
    for (auto it = detailMessages_.rbegin(); it != detailMessages_.rend(); ++it) {
        if (it->role != "assistant") continue;
        const std::vector<MessageBlock> blocks = decodeMessageBlocks(it->contentJson);
        for (const auto& b : blocks) {
            if (b.type == MessageBlock::Type::Text && !b.text.empty()) {
                target = it->seq;
                break;
            }
        }
        if (target) break;
    }
    if (target == autoExpandedSeq_) return;
    if (autoExpandedSeq_) expandedMsgSeqs_.erase(*autoExpandedSeq_);
    autoExpandedSeq_ = target;
    if (target) {
        expandedMsgSeqs_.insert(*target);
        ++msgScrollVersion_;   // 内容高变了 → scrollView 重测
    }
}

void AgentModule::toggleMessageExpanded(int64_t seq) {
    if (expandedMsgSeqs_.count(seq) > 0) {
        expandedMsgSeqs_.erase(seq);
    } else {
        expandedMsgSeqs_.insert(seq);
    }
    ++msgScrollVersion_;
}

// 「可用工具」注册表快照（惰性装载；展开时强制重装反映模块启停）
void AgentModule::ensureToolsCache(bool forceReload) {
    if (toolsCacheLoaded_ && !forceReload) return;
    toolsCache_ = listRegisteredTools();
    toolsCacheLoaded_ = true;
}

// 授权弹窗时机：Module::onStart 在窗口可见之前（Router::startAll 于装配期执行），
// 直接放 onStart 会让 macOS 授权框在无前台窗口时出现。pollRunEnd 是 app::compose 在
// splash 结束后每帧调用的现有启动通道，此时窗口已可见；因此授权请求挂在这里。
// 只请求一次：成员闸跨 onStop/onStart 保留（系统对已决定状态本就幂等）。
void AgentModule::requestNotificationAuthorizationOnce() {
    if (notificationAuthorizationRequested_) return;
    notificationAuthorizationRequested_ = true;
    platform::notify::requestAuthorization();
}

// 装配层每帧拉取路径（app::compose 内调用）：迁移检测不依赖智能体页可见性。
// 守卫：store_ 在 +（uiPolling_ 或 页脏 或 上次观测仍有 running run）。第三个条件
// 覆盖不经页面启动的 run（直写库/外部进程）：prevRunStatuses_ 有 running 就该继续
// 轮询到终态，否则迁移永不发生。三条件全假时零常规查询——但那也是冷启动盲区
// （外部直写库新插入的 running run 永远进不了 prevRunStatuses_），靠低频轻查询兜底
// （pollRunEndFallback），命中即 refreshData 进入正常观测路径。
void AgentModule::pollRunEnd() {
    requestNotificationAuthorizationOnce();
    reapNotifySends();
    if (store_ == nullptr) return;
    bool prevAlive = false;
    for (const auto& [id, st] : prevRunStatuses_) {
        (void)id;
        if (st == "running") {
            prevAlive = true;
            break;
        }
    }
    if (!uiPolling_ && !tasksDirty_ && !prevAlive) {
        pollRunEndFallback();
        return;
    }
    refreshData();
}

// 冷启动兜底：外部直写库插入 running run 时三守卫全假、prevRunStatuses_ 永远观测不到它，
// 其终态迁移零通知；唯一出路是低频轻查询。节流 = kRunFallbackIntervalSec（时钟走
// schedulerNow 缝，测试可冻结；unix 秒对 15s 窗口的 NTP 跳变最坏差一个周期，可接受）。
// 命中即 refreshData 一次——该 run 随即进 prevRunStatuses_，后续帧由 prevAlive 接管走
// 常规轮询，兜底自动停摆。查询失败 WARN 留痕不吞，下个周期自然重试。
// 已知洞（登记于 backlog，不修）：起止**都落在两个检查点之间**的短命外部 run
// （寿命 < kRunFallbackIntervalSec）两头都没被观测到，仍零通知——要彻底关掉它得让外部
// 直写库的一方也发事件，成本大于收益。
void AgentModule::pollRunEndFallback() {
    const int64_t now = deps_.schedulerNow ? deps_.schedulerNow()
                                           : core::time::nowUnix();
    if (lastRunFallbackSec_ != 0 && now - lastRunFallbackSec_ < kRunFallbackIntervalSec) {
        return;
    }
    lastRunFallbackSec_ = now;
    ++runFallbackChecks_;
    auto r = store_->hasRunningRun();
    if (r.isFail()) {
        LOG_WARN("Agent: 兜底轮询轻查询失败: " + r.errorMessage());
        return;
    }
    if (!r.value()) return;
    LOG_DEBUG("Agent: 兜底轮询发现库内 running run（三守卫全假冷启动盲区）→ 触发 refreshData");
    refreshData();
}

// 取走语义：返回队首待发通知并移除；无待发 = nullopt。装配层拉到后调
// app::showAppNotice（app 级 toast，跨页可见）。
std::optional<std::pair<std::string, std::string>>
AgentModule::takeRunEndNotice() {
    std::lock_guard<std::mutex> lock(pendingNoticesMu_);
    if (pendingNotices_.empty()) return std::nullopt;
    auto notice = std::move(pendingNotices_.front());
    pendingNotices_.erase(pendingNotices_.begin());
    return notice;
}
// 任务级 run 汇总缓存（writeVersion + 7d 窗口起点双键，spec §8.2/§8.6）：
// 版本与窗口都没变零查询零锁；跨天即使无写入也必须重查（窗口平移）；
// 失败不静默——错误文案留在 runSummariesError_，详情弹窗摘要卡显形
void AgentModule::refreshRunSummaries() {
    if (store_ == nullptr) return;
    const int64_t windowStart =
        static_cast<int64_t>(sevenDayWindowStart(core::time::nowUnix()));
    const uint64_t ver = store_->writeVersion();
    if (runSummariesSeenVersionValid_ && runSummariesSeenVersion_ == ver &&
        runSummariesSeenWindowStart_ == windowStart) {
        return;
    }
    auto r = store_->listTaskRunSummaries(windowStart);
    if (r.isOk()) {
        runSummaries_.clear();
        for (auto& s : r.value()) {
            runSummaries_[s.taskId] = std::move(s);
        }
        runSummariesError_.clear();
    } else {
        runSummaries_.clear();
        runSummariesError_ = r.errorMessage();
        LOG_ERROR("Agent: 任务 run 汇总读取失败: " + r.errorMessage());
    }
    runSummariesSeenVersion_ = ver;
    runSummariesSeenWindowStart_ = windowStart;
    runSummariesSeenVersionValid_ = true;
}

// 重查任务/最近 run/详情消息；触发：tasksDirty_ 或 uiPolling_。判停：run 线程全 done 且库内无 running
void AgentModule::refreshData() {
    tasksDirty_ = false;
    refreshRunSummaries();
    tasks_.clear();
    latestRuns_.clear();
    taskRuns_.clear();
    auto t = store_->listTasks();
    if (t.isFail()) {
        LOG_ERROR("Agent: 读取任务失败: " + t.errorMessage());
    } else {
        tasks_ = std::move(t).value();
    }
    // 配置异常缓存：Scheduler 每跑必跳过的坏配置任务（scheduler.cpp 的 decode / parseCron
    // 失败分支），在列表上必须与正常任务可区分——「任务配好了，什么都没发生」是最坏的
    // 失败模式。判定是纯函数、与 Scheduler 同源；随本次重查算一次，不在渲染里反复解析
    taskTriggerProblems_.clear();
    for (const auto& task : tasks_) {
        std::string problem =
            triggerConfigProblem(task.triggerType, task.triggerConfig);
        if (!problem.empty()) taskTriggerProblems_[task.id] = std::move(problem);
    }
    // 限次缓存（列表徽章 / 详情摘要的展示依据）：只认 schedule / event —— manual /
    // resident 的 trigger_config 不被 Scheduler 消费，挂了「一次性」也是永不生效的假承诺。
    // 坏配置不进表（decode 失败 → 0），那种任务已经有「配置异常」徽章，不该再挂限次标记
    taskRunLimits_.clear();
    for (const auto& task : tasks_) {
        if (task.triggerType != "schedule" && task.triggerType != "event") continue;
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        if (cfg.isOk() && cfg.value().maxRuns && *cfg.value().maxRuns > 0) {
            taskRunLimits_[task.id] = *cfg.value().maxRuns;
        }
    }
    // 列表每任务只查最近 1 条（D8：状态列/通知/判停全只用 front，store 排序契约
    // (started_at DESC, id DESC) 从纵深防御变成唯一依赖——见 R-E，回归用例钉住）。
    // 详情任务单独查 20 条进 taskRuns_，其余任务不再占内存
    for (const auto& task : tasks_) {
        auto runs = store_->listRuns(task.id, kRunListLimit);
        if (runs.isFail()) {
            LOG_ERROR("Agent: 读取 run 列表失败: " + runs.errorMessage());
            continue;
        }
        auto list = std::move(runs).value();
        // store 已按 (started_at DESC, id DESC) 排序，此处再排一次是纵深防御，不依赖 store 契约
        std::stable_sort(list.begin(), list.end(),
                         [](const AgentRunRow& a, const AgentRunRow& b) {
                             if (a.startedAt != b.startedAt) {
                                 return a.startedAt > b.startedAt;
                             }
                             return a.id > b.id;
                         });
        if (!list.empty()) {
            latestRuns_[task.id] = list.front();
        }
    }
    taskRuns_.clear();
    if (detailTaskId_) {
        auto runs = store_->listRuns(*detailTaskId_, kDetailRunListLimit);
        if (runs.isFail()) {
            LOG_ERROR("Agent: 读取详情 run 列表失败: " + runs.errorMessage());
        } else {
            auto list = std::move(runs).value();
            std::stable_sort(list.begin(), list.end(),
                             [](const AgentRunRow& a, const AgentRunRow& b) {
                                 if (a.startedAt != b.startedAt) {
                                     return a.startedAt > b.startedAt;
                                 }
                                 return a.id > b.id;
                             });
            taskRuns_[*detailTaskId_] = std::move(list);
        }
    }
    // 配置不可用类错误（无激活 profile / 未配 Base URL / 未配 API Key / 槽位未配置）
    // 同步到页内错误行：run 线程失败只 LOG_ERROR，不落 pageError_，弹窗外将零可见出口。
    // 仅 pageError_ 为空时同步（用户动作的错误优先），且只认 needsClaudePageHint 那一类
    // 前缀——普通失败（网络 / 上游 4xx）不上页内错误行。
    // 已展示过的 runId 先剪枝：只保留「仍是某任务最近一次 run」的条目（任务再跑出新
    // run → 旧 id 永不再匹配，留着只是无界增长）。
    if (!pageErrorRunIds_.empty()) {
        for (auto it = pageErrorRunIds_.begin(); it != pageErrorRunIds_.end();) {
            bool stillLatest = false;
            for (const auto& [taskId, run] : latestRuns_) {
                (void)taskId;
                if (run.id == *it) {
                    stillLatest = true;
                    break;
                }
            }
            if (stillLatest) {
                ++it;
            } else {
                it = pageErrorRunIds_.erase(it);
            }
        }
    }
    // 集合命中即跳过：那条 run 的错误用户已经看过（并被清除），同一份旧错误不回弹；
    // 有别的任务/新 run 的错，循环照常接着找（不是「遇错即停」）。
    if (pageError_.empty()) {
        for (const auto& [taskId, run] : latestRuns_) {
            (void)taskId;
            if (pageErrorRunIds_.count(run.id) != 0) continue;
            if (run.error && needsClaudePageHint(*run.error)) {
                pageError_ = *run.error;
                pageErrorRunIds_.insert(run.id);
                break;
            }
        }
    }
    // run 完成通知：本次轮询 vs 上次轮询的 running→failed/limited 迁移必响；
    // completed/cancelled 静默；同一 runId 只通知一次（notifiedRunIds_）。
    // 只看 latestRuns_ 即可：刚结束的 run 必是其任务的最近一次 run，不会漏迁移。
    // 通知不在此直发 toast：入 pendingNotices_ 队列，由装配层每帧 takeRunEndNotice()
    // 取走后发 app 级 toast（app::showAppNotice，跨页可见）——页面无关（fix round）。
    // 失败/受限另有系统通知通道（下方 send）：toast 4.5s 即逝，定时任务失败恰恰
    // 发生在人不在应用前的场景，OS 通道才接得住；成功仍走全静默，两通道都不发。
    {
        std::map<int64_t, std::string> nowStatus;
        for (const auto& [taskId, run] : latestRuns_) {
            (void)taskId;
            nowStatus[run.id] = run.status;
        }
        for (int64_t runId : detectRunEndNotifications(prevRunStatuses_, nowStatus,
                                                       notifiedRunIds_)) {
            // 任务名反查：runId → taskId（latestRuns_ map 值）→ tasks_ 名称；缺失兜底
            std::string taskName;
            for (const auto& [tid, run] : latestRuns_) {
                if (run.id != runId) continue;
                for (const auto& task : tasks_) {
                    if (task.id == tid) {
                        taskName = task.name;
                        break;
                    }
                }
                break;
            }
            if (taskName.empty()) taskName = "任务 " + std::to_string(runId);
            const std::string& st = nowStatus[runId];
            // 同一通知的两种投递（app toast + OS 通知）共用一份文案，改文案只改这里
            const std::string title =
                st == "failed" ? "智能体任务失败" : "智能体任务受限";
            const std::string body =
                "「" + taskName + "」运行结束（" + runStatusText(st) + "）";
            {
                std::lock_guard<std::mutex> lock(pendingNoticesMu_);
                pendingNotices_.emplace_back(title, body);
            }
            // 系统通知同发：受理≠展示（Focus/勿扰可能静默抑制），未受理只 LOG 不重试；
            // 「同一 runId 只发一次」由 notifiedRunIds_ 保证（入队侧，仍在主线程）。
            // 稳定 id 复用系统端替换语义（同 id 重发更新旧通知而非堆叠）。
            // send 挪后台线程：facade 的授权检查在 macOS 未决期可阻塞 ≤1s（平台后端
            // 文件头自警），不得占 UI 主线程。fire-and-forget + NotifySend 线程表：
            // onStop/析构全量 join（T7b 纪律），不用 detach。线程创建失败只 LOG 丢弃
            //——通知是 best-effort，与「未受理不重试」同口径
            {
                platform::notify::Notification n;
                n.id = "agent-run-" + std::to_string(runId);
                n.title = title;
                n.body = body;
                n.level = platform::notify::Level::Error;
                auto item = std::make_unique<NotifySend>();
                NotifySend* raw = item.get();
                try {
                    raw->thread = std::thread([raw, n = std::move(n)]() mutable {
                        auto receipt = platform::notify::send(n);
                        if (receipt.isFail() ||
                            receipt.value().status !=
                                platform::notify::Status::Accepted) {
                            LOG_DEBUG(
                                "Agent: 系统通知未受理 id=" + n.id + ": " +
                                (receipt.isOk()
                                     ? platform::notify::statusToString(
                                           receipt.value().status)
                                     : receipt.errorMessage()));
                        }
                        raw->done.store(true, std::memory_order_release);
                    });
                    std::lock_guard<std::mutex> lock(notifySendsMu_);
                    notifySends_.push_back(std::move(item));
                } catch (const std::system_error& e) {
                    LOG_ERROR("Agent: 系统通知发送线程创建失败（通知丢弃）: " +
                              std::string(e.what()));
                }
            }
            notifiedRunIds_.insert(runId);
        }
        prevRunStatuses_ = std::move(nowStatus);
        // 去重集随窗口收敛：否则它是本模块唯一只增不减的容器（会话内不释放）。
        // 放在快照交接之后——此时 prevRunStatuses_ 就是本轮窗口。
        pruneNotifiedRunIds(prevRunStatuses_, notifiedRunIds_);
    }
    if (detailTaskId_) {
        detailMessages_.clear();
        // 配置摘要快照：走 getTask（tasks_ 列表可能滞后于库）
        auto row = store_->getTask(*detailTaskId_);
        if (row.isOk()) {
            detailTaskRow_ = std::move(row).value();
        } else {
            detailTaskRow_.reset();
            LOG_WARN("Agent: 详情弹窗读取任务失败: " + row.errorMessage());
        }
        auto it = taskRuns_.find(*detailTaskId_);
        if (it != taskRuns_.end() && !it->second.empty()) {
            // 显式选中优先；不在本任务列表（换了任务 / 被上限挤出）→ 回落最近一次
            bool stillListed = false;
            if (selectedRunId_) {
                for (const auto& r : it->second) {
                    if (r.id == *selectedRunId_) {
                        stillListed = true;
                        break;
                    }
                }
            }
            if (!stillListed) {
                selectedRunId_ = it->second.front().id;
            }
            auto msgs = store_->listMessages(*selectedRunId_);
            if (msgs.isOk()) {
                detailMessages_ = std::move(msgs).value();
                detailMessagesError_.clear();
            } else {
                // 读取失败显形（spec §10）：红字提示，不拿旧消息伪装当前态
                detailMessagesError_ = msgs.errorMessage();
                LOG_ERROR("Agent: 读取 run 消息失败: " + msgs.errorMessage());
            }
        } else {
            selectedRunId_.reset();
            detailMessagesError_.clear();
        }
        // 「计划 vs 实际」缓存：只在选中 run / 任务配置变化时重算（spec §8.4——
        // previousCronMatch 稀疏 cron 最坏回扫一年分钟，禁在 compose 逐帧做）
        if (detailTaskRow_ && selectedRunId_) {
            const AgentRunRow* sel = nullptr;
            const auto runsIt = taskRuns_.find(*detailTaskId_);
            if (runsIt != taskRuns_.end()) {
                for (const auto& r : runsIt->second) {
                    if (r.id == *selectedRunId_) {
                        sel = &r;
                        break;
                    }
                }
            }
            if (sel != nullptr) {
                updateDetailPlanCache(*detailTaskRow_, *sel);
            } else {
                detailPlan_.reset();
                detailPlanKey_.clear();
            }
        } else {
            detailPlan_.reset();
            detailPlanKey_.clear();
        }
        // 装载后套默认展开态——只在消息区展开时（P3 起 messagesOpen_ 是「展开态内
        // 规则」；折叠态消息不渲染，展开集动了也没意义）
        if (messagesOpen_) applyDefaultMessageExpansion();
    } else {
        detailTaskRow_.reset();
    }
    bool anyAlive = false;
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        for (const auto& r : runs_) {
            if (!r->done.load()) {
                anyAlive = true;
                break;
            }
        }
    }
    bool dbRunning = false;
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        dbRunning = hasLiveRun(latestRuns_, orphanedRunIds_);
    }
    if (!anyAlive && !dbRunning) {
        uiPolling_ = false;
    }
}

// 消息行渲染：.head（徽章 + 时间 + 展开/收起 + 复制 chip，头高 24）、收起态 .preview、
// 展开态逐块 .blk.<j>；正文/预览/工具输出统一 fsS/20（spec §9.2），text 块 maxWidth
// 必须显式给（measureTextSize 只认 element.maxWidth）
void AgentModule::renderMessageRow(
    ::eui::Ui& inner, float cw, const std::string& prefix, size_t index,
    const AgentMessageRow& msg, int64_t refSec, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& tokens) {
    const float fsS = t.sizes.fontSizeSmall;
    // §9.2 消息行高下限 20：fontScale <1 时 lhS 派生值会更小，钳回 20 防拥挤
    const float msgLh = std::max(fsS * 1.4f, 20.0f);
    const std::string mid = prefix + ".msg." + std::to_string(index);
    const bool expanded = expandedMsgSeqs_.count(msg.seq) > 0;
    const float boxH = estimateMessageHeight(msg, expanded, cw, fsS, msgLh);
    const std::vector<MessageBlock> blocks =
        decodeMessageBlocks(msg.contentJson);
    const std::string badge = messageRoleBadge(msg);
    constexpr float kMsgBadgeW = 44.0f;   // 2 CJK @13 + 余量
    constexpr float kMsgTimeW = 110.0f;   // MM-DD HH:MM:SS（14 字符）@13 ≈ 96

    inner.column(mid)
        .size(cw, boxH)
        .gap(kMsgGap)
        .content([&] {
            inner.row(mid + ".head")
                .size(cw, kMsgHeaderH)
                .gap(8.0f)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    inner.text(mid + ".badge")
                        .size(kMsgBadgeW, 20.0f)
                        .text(badge)
                        .fontSize(fsS)
                        .lineHeight(20.0f)
                        .fontWeight(600)
                        .color(badge == "工具" ? tokens.primary
                                              : t.colors.textSecondary)
                        .build();
                    // 头行顺序 §10：badge → time → toggle → copy（copy 仅展开态）
                    inner.text(mid + ".time")
                        .size(kMsgTimeW, 20.0f)
                        .text(messageTimeText(msg.createdAt, refSec))
                        .fontSize(fsS)
                        .lineHeight(20.0f)
                        .wrap(false)
                        .maxWidth(kMsgTimeW)
                        .color(t.colors.textTertiary)
                        .build();
                    actionChip(inner, mid + ".toggle",
                               expanded ? "收起" : "展开", 48.0f,
                               /*primary=*/false, /*enabled=*/true, tokens,
                               [this, seq = msg.seq] {
                                   toggleMessageExpanded(seq);
                               }, fsS);
                    if (expanded) {
                        actionChip(inner, mid + ".copy", "复制", 48.0f,
                                   /*primary=*/false, /*enabled=*/true, tokens,
                                   [this, text = messagePlainText(blocks)] {
                                       // setClipboardText 返回 void 无法校验 → LOG 留痕
                                       core::window::setClipboardText(text);
                                       LOG_INFO("Agent: 已复制消息全文 " +
                                                std::to_string(text.size()) +
                                                " 字节到剪贴板");
                                       toast_.show("已复制",
                                                   "消息全文已复制到剪贴板");
                                   }, fsS);
                    }
                })
                .build();
            if (!expanded) {
                inner.text(mid + ".preview")
                    .size(cw, msgLh)
                    .text(messagePreview(msg))
                    .fontSize(fsS)
                    .lineHeight(msgLh)
.wrap(false)
                    .color(t.colors.textSecondary)
                    .onClick([this, seq = msg.seq] {
                        toggleMessageExpanded(seq);
                    })
                    .build();
                return;
            }
            if (blocks.empty()) {
                inner.text(mid + ".blk.empty")
                    .size(cw, msgLh)
                    .text("（空消息）")
                    .fontSize(fsS)
                    .lineHeight(msgLh)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }
            for (size_t j = 0; j < blocks.size(); ++j) {
                const MessageBlock& b = blocks[j];
                const std::string bid = mid + ".blk." + std::to_string(j);
                switch (b.type) {
                    case MessageBlock::Type::Text:
                    case MessageBlock::Type::Plain: {
                        inner.text(bid)
                            .size(cw, estimateTextBlockHeight(b.text, cw, fsS,
                                                              msgLh))
                            .text(b.text)
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .wrap(true)
                            .maxWidth(cw)
                            .color(b.type == MessageBlock::Type::Plain
                                       ? t.colors.textTertiary
                                       : t.colors.textPrimary)
                            .build();
                        break;
                    }
                    case MessageBlock::Type::ToolUse: {
                        inner.text(bid)
                            .size(cw, 20.0f)
                            .text("工具 · " + b.toolName + "  " +
                                  toolArgsSummary(b.text, 120))
                            .fontSize(fsS)
                            .lineHeight(20.0f)
                            .wrap(false)
                            .color(tokens.primary)
                            .build();
                        break;
                    }
                    case MessageBlock::Type::ToolResult: {
                        inner.text(bid)
                            .size(cw, msgLh)
                            .text(b.isError ? "工具输出（错误）" : "工具输出")
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .color(b.isError ? t.colors.errorText
                                             : t.colors.textTertiary)
                            .build();
                        inner.text(bid + ".out")
                            .size(cw, estimateTextBlockHeight(b.text, cw, fsS,
                                                              msgLh))
                            .text(b.text)
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .wrap(true)
                            .maxWidth(cw)
                            .color(b.isError ? t.colors.errorText
                                             : t.colors.textSecondary)
                            .build();
                        break;
                    }
                }
            }
        })
        .build();
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
        .padding(pad, pad, 0.0f, pad)  // 上右下左：底边贴底、内容随窗高伸缩（S4.2 弹性骨架）
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
                    .fontSize(12.0f)
                    .lineHeight(kPageErrorH)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.errorText)
                    .build();
                // 「去 Claude 页」引导：仅「LLM 配置不可用」类错误渲染（needsClaudePageHint 前缀钉死）
                if (hasPageHint) {
                    ui.text("agent.page.error.hint")
                        .size(contentW, kPageHintH)
                        .text("去 Claude 页 →")
                        .fontSize(12.0f)
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
                                .fontSize(13.0f)
                                .lineHeight(20.0f)
                                .fontWeight(700)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.text("agent.tools.header.hint")
                                .size(64.0f, 20.0f)
                                .text(toolsOpen_ ? "收起 ▲" : "展开 ▼")
                                .fontSize(12.0f)
                                .lineHeight(20.0f)
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
                                                        .fontSize(14.0f)
                                                        .lineHeight(17.0f)
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
                                                        .fontSize(12.0f)
                                                        .lineHeight(15.0f)
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
                                    .fontSize(11.0f)
                                    .lineHeight(16.0f)
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
                            .fontSize(20.0f)
                            .lineHeight(24.0f)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        inner.text("agent.empty.title")
                            .size(cw, 24.0f)
                            .text("暂无任务")
                            .fontSize(16.0f)
                            .lineHeight(24.0f)
                            .fontWeight(600)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textPrimary)
                            .build();
                        inner.text("agent.empty.desc")
                            .size(cw, 20.0f)
                            .text("创建智能任务，让 AI 自动执行工作流")
                            .fontSize(12.0f)
                            .lineHeight(20.0f)
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
                                    .fontSize(14.0f)
                                    .lineHeight(40.0f)
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
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.errorText)
                                        .build();
                                }
                                // 限次标记：看得出「这是一次性任务」（跑满会自动停用）
                                if (limited) {
                                    inner.text(id + ".limit")
                                        .size(kTaskWarnW, 20.0f)
                                        .text(runLimitBadge(limitIt->second))
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.textTertiary)
                                        .build();
                                }
                                // 已停用徽章：与标题置灰双通道并存（启用后两者同消）
                                if (taskOff) {
                                    inner.text(id + ".off")
                                        .size(kTaskWarnW, 20.0f)
                                        .text("已停用")
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
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
                                    .fontSize(12.0f)
                                    .lineHeight(20.0f)
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
                                    .fontSize(12.0f)
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

// 任务弹窗重设计（UAT5，demo task-new-dialog-redesign.html 智能体面板）：
// 任务名称大输入 → 触发 segment 六选（demo 只画了三选，但 TaskDef/存量任务有 6 类
// 触发——砍 UI 等于改写存量任务语义，全部保留；选中 = 绿底白字）→ 状态行
//（cron+模板 / 闹钟时间 / 事件 chips，互斥）→ 指令 textarea 全宽（标签行右缘
//「✨ AI 起草」绿 chip，行为接既有草稿功能）→ 执行记忆 → 模型槽位|迭代上限|只跑
// 一次 → 安全组（破坏性开关独立红组保持 spec §8 item 5）→ 底部 取消|创建/保存。
// 只动布局不动功能：字段与 TaskDef 一一对应。弹性宽 = max(720, 窗口/2)（demo 最小档）。
// 元素 id 契约：触发 segment 沿用 item.N（原下拉项名；可点面 .item.N.bg，
// test 的 callDropdownChange 有 .bg 回落）；slot/iter/limit/destructive/
// UAT6 步骤条重设计：顶部 4 步节点条（快递详情风）+ 每步只显示该步配置 + 底部常驻按钮
//（取消/上一步/下一步(创建)/测试）。字段与数据模型零增删，纯布局重排。
void AgentModule::composeTaskDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = std::max(640.0f, screen.width * 0.5f);
    const float h = kTaskDialogH;
    // 竖向导航案几何：标题栏/底栏各占上下 44 带，左导航固定 160（弹窗宽仍弹性），内容区夹于其间
    constexpr float kVertNavW = 160.0f;        // 左导航宽
    constexpr float kVertContentX = 164.0f;    // 内容区左缘 = 导航右缘 + 呼吸
    constexpr float kVertContentPad = 16.0f;   // 内容区四边内距
    constexpr float kContentY = 44.0f;         // 标题栏高 = 内容区顶 = 底栏高
    const float kContentW = w - kVertContentX - kVertContentPad * 2.0f;  // 字段有效宽 = W-196
    const float x0 = kVertContentX + kVertContentPad;                    // 字段左缘 = 180
    const bool isSchedule = editTriggerIdx_ == kTriggerScheduleIdx;
    const bool isEvent = editTriggerIdx_ == kTriggerEventIdx;
    const bool isOnce = editTriggerIdx_ == kTriggerOnceIdx;
    const bool isEdit = editingTaskId_.has_value();

    // --- 步骤定义 ---
    constexpr const char* kStepLabels[] = {"基础信息", "触发", "指令", "执行与安全"};
    constexpr int kStepCount = 4;

    // 逐步校验（W4 C1）：下一步按钮按当前步表单校验启用；出错标节点不弹全局错误
    const auto stepValidFor = [&](int i) -> bool {
        if (i == 0) return true;  // 名称可留空（自动取名）
        if (i == 1) {
            if (isSchedule) return parseCron(editCron_).isOk();
            if (isOnce) return !editOnceAtText_.empty()
                                && parseOnceAtLocalText(editOnceAtText_).isOk();
            if (isEvent) return std::any_of(editEvents_.begin(), editEvents_.end(),
                                            [](bool v) { return v; });
            return true;  // manual / resident 等无需配置
        }
        if (i == 2) return !editGoal_.empty();
        return true;  // step 3 执行与安全
    };

    // 试运行结果收割：dryRun_ done 后才 join + 显结果（线程安全：只用原子 done 作同步点）
    if (dryRun_ && dryRun_->done.load(std::memory_order_acquire)) {
        if (dryRun_->thread.joinable()) dryRun_->thread.join();
        // run 结局从最后一次 agent_run 行判定（completed/failed/limited/cancelled）
        testRunResult_ = "已完成试运行，查看执行记录页确认结果";
        dryRun_.reset();
    }

    // 触发提示文案（仅 step 1 用；与旧版同源）
    std::string triggerHint;
    if (isSchedule) {
        triggerHint = cronPreview_.empty() ? kTriggerHints[kTriggerScheduleIdx] : "";
    } else if (isOnce) {
        triggerHint = oncePreview_.empty() ? kTriggerHints[kTriggerOnceIdx] : "";
    } else if (editTriggerIdx_ >= 0 && editTriggerIdx_ < kTriggerCount) {
        triggerHint = kTriggerHints[editTriggerIdx_];
    }

    components::dialog(ui, "agent.dialog.task")
        .open(taskDialogOpen_)
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .onOpenChange([this](bool v) {
            if (!v) closeTaskDialog();
        })
        .content([&] {
            if (!taskDialogOpen_) return;
            // 标题栏独立段 (0,0) W×44：标题带内垂直居中（行高 28），底部 1px 分隔线
            ui_kit::drawDialogTitle(ui, "agent.dialog.task.title", w,
                                    isEdit ? "编辑任务" : "新建智能任务",
                                    20.0f, t.colors.textPrimary, {.y = 8.0f});
            ui.rect("agent.dialog.task.titlebar.edge")
                .position(0.0f, kContentY - 1.0f)
                .size(w, 1.0f)
                .color(t.colors.borderDefault)
                .build();

            // ✕ 关闭（带内垂直居中）
            ui.text("agent.dialog.task.close")
                .position(w - 56.0f, 8.0f)
                .size(28.0f, 28.0f)
                .text("✕")
                .fontSize(14.0f)
                .lineHeight(28.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .onClick([this] { closeTaskDialog(); })
                .build();

            // ====== 左侧竖向导航 (0,44) 160×(H-44)：nav-item 高 56，序号圆 r=11 + 标签/副标竖排 ======
            // 四态：done=#E6FBF2 底+#36D399 字+✓ / cur=#36D399 填充+白字+左3px绿条
            //       cur-err=#EF4444 10% 底+errorText 红字+!+左3px红条 / todo=hover 底+textSecondary 字
            {
                // 侧栏底（#FAFAF9）+ 右缘 1px 边线
                ui.rect("agent.dialog.task.nav")
                    .position(0.0f, kContentY)
                    .size(kVertNavW, h - kContentY)
                    .color(t.colors.background)
                    .build();
                ui.rect("agent.dialog.task.nav.edge")
                    .position(kVertNavW, kContentY)
                    .size(1.0f, h - kContentY)
                    .color(t.colors.borderDefault)
                    .build();

                constexpr float kVertNavItemH = 56.0f;
                constexpr float kVertNumD = 22.0f;   // 序号圆直径（r=11）
                constexpr const char* kStepSubs[] = {
                    "任务名称", "六类 + cron", "目标 / 系统提示词", "模型 / 限次 / 安全组"};
                for (int i = 0; i < kStepCount; ++i) {
                    std::string nodeId = "agent.dialog.task.step." + std::to_string(i);
                    const float navY = kContentY + static_cast<float>(i) * kVertNavItemH;
                    // 当前步校验失败 → 节点标红（#EF4444 10% 底 + errorText 红字 + !）
                    const bool stepInvalid = (i == currentStep_) && !stepValidFor(i);
                    const bool done = i < currentStep_;
                    const bool cur = i == currentStep_;
                    const auto itemBg = done ? t.colors.primarySoftBg
                        : stepInvalid ? eui::mixColor(t.colors.background, t.colors.error, 0.10f)
                        : cur ? t.colors.primary : t.colors.hover;
                    const auto itemFg = done ? t.colors.primary
                        : stepInvalid ? t.colors.errorText
                        : cur ? t.colors.onPrimary : t.colors.textSecondary;
                    // 可点面（可点元素 id 本身带 onClick，测试锚 <nodeId>.bg）
                    ui.rect(nodeId + ".bg")
                        .position(0.0f, navY)
                        .size(kVertNavW, kVertNavItemH)
                        .color(itemBg)
                        .onClick([this, i] { currentStep_ = i; })
                        .build();
                    // cur / cur-err 左 3px 态条
                    if (cur) {
                        ui.rect(nodeId + ".bar")
                            .position(0.0f, navY)
                            .size(3.0f, kVertNavItemH)
                            .color(stepInvalid ? t.colors.error : t.colors.primary)
                            .build();
                    }
                    // 序号圆（done=✓ / cur-err=!，其余显序号）
                    const float numY = navY + (kVertNavItemH - kVertNumD) * 0.5f;
                    ui.rect(nodeId + ".num")
                        .position(14.0f, numY)
                        .size(kVertNumD, kVertNumD)
                        .radius(kVertNumD * 0.5f)
                        .color(done ? t.colors.primarySoftBg
                            : stepInvalid ? t.colors.error
                            : cur ? t.colors.primary : t.colors.hover)
                        .build();
                    ui.text(nodeId + ".num.text")
                        .position(14.0f, numY)
                        .size(kVertNumD, kVertNumD)
                        .text(done ? "✓" : stepInvalid ? "!" : std::to_string(i + 1))
                        .fontSize(12.0f)
                        .lineHeight(kVertNumD)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(done ? t.colors.primary
                            : (stepInvalid || cur) ? t.colors.onPrimary
                                                   : t.colors.textSecondary)
                        .build();
                    // 标签 + 副标竖排（圆右 8px，块在 56 高内垂直居中）
                    ui.text(nodeId + ".label")
                        .position(44.0f, navY + 12.0f)
                        .size(kVertNavW - 44.0f - 14.0f, 18.0f)
                        .text(kStepLabels[i])
                        .fontSize(14.0f)
                        .lineHeight(18.0f)
                        .fontWeight(600)
                        .wrap(false)
                        .color(itemFg)
                        .build();
                    ui.text(nodeId + ".sub")
                        .position(44.0f, navY + 30.0f)
                        .size(kVertNavW - 44.0f - 14.0f, 14.0f)
                        .text(kStepSubs[i])
                        .fontSize(12.0f)
                        .lineHeight(14.0f)
                        .wrap(false)
                        // 白字只压 valid 当前步绿底；无效态粉底仍走 textSecondary
                        //（白字压粉底仅 1.18:1；label 走 errorText 为 5.46:1）
                        .color((cur && !stepInvalid) ? t.colors.onPrimary
                                                     : t.colors.textSecondary)
                        .build();
                }
            }

            // ====== 内容区（按 currentStep_ 切显） ======
            {
                // --- Step 0: 基础信息（名称） ---
                if (currentStep_ == 0) {
                    fieldCaption(ui, "agent.dialog.task.name.caption", "任务名称",
                                 x0, kContentY + kVertContentPad, kDialogLabelW, 24.0f,
                                 t.colors.textSecondary);
                    components::input(ui, "agent.dialog.task.name")
                        .position(x0, kContentY + kVertContentPad + 28.0f)
                        .size(kContentW, 40.0f)
                        .value(editName_)
                        .placeholder("给任务起个名字，留空则按任务目标自动取名")
                        .fontSize(t.sizes.fontSizeBody)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { editName_ = v; })
                        .onCancel([this] { closeTaskDialog(); })
                        .build();
                }

                // --- Step 1: 触发 ---
                if (currentStep_ == 1) {
                    const float s1y = kContentY + kVertContentPad;
                    // 触发 segment（六类触发平分整行；保持旧 id）
                    fieldCaption(ui, "agent.dialog.task.trigger.caption", "触发方式",
                                 x0, s1y, kDialogLabelW, 16.0f,
                                 t.colors.textSecondary);
                    {
                        constexpr float kGap = 6.0f;
                        const float opW = (kContentW - kGap * (kTriggerCount - 1)) /
                                         static_cast<float>(kTriggerCount);
                        ui.stack("agent.dialog.task.trigger")
                            .position(x0, s1y + 20.0f)
                            .size(kContentW, 32.0f)
                            .content([&] {
                                for (int i = 0; i < kTriggerCount; ++i) {
                                    const bool on = editTriggerIdx_ == i;
                                    components::button(
                                        ui, "agent.dialog.task.trigger.item." +
                                                std::to_string(i))
                                        .position(static_cast<float>(i) * (opW + kGap),
                                                   0.0f)
                                        .size(opW, 32.0f)
                                        .text(kTriggerLabels[i])
                                        .fontSize(12.0f)
                                        .colors(on ? t.colors.primary
                                                   : t.colors.background,
                                                on ? t.colors.primaryHover
                                                   : t.colors.hover,
                                                on ? t.colors.primaryPressed
                                                   : t.colors.primarySoftBg)
                                        .textColor(on ? t.colors.onPrimary
                                                      : t.colors.textPrimary)
                                        .radius(8.0f)
                                        .border(1.0f,
                                                on ? t.colors.primary
                                                   : t.colors.borderDefault)
                                        .onClick([this, i] {
                                            editTriggerIdx_ = i;
                                            if (i == kTriggerOnceIdx) refreshOncePreview();
                                            else if (i == kTriggerScheduleIdx) refreshCronPreview();
                                        })
                                        .build();
                                }
                            })
                            .build();
                    }

                    // 触发配置行（cron / 闹钟 / 事件 chips）
                    const float cfgY = s1y + 60.0f;
                    if (isSchedule) {
                        fieldCaption(ui, "agent.dialog.task.cron.caption", "cron 表达式",
                                     x0, cfgY, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        const float cronX = x0 + kDialogLabelW;
                        const float tplW = 110.0f;
                        const float cronW = kContentW - kDialogLabelW - tplW - kDialogGap;
                        components::input(ui, "agent.dialog.task.cron")
                            .position(cronX, cfgY)
                            .size(cronW, kDialogRowH)
                            .value(editCron_)
                            .placeholder("分 时 日 月 周，例如 0 9 * * 1-5")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(themeTokens)
                            .onChange([this](const std::string& v) { setEditCronDraft(v); })
                            .onCancel([this] { closeTaskDialog(); })
                            .build();
                        ui.stack("agent.dialog.task.cron.template.wrap")
                            .position(cronX + cronW + kDialogGap, cfgY)
                            .size(tplW, kDialogRowH)
                            .content([&] {
                                std::vector<std::string> items;
                                items.reserve(kCronTemplateCount + 1);
                                items.emplace_back("选择模板…");
                                for (int i = 0; i < kCronTemplateCount; ++i)
                                    items.emplace_back(kCronTemplates[i].label);
                                components::dropdown(ui, "agent.dialog.task.cron.template")
                                    .size(tplW, kDialogRowH)
                                    .theme(themeTokens)
                                    .items(std::move(items))
                                    .selected(0)
                                    .open(editCronTemplateOpen_)
                                    .onOpenChange([this](bool v) { editCronTemplateOpen_ = v; })
                                    .onChange([this](int idx) {
                                        if (idx >= 1 && idx <= kCronTemplateCount)
                                            setEditCronDraft(kCronTemplates[idx - 1].expr);
                                    })
                                    .build();
                            })
                            .build();
                    }
                    if (isOnce) {
                        fieldCaption(ui, "agent.dialog.task.once.caption", "闹钟时间",
                                     x0, cfgY, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        components::input(ui, "agent.dialog.task.once")
                            .position(x0 + kDialogLabelW, cfgY)
                            .size(kContentW - kDialogLabelW, kDialogRowH)
                            .value(editOnceAtText_)
                            .placeholder("YYYY-MM-DD HH:MM（本地时间，24 小时制）")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(themeTokens)
                            .onChange([this](const std::string& v) { setEditOnceDraft(v); })
                            .onCancel([this] { closeTaskDialog(); })
                            .build();
                    }
                    if (isEvent) {
                        fieldCaption(ui, "agent.dialog.task.event.caption", "监听事件",
                                     x0, cfgY, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        const float evX = x0 + kDialogLabelW;
                        const float evW = (kContentW - kDialogLabelW - kDialogGap * 2.0f) /
                                          static_cast<float>(kEventCount);
                        for (int i = 0; i < kEventCount; ++i) {
                            const bool on = editEvents_[i];
                            components::button(
                                ui, "agent.dialog.task.event." + std::to_string(i))
                                .position(evX + static_cast<float>(i) * (evW + kDialogGap),
                                          cfgY)
                                .size(evW, 32.0f)
                                .text(kEventLabels[i])
                                .fontSize(12.0f)
                                .colors(on ? eui::mixColor(t.colors.surface,
                                                           t.colors.primary, 0.12f)
                                           : t.colors.background,
                                        t.colors.hover,
                                        t.colors.primarySoftBg)
                                .textColor(on ? t.colors.primaryText
                                              : t.colors.textSecondary)
                                .radius(16.0f)
                                .border(1.0f,
                                        on ? eui::mixColor(t.colors.surface,
                                                          t.colors.primary, 0.24f)
                                           : t.colors.borderDefault)
                                .onClick([this, i] { editEvents_[i] = !editEvents_[i]; })
                                .build();
                        }
                    }

                    // 触发提示 / 预览行
                    const float hintY = cfgY + 44.0f;
                    ui_kit::drawDialogText(ui, "agent.dialog.task.trigger.hint", w,
                                           triggerHint, t.sizes.fontSizeSmall,
                                           t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                    ui_kit::drawDialogText(ui, "agent.dialog.task.cron.preview", w,
                                           cronPreview_,
                                           t.sizes.fontSizeSmall,
                                           cronPreviewIsError_ ? t.colors.errorText
                                                               : t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                    ui_kit::drawDialogText(ui, "agent.dialog.task.once.preview", w,
                                           oncePreview_,
                                           t.sizes.fontSizeSmall,
                                           oncePreviewIsError_ ? t.colors.errorText
                                                               : t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                }

                // --- Step 2: 指令 ---
                if (currentStep_ == 2) {
                    const float s2y = kContentY + kVertContentPad;
                    // 指令标签行 + AI 起草 Outline 次按钮（白色底 + 主色边框，W4 C1）
                    fieldCaption(ui, "agent.dialog.task.goal.caption",
                                 "指令（发给 AI 的 prompt）", x0, s2y,
                                 kContentW - 116.0f, 24.0f, t.colors.textSecondary);
                    {
                        const float chipW = 96.0f;
                        const float chipH = 32.0f;
                        components::button(ui, "agent.dialog.task.aichip")
                            .position(w - kVertContentPad - chipW, s2y)
                            .size(chipW, chipH)
                            .text("✎ AI 起草")
                            .fontSize(13.0f)
                            .colors(t.colors.background,
                                    t.colors.primarySoftBg,
                                    t.colors.primarySoftBg)
                            .textColor(t.colors.primary)
                            .radius(8.0f)
                            .border(1.0f, t.colors.primary)
                            .onClick([this] { openDraftDialog(); })
                            .build();
                    }
                    // 指令 textarea
                    const float goalInY = s2y + 26.0f;
                    constexpr float kStepGoalH = 60.0f;
                    components::input(ui, "agent.dialog.task.goal")
                        .position(x0, goalInY)
                        .size(kContentW, kStepGoalH)
                        .value(editGoal_)
                        .placeholder("描述这个任务要做什么，例如：把逾期的待办全部完成")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(true)
                        .scrollbar(true)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { editGoal_ = v; })
                        .onCancel([this] { closeTaskDialog(); })
                        .build();
                    ui_kit::drawDialogText(
                        ui, "agent.dialog.task.goal.valid", w,
                        editGoal_.empty() ? "请填写任务目标" : "",
                        t.sizes.fontSizeSmall, t.colors.errorText,
                        {.x = x0, .y = goalInY + kStepGoalH + 4.0f, .height = 14.0f,
                         .lineHeight = 14.0f, .skipIfEmpty = true});

                    // 执行记忆已并入系统提示词（W4 C3）：只保留 systemPrompt 单字段
                    // 系统提示词
                    const float sysY = goalInY + kStepGoalH + 24.0f;
                    fieldCaption(ui, "agent.dialog.task.sysprompt.caption",
                                 "系统提示词（发给 AI 的环境上下文，可编辑）",
                                 x0, sysY, kContentW, 16.0f, t.colors.textSecondary);
                    constexpr float kStepSysH = 54.0f;
                    components::input(ui, "agent.dialog.task.sysprompt")
                        .position(x0, sysY + 20.0f)
                        .size(kContentW, kStepSysH)
                        .value(editSystemPrompt_)
                        .placeholder("系统提示词，运行时注入；留空则使用自动生成的默认提示词")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(true)
                        .scrollbar(true)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { editSystemPrompt_ = v; })
                        .onCancel([this] { closeTaskDialog(); })
                        .build();
                }

                // --- Step 3: 执行与安全 ---
                if (currentStep_ == 3) {
                    const float s3y = kContentY + kVertContentPad;
                    // 模型槽位（左）| 迭代上限（中）| 只跑一次（右）
                    const float slotDdW = 116.0f;
                    fieldCaption(ui, "agent.dialog.task.slot.caption", "模型槽位",
                                 x0, s3y, kDialogLabelW, kDialogRowH,
                                 t.colors.textSecondary);
                    ui.stack("agent.dialog.task.slot.wrap")
                        .position(x0 + kDialogLabelW + kDialogGap, s3y)
                        .size(slotDdW, kDialogRowH)
                        .content([&] {
                            components::dropdown(ui, "agent.dialog.task.slot")
                                .size(slotDdW, kDialogRowH)
                                .theme(themeTokens)
                                .items(std::vector<std::string>(
                                    kModelSlotLabels, kModelSlotLabels + kModelSlotCount))
                                .selected(slotIndexOf(editSlot_))
                                .open(editSlotOpen_)
                                .onOpenChange([this](bool v) { editSlotOpen_ = v; })
                                .onChange([this](int idx) {
                                    if (idx >= 0 && idx < kModelSlotCount)
                                        editSlot_ = kModelSlotKeys[idx];
                                })
                                .build();
                        })
                        .build();
                    {
                        const float iterLabelX = x0 + kDialogLabelW + kDialogGap +
                                                 slotDdW + kDialogGap;
                        fieldCaption(ui, "agent.dialog.task.iter.caption", "迭代上限",
                                     iterLabelX, s3y, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        components::input(ui, "agent.dialog.task.iter")
                            .position(iterLabelX + kDialogLabelW + kDialogGap, s3y)
                            .size(80.0f, kDialogRowH)
                            .value(std::to_string(editIter_))
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(themeTokens)
                            .onChange([this](const std::string& v) {
                                editIter_ = parseIterInput(v);
                            })
                            .onCancel([this] { closeTaskDialog(); })
                            .build();
                        const bool supportsRunLimit = isSchedule || isEvent;
                        if (supportsRunLimit) {
                            const float limitX = iterLabelX + kDialogLabelW +
                                                 kDialogGap + 80.0f + kDialogGap;
                            components::toggleSwitch(ui, "agent.dialog.task.limit")
                                .position(limitX, s3y)
                                .size(kContentW - (limitX - x0), 32.0f)
                                .text(runLimitText(editMaxRuns_ > 0 ? editMaxRuns_ : 1))
                                .checked(editMaxRuns_ > 0)
                                .fontSize(12.0f)
                                .theme(themeTokens)
                                .onChange([this](bool v) {
                                    if (!v) editMaxRuns_ = 0;
                                    else if (editMaxRuns_ < 1) editMaxRuns_ = 1;
                                })
                                .build();
                        }
                    }

                    // meta 提示（保持 "受限" "高危" 关键词）
                    const float hintY = s3y + 52.0f;
                    ui_kit::drawDialogText(
                        ui, "agent.dialog.task.meta.hint", w,
                        "迭代＝单次运行的最多轮数，跑满以「受限」结束、不再重试；"
                        "破坏性操作＝允许删除文件等高危工具，不勾则被拒",
                        t.sizes.fontSizeSmall, t.colors.textTertiary,
                        {.x = x0, .y = hintY, .height = 12.0f, .lineHeight = 12.0f});

                    // 安全组
                    const float safeY = hintY + 18.0f;
                    ui.text("agent.dialog.task.group.danger")
                        .position(x0, safeY)
                        .size(kContentW, kGroupTitleH)
                        .text("安全")
                        .fontSize(14.0f)
                        .lineHeight(kGroupTitleH)
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                    {
                        const float destructiveY = safeY + kGroupTitleH + 4.0f;
                        if (editDestructive_) {
                            ui.text("agent.dialog.task.destructive.warn")
                                .position(x0, destructiveY)
                                .size(20.0f, 32.0f)
                                .text("⚠")
                                .fontSize(14.0f)
                                .lineHeight(32.0f)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.errorText)
                                .build();
                        }
                        ui.text("agent.dialog.task.destructive.label")
                            .position(x0 + (editDestructive_ ? 24.0f : 0.0f), destructiveY)
                            .size(160.0f, 32.0f)
                            .text("允许破坏性操作")
                            .fontSize(12.0f)
                            .lineHeight(32.0f)
                            .wrap(false)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(editDestructive_ ? t.colors.errorText
                                                     : t.colors.textSecondary)
                            .build();
                        ui_kit::wswitch(ui, "agent.dialog.task.destructive",
                                        editDestructive_,
                                        x0 + 200.0f, destructiveY + 4.0f, 44.0f, 24.0f, t,
                                        [this](bool v) { editDestructive_ = v; });
                        // 自动记录开关已移除（W4 C3）：行为固定为开启，字段五处清链
                    }
                }
            }

            // ====== 试运行结果行（W4 C1：移除去全局错误行，出错标节点不弹全局错误） ======
            // 锚内容区底缘内侧（底栏上方 18px），新竖向案内容区与底栏间无过渡带
            ui_kit::drawDialogText(
                ui, "agent.dialog.task.test.result", w, testRunResult_,
                t.sizes.fontSizeSmall, t.colors.success,
                {.x = x0, .y = h - kContentY - 18.0f, .height = 14.0f,
                 .lineHeight = 14.0f, .skipIfEmpty = true});

            // ====== 底部按钮区（W4 C1 重做：40px 高、8px 间距、居中文字；按钮顺序从右到左：下一步 Filled 最右 → 上一步 Outline → 测试 Outline → 取消 Ghost 最左） ======
            const float btnY = h - kContentY;   // 底栏带 (164, H-44) 高 44，按钮右对齐
            const float btnH = 40.0f;
            const float bw = 96.0f;
            const float testW = 112.0f;
            const float kBtnGap = 8.0f;
            const float rightEdge = w - kVertContentPad;   // 与内容区共用右留 16px

            // 从右到左逐按钮排位，cursor 追随后退
            float xCursor = rightEdge;

            // 下一步/创建/保存（Filled，最右）
            const bool isLastStep = currentStep_ == kStepCount - 1;
            const bool canNext = stepValidFor(currentStep_);
            const char* nextLabel = isLastStep ? "保存" : "下一步";  // 末步定死「保存」，不区分编辑/新建态
            const float saveX = xCursor - bw;
            components::button(ui, "agent.dialog.task.save")
                .position(saveX, btnY)
                .size(bw, btnH)
                .text(nextLabel)
                .fontSize(14.0f)
                .colors(canNext ? t.colors.primary
                                : t.colors.background,
                        canNext ? t.colors.primaryHover
                                : t.colors.hover,
                        canNext ? t.colors.primaryPressed
                                : t.colors.primarySoftBg)
                .textColor(canNext ? t.colors.onPrimary
                                   : t.colors.textTertiary)
                .radius(8.0f)
                .border(1.0f, canNext ? t.colors.primary
                                      : t.colors.borderDefault)
                .onClick(canNext ? std::function<void()>{[this, isLastStep] {
                    if (isLastStep) {
                        saveTaskDialog();
                    } else {
                        currentStep_ = std::min(kStepCount - 1, currentStep_ + 1);
                    }
                }} : std::function<void()>{})
                .build();
            xCursor -= bw + kBtnGap;

            // 上一步（Outline）—— 非首步才渲染
            if (currentStep_ > 0) {
                const float prevX = xCursor - bw;
                components::button(ui, "agent.dialog.task.prev")
                    .position(prevX, btnY)
                    .size(bw, btnH)
                    .text("上一步")
                    .fontSize(14.0f)
                    .colors(t.colors.background,
                            t.colors.hover,
                            t.colors.primarySoftBg)
                    .textColor(t.colors.textPrimary)
                    .radius(8.0f)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { currentStep_ = std::max(0, currentStep_ - 1); })
                    .build();
                xCursor -= bw + kBtnGap;
            }

            // 测试（Outline）—— 仅最后一步渲染
            if (isLastStep) {
                const bool testDisabled = !canNext || dryRun_ != nullptr;
                const std::string testLabel = dryRun_ ? "测试中…" : "测试试运行";
                const float testX = xCursor - testW;
                components::button(ui, "agent.dialog.task.test")
                    .position(testX, btnY)
                    .size(testW, btnH)
                    .text(testLabel)
                    .fontSize(14.0f)
                    .colors(testDisabled ? t.colors.background
                                         : t.colors.background,
                            testDisabled ? t.colors.hover
                                         : t.colors.primary,
                            testDisabled ? t.colors.primarySoftBg
                                         : t.colors.primarySoftBg)
                    .textColor(testDisabled ? t.colors.textTertiary
                                            : t.colors.primary)
                    .radius(8.0f)
                    .border(1.0f, testDisabled ? t.colors.borderDefault
                                               : t.colors.primary)
                    .onClick(testDisabled ? std::function<void()>{}
                                         : std::function<void()>{[this] { testRunDialog(); }})
                    .build();
                xCursor -= testW + kBtnGap;
            }

            // 取消（Ghost，最左）
            const float cancelX = xCursor - bw;
            components::button(ui, "agent.dialog.task.cancel")
                .position(cancelX, btnY)
                .size(bw, btnH)
                .text("取消")
                .fontSize(14.0f)
                .colors(t.colors.background,
                        t.colors.primarySoftBg,
                        t.colors.primarySoftBg)
                .textColor(t.colors.accentText)
                .radius(8.0f)
                .border(0.0f, t.colors.borderDefault)
                .onClick([this] { closeTaskDialog(); })
                .build();
        })
        .build();
}

// ---- AI 草稿子弹窗（T7b）----
//
// 异步时序：主线程 startDraftGeneration 起 worker（requestTaskDraft → parseTaskDraft →
// 写槽位 → done.release → requestUiUpdate 要帧）；主线程 compose 每帧 reapDraftJob
//（done.acquire → join → 预填/报错/丢弃）。worker 全程不摸 UI 成员（输入/槽位值拷贝）；
// onStop join 先于 llm_.reset()；关闭子弹窗只 bump epoch 作废结果，请求不杀
//（DirectLlmClient 无取消口，已定案不动）。

void AgentModule::composeDraftDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = kDraftDialogH;
    const float x0 = ui_kit::kDialogInsetX;
    const float innerW = w - ui_kit::kDialogBodyInsetX;
    const bool generating = draftPhase_ == DraftPhase::Generating;

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.task.draft";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = draftDialogOpen_;
    frame.zIndex = 1100;   // 叠在任务弹窗（组件默认 1000）之上；遮罩同时挡住外层
    frame.onOpenChange = [this](bool v) {
        if (!v) closeDraftDialog();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!draftDialogOpen_) return;
        ui_kit::drawDialogTitle(ui, "agent.dialog.task.draft.title", w,
                                "AI 生成草稿", 20.0f,
                                t.colors.textPrimary, {.y = 16.0f});

        // ✕ 关闭（与遮罩 / Esc / 「关闭」按钮同一路径）
        ui.text("agent.dialog.task.draft.close")
            .position(w - 56.0f, 22.0f)
            .size(28.0f, 28.0f)
            .text("✕")
            .fontSize(14.0f)
            .lineHeight(28.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .onClick([this] { closeDraftDialog(); })
            .build();

        ui_kit::drawDialogText(
            ui, "agent.dialog.task.draft.caption", w,
            "用一句话描述这个任务，AI 生成表单草稿；结果只预填，保存仍由你确认",
            t.sizes.fontSizeSmall, t.colors.textSecondary,
            {.x = x0, .y = 64.0f, .height = 18.0f, .lineHeight = 18.0f});

        components::input(ui, "agent.dialog.task.draft.input")
            .position(x0, 92.0f)
            .size(innerW, kDraftInputH)
            .value(draftInput_)
            .placeholder("例如：每天早上 8 点看一下热榜，有大新闻就通知我")
            .fontSize(t.sizes.fontSizeBody)
            .multiline(true)
            .scrollbar(true)
            .theme(themeTokens)
            .onChange([this](const std::string& v) { draftInput_ = v; })
            .onCancel([this] { closeDraftDialog(); })   // 输入框内 Esc = 关子弹窗
            .build();

        // 底部按钮带（UAT6 重排）：生成=Primary 微信绿（核心确认）、关闭=Secondary
        //（次要），两钮同排整体居中、行顶 y=h-60（与 dialog_form 按钮带同口径）。
        const float btnY = h - 60.0f;
        const float btnX0 = (w - (kDraftBtnW * 2.0f + kDraftBtnGap)) * 0.5f;
        components::button(ui, "agent.dialog.task.draft.generate")
            .position(btnX0, btnY)
            .size(kDraftBtnW, kDraftBtnH)
            .text(generating ? "生成中…" : "生成草稿")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, true)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radius)
            .disabled(generating)
            .onClick([this] { startDraftGeneration(); })
            .build();
        components::button(ui, "agent.dialog.task.draft.dismiss")
            .position(btnX0 + kDraftBtnW + kDraftBtnGap, btnY)
            .size(kDraftBtnW, kDraftBtnH)
            .text("关闭")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, false)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { closeDraftDialog(); })
            .build();

        // 结果行（Done → notice+warnings；Failed → error；同一槽位互斥渲染）。
        // 错误行走 wechat.warningRed（UAT6 微信化）；警告保留 amber t.colors.warning
        //（微信档无 amber 槽位，WeChat 化模块同款先例：claude_ui_detail 警示行）。
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.notice", w, draftNotice_,
                               t.sizes.fontSizeSmall, t.colors.textSecondary,
                               {.x = x0, .y = kDraftResultY, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.warnings", w,
                               draftWarnings_, t.sizes.fontSizeSmall,
                               t.colors.warning,
                               {.x = x0, .y = kDraftResultY + 18.0f, .height = 44.0f,
                                .lineHeight = 16.0f, .wrap = true,
                                .skipIfEmpty = true});
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.error", w, draftError_,
                               t.sizes.fontSizeSmall, t.colors.errorText,
                               {.x = x0, .y = kDraftResultY, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});
    });
}

void AgentModule::openDraftDialog() {
    draftDialogOpen_ = true;
    std::lock_guard<std::mutex> lock(draftMu_);
    if (draftJob_ != nullptr &&
        !draftJob_->done.load(std::memory_order_acquire)) {
        draftPhase_ = DraftPhase::Generating;   // 请求在途：如实显示生成中
    } else if (draftPhase_ == DraftPhase::Generating) {
        draftPhase_ = DraftPhase::Idle;         // 状态陈旧（job 已被收走）
    }
    // Done/Failed 的提示行保留到下次生成前——重开还能看到上次结果说明
}

void AgentModule::closeDraftDialog() {
    if (!draftDialogOpen_) return;
    draftDialogOpen_ = false;
    // 关闭 = 作废未收割的结果：epoch bump 后 reap 走丢弃分支（请求本身不杀）。
    // Generating 阶段位保留：重开如实显示生成中，跑完在 reap 丢弃后回 Idle + 错误行点名
    ++draftEpoch_;
    if (draftPhase_ != DraftPhase::Generating) {
        draftPhase_ = DraftPhase::Idle;
    }
    draftNotice_.clear();
    draftWarnings_.clear();
    draftError_.clear();
}


// 任务删除确认弹窗（E1）：仿 claude 删除确认（Compact 420 / 标题 / 红字正文 / 按钮对）。
// 取消/遮罩/Esc 任一路径只关弹窗不动库；确认才走 performDeleteTask
void AgentModule::composeDeleteConfirmDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Compact);
    const float h = 190.0f;

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.delete";
    frame.width = ui_kit::DialogWidth::Compact;
    frame.height = h;
    frame.open = deleteConfirmTaskId_.has_value();
    frame.zIndex = 1100;   // 叠在任务/详情弹窗（默认 1000）之上，同草稿子弹窗
    frame.onOpenChange = [this](bool v) {
        if (!v) deleteConfirmTaskId_.reset();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!deleteConfirmTaskId_) return;
        // 名字现查 tasks_：待删任务已不存在（他处删了）时如实显示读取失败
        std::string name = "（读取失败）";
        for (const auto& task : tasks_) {
            if (task.id == *deleteConfirmTaskId_) {
                name = task.name;
                break;
            }
        }
        ui_kit::drawDialogTitle(ui, "agent.dialog.delete.title", w, "确认删除？",
                                20.0f, t.colors.textPrimary, {.y = 16.0f});
        ui_kit::drawDialogText(
            ui, "agent.dialog.delete.msg", w,
            "将删除任务「" + name + "」及其全部运行记录，不可恢复。",
            t.sizes.fontSizeSmall, t.colors.errorText,
            {.y = 58.0f, .height = 60.0f, .lineHeight = 20.0f, .wrap = true});

        ui_kit::DialogButtonSpec cancel;
        cancel.label = "取消";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
        cancel.onClick = [this] { deleteConfirmTaskId_.reset(); };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "确认删除";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::DangerThemed;
        confirm.onClick = [this] {
            if (deleteConfirmTaskId_) performDeleteTask(*deleteConfirmTaskId_);
        };
        ui_kit::drawDialogButtons(ui, "agent.dialog.delete", w, h, cancel, confirm,
                                  t, themeTokens);
    });
}

// 确认后的真删除（E1）：store 单事务级联清 runs/messages；关确认弹窗与可能开着的详情
// 弹窗（详情正显示被删任务时留着会渲染空数据），刷列表。失败置页级错误行不吞
void AgentModule::performDeleteTask(int64_t taskId) {
    deleteConfirmTaskId_.reset();
    if (store_ == nullptr) return;
    // E1 二道闸（确认时刻复查）：删除 chip 的禁用态与点击闭包的 busy 都是渲染帧快照，
    // 「弹窗开着 → 确认点击」之间 run 可能已起（scheduler/事件派发不挑时机）。判据与
    // 列表 runBusy 同口径：DB running 行 || 在跑 ActiveRun 任一即挡——scheduler 起的
    // run 不进 runs_，但同样经 store 落 running 行，照挡。scheduler 派发线程不触本闸
    // 读的任何状态（runs_ 走 runsMu_、latestRuns_ 仅主线程），无跨线程读面
    {
        const auto latest = latestRuns_.find(taskId);
        const bool runBusy =
            isTaskRunActive(taskId) ||
            (latest != latestRuns_.end() && latest->second.status == "running");
        if (runBusy) {
            pageError_ = "任务正在运行，请先停止后再删除";
            LOG_WARN("Agent: 删除被拒绝（任务 " + std::to_string(taskId) +
                     " 有在跑 run）");
            return;
        }
    }
    auto r = store_->deleteTask(taskId);
    if (r.isFail()) {
        pageError_ = "删除任务失败：" + r.errorMessage();
        LOG_WARN("Agent: 删除任务失败（task " + std::to_string(taskId) +
                 "）: " + r.errorMessage());
        return;
    }
    if (detailTaskId_ == taskId) closeTaskDetail();
    pageError_.clear();
    tasksDirty_ = true;
    LOG_INFO("Agent: 任务 " + std::to_string(taskId) + " 已删除（含全部运行记录）");
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
