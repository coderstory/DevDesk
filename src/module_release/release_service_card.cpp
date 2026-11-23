// release_service_card.cpp
// 服务卡绘制（自 release_detail_draw.cpp 拆出，debt419）：纯搬移，语义零变更。
// 公共 chip/card 基础绘制仍在 release_detail_draw.cpp（声明见 release_detail_internal.h）。

#include "release_detail_internal.h"

#include "release_checkbox.h"
#include "release_config_ui.h"
#include "release_list_ui.h"

#include <core/component_theme.h>
#include <core/render/text.h>

#include <components/dialog.h>
#include <components/input.h>
#include <components/progress.h>
#include <components/scrollview.h>

#include <ui_kit/button.h>
#include <ui_kit/dialog_shell.h>
#include <ui_kit/materials.h>
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

void drawServiceCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
                     const TaskEnv& env, const TaskService& svc,
                     ReleaseDetailUiState& state, bool readOnly, bool configurable,
                     bool allowServiceDrag, int nowMinute, const Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     ReleaseStore& store) {
    const float height = serviceCardHeight(state, svc);
    drawCardBackground(ui, id + ".bg", x, y, width, height, t, themeTokens, true);
    if (state.drag.kind == ReleaseDetailDragKind::ServiceOrder && state.drag.active &&
        state.drag.serviceId == svc.id) {
        eui::Color veil = t.colors.surface;
        veil.a = 0.55f;
        ui.rect(id + ".drag.veil")
            .position(x, y)
            .size(width, height)
            .color(veil)
            .radius(t.sizes.radiusLarge)
            .build();
    }

    const float inset = kServiceInset;
    const float branchY = y + 32.0f;
    // 「移除」入口（服务卡右上角；Done / Voided 只读时隐藏）。Text 级 + 错误红，
    // 点击只开二次确认，不直接写库。
    const float removeW = readOnly ? 0.0f : 44.0f;
    const float chipW = chipWidth(serviceKindLabel(svc.kind), t.sizes.fontSizeSmall - 1.0f);
    const float chipX =
        x + width - inset - chipW - (removeW > 0.0f ? removeW + 8.0f : 0.0f);
    drawChip(ui, id + ".kind", chipX, y + 12.0f,
             serviceKindLabel(svc.kind),
             svc.kind == ServiceKind::Backend ? t.colors.primarySoftBg : t.colors.hover,
             svc.kind == ServiceKind::Backend ? t.colors.primaryText
                                              : t.colors.textSecondary,
             t);
    if (removeW > 0.0f) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state, serviceId = svc.id, name = svc.serviceName] {
            openRemoveService(state, serviceId, name);
        };
        options.hasTextColorOverride = true;
        options.textColorOverride = t.colors.errorText;
        ui_kit::button(ui, id + ".remove", "移除", ui_kit::ButtonLevel::Text,
                       x + width - inset - removeW, y + 12.0f, removeW, 22.0f, t,
                       options);
    }
    // 服务顺序拖拽只在配置中 / 待执行可用；单服务没有排序意义，不画手柄；
    // 焦点式双栏下（allowServiceDrag=false）焦点区单卡无序列感，把柄隐藏，
    // 落库契约与逻辑函数不变（need192 §1.2 断点：双栏拖拽入口后批补）。
    const bool showOrderHandle =
        configurable && allowServiceDrag && countLiveServices(state, env) > 1;
    const float nameX = x + inset + (showOrderHandle ? kDragHandleW + kDragHandleGap : 0.0f);
    const float nameW = std::max(
        0.0f, width - inset * 2.0f - chipW - 8.0f -
                  (removeW > 0.0f ? removeW + 8.0f : 0.0f) -
                  (showOrderHandle ? kDragHandleW + kDragHandleGap : 0.0f));
    if (showOrderHandle) {
        drawDragHandle(
            ui, id + ".order", x + inset, y + 8.0f, kDragHandleW, kDragHandleH, t,
            [&state, serviceId = svc.id, envId = env.id,
             envName = env.envName](const eui::PointerEvent&, const eui::Rect&) {
                state.drag = ReleaseDetailDragState{};
                state.drag.kind = ReleaseDetailDragKind::ServiceOrder;
                state.drag.envId = envId;
                state.drag.envName = envName;
                state.drag.serviceId = serviceId;
            },
            [&state](const core::dsl::DragEvent& event) {
                if (state.drag.kind != ReleaseDetailDragKind::ServiceOrder) return;
                state.drag.active = true;
                const double scale = event.scale > 0.0f ? event.scale : 1.0f;
                state.drag.deltaY = static_cast<float>(event.totalY / scale);
                updateServiceDragTarget(state);
            },
            [&store, &state](const eui::PointerEvent& event, const eui::Rect&) {
                finishServiceDrag(store, state,
                                  event.action == eui::PointerAction::Release);
            });
    }
    ui.text(id + ".name")
        .position(nameX, y + 10.0f)
        .size(nameW, core::theme::scaled(20.0f))
        .text(svc.serviceName)
        .fontSize(core::theme::scaled(18.0f))  // §4.2 L1：服务名 15→18
        .lineHeight(core::theme::scaled(20.0f))
        .fontWeight(600)  // need219 §2.1：行首标题两档制 600（原 700 越档）
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .build();
    const bool branchFlagged = serviceBranchFlagged(state, svc.serviceName);
    if (isEditMode(state.displayStatus)) {
        // 池 54（F-54-1）：分支行 = 44px 表单行（label 96 + 输入框 32，前置单据流
        // 同语言）；分支的确认语义在「合并分支」步骤行，行尾不放勾。守卫是 F-03
        // 的绘制点防御——发布/归档走流程轨道不进本节（drawEnvsSection 仅编辑态），
        // 分支值在发布态无展示面为池 53 IA 既有，非本单缺口。
        const float labelW = kFormLabelW;
        const float inputX = nameX + labelW + 12.0f;
        const float inputW = std::max(0.0f, x + width - inset - inputX);
        if (branchFlagged) {
            ui.rect(id + ".branch.flag")
                .position(nameX - 4.0f, branchY)
                .size(nameW + 8.0f, kStepRowH)
                .color(eui::mixColor(t.colors.surface, t.colors.error, 0.10f))
                .radius(4.0f)
                .build();
        }
        ui.text(id + ".branch.label")
            .position(nameX, branchY)
            .size(labelW, kStepRowH)
            .text("分支")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(kStepRowH)
            .verticalAlign(eui::VerticalAlign::Center)
            // need192 §4.3：标签红字废止，flagged 由浅红底（branch.flag）承载。
            .color(t.colors.textPrimary)
            .build();
        const bool editingThis = state.branchEditServiceId == svc.id;
        components::input(ui, id + ".branch.input")
            .position(inputX, branchY + (kStepRowH - 32.0f) * 0.5f)
            .size(inputW, 32.0f)
            .value(editingThis ? state.branchEditText : svc.branch)
            .placeholder("填写分支")
            .fontSize(t.sizes.fontSizeBody)
            .theme(themeTokens)
            .onChange([&state, serviceId = svc.id](const std::string& value) {
                beginBranchEdit(state, serviceId, value);
            })
            .onFocus([&store, &state](bool focused) {
                if (!focused) applySaveBranch(store, state);
            })
            .onEnter([&store, &state]() { applySaveBranch(store, state); })
            .onCancel([&store, &state]() { applySaveBranch(store, state); })
            .build();
    }

    std::vector<StepRowView> rows;
    {
        StepRowView merge;
        merge.id = "merge";
        merge.label = stepLabel(StepKind::MergeBranch);
        merge.done = stepDone(state, svc.id, StepKind::MergeBranch);
        merge.statusText = merge.done ? "已完成" : "未完成";
        merge.stepKind = StepKind::MergeBranch;
        rows.push_back(std::move(merge));

        const std::vector<TaskConfig> configs = orderedConfigs(state, svc);
        for (std::size_t i = 0; i < configs.size(); ++i) {
            const TaskConfig& cfg = configs[i];
            StepRowView row;
            row.id = "config." + std::to_string(configKindIndex(cfg.kind));
            row.label = configLabel(cfg.kind);
            row.skipped = cfg.skipped;
            // 跳过态优先于完成态：逻辑层对 skipped 配置不计进度，视觉也不能显示
            // 成「已完成」，否则会出现划线与绿勾并存的矛盾状态。
            row.done = !cfg.skipped && cfg.doneAt != 0;
            row.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
            row.invalid = serviceConfigFlagged(state, svc.serviceName, cfg.kind);
            row.dragging = state.drag.kind == ReleaseDetailDragKind::ConfigOrder &&
                           state.drag.active && state.drag.serviceId == svc.id &&
                           state.drag.configKind == cfg.kind;
            row.config = true;
            row.configKind = cfg.kind;
            row.configIndex = i;
            row.configCount = configs.size();
            row.statusText = row.skipped
                                 ? "已跳过"
                                 : (row.done ? "已完成"
                                             : (cfg.content.empty()
                                                    ? "未完成"
                                                    : "已填 " +
                                                          std::to_string(
                                                              utf8CharCount(cfg.content)) +
                                                          " 字"));
            rows.push_back(std::move(row));
        }

        StepRowView deploy;
        deploy.id = "deploy";
        deploy.label = stepLabel(StepKind::Deploy);
        deploy.done = stepDone(state, svc.id, StepKind::Deploy);
        deploy.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
        deploy.statusText = deploy.done ? "已完成" : "未完成";
        deploy.stepKind = StepKind::Deploy;
        rows.push_back(std::move(deploy));

        StepRowView verify;
        verify.id = "verify";
        verify.label = stepLabel(StepKind::Verify);
        verify.done = stepDone(state, svc.id, StepKind::Verify);
        verify.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
        verify.statusText = verify.done ? "已完成" : "未完成";
        verify.stepKind = StepKind::Verify;
        rows.push_back(std::move(verify));
    }

    std::size_t currentIndex = rows.size();
    if (!readOnly) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].done && !rows[i].skipped && rows[i].timeReady) {
                currentIndex = i;
                break;
            }
        }
    }

    const std::string scope = svc.serviceName;
    float rowY = y + kServiceRowStart +
                 (isEditMode(state.displayStatus) ? kStepRowH - 16.0f : 0.0f);
    std::vector<float> rowYs;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const StepRowView& row = rows[i];
        const std::string rowId = id + ".step." + row.id;
        rowYs.push_back(rowY);

        std::function<void()> onRowClick;
        if (!row.done) {
            if (row.config) {
                onRowClick = [&state, serviceId = svc.id, kind = row.configKind,
                              timeReady = row.timeReady, envCopy = env,
                              envName = env.envName, svcName = svc.serviceName,
                              nowMinute] {
                    if (isReadOnly(state.displayStatus)) return;
                    if (!timeReady) {
                        setNotice(state,
                                  "环境「" + envName + "」时间窗未开始，暂时不能执行该步骤",
                                  true);
                        return;
                    }
                    // 门槛按「此刻」的表单值重查：输入框刚填完、同一帧点击时捕获值
                    // 可能还是旧的，不能拿快照挡下合法确认。
                    const std::vector<std::string> missing =
                        configGateMissing(state.task, kind);
                    if (!missing.empty()) {
                        setNotice(state, "请先填写：" + joinMissing(missing), true);
                        return;
                    }
                    // need192 §6：完成类动作先过强确认门，确认才落态。
                    openCompletionGate(
                        state,
                        envName + " / " + svcName + " / " + configLabel(kind),
                        "计入任务完成度", configLabel(kind),
                        [serviceId, kind, envCopy, nowMinute](
                            ReleaseStore& s, ReleaseDetailUiState& st) {
                            markConfigCompleted(s, st, serviceId, kind, envCopy,
                                                nowMinute);
                        });
                };
            } else {
                onRowClick = [&state, serviceId = svc.id, kind = row.stepKind,
                              timeReady = row.timeReady, envCopy = env,
                              envName = env.envName, svcName = svc.serviceName,
                              nowMinute] {
                    if (isReadOnly(state.displayStatus)) return;
                    if (!timeReady) {
                        setNotice(state,
                                  "环境「" + envName + "」时间窗未开始，暂时不能执行该步骤",
                                  true);
                        return;
                    }
                    if (kind == StepKind::Deploy) {
                        const std::vector<std::string> missing =
                            deployGateMissing(state.task, scanTicketNeeds(state));
                        if (!missing.empty()) {
                            setNotice(state, "请先填写：" + joinMissing(missing), true);
                            return;
                        }
                    }
                    openCompletionGate(
                        state,
                        envName + " / " + svcName + " / " + stepLabel(kind),
                        stepGateImpact(kind), stepLabel(kind),
                        [serviceId, kind, envCopy, nowMinute](
                            ReleaseStore& s, ReleaseDetailUiState& st) {
                            markStepCompleted(s, st, serviceId, kind, envCopy, nowMinute);
                        });
                };
            }
        }

        std::function<void()> onRowClickWithRevoke = onRowClick;
        if (row.done) {
            if (row.config) {
                onRowClickWithRevoke = [&state, serviceId = svc.id,
                                        kind = row.configKind, scope] {
                    if (isReadOnly(state.displayStatus)) return;
                    openRevokeConfig(state, serviceId, kind, scope);
                };
            } else {
                onRowClickWithRevoke = [&state, serviceId = svc.id, kind = row.stepKind,
                                        scope] {
                    if (isReadOnly(state.displayStatus)) return;
                    openRevokeStep(state, serviceId, kind, scope);
                };
            }
        }

        std::function<void()> onView;
        if (row.config) {
            // 配置查看 / 编辑 = 页内自绘全屏编辑层（发布态入口在 logic 层拒绝）。
            onView = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                openConfigEditor(store, state, serviceId, kind);
            };
        }

        std::function<void()> onSkipToggle;
        if (row.config && !readOnly) {
            onSkipToggle = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                toggleConfigSkip(store, state, serviceId, kind);
            };
        }
        drawStepRow(ui, rowId, x, rowY, width, row, i == currentIndex, readOnly,
                    configurable, true, t, onRowClickWithRevoke, onView, onSkipToggle);
        rowY += kStepRowH + kStepRowGap;

        if (row.config && configurable) {
            const float handleX = x + 8.0f + 10.0f + kStepDot + 4.0f;
            const float handleY = rowYs.back() + (kStepRowH - kDragHandleH) * 0.5f;
            drawDragHandle(
                ui, rowId + ".drag", handleX, handleY, kDragHandleW, kDragHandleH, t,
                [&state, serviceId = svc.id,
                 kind = row.configKind](const eui::PointerEvent&, const eui::Rect&) {
                    state.drag = ReleaseDetailDragState{};
                    state.drag.kind = ReleaseDetailDragKind::ConfigOrder;
                    state.drag.serviceId = serviceId;
                    state.drag.configKind = kind;
                },
                [&state](const core::dsl::DragEvent& event) {
                    if (state.drag.kind != ReleaseDetailDragKind::ConfigOrder) return;
                    state.drag.active = true;
                    const double scale = event.scale > 0.0f ? event.scale : 1.0f;
                    state.drag.deltaY = static_cast<float>(event.totalY / scale);
                    updateConfigDragTarget(state);
                },
                [&store, &state](const eui::PointerEvent& event, const eui::Rect&) {
                    finishConfigDrag(store, state,
                                     event.action == eui::PointerAction::Release);
                });
        }
    }

    // 配置拖拽的插入位指示线：画在目标边界（行间，与展开次行后的实际行位对齐）。
    if (configurable && state.drag.kind == ReleaseDetailDragKind::ConfigOrder &&
        state.drag.active && state.drag.serviceId == svc.id) {
        const std::vector<TaskConfig> allConfigs = orderedConfigs(state, svc);
        const int source = state.drag.sourceIndex;
        const int target = state.drag.targetIndex;
        if (!allConfigs.empty() && source >= 0 && target >= 0 &&
            source < static_cast<int>(allConfigs.size())) {
            const int boundary = target < source ? target : target + 1;
            const float step = kStepRowH + kStepRowGap;
            const float firstConfigY = rowYs.size() > 1 ? rowYs[1] : y + kServiceRowStart + step;
            float lineY = 0.0f;
            if (boundary < static_cast<int>(allConfigs.size())) {
                lineY = firstConfigY + static_cast<float>(boundary) * step -
                        kStepRowGap * 0.5f - kDragLineH * 0.5f;
            } else {
                lineY = firstConfigY +
                        static_cast<float>(allConfigs.size() - 1) * step + kStepRowH +
                        kStepRowGap * 0.5f - kDragLineH * 0.5f;
            }
            ui.rect(id + ".drag.insert")
                .position(x + 8.0f, lineY)
                .size(std::max(0.0f, width - 16.0f), kDragLineH)
                .color(t.colors.primary)
                .radius(1.0f)
                .build();
        }
    }
}
} // namespace detail
} // namespace release
