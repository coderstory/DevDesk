// src/module_agent/agent_module.cpp
// 智能体页模块：生命周期 + UI（工具区 / 任务列表 / 任务弹窗 / 详情弹窗）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）。
// 可点元素契约：id 元素本身须同时带 text 与 onClick；components::button 的 onClick
// 落在 <id>.bg 上 → 行动作 chip 手搓「文本元素 + .bg 底」两层。
// 弹窗字段标签一律 .caption：components::dropdown 内部占用 <id>.label，同 id 会串位
//（agent.tools.header.label 例外，header 非 dropdown）。

#include "agent_module.h"

#include <algorithm>
#include <functional>
#include <system_error>
#include <utility>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <core/window/window_backend.h>
#include <eui/dsl.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>

#include "model_slots.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/service_locator.h"
#include "core/theme.h"
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
        // 孤儿 = 收尾写库失败，DB 行永停 running 但无活线程；算进去 uiPolling_ 永不复位。
        // 比对 run.id（非 map 键 taskId）——FinishFailureFn 回报的是 runId
        if (orphanedRunIds.count(run.id) != 0) continue;
        return true;
    }
    return false;
}

namespace {

// run 状态 → 中文展示；未知态显示「—」（字串源：AgentStore.listRuns）
std::string runStatusText(const std::string& status) {
    if (status == "completed") return "成功";
    if (status == "failed") return "失败";
    if (status == "limited") return "受限";
    if (status == "cancelled") return "已取消";
    if (status == "running") return "运行中";
    return "—";
}

// run 行文案：耗时秒 / 迭代 / token 合计（皆空显示「—」）；token 不加千分位
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

// 行内动作 chip：契约要求 id 落在可见文本元素上，故手搓两层——<id>.bg（底+态+
// onClick 真实命中面）与 <id>（文本元素 hitTestMode=None 穿透到 bg，测试直调 onClick）
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

// 消息收起态预览：解码纯文本前 120 字节（解码/截断统一走 message_blocks）
std::string messagePreview(const AgentMessageRow& m) {
    return messagePreviewFromJson(m.contentJson, 120);
}

// 任务名派生：goal 前 24 字节（UTF-8 边界回退）、换行压空格恒单行；goal 非空故结果非空
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

// 模型槽位键集与显示名：真源 = src/model_slots.h 的 X 宏表，新增槽位只改那里；
// 键集进库（agent_tasks.model_slot），显示名只上屏
#define DEVDESK_MODEL_SLOT_KEY(name, jsonKey, label, formLabel, envKey, nameKey, oneM) jsonKey,
constexpr const char* kModelSlotKeys[] = {DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_KEY)};
#undef DEVDESK_MODEL_SLOT_KEY
#define DEVDESK_MODEL_SLOT_LABEL(name, jsonKey, label, formLabel, envKey, nameKey, oneM) label,
constexpr const char* kModelSlotLabels[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_LABEL)};
#undef DEVDESK_MODEL_SLOT_LABEL
constexpr int kModelSlotCount = static_cast<int>(
    sizeof(kModelSlotKeys) / sizeof(kModelSlotKeys[0]));

// 触发类型三档（agent_tasks.trigger_type）；下拉下标 = 本表下标
constexpr const char* kTriggerKeys[] = {"manual", "schedule", "resident"};
constexpr const char* kTriggerLabels[] = {"手动", "定时", "常驻"};
constexpr int kTriggerCount = 3;
constexpr int kTriggerScheduleIdx = 1;

constexpr float kPageHeaderH = 40.0f;
constexpr float kNewTaskBtnW = 140.0f;
constexpr float kTaskRowH = 44.0f;
// 页内错误行高（agent.page.error，进页面 column 子项预算）
constexpr float kPageErrorH = 16.0f;

// 任务弹窗几何（cron 与触发同行、仅「定时」渲染、弹窗高度不变）
constexpr float kDialogRowH = 40.0f;
constexpr float kDialogLabelW = 72.0f;
constexpr float kDialogGap = 12.0f;
constexpr float kTaskDialogH = 400.0f;
constexpr float kGoalInputH = 96.0f;

// 详情弹窗内容高 660（窄屏由 dialog 组件按视口钳制）
constexpr float kDetailDialogH = 660.0f;

