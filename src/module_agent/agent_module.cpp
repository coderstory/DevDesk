// src/module_agent/agent_module.cpp
// 智能体页模块实现（agent M1 Task 12）。Task 11 装配面（图标码点 / 工厂
// 签名）保持不变；本文件填实生命周期与最小 UI。
//
// 图标码点 U+F544（fa-robot）：已核对存在于 EUI-NEO 内置
// assets/Font Awesome 7 Free-Solid-900.otf 的 cmap（Task 11 离线解析）；
// eui::utf8(0xF544) 的 UTF-8 字节为 \xEF\x95\x84，与 app_registry.h
// 图标列同值（assembly_registry_matches_module_getters 锁一致性）。
//
// 元素 id 契约表（plan task-12/10/11-brief，UI 测试逐字钉；2026-10-02 run 日志
// 改造 + 可用工具区块 + 多行目标输入增补）：
//   agent.page / agent.title /
//   agent.form.goal.label（「任务目标」标签）/ agent.form.goal（多行输入）/
//   agent.form.create /
//   agent.tools（可用工具区块容器）/ agent.tools.header（点开/收起）/
//     agent.tools.header.label / agent.tools.header.hint /
//     agent.tools.scroll / agent.tools.card.<k>
//     （.bg 底 / .name / .desc / .params）
//   agent.tasklist / agent.task.<taskId> / agent.task.<taskId>.title /
//   agent.task.<taskId>.run（text=="启动"）/ agent.task.<taskId>.status /
//   agent.task.<taskId>.edit（点开编辑表单）/ agent.task.<taskId>.toggle
//     （text=="禁用"/"启用"，按 enabled）/ agent.task.<taskId>.enqueue
//     （仅 trigger_type=="resident"）/
//   agent.task.<taskId>.detail（展开才存在）/ agent.task.<taskId>.info
//     （只读任务信息行）/ agent.task.<taskId>.runlist（最近 5 次 run）/
//   agent.task.<taskId>.msgscroll（消息滚动区，展开才存在）/
//   agent.task.<taskId>.msg.<i>（消息容器，列）/
//     .msg.<i>.head / .msg.<i>.badge（角色徽章：用户/助手/工具）/
//     .msg.<i>.toggle（展开/收起 chip）/ .msg.<i>.copy（复制 chip，展开才在）/
//     .msg.<i>.preview（收起态 120 字节预览）/ .msg.<i>.blk.<j>（内容块，
//     tool_use=🔧 chip 行 / tool_result=标签行）/ .msg.<i>.blk.<j>.out
//     （tool_result 输出全文，wrap 多行）
//   agent.edit.form 及子元素（editingTaskId_ 非空才存在）：agent.edit.goal /
//     .trigger（下拉：手动/定时/常驻）/ .cron（trigger==定时才渲染）/
//     .slot（下拉）/ .iter / .destructive / .save / .cancel / .error
//
// 可点元素（edit/toggle/enqueue/save/cancel）用 actionChip：契约要求 id 元素本身
// 同时带 text 与 onClick，而 components::button 的 onClick 落在 <id>.bg 上，故
// 手搓「<id> 文本元素 + <id>.bg 底」两层（观感取自同一组 button token）。
// 表单字段标签一律 .caption 后缀——components::dropdown 内部占用 <id>.label
//（字段显示名），宿主标签若同 id 会串位（2026-10-02 实测的 UI 缺陷）。
// 注意 agent.tools.header.label 不受此约束：header 不是 dropdown，无内部 .label。
#include "agent_module.h"

#include <algorithm>
#include <functional>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <core/window/window_backend.h>
#include <eui/dsl.h>

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
namespace {

// run 状态 → 中文展示（契约：完成态含「成功/失败/已取消」；无 run / 未知态
// 显示「—」）。状态字串来自 AgentStore.listRuns（running/completed/failed/
// limited/cancelled），此处不做兜底映射以外的解读
std::string runStatusText(const std::string& status) {
    if (status == "completed") return "成功";
    if (status == "failed") return "失败";
    if (status == "limited") return "受限";
    if (status == "cancelled") return "已取消";
    if (status == "running") return "运行中";
    return "—";
}

// run 列表行文案（Task 11 契约）：状态中文化 / 耗时（endedAt-startedAt 秒）/
// 迭代 N / token 合计（input+output；两者皆空显示「—」）。
// 分隔符用「 · 」——测试按数值子串钉（如 "205000"），不加千分位
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

// 行内动作 chip（2026-10-02 用户反馈「编辑/禁用是纯文本、启动却是按钮」）：观感与
// components::button 同源（同一组 token：底色/悬停/按下/描边/圆角/高度），但
// **契约 id 必须落在可见文本元素上**（契约要求 id 元素同时带 text 与 onClick），
// 而 components::button 把 onClick 挂在 <id>.bg 上。故手搓成与 button 同构的两层：
//   <id>.bg   —— 底 + 悬停/按下态 + onClick（真实命中面）
//   <id>      —— 文本元素，带 id/text/onClick；hitTestMode=None 让指针穿透到 bg
//                （悬停态才一致），测试直调其 onClick 亦有效
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

// 表单字段标签：统一 12px + 显式垂直居中（2026-10-02 用户反馈「文本没垂直居中」——
// 光靠 lineHeight 撑高不会居中）。id 一律用 .caption：components::dropdown 内部有
// <id>.label（字段显示名），宿主标签若同 id 会与之撞车串位
void fieldCaption(::eui::Ui& ui, const std::string& id, const std::string& text,
                  float width, float height, const ::core::Color& color) {
    ui.text(id)
        .size(width, height)
        .text(text)
        .fontSize(12.0f)
        .lineHeight(20.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(color)
        .build();
}

// 消息收起态预览：解码后纯文本前 120 字节（UTF-8 边界回退 + 省略号）。
// 2026-10-02 改造：role 前缀挪进独立徽章（.badge），预览只承担文本；
// 解码/截断统一走 message_blocks（单一解码点，兼容旧纯文本行）
std::string messagePreview(const AgentMessageRow& m) {
    return messagePreviewFromJson(m.contentJson, 120);
}

// 任务名派生：M1 表单只有 goal 一个输入，任务名取 goal 前 24 字节
//（UTF-8 边界回退）。goal 非空（调用方保证）故派生结果恒非空
// 任务名派生：M1 表单只有 goal 一个输入，任务名取 goal 前 24 字节
//（UTF-8 边界边界回退）。goal 非空（调用方保证）故派生结果恒非空。
// 2026-10-02 goal 改多行输入：换行压成空格，任务名恒单行
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

// 模型槽位下拉的键集与显示名（Task 10：真源 = src/model_slots.h 的 X 宏表）。
// 「一份真源、零漂移」：新增槽位只改 model_slots.h，本模块与 provider_config
// 各自展开同一张表。键集进库（agent_tasks.model_slot），显示名只上屏
#define DEVDESK_MODEL_SLOT_KEY(name, jsonKey, label, formLabel, envKey, nameKey) jsonKey,
constexpr const char* kModelSlotKeys[] = {DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_KEY)};
#undef DEVDESK_MODEL_SLOT_KEY
#define DEVDESK_MODEL_SLOT_LABEL(name, jsonKey, label, formLabel, envKey, nameKey) label,
constexpr const char* kModelSlotLabels[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_LABEL)};
#undef DEVDESK_MODEL_SLOT_LABEL
constexpr int kModelSlotCount = static_cast<int>(
    sizeof(kModelSlotKeys) / sizeof(kModelSlotKeys[0]));

