// 任务执行页实现（票据 06 / 07 / 08 / 10）：任务级前置卡、环境与服务执行清单、
// 撤回、任务状态（作废 / 恢复）。
//
// 反重构警告：
//   - 工单卡适用性与工单号门槛必须与 release_logic 的 ticketNeeds 同口径
//     （未移除环境下的未移除服务、未跳过且内容非空）。改一边不改另一边会出现
//     「提醒有工单、执行页没卡」的反向错位。
//   - 五项配置以固定 kind 为准；库里缺行时只在用户交互（查看 / 勾选 / 排序）时补建
//     数据库行。渲染 / 校验的占位行由 orderedConfigs 按期望配置槽位在返回值里补齐
//     （不写库、不改 state.configs；进度与完成判定按期望槽位口径，缺行天然按未完成计）。
//     渲染帧禁止常规写库：每帧写库会让输入卡顿，并和滚动回调节流互相踩踏；唯一例外是
//     advanceTaskStatus 的一次性状态推进（仅当推导状态与库中状态不同才写）。
//   - 状态推进只能在数据加载完成（dirty=false）后执行，且 Draft 不得自动写 Pending
//     ——只有本页「提交」动作（validateTask 通过后）显式推进，提前勾前置不算执行中。

#include "release_detail_internal.h"

#include "release_config_ui.h"
#include "release_list_ui.h"    // releaseTaskStatusText

#include <core/component_theme.h>

#include <components/dialog.h>
#include <components/input.h>
#include <components/progress.h>
#include <components/scrollview.h>

#include <ui_kit/button.h>
#include <ui_kit/metrics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <limits>
#include <utility>

