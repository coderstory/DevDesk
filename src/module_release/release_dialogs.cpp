// 新建任务弹层（票据 04）实现：日期滚轮 + 非常规日二次确认 + 同日复用 +
// 环境多选与服务预览 + 快照写入。
//
// 反重构警告：
//   - 环境/服务快照必须在创建时按基础配置取值后落库，不允许在任务详情阶段
//     再回查基础配置（那会让改/删基础配置污染已建任务的历史）。
//   - 「同日已有任务」以库中非作废任务为准（作废不占当天名额），不能只用
//     弹层打开时缓存的列表判断——提交前必须重查。
#include "release_dialogs.h"

#include "release_checkbox.h"
#include "release_config_ui.h"  // formatTimeWindow：时间窗显示唯一口径
#include "release_list_ui.h"    // releaseIsIsoDate / releaseWeekdayLabel
#include "release_logic.h"      // isRegularReleaseDay
#include "release_store.h"

#include <core/component_theme.h>
#include <core/theme.h>
#include <core/time.h>

#include <components/datepicker.h>
#include <components/scrollview.h>

#include <eui/types.h>

#include <ui_kit/button.h>
#include <ui_kit/card.h>
#include <ui_kit/dialog_shell.h>
#include <ui_kit/metrics.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace module::release {