// 触发类型（agent_tasks.trigger_type）三档：下拉顺序 = 下标即本表下标
constexpr const char* kTriggerKeys[] = {"manual", "schedule", "resident"};
constexpr const char* kTriggerLabels[] = {"手动", "定时", "常驻"};
constexpr int kTriggerCount = 3;
constexpr int kTriggerScheduleIdx = 1;

// 编辑表单固定高度：内层 5 行（标题 20 / 目标 96 / 触发 40 / 槽位 40 / 动作 32
// + 4 个 8px 间距 = 260）+ 上下 padding 12×2 = 284，取 300 留余量。2026-10-02
// 目标输入改多行（96px ≈ 4 行视口，用户反馈③「输入框又小又简陋」）。列表高度
// 计算里的减项必须与它一致（compose 的 gapCount/listH），否则行会溢出页底
constexpr float kEditFormH = 300.0f;

// 新建表单目标输入高（多行，与编辑表单同款 96）
constexpr float kGoalInputH = 96.0f;

// 详情区 run 列表长度上限（契约表：最近 5 次）
constexpr int kRunListLimit = 5;

// ---- run 日志消息区（2026-10-02 改造）----
// 消息滚动区外高钳制：min ~5 行，max 520（≈26 行）——超出的真实内容高由
// scrollView 内滚（EUI-NEO 内部滚动容器），外高不再往上加
constexpr float kMsgAreaMinH = 96.0f;
constexpr float kMsgAreaMaxH = 520.0f;

// 可用工具区块：头部行高 / 工具卡高 / 滚动区最大高（卡多了内滚）
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

// 迭代上限输入框的容错解析：非数字字符忽略（正在输入的中间态），>1000 截断防溢出；
// 无数字 → 0（保存时再 clamp 进 [1,50]，与 AgentLoop 的硬上限一致）
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

// trigger_config 里的 cron 原文（只读展示用）。解析走 trigger_config.h 的唯一
// 解码点（ARCH-02）；失败 → 表单显示空串（用户可重填），但**必须留痕**：
// manual 任务的空 trigger_config 是正常态（不刷警告），schedule 任务的坏配置
// 若无声吞掉，UI 上就是「任务存在、永不触发、没有任何提示」
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

// 简易 trim（空格 / \t \r \n）
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
    // 兜底：正常路径 onStop 已清空 runs_，此处遍历为空集 no-op。
    // 只收线程，不注销工具（注册表生命周期独立于本模块；unregisterTool
    // 按名幂等，但工具对象可能正被 run 线程的 shared_ptr keep-alive 持有，
    // 析构期不碰它）
    for (auto& r : runs_) {
        r->cancel = true;
    }
    for (auto& r : runs_) {
        if (r->thread.joinable()) r->thread.join();
    }
}

void AgentModule::onStart() {
    // 重入幂等（活门控 Task 4）：已启动的 onStart = no-op。双 onStart 会重建
    // store_/llm_/loop_/statsTool_ 并重跑 sweepStaleRuns——statsTool_ 重复注册
    // 撞同名旧指针 fail 被日志吞，清扫还会误伤重入期间新出现的 running run
    if (started_) return;
    started_ = true;
    // deps 优先，缺省回退既有装配渠道（照 todo 的 appDatabase 模式）
    core::data::SqliteDb* db = deps_.db ? deps_.db : ::core::appDatabase();
    if (db == nullptr) {
        LOG_WARN("Agent: 数据库尚未注册，跳过服务装配");
        return;
    }
    store_ = std::make_unique<AgentStore>(*db);

    // 上次会话悬挂的 running run 清成 failed（应用异常退出恢复口径，spec §5.2）
    auto swept = store_->sweepStaleRuns();
    if (swept.isFail()) {
        LOG_WARN("Agent: 悬挂 run 清扫失败: " + swept.errorMessage());
    }

    // LLM 通道装配（惰性单入口的预热调用，Task 6 Step 1b）：有服务 → 建
    // llm_/loop_/scheduler_；无服务 → 只 WARN，store/工具照常装配，手动启动禁用。
    // 同一个入口在首次使用时再被调用，故「启动时 claude 关、会话内拨开」无须
    // off→on 本模块（claude-lazy-assembly 终审 finding，2026-10-01）
    ensureLlm();

    // 注册自带工具（name 冲突属装配 bug，无错误回传通道，日志留痕不中断）
    statsTool_ = makeAppStatsTool(*db);
    auto reg = ::core::ToolRegistry::instance().registerTool(statsTool_);
    if (reg.isFail()) {
        LOG_ERROR("Agent: app.stats 注册失败: " + reg.errorMessage());
    }

    tasksDirty_ = true;
    LOG_INFO("Agent 模块启动");
}

