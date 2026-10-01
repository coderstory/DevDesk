// src/module_agent/agent_module.cpp
// 智能体页模块实现（agent M1 Task 12）。Task 11 装配面（图标码点 / 工厂
// 签名）保持不变；本文件填实生命周期与最小 UI。
//
// 图标码点 U+F544（fa-robot）：已核对存在于 EUI-NEO 内置
// assets/Font Awesome 7 Free-Solid-900.otf 的 cmap（Task 11 离线解析）；
// eui::utf8(0xF544) 的 UTF-8 字节为 \xEF\x95\x84，与 app_registry.h
// 图标列同值（assembly_registry_matches_module_getters 锁一致性）。
//
// 元素 id 契约表（plan task-12-brief，Task 13 UI 测试逐字钉）：
//   agent.page / agent.title / agent.form.goal / agent.form.create /
//   agent.tasklist / agent.task.<taskId> / agent.task.<taskId>.title /
//   agent.task.<taskId>.run（text=="启动"）/ agent.task.<taskId>.status /
//   agent.task.<taskId>.detail（展开才存在）/ agent.task.<taskId>.msg.<i>
#include "agent_module.h"

#include <algorithm>

#include <components/button.h>
#include <components/input.h>
#include <eui/dsl.h>

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "module_agent/agent_loop.h"
#include "module_agent/app_stats_tool.h"

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