// run 列表长度上限（契约：最近 5 次）
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
    return 0;   // 未知触发类型（旧数据）→ 显示「手动」，保存时按当前下拉值归一
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
    // 兜底：正常路径 onStop 已清空 runs_，此处多为 no-op。只收线程不注销工具
    //（工具对象可能仍被 run 线程的 shared_ptr 持有）
    for (auto& r : runs_) {
        r->cancel = true;
    }
    for (auto& r : runs_) {
        if (r->thread.joinable()) r->thread.join();
    }
}

void AgentModule::onStart() {
    // 重入幂等：已启动则 no-op。双 onStart 会重建服务、重复注册工具，
    // 清扫还会误伤重入期间新出现的 running run
    if (started_) return;
    started_ = true;
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

    // LLM 通道装配预热：有服务 → 建 llm_/loop_/scheduler_；无服务 → 只 WARN，
    // store/工具照常装配、手动启动禁用。首次使用时会再调本入口（见 ensureLlm）
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
    // 重入幂等：未启动则 no-op。teardown 顺序：置取消 → join → 清容器 →
    // 注销工具 → 放服务（工具注销须晚于所有 run 线程退出）
    if (!started_) return;
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
    // Scheduler 收口：onStart 早退时 scheduler_ 未创建，须判空。stopAll 先停
    // tick/常驻派发线程，再 cancel+join 在跑 run（先灭派发源，收尾窗口不再起新 run）
    if (scheduler_) scheduler_->stopAll();
    ::core::ToolRegistry::instance().unregisterTool("app.stats");
    // 释放序：loop_/llm_/store_ 先走也安全——stopAll 已 join 全部线程
    loop_.reset();
    llm_.reset();
    store_.reset();
    scheduler_.reset();
    statsTool_.reset();
    llmMissingWarned_ = false;
    tasks_.clear();
    latestRuns_.clear();
    taskRuns_.clear();
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

// 惰性装配单入口：代理服务首次使用时解析——启动时 claude 关、会话内拨开无须
// off→on 本模块。幂等：loop_ 非空即返回；scheduler_ 须在此与 loop_ 同建（否则惰性路径下 cron 永不触发）
void AgentModule::ensureLlm() {
    if (loop_ != nullptr) return;       // 已就绪（llm_/loop_/scheduler_ 三者同生共死）
    if (store_ == nullptr) return;      // 装配未完成（db 缺失的早退路径）
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
                                            // 收尾写库失败 → DB 行永停 running，
                                            // 记为孤儿供判停排除（否则 uiPolling_ 永不复位）
                                            std::lock_guard<std::mutex> lock(runsMu_);
                                            orphanedRunIds_.insert(runId);
                                            LOG_ERROR("Agent: run " + std::to_string(runId) +
                                                      " 收尾写库失败，已记为孤儿 run（"
                                                      "不计入 UI 轮询判停）：" + reason);
                                        });
    if (scheduler_ == nullptr) {
        scheduler_ = std::make_unique<Scheduler>(*store_, *loop_, deps_.schedulerNow);
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
            // 生命周期由 onStop 的 join 先于 reset 保证；落库在 run 内完成。
            // taskFromRow = store 行 → AgentTask 的唯一映射，不留手抄副本
            const AgentTask t = taskFromRow(task);
            // 返回值 = 起库失败这类不可恢复错误——不能静默
            auto started = loop_->run(t, raw->cancel);
            if (started.isFail()) {
                LOG_ERROR("Agent: run 启动失败（task " +
                          std::to_string(task.id) + "）: " +
                          started.errorMessage());
            }
            raw->done.store(true);
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

// 新建态：editingTaskId_ 清空即「新建」，字段回默认值；与编辑态共用同一张表单
void AgentModule::openNewTaskDialog() {
    taskDialogOpen_ = true;
    pageError_.clear();   // 成功动作清掉上一次的页内错误
    editingTaskId_.reset();
    editGoal_.clear();
    editCron_.clear();
    editSlot_ = kModelSlotKeys[0];
    editIter_ = 10;
    editDestructive_ = false;
    editTriggerIdx_ = 0;
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
}

// 关闭任务弹窗（多条路径共用）：不落库、不碰任务列表，只退弹窗态与草稿
void AgentModule::closeTaskDialog() {
    taskDialogOpen_ = false;
    editingTaskId_.reset();
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
}

void AgentModule::setEditCronDraft(const std::string& cron) {
    editCron_ = cron;
}

void AgentModule::setDialogIterDraft(int iter) {
    editIter_ = iter;
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
    pageError_.clear();   // 成功动作清掉上一次的页内错误
    taskDialogOpen_ = true;
    editingTaskId_ = r.id;
    editGoal_ = r.goal;
    editCron_ = cronTextOf(r);
    editSlot_ = r.modelSlot;
    editIter_ = r.maxIterations;
    editDestructive_ = r.allowDestructive;
    editTriggerIdx_ = triggerIndexOf(r.triggerType);
    editError_.clear();
    editTriggerOpen_ = false;
    editSlotOpen_ = false;
}

// 保存任务弹窗（新建/编辑同一路径）：定时先 parseCron 校验，非法 → 弹窗内报错不关窗
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
        auto encoded = encodeTriggerConfig(TriggerConfig{cron});
        if (encoded.isFail()) {
            editError_ = "保存失败：" + encoded.errorMessage();
            LOG_ERROR("Agent: 触发配置编码失败: " + encoded.errorMessage());
            return;
        }
        triggerConfig = encoded.value();
    }
    const int iter = editIter_ < 1 ? 1 : (editIter_ > 50 ? 50 : editIter_);
    if (editingTaskId_) {
        auto updated = store_->updateTask(*editingTaskId_, deriveTaskName(goal), goal,
                                          triggerType, triggerConfig, editSlot_, iter,
                                          editDestructive_);
        if (updated.isFail()) {
            editError_ = "保存失败：" + updated.errorMessage();
            LOG_ERROR("Agent: 保存任务失败: " + updated.errorMessage());
            return;
        }
    } else {
        auto created = store_->createTask(deriveTaskName(goal), goal, triggerType,
                                          editSlot_, iter, editDestructive_,
                                          triggerConfig);
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

// 关详情弹窗：只退弹窗态（不触发重查，列表数据同一份）
void AgentModule::closeTaskDetail() {
    detailTaskId_.reset();
    detailTaskRow_.reset();
    selectedRunId_.reset();
    detailMessages_.clear();
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    ++msgScrollVersion_;
}

// 默认展开态：最近一条带 text 的 assistant 消息；只在目标 seq 变化时改展开集，
// 用户手动收起后不被每帧顶回来
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
    ++msgScrollVersion_;   // 内容高变了 → scrollView 重测
}