void AgentModule::onStop() {
    // 重入幂等（活门控 Task 4）：未启动的 off = no-op（设置页对未启用模块拨
    // off 时 dispatchModuleToggle 仍会调 onStop）。teardown 顺序契约不动：
    // 1) 全部置取消 → 2) 全部 join → 3) 清容器 → 4) 注销工具 → 5) 放服务。
    // run 是同步循环，cancel 在下轮迭代检查点前退出（agent_loop.cpp while 顶）
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        for (auto& r : runs_) {
            r->cancel = true;
        }
    }
    // join 不持锁：run 线程不碰 runsMu_，持锁无死锁风险但会白堵并发访问者
    for (auto& r : runs_) {
        if (r->thread.joinable()) r->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(runsMu_);
        runs_.clear();
    }
    // Scheduler 收口（spec §3 步骤 3「停常驻 worker 线程」）：onStart 早退时
    // scheduler_ 从未创建，必须判空；位置在「cancel 全部 + join 全部 + 清容器」
    // 之后、unregisterTool 之前——工具注销必须晚于所有 run 线程退出（否则 run
    // 线程里的工具执行摸到已注销的注册表）。stopAll 内部顺序：先停 tick/常驻
    // 派发线程，再 cancel+join 在跑 run（先灭派发源，收尾窗口不再起新 run）。
    if (scheduler_) scheduler_->stopAll();
    ::core::ToolRegistry::instance().unregisterTool("app.stats");
    // 释放序：loop_/llm_/store_ 先走也安全——stopAll 已 join 其全部线程，析构期
    // 的兜底 stopAll（幂等）只 join + 摘表，不触 loop_/store_（brief Task 6）
    loop_.reset();
    llm_.reset();
    store_.reset();
    scheduler_.reset();
    statsTool_.reset();
    llmMissingWarned_ = false;
    tasks_.clear();
    latestRuns_.clear();
    detailMessages_.clear();
    expandedTaskId_.reset();
    uiPolling_ = false;
    started_ = false;
    LOG_INFO("Agent 模块停止");
}

void AgentModule::onPageEnter() {
    tasksDirty_ = true;   // 进页重查（照 Home 仪表盘进页刷新模式）
}

// 收割已结束的 run（线程收尾但未 join 的）：join 后摘除。保证「同 task 不
// 并发」判定（spec §10）不被已完成的 run 误伤。仅主线程调用
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

// 惰性装配单入口（Task 6 Step 1b / 2026-10-01 claude-lazy-assembly 终审 finding）：
// 代理服务从「onStart 一次性解析」改为「首次使用时解析」——启动 claude 关、
// 会话内拨开 claude 后，已在运行的 agent 模块无须 off→on 即可用。
// 幂等且单例：loop_ 非空即返回，故一个模块实例最多一个 LlmClient / AgentLoop /
// Scheduler（不会起第二个 tick 线程）。scheduler_ 必须在本入口内与 loop_ 同建：
// 早退路径下 onStart 从未创建它，不补建则惰性路径下 cron 永不触发。
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
        // 无服务：WARN 保留并补可操作提示；每段「无服务」只提示一次——本函数会
        // 被页面帧调用，不节流会刷屏
        if (!llmMissingWarned_) {
            llmMissingWarned_ = true;
            LOG_WARN("Agent: 代理服务未装配，LLM 通道不可用"
                     "（请在设置页开启 Claude 模块后重试）");
        }
        return;
    }
    llm_ = std::make_unique<LlmClient>(*proxy, deps_.http);
    loop_ = std::make_unique<AgentLoop>(*llm_, ::core::ToolRegistry::instance(),
                                        *store_);
    if (scheduler_ == nullptr) {
        scheduler_ = std::make_unique<Scheduler>(*store_, *loop_, deps_.schedulerNow);
        scheduler_->start();
    }
    llmMissingWarned_ = false;
    LOG_INFO("Agent: LLM 通道就绪（惰性解析）");
}

void AgentModule::startTask(int64_t taskId) {
    ensureLlm();   // 首次使用路径：会话内拨开的代理服务在这里被解析
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
                // 同 task 不并发（spec §10）：已在跑 → 忽略
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
            // AgentLoop / store 的生命周期由 onStop 的 join 先于 reset 保证；
            // Result 落库在 run 内完成（agent_loop.cpp 收尾段）
            // store 行 → AgentTask 的唯一映射（taskFromRow）：run 线程里不留
            // 第二份会漂移的手抄副本（scheduler / tests 同用它）
            const AgentTask t = taskFromRow(task);
            // Result 落库在 run 内完成（agent_loop.cpp 收尾段）；run 的
            // 返回值 = 起库失败（无处落状态）这类不可恢复错误——不能静默
            auto started = loop_->run(t, raw->cancel);
            if (started.isFail()) {
                LOG_ERROR("Agent: run 启动失败（task " +
                          std::to_string(task.id) + "）: " +
                          started.errorMessage());
            }
            raw->done.store(true);
        });
    } catch (const std::system_error& e) {
        // 线程创建失败（资源耗尽）：摘除占位（线程未启动，无需 join），
        // 状态列不打「运行中」
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
    uiPolling_ = true;    // 状态列每帧跟进，直到全部 run 终止（refreshData 判停）
    tasksDirty_ = true;
}