// 消息行预览：role + ": " + content 前 80 字节（超长在 UTF-8 边界回退，
// 不切半个字符，末尾加省略号）。content 用 contentJson 原文（M1 最小实现，
// 不做 blocks 提取）
std::string messagePreview(const AgentMessageRow& m) {
    constexpr size_t kMaxPreviewBytes = 80;
    std::string content = m.contentJson;
    if (content.size() > kMaxPreviewBytes) {
        size_t cut = kMaxPreviewBytes;
        while (cut > 0 &&
               (static_cast<unsigned char>(content[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        content = content.substr(0, cut) + "…";
    }
    return m.role + ": " + content;
}

// 任务名派生：M1 表单只有 goal 一个输入，任务名取 goal 前 24 字节
//（UTF-8 边界回退）。goal 非空（调用方保证）故派生结果恒非空
std::string deriveTaskName(const std::string& goal) {
    constexpr size_t kMaxNameBytes = 24;
    if (goal.size() <= kMaxNameBytes) return goal;
    size_t cut = kMaxNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(goal[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return goal.substr(0, cut) + "…";
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

    // proxy：deps 优先，缺省回退 core 服务定位器（settings 同款渠道）。
    // 无代理 = LLM 通道不可用：store/工具照常装配，手动启动不可用（UI 禁用）
    core::IClaudeProxyService* proxy = deps_.proxy;
    if (proxy == nullptr) {
        auto located =
            ::core::ServiceLocator::requireService<::core::IClaudeProxyService>();
        if (located.isOk()) {
            proxy = located.value();
        } else {
            LOG_WARN("Agent: 代理服务未装配，LLM 通道不可用（手动启动禁用）");
        }
    }
    if (proxy != nullptr) {
        llm_ = std::make_unique<LlmClient>(*proxy, deps_.http);
        loop_ = std::make_unique<AgentLoop>(*llm_, ::core::ToolRegistry::instance(),
                                            *store_);
    }

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
    // teardown 顺序是结构契约（spec §3），勿改：
    // 1) 全部置取消 → 2) 全部 join → 3) 清容器 → 4) 注销工具 → 5) 放服务。
    // run 是同步循环，cancel 在下轮迭代检查点前退出（agent_loop.cpp while 顶）
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
    ::core::ToolRegistry::instance().unregisterTool("app.stats");
    loop_.reset();
    llm_.reset();
    store_.reset();
    statsTool_.reset();
    tasks_.clear();
    latestRuns_.clear();
    detailMessages_.clear();
    expandedTaskId_.reset();
    uiPolling_ = false;
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

void AgentModule::startTask(int64_t taskId) {
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
            AgentTask t;
            t.id = task.id;
            t.name = task.name;
            t.goal = task.goal;
            t.modelSlot = task.modelSlot;
            t.maxIterations = task.maxIterations;
            t.allowDestructive = task.allowDestructive;
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

void AgentModule::toggleExpand(int64_t taskId) {
    if (expandedTaskId_ && *expandedTaskId_ == taskId) {
        expandedTaskId_.reset();
    } else {
        expandedTaskId_ = taskId;
    }
    detailMessages_.clear();
    tasksDirty_ = true;   // 触发一次刷新装载详情消息
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
    auto t = store_->listTasks();
    if (t.isFail()) {
        LOG_ERROR("Agent: 读取任务失败: " + t.errorMessage());
    } else {
        tasks_ = std::move(t).value();
    }
    for (const auto& task : tasks_) {
        auto runs = store_->listRuns(task.id, 1);
        if (runs.isOk() && !runs.value().empty()) {
            latestRuns_[task.id] = runs.value().front();
        }
    }
    if (expandedTaskId_) {
        detailMessages_.clear();
        auto it = latestRuns_.find(*expandedTaskId_);
        if (it != latestRuns_.end()) {
            auto msgs = store_->listMessages(it->second.id);
            if (msgs.isOk()) {
                detailMessages_ = std::move(msgs).value();
            } else {
                LOG_ERROR("Agent: 读取 run 消息失败: " + msgs.errorMessage());
            }
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

void AgentModule::compose(::eui::Ui& ui, const ::eui::Screen& screen) {
    const auto& t = ::core::theme::current();
    const auto themeTokens = ::core::theme::componentTokens(t);
    const float pad = t.sizes.spacingXXLarge;              // 40，照 claude 页
    const float contentW = screen.width - pad * 2.0f;

    // 数据刷新：脏标记或有 run 在身（每帧跟进状态/消息）；查询在主线程，
    // 量级 = 任务数 + 展开任务的消息数，M1 可接受
    if (store_ != nullptr && (tasksDirty_ || uiPolling_)) {
        refreshData();
    }

    const bool hasError = !formError_.empty();
    // 页 column 子项：title / form / [error] / tasklist，gap = spacingLarge
    const int gapCount = hasError ? 3 : 2;
    const float listH = screen.height - pad * 2.0f - 32.0f - 40.0f -
                        (hasError ? 16.0f : 0.0f) -
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

            // ---- 新建表单：goal 输入 + 提交（M1 单输入；槽位 default /
            // 迭代上限 10 固定默认，见 createTaskFromForm）----
            ui.row("agent.form")
                .size(contentW, 40.0f)
                .gap(12.0f)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    components::input(ui, "agent.form.goal")
                        .size(contentW - 132.0f, 40.0f)
                        .value(goalInput_)
                        .placeholder("输入任务目标，例如：把逾期的待办全部完成")
                        .fontSize(t.sizes.fontSizeBody)
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
                        // 任务行：点击展开/收起详情；行内「启动」按钮独立消费
                        ui.row(id)
                            .size(contentW, 44.0f)
                            .alignItems(eui::Align::CENTER)
                            .gap(12.0f)
                            .interactive(true)
                            .onClick([this, taskId = task.id] {
                                toggleExpand(taskId);
                            })
                            .content([&] {
                                ui.text(id + ".title")
                                    .size(contentW - 90.0f - 72.0f - 12.0f * 2,
                                          20.0f)
                                    .text(task.name)
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text(id + ".status")
                                    .size(90.0f, 20.0f)
                                    .text(status)
                                    .fontSize(12.0f)
                                    .lineHeight(20.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
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
                                    if (detailMessages_.empty()) {
                                        ui.text(id + ".detail.empty")
                                            .size(contentW - 24.0f, 18.0f)
                                            .text("（最近一次 run 还没有消息）")
                                            .fontSize(12.0f)
                                            .lineHeight(18.0f)
                                            .color(t.colors.textTertiary)
                                            .build();
                                    }
                                    for (size_t i = 0; i < detailMessages_.size();
                                         ++i) {
                                        ui.text(id + ".msg." + std::to_string(i))
                                            .size(contentW - 24.0f, 18.0f)
                                            .text(messagePreview(detailMessages_[i]))
                                            .fontSize(12.0f)
                                            .lineHeight(18.0f)
                                            .color(t.colors.textSecondary)
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
