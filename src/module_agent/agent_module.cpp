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
#include "module_agent/app_notify_tool.h"
#include "module_agent/app_stats_tool.h"
#include "module_agent/cron.h"
#include "module_agent/hot_tool.h"
#include "module_agent/message_blocks.h"
#include "module_agent/task_draft_parse.h"
#include "module_agent/tool_info.h"
#include "module_agent/trigger_config.h"

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

// 时长/状态/时间文案 helper 已迁 module_agent/agent_time_text.{h,cpp}（详情弹窗 P2，
// spec §11）：本文件保留列表侧的状态色映射与「去 Claude 页」判定，不再各自拼时间文案。

namespace {

// 列表状态圆点色：只映射既有 theme token，不新造色。completed=success /
// failed=error / limited=warning / running=info；cancelled 与从未运行共用
// tertiary（中性灰 =「无需行动」态，不与四个语义色抢注意力）。点 + 中文文字
// 并存，不只靠色（可访问性双通道）。
eui::Color statusDotColor(const std::string& status,
                          const ::core::theme::Tokens& t) {
    if (status == "completed") return t.colors.success;
    if (status == "failed") return t.colors.error;
    if (status == "limited") return t.colors.warning;
    if (status == "running") return t.colors.info;
    return t.colors.textTertiary;
}

// 行内动作 chip：契约要求 id 落在可见文本上，故手搓两层——<id>.bg 为命中面、文本穿透。
// fontSize 缺省 12（任务列表等既有站点不变）；详情弹窗按 spec §9.2 传 fsS，高度仍 32
void actionChip(::eui::Ui& ui, const std::string& id, const std::string& label,
                float width, bool primary, bool enabled,
                const components::theme::ThemeColorTokens& tokens,
                std::function<void()> onClick, float fontSize = 12.0f) {
    constexpr float kChipH = 32.0f;
    const ::core::Color base = primary ? tokens.primary : tokens.surface;
    ::core::Color textColor =
        primary ? ::core::Color{0.94f, 0.97f, 1.0f, 1.0f} : tokens.text;
    if (!enabled) textColor = components::theme::withOpacity(textColor, 0.40f);
    ui.stack(id + ".chip")
        .size(width, kChipH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, kChipH)
                .states(base, components::theme::buttonHover(tokens, base),
                        components::theme::buttonPressed(tokens, base))
                .radius(tokens.metrics.radius.overlay)
                .border(components::theme::buttonBorder(tokens, primary))
                .onClick(onClick)
                .build();
            ui.text(id)
                .size(width, kChipH)
                .text(label)
                .fontSize(fontSize)
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(textColor)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .onClick(onClick)
                .build();
        })
        .build();
}

// 「去 Claude 页」引导的跳转动作：Router 未装配时仅 LOG_WARN（测试 compose 无 router 不崩）
void navigateToClaudePage() {
    auto* router = ::core::appRouter();
    if (router == nullptr) {
        LOG_WARN("Agent: 跳转 Claude 页失败——Router 未装配");
        return;
    }
    router->navigateTo(::core::PageId::ClaudeProxy);
}

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
// 任务行「配置异常」标记宽：4 个 CJK 字 @12px = 48，留 8 余量（文案再长就换行/截断，
// 原因全文在详情弹窗配置摘要里）
constexpr float kTaskWarnW = 56.0f;
constexpr float kPageErrorH = 16.0f;
// 页内错误行下的「去 Claude 页」引导行高（仅「LLM 配置不可用」类错误渲染）
constexpr float kPageHintH = 16.0f;

// 任务弹窗几何（cron 与触发同行、仅「定时」渲染、弹窗高度不变）
constexpr float kDialogRowH = 40.0f;
constexpr float kDialogLabelW = 72.0f;
constexpr float kDialogGap = 12.0f;
constexpr float kTaskDialogH = 500.0f;
constexpr float kGoalInputH = 96.0f;
constexpr float kContextNoteH = 72.0f;

// AI 草稿子弹窗几何（Standard 460 宽；y 均为本弹窗面板内坐标，与任务弹窗互不影响
// ——外层 500 高 / metaY=224 契约靠「不往外层里塞行」守住，chip 是标题行内绝对定位）
constexpr float kDraftDialogH = 320.0f;
constexpr float kDraftInputH = 88.0f;
constexpr float kDraftGenH = 36.0f;
// 标题行「AI 生成草稿」chip：右缘距 ✕（w-56）一个 gap；高 32 与 actionChip 同款
constexpr float kDraftChipW = 96.0f;
constexpr float kDraftChipH = 32.0f;
constexpr float kDraftChipY = 20.0f;   // 底 52 ≤ goal 行顶 64，不压表单

constexpr float kDetailDialogH = 660.0f;

// 详情弹窗标题行「停止」chip（C1）：长文案把「当前轮结束后生效」说在点击前（协作式取消
// 的诚实口径）；几何同 aichip——标题行内绝对定位，y+32 底 52 ≤ info 行顶 60，不压内容
constexpr float kStopChipW = 176.0f;
constexpr float kStopChipH = 32.0f;
constexpr float kStopChipY = 20.0f;

// 契约：详情弹窗展示最近 5 次 run
// 任务列表每任务只查最近 1 条（列表状态/通知足够，spec §8.6 D8）；
// 详情任务单独查 20 条（时间线，spec D8）
constexpr int kRunListLimit = 1;
constexpr int kDetailRunListLimit = 20;

// run 日志消息区外高钳制在 [kMsgAreaMinH, kMsgAreaMaxH]，超出由 scrollView 内滚
constexpr float kMsgAreaMinH = 96.0f;
constexpr float kMsgAreaMaxH = 520.0f;

constexpr float kToolsHeaderH = 28.0f;
constexpr float kToolCardH = 44.0f;
constexpr float kToolsListMaxH = 240.0f;

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

// trigger_config 的 cron 原文（只读展示）；解析失败显示空串，但非空配置必须留痕
std::string cronTextOf(const AgentTaskRow& task) {
    auto cfg = decodeTriggerConfig(task.triggerConfig);
    if (cfg.isOk()) return cfg.value().cron;
    if (!task.triggerConfig.empty()) {
        LOG_WARN("Agent: 任务 " + std::to_string(task.id) +
                 " 的 trigger_config 无法解析（编辑表单按空 cron 显示）：" +
                 cfg.errorMessage());
    }
    return {};
}

// 限次语义的两种文案（UI 与详情摘要共用同一份口径，防两处措辞漂移）：
//   徽章 = 列表行里 4 字内的短标记；正文 = 详情摘要里的完整说明
// 用「大白话」而不是 max_runs 术语 —— 用户看到的是「只跑一次」不是「上限 1 次」。
// N > 1 只可能来自手改库 / 将来放开 N 次的 UI：文案跟着 N 走，不与「只跑一次」混
std::string runLimitBadge(int maxRuns) {
    if (maxRuns <= 1) return "一次性";
    return "限" + std::to_string(maxRuns) + "次";
}

std::string runLimitText(int maxRuns) {
    if (maxRuns <= 1) return "只跑一次";
    return "最多 " + std::to_string(maxRuns) + " 次";
}

// 闹钟时间文本 ↔ epoch 的纯函数集（trimCopy / parseOnceAtLocalText /
// formatOnceAtText / defaultOnceAtText / onceTextOf）已搬至 task_draft_parse.h：
// 草稿解析层（task_draft.cpp）需要同一份闹钟校验，留在本文件私有会造成
// 「模型草稿说合法、UI 说非法」的双真源。搬移是行为等价的，判定规则与文案逐字未改。