void AgentModule::createTaskFromForm() {
    if (store_ == nullptr) {
        formError_ = "数据库未就绪";
        return;
    }
    const std::string goal = trimCopy(goalInput_);
    if (goal.empty()) {
        formError_ = "请先填写任务目标";
        return;
    }
    // M1 固定项：触发 manual、槽位 default、迭代上限 10（brief 指定）
    auto created =
        store_->createTask(deriveTaskName(goal), goal, "manual", "default", 10, false);
    if (created.isFail()) {
        formError_ = "创建失败：" + created.errorMessage();
        LOG_ERROR("Agent: 创建任务失败: " + created.errorMessage());
        return;
    }
    goalInput_.clear();
    formError_.clear();
    tasksDirty_ = true;
}

void AgentModule::setEditCronDraft(const std::string& cron) {
    editCron_ = cron;
}

// 打开编辑表单：字段以库为准回填（tasks_ 缓存可能滞后于库，编辑入口直接
// getTask——与详情展示同源同一份 AgentTaskRow）
void AgentModule::openTaskEditor(int64_t taskId) {
    if (store_ == nullptr) return;
    auto row = store_->getTask(taskId);
    if (row.isFail()) {
        formError_ = "读取任务失败：" + row.errorMessage();
        LOG_WARN("Agent: 打开编辑表单失败: " + row.errorMessage());
        return;
    }
    const AgentTaskRow& r = row.value();
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

// 保存编辑表单（spec §14 UI 行「cron 输入校验」）：trigger==定时 时先经 parseCron
// 校验，非法 → agent.edit.error 文案 + **不落库**（表单留在原地让用户改）；合法 →
// updateTask 七列覆盖 + 退编辑态 + 置脏重查。enabled 不在此路径（归 toggle）
void AgentModule::saveTaskEdit() {
    if (store_ == nullptr || !editingTaskId_) return;
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
        // ARCH-02（M2 静态审计）：编码唯一落点 = trigger_config.h。此前在这里
        // 手拼 `{"cron":"<expr>"}`，靠「校验通过的表达式只含 [0-9 * , - / 空格]」
        // 这条推理免转义——表达式字符集一变、或 trigger_config 加第二个键，
        // 手拼点就得逐个想起来
        auto encoded = encodeTriggerConfig(TriggerConfig{cron});
        if (encoded.isFail()) {
            editError_ = "保存失败：" + encoded.errorMessage();
            LOG_ERROR("Agent: 触发配置编码失败: " + encoded.errorMessage());
            return;
        }
        triggerConfig = encoded.value();
    }
    const int iter = editIter_ < 1 ? 1 : (editIter_ > 50 ? 50 : editIter_);
    auto updated = store_->updateTask(*editingTaskId_, deriveTaskName(goal), goal,
                                      triggerType, triggerConfig, editSlot_, iter,
                                      editDestructive_);
    if (updated.isFail()) {
        editError_ = "保存失败：" + updated.errorMessage();
        LOG_ERROR("Agent: 保存任务失败: " + updated.errorMessage());
        return;
    }
    editingTaskId_.reset();
    editError_.clear();
    tasksDirty_ = true;
}

// 启用/禁用取反（与编辑表单是两个独立动作：本路径不碰其它七列）
void AgentModule::toggleTaskEnabled(const AgentTaskRow& task) {
    if (store_ == nullptr) return;
    auto r = store_->setTaskEnabled(task.id, !task.enabled);
    if (r.isFail()) {
        LOG_WARN("Agent: 启停切换失败（task " + std::to_string(task.id) +
                 "）: " + r.errorMessage());
        return;
    }
    tasksDirty_ = true;
}

void AgentModule::toggleExpand(int64_t taskId) {
    if (expandedTaskId_ && *expandedTaskId_ == taskId) {
        expandedTaskId_.reset();
    } else {
        expandedTaskId_ = taskId;
    }
    selectedRunId_.reset();   // 展开语义：默认选中最近一次 run（runlist[0]）
    detailMessages_.clear();
    // 消息展开态按 seq 定位且只在 run 内有意义：切任务即清（切 run 的清在
    // runlist 选中回调里）
    expandedMsgSeqs_.clear();
    ++msgScrollVersion_;
    tasksDirty_ = true;   // 触发一次刷新装载 run 列表与详情消息
}

// 单条消息展开/收起（run 日志改造 2026-10-02）
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