// 「可用工具」注册表快照（惰性装载；展开时强制重装反映模块启停）
void AgentModule::ensureToolsCache(bool forceReload) {
    if (toolsCacheLoaded_ && !forceReload) return;
    toolsCache_ = listRegisteredTools();
    toolsCacheLoaded_ = true;
}

// 重查任务 / 最近 run / 详情消息。触发：tasksDirty_ 或 uiPolling_。判停：run 线程全
// done 且库内无 running；先查 done 再查库——done==true 时 finishRun 已先行发生
void AgentModule::refreshData() {
    tasksDirty_ = false;
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
        // 最近一次在前：store 已按 (started_at DESC, id DESC) 排序，此处再排一次
        // 是纵深防御，让 runlist[0] 语义不依赖 store 排序契约
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
            selectedRunId_.reset();   // 该任务还没有 run
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

// 单条消息行渲染：.head（徽章 + 展开/收起 chip + 展开时复制 chip）；收起态 .preview；
// 展开态逐块 .blk.<j>（text 的 maxWidth 必须显式给——measureTextSize 只认 element.maxWidth）
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
                    // 点消息行也能展开
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
    const auto& t = ::core::theme::current();
    const auto themeTokens = ::core::theme::componentTokens(t);
    const float pad = t.sizes.spacingXXLarge;              // 40，照 claude 页
    const float contentW = screen.width - pad * 2.0f;

    // 首次使用路径之二：页面帧。会话内拨开 claude 后回本页即解析服务，否则
    // loop_ 恒空、「启动」永久禁用，惰性解析在真实 UI 上不可达
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
    // 页 column 子项：标题行 / [错误行] / tools / tasklist，gap = spacingLarge。
    // 子项数或行高一变，kPageGapCount 与减项必须同步（否则列表高度算错、末尾行溢出）
    const bool hasPageError = !pageError_.empty();
    const int kPageGapCount = 2 + (hasPageError ? 1 : 0);
    const float listH = screen.height - pad * 2.0f - kPageHeaderH - toolsH -
                        (hasPageError ? kPageErrorH : 0.0f) -
                        static_cast<float>(kPageGapCount) * t.sizes.spacingLarge;

    ui.column("agent.page")
        .size(screen.width, screen.height)
        .padding(pad)
        .gap(t.sizes.spacingLarge)
        .content([&] {
            // 标题行：标题 + 「+ 新建任务」（唯一新建入口）
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
            }

            // 可用工具区块：收起默认，头部点击展开。列 ToolRegistry 全部已注册工具，
            // 卡多了内滚（kToolsListMaxH）
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

            // 任务列表：scrollView 承载，viewport = 页面剩余高；行高 kTaskRowH（step
            // 同源）。行内动作：启动 / 详情 / 编辑 / 禁用（常驻多一个入队）
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
                        // 宽度分配：状态 80 / 详情·编辑·启停 各 48 / [入队 56] / 启动 72，
                        // 间距 12，标题吃剩余。childCount 必须与实际子元素数一致
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
                                // 契约要求 id 元素本身带 text 与 onClick（见 actionChip）
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
                                    // 常驻入队（本期只跑手动入队子任务）；Scheduler 未装配时置灰
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

    // 弹窗层（模态，画在页面之后）：关闭态不建内容节点，存在性即开关
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
    const bool isEdit = editingTaskId_.has_value();

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.task";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = h;
    frame.open = taskDialogOpen_;
    // 点遮罩 = 关弹窗（不落库）
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

        // 任务目标（多行）
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

        // 触发方式（定时才在同一行尾随 cron 输入，弹窗高度不变）
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
        if (isSchedule) {
            const float cronLabelX = fieldX + ddW + kDialogGap;
            const float cronX = cronLabelX + kDialogLabelW + kDialogGap;
            fieldCaption(ui, "agent.dialog.task.cron.caption", "cron 表达式",
                         cronLabelX, triggerY, kDialogLabelW, kDialogRowH,
                         t.colors.textSecondary);
            components::input(ui, "agent.dialog.task.cron")
                .position(cronX, triggerY)
                .size(innerW - (cronX - x0), kDialogRowH)
                .value(editCron_)
                .placeholder("分 时 日 月 周，例如 0 9 * * 1-5")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([this](const std::string& v) { editCron_ = v; })
                .onCancel([this] { closeTaskDialog(); })
                .build();
        }

        // 模型槽位 + 迭代上限 + 破坏性授权（同一行）
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
        // 用 switch 而非 checkbox（后者在 -Werror 下有未使用变量告警）；
        // SwitchBuilder 无 position，包一层同尺寸 stack 定位
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

        // 校验 / 保存错误（有错才渲染，弹窗不关）
        ui_kit::drawDialogText(ui, "agent.dialog.task.error", w, editError_,
                               t.sizes.fontSizeSmall, t.colors.error,
                               {.x = x0, .y = 276.0f, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});

        // 底部按钮对：取消 / 保存（几何与 ui_kit 标准按钮对同源）
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

        // ① 任务配置摘要（getTask 快照）
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

        // ② run 列表（最近 5 次，最近在前；选中/最近一次高亮）
        const auto runsIt = taskRuns_.find(*detailTaskId_);
        const std::vector<AgentRunRow>* runs =
            runsIt != taskRuns_.end() ? &runsIt->second : nullptr;
        ui.text("agent.dialog.detail.runs.title")
            .position(x0, 104.0f)
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
            // gap 数 = 子元素数 - 1：error 行也是子元素，漏算会让列表底偏矮
            // → 消息标题压到 error 行上
            float childrenH = 0.0f;
            int childCount = 0;
            for (const auto& run : *runs) {
                ++childCount;
                childrenH += 18.0f;
                if (run.error && !run.error->empty()) {
                    ++childCount;
                    childrenH += 16.0f;
                }
            }
            runsH = childrenH + 4.0f * static_cast<float>(childCount - 1);
            ui.column("agent.dialog.detail.runlist")
                .position(x0, 126.0f)
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
                        }
                    }
                })
                .build();
        } else {
            ui.text("agent.dialog.detail.runlist.empty")
                .position(x0, 126.0f)
                .size(innerW, 18.0f)
                .text("（还没有运行记录）")
                .fontSize(12.0f)
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();
        }

        // ③ 消息流：scrollView 内滚。最终 assistant 文本块默认展开，其余默认折叠
        const float msgsTitleY = 134.0f + runsH;
        ui.text("agent.dialog.detail.msgs.title")
            .position(x0, msgsTitleY)
            .size(innerW, 18.0f)
            .text("消息（" + std::to_string(detailMessages_.size()) + "）")
            .fontSize(12.0f)
            .lineHeight(18.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        const float scrollY = msgsTitleY + 22.0f;
        // 消息区外高：估算高夹在 [kMsgAreaMinH, kMsgAreaMaxH] 内，超出内滚
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
