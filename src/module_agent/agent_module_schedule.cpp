// 智能体模块·调度块（AgentModule 成员函数定义）：run/线程编排（startTask / 停止 /
// 收割 join）、LLM 通道与 Scheduler 惰性装配、结束轮询与完成通知、任务/运行数据
// 刷新（refreshData / run 汇总 / 下一次·计划缓存），以及任务弹窗的调度配置编辑器
//（新建/编辑/保存/试运行，cron·事件·闹钟校验与预览、触发配置编解码）。
// 拆分（04 号票）产物：类定义在 agent_module.h，跨 TU 共享助手在
// agent_module_internal.h；本文件只承载成员函数体与 TU 私有助手，
// 纯代码搬移，逻辑与签名零变化。

#include "agent_module_internal.h"

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

// 任务列表每任务只查最近 1 条（列表状态/通知足够，spec §8.6 D8）。
// kDetailRunListLimit（=20）在 agent_ui_shared.h——列表查询与详情 UI 共用同一上限
constexpr int kRunListLimit = 1;

// 闹钟时间文本 ↔ epoch 的纯函数集（trimCopy / parseOnceAtLocalText /
// formatOnceAtText / defaultOnceAtText / onceTextOf）已搬至 task_draft_parse.h：
// 草稿解析层（task_draft.cpp）需要同一份闹钟校验，留在本文件私有会造成
// 「模型草稿说合法、UI 说非法」的双真源。搬移是行为等价的，判定规则与文案逐字未改。


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

} // namespace module::agent