// 弹窗实际宽/高的单边复算：镜像 EUI-NEO components/dialog.h build() 的钳制公式
// min(requested, max(0, screen − spacing.overlay))。DevDesk 拿不到 dialog 实际尺寸
//（drawDialogFrame 无回读），排版前用同一公式复算。overlayInset 必须从
// ThemeColorTokens.metrics.spacing.overlay 读（上游 token 改值时自动跟随），
// 禁写死字面量；若上游改的是公式本身而非 token，此处需同步
float detailDialogActualExtent(float requested, float screenExtent,
                               float overlayInset) {
    return std::min(requested, std::max(0.0f, screenExtent - overlayInset));
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
    loop_.reset();
    llm_.reset();
    store_.reset();
    scheduler_.reset();
    notifyTool_.reset();
    hotTool_.reset();
    statsTool_.reset();
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
    editGoal_.clear();
    editCron_.clear();
    editSlot_ = kModelSlotKeys[0];
    editIter_ = 10;
    editDestructive_ = false;
    editContextNote_.clear();
    editAutoNote_ = true;
    editEvents_ = {};
    editMaxRuns_ = 0;   // 新建缺省 = 不限次数（与旧任务同语义：恒循环）
    editTriggerIdx_ = 0;
    editOnceAtText_ = defaultOnceAtText(
        static_cast<int64_t>(core::time::nowUnix()));   // 切到「闹钟」时就有可用缺省，不逼用户从零拼格式
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
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
    editGoal_ = r.goal;
    editCron_ = cronTextOf(r);
    editOnceAtText_.clear();
    editSlot_ = r.modelSlot;
    editIter_ = r.maxIterations;
    editDestructive_ = r.allowDestructive;
    editContextNote_ = r.contextNote;
    editAutoNote_ = (r.autoNote != 0);
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
    const int autoNoteInt = editAutoNote_ ? 1 : 0;
    if (editingTaskId_) {
        auto updated = store_->updateTask(*editingTaskId_, deriveTaskName(goal), goal,
                                          triggerType, triggerConfig, editSlot_, iter,
                                          editDestructive_, editContextNote_,
                                          autoNoteInt);
        if (updated.isFail()) {
            editError_ = "保存失败：" + updated.errorMessage();
            LOG_ERROR("Agent: 保存任务失败: " + updated.errorMessage());
            return;
        }
    } else {
        auto created = store_->createTask(deriveTaskName(goal), goal, triggerType,
                                          editSlot_, iter, editDestructive_,
                                          triggerConfig, editContextNote_,
                                          autoNoteInt);
        if (created.isFail()) {
            editError_ = "创建失败：" + created.errorMessage();
            LOG_ERROR("Agent: 创建任务失败: " + created.errorMessage());
            return;
        }
    }
    closeTaskDialog();
    tasksDirty_ = true;
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
// 布局态一并清零（窄屏回 tab 0、配置头收起），弹窗不记忆上一次的分支状态
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
    configOpen_ = false;
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
    configOpen_ = false;
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
                            .color(b.isError ? t.colors.error
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
                            .color(b.isError ? t.colors.error
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
    const auto themeTokens = ::core::theme::componentTokens(t);
    const float pad = t.sizes.spacingXXLarge;
    const float contentW = screen.width - pad * 2.0f;

    // 页面帧惰性解析：否则 loop_ 恒空、「启动」永久禁用，惰性解析在真实 UI 上不可达
    if (store_ != nullptr && loop_ == nullptr && tasksDirty_) {
        ensureLlm();
    }

    // 数据刷新：脏标记或有 run 在身（每帧跟进）；查询在主线程，量级可控
    if (store_ != nullptr && (tasksDirty_ || uiPolling_)) {
        refreshData();
    }

    ensureToolsCache(/*forceReload=*/false);
    const float toolsListH =
        toolsOpen_ ? std::min(static_cast<float>(toolsCache_.size()) * kToolCardH,
                              kToolsListMaxH)
                   : 0.0f;
    const float toolsH = kToolsHeaderH + (toolsOpen_ ? kMsgGap + toolsListH : 0.0f);
    // 页 column 子项：标题行 / [错误行] / [引导行] / tools / tasklist；子项数或行高一变，kPageGapCount 与减项必须同步
    const bool hasPageError = !pageError_.empty();
    const bool hasPageHint = hasPageError && needsClaudePageHint(pageError_);
    const int kPageGapCount = 2 + (hasPageError ? 1 : 0) + (hasPageHint ? 1 : 0);
    const float listH = screen.height - pad * 2.0f - kPageHeaderH - toolsH -
                        (hasPageError ? kPageErrorH : 0.0f) -
                        (hasPageHint ? kPageHintH : 0.0f) -
                        static_cast<float>(kPageGapCount) * t.sizes.spacingLarge;

    ui.column("agent.page")
        .size(screen.width, screen.height)
        .padding(pad)
        .gap(t.sizes.spacingLarge)
                .content([&] {
            ui.row("agent.page.header")
                .size(contentW, kPageHeaderH)
                .gap(kDialogGap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    ui.text("agent.title")
                        .size(contentW - kNewTaskBtnW - kDialogGap, 32.0f)
                        .text("智能体")
                        .fontSize(t.sizes.fontSizeTitle)
                        .lineHeight(32.0f)
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                    components::button(ui, "agent.page.newTask")
                        .size(kNewTaskBtnW, 40.0f)
                        .text("+ 新建任务")
                        .theme(themeTokens)
                        .disabled(store_ == nullptr)
                        .onClick([this] { openNewTaskDialog(); })
                        .build();
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
                    .color(t.colors.error)
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
                        components::scrollView(ui, "agent.tools.scroll")
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
                                            inner.rect(cid + ".bg")
                                                .position(0.0f, 0.0f)
                                                .size(cw, kToolCardH)
                                                .color(t.colors.surface)
                                                .radius(t.sizes.radiusSmall)
                                                .build();
                                            inner.text(cid + ".name")
                                                .position(12.0f, 5.0f)
                                                .size(cw - 24.0f, 18.0f)
                                                .text(tool.name)
                                                .fontSize(12.0f)
                                                .lineHeight(18.0f)
                                                .fontWeight(600)
                                                .wrap(false)
                                                .color(t.colors.primary)
                                                .build();
                                            inner.text(cid + ".desc")
                                                .position(12.0f, 23.0f)
                                                .size(cw - 24.0f, 16.0f)
                                                .text(
                                                    tool.description.empty()
                                                        ? "（无描述）"
                                                        : utf8Truncate(
                                                              tool.description,
                                                              100))
                                                .fontSize(11.0f)
                                                .lineHeight(16.0f)
                                                .wrap(false)
                                                .color(t.colors.textSecondary)
                                                .build();
                                            std::string params = "参数：";
                                            if (tool.params.empty()) {
                                                params += "无";
                                            } else {
                                                for (size_t p = 0;
                                                     p < tool.params.size();
                                                     ++p) {
                                                    if (p > 0) params += ", ";
                                                    params += tool.params[p];
                                                }
                                            }
                                            inner.text(cid + ".params")
                                                .position(
                                                    12.0f + 8.0f +
                                                        static_cast<float>(
                                                            tool.name.size()) *
                                                            7.5f,
                                                    5.0f)
                                                .size(std::max(
                                                          40.0f,
                                                          cw - 24.0f - 8.0f -
                                                              static_cast<float>(
                                                                  tool.name
                                                                      .size()) *
                                                                  7.5f),
                                                      18.0f)
                                                .text(params)
                                                .fontSize(11.0f)
                                                .lineHeight(18.0f)
                                                .wrap(false)
                                                .horizontalAlign(
                                                    eui::HorizontalAlign::Right)
                                                .color(t.colors.textTertiary)
                                                .build();
                                        })
                                        .build();
                                }
                            })
                            .build();
                    }
                })
                .build();

            // 任务列表：viewport = 页面剩余高，行高 kTaskRowH（step 同源）；行内动作：启动/详情/编辑/禁用（常驻多一个入队）
            components::scrollView(ui, "agent.tasklist")
                .size(contentW, std::max(0.0f, listH))
                .gap(8.0f)
                .step(kTaskRowH)
                .theme(themeTokens)
                .contentKey("agent.tasklist.n" + std::to_string(tasks_.size()))
                .content([&](eui::Ui& inner, float cw, float) {
                    if (store_ == nullptr) {
                        inner.text("agent.error")
                            .size(cw, 20.0f)
                            .text("数据库未就绪（Agent 模块服务未装配）")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(20.0f)
                            .color(t.colors.error)
                            .build();
                        return;
                    }
                    if (tasks_.empty()) {
                        inner.text("agent.empty")
                            .size(cw, 20.0f)
                            .text("暂无任务——点右上角「+ 新建任务」创建")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(20.0f)
                            .color(t.colors.textTertiary)
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
                        // 宽度分配：状态 80（圆点 8 + gap 12 + 文字 60）/
                        // 详情·编辑·启停·删除 各 48 / [入队 56] / 启动 72 /
                        // [配置异常·已停用 各 56]；childCount 须与实际子元素数一致
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
                        const float fixedW = 80.0f + 48.0f * 4.0f + 72.0f +
                                             (resident ? 56.0f : 0.0f) +
                                             (hasProblem ? kTaskWarnW : 0.0f) +
                                             (limited ? kTaskWarnW : 0.0f) +
                                             (taskOff ? kTaskWarnW : 0.0f);
                        const float titleW =
                            cw - fixedW - rowGap * static_cast<float>(childCount - 1);
                        // 行 = 两个同尺寸层叠在 stack 里：底层 bg rect 承担 hover
                        // 反馈与「点行开详情」命中（容器 row 不画自身背景），上层
                        // row 只做布局；非交互的 row 不拦指针，子 chip 各自消费点击
                        inner.stack(id)
                            .size(cw, kTaskRowH)
                            .content([&] {
                                inner.rect(id + ".bg")
                                    .size(cw, kTaskRowH)
                                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                            t.colors.hover,
                                            eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                    .radius(t.sizes.radiusSmall)
                                    .onClick([this, taskId = task.id] {
                                        openTaskDetail(taskId);
                                    })
                                    .build();
                                inner.row(id + ".row")
                            .size(cw, kTaskRowH)
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
                                        .color(t.colors.error)
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
                                actionChip(inner, id + ".detail", "详情", 48.0f,
                                           /*primary=*/false, /*enabled=*/true,
                                               themeTokens, [this, taskId = task.id] {
                                                   openTaskDetail(taskId);
                                               });
                                actionChip(inner, id + ".edit", "编辑", 48.0f,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this, taskId = task.id] {
                                               openTaskEditor(taskId);
                                           });
                                actionChip(inner, id + ".toggle",
                                           task.enabled ? "禁用" : "启用", 48.0f,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this, taskCopy = task] {
                                               toggleTaskEnabled(taskCopy);
                                           });
                                actionChip(inner, id + ".delete", "删除", 48.0f,
                                           /*primary=*/false, /*enabled=*/!runBusy,
                                           themeTokens,
                                           [this, taskId = task.id, busy = runBusy] {
                                               if (busy) return;   // 禁用态点击无效果（与入队 chip 同守卫法）
                                               deleteConfirmTaskId_ = taskId;
                                           });
                                if (resident) {
                                    // Scheduler 未装配时置灰；入队跑手动子任务
                                    actionChip(inner, id + ".enqueue", "入队", 56.0f,
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
                                components::button(inner, id + ".run")
                                    .size(72.0f, 32.0f)
                                    .text(runStarted ? "运行中…" : "启动")
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

// 任务弹窗：新建/编辑共用同一张表单；关闭态在内容回调内 return——内容节点即开合状态
void AgentModule::composeTaskDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    const float h = kTaskDialogH;
    const float innerW = w - ui_kit::kDialogBodyInsetX;
    const float x0 = ui_kit::kDialogInsetX;
    const bool isSchedule = editTriggerIdx_ == kTriggerScheduleIdx;
    const bool isEvent = editTriggerIdx_ == kTriggerEventIdx;
    const bool isOnce = editTriggerIdx_ == kTriggerOnceIdx;
    const bool isEdit = editingTaskId_.has_value();

    // 触发提示文案：compose 时派生（纯字符串选择，无状态可失步）。定时/闹钟的
    // 预览行非空时让位（下方 trigger.hint 与对应 preview 互斥渲染，同一槽位
    // 同一时刻只有一个信息源）
    std::string triggerHint;
    if (isSchedule) {
        triggerHint =
            cronPreview_.empty() ? kTriggerHints[kTriggerScheduleIdx] : "";
    } else if (isOnce) {
        triggerHint = oncePreview_.empty() ? kTriggerHints[kTriggerOnceIdx] : "";
    } else if (editTriggerIdx_ >= 0 && editTriggerIdx_ < kTriggerCount) {
        triggerHint = kTriggerHints[editTriggerIdx_];
    }

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.task";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = h;
    frame.open = taskDialogOpen_;
    frame.onOpenChange = [this](bool v) {
        if (!v) closeTaskDialog();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!taskDialogOpen_) return;
        ui_kit::drawDialogTitle(ui, "agent.dialog.task.title", w,
                                isEdit ? "编辑任务" : "新建任务",
                                t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

        // ✕ 关闭（与取消 / 遮罩 / Esc 同一路径）
        ui.text("agent.dialog.task.close")
            .position(w - 56.0f, 22.0f)
            .size(28.0f, 28.0f)
            .text("✕")
            .fontSize(14.0f)
            .lineHeight(28.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .onClick([this] { closeTaskDialog(); })
            .build();

        // 「AI 生成草稿」chip（T7b）：标题行内绝对定位，右缘让一个 gap 给 ✕；
        // pill 形（actionChip 的 overlay 圆角）+ 主品牌底（primary=true）。点开
        // 子弹窗，不在外层新增任何行——kTaskDialogH / metaY=224 几何契约不受影响
        ui.stack("agent.dialog.task.aichip.wrap")
            .position(w - 56.0f - kDialogGap - kDraftChipW, kDraftChipY)
            .size(kDraftChipW, kDraftChipH)
            .content([&] {
                actionChip(ui, "agent.dialog.task.aichip", "AI 生成草稿",
                           kDraftChipW, /*primary=*/true, /*enabled=*/true,
                           themeTokens, [this] { openDraftDialog(); });
            })
            .build();

        ui.text("agent.dialog.task.goal.caption")
            .position(x0, 64.0f)
            .size(kDialogLabelW, 20.0f)
            .text("任务目标")
            .fontSize(12.0f)
            .lineHeight(20.0f)
            .verticalAlign(eui::VerticalAlign::Top)
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "agent.dialog.task.goal")
            .position(x0 + kDialogLabelW + kDialogGap, 64.0f)
            .size(innerW - kDialogLabelW - kDialogGap, kGoalInputH)
            .value(editGoal_)
            .placeholder("描述这个任务要做什么，例如：把逾期的待办全部完成")
            .fontSize(t.sizes.fontSizeBody)
            .multiline(true)
            .scrollbar(true)
            .theme(themeTokens)
            .onChange([this](const std::string& v) { editGoal_ = v; })
            .onCancel([this] { closeTaskDialog(); })   // 输入框内 Esc = 关弹窗
            .build();

        const float fieldX = x0 + kDialogLabelW + kDialogGap;
        // 触发 / 槽位下拉宽度。140 → 116：为行为开关行右端的「只跑一次」开关腾位
        // （触发行没有余量——塞在那里会把 cron 提示词挤到截断）。两个下拉最长标签
        // = 4 字（「子代理」48px @16px），字段标签盒 = ddW − 48 = 68 仍富余
        const float ddW = 116.0f;
        const float triggerY = 172.0f;
        fieldCaption(ui, "agent.dialog.task.trigger.caption", "触发方式", x0,
                     triggerY, kDialogLabelW, kDialogRowH, t.colors.textSecondary);
        ui.stack("agent.dialog.task.trigger.wrap")
            .position(fieldX, triggerY)
            .size(ddW, kDialogRowH)
            .content([&] {
                components::dropdown(ui, "agent.dialog.task.trigger")
                    .size(ddW, kDialogRowH)
                    .theme(themeTokens)
                    .items(std::vector<std::string>(kTriggerLabels,
                                                    kTriggerLabels + kTriggerCount))
                    .selected(editTriggerIdx_)
                    .open(editTriggerOpen_)
                    .onOpenChange([this](bool v) { editTriggerOpen_ = v; })
                    .onChange([this](int idx) {
                        editTriggerIdx_ = idx;
                        // 预览行随类型切换重算：切到闹钟立即显示缺省时间是否合法
                        //（不重算的话预览行还停在上一个类型的文案 / 空白）
                        if (idx == kTriggerOnceIdx) {
                            refreshOncePreview();
                        } else if (idx == kTriggerScheduleIdx) {
                            refreshCronPreview();
                        }
                    })
                    .build();
            })
            .build();
        if (isEvent) {
            // 事件分支：触发下拉右侧同行放三个事件开关（与 schedule 分支的 cron 行
            // 同一行、互斥渲染；不新增行——kTaskDialogH/metaY 几何契约不许动）
            const float evX = fieldX + ddW + kDialogGap;
            const float evW = innerW - (evX - x0);
            const float itemW = (evW - kDialogGap * 2.0f) / 3.0f;
            for (int i = 0; i < kEventCount; ++i) {
                components::toggleSwitch(
                    ui, "agent.dialog.task.event." + std::to_string(i))
                    .position(evX + static_cast<float>(i) * (itemW + kDialogGap),
                              triggerY)
                    .size(itemW, 32.0f)
                    .text(kEventLabels[i])
                    .checked(editEvents_[i])
                    .fontSize(12.0f)
                    .theme(themeTokens)
                    .onChange([this, i](bool v) { editEvents_[i] = v; })
                    .build();
            }
        }
        if (isSchedule) {
            const float cronLabelX = fieldX + ddW + kDialogGap;
            const float cronX = cronLabelX + kDialogLabelW + kDialogGap;
            fieldCaption(ui, "agent.dialog.task.cron.caption", "cron 表达式",
                         cronLabelX, triggerY, kDialogLabelW, kDialogRowH,
                         t.colors.textSecondary);
            // cron 输入右侧让位给模板下拉（选中即回填，宽度预算见下行两条 size）
            const float tplW = 110.0f;
            const float cronW = innerW - (cronX - x0) - tplW - kDialogGap;
            components::input(ui, "agent.dialog.task.cron")
                .position(cronX, triggerY)
                .size(cronW, kDialogRowH)
                .value(editCron_)
                .placeholder("分 时 日 月 周，例如 0 9 * * 1-5")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([this](const std::string& v) { setEditCronDraft(v); })
                .onCancel([this] { closeTaskDialog(); })
                .build();
            ui.stack("agent.dialog.task.cron.template.wrap")
                .position(cronX + cronW + kDialogGap, triggerY)
                .size(tplW, kDialogRowH)
                .content([&] {
                    // selected 恒 0（「选择模板…」占位）——下拉当菜单用：选完不
                    // 留选中态，label 不许变成表达式（表达式在 cron 输入框里）
                    std::vector<std::string> items;
                    items.reserve(kCronTemplateCount + 1);
                    items.emplace_back("选择模板…");
                    for (int i = 0; i < kCronTemplateCount; ++i) {
                        items.emplace_back(kCronTemplates[i].label);
                    }
                    components::dropdown(ui, "agent.dialog.task.cron.template")
                        .size(tplW, kDialogRowH)
                        .theme(themeTokens)
                        .items(std::move(items))
                        .selected(0)
                        .open(editCronTemplateOpen_)
                        .onOpenChange(
                            [this](bool v) { editCronTemplateOpen_ = v; })
                        .onChange([this](int idx) {
                            // idx 0 = 占位项，不回填
                            if (idx >= 1 && idx <= kCronTemplateCount) {
                                setEditCronDraft(kCronTemplates[idx - 1].expr);
                            }
                        })
                        .build();
                })
                .build();
            // 预览行：cron 行与 meta 行之间的原 12px gap 内，弹窗总高不变
            // （kTaskDialogH 与 metaY=224 都不许动，改即破几何契约）
            ui_kit::drawDialogText(
                ui, "agent.dialog.task.cron.preview", w, cronPreview_,
                t.sizes.fontSizeSmall - 1.0f,
                cronPreviewIsError_ ? t.colors.error : t.colors.textTertiary,
                {.x = x0, .y = 212.0f, .height = 12.0f, .lineHeight = 12.0f,
                 .skipIfEmpty = true});
        }
        if (isOnce) {
            // 闹钟分支占用与 cron 同一行 / 同一预览行：不新增行，几何契约不变。
            // 只写一个绝对时刻——cron / 事件勾选 / max_runs 全部不渲染（once_at 与
            // 重复载荷在 encode 层互斥，表单不提供制造坏配置的入口）
            const float onceLabelX = fieldX + ddW + kDialogGap;
            const float onceX = onceLabelX + kDialogLabelW + kDialogGap;
            fieldCaption(ui, "agent.dialog.task.once.caption", "闹钟时间",
                         onceLabelX, triggerY, kDialogLabelW, kDialogRowH,
                         t.colors.textSecondary);
            components::input(ui, "agent.dialog.task.once")
                .position(onceX, triggerY)
                .size(innerW - (onceX - x0), kDialogRowH)
                .value(editOnceAtText_)
                .placeholder("YYYY-MM-DD HH:MM（本地时间，24 小时制）")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([this](const std::string& v) { setEditOnceDraft(v); })
                .onCancel([this] { closeTaskDialog(); })
                .build();
            ui_kit::drawDialogText(
                ui, "agent.dialog.task.once.preview", w, oncePreview_,
                t.sizes.fontSizeSmall - 1.0f,
                oncePreviewIsError_ ? t.colors.error : t.colors.textTertiary,
                {.x = x0, .y = 212.0f, .height = 12.0f, .lineHeight = 12.0f,
                 .skipIfEmpty = true});
        }

        // 触发语义提示行：占触发行与 meta 行之间的 12px 缝（cron/闹钟预览行同款
        // 打法，弹窗高度与各行位置不动）；与预览行互斥渲染，见上方 triggerHint 派生
        ui_kit::drawDialogText(ui, "agent.dialog.task.trigger.hint", w,
                               triggerHint, t.sizes.fontSizeSmall - 1.0f,
                               t.colors.textTertiary,
                               {.x = x0, .y = 212.0f, .height = 12.0f,
                                .lineHeight = 12.0f, .skipIfEmpty = true});

        const float metaY = 224.0f;
        const float iterLabelX = fieldX + ddW + kDialogGap;
        const float iterX = iterLabelX + kDialogLabelW + kDialogGap;
        fieldCaption(ui, "agent.dialog.task.slot.caption", "模型槽位", x0, metaY,
                     kDialogLabelW, kDialogRowH, t.colors.textSecondary);
        ui.stack("agent.dialog.task.slot.wrap")
            .position(fieldX, metaY)
            .size(ddW, kDialogRowH)
            .content([&] {
                components::dropdown(ui, "agent.dialog.task.slot")
                    .size(ddW, kDialogRowH)
                    .theme(themeTokens)
                    .items(std::vector<std::string>(kModelSlotLabels,
                                                    kModelSlotLabels + kModelSlotCount))
                    .selected(slotIndexOf(editSlot_))
                    .open(editSlotOpen_)
                    .onOpenChange([this](bool v) { editSlotOpen_ = v; })
                    .onChange([this](int idx) {
                        if (idx >= 0 && idx < kModelSlotCount) {
                            editSlot_ = kModelSlotKeys[idx];
                        }
                    })
                    .build();
            })
            .build();
        fieldCaption(ui, "agent.dialog.task.iter.caption", "迭代上限", iterLabelX,
                     metaY, kDialogLabelW, kDialogRowH, t.colors.textSecondary);
        components::input(ui, "agent.dialog.task.iter")
            .position(iterX, metaY)
            .size(100.0f, kDialogRowH)
            .value(std::to_string(editIter_))
            .fontSize(t.sizes.fontSizeBody)
            .theme(themeTokens)
            .onChange([this](const std::string& v) { editIter_ = parseIterInput(v); })
            .onCancel([this] { closeTaskDialog(); })
            .build();
        // 用 switch 而非 checkbox（-Werror 下有未使用变量告警）
        const float switchX = iterX + 100.0f + kDialogGap;
        const float switchW = innerW - (switchX - x0);
        // 「只跑一次」开关：与「允许破坏性操作」同排（行为开关行）右端。为什么不进触发行：
        // cron 行宽度预算已被 cron 输入 + 模板下拉占满，塞进去只能把 cron 提示词挤截断。
        // 只对 schedule / event 渲染：manual / resident 的 trigger_config 不被 Scheduler
        // 消费，给不消费该列的任务显示开关等于假承诺。文案随草稿 N 走（手改库的 N>1
        // 不显示成「只跑一次」）。标签盒 = limitW − 66（switch.h: labelX 56 + inset 10），
        // 116 − 66 = 50 ≥ 实测「只跑一次」@12 = 48（test_agent_ui 有标签盒容纳断言钉住）
        const bool supportsRunLimit = isSchedule || isEvent;
        const float limitW = 116.0f;
        const float destructiveW =
            supportsRunLimit ? switchW - limitW - kDialogGap : switchW;
        components::toggleSwitch(ui, "agent.dialog.task.destructive")
            .position(switchX, metaY)
            .size(destructiveW, 32.0f)
            .text("允许破坏性操作")
            .checked(editDestructive_)
            .fontSize(12.0f)
            .theme(themeTokens)
            .onChange([this](bool v) { editDestructive_ = v; })
            .build();
        if (supportsRunLimit) {
            components::toggleSwitch(ui, "agent.dialog.task.limit")
                .position(switchX + destructiveW + kDialogGap, metaY)
                .size(limitW, 32.0f)
                .text(runLimitText(editMaxRuns_ > 0 ? editMaxRuns_ : 1))
                .checked(editMaxRuns_ > 0)
                .fontSize(12.0f)
                .theme(themeTokens)
                .onChange([this](bool v) {
                    // 关掉 → 不限次数（不写该键）；打开 → 至多回落到 1（手改库的 N>1 被
                    // 关掉再打开后回到 1——用户显式动作，接受）
                    if (!v) {
                        editMaxRuns_ = 0;
                    } else if (editMaxRuns_ < 1) {
                        editMaxRuns_ = 1;
                    }
                })
                .build();
        }

        // meta 行提示：占 meta 行与执行记忆行之间的 12px 缝（触发提示同款打法，
        // 不动任何行位置）。迭代上限讲清「跑满会怎样」（受限终态的直接前因），
        // 破坏性开关讲清「勾了会发生什么/不勾会怎样」
        ui_kit::drawDialogText(
            ui, "agent.dialog.task.meta.hint", w,
            "迭代＝单次运行的最多轮数，跑满以「受限」结束、不再重试；"
            "破坏性操作＝允许删除文件等高危工具，不勾则被拒",
            t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
            {.x = x0, .y = 264.0f, .height = 12.0f, .lineHeight = 12.0f});

        // 执行记忆：多行文本输入（与 goal 同款 multiline + scrollbar），
        // 用于 agent 运行时注入 system prompt；编辑态从 AgentTaskRow 回填
        const float noteY = 276.0f;
        fieldCaption(ui, "agent.dialog.task.note.caption", "执行记忆", x0,
                     noteY, kDialogLabelW, kContextNoteH, t.colors.textSecondary);
        components::input(ui, "agent.dialog.task.note")
            .position(x0 + kDialogLabelW + kDialogGap, noteY)
            .size(innerW - kDialogLabelW - kDialogGap, kContextNoteH)
            .value(editContextNote_)
            .placeholder("记录任务执行上下文，运行时注入 system prompt")
            .fontSize(t.sizes.fontSizeBody)
            .multiline(true)
            .scrollbar(true)
            .theme(themeTokens)
            .onChange([this](const std::string& v) { editContextNote_ = v; })
            .onCancel([this] { closeTaskDialog(); })
            .build();

        // 自动记录开关：结束后把最后一条 assistant 回复追加进 context note
        const float autoY = 356.0f;
        const float autoSwitchW = innerW - kDialogLabelW - kDialogGap;
        components::toggleSwitch(ui, "agent.dialog.task.autonote")
            .position(x0 + kDialogLabelW + kDialogGap, autoY)
            .size(autoSwitchW, 32.0f)
            .text("结束后自动记录回复")
            .checked(editAutoNote_)
            .fontSize(12.0f)
            .theme(themeTokens)
            .onChange([this](bool v) { editAutoNote_ = v; })
            .build();

        // 校验 / 保存错误（有错才渲染，弹窗不关）
        ui_kit::drawDialogText(ui, "agent.dialog.task.error", w, editError_,
                               t.sizes.fontSizeSmall, t.colors.error,
                               {.x = x0, .y = 408.0f, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});

        const float btnW =
            (w - ui_kit::kDialogInsetX * 2.0f - kDialogGap) * 0.5f;
        const float btnY = h - 60.0f;
        components::button(ui, "agent.dialog.task.cancel")
            .position(x0, btnY)
            .size(btnW, 40.0f)
            .text("取消")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, false)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { closeTaskDialog(); })
            .build();
        components::button(ui, "agent.dialog.task.save")
            .position(x0 + btnW + kDialogGap, btnY)
            .size(btnW, 40.0f)
            .text("保存")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, true)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radius)
            .onClick([this] { saveTaskDialog(); })
            .build();
    });
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
                                "AI 生成草稿", t.sizes.fontSizeLarge,
                                t.colors.textPrimary);

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

        const float btnY = 196.0f;
        components::button(ui, "agent.dialog.task.draft.generate")
            .position(x0, btnY)
            .size(110.0f, kDraftGenH)
            .text(generating ? "生成中…" : "生成草稿")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, true)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radius)
            .disabled(generating)
            .onClick([this] { startDraftGeneration(); })
            .build();
        components::button(ui, "agent.dialog.task.draft.dismiss")
            .position(x0 + 110.0f + kDialogGap, btnY)
            .size(110.0f, kDraftGenH)
            .text("关闭")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, false)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { closeDraftDialog(); })
            .build();

        // 结果行（Done → notice+warnings；Failed → error；同一槽位互斥渲染）
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.notice", w, draftNotice_,
                               t.sizes.fontSizeSmall, t.colors.textSecondary,
                               {.x = x0, .y = 248.0f, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.warnings", w,
                               draftWarnings_, t.sizes.fontSizeSmall - 1.0f,
                               t.colors.warning,
                               {.x = x0, .y = 268.0f, .height = 44.0f,
                                .lineHeight = 16.0f, .wrap = true,
                                .skipIfEmpty = true});
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.error", w, draftError_,
                               t.sizes.fontSizeSmall, t.colors.error,
                               {.x = x0, .y = 248.0f, .height = 18.0f,
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
                                20.0f, t.colors.textPrimary);
        ui_kit::drawDialogText(
            ui, "agent.dialog.delete.msg", w,
            "将删除任务「" + name + "」及其全部运行记录，不可恢复。",
            t.sizes.fontSizeSmall, t.colors.error,
            {.y = 58.0f, .height = 60.0f, .lineHeight = 20.0f, .wrap = true});

        ui_kit::DialogButtonSpec cancel;
        cancel.label = "取消";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::Secondary;
        cancel.onClick = [this] { deleteConfirmTaskId_.reset(); };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "确认删除";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::Danger;
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