namespace module::release {
namespace detail {

void applyContentRevocations(ReleaseStore& store, ReleaseDetailUiState& state,
                             const std::vector<TaskService>& previousServices,
                             const std::vector<TaskConfig>& previousConfigs) {
    if (isReadOnly(state.displayStatus)) return;
    int revoked = 0;
    bool failed = false;

    for (const auto& prev : previousServices) {
        if (prev.removed) continue;
        const TaskService* current = findService(state, prev.id);
        if (current == nullptr || current->removed) continue;
        if (current->branch == prev.branch) continue;
        if (!stepDone(state, current->id, StepKind::MergeBranch)) continue;
        auto result = store.setStepDone(current->id, StepKind::MergeBranch, 0);
        if (result.isFail()) {
            setNotice(state, "自动撤回合并分支失败：" + result.errorMessage(), true);
            failed = true;
            continue;
        }
        for (auto& step : state.steps) {
            if (step.taskServiceId == current->id && step.kind == StepKind::MergeBranch) {
                step.doneAt = 0;
            }
        }
        if (!appendActionLog(store, state, current->serviceName,
                             stepLabel(StepKind::MergeBranch), "revoke",
                             "服务分支已修改")) {
            failed = true;
        }
        ++revoked;
    }

    for (const auto& prev : previousConfigs) {
        if (prev.skipped || prev.id == 0) continue;  // 占位行没有已确认状态可撤
        const TaskService* svc = findService(state, prev.taskServiceId);
        if (svc == nullptr || svc->removed) continue;
        for (auto& cfg : state.configs) {
            if (cfg.taskServiceId != prev.taskServiceId || cfg.kind != prev.kind) continue;
            if (cfg.skipped || cfg.content == prev.content || cfg.doneAt == 0) break;
            TaskConfig next = cfg;
            next.doneAt = 0;
            auto result = store.upsertTaskConfig(next);
            if (result.isFail()) {
                setNotice(state, "自动撤回配置步骤失败：" + result.errorMessage(), true);
                failed = true;
                break;
            }
            cfg.doneAt = 0;
            if (!appendActionLog(store, state, svc->serviceName, configLabel(cfg.kind),
                                 "revoke", "配置内容已修改")) {
                failed = true;
            }
            ++revoked;
            break;
        }
    }

    if (revoked > 0 && !failed) {
        setNotice(state, "配置已修改，已自动撤回 " + std::to_string(revoked) + " 项旧确认",
                  false);
    }
}

void loadDetail(ReleaseStore& store, ReleaseDetailUiState& state) {
    // 同一任务重读 = 其它层保存后的刷新：先留旧快照，加载完成后对比并自动撤回
    // （见 applyContentRevocations）。切换任务没有可比的旧任务，不做对比。
    const bool compareContentChanges =
        !state.loadedTaskId.empty() && state.loadedTaskId == state.taskId;
    std::vector<TaskService> previousServices;
    std::vector<TaskConfig> previousConfigs;
    if (compareContentChanges) {
        previousServices = state.services;
        previousConfigs = state.configs;
    }
    // 切任务时清掉上一个任务的反馈与未决确认，避免把旧任务的撤回目标套到新任务上；
    // 编辑层目标（editorServiceId）同理作废，防止对旧任务的服务写库。
    if (state.loadedTaskId != state.taskId) {
        state.notice.clear();
        state.noticeIsError = false;
        state.confirm = ReleaseDetailConfirm::None;
        state.validationActive = false;
        state.validationMissing.clear();
        state.editorOpen = false;
        state.editorText.clear();
        state.menuOpen = false;
        state.openMenuRowKey.clear();
    }
    state.loadedTaskId = state.taskId;
    state.dirty = false;
    state.loadError.clear();
    state.task = Task{};
    state.displayStatus = TaskStatus::Draft;
    state.statusWriteFailed = false;
    state.envs.clear();
    state.services.clear();
    state.configs.clear();
    state.steps.clear();
    state.logs.clear();
    state.drag = ReleaseDetailDragState{};
    // 轨道组折叠记忆随重读重算默认态（确认/撤回等状态变化都会触发重读）。
    state.trackGroupState.clear();

    std::int64_t taskId = 0;
    if (!parseTaskId(state.taskId, &taskId)) {
        state.loadError = "任务 id 无效";
        return;
    }

    auto tasks = store.listTasks();
    if (tasks.isFail()) {
        state.loadError = "读取任务失败：" + tasks.errorMessage();
        return;
    }
    const Task* found = nullptr;
    for (const auto& task : tasks.value()) {
        if (task.id == taskId) {
            found = &task;
            break;
        }
    }
    if (found == nullptr) {
        state.loadError = "任务不存在";
        return;
    }
    state.task = *found;
    state.displayStatus = state.task.status;

    // 服务顺序还原只消费日志；读库顺序（at ASC, id ASC）即「最后一条 reorder」的判据。
    auto logs = store.listLogs(taskId);
    if (logs.isFail()) {
        state.loadError = "读取任务日志失败：" + logs.errorMessage();
        return;
    }
    state.logs = logs.value();

    auto envs = store.listTaskEnvs(taskId);
    if (envs.isFail()) {
        state.loadError = "读取任务环境失败：" + envs.errorMessage();
        return;
    }
    for (const auto& env : envs.value()) {
        if (env.removed) continue;
        state.envs.push_back(env);

        auto services = store.listTaskServices(env.id);
        if (services.isFail()) {
            state.loadError = "读取环境服务失败：" + services.errorMessage();
            return;
        }
        for (const auto& svc : services.value()) {
            state.services.push_back(svc);
            if (svc.removed) continue;
            auto configs = store.listTaskConfigs(svc.id);
            if (configs.isFail()) {
                state.loadError = "读取服务配置失败：" + configs.errorMessage();
                return;
            }
            for (const auto& cfg : configs.value()) state.configs.push_back(cfg);
            auto steps = store.listTaskSteps(svc.id);
            if (steps.isFail()) {
                state.loadError = "读取服务步骤失败：" + steps.errorMessage();
                return;
            }
            for (const auto& step : steps.value()) state.steps.push_back(step);
        }
    }

    // 内容已变的步骤先撤回再重算校验：以撤回后的最新数据为准。
    if (compareContentChanges) {
        applyContentRevocations(store, state, previousServices, previousConfigs);
    }

    // 校验结果随数据变化重算：规则只来自 release_logic，UI 不自行推断哪些项算缺。
    if (state.validationActive) {
        state.validationMissing = isEditMode(state.displayStatus)
            ? validateTaskSubmission(state.task, state.envs, state.services,
                                     configsForValidation(state))
            : validateTask(state.services, configsForValidation(state));
        if (state.validationMissing.empty()) {
            state.validationActive = false;
            if (state.confirm == ReleaseDetailConfirm::ValidationReport) {
                state.confirm = ReleaseDetailConfirm::None;
            }
        }
    }
}

void drawErrorPage(eui::Ui& ui, const eui::Screen& screen,
                   const ReleaseDetailUiState& state, const Tokens& t) {
    const float pad = t.sizes.spacingLarge * 2.0f;
    ui.column("release.detail.root")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)
        .gap(t.sizes.spacingLarge)
        .content([&] {
            ui.text("release.detail.title")
                .size(std::max(0.0f, screen.width - pad * 2.0f), ui_kit::kHeaderH)
                .text("上线任务")
                .fontSize(t.sizes.fontSizeTitle)
                .lineHeight(ui_kit::kHeaderH)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .build();
            ui.text("release.detail.error")
                .size(std::max(0.0f, screen.width - pad * 2.0f), 24.0f)
                .text(state.loadError)
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(24.0f)
                .color(t.colors.errorText)
                .build();
        })
        .build();
}

}  // namespace detail

