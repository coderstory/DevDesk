// 智能体页模块：生命周期 + UI（工具区 / 任务列表 / 任务弹窗 / 详情弹窗）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）；可点元素契约：id 元素
// 须带 text 与 onClick（button 的 onClick 落在 .bg 上）→ chip 手搓两层；弹窗字段标签
// 一律 .caption：dropdown 内部占用 <id>.label，同 id 会串位。

#include "agent_module.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <system_error>
#include <utility>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <core/window/window_backend.h>
#include <core/platform/platform.h>
#include <eui/dsl.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>

#include "model_slots.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/time.h"
#include "module_agent/agent_loop.h"
#include "module_agent/app_stats_tool.h"
#include "module_agent/cron.h"
#include "module_agent/message_blocks.h"
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
    // 前缀必须与 llm_client.cpp:56 / :63 原文逐字一致（见头文件注释）
    if (errorText.rfind("代理未启", 0) == 0) return true;
    if (errorText.rfind("无法解析模型槽位", 0) == 0) return true;
    return false;
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

std::string formatRunDurationText(int64_t seconds) {
    if (seconds < 0) seconds = 0;   // 时钟回拨防御：不显示负时长（与 runRowText 同口径）
    const int64_t days = seconds / 86400;
    const int64_t hours = (seconds % 86400) / 3600;
    const int64_t mins = (seconds % 3600) / 60;
    const int64_t secs = seconds % 60;
    if (days > 0) return std::to_string(days) + "天" + std::to_string(hours) + "时";
    if (hours > 0) {
        return std::to_string(hours) + "时" +
               (mins > 0 ? std::to_string(mins) + "分" : "");
    }
    if (mins > 0) {
        return std::to_string(mins) + "分" +
               (secs > 0 ? std::to_string(secs) + "秒" : "");
    }
    return std::to_string(secs) + "秒";
}

std::string formatRunResultTimeText(const std::tm& local) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
    return buf;
}

std::string formatTaskRunSummaryText(const AgentTaskRunSummary& summary,
                                     const std::string& lastTimeText) {
    const int64_t total = summary.totalRuns;
    const int64_t pct = total > 0 ? (summary.okRuns * 100) / total : 0;
    return "共 " + std::to_string(total) + " 次 · 成功率 " + std::to_string(pct) +
           "%（" + std::to_string(summary.okRuns) + "/" + std::to_string(total) +
           "）· 总耗时 " + formatRunDurationText(summary.totalSeconds) +
           " · 最近结果 " + lastTimeText;
}