namespace {

// 弹窗宽走规范宽五档 Wide=640（need156）；高 540 保留（内容区随壳扣头/底 158）。
constexpr float kDialogH = 540.0f;
constexpr float kEnvRowH = 36.0f;
constexpr float kEnvRowGap = 4.0f;
constexpr float kChipH = 22.0f;

bool parseDateParts(const std::string& isoDate, int& year, int& month, int& day) {
    if (!releaseIsIsoDate(isoDate)) return false;
    year = (isoDate[0] - '0') * 1000 + (isoDate[1] - '0') * 100 +
           (isoDate[2] - '0') * 10 + (isoDate[3] - '0');
    month = (isoDate[5] - '0') * 10 + (isoDate[6] - '0');
    day = (isoDate[8] - '0') * 10 + (isoDate[9] - '0');
    return true;
}

std::string formatIsoDate(int year, int month, int day) {
    char buffer[11] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return std::string(buffer);
}

// 默认计划日 = 本机今天（core::time 已做平台分支隔离）
void setToday(ReleaseNewTaskDialogState& state) {
    std::tm local{};
    core::time::localTime(core::time::nowUnix(), &local);
    state.year = local.tm_year + 1900;
    state.month = local.tm_mon + 1;
    state.day = local.tm_mday;
    state.planDate = formatIsoDate(state.year, state.month, state.day);
}

float chipWidth(const std::string& text) {
    float w = 17.6f;  // 11px 内距
    for (unsigned char c : text) {
        w += (c >= 0x80) ? 4.73f : 6.16f;
    }
    return w;
}

const EnvDef* findEnv(const ReleaseNewTaskDialogState& state, std::int64_t id) {
    for (const auto& env : state.envs) {
        if (env.id == id) return &env;
    }
    return nullptr;
}

const ServiceDef* findService(const ReleaseNewTaskDialogState& state,
                              std::int64_t id) {
    for (const auto& service : state.services) {
        if (service.id == id) return &service;
    }
    return nullptr;
}

void loadDialogData(ReleaseStore& store, ReleaseNewTaskDialogState& state) {
    // 失败也置位：弹层内持续重试会每帧打库；重新打开（打开沿）会重新加载
    state.dataLoaded = true;
    state.envs.clear();
    state.services.clear();

    auto envs = store.listEnvs();
    if (envs.isFail()) {
        state.error = "读取环境失败：" + envs.errorMessage();
        return;
    }
    state.envs = envs.value();

    auto services = store.listServices();
    if (services.isFail()) {
        state.error = "读取服务目录失败：" + services.errorMessage();
        return;
    }
    state.services = services.value();
}

std::vector<std::string> previewServiceNames(const ReleaseNewTaskDialogState& state) {
    std::vector<std::string> names;
    for (const std::int64_t envId : state.selectedEnvIds) {
        const EnvDef* env = findEnv(state, envId);
        if (env == nullptr) continue;
        for (const std::int64_t serviceId : env->serviceIds) {
            const ServiceDef* service = findService(state, serviceId);
            if (service == nullptr) continue;
            if (std::find(names.begin(), names.end(), service->name) == names.end()) {
                names.push_back(service->name);
            }
        }
    }
    return names;
}

void toggleEnv(ReleaseNewTaskDialogState& state, std::int64_t envId, bool selected) {
    auto it = std::find(state.selectedEnvIds.begin(), state.selectedEnvIds.end(), envId);
    if (selected) {
        if (it == state.selectedEnvIds.end()) state.selectedEnvIds.push_back(envId);
    } else if (it != state.selectedEnvIds.end()) {
        state.selectedEnvIds.erase(it);
    }
    state.error.clear();
}

// 提交：同日复用 → 非常规日确认 → 创建任务 → 逐环境写快照。
void submitNewTask(ReleaseStore& store, ReleaseNewTaskDialogState& state,
                   const std::function<void(std::int64_t)>& openTask) {
    state.error.clear();
    if (!releaseIsIsoDate(state.planDate)) {
        state.error = "请先选择计划上线日期";
        return;
    }

    // 同日复用先于环境校验：已有任务时点「创建」应直接打开它，不该因没选环境被挡
    auto existing = store.findTaskByDate(state.planDate);
    if (existing.isFail()) {
        state.error = "读取计划日任务失败：" + existing.errorMessage();
        return;
    }
    std::vector<Task> sameDate;
    if (existing.value().has_value()) sameDate.push_back(*existing.value());
    const ReleaseNewTaskDateDecision decision =
        releaseDecideNewTaskDate(state.planDate, sameDate);
    if (decision.action == ReleaseNewTaskDateAction::OpenExisting) {
        state.open = false;
        state.pickerOpen = false;
        state.irregularConfirmOpen = false;
        if (openTask) openTask(decision.existingTaskId);
        return;
    }
    if (decision.action == ReleaseNewTaskDateAction::InvalidDate) {
        state.error = "计划日期无效";
        return;
    }
    if (state.selectedEnvIds.empty()) {
        state.error = "请至少选择一个环境";
        return;
    }
    if (decision.action == ReleaseNewTaskDateAction::ConfirmIrregular &&
        !state.irregularConfirmed) {
        state.irregularConfirmOpen = true;
        return;
    }

    // 建任务 + 全部环境/服务快照走原子接口（同一事务）：任一环境写失败整体回滚，
    // 不允许残留「任务建好了、快照只写了一部分」的 Draft 死单。
    Task task;
    task.planDate = state.planDate;
    task.status = TaskStatus::Draft;

    std::vector<TaskEnv> envSnapshots;
    std::vector<std::vector<TaskService>> servicesPerEnv;
    for (const std::int64_t envId : state.selectedEnvIds) {
        const EnvDef* env = findEnv(state, envId);
        if (env == nullptr) continue;  // 基础配置被并发删除：跳过该环境，不中断其余写入
        TaskEnv envSnapshot;
        envSnapshot.envName = env->name;
        envSnapshot.startMinute = env->startMinute;
        envSnapshot.endMinute = env->endMinute;

        std::vector<TaskService> services;
        for (const std::int64_t serviceId : env->serviceIds) {
            const ServiceDef* service = findService(state, serviceId);
            if (service == nullptr) continue;
            TaskService snapshot;
            snapshot.serviceName = service->name;
            snapshot.kind = service->kind;
            snapshot.defaultBranch = service->defaultBranch;
            snapshot.branch = service->defaultBranch;  // 预填默认分支，执行页可改
            services.push_back(snapshot);
        }
        envSnapshots.push_back(envSnapshot);
        servicesPerEnv.push_back(services);
    }

    auto created = store.createTaskWithEnvs(task, envSnapshots, servicesPerEnv);
    if (created.isFail()) {
        state.error = "创建失败，未产生任何数据，可直接重试：" + created.errorMessage();
        return;
    }
    const std::int64_t taskId = created.value();

    state.selectedEnvIds.clear();
    state.open = false;
    state.pickerOpen = false;
    state.irregularConfirmOpen = false;
    state.irregularConfirmed = false;
    if (openTask) openTask(taskId);
}

void drawDialogChip(eui::Ui& ui, const std::string& id, float x, float y,
                    const std::string& text, const eui::Color& bg,
                    const eui::Color& fg, const core::theme::Tokens& t) {
    const float width = chipWidth(text);
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, kChipH)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(width, kChipH)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(kChipH)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .build();
}