void composeReleaseDetailPage(eui::Ui& ui, const eui::Screen& screen,
                              ReleaseStore& store, ReleaseDetailUiState& state,
                              const ReleaseDetailActions& actions) {
    using namespace detail;  // 拆分后：本文件只剩公开入口，内部实现全在 detail
    const bool taskSwitched = state.loadedTaskId != state.taskId;
    if (state.dirty || taskSwitched) loadDetail(store, state);
    if (taskSwitched) {
        // 切任务时回到顶部：沿用上一个任务的滚动位置会落在新任务中段。
        ui.setScrollOffset("release.detail.scroll", 0.0f);
    }

    const auto& t = core::theme::current();
    const float pad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pad * 2.0f);

    if (!state.loadError.empty()) {
        drawErrorPage(ui, screen, state, t);
        return;
    }

    const int nowMinute = planMinuteNow(state.task.planDate);
    advanceTaskStatus(store, state, nowMinute);

    const bool readOnly = isReadOnly(state.displayStatus);
    const bool editMode = isEditMode(state.displayStatus);
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    // notice 3s 自动消失（秒级判据；statusWriteFailed 等持续态由重设 noticeAt 续期）。
    if (!state.notice.empty() && state.noticeAt > 0 &&
        core::time::nowUnix() - state.noticeAt >= 3) {
        state.notice.clear();
        state.noticeIsError = false;
        state.noticeAt = 0;
    }
    const bool hasNotice = !state.notice.empty();
    // F-23 锚点：发布态进页一次性滚动定位当前步（设计 §6.4；240ms 缓动因框架
    // 无毫秒时钟降级为瞬时定位，欠债已登记清单卡）。消费后立即清空。
    if (isPublishMode(state.displayStatus) && !state.pendingAnchor.empty()) {
        state.pendingAnchor.clear();
        const std::vector<TrackRowView> anchorRows =
            collectTrackRows(state, nowMinute);
        ui.setScrollOffset("release.detail.scroll", trackCurrentStepOffset(anchorRows));
    }
    // 反馈条固定在页头下方（不随内容滚动），用户在清单底部操作失败时也能看到原因。
    const float noticeBlockH = hasNotice ? kNoticeH + t.sizes.spacingLarge : 0.0f;
    const float bodyH = std::max(
        0.0f, screen.height - pad - ui_kit::kHeaderH - t.sizes.spacingLarge - noticeBlockH);

    const TicketNeeds needs = scanTicketNeeds(state);
    // 高度只由可见行数决定；把「未移除服务 + 每个服务的步骤行数」纳入 key，
    // 否则移除一个无配置/无步骤的服务时计数不变，滚动区会继续用旧内容高。
    std::size_t visibleRows = 0;
    std::size_t liveServiceCount = 0;
    for (const auto& svc : state.services) {
        if (svc.removed) continue;
        ++liveServiceCount;
        visibleRows += 3 + orderedConfigs(state, svc).size();
    }
    const std::string contentKey =
        "release.detail." + state.taskId + "." +
        std::to_string(state.envs.size()) + "." + std::to_string(liveServiceCount) +
        "." + std::to_string(visibleRows) + "." +
        std::to_string(state.configs.size()) + "." +
        std::to_string(state.steps.size()) + "." +
        (needs.nacos ? "1" : "0") + (needs.db ? "1" : "0") + "." +
        releaseTaskStatusText(state.displayStatus);

    ui.column("release.detail.root")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)
        .gap(t.sizes.spacingLarge)
        .content([&] {
            ui.stack("release.detail.header")
                .size(contentW, ui_kit::kHeaderH)
                .content([&] {
                    drawPageHeader(ui, contentW, t, state, store, nowMinute, actions);
                })
                .build();

            if (hasNotice) {
                drawNotice(ui, contentW, state, t);
            }

            components::scrollView(ui, "release.detail.scroll")
                .size(contentW, bodyH)
                .gap(t.sizes.spacingLarge)
                .step(60.0f)
                .theme(themeTokens)
                .scrollbarOverlay()
                .contentKey(contentKey)
                .content([&](eui::Ui& inner, float innerWidth, float viewportHeight) {
                    (void)viewportHeight;
                    if (editMode) {
                        // 编辑态：发布准备（单据式表单流）+ 环境与服务（F-10 双形态之一）。
                        drawPrereqSection(inner, innerWidth, state, readOnly, nowMinute,
                                          t, themeTokens, store);
                        drawEnvSection(inner, innerWidth, state, readOnly, editMode,
                                       nowMinute, t, themeTokens, store, actions);
                    } else {
                        // 发布态 / 归档态：流程轨道（F-14；前置并入轨道）。
                        drawPublishTrack(inner, innerWidth, state, nowMinute, t,
                                         themeTokens, store);
                    }
                })
                .build();
        })
        .build();

    drawConfirmDialogs(ui, screen, state, store, nowMinute, t, themeTokens);
    if (state.editorOpen) {
        drawConfigEditor(ui, screen, state, store, t, themeTokens);
    }
}

}  // namespace module::release