namespace {

// run 状态 → 中文展示；未知态显示「—」
std::string runStatusText(const std::string& status) {
    if (status == "completed") return "成功";
    if (status == "failed") return "失败";
    if (status == "limited") return "受限";
    if (status == "cancelled") return "已取消";
    if (status == "running") return "运行中";
    return "—";
}

std::string runRowText(const AgentRunRow& run) {
    std::string s = runStatusText(run.status);
    if (run.endedAt) {
        int64_t secs = *run.endedAt - run.startedAt;
        if (secs < 0) secs = 0;   // 时钟回拨防御：不显示负耗时
        s += " · 耗时 " + std::to_string(secs) + "s";
    }
    s += " · 迭代 " + std::to_string(run.iterations);
    if (run.inputTokens || run.outputTokens) {
        s += " · token " + std::to_string(run.inputTokens.value_or(0) +
                                          run.outputTokens.value_or(0));
    } else {
        s += " · token —";
    }
    return s;
}

// 行内动作 chip：契约要求 id 落在可见文本上，故手搓两层——<id>.bg 为命中面、文本穿透
void actionChip(::eui::Ui& ui, const std::string& id, const std::string& label,
                float width, bool primary, bool enabled,
                const components::theme::ThemeColorTokens& tokens,
                std::function<void()> onClick) {
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
                .fontSize(12.0f)
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

constexpr const char* kTriggerKeys[] = {"manual", "schedule", "resident", "event"};
constexpr const char* kTriggerLabels[] = {"手动", "定时", "常驻", "事件"};
constexpr int kTriggerCount = 4;
constexpr int kTriggerScheduleIdx = 1;
constexpr int kTriggerEventIdx = 3;

// 事件触发可选事件表：名字 = core::event 常量（唯一真源），label 是表单文案。
// 顺序即 editEvents_ 下标——两表必须同长，加事件只改这里 + editEvents_ 容量
constexpr const char* kEventNames[] = {
    ::core::event::kTodoCreated, ::core::event::kTodoCompleted,
    ::core::event::kMdSaved};
constexpr const char* kEventLabels[] = {"新建待办", "完成待办", "保存笔记"};
constexpr int kEventCount = 3;

constexpr float kPageHeaderH = 40.0f;
constexpr float kNewTaskBtnW = 140.0f;
constexpr float kTaskRowH = 44.0f;
constexpr float kPageErrorH = 16.0f;
// 页内错误行下的「去 Claude 页」引导行高（仅两类 LLM 预检错误渲染）
constexpr float kPageHintH = 16.0f;

// 任务弹窗几何（cron 与触发同行、仅「定时」渲染、弹窗高度不变）
constexpr float kDialogRowH = 40.0f;
constexpr float kDialogLabelW = 72.0f;
constexpr float kDialogGap = 12.0f;
constexpr float kTaskDialogH = 500.0f;
constexpr float kGoalInputH = 96.0f;
constexpr float kContextNoteH = 72.0f;

constexpr float kDetailDialogH = 660.0f;

// 契约：详情弹窗展示最近 5 次 run
constexpr int kRunListLimit = 5;

// run 日志消息区外高钳制在 [kMsgAreaMinH, kMsgAreaMaxH]，超出由 scrollView 内滚
constexpr float kMsgAreaMinH = 96.0f;
constexpr float kMsgAreaMaxH = 520.0f;

constexpr float kToolsHeaderH = 28.0f;
constexpr float kToolCardH = 44.0f;
constexpr float kToolsListMaxH = 240.0f;

int triggerIndexOf(const std::string& triggerType) {
    for (int i = 0; i < kTriggerCount; ++i) {
        if (triggerType == kTriggerKeys[i]) return i;
    }
    return 0;   // 未知触发类型 → 显示「手动」，保存时按当前下拉值归一
}

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

std::string trimCopy(const std::string& s) {
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t b = 0, e = s.size();
    while (b < e && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
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
    // Scheduler 收口：先退订事件（断 emit→onEvent 入队源），再 stopAll 停派发源并收在跑 run
    eventSubs_.clear();
    if (scheduler_) scheduler_->stopAll();
    ::core::ToolRegistry::instance().unregisterTool("app.stats");
    loop_.reset();
    llm_.reset();
    store_.reset();
    scheduler_.reset();
    statsTool_.reset();
    llmMissingWarned_ = false;
    tasks_.clear();
    latestRuns_.clear();
    taskRuns_.clear();
    runSummaries_.clear();
    runSummariesError_.clear();
    runSummariesSeenVersionValid_ = false;
    detailMessages_.clear();
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

// 收割已结束的 run：join 后摘除，避免「同 task 不并发」判定被已完成 run 误伤。仅主线程调用
void AgentModule::reapFinishedRuns() {
    std::lock_guard<std::mutex> lock(runsMu_);
    for (auto it = runs_.begin(); it != runs_.end();) {
        if ((*it)->done.load()) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = runs_.erase(it);
        } else {
            ++it;
        }
    }
}

// 惰性装配单入口：启动时 claude 关、会话内拨开无须重载本模块。幂等：loop_ 非空即返回；
// scheduler_ 须在此与 loop_ 同建（否则惰性路径下 cron 永不触发）
void AgentModule::ensureLlm() {
    if (loop_ != nullptr) return;       // 已就绪（llm_/loop_/scheduler_ 三者同生共死）
    if (store_ == nullptr) return;
    core::IClaudeProxyService* proxy = deps_.proxy;
    if (proxy == nullptr) {
        auto located =
            ::core::ServiceLocator::requireService<::core::IClaudeProxyService>();
        if (located.isOk()) proxy = located.value();
    }
    if (proxy == nullptr) {
        // 每段「无服务」只提示一次——本函数被页面帧调用，不节流会刷屏
        if (!llmMissingWarned_) {
            llmMissingWarned_ = true;
            LOG_WARN("Agent: 代理服务未装配，LLM 通道不可用"
                     "（请在设置页开启 Claude 模块后重试）");
        }
        return;
    }
    llm_ = std::make_unique<LlmClient>(*proxy, deps_.http);
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
    ensureLlm();   // 首次使用路径：会话内拨开的代理服务在此解析
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
    editGoal_.clear();
    editCron_.clear();
    editSlot_ = kModelSlotKeys[0];
    editIter_ = 10;
    editDestructive_ = false;
    editContextNote_.clear();
    editAutoNote_ = true;
    editEvents_ = {};
    editTriggerIdx_ = 0;
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
    refreshCronPreview();
}

// 关闭任务弹窗（多条路径共用）：不落库、不碰任务列表，只退弹窗态与草稿
void AgentModule::closeTaskDialog() {
    taskDialogOpen_ = false;
    editingTaskId_.reset();
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
}

void AgentModule::setEditCronDraft(const std::string& cron) {
    editCron_ = cron;
    refreshCronPreview();
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
    editGoal_ = r.goal;
    editCron_ = cronTextOf(r);
    editSlot_ = r.modelSlot;
    editIter_ = r.maxIterations;
    editDestructive_ = r.allowDestructive;
    editContextNote_ = r.contextNote;
    editAutoNote_ = (r.autoNote != 0);
    editTriggerIdx_ = triggerIndexOf(r.triggerType);
    // 「事件」分支回填：按 decode 结果逐项勾选；坏配置不静默（与 cronTextOf 同口径）
    editEvents_ = {};
    if (r.triggerType == "event") {
        auto cfg = decodeTriggerConfig(r.triggerConfig);
        if (cfg.isOk()) {
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
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
    editCronTemplateOpen_ = false;
    refreshCronPreview();
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

// 打开详情弹窗：每开一次回到「最近一次 run」（selectedRunId_ 清空，refreshData 回落首条）
void AgentModule::openTaskDetail(int64_t taskId) {
    detailTaskId_ = taskId;
    selectedRunId_.reset();
    detailMessages_.clear();
    // 消息展开态按 seq 定位且只在 run 内有意义：换任务即清
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    ++msgScrollVersion_;
    tasksDirty_ = true;
}

// 关详情弹窗：只退弹窗态，不触发重查
void AgentModule::closeTaskDetail() {
    detailTaskId_.reset();
    detailTaskRow_.reset();
    selectedRunId_.reset();
    detailMessages_.clear();
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    ++msgScrollVersion_;
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

// 装配层每帧拉取路径（app::compose 内调用）：迁移检测不依赖智能体页可见性。
// 守卫：store_ 在 +（uiPolling_ 或 页脏 或 上次观测仍有 running run）。第三个条件
// 覆盖不经页面启动的 run（直写库/外部进程）：prevRunStatuses_ 有 running 就该继续
// 轮询到终态，否则迁移永不发生。三条件全假时零常规查询——但那也是冷启动盲区
// （外部直写库新插入的 running run 永远进不了 prevRunStatuses_），靠低频轻查询兜底
// （pollRunEndFallback），命中即 refreshData 进入正常观测路径。
void AgentModule::pollRunEnd() {
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
    if (pendingNotices_.empty()) return std::nullopt;
    auto notice = std::move(pendingNotices_.front());
    pendingNotices_.erase(pendingNotices_.begin());
    return notice;
}
// 任务级 run 汇总缓存（writeVersion 判脏，同 home 余量卡口径）：版本没变零查询零锁；
// 失败不静默——错误文案留在 runSummariesError_，详情弹窗汇总行显形
void AgentModule::refreshRunSummaries() {
    if (store_ == nullptr) return;
    const uint64_t ver = store_->writeVersion();
    if (runSummariesSeenVersionValid_ && runSummariesSeenVersion_ == ver) return;
    auto r = store_->listTaskRunSummaries();
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
        taskRuns_[task.id] = std::move(list);
    }
    // 两类 LLM 预检错误（代理未启 / 槽位解析失败）同步到页内错误行：run 线程失败只
    // LOG_ERROR，不落 pageError_，弹窗外将零可见出口。仅 pageError_ 为空时同步（用户
    // 动作的错误优先），且只认 needsClaudePageHint 的两类错误——普通失败不上页内错误行。
    if (pageError_.empty()) {
        for (const auto& [taskId, run] : latestRuns_) {
            (void)taskId;
            if (run.error && needsClaudePageHint(*run.error)) {
                pageError_ = *run.error;
                break;
            }
        }
    }
    // run 完成通知：本次轮询 vs 上次轮询的 running→failed/limited 迁移必响；
    // completed/cancelled 静默；同一 runId 只通知一次（notifiedRunIds_）。
    // 只看 latestRuns_ 即可：刚结束的 run 必是其任务的最近一次 run，不会漏迁移。
    // 通知不在此直发 toast：入 pendingNotices_ 队列，由装配层每帧 takeRunEndNotice()
    // 取走后发 app 级 toast（app::showAppNotice，跨页可见）——页面无关（fix round）。
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
            pendingNotices_.emplace_back(
                st == "failed" ? "智能体任务失败" : "智能体任务受限",
                "「" + taskName + "」运行结束（" + runStatusText(st) + "）");
            notifiedRunIds_.insert(runId);
        }
        prevRunStatuses_ = std::move(nowStatus);
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
            } else {
                LOG_ERROR("Agent: 读取 run 消息失败: " + msgs.errorMessage());
            }
        } else {
            selectedRunId_.reset();
        }
        // 装载后套默认展开态（进行中的 run 新落 assistant 文本会成为新展开目标）
        applyDefaultMessageExpansion();
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

// 消息行渲染：.head（徽章 + 展开/收起 + 复制 chip）、收起态 .preview、展开态逐块 .blk.<j>；
// text 块 maxWidth 必须显式给（measureTextSize 只认 element.maxWidth）
void AgentModule::renderMessageRow(
    ::eui::Ui& inner, float cw, const std::string& prefix, size_t index,
    const AgentMessageRow& msg, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& tokens) {
    (void)t;
    const std::string mid = prefix + ".msg." + std::to_string(index);
    const bool expanded = expandedMsgSeqs_.count(msg.seq) > 0;
    const float boxH = estimateMessageHeight(msg, expanded, cw, 18.0f);
    const std::vector<MessageBlock> blocks =
        decodeMessageBlocks(msg.contentJson);
    const std::string badge = messageRoleBadge(msg);

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
                        .size(44.0f, 20.0f)
                        .text(badge)
                        .fontSize(11.0f)
                        .lineHeight(20.0f)
                        .fontWeight(600)
                        .color(badge == "工具" ? tokens.primary
                                              : t.colors.textSecondary)
                        .build();
                    actionChip(inner, mid + ".toggle",
                               expanded ? "收起" : "展开", 48.0f,
                               /*primary=*/false, /*enabled=*/true, tokens,
                               [this, seq = msg.seq] {
                                   toggleMessageExpanded(seq);
                               });
                    if (expanded) {
                        actionChip(inner, mid + ".copy", "复制", 48.0f,
                                   /*primary=*/false, /*enabled=*/true, tokens,
                                   [text = messagePlainText(blocks)] {
                                       // setClipboardText 返回 void 无法校验 → LOG 留痕
                                       core::window::setClipboardText(text);
                                       LOG_INFO("Agent: 已复制消息全文 " +
                                                std::to_string(text.size()) +
                                                " 字节到剪贴板");
                                   });
                    }
                })
                .build();
            if (!expanded) {
                inner.text(mid + ".preview")
                    .size(cw, 18.0f)
                    .text(messagePreview(msg))
                    .fontSize(12.0f)
                    .lineHeight(18.0f)
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
                    .size(cw, 18.0f)
                    .text("（空消息）")
                    .fontSize(12.0f)
                    .lineHeight(18.0f)
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
                            .size(cw, estimateTextBlockHeight(b.text, cw, 12.0f,
                                                              18.0f))
                            .text(b.text)
                            .fontSize(12.0f)
                            .lineHeight(18.0f)
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
                            .size(cw, kMsgHeaderH)
                            .text("🔧 " + b.toolName + "  " +
                                  toolArgsSummary(b.text, 120))
                            .fontSize(12.0f)
                            .lineHeight(20.0f)
                            .wrap(false)
                            .color(tokens.primary)
                            .build();
                        break;
                    }
                    case MessageBlock::Type::ToolResult: {
                        inner.text(bid)
                            .size(cw, 18.0f)
                            .text(b.isError ? "工具输出（错误）" : "工具输出")
                            .fontSize(11.0f)
                            .lineHeight(18.0f)
                            .color(b.isError ? t.colors.error
                                             : t.colors.textTertiary)
                            .build();
                        inner.text(bid + ".out")
                            .size(cw, estimateTextBlockHeight(b.text, cw, 12.0f,
                                                              18.0f))
                            .text(b.text)
                            .fontSize(12.0f)
                            .lineHeight(18.0f)
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
                // 「去 Claude 页」引导：仅两类 LLM 预检错误渲染（needsClaudePageHint 前缀钉死）
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
                        auto it = latestRuns_.find(task.id);
                        if (it != latestRuns_.end()) {
                            status = runStatusText(it->second.status);
                        }
                        // 宽度分配：状态 80 / 详情·编辑·启停 各 48 / [入队 56] / 启动 72；childCount 须与实际子元素数一致
                        const bool resident = task.triggerType == "resident";
                        const float rowGap = 12.0f;
                        const int childCount = 6 + (resident ? 1 : 0);
                        const float fixedW = 80.0f + 48.0f * 3.0f + 72.0f +
                                             (resident ? 56.0f : 0.0f);
                        const float titleW =
                            cw - fixedW - rowGap * static_cast<float>(childCount - 1);
                        // 点行本体开详情弹窗（子元素先于父行命中，各自独立消费点击）
                        inner.row(id)
                            .size(cw, kTaskRowH)
                            .alignItems(eui::Align::CENTER)
                            .gap(rowGap)
                            .interactive(true)
                            .onClick([this, taskId = task.id] {
                                openTaskDetail(taskId);
                            })
                            .content([&] {
                                inner.text(id + ".title")
                                    .size(titleW, 20.0f)
                                    .text(task.name)
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                inner.text(id + ".status")
                                    .size(80.0f, 20.0f)
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
                                    .text("启动")
                                    .theme(themeTokens)
                                    .disabled(loop_ == nullptr)
                                    .onClick([this, taskId = task.id] {
                                        startTask(taskId);
                                        // 启动 run 后立即开详情弹窗，随 uiPolling_ 实时更新
                                        openTaskDetail(taskId);
                                    })
                                    .build();
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();

    if (taskDialogOpen_) {
        composeTaskDialog(ui, screen, t, themeTokens);
    }
    if (detailTaskId_) {
        composeTaskDetailDialog(ui, screen, t, themeTokens);
    }
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
    const bool isEdit = editingTaskId_.has_value();

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
        const float ddW = 140.0f;
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
                    .onChange([this](int idx) { editTriggerIdx_ = idx; })
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
                // SwitchBuilder 无 position，套 stack 定位（destructive 同款）
                ui.stack("agent.dialog.task.event.wrap." + std::to_string(i))
                    .position(evX + static_cast<float>(i) * (itemW + kDialogGap),
                              triggerY)
                    .size(itemW, kDialogRowH)
                    .content([&] {
                        components::toggleSwitch(
                            ui, "agent.dialog.task.event." + std::to_string(i))
                            .size(itemW, 32.0f)
                            .text(kEventLabels[i])
                            .checked(editEvents_[i])
                            .fontSize(12.0f)
                            .theme(themeTokens)
                            .onChange([this, i](bool v) { editEvents_[i] = v; })
                            .build();
                    })
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
                .onChange([this](const std::string& v) {
                    editCron_ = v;
                    refreshCronPreview();
                })
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
        // 用 switch 而非 checkbox（-Werror 下有未使用变量告警）；SwitchBuilder 无 position，包一层 stack 定位
        const float switchX = iterX + 100.0f + kDialogGap;
        const float switchW = innerW - (switchX - x0);
        ui.stack("agent.dialog.task.destructive.wrap")
            .position(switchX, metaY)
            .size(switchW, kDialogRowH)
            .content([&] {
                components::toggleSwitch(ui, "agent.dialog.task.destructive")
                    .size(switchW, 32.0f)
                    .text("允许破坏性操作")
                    .checked(editDestructive_)
                    .fontSize(12.0f)
                    .theme(themeTokens)
                    .onChange([this](bool v) { editDestructive_ = v; })
                    .build();
            })
            .build();

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
        ui.stack("agent.dialog.task.autonote.wrap")
            .position(x0 + kDialogLabelW + kDialogGap, autoY)
            .size(autoSwitchW, kDialogRowH)
            .content([&] {
                components::toggleSwitch(ui, "agent.dialog.task.autonote")
                    .size(autoSwitchW, 32.0f)
                    .text("结束后自动记录回复")
                    .checked(editAutoNote_)
                    .fontSize(12.0f)
                    .theme(themeTokens)
                    .onChange([this](bool v) { editAutoNote_ = v; })
                    .build();
            })
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

// 详情弹窗：配置摘要 / run 列表 / 消息流三段，数据来自模块缓存（run 进行中实时更新）
void AgentModule::composeTaskDetailDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    const float h = std::min(kDetailDialogH, screen.height - 40.0f);
    const float innerW = w - ui_kit::kDialogBodyInsetX;
    const float x0 = ui_kit::kDialogInsetX;

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

        std::string info;
        if (detailTaskRow_) {
            const AgentTaskRow& task = *detailTaskRow_;
            info = "目标：" + task.goal;
            info += " · 触发：" + task.triggerType;
            if (task.triggerType == "schedule") {
                const std::string cron = cronTextOf(task);
                info += " · cron：" + (cron.empty() ? "（未设置）" : cron);
            }
            info += " · 槽位：" + task.modelSlot;
            info += " · 迭代上限：" + std::to_string(task.maxIterations);
            info += task.allowDestructive ? " · 破坏性：允许" : " · 破坏性：禁止";
        } else {
            info = "（任务信息读取失败）";
        }
        ui.text("agent.dialog.detail.info")
            .position(x0, 60.0f)
            .size(innerW, 36.0f)
            .text(info)
            .fontSize(12.0f)
            .lineHeight(18.0f)
            .wrap(true)
            .maxWidth(innerW)
            .color(t.colors.textSecondary)
            .build();

        const auto runsIt = taskRuns_.find(*detailTaskId_);
        const std::vector<AgentRunRow>* runs =
            runsIt != taskRuns_.end() ? &runsIt->second : nullptr;
        // 任务级汇总行（数据已在 refreshRunSummaries 缓存）：有 run 或查询失败才渲染，
        // 占 y=100..118，下方区块整体下移 18；无 run 的任务不渲染、几何保持原值
        const auto sumIt = runSummaries_.find(*detailTaskId_);
        const bool hasSummary =
            sumIt != runSummaries_.end() || !runSummariesError_.empty();
        const float runsTitleY = hasSummary ? 122.0f : 104.0f;
        if (hasSummary) {
            std::string summaryText;
            if (!runSummariesError_.empty()) {
                summaryText = "运行汇总读取失败：" + runSummariesError_;
            } else {
                std::string lastTimeText = "—";
                if (sumIt->second.lastEndedAt) {
                    std::tm local{};
                    core::time::localTime(
                        static_cast<std::time_t>(*sumIt->second.lastEndedAt),
                        &local);
                    lastTimeText = formatRunResultTimeText(local);
                }
                summaryText = formatTaskRunSummaryText(sumIt->second, lastTimeText);
            }
            ui.text("agent.dialog.detail.summary")
                .position(x0, 100.0f)
                .size(innerW, 18.0f)
                .text(summaryText)
                .fontSize(12.0f)
                .lineHeight(18.0f)
                .wrap(false)
                .color(runSummariesError_.empty() ? t.colors.textSecondary
                                                  : t.colors.error)
                .build();
        }
        ui.text("agent.dialog.detail.runs.title")
            .position(x0, runsTitleY)
            .size(innerW, 18.0f)
            .text(runs != nullptr && !runs->empty()
                      ? "运行记录（最近 " + std::to_string(runs->size()) + " 次）"
                      : "运行记录")
            .fontSize(12.0f)
            .lineHeight(18.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        float runsH = 18.0f;
        if (runs != nullptr && !runs->empty()) {
            // gap 数 = 子元素数 - 1：error 行与 hint 行都是子元素，漏算会让消息标题压到上者
            float childrenH = 0.0f;
            int childCount = 0;
            for (const auto& run : *runs) {
                ++childCount;
                childrenH += 18.0f;
                if (run.error && !run.error->empty()) {
                    ++childCount;
                    childrenH += 16.0f;
                    // 「去 Claude 页」引导行：仅两类 LLM 预检错误渲染，与下方渲染条件一致
                    if (needsClaudePageHint(*run.error)) {
                        ++childCount;
                        childrenH += 16.0f;
                    }
                }
            }
            runsH = childrenH + 4.0f * static_cast<float>(childCount - 1);
            ui.column("agent.dialog.detail.runlist")
                .position(x0, runsTitleY + 22.0f)
                .size(innerW, runsH)
                .gap(4.0f)
                .content([&] {
                    for (size_t i = 0; i < runs->size(); ++i) {
                        const std::string runRowId =
                            "agent.dialog.detail.runlist." + std::to_string(i);
                        const AgentRunRow& run = (*runs)[i];
                        const bool highlighted =
                            selectedRunId_ && *selectedRunId_ == run.id;
                        ui.text(runRowId)
                            .size(innerW, 18.0f)
                            .text(runRowText(run))
                            .fontSize(12.0f)
                            .lineHeight(18.0f)
                            .wrap(false)
                            .color(highlighted ? t.colors.primary
                                               : t.colors.textSecondary)
                            .onClick([this, runId = run.id] {
                                selectedRunId_ = runId;
                                // seq 只在 run 内唯一：切 run 清消息展开态
                                expandedMsgSeqs_.clear();
                                autoExpandedSeq_.reset();
                                ++msgScrollVersion_;
                                tasksDirty_ = true;
                            })
                            .build();
                        if (run.error && !run.error->empty()) {
                            ui.text(runRowId + ".err")
                                .size(innerW, 16.0f)
                                .text("错误：" + *run.error)
                                .fontSize(11.0f)
                                .lineHeight(16.0f)
                                .wrap(false)
                                .color(t.colors.error)
                                .build();
                            // 「去 Claude 页」引导：仅两类 LLM 预检错误渲染（needsClaudePageHint 前缀钉死）
                            if (needsClaudePageHint(*run.error)) {
                                ui.text(runRowId + ".err.hint")
                                    .size(innerW, 16.0f)
                                    .text("去 Claude 页 →")
                                    .fontSize(11.0f)
                                    .lineHeight(16.0f)
                                    .wrap(false)
                                    .color(t.colors.primary)
                                    .onClick([] { navigateToClaudePage(); })
                                    .build();
                            }
                        }
                    }
                })
                .build();
        } else {
            ui.text("agent.dialog.detail.runlist.empty")
                .position(x0, runsTitleY + 22.0f)
                .size(innerW, 18.0f)
                .text("（还没有运行记录）")
                .fontSize(12.0f)
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();
        }

        // ③ 消息流：scrollView 内滚。最终 assistant 文本块默认展开，其余默认折叠
        const float msgsTitleY = runsTitleY + 30.0f + runsH;
        ui.text("agent.dialog.detail.msgs.title")
            .position(x0, msgsTitleY)
            .size(innerW, 18.0f)
            .text("消息（" + std::to_string(detailMessages_.size()) + "）")
            .fontSize(12.0f)
            .lineHeight(18.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        // 「复制回复」：一键复制 run 最终回复全文（最后一条 assistant 的全部 text 块，
        // 见 finalAssistantText）。空串（run 刚起 / 全 tool_use）不渲染。
        // 几何：chip 高 32 比标题行 18 高，y 上移 7 与标题文本带居中对齐（destructive
        // switch 同款 wrap 定位法——actionChip 自身不带 position）
        const std::string finalText = finalAssistantText(detailMessages_);
        if (!finalText.empty()) {
            constexpr float kCopyFinalChipW = 76.0f;
            const float chipX = x0 + innerW - kCopyFinalChipW;
            const float chipY = msgsTitleY - 7.0f;
            ui.stack("agent.dialog.detail.msgs.copyfinal.wrap")
                .position(chipX, chipY)
                .size(kCopyFinalChipW, 32.0f)
                .content([&] {
                    actionChip(ui, "agent.dialog.detail.msgs.copyfinal",
                               "复制回复", kCopyFinalChipW, /*primary=*/true,
                               /*enabled=*/true, themeTokens, [text = finalText] {
                                   // setClipboardText 返回 void 无法校验 → LOG 留痕
                                   core::window::setClipboardText(text);
                                   LOG_INFO("Agent: 已复制最终回复 " +
                                            std::to_string(text.size()) +
                                            " 字节到剪贴板");
                               });
                })
                .build();
        }
        const float scrollY = msgsTitleY + 22.0f;
        const float availH = std::max(kMsgAreaMinH, h - scrollY - 24.0f);
        const float scrollH = std::min(
            estimateMessagesAreaHeight(detailMessages_, expandedMsgSeqs_, innerW,
                                       18.0f, kMsgAreaMinH, kMsgAreaMaxH),
            availH);
        if (detailMessages_.empty()) {
            ui.text("agent.dialog.detail.empty")
                .position(x0, scrollY)
                .size(innerW, scrollH)
                .text("（选中的 run 还没有消息）")
                .fontSize(12.0f)
                .lineHeight(18.0f)
                // 盒高 ≠ 行高，不给 verticalAlign 会顶在上缘
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        } else {
            components::scrollView(ui, "agent.dialog.detail.msgscroll")
                .position(x0, scrollY)
                .size(innerW, scrollH)
                .gap(kMsgRowGap)
                .step(18.0f)
                .theme(themeTokens)
                .contentKey("agent.dialog.detail.msgscroll.v" +
                            std::to_string(msgScrollVersion_) + ".n" +
                            std::to_string(detailMessages_.size()))
                .content([&](eui::Ui& inner, float cw, float) {
                    for (size_t i = 0; i < detailMessages_.size(); ++i) {
                        renderMessageRow(inner, cw, "agent.dialog.detail", i,
                                         detailMessages_[i], t, themeTokens);
                    }
                })
                .build();
        }
    });
}

// Esc 关最上层弹窗（详情优先）。键走得到本通道 = 焦点元素没消费（输入框聚焦时已被 onCancel 收走）
bool AgentModule::onKeyEvent(const ::eui::KeyEvent& event) {
    if (!event.isDown()) return false;
    if (event.key != eui::InputKey::Escape) return false;
    if (detailTaskId_) {
        closeTaskDetail();
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