void drawEnvRow(eui::Ui& ui, const std::string& id, float rowW,
                const EnvDef& env, bool selected, ReleaseNewTaskDialogState& state,
                const core::theme::Tokens& t) {
    ui.stack(id)
        .size(rowW, kEnvRowH)
        .content([&] {
            ReleaseCheckboxOptions box;
            box.checked = selected;
            box.label =
                env.name + "   " + formatTimeWindow(env.startMinute, env.endMinute);
            box.boxSize = 16.0f;
            box.fontSize = 12.0f;
            box.width = rowW;
            box.height = kEnvRowH;
            drawReleaseCheckbox(
                ui, id + ".check", box,
                [&state, envId = env.id]() {
                    toggleEnv(state, envId,
                              std::find(state.selectedEnvIds.begin(),
                                        state.selectedEnvIds.end(),
                                        envId) == state.selectedEnvIds.end());
                },
                t);
        })
        .build();
}

} // namespace

ReleaseNewTaskDateDecision releaseDecideNewTaskDate(const std::string& planDate,
                                                    const std::vector<Task>& tasks) {
    ReleaseNewTaskDateDecision decision;
    if (!releaseIsIsoDate(planDate)) {
        decision.action = ReleaseNewTaskDateAction::InvalidDate;
        return decision;
    }
    for (const auto& task : tasks) {
        // 作废任务不占当天名额（设计：作废后可再建）
        if (task.planDate == planDate && task.status != TaskStatus::Voided) {
            decision.action = ReleaseNewTaskDateAction::OpenExisting;
            decision.existingTaskId = task.id;
            return decision;
        }
    }
    decision.action = isRegularReleaseDay(planDate)
        ? ReleaseNewTaskDateAction::Create
        : ReleaseNewTaskDateAction::ConfirmIrregular;
    return decision;
}