// 详情弹窗布局壳：摘要三卡（常驻）→ 配置头（+覆盖式配置浮层）→ 主区（宽屏左右分栏 /
// 窄屏单列 tab）。所有区域坐标从实际钳制后的 w/h 推导（纯函数镜像上游钳制公式），
// 禁用屏幕绝对坐标；消息区默认折叠（messagesOpen_），复制反馈走页级 toast
void AgentModule::composeTaskDetailDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float overlayInset = themeTokens.metrics.spacing.overlay;
    const float w = detailDialogActualExtent(
        ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form), screen.width,
        overlayInset);
    const float h =
        detailDialogActualExtent(kDetailDialogH, screen.height, overlayInset);
    const float innerW = std::max(0.0f, w - ui_kit::kDialogBodyInsetX);
    const float x0 = ui_kit::kDialogInsetX;

    // 文本高度全部由缩放后字号派生（fontScale 只缩 fontSize，行高按比例重算）
    const float fsS = t.sizes.fontSizeSmall;
    const float fsB = t.sizes.fontSizeBody;
    const float lhS = static_cast<float>(std::lround(fsS * 1.4f));
    const float lhB = static_cast<float>(std::lround(fsB * 1.4f));

    // 响应式档位：宽 ≥680 且高 ≥520 分栏；否则单列 tab。矮宽屏（h<600）右栏整栏滚动
    const bool wide = w >= 680.0f && h >= 520.0f;

    // 区域划分（面板内坐标）
    const float summaryY = 60.0f;
    const float summaryH = wide
                               ? std::max(81.0f, 2.0f * 10.0f + lhS + lhB + lhS + 4.0f)
                               : (h < 600.0f ? 116.0f : 122.0f);
    const float configY = summaryY + summaryH + 8.0f;
    const float configH = std::max(40.0f, lhS + 20.0f);
    const float mainY = configY + configH + 12.0f;
    const float mainH = std::max(56.0f, h - mainY - 24.0f);
    const float leftW = static_cast<float>(
        std::clamp(std::lround(innerW * 0.40f), 240L, 282L));
    const float rightX = x0 + leftW + 20.0f;
    const float rightW = std::max(0.0f, innerW - leftW - 20.0f);
    const float navH = h < 600.0f ? std::max(32.0f, lhS + 8.0f)
                                  : std::max(36.0f, lhS + 12.0f);
    const float navY = configY + configH + 8.0f;
    const float contentY = navY + navH + 8.0f;
    const float contentH = std::max(56.0f, h - contentY - 24.0f);

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.detail";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = h;
    frame.open = detailTaskId_.has_value();
    frame.onOpenChange = [this](bool v) {
        if (!v) closeTaskDetail();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!detailTaskId_) return;
        ui_kit::drawDialogTitle(ui, "agent.dialog.detail.title", w, "任务详情",
                                t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);
        ui.text("agent.dialog.detail.close")
            .position(w - 56.0f, 22.0f)
            .size(28.0f, 28.0f)
            .text("✕")
            .fontSize(14.0f)
            .lineHeight(28.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .onClick([this] { closeTaskDetail(); })
            .build();

        // 「停止」chip（C1）：只在有在跑 run 时渲染——无 ActiveRun 可置位的 run（含崩溃
        // 遗留的 DB running 行）没有可停止对象，渲染入口等于骗点击。点击后切「停止中」
        // 禁用（cancel 位幂等，禁用只为给点击一个反馈，防 A5 型「点了没反应反复点」）
        if (isTaskRunActive(*detailTaskId_)) {
            const bool stopping = isStopRequested(*detailTaskId_);
            ui.stack("agent.dialog.detail.stop.wrap")
                .position(w - 56.0f - kDialogGap - kStopChipW, kStopChipY)
                .size(kStopChipW, kStopChipH)
                .content([&] {
                    actionChip(ui, "agent.dialog.detail.stop",
                               stopping ? "停止中 · 当前轮结束后生效"
                                        : "停止 · 当前轮结束后生效",
                               kStopChipW, /*primary=*/false, /*enabled=*/!stopping,
                               themeTokens, [this, taskId = *detailTaskId_] {
                                   requestStopRun(taskId);
                               }, fsS);
                })
                .build();
        }

        const auto runsIt = taskRuns_.find(*detailTaskId_);
        const std::vector<AgentRunRow>* runs =
            runsIt != taskRuns_.end() ? &runsIt->second : nullptr;
        const bool hasRuns = runs != nullptr && !runs->empty();
        const AgentRunRow* latestRun = hasRuns ? &runs->front() : nullptr;

        const int64_t nowSec = core::time::nowUnix();
        // 「下一次」缓存：键判等早退，nextCronMatch 回扫不在逐帧路径（spec §8.5）
        if (detailTaskRow_) {
            updateNextRunCache(*detailTaskRow_, latestRun, nowSec);
        }

        // —— 摘要三卡（常驻；真实数据，spec §8.2/§8.5）——
        // 汇总查询失败必须显形（静默失败面）：last/stats 值转「读取失败」error 色
        const bool summaryFailed = !runSummariesError_.empty();
        // 最近一次：主值 = 状态 · 耗时（运行中 = now-startedAt；无 ended = 状态裸值）
        std::string lastValue = summaryFailed ? "读取失败" : "从未执行";
        std::string lastSub;
        eui::Color lastValueColor = summaryFailed ? t.colors.error : t.colors.textPrimary;
        if (!summaryFailed && latestRun != nullptr) {
            if (latestRun->status == "running") {
                int64_t secs = nowSec - latestRun->startedAt;
                if (secs < 0) secs = 0;
                lastValue = "运行中 · " + formatRunDurationText(secs);
                lastValueColor = t.colors.info;
            } else if (latestRun->endedAt) {
                int64_t secs = *latestRun->endedAt - latestRun->startedAt;
                if (secs < 0) secs = 0;   // 时钟回拨防御
                lastValue = std::string(runStatusText(latestRun->status)) + " · " +
                            formatRunDurationText(secs);
                lastValueColor = statusDotColor(latestRun->status, t);
            } else {
                lastValue = runStatusText(latestRun->status);
                lastValueColor = statusDotColor(latestRun->status, t);
            }
            lastSub = formatLocalShortDateTime(latestRun->startedAt) + " · " +
                      relativeTimeText(latestRun->startedAt, nowSec);
        }
        // 近 7 天：主值 = 成功 N · 失败 M；副值 = 其中受限 K · P%（分母 = 成功+失败）
        std::string statsValue = summaryFailed ? "读取失败" : "近 7 天暂无执行";
        std::string statsSub;
        const auto sumIt = runSummaries_.find(*detailTaskId_);
        if (!summaryFailed && sumIt != runSummaries_.end()) {
            const AgentTaskRunSummary& s = sumIt->second;
            statsValue = "成功 " + std::to_string(s.okRuns7d) + " · 失败 " +
                         std::to_string(s.failedRuns7d);
            const int64_t denom = s.okRuns7d + s.failedRuns7d;
            const int64_t pct = denom > 0 ? (s.okRuns7d * 100) / denom : 0;
            statsSub = "其中受限 " + std::to_string(s.limitedRuns7d) + " · " +
                       std::to_string(pct) + "%";
        }
        // 下一次：describeNextRun 的真话（手动/事件不造假时间，spec D10/A6）
        const NextRunInfo nextInfo =
            (detailTaskRow_ && nextRunText_) ? *nextRunText_ : NextRunInfo{};
        auto toneColor = [&t](TextTone tone, eui::Color normal) -> eui::Color {
            switch (tone) {
                case TextTone::Muted: return t.colors.textTertiary;
                case TextTone::Warning: return t.colors.warning;
                case TextTone::Error: return t.colors.error;
                case TextTone::Normal: break;
            }
            return normal;
        };
        constexpr float kCardPad = 10.0f;
        // 副值行按卡高自适应：装得下（宽屏 81 ≥ pad*2+lhS+lhB+lhS）才画，窄屏两行卡不画
        auto hasSubRow = [&](float ch) {
            return ch >= 2.0f * kCardPad + lhS + lhB + lhS;
        };
        auto summaryCard = [&](const std::string& id, float cx, float cy, float cw,
                               float ch, const char* label,
                               const std::string& value, eui::Color valueColor,
                               const std::string& sub, eui::Color subColor) {
            ui.stack(id)
                .position(cx, cy)
                .size(cw, ch)
                .content([&] {
                    ui.rect(id + ".bg")
                        .size(cw, ch)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .build();
                    ui.text(id + ".label")
                        .position(kCardPad, kCardPad)
                        .size(std::max(0.0f, cw - 2.0f * kCardPad), lhS)
                        .text(label)
                        .fontSize(fsS)
                        .lineHeight(lhS)
                        .color(t.colors.textTertiary)
                        .build();
                    ui.text(id + ".value")
                        .position(kCardPad, kCardPad + lhS)
                        .size(std::max(0.0f, cw - 2.0f * kCardPad), lhB)
                        .text(value)
                        .fontSize(fsB)
                        .lineHeight(lhB)
                        .wrap(false)
                        .maxWidth(std::max(0.0f, cw - 2.0f * kCardPad))
                        .color(valueColor)
                        .build();
                    // 副值只在卡高装得下时画：窄屏两行卡高装不下第三行（卡高与 §6 的
                    // 122/116 耦合）
                    if (hasSubRow(ch)) {
                        ui.text(id + ".sub")
                            .position(kCardPad, kCardPad + lhS + lhB)
                            .size(std::max(0.0f, cw - 2.0f * kCardPad), lhS)
                            .text(sub)
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .wrap(false)
                            .maxWidth(std::max(0.0f, cw - 2.0f * kCardPad))
                            .color(subColor)
                            .build();
                    }
                })
                .build();
        };
        constexpr float kCardGap = 8.0f;
        if (wide) {
            const float cardW = std::max(0.0f, (innerW - 2.0f * kCardGap) / 3.0f);
            summaryCard("agent.dialog.detail.summary.last", x0, summaryY, cardW,
                        summaryH, "最近一次", lastValue, lastValueColor, lastSub,
                        t.colors.textTertiary);
            summaryCard("agent.dialog.detail.summary.next", x0 + cardW + kCardGap,
                        summaryY, cardW, summaryH, "下一次", nextInfo.value,
                        toneColor(nextInfo.tone, t.colors.textPrimary),
                        nextInfo.detail,
                        toneColor(nextInfo.tone, t.colors.textTertiary));
            summaryCard("agent.dialog.detail.summary.stats",
                        x0 + 2.0f * (cardW + kCardGap), summaryY, cardW, summaryH,
                        "近 7 天", statsValue,
                        summaryFailed ? t.colors.error : t.colors.textPrimary,
                        statsSub, t.colors.textTertiary);
        } else {
            const float rowH = (summaryH - kCardGap) / 2.0f;
            const float halfW = std::max(0.0f, (innerW - kCardGap) / 2.0f);
            summaryCard("agent.dialog.detail.summary.last", x0, summaryY, halfW,
                        rowH, "最近一次", lastValue, lastValueColor, lastSub,
                        t.colors.textTertiary);
            summaryCard("agent.dialog.detail.summary.next", x0 + halfW + kCardGap,
                        summaryY, halfW, rowH, "下一次", nextInfo.value,
                        toneColor(nextInfo.tone, t.colors.textPrimary),
                        nextInfo.detail,
                        toneColor(nextInfo.tone, t.colors.textTertiary));
            summaryCard("agent.dialog.detail.summary.stats", x0,
                        summaryY + rowH + kCardGap, innerW, rowH, "近 7 天",
                        statsValue,
                        summaryFailed ? t.colors.error : t.colors.textPrimary,
                        statsSub, t.colors.textTertiary);
        }

        // —— 配置头（常驻）+ 覆盖式浮层（spec §6.4/§7.2，P3 落地）——
        // hint 是折叠态摘要：触发 · cron（闹钟任务为闹钟时间）· 模型 · 迭代；
        // 目标/限次/破坏性/异常/执行记忆全在浮层字段（点开看全量，信息不丢）
        std::string configHint;
        if (detailTaskRow_) {
            const AgentTaskRow& task = *detailTaskRow_;
            configHint = "触发：" + triggerLabelOf(task.triggerType);
            if (task.triggerType == "schedule") {
                const std::string cron = cronTextOf(task);
                configHint += " · cron：" + (cron.empty() ? "（未设置）" : cron);
            } else if (task.triggerType == "once") {
                // 闹钟时间 = once 型的「定时」等价物，留在折叠摘要
                const std::string onceText = onceTextOf(task);
                configHint += " · 闹钟时间：" +
                              (onceText.empty() ? "（未设置）" : onceText);
            }
            configHint += " · 模型：" + task.modelSlot;
            configHint += " · 迭代：" + std::to_string(task.maxIterations);
        } else {
            configHint = "任务信息读取失败";
        }
        constexpr float kConfigTitleW = 64.0f;   // 「任务配置」4 CJK @15
        constexpr float kConfigToggleW = 40.0f;  // 「展开/收起」2 CJK @13 + 余量
        ui.stack("agent.dialog.detail.config.header")
            .position(x0, configY)
            .size(innerW, configH)
            .content([&] {
                ui.rect("agent.dialog.detail.config.header.bg")
                    .size(innerW, configH)
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .build();
                ui.text("agent.dialog.detail.config.header.title")
                    .position(kCardPad, (configH - lhB) * 0.5f)
                    .size(kConfigTitleW, lhB)
                    .text("任务配置")
                    .fontSize(fsB)
                    .lineHeight(lhB)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("agent.dialog.detail.config.header.hint")
                    .position(kCardPad + kConfigTitleW + kDialogGap,
                              (configH - lhS) * 0.5f)
                    .size(std::max(0.0f, innerW - kCardPad - kConfigTitleW -
                                        kConfigToggleW - 2.0f * kDialogGap),
                          lhS)
                    .text(configHint)
                    .fontSize(fsS)
                    .lineHeight(lhS)
                    .wrap(false)
                    .maxWidth(std::max(
                        0.0f, innerW - kCardPad - kConfigTitleW -
                                  kConfigToggleW - 2.0f * kDialogGap))
                    .color(t.colors.textSecondary)
                    .build();
                ui.text("agent.dialog.detail.config.header.toggle")
                    .position(innerW - kCardPad - kConfigToggleW,
                              (configH - lhS) * 0.5f)
                    .size(kConfigToggleW, lhS)
                    .text(configOpen_ ? "收起" : "展开")
                    .fontSize(fsS)
                    .lineHeight(lhS)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.primary)
                    .build();
                // 整行命中面最后画（盖在文本上收点击）；P1 只切状态位，浮层 P3 渲染
                ui.rect("agent.dialog.detail.config.header.hit")
                    .size(innerW, configH)
                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([this] { configOpen_ = !configOpen_; })
                    .build();
            })
            .build();

        // —— 时间线（宽屏左栏 / 窄屏 tab 0）：按天分组 + 行容器化（spec §7.2）——
        // 行 = <i> 容器（stack），子节点 .bg（命中面+选中底）/ .dot / .time / .status /
        // .duration；失败行内 .err（48 字节摘要）[.err.hint 仅 LLM 配置类]。
        // onClick 落 .bg——文件头「chip 手搓两层」契约同款
        auto composeRunlist = [&](float rx, float ry, float rw, float rh) {
            const float titleH = std::max(22.0f, lhB);
            // 行高 fs13 派生（§9.3 的 52/68/84 是 1.0 档实测值；err/hint 各 +16）
            const float rowMainH = std::max(52.0f, lhS + 34.0f);
            const float rowErrH = std::max(16.0f, lhS - 2.0f);
            constexpr float kRowInnerX = 12.0f;
            constexpr float kDotSize = 8.0f;
            const float timeX = kRowInnerX + kDotSize + 8.0f;
            constexpr float kTimeW = 100.0f;   // MM-DD HH:MM:SS @13 ≈ 96
            const float statusX = timeX + kTimeW + 4.0f;
            constexpr float kStatusW = 48.0f;  // 3 CJK @13 = 39
            const float durationX = statusX + kStatusW + 4.0f;
            const bool capped =
                hasRuns && runs->size() >= static_cast<size_t>(kDetailRunListLimit);
            ui.stack("agent.dialog.detail.runlist")
                .position(rx, ry)
                .size(rw, rh)
                .content([&] {
                    ui.text("agent.dialog.detail.runlist.title")
                        .position(0.0f, 0.0f)
                        .size(std::max(0.0f, rw - 90.0f), titleH)
                        .text("执行记录")
                        .fontSize(fsB)
                        .lineHeight(lhB)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    ui.text("agent.dialog.detail.runlist.count")
                        .position(0.0f, 0.0f)
                        .size(rw, titleH)
                        .text(hasRuns ? "最近 " + std::to_string(runs->size()) +
                                            " 次"
                                      : std::string())
                        .fontSize(fsS)
                        .lineHeight(lhB)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .color(t.colors.textTertiary)
                        .build();
                    if (!hasRuns) {
                        ui.text("agent.dialog.detail.runlist.empty")
                            .position(0.0f, titleH + 8.0f)
                            .size(rw, std::max(lhS, rh - titleH - 8.0f))
                            .text("从未执行")
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    const float noteH = capped ? lhS + 4.0f : 0.0f;
                    components::scrollView(ui,
                                           "agent.dialog.detail.runlist.scroll")
                        .position(0.0f, titleH + 8.0f)
                        .size(rw, std::max(0.0f, rh - titleH - 8.0f - noteH))
                        .gap(4.0f)
                        .step(18.0f)
                        .theme(themeTokens)
                        // key 含 size/选中/最新状态（spec §8.6：运行中耗时文本变化
                        // 不改行高，不需每帧换 key）
                        .contentKey("agent.dialog.detail.runlist.scroll.v" +
                                    std::to_string(msgScrollVersion_) + ".n" +
                                    std::to_string(runs->size()) + ".s" +
                                    std::to_string(
                                        selectedRunId_.value_or(0)) +
                                    ".w" +
                                    std::to_string(static_cast<int>(rw)))
                        .content([&](eui::Ui& inner, float cw, float) {
                            std::string lastDayLabel;
                            int dayIdx = -1;
                            for (size_t i = 0; i < runs->size(); ++i) {
                                const AgentRunRow& run = (*runs)[i];
                                // 日期分组头：今天 / 昨天 / YYYY-MM-DD（跨组插一行）
                                const std::string dayText =
                                    localDayLabel(run.startedAt, nowSec);
                                if (dayText != lastDayLabel) {
                                    lastDayLabel = dayText;
                                    ++dayIdx;
                                    inner.text("agent.dialog.detail.runlist.day." +
                                               std::to_string(dayIdx))
                                        .size(cw, lhS)
                                        .text(dayText)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .fontWeight(600)
                                        .color(t.colors.textTertiary)
                                        .build();
                                }
                                const std::string runRowId =
                                    "agent.dialog.detail.runlist." +
                                    std::to_string(i);
                                const bool highlighted =
                                    selectedRunId_ && *selectedRunId_ == run.id;
                                const bool hasErr =
                                    run.error && !run.error->empty();
                                const bool hasHint = hasErr &&
                                                     needsClaudePageHint(*run.error);
                                const float rowH =
                                    rowMainH +
                                    (hasErr ? rowErrH : 0.0f) +
                                    (hasHint ? rowErrH : 0.0f);
                                const std::string statusText =
                                    runStatusText(run.status);
                                // 耗时：结束 = ended-started（负差钳 0）；运行中 =
                                // now-started（spec §10）
                                std::string durationText = "—";
                                if (run.status == "running") {
                                    int64_t secs = nowSec - run.startedAt;
                                    if (secs < 0) secs = 0;
                                    durationText = formatRunDurationText(secs);
                                } else if (run.endedAt) {
                                    int64_t secs = *run.endedAt - run.startedAt;
                                    if (secs < 0) secs = 0;
                                    durationText = formatRunDurationText(secs);
                                }
                                inner.stack(runRowId)
                                    .size(cw, rowH)
                                    .content([&] {
                                        // .bg = 命中面 + 选中底（未选中透明，hover 微亮）
                                        inner.rect(runRowId + ".bg")
                                            .size(cw, rowH)
                                            .radius(t.sizes.radiusSmall)
                                            .states(highlighted
                                                        ? t.colors.primarySoftBg
                                                        : eui::Color{0.0f, 0.0f,
                                                                     0.0f, 0.0f},
                                                    t.colors.hover,
                                                    t.colors.hover)
                                            .onClick([this, runId = run.id] {
                                                selectedRunId_ = runId;
                                                // seq 只在 run 内唯一：切 run 清
                                                // 消息展开态并收回消息区（§6.2）；
                                                // 窄屏点行自动切「运行详情」tab、
                                                // 收起配置头
                                                expandedMsgSeqs_.clear();
                                                autoExpandedSeq_.reset();
                                                messagesOpen_ = false;
                                                detailTab_ = 1;
                                                configOpen_ = false;
                                                ++msgScrollVersion_;
                                                tasksDirty_ = true;
                                            })
                                            .build();
                                        if (highlighted) {
                                            // 选中行左缘 2px primary（spec §9.1）
                                            inner.rect(runRowId + ".edge")
                                                .size(2.0f, rowH)
                                                .color(t.colors.primary)
                                                .build();
                                        }
                                        inner.rect(runRowId + ".dot")
                                            .position(kRowInnerX,
                                                      (rowMainH - kDotSize) * 0.5f)
                                            .size(kDotSize, kDotSize)
                                            .radius(t.sizes.radiusPill)
                                            .color(statusDotColor(run.status, t))
                                            .build();
                                        inner.text(runRowId + ".time")
                                            .position(timeX,
                                                      (rowMainH - lhS) * 0.5f)
                                            .size(kTimeW, lhS)
                                            .text(formatLocalShortDateTime(
                                                run.startedAt))
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(false)
                                            .maxWidth(kTimeW)
                                            .color(t.colors.textSecondary)
                                            .build();
                                        inner.text(runRowId + ".status")
                                            .position(statusX,
                                                      (rowMainH - lhS) * 0.5f)
                                            .size(kStatusW, lhS)
                                            .text(statusText)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(false)
                                            .maxWidth(kStatusW)
                                            .color(statusDotColor(run.status, t))
                                            .build();
                                        inner.text(runRowId + ".duration")
                                            .position(durationX,
                                                      (rowMainH - lhS) * 0.5f)
                                            .size(std::max(
                                                0.0f, cw - durationX - kRowInnerX),
                                                  lhS)
                                            .text(durationText)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(false)
                                            .maxWidth(std::max(
                                                0.0f, cw - durationX - kRowInnerX))
                                            .color(t.colors.textTertiary)
                                            .build();
                                        if (hasErr) {
                                            inner.text(runRowId + ".err")
                                                .position(timeX, rowMainH)
                                                .size(std::max(
                                                          0.0f,
                                                          cw - timeX -
                                                              kRowInnerX),
                                                      rowErrH)
                                                .text("错误：" +
                                                      utf8Truncate(*run.error, 48))
                                                .fontSize(fsS)
                                                .lineHeight(rowErrH)
                                                .wrap(false)
                                                .maxWidth(std::max(
                                                    0.0f, cw - timeX - kRowInnerX))
                                                .color(t.colors.error)
                                                .build();
                                            // 「去 Claude 页」引导：仅「LLM 配置不可
                                            // 用」类错误渲染（needsClaudePageHint
                                            // 前缀钉死）
                                            if (hasHint) {
                                                inner.text(runRowId + ".err.hint")
                                                    .position(timeX,
                                                              rowMainH + rowErrH)
                                                    .size(std::max(
                                                              0.0f,
                                                              cw - timeX -
                                                                  kRowInnerX),
                                                          rowErrH)
                                                    .text("去 Claude 页 →")
                                                    .fontSize(fsS)
                                                    .lineHeight(rowErrH)
                                                    .wrap(false)
                                                    .color(t.colors.primary)
                                                    .onClick(
                                                        [] {
                                                            navigateToClaudePage();
                                                        })
                                                    .build();
                                            }
                                        }
                                    })
                                    .build();
                            }
                        })
                        .build();
                    // 20 条上限提示（spec §7.2/§10：不提供分页）
                    if (capped) {
                        ui.text("agent.dialog.detail.runlist.note")
                            .position(0.0f, rh - lhS)
                            .size(rw, lhS)
                            .text("仅显示最近 " +
                                  std::to_string(kDetailRunListLimit) + " 次")
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .color(t.colors.textTertiary)
                            .build();
                    }
                })
                .build();
        };

        // —— 右栏 / 窄屏 tab 1：run 详情块（状态/元信息/计划对照/错误全文）+ 消息区 ——
        // 宽屏正常高：详情块固定 + 消息标题固定 + .msgscroll 独立滚；矮宽屏 / 窄屏 /
        // 余高不足 96：.detail.scroll 整栏滚（不建 .msgscroll，避免嵌套滚动）
        constexpr float kMsgsTitleH = 32.0f;
        constexpr float kCopyFinalChipW = 76.0f;
        // 选中 run（无 → .detail.empty；默认选中 = 最近一次，refreshData 已回落）
        const AgentRunRow* selRun = nullptr;
        if (hasRuns && selectedRunId_) {
            for (const auto& r : *runs) {
                if (r.id == *selectedRunId_) {
                    selRun = &r;
                    break;
                }
            }
        }
        auto composeDetailColumn = [&](float dx, float dy, float dw, float dh) {
            const std::string finalText = finalAssistantText(detailMessages_);
            const bool hasCopyFinal = !finalText.empty();
            // 详情块高度预算（spec §6.3 detailTopH）：状态行 + 元信息 6 行 + 计划对照
            // + [错误 label+滚动区]；错误块出现时预算变大，消息余高不足自动降级整栏滚
            const float statusH = std::max(24.0f, lhB + 2.0f);
            const float metaPitch = lhS + 4.0f;
            const float metaH = 6.0f * metaPitch - 4.0f;
            const float scheduleH = lhS;
            float errScrollH = 0.0f;
            float errBlockH = 0.0f;
            const bool hasErr = selRun != nullptr && selRun->error &&
                                !selRun->error->empty();
            if (hasErr) {
                errScrollH = std::clamp(
                    estimateTextBlockHeight(*selRun->error, dw - 16.0f, fsS, lhS),
                    lhS, 96.0f);
                errBlockH = lhS + 8.0f + errScrollH;   // label + 列 gap + scroll
            }
            // 列内容高：status + gap8 + meta + gap8 + schedule [+ gap8 + 错误块]
            //（与 .blocks 列的 gap 8 逐项对应，供 fixed 态 msgs head 定位与降级判定）
            const float detailTopH =
                selRun != nullptr
                    ? statusH + 8.0f + metaH + 8.0f + scheduleH +
                          (hasErr ? 8.0f + errBlockH : 0.0f)
                    : 0.0f;
            const bool fixedMsgArea =
                selRun != nullptr && wide && h >= 600.0f &&
                (dh - detailTopH - kMsgsTitleH - 12.0f) >= 96.0f;

            // 标题行 + 复制 chip + 区级开合 chip（flow 排）：固定态包在绝对定位
            // stack 里，滚动态直接进 .detail.scroll 内容列。copyfinal 与 toggle 都
            // 常驻标题行（折叠态 copyfinal 仍可见，spec §12.1）
            constexpr float kMsgsToggleW = 48.0f;
            auto msgsHeaderRow = [&](eui::Ui& inner, float cw) {
                const float chipsW =
                    (hasCopyFinal ? kCopyFinalChipW + 8.0f : 0.0f) +
                    kMsgsToggleW;
                inner.row("agent.dialog.detail.msgs.head")
                    .size(cw, kMsgsTitleH)
                    .content([&] {
                        inner.text("agent.dialog.detail.msgs.title")
                            .size(std::max(0.0f, cw - chipsW), kMsgsTitleH)
                            .text("消息（" +
                                  std::to_string(detailMessages_.size()) + "）")
                            .fontSize(fsB)
                            .lineHeight(lhB)
                            .fontWeight(600)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textPrimary)
                            .build();
                        // 「复制回复」：一键复制 run 最终回复全文（最后一条 assistant
                        // 的全部 text 块，见 finalAssistantText）。空串不渲染
                        if (hasCopyFinal) {
                            inner.stack(
                                "agent.dialog.detail.msgs.copyfinal.wrap")
                                .size(kCopyFinalChipW, kMsgsTitleH)
                                .content([&] {
                                    actionChip(
                                        inner,
                                        "agent.dialog.detail.msgs.copyfinal",
                                        "复制回复", kCopyFinalChipW,
                                        /*primary=*/true, /*enabled=*/true,
                                        themeTokens, [this, text = finalText] {
                                            // setClipboardText 返回 void 无法校验
                                            // → LOG 留痕；反馈走页级 toast（D13）
                                            core::window::setClipboardText(text);
                                            LOG_INFO("Agent: 已复制最终回复 " +
                                                     std::to_string(text.size()) +
                                                     " 字节到剪贴板");
                                            toast_.show("已复制",
                                                        "最终回复已复制到剪贴板");
                                        }, fsS);
                                })
                                .build();
                        }
                        // 区级开合（D6 默认折叠）：展开时「最终 assistant 默认展开」
                        // 规则生效（applyDefaultMessageExpansion 降级为展开态内规则）
                        inner.stack("agent.dialog.detail.msgs.toggle.wrap")
                            .size(kMsgsToggleW, kMsgsTitleH)
                            .content([&] {
                                actionChip(
                                    inner, "agent.dialog.detail.msgs.toggle",
                                    messagesOpen_ ? "收起" : "展开",
                                    kMsgsToggleW,
                                    /*primary=*/false, /*enabled=*/true,
                                    themeTokens,
                                    [this] {
                                        messagesOpen_ = !messagesOpen_;
                                        if (messagesOpen_) {
                                            applyDefaultMessageExpansion();
                                        }
                                        ++msgScrollVersion_;   // 内容高变了 → scrollView 重测
                                    }, fsS);
                            })
                            .build();
                    })
                    .build();
            };

            // run 详情块：宽屏固定区与整栏滚动内容共用一套绘制（返回消耗高度）。
            // 布局必须流式：scrollView 内容里 .position 会被流式布局接管（实测钉死），
            // 区块统一走 column/row 流式排——外层列 gap 8（区块间 §9.3），meta 行内 4。
            // ID 契约（spec §7.2）：.detail.status[.dot/.text] / .detail.id /
            // .detail.meta.<field>[.label/.value] / .detail.schedule[.plan/.actual/
            // .verdict] / .detail.error[.label/.scroll/.text]
            auto drawDetailBlocks = [&](eui::Ui& inner, float cw, float y0) {
                if (selRun == nullptr) return y0;
                const std::string stText = runStatusText(selRun->status);
                const eui::Color stColor = statusDotColor(selRun->status, t);
                // 元信息 6 行单列（spec D：保证完整时间戳不截断）
                std::string durV = "—";
                if (selRun->status == "running") {
                    int64_t secs = nowSec - selRun->startedAt;
                    if (secs < 0) secs = 0;
                    durV = "进行中 · " + formatRunDurationText(secs);
                } else if (selRun->endedAt) {
                    int64_t secs = *selRun->endedAt - selRun->startedAt;
                    if (secs < 0) secs = 0;
                    durV = formatRunDurationText(secs);
                }
                struct MetaRow {
                    const char* id;
                    const char* label;
                    std::string value;
                };
                const MetaRow metaRows[6] = {
                    {"started", "开始", formatLocalDateTime(selRun->startedAt)},
                    {"ended", "结束",
                     selRun->endedAt ? formatLocalDateTime(*selRun->endedAt)
                                     : std::string("—")},
                    {"duration", "耗时", durV},
                    {"iterations", "迭代", std::to_string(selRun->iterations)},
                    {"input", "输入",
                     selRun->inputTokens ? formatTokenCount(*selRun->inputTokens)
                                         : std::string("—")},
                    {"output", "输出",
                     selRun->outputTokens
                         ? formatTokenCount(*selRun->outputTokens)
                         : std::string("—")},
                };
                // 计划对照（describeRunPlan 缓存产物；无计划不造假）
                const RunPlanInfo plan =
                    detailPlan_.has_value() ? *detailPlan_ : RunPlanInfo{};
                inner.column("agent.dialog.detail.detail.blocks")
                    .size(cw, detailTopH)
                    .gap(8.0f)
                    .content([&] {
                        // 状态 pill + #id（plain stack 内绝对定位合法）
                        inner.stack("agent.dialog.detail.detail.status")
                            .size(cw, statusH)
                            .content([&] {
                                // pill 底/边 = 状态色 withAlpha 0.14/0.40（spec §9.1）
                                inner.rect(
                                          "agent.dialog.detail.detail.status.pill")
                                    .size(88.0f, statusH)
                                    .color(components::theme::withOpacity(
                                        stColor, 0.14f))
                                    .radius(t.sizes.radiusPill)
                                    .border(1.0f,
                                            components::theme::withOpacity(
                                                stColor, 0.40f))
                                    .build();
                                inner.rect("agent.dialog.detail.detail.status.dot")
                                    .position(8.0f, (statusH - 8.0f) * 0.5f)
                                    .size(8.0f, 8.0f)
                                    .radius(t.sizes.radiusPill)
                                    .color(stColor)
                                    .build();
                                inner.text(
                                          "agent.dialog.detail.detail.status.text")
                                    .position(24.0f, 0.0f)
                                    .size(60.0f, statusH)
                                    .text(stText)
                                    .fontSize(fsB)
                                    .lineHeight(statusH)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(stColor)
                                    .build();
                                inner.text("agent.dialog.detail.detail.id")
                                    .position(std::max(0.0f, cw - 72.0f), 0.0f)
                                    .size(72.0f, statusH)
                                    .text("#" + std::to_string(selRun->id))
                                    .fontSize(fsS)
                                    .lineHeight(statusH)
                                    .horizontalAlign(eui::HorizontalAlign::Right)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();
                            })
                            .build();
                        // 元信息 6 行（行盒 flow，行内 label 64 + value）
                        inner.column("agent.dialog.detail.detail.meta")
                            .size(cw, metaH)
                            .gap(4.0f)
                            .content([&] {
                                for (const auto& row : metaRows) {
                                    const std::string base =
                                        std::string(
                                            "agent.dialog.detail.detail.meta.") +
                                        row.id;
                                    inner.row(base)
                                        .size(cw, lhS)
                                        .content([&] {
                                            inner.text(base + ".label")
                                                .size(64.0f, lhS)
                                                .text(row.label)
                                                .fontSize(fsS)
                                                .lineHeight(lhS)
                                                .color(t.colors.textTertiary)
                                                .build();
                                            inner.text(base + ".value")
                                                .size(std::max(0.0f, cw - 72.0f),
                                                      lhS)
                                                .text(row.value)
                                                .fontSize(fsS)
                                                .lineHeight(lhS)
                                                .wrap(false)
                                                .maxWidth(std::max(
                                                    0.0f, cw - 72.0f))
                                                .color(t.colors.textSecondary)
                                                .build();
                                        })
                                        .build();
                                }
                            })
                            .build();
                        // 计划对照一行三列（flow row）
                        inner.row("agent.dialog.detail.detail.schedule")
                            .size(cw, lhS)
                            .content([&] {
                                inner.text(
                                          "agent.dialog.detail.detail.schedule.plan")
                                    .size(cw * 0.44f, lhS)
                                    .text(std::string("计划 ") +
                                          (plan.hasPlan ? plan.plan : "—"))
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .wrap(false)
                                    .color(t.colors.textSecondary)
                                    .build();
                                inner.text(
                                          "agent.dialog.detail.detail.schedule.actual")
                                    .size(cw * 0.34f, lhS)
                                    .text("实际 " + plan.actual)
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .wrap(false)
                                    .color(t.colors.textSecondary)
                                    .build();
                                inner.text(
                                          "agent.dialog.detail.detail.schedule.verdict")
                                    .size(std::max(0.0f, cw * 0.22f), lhS)
                                    .text(plan.verdict)
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .horizontalAlign(eui::HorizontalAlign::Right)
                                    // verdict 色：Normal=按时→success；其余各归 token
                                    .color(plan.tone == TextTone::Normal
                                               ? t.colors.success
                                           : plan.tone == TextTone::Warning
                                               ? t.colors.warning
                                           : plan.tone == TextTone::Error
                                               ? t.colors.error
                                               : t.colors.textTertiary)
                                    .build();
                            })
                            .build();
                        if (hasErr) {
                            inner.text("agent.dialog.detail.detail.error.label")
                                .size(cw, lhS)
                                .text("错误详情")
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .fontWeight(600)
                                .color(t.colors.error)
                                .build();
                            components::scrollView(
                                inner, "agent.dialog.detail.detail.error.scroll")
                                .size(cw, errScrollH)
                                .step(18.0f)
                                .theme(themeTokens)
                                .contentKey(
                                    "agent.dialog.detail.detail.error.scroll.v" +
                                    std::to_string(
                                        hasErr ? selRun->error->size() : 0) +
                                    ".w" + std::to_string(static_cast<int>(cw)))
                                .content([&](eui::Ui& errInner, float ew, float) {
                                    errInner.text(
                                              "agent.dialog.detail.detail.error.text")
                                        .size(ew,
                                              estimateTextBlockHeight(
                                                  *selRun->error, ew, fsS, lhS))
                                        .text(*selRun->error)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .wrap(true)
                                        .maxWidth(ew)
                                        .color(t.colors.error)
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
                return y0 + detailTopH;
            };

            ui.stack("agent.dialog.detail.detail")
                .position(dx, dy)
                .size(dw, dh)
                .content([&] {
                    if (selRun == nullptr) {
                        // 无 run（或选中失效）：右栏空态（spec §10）
                        ui.text("agent.dialog.detail.detail.empty")
                            .size(dw, dh)
                            .text("选择一次执行查看详情")
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    if (fixedMsgArea) {
                        const float blocksH = drawDetailBlocks(ui, dw, 0.0f);
                        ui.stack("agent.dialog.detail.msgs.head.wrap")
                            .position(0.0f, blocksH)
                            .size(dw, kMsgsTitleH)
                            .content([&] { msgsHeaderRow(ui, dw); })
                            .build();
                        const float yMsg = blocksH + kMsgsTitleH;
                        if (!detailMessagesError_.empty()) {
                            ui.text("agent.dialog.detail.msgs.error")
                                .position(0.0f, yMsg)
                                .size(dw, std::max(0.0f, dh - yMsg))
                                .text("消息读取失败：" + detailMessagesError_)
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .wrap(true)
                                .maxWidth(dw)
                                .color(t.colors.error)
                                .build();
                            return;
                        }
                        if (detailMessages_.empty()) {
                            ui.text("agent.dialog.detail.empty")
                                .position(0.0f, yMsg)
                                .size(dw, std::max(0.0f, dh - yMsg))
                                .text("（选中的 run 还没有消息）")
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                // 盒高 ≠ 行高，不给 verticalAlign 会顶在上缘
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                            return;
                        }
                        if (!messagesOpen_) {
                            // 折叠态（D6）：只给占位提示，不建 .msgscroll（spec
                            // §12.3 反向断言钉「默认无 msgscroll」）
                            ui.text("agent.dialog.detail.msgs.hint")
                                .position(0.0f, yMsg)
                                .size(dw, std::max(0.0f, dh - yMsg))
                                .text("展开查看完整链路")
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                            return;
                        }
                        components::scrollView(
                            ui, "agent.dialog.detail.msgscroll")
                            .position(0.0f, yMsg)
                            .size(dw, std::max(0.0f, dh - yMsg))
                            .gap(kMsgRowGap)
                            .step(18.0f)
                            .theme(themeTokens)
                            .contentKey("agent.dialog.detail.msgscroll.v" +
                                        std::to_string(msgScrollVersion_) +
                                        ".n" +
                                        std::to_string(
                                            detailMessages_.size()) +
                                        ".o" +
                                        std::to_string(messagesOpen_ ? 1 : 0) +
                                        ".x" +
                                        std::to_string(
                                            expandedMsgSeqs_.size()) +
                                        ".e" +
                                        std::to_string(
                                            detailMessagesError_.size()) +
                                        ".w" +
                                        std::to_string(static_cast<int>(dw)))
                            .content([&](eui::Ui& inner, float cw, float) {
                                for (size_t i = 0;
                                     i < detailMessages_.size(); ++i) {
                                    renderMessageRow(inner, cw,
                                                     "agent.dialog.detail",
                                                     i, detailMessages_[i],
                                                     selRun->startedAt, t,
                                                     themeTokens);
                                }
                            })
                            .build();
                        return;
                    }
                    components::scrollView(ui,
                                           "agent.dialog.detail.detail.scroll")
                        .size(dw, dh)
                        .gap(kMsgRowGap)
                        .step(18.0f)
                        .theme(themeTokens)
                        .contentKey("agent.dialog.detail.detail.scroll.v" +
                                    std::to_string(msgScrollVersion_) + ".n" +
                                    std::to_string(detailMessages_.size()) +
                                    ".o" +
                                    std::to_string(messagesOpen_ ? 1 : 0) +
                                    ".x" +
                                    std::to_string(expandedMsgSeqs_.size()) +
                                    ".e" +
                                    std::to_string(
                                        detailMessagesError_.size()) +
                                    ".d" +
                                    std::to_string(static_cast<int>(detailTopH)) +
                                    ".w" + std::to_string(static_cast<int>(dw)))
                        .content([&](eui::Ui& inner, float cw, float) {
                            drawDetailBlocks(inner, cw, 0.0f);
                            // 详情块 → 消息标题的定位间隔（flow 布局无绝对 y，
                            // 用透明占位撑出标题行高）
                            inner.rect("agent.dialog.detail.detail.scroll.gap")
                                .size(cw, kMsgsTitleH)
                                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                .build();
                            msgsHeaderRow(inner, cw);
                            if (!detailMessagesError_.empty()) {
                                inner.text("agent.dialog.detail.msgs.error")
                                    .size(cw, lhS)
                                    .text("消息读取失败：" + detailMessagesError_)
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .wrap(true)
                                    .maxWidth(cw)
                                    .color(t.colors.error)
                                    .build();
                                return;
                            }
                            if (detailMessages_.empty()) {
                                inner.text("agent.dialog.detail.empty")
                                    .size(cw,
                                          std::clamp(dh - kMsgsTitleH,
                                                     kMsgAreaMinH,
                                                     kMsgAreaMaxH))
                                    .text("（选中的 run 还没有消息）")
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();
                                return;
                            }
                            if (!messagesOpen_) {
                                inner.text("agent.dialog.detail.msgs.hint")
                                    .size(cw, lhS)
                                    .text("展开查看完整链路")
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .color(t.colors.textTertiary)
                                    .build();
                                return;
                            }
                            for (size_t i = 0; i < detailMessages_.size(); ++i) {
                                renderMessageRow(inner, cw, "agent.dialog.detail",
                                                 i, detailMessages_[i],
                                                 selRun->startedAt, t,
                                                 themeTokens);
                            }
                        })
                        .build();
                })
                .build();
        };

        if (wide) {
            composeRunlist(x0, mainY, leftW, mainH);
            composeDetailColumn(rightX, mainY, rightW, mainH);
        } else {
            // 窄屏单列：摘要 → 配置头 → segmented（两 tab）→ 当前 tab 内容。
            // segmented 组件不暴露 position，包一层 wrap stack 落位
            ui.stack("agent.dialog.detail.nav.wrap")
                .position(x0, navY)
                .size(innerW, navH)
                .content([&] {
                    components::segmented(ui, "agent.dialog.detail.nav")
                        .size(innerW, navH)
                        .items({"执行记录", "运行详情"})
                        .selected(detailTab_)
                        .fontSize(fsS)
                        .theme(themeTokens)
                        .onChange([this](int index) { detailTab_ = index; })
                        .build();
                })
                .build();
            if (detailTab_ == 0) {
                composeRunlist(x0, contentY, innerW, contentH);
            } else {
                composeDetailColumn(x0, contentY, innerW, contentH);
            }
        }

        // —— 配置浮层（spec §6.4，P3）：先画 scrim 再画 popover，盖在主区上。
        // 两者是弹窗内容内的相对层（z19/z20），不改变 dialog 自身 z=1000；
        // 浮层不改变主区几何——关闭后主区原位（测试钉「开合前后 runlist y 不变」）
        if (configOpen_) {
            constexpr float kFieldLabelW = 64.0f;   // §9.3 配置标签宽
            constexpr float kFieldGap = 10.0f;      // §9.3 配置字段行距
            const float scrimY = configY + configH;
            const float popoverY = scrimY + 4.0f;
            const float popoverMaxH = std::max(0.0f, h - popoverY - 24.0f);
            // 字段值宽 = 浮层内宽 − 左右 pad − 标签 − gap（wrap 字段按它估高）
            const float valueX = kCardPad + kFieldLabelW + 6.0f;
            const float valueW =
                std::max(0.0f, innerW - 2.0f * kCardPad - kFieldLabelW - 6.0f);
            const AgentTaskRow* cfgTask =
                detailTaskRow_ ? &*detailTaskRow_ : nullptr;
            // 字段表（§7.2）：读取失败/缺数据显「—」，不造假；problem 无异常不渲染
            struct CfgField {
                const char* id;
                const char* label;
                std::string value;
                bool wrap;
                bool error;
            };
            std::vector<CfgField> fields;
            auto taskValue = [&](const std::string& v) {
                return cfgTask != nullptr ? v : std::string("—");
            };
            fields.push_back({"goal", "目标",
                              taskValue(cfgTask ? cfgTask->goal : std::string()),
                              true, false});
            fields.push_back({"trigger", "触发",
                              taskValue(cfgTask
                                            ? triggerLabelOf(cfgTask->triggerType)
                                            : std::string()),
                              false, false});
            std::string scheduleV = "—";
            if (cfgTask != nullptr) {
                if (cfgTask->triggerType == "schedule") {
                    const std::string cron = cronTextOf(*cfgTask);
                    scheduleV = cron.empty() ? "（未设置）" : cron;
                } else if (cfgTask->triggerType == "once") {
                    const std::string onceText = onceTextOf(*cfgTask);
                    scheduleV = onceText.empty() ? "（未设置）" : onceText;
                }
            }
            fields.push_back({"schedule", "定时", scheduleV, false, false});
            fields.push_back({"model", "模型槽位",
                              taskValue(cfgTask ? cfgTask->modelSlot
                                                : std::string()),
                              false, false});
            fields.push_back({"iterations", "迭代上限",
                              taskValue(cfgTask
                                            ? std::to_string(cfgTask->maxIterations)
                                            : std::string()),
                              false, false});
            fields.push_back({"destructive", "破坏性",
                              taskValue(cfgTask
                                            ? std::string(cfgTask->allowDestructive
                                                              ? "允许"
                                                              : "禁止")
                                            : std::string()),
                              false, false});
            std::string limitV = "不限次数";
            if (cfgTask != nullptr) {
                // 限次语义口径与列表徽章同源：失败也算跑过，承诺写「跑完自动停用」
                const auto limitIt = taskRunLimits_.find(cfgTask->id);
                if (limitIt != taskRunLimits_.end()) {
                    limitV = runLimitText(limitIt->second) + "（跑完自动停用）";
                }
            } else {
                limitV = "—";
            }
            fields.push_back({"limit", "次数上限", limitV, false, false});
            if (cfgTask != nullptr) {
                const auto problemIt = taskTriggerProblems_.find(cfgTask->id);
                if (problemIt != taskTriggerProblems_.end()) {
                    fields.push_back({"problem", "配置异常", problemIt->second,
                                      true, true});
                }
            }
            fields.push_back({"context", "执行记忆",
                              taskValue(cfgTask && !cfgTask->contextNote.empty()
                                            ? cfgTask->contextNote
                                            : std::string()),
                              true, false});
            // 内容高估算（§6.4 min(内容高, popoverMaxH)；真实超高由 scrollView 内滚）
            float contentH = 0.0f;
            for (const auto& f : fields) {
                contentH +=
                    (f.wrap ? estimateTextBlockHeight(f.value, valueW, fsS, lhS)
                            : lhS) +
                    kFieldGap;
            }
            contentH -= kFieldGap;
            const float kPopoverPad = 12.0f;   // §9.3 卡片内边距
            const float scrollH =
                std::min(std::max(0.0f, contentH),
                         std::max(0.0f, popoverMaxH - 2.0f * kPopoverPad));
            const float popoverH = scrollH + 2.0f * kPopoverPad;
            const std::string cfgBase = "agent.dialog.detail.config";
            ui.rect(cfgBase + ".scrim")
                .position(x0, scrimY)
                .size(innerW, std::max(0.0f, h - scrimY))
                .color(components::theme::withOpacity(t.colors.textPrimary,
                                                      0.08f))
                .zIndex(19)
                .onClick([this] { configOpen_ = false; })
                .build();
            ui.stack(cfgBase + ".popover")
                .position(x0, popoverY)
                .size(innerW, popoverH)
                .zIndex(20)
                .content([&] {
                    ui.rect(cfgBase + ".popover.bg")
                        .size(innerW, popoverH)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .build();
                    components::scrollView(ui, cfgBase + ".popover.scroll")
                        .position(kPopoverPad, kPopoverPad)
                        .size(std::max(0.0f, innerW - 2.0f * kPopoverPad),
                              scrollH)
                        .step(18.0f)
                        .theme(themeTokens)
                        .contentKey(
                            cfgBase + ".popover.scroll.v" +
                            std::to_string(cfgTask ? cfgTask->goal.size() : 0) +
                            "." +
                            std::to_string(cfgTask ? cfgTask->contextNote.size()
                                                   : 0) +
                            "." +
                            (cfgTask ? cfgTask->triggerType : std::string()) +
                            "." +
                            (cfgTask ? cfgTask->triggerConfig : std::string()) +
                            ".p" +
                            std::to_string([&] {
                                if (cfgTask == nullptr) return size_t{0};
                                const auto it =
                                    taskTriggerProblems_.find(cfgTask->id);
                                return it != taskTriggerProblems_.end()
                                           ? it->second.size()
                                           : size_t{0};
                            }()) +
                            ".f" + std::to_string(fsS))
                        .content([&](eui::Ui& f, float fw, float) {
                            for (const auto& fld : fields) {
                                const std::string base =
                                    cfgBase + ".body." + fld.id;
                                const float valueH =
                                    fld.wrap
                                        ? estimateTextBlockHeight(
                                              fld.value, valueW, fsS, lhS)
                                        : lhS;
                                const float valW =
                                    std::max(0.0f, fw - (valueX - kPopoverPad));
                                f.row(base)
                                    .size(fw, valueH)
                                    .content([&] {
                                        f.text(base + ".label")
                                            .size(kFieldLabelW, lhS)
                                            .text(fld.label)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .color(t.colors.textTertiary)
                                            .build();
                                        f.text(base + ".value")
                                            .size(valW, valueH)
                                            .text(fld.value)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(fld.wrap)
                                            .maxWidth(valW)
                                            .color(fld.error
                                                       ? t.colors.error
                                                       : t.colors.textPrimary)
                                            .build();
                                    })
                                    .build();
                            }
                        })
                        .build();
                })
                .build();
        }
    });
}

// Esc 关最上层弹窗（详情优先）。键走得到本通道 = 焦点元素没消费（输入框聚焦时已被 onCancel 收走）
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