// 重查任务 / 各任务最近 run / 展开详情的消息。触发条件（compose 内判定）：
// tasksDirty_（onStart / onPageEnter / 写操作 / 展开切换）或 uiPolling_
//（有 run 期间每帧跟进状态与消息）。
// 判停口径：run 线程全 done 且库内无 running 态 → uiPolling_ = false。
// done 先查再查库：observe done==true ⟹ run 线程的 finishRun 先行发生
//（atomic release/acquire），随后这次库读必然读到终态，不会漏掉最后一帧
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
        // 最近一次在前（契约）：store 侧已按 (started_at DESC, id DESC) 并列兜底，
        // 这里再按 (startedAt, id) 逆序排一次是纵深防御——runlist[0] 与状态列的
        // 「最近一次」语义不依赖调用方对 store 排序契约的记忆
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
    if (expandedTaskId_) {
        detailMessages_.clear();
        auto it = taskRuns_.find(*expandedTaskId_);
        if (it != taskRuns_.end() && !it->second.empty()) {
            // 选中 run：显式选中（点过 runlist 行）优先；不在本任务列表里
            //（换了任务 / 被 5 条上限挤出）→ 回落最近一次
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
    for (const auto& [id, run] : latestRuns_) {
        if (run.status == "running") {
            dbRunning = true;
            break;
        }
    }
    if (!anyAlive && !dbRunning) {
        uiPolling_ = false;
    }
}

// 单条消息行渲染（run 日志改造 2026-10-02）。结构：
//   msg.<i>（列，定高 = 估算高，宁高勿低）
//     .head    —— 角色徽章 + 展开/收起 chip +（展开时）复制 chip
//     收起：.preview（解码文本前 120 字节单行，点击也可展开）
//     展开：逐块 .blk.<j>
//       text/plain   → wrap 多行全文（maxWidth 必须显式给：measureTextSize
//                      只认 element.maxWidth，.size() 不带它）
//       tool_use     → 「🔧 名称 + 参数摘要」单行 chip 文本
//       tool_result  → 「工具输出」标签行 + 输出全文 wrap 多行（错误标红）
void AgentModule::renderMessageRow(
    ::eui::Ui& inner, float cw, const std::string& taskId, size_t index,
    const AgentMessageRow& msg, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& tokens) {
    (void)t;
    const std::string mid = taskId + ".msg." + std::to_string(index);
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
                                       // setClipboardText 返回 void（模块层无
                                       // 窗口句柄无法回读校验）→ LOG 留痕，
                                       // claude_module doCopyConfig 同款口径
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
                    // 点消息行也能展开（需求 B：click message 或专用 chip）
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

    // 首次使用路径之二：页面帧。会话内拨开 claude 后回到本页（onPageEnter 置
    // tasksDirty_）即解析服务——否则 loop_ 恒空、「启动」按钮永久禁用，惰性解析
    // 在真实 UI 上不可达。已就绪时只做一次空指针判断，无每帧开销
    if (store_ != nullptr && loop_ == nullptr && tasksDirty_) {
        ensureLlm();
    }

    // 数据刷新：脏标记或有 run 在身（每帧跟进状态/消息）；查询在主线程，
    // 量级 = 任务数 + 展开任务的消息数，M1 可接受
    if (store_ != nullptr && (tasksDirty_ || uiPolling_)) {
        refreshData();
    }

    const bool hasError = !formError_.empty();
    ensureToolsCache(/*forceReload=*/false);
    const float toolsListH =
        toolsOpen_ ? std::min(static_cast<float>(toolsCache_.size()) *
                                      kToolCardH,
                                  kToolsListMaxH)
                   : 0.0f;
    const float toolsH =
        kToolsHeaderH + (toolsOpen_ ? kMsgGap + toolsListH : 0.0f);
    // 页 column 子项：title / form / [error] / tools / [编辑表单] / tasklist，
    // gap = spacingLarge；子项数一变，gapCount 与减项必须同步（否则 tasklist
    // 高度算错 → 末尾行溢出页底）。form 行高 = kGoalInputH（多行目标输入）
    const int gapCount = (hasError ? 1 : 0) + 3 + (editingTaskId_ ? 1 : 0);
    const float listH = screen.height - pad * 2.0f - 32.0f - kGoalInputH -
                        toolsH -
                        (hasError ? 16.0f : 0.0f) -
                        (editingTaskId_ ? kEditFormH : 0.0f) -
                        static_cast<float>(gapCount) * t.sizes.spacingLarge;

    ui.column("agent.page")
        .size(screen.width, screen.height)
        .padding(pad)
        .gap(t.sizes.spacingLarge)
        .content([&] {
            // ---- 标题 ----
            ui.text("agent.title")
                .size(contentW, 32.0f)
                .text("智能体")
                .fontSize(t.sizes.fontSizeTitle)
                .lineHeight(32.0f)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .build();

            // ---- 新建表单：任务目标标签 + 多行输入 + 提交（2026-10-02 用户
            // 反馈③：单行小框改多行 4 行视口 + 引导 placeholder；M1 固定项
            // 槽位 default / 迭代上限 10 见 createTaskFromForm）----
            ui.row("agent.form")
                .size(contentW, kGoalInputH)
                .gap(12.0f)
                .content([&] {
                    // 标签顶对齐（多行输入框的标签贴顶，垂直居中会悬在半空）
                    ui.text("agent.form.goal.label")
                        .size(72.0f, 20.0f)
                        .text("任务目标")
                        .fontSize(12.0f)
                        .lineHeight(20.0f)
                        .verticalAlign(eui::VerticalAlign::Top)
                        .color(t.colors.textSecondary)
                        .build();
                    components::input(ui, "agent.form.goal")
                        .size(contentW - 72.0f - 12.0f - 120.0f - 12.0f,
                              kGoalInputH)
                        .value(goalInput_)
                        .placeholder("描述这个任务要做什么，例如：把逾期的待办全部完成")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(true)
                        .scrollbar(true)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { goalInput_ = v; })
                        .build();
                    components::button(ui, "agent.form.create")
                        .size(120.0f, 40.0f)
                        .text("新建任务")
                        .theme(themeTokens)
                        .disabled(store_ == nullptr)
                        .onClick([this] { createTaskFromForm(); })
                        .build();
                })
                .build();
            if (hasError) {
                ui.text("agent.form.error")
                    .size(contentW, 16.0f)
                    .text(formError_)
                    .fontSize(12.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.error)
                    .build();
            }

            // ---- 可用工具区块（2026-10-02 用户反馈④）：收起默认，头部点击
            // 展开。列 ToolRegistry 全部已注册工具（name + 描述 + 参数名），
            // 卡多了内滚（kToolsListMaxH）----
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
                                            // 工具名（左上）+ 参数名（同行右侧）
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

            // ---- 编辑表单（Task 10）：editingTaskId_ 非空才渲染 ----
            // 触发/槽位用 components::dropdown，包一层同尺寸 stack（settings 页
            // 同款）：dropdown 根 stack 含弹层高度，直接当行子项会把行撑高
            if (editingTaskId_) {
                const float innerW = contentW - 24.0f;
                const float labelW = 72.0f;
                const float ddW = 140.0f;
                const float rowGap = 12.0f;
                const bool isSchedule = editTriggerIdx_ == kTriggerScheduleIdx;
                ui.column("agent.edit.form")
                    .size(contentW, kEditFormH)
                    .padding(12.0f)
                    .gap(8.0f)
                    .content([&] {
                        ui.text("agent.edit.form.title")
                            .size(innerW, 20.0f)
                            .text("编辑任务")
                            .fontSize(13.0f)
                            .lineHeight(20.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        // 目标（2026-10-02 用户反馈③：多行 4 行视口 + 引导文案）
                        ui.row("agent.edit.goal.row")
                            .size(innerW, kGoalInputH)
                            .gap(rowGap)
                            .content([&] {
                                ui.text("agent.edit.goal.caption")
                                    .size(labelW, 20.0f)
                                    .text("任务目标")
                                    .fontSize(12.0f)
                                    .lineHeight(20.0f)
                                    .verticalAlign(eui::VerticalAlign::Top)
                                    .color(t.colors.textSecondary)
                                    .build();
                                components::input(ui, "agent.edit.goal")
                                    .size(innerW - labelW - rowGap,
                                          kGoalInputH)
                                    .value(editGoal_)
                                    .placeholder("描述这个任务要做什么…")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .multiline(true)
                                    .scrollbar(true)
                                    .theme(themeTokens)
                                    .onChange([this](const std::string& v) {
                                        editGoal_ = v;
                                    })
                                    .build();
                            })
                            .build();
                        // 触发方式 [+ cron（定时才渲染）]
                        ui.row("agent.edit.trigger.row")
                            .size(innerW, 40.0f)
                            .gap(rowGap)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                fieldCaption(ui, "agent.edit.trigger.caption", "触发方式", labelW,
                                             40.0f, t.colors.textSecondary);
                                ui.stack("agent.edit.trigger.wrap")
                                    .size(ddW, 40.0f)
                                    .content([&] {
                                        components::dropdown(ui, "agent.edit.trigger")
                                            .size(ddW, 40.0f)
                                            .theme(themeTokens)
                                            .items(std::vector<std::string>(
                                                kTriggerLabels,
                                                kTriggerLabels + kTriggerCount))
                                            .selected(editTriggerIdx_)
                                            .open(editTriggerOpen_)
                                            .onOpenChange([this](bool v) {
                                                editTriggerOpen_ = v;
                                            })
                                            .onChange([this](int idx) {
                                                editTriggerIdx_ = idx;
                                            })
                                            .build();
                                    })
                                    .build();
                                if (isSchedule) {
                                    fieldCaption(ui, "agent.edit.cron.caption", "cron 表达式", 72.0f,
                                                 40.0f, t.colors.textSecondary);
                                    components::input(ui, "agent.edit.cron")
                                        .size(innerW - labelW - ddW - 72.0f -
                                                  rowGap * 3.0f,
                                              40.0f)
                                        .value(editCron_)
                                        .placeholder("分 时 日 月 周，例如 0 9 * * 1-5")
                                        .fontSize(t.sizes.fontSizeBody)
                                        .theme(themeTokens)
                                        .onChange([this](const std::string& v) {
                                            editCron_ = v;
                                        })
                                        .build();
                                }
                            })
                            .build();
                        // 模型槽位 + 迭代上限 + 破坏性授权
                        ui.row("agent.edit.meta.row")
                            .size(innerW, 40.0f)
                            .gap(rowGap)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                fieldCaption(ui, "agent.edit.slot.caption", "模型槽位", labelW,
                                             40.0f, t.colors.textSecondary);
                                ui.stack("agent.edit.slot.wrap")
                                    .size(ddW, 40.0f)
                                    .content([&] {
                                        components::dropdown(ui, "agent.edit.slot")
                                            .size(ddW, 40.0f)
                                            .theme(themeTokens)
                                            .items(std::vector<std::string>(
                                                kModelSlotLabels,
                                                kModelSlotLabels + kModelSlotCount))
                                            .selected(slotIndexOf(editSlot_))
                                            .open(editSlotOpen_)
                                            .onOpenChange([this](bool v) {
                                                editSlotOpen_ = v;
                                            })
                                            .onChange([this](int idx) {
                                                if (idx >= 0 && idx < kModelSlotCount) {
                                                    editSlot_ = kModelSlotKeys[idx];
                                                }
                                            })
                                            .build();
                                    })
                                    .build();
                                fieldCaption(ui, "agent.edit.iter.caption", "迭代上限", 72.0f,
                                             40.0f, t.colors.textSecondary);
                                components::input(ui, "agent.edit.iter")
                                    .size(100.0f, 40.0f)
                                    .value(std::to_string(editIter_))
                                    .fontSize(t.sizes.fontSizeBody)
                                    .theme(themeTokens)
                                    .onChange([this](const std::string& v) {
                                        editIter_ = parseIterInput(v);
                                    })
                                    .build();
                                // 破坏性授权开关：用 switch（settings 页同款，
                                // 已在 DevDesk 编译面内）——components::checkbox
                                // 头在 -Wall -Wextra -Werror 下有未使用变量告警
                                //（EUI-NEO 侧既有问题，不属本次改动范围）
                                components::toggleSwitch(ui, "agent.edit.destructive")
                                    .size(innerW - labelW - ddW - 72.0f - 100.0f -
                                              rowGap * 4.0f,
                                          32.0f)
                                    .text("允许破坏性操作")
                                    .checked(editDestructive_)
                                    .fontSize(12.0f)
                                    .theme(themeTokens)
                                    .onChange([this](bool v) {
                                        editDestructive_ = v;
                                    })
                                    .build();
                            })
                            .build();
                        // 保存 / 取消 / 错误文案
                        ui.row("agent.edit.actions")
                            .size(innerW, 32.0f)
                            .gap(rowGap)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                actionChip(ui, "agent.edit.save", "保存", 64.0f,
                                           /*primary=*/true, /*enabled=*/true,
                                           themeTokens,
                                           [this] { saveTaskEdit(); });
                                actionChip(ui, "agent.edit.cancel", "取消", 64.0f,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this] {
                                               editingTaskId_.reset();
                                               editError_.clear();
                                           });
                                if (!editError_.empty()) {
                                    ui.text("agent.edit.error")
                                        .size(innerW - 64.0f * 2.0f - rowGap * 3.0f,
                                              32.0f)
                                        .text(editError_)
                                        .fontSize(12.0f)
                                        .lineHeight(32.0f)
                                        .wrap(false)
                                        .color(t.colors.error)
                                        .build();
                                }
                            })
                            .build();
                    })
                    .build();
            }

            // ---- 任务列表 ----
            ui.column("agent.tasklist")
                .size(contentW, std::max(0.0f, listH))
                .gap(8.0f)
                .content([&] {
                    if (store_ == nullptr) {
                        ui.text("agent.error")
                            .size(contentW, 20.0f)
                            .text("数据库未就绪（Agent 模块服务未装配）")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(20.0f)
                            .color(t.colors.error)
                            .build();
                        return;
                    }
                    if (tasks_.empty()) {
                        ui.text("agent.empty")
                            .size(contentW, 20.0f)
                            .text("暂无任务——先在上方填写目标并点「新建任务」")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(20.0f)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    for (const auto& task : tasks_) {
                        const std::string id =
                            "agent.task." + std::to_string(task.id);
                        const bool expanded =
                            expandedTaskId_ &&
                            *expandedTaskId_ == task.id;
                        std::string status = "—";
                        auto it = latestRuns_.find(task.id);
                        if (it != latestRuns_.end()) {
                            status = runStatusText(it->second.status);
                        }
                        // 行内控件宽度分配：状态 80 / 编辑 48 / 启停 48 /
                        // [入队 56（仅常驻）] / 启动 72，间距 12；标题吃剩余宽度。
                        // childCount 必须与实际子元素个数一致（间距数 = 子元素数-1）
                        const bool resident = task.triggerType == "resident";
                        const float rowGap = 12.0f;
                        const int childCount = 5 + (resident ? 1 : 0);
                        const float fixedW = 80.0f + 48.0f + 48.0f + 72.0f +
                                             (resident ? 56.0f : 0.0f);
                        const float titleW = contentW - fixedW -
                                             rowGap * static_cast<float>(childCount - 1);
                        // 任务行：点击展开/收起详情；行内控件各自独立消费点击
                        //（子元素先于父行命中——runtime 逆向命中序）
                        ui.row(id)
                            .size(contentW, 44.0f)
                            .alignItems(eui::Align::CENTER)
                            .gap(rowGap)
                            .interactive(true)
                            .onClick([this, taskId = task.id] {
                                toggleExpand(taskId);
                            })
                            .content([&] {
                                ui.text(id + ".title")
                                    .size(titleW, 20.0f)
                                    .text(task.name)
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text(id + ".status")
                                    .size(80.0f, 20.0f)
                                    .text(status)
                                    .fontSize(12.0f)
                                    .lineHeight(20.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                                // 编辑 / 启停 / 入队：契约要求 id 元素本身带 text
                                // 与 onClick（text 元素挂 onClick 即 interactive）
                                actionChip(ui, id + ".edit", "编辑", 48.0f,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this, taskId = task.id] {
                                               openTaskEditor(taskId);
                                           });
                                actionChip(ui, id + ".toggle",
                                           task.enabled ? "禁用" : "启用", 48.0f,
                                           /*primary=*/false, /*enabled=*/true,
                                           themeTokens, [this, taskCopy = task] {
                                               toggleTaskEnabled(taskCopy);
                                           });
                                if (resident) {
                                    // 常驻入队（spec §5.1：常驻本阶段只跑手动
                                    // 入队子任务）；Scheduler 未装配时置灰不可点
                                    actionChip(ui, id + ".enqueue", "入队", 56.0f,
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
                                components::button(ui, id + ".run")
                                    .size(72.0f, 32.0f)
                                    .text("启动")
                                    .theme(themeTokens)
                                    .disabled(loop_ == nullptr)  // 无 LLM 通道禁用
                                    .onClick([this, taskId = task.id] {
                                        startTask(taskId);
                                    })
                                    .build();
                            })
                            .build();
                        // run 详情：展开时才存在（契约表）；消息行 = role+content
                        // 前 80 字节（最近一次 run，seq 升序）
                        if (expanded) {
                            ui.column(id + ".detail")
                                .width(contentW)
                                .height(::eui::SizeValue::wrapContent())
                                .padding(12.0f)
                                .gap(6.0f)
                                .content([&] {
                                    // 只读任务信息行（Step 3b）：goal / cron 原文
                                    //（定时任务）/ 模型槽位 / 迭代上限 / 破坏性标记。
                                    // 字段来源 = 本循环迭代的同一份 AgentTaskRow
                                    //（与编辑表单回填同源，不另起第二套读法）
                                    std::string info = "目标：" + task.goal;
                                    info += " · 触发：" + task.triggerType;
                                    if (task.triggerType == "schedule") {
                                        const std::string cron = cronTextOf(task);
                                        info += " · cron：" +
                                                (cron.empty() ? "（未设置）" : cron);
                                    }
                                    info += " · 槽位：" + task.modelSlot;
                                    info += " · 迭代上限：" +
                                            std::to_string(task.maxIterations);
                                    info += task.allowDestructive
                                                ? " · 破坏性：允许"
                                                : " · 破坏性：禁止";
                                    ui.text(id + ".info")
                                        .size(contentW - 24.0f, 18.0f)
                                        .text(info)
                                        .fontSize(12.0f)
                                        .lineHeight(18.0f)
                                        .wrap(false)
                                        .color(t.colors.textSecondary)
                                        .build();
                                    // run 列表（Task 11 契约）：最近 5 次，空
                                    // run 时不渲染；点行 → 选中该 run 载其消息
                                    const auto runsIt = taskRuns_.find(task.id);
                                    if (runsIt != taskRuns_.end() &&
                                        !runsIt->second.empty()) {
                                        const auto& runs = runsIt->second;
                                        const float runW = contentW - 24.0f;
                                        ui.column(id + ".runlist")
                                            .width(runW)
                                            .height(::eui::SizeValue::wrapContent())
                                            .gap(4.0f)
                                            .content([&] {
                                                for (size_t i = 0; i < runs.size();
                                                     ++i) {
                                                    const std::string runRowId =
                                                        id + ".runlist." +
                                                        std::to_string(i);
                                                    const bool selected =
                                                        selectedRunId_ &&
                                                        *selectedRunId_ == runs[i].id;
                                                    ui.text(runRowId)
                                                        .size(runW, 18.0f)
                                                        .text(runRowText(runs[i]))
                                                        .fontSize(12.0f)
                                                        .lineHeight(18.0f)
                                                        .wrap(false)
                                                        .color(selected
                                                                   ? t.colors.primary
                                                                   : t.colors.textSecondary)
                                                        .onClick([this, runId = runs[i].id] {
                                                            selectedRunId_ = runId;
                                                            // seq 只在 run 内唯一：切 run 清消息展开态
                                                            expandedMsgSeqs_.clear();
                                                            ++msgScrollVersion_;
                                                            tasksDirty_ = true;
                                                        })
                                                        .build();
                                                    if (runs[i].error &&
                                                        !runs[i].error->empty()) {
                                                        ui.text(runRowId + ".err")
                                                            .size(runW, 16.0f)
                                                            .text("错误：" +
                                                                  *runs[i].error)
                                                            .fontSize(11.0f)
                                                            .lineHeight(16.0f)
                                                            .wrap(false)
                                                            .color(t.colors.error)
                                                            .build();
                                                    }
                                                }
                                            })
                                            .build();
                                    }
                                    if (detailMessages_.empty()) {
                                        ui.text(id + ".detail.empty")
                                            .size(contentW - 24.0f, 18.0f)
                                            .text("（选中的 run 还没有消息）")
                                            .fontSize(12.0f)
                                            .lineHeight(18.0f)
                                            .color(t.colors.textTertiary)
                                            .build();
                                    } else {
                                    // 消息区（2026-10-02 改造）：块级渲染 +
                                    // 单条展开全文 + 工具调用展示。外高按
                                    // 估算钳在 [kMsgAreaMinH, kMsgAreaMaxH]，
                                    // 真实内容超高由 scrollView 内滚。
                                    // contentKey 带版本号：展开/收起改变内容
                                    // 高，bump 让 MeasureCache 重测
                                    const float msgW = contentW - 24.0f;
                                    const float msgAreaH =
                                        estimateMessagesAreaHeight(
                                            detailMessages_, expandedMsgSeqs_,
                                            msgW, 18.0f, kMsgAreaMinH,
                                            kMsgAreaMaxH);
                                    components::scrollView(
                                        ui, id + ".msgscroll")
                                        .size(msgW, msgAreaH)
                                        .gap(6.0f)
                                        .step(18.0f)
                                        .theme(themeTokens)
                                        .contentKey(
                                            id + ".msgscroll.v" +
                                            std::to_string(msgScrollVersion_) +
                                            ".n" +
                                            std::to_string(
                                                detailMessages_.size()))
                                        .content([&](eui::Ui& inner, float cw,
                                                     float) {
                                            for (size_t i = 0;
                                                 i < detailMessages_.size();
                                                 ++i) {
                                                renderMessageRow(
                                                    inner, cw, id, i,
                                                    detailMessages_[i], t,
                                                    themeTokens);
                                            }
                                        })
                                        .build();
                                    }
                                })
                                .build();
                        }
                    }
                })
                .build();
        })
        .build();
}

std::string AgentModule::iconGlyph() const {
    return ::eui::utf8(0xF544);  // fa-robot
}

} // namespace module::agent

extern "C" ::core::Module* create_agent_module() {
    // 生产装配：Deps 全空，onStart 内回退 appDatabase / ServiceLocator 解析
    return new module::agent::AgentModule(module::agent::AgentModule::Deps{});
}