void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen, ReleaseStore& store,
                       ReleaseNewTaskDialogState& state,
                       const std::function<void(std::int64_t taskId)>& openTask) {
    // 打开沿：清上一次会话的确认态/选择/缓存，重新加载基础配置
    if (state.open && !state.wasOpen) {
        state.error.clear();
        state.pickerOpen = false;
        state.irregularConfirmOpen = false;
        state.irregularConfirmed = false;
        state.dataLoaded = false;
        state.selectedEnvIds.clear();
        // 空表单语义 = 新建草稿：日期锚点重置为今天（池 BUG 212：残留上一次
        // 会话的 planDate 会让「创建任务」命中同日复用，打开上一个任务的执行页。
        // 现无调用方预填 planDate，见 clue devdesk:bug212-newtask-stale-date）。
        setToday(state);
    }
    state.wasOpen = state.open;
    if (!state.open) {
        state.pickerOpen = false;
        state.irregularConfirmOpen = false;
        return;
    }

    const auto& t = core::theme::current();
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    if (!state.dataLoaded) loadDialogData(store, state);

    const bool regularDay = releaseIsRegularReleaseDay(state.planDate);

    // need156：壳改走标准弹窗壳（头区分隔线 + 底区细线 + 右锚按钮对）；宽 560
    // 归入宽五档 Wide（640），内容区自 y=69 起（原 58，整体 +11 平移）。
    ui_kit::DialogShellSpec shell;
    shell.id = "release.newtask.dialog";
    shell.width = ui_kit::DialogWidth::Wide;
    shell.height = kDialogH;
    shell.open = true;
    shell.zIndex = 1000;
    shell.title = "新建上线任务";
    shell.cancel = {"取消", [&state] { state.open = false; }};
    shell.confirm = {"创建任务",
                     [&store, &state, openTask] { submitNewTask(store, state, openTask); }};
    shell.onOpenChange = [&state](bool open) { state.open = open; };
    ui_kit::drawDialogShell(ui, screen, shell, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
        const float x0 = area.x;
        const float innerW = area.w;
        const float yBase = area.y;  // 69：内容区顶（规范 §3.2）

        // 计划上线日期（三列滚轮 picker 的提交值）
        ui.text("release.newtask.date.label")
            .position(x0, yBase)
            .size(innerW, core::theme::scaled(16.0f))
            .text("计划上线日期")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(core::theme::scaled(16.0f))
            .color(t.colors.textSecondary)
            .build();
        // 弹窗内豁免档（need219 §5.2-2）：cardPanelColored，底色比弹窗白底可辨
        ui_kit::cardPanelColored(ui, "release.newtask.date", x0, yBase + 20.0f, innerW,
                                 40.0f, t.sizes.radius, t,
                                 t.colors.surfaceElevated);
        const float dateChipW = chipWidth(regularDay ? "常规发布日" : "非常规发布日");
        ui.text("release.newtask.date.text")
            .position(x0 + 12.0f, yBase + 20.0f)
            .size(std::max(0.0f, innerW - 12.0f - dateChipW - 16.0f), core::theme::scaled(40.0f))
            .text(state.planDate + "   " + releaseWeekdayLabel(state.planDate))
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(core::theme::scaled(40.0f))
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textPrimary)
            .build();
        drawDialogChip(ui, "release.newtask.date.chip",
                       x0 + innerW - dateChipW - 8.0f, yBase + 29.0f,
                       regularDay ? "常规发布日" : "非常规发布日",
                       regularDay ? t.colors.primarySoftBg
                                  : eui::mixColor(t.colors.surface, t.colors.warning, 0.14f),
                       regularDay ? t.colors.primaryText : t.colors.warning, t);
        // 透明命中层压在文字/chip 之上：整块日期框都可点开滚轮
        ui.rect("release.newtask.date.hit")
            .position(x0, yBase + 20.0f)
            .size(innerW, 40.0f)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .onClick([&state] {
                int year = 0;
                int month = 0;
                int day = 0;
                if (parseDateParts(state.planDate, year, month, day)) {
                    state.year = year;
                    state.month = month;
                    state.day = day;
                }
                state.pickerOpen = true;
            })
            .build();

        // 环境多选 + 时间窗（选中即预览将带入的服务）
        ui.text("release.newtask.env.label")
            .position(x0, yBase + 76.0f)
            .size(innerW, core::theme::scaled(16.0f))
            .text("加入环境（可多选）")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(core::theme::scaled(16.0f))
            .color(t.colors.textSecondary)
            .build();
        components::scrollView(ui, "release.newtask.env.scroll")
            .position(x0, yBase + 96.0f)
                .size(innerW, 168.0f)
                .step(60.0f)
                .theme(themeTokens)
                .scrollbarOverlay()
                .contentKey("release.newtask.env." + std::to_string(state.envs.size()))
                .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    if (state.envs.empty()) {
                        inner.text("release.newtask.env.empty")
                            .size(contentWidth, core::theme::scaled(36.0f))
                            .text("基础配置里还没有环境")
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(core::theme::scaled(36.0f))
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    const float rowsH = static_cast<float>(state.envs.size()) * kEnvRowH +
                                        static_cast<float>(state.envs.size() - 1) * kEnvRowGap;
                    inner.stack("release.newtask.env.spacer")
                        .size(contentWidth, rowsH)
                        .build();
                    for (std::size_t i = 0; i < state.envs.size(); ++i) {
                        const EnvDef& env = state.envs[i];
                        const bool selected =
                            std::find(state.selectedEnvIds.begin(),
                                      state.selectedEnvIds.end(),
                                      env.id) != state.selectedEnvIds.end();
                        const std::int64_t envId = env.id;
                        inner.stack("release.newtask.env.row." + std::to_string(envId))
                            .position(0.0f, static_cast<float>(i) * (kEnvRowH + kEnvRowGap))
                            .ignoreLayout()
                            .size(contentWidth, kEnvRowH)
                            .content([&, i, selected] {
                                drawEnvRow(inner,
                                           "release.newtask.env." +
                                               std::to_string(state.envs[i].id),
                                           contentWidth, state.envs[i], selected, state,
                                           t);
                            })
                            .build();
                    }
                })
                .build();

            // 预览：选中环境涉及的去重服务名（空环境也明确告知）
            const std::vector<std::string> serviceNames = previewServiceNames(state);
            std::string preview;
            if (state.selectedEnvIds.empty()) {
                preview = "尚未选择环境";
            } else {
                preview = "已选 " + std::to_string(state.selectedEnvIds.size()) +
                          " 个环境，将带入 " + std::to_string(serviceNames.size()) + " 个服务";
                if (!serviceNames.empty()) {
                    preview += "：";
                    for (std::size_t i = 0; i < serviceNames.size(); ++i) {
                        if (i > 0) preview += "、";
                        preview += serviceNames[i];
                    }
                }
            }
            ui.text("release.newtask.preview")
                .position(x0, yBase + 296.0f)
                .size(innerW, core::theme::scaled(36.0f))
                .text(preview)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(core::theme::scaled(18.0f))
                .maxWidth(innerW)
                .wrap(true)
                .color(t.colors.textSecondary)
                .build();

            if (!state.error.empty()) {
                ui.text("release.newtask.error")
                    .position(x0, yBase + 340.0f)
                    .size(innerW, core::theme::scaled(20.0f))
                    .text(state.error)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(core::theme::scaled(20.0f))
                    .maxWidth(innerW)
                    .wrap(false)
                    .overflow(core::TextOverflow::Ellipsis)
                    .color(t.colors.errorText)
                    .build();
            }
        });

    // 日期滚轮（模态，z 高于弹层、低于确认层）
    if (state.pickerOpen) {
        components::datePicker(ui, "release.newtask.datepicker")
            .open(true)
            .screen(screen.width, screen.height)
            .size(380.0f, 260.0f)
            .date(state.year, state.month, state.day)
            .theme(themeTokens)
            .zIndex(1200)
            .onChange([&state](int year, int month, int day) {
                state.year = year;
                state.month = month;
                state.day = day;
                state.planDate = formatIsoDate(year, month, day);
                state.irregularConfirmed = false;  // 换日期后旧确认作废
                state.error.clear();
            })
            .onOpenChange([&state](bool open) { state.pickerOpen = open; })
            .build();
    }

    // 非常规发布日二次确认：取消只关本层，回到日期选择（need156：标准壳）
    ui_kit::DialogShellSpec confirmShell;
    confirmShell.id = "release.newtask.confirm";
    confirmShell.width = ui_kit::DialogWidth::Standard;
    confirmShell.height = ui_kit::kDialogMinH;  // 240（规范 §3.4 下限，原 220）
    confirmShell.open = state.irregularConfirmOpen;
    confirmShell.zIndex = 1300;
    confirmShell.title = "非常规发布日";
    confirmShell.cancel = {"取消", [&state] { state.irregularConfirmOpen = false; }};
    confirmShell.confirm = {"确定要建", [&store, &state, openTask] {
        state.irregularConfirmed = true;
        state.irregularConfirmOpen = false;
        submitNewTask(store, state, openTask);
    }};
    confirmShell.onOpenChange = [&state](bool open) { state.irregularConfirmOpen = open; };
    ui_kit::drawDialogShell(ui, screen, confirmShell, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
                                if (!state.irregularConfirmOpen) return;
                                ui.text("release.newtask.confirm.message")
                                    .position(area.x, area.y)
                                    .size(area.w, core::theme::scaled(28.0f))
                                    .text("所选日期不是周二/周四，确定要建吗？")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(core::theme::scaled(28.0f))
                                    .color(t.colors.textPrimary)
                                    .build();
                            });
}

void drawFullConfigEditorDialog(eui::Ui& ui) { (void)ui; }
void drawWithdrawConfirmDialog(eui::Ui& ui) { (void)ui; }

} // namespace module::release