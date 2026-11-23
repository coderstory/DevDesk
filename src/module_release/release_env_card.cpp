// release_env_card.cpp
// 环境卡 + 前置检查分区绘制（自 release_detail_draw.cpp 拆出，debt419）：纯搬移，语义零变更。
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

void drawPrereqSection(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                       bool readOnly, int nowMinute, const Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens,
                       ReleaseStore& store) {
    (void)readOnly;   // 本节只在编辑态渲染（发布/归档走流程轨道）
    (void)nowMinute;  // 到期提示不逐行常驻（设计 §5.2）
    const TicketNeeds needs = scanTicketNeeds(state);
    struct DocRow {
        PrereqKey key;
    };
    std::vector<DocRow> docs{{PrereqKey::Pretest}, {PrereqKey::Cab}};
    if (needs.nacos) docs.push_back({PrereqKey::Nacos});
    if (needs.db) docs.push_back({PrereqKey::Db});

    const float inset = 16.0f;
    const float rowH = kStepRowH;
    const float sectionH = prereqSectionHeight(state);
    const float cardH = sectionH - kSectionTitleH - kSectionGap;

    ui.stack("release.detail.prereq.section")
        .size(width, sectionH)
        .content([&] {
            drawSectionTitle(ui, "release.detail.prereq.title", 0.0f, 0.0f, "发布准备", t);
            const float cardY = kSectionTitleH + kSectionGap;
            drawCardBackground(ui, "release.detail.prereq.card", 0.0f, cardY, width,
                               cardH, t, themeTokens, false);
            float y = cardY + inset;
            ui.text("release.detail.prereq.group.docs")
                .position(inset, y)
                .size(width - inset * 2.0f, core::theme::scaled(18.0f))
                .text("审批单据")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textTertiary)
                .build();
            y += 18.0f + 8.0f;

            for (std::size_t i = 0; i < docs.size(); ++i) {
                const PrereqKey key = docs[i].key;
                const std::string id = "release.detail.prereq." + std::to_string(i);
                const bool checked = prereqChecked(state, key);
                const bool gateOpen = prereqGateOpen(state, key);
                const bool flagged = prereqFlagged(state, key);
                // label 定宽 96 + 输入 flex + 行尾勾
                const float labelW = kFormLabelW;
                const float checkW = 64.0f;
                const float inputX = inset + labelW + 12.0f;
                const float inputW = std::max(
                    0.0f, width - inset * 2.0f - labelW - 12.0f - checkW - 12.0f);
                ui.text(id + ".label")
                    .position(inset, y)
                    .size(labelW, rowH)
                    .text(prereqTitle(key))
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(rowH)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(flagged && !checked ? t.colors.errorText
                                               : t.colors.textPrimary)
                    .build();
                components::input(ui, id + ".input")
                    .position(inputX, y + (rowH - 32.0f) * 0.5f)
                    .size(inputW, 32.0f)
                    .value(prereqValue(state, key))
                    .placeholder("填写单号")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([&state, key](const std::string& value) {
                        setPrereqValue(state, key, value);
                    })
                    .onFocus([&store, &state](bool focused) {
                        if (!focused) saveTask(store, state);
                    })
                    .onEnter([&store, &state]() { saveTask(store, state); })
                    .onCancel([&store, &state]() { saveTask(store, state); })
                    .build();
                if (!checked && !gateOpen) {
                    // 提示字叠在输入框容器内（右对齐）：须垂直居中，否则顶格贴
                    // 行顶、与 32px 输入框错位（#362）。
                    ui.text(id + ".gate")
                        .position(inputX, y)
                        .size(std::max(0.0f, inputW - 120.0f), rowH)
                        .text("请先填写单据号")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(rowH)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                }
                ReleaseCheckboxOptions check;
                check.checked = checked;
                check.enabled = gateOpen || checked;
                check.label = prereqCheckLabel(key);
                check.boxSize = kCheckSize;
                check.fontSize = t.sizes.fontSizeSmall;
                check.width = checkW;
                check.height = kCheckSize;
                check.inset = 0.0f;
                check.gap = 6.0f;
                std::function<void()> onCheck;
                const std::string code = prereqCode(key);
                const std::string label = prereqCheckLabel(key);
                onCheck = [&state, key, code, label]() {
                    if (isReadOnly(state.displayStatus)) return;
                    if (prereqChecked(state, key)) {
                        openRevokePrereq(state, code, label, false);
                        return;
                    }
                    // need192 §6.1：前置带完成勾同走强确认门（确认前不写库）。
                    const std::string title = prereqTitle(key);
                    openCompletionGate(state, title, "计入前置完成度并解锁提交校验",
                                       title,
                                       [code, title](ReleaseStore& s,
                                                     ReleaseDetailUiState& st) {
                                           markPrereqCompleted(s, st, code, title);
                                       });
                };
                drawReleaseCheckboxAt(ui, id + ".check",
                                      width - inset - checkW, y + (rowH - kCheckSize) * 0.5f,
                                      check, onCheck, t);
                y += rowH + 8.0f;
            }

            y += 8.0f;
            // 公告摘要行（need192 §4.6）：首行截断 + 字数 + [查看/编辑] → 640 弹窗
            // multiline；长文本不再内联撑表单。label 红字保留前置带「必填未填」一景。
            const std::string aid = "release.detail.prereq.announce";
            const bool announceChecked = prereqChecked(state, PrereqKey::Announce);
            const bool announceGate = prereqGateOpen(state, PrereqKey::Announce);
            const bool announceFlagged = prereqFlagged(state, PrereqKey::Announce);
            const std::string announceText = state.task.announceText;
            std::string firstLine = announceText;
            const std::size_t newlineAt = announceText.find('\n');
            if (newlineAt != std::string::npos) firstLine = announceText.substr(0, newlineAt);
            const bool announceEmpty = trimmed(announceText).empty();
            const float announceRowH = kStepRowH;
            const float checkW = 72.0f;
            const float viewW = 72.0f;
            const float countW = 56.0f;
            const float summaryX = inset + kFormLabelW + 12.0f;
            const float summaryW = std::max(
                0.0f, width - inset * 2.0f - kFormLabelW - 12.0f - viewW - 8.0f -
                          checkW - 8.0f - countW);
            ui.text(aid + ".label")
                .position(inset, y)
                .size(kFormLabelW, announceRowH)
                .text("上线公告")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(announceRowH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(announceFlagged && !announceChecked ? t.colors.errorText
                                                           : t.colors.textPrimary)
                .build();
            ui.text(aid + ".summary")
                .position(summaryX, y)
                .size(summaryW, announceRowH)
                .text(announceEmpty ? "未填写公告" : firstLine)
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(announceRowH)
                .maxWidth(summaryW)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(announceEmpty ? t.colors.textTertiary : t.colors.textSecondary)
                .build();
            ui.text(aid + ".count")
                .position(summaryX + summaryW + 8.0f, y)
                .size(countW, announceRowH)
                .text(announceEmpty
                          ? ""
                          : std::to_string(utf8CharCount(announceText)) + " 字")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(announceRowH)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            {
                ui_kit::ButtonOptions options;
                options.onClick = [&state] {
                    state.announceEditText = state.task.announceText;
                    state.announceEditorOpen = true;
                };
                ui_kit::button(ui, aid + ".edit", "查看/编辑", ui_kit::ButtonLevel::Text,
                               width - inset - checkW - 8.0f - viewW, y, viewW,
                               announceRowH, t, options);
            }
            ReleaseCheckboxOptions acheck;
            acheck.checked = announceChecked;
            acheck.enabled = announceGate || announceChecked;
            acheck.label = prereqCheckLabel(PrereqKey::Announce);
            acheck.boxSize = kCheckSize;
            acheck.fontSize = t.sizes.fontSizeSmall;
            acheck.width = checkW;
            acheck.height = kCheckSize;
            acheck.inset = 0.0f;
            acheck.gap = 6.0f;
            std::function<void()> aOnCheck = [&state]() {
                if (isReadOnly(state.displayStatus)) return;
                if (prereqChecked(state, PrereqKey::Announce)) {
                    openRevokePrereq(state, "announce", prereqCheckLabel(PrereqKey::Announce),
                                     true);
                    return;
                }
                openCompletionGate(state, "上线公告", "计入前置完成度并解锁提交校验",
                                   "上线公告",
                                   [](ReleaseStore& s, ReleaseDetailUiState& st) {
                                       markAnnouncementSent(s, st);
                                   });
            };
            drawReleaseCheckboxAt(ui, aid + ".check", width - inset - checkW,
                                  y + (announceRowH - kCheckSize) * 0.5f,
                                  acheck, aOnCheck, t);
            y += announceRowH;
        })
        .build();
}

void drawEnvCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const TaskEnv& env, ReleaseDetailUiState& state, bool readOnly,
                 bool configurable, int nowMinute, const Tokens& t,
                 const components::theme::ThemeColorTokens& themeTokens,
                 ReleaseStore& store) {
    const float height = envCardHeight(state, env);
    drawCardBackground(ui, id + ".bg", x, y, width, height, t, themeTokens, false);

    const Progress progress = envProgress(env, state.services, state.configs, state.steps);
    const bool completed = envCompleted(env, state.services, state.configs, state.steps);
    const bool overdue = nowMinute >= 0 && nowMinute > envEndMinuteAdjusted(env) &&
                         !completed;
    const float barW = 120.0f;
    const float right = x + width - kEnvInset;
    const float barX = right - barW;
    const float barY = y + 34.0f;
    const float countW = 64.0f;
    // 「移除环境」入口（环境分组头右上角；Done / Voided 只读时隐藏）。
    // Text 级 + 错误红，点击只开二次确认，不直接写库。
    const float removeW = readOnly ? 0.0f : 64.0f;
    const float removeX = right - removeW;
    // 113-3：「移除环境」是破坏性入口，与左侧进度计数拉开 16px（原 8px 近乎贴死，
    // 破坏性操作与进度混排易误触）。
    const float topRight = right - (removeW > 0.0f ? removeW + 16.0f : 0.0f);
    const float chipTextW = chipWidth("已超时", t.sizes.fontSizeSmall - 1.0f);
    const float chipX = barX - 8.0f - chipTextW;
    if (overdue) {
        drawChip(ui, id + ".overdue", chipX, y + 17.0f, "已超时",
                 eui::mixColor(t.colors.surface, t.colors.error, 0.12f),
                 t.colors.errorText, t);
    }
    if (removeW > 0.0f) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state, envId = env.id, name = env.envName] {
            openRemoveEnv(state, envId, name);
        };
        options.hasTextColorOverride = true;
        options.textColorOverride = t.colors.errorText;
        ui_kit::button(ui, id + ".remove", "移除环境", ui_kit::ButtonLevel::Text,
                       removeX, y + 8.0f, removeW, 24.0f, t, options);
    }
    ui.text(id + ".progress")
        .position(topRight - countW, y + 10.0f)
        .size(countW, core::theme::scaled(16.0f))
        .text(std::to_string(progress.done) + "/" + std::to_string(progress.total))
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(core::theme::scaled(16.0f))
        .horizontalAlign(eui::HorizontalAlign::Right)
        .color(completed ? t.colors.success : t.colors.textSecondary)
        .build();
    ui.stack(id + ".progress.bar")
        .position(barX, barY)
        .size(barW, 4.0f)
        .content([&] {
            components::progress(ui, id + ".progress.fill")
                .size(barW, 4.0f)
                .value(progress.total > 0
                           ? static_cast<float>(progress.done) /
                                 static_cast<float>(progress.total)
                           : 1.0f)
                .theme(themeTokens)
                .build();
        })
        .build();

    const float nameW = std::max(0.0f, (overdue ? chipX : barX) - x - kEnvInset - 8.0f);
    ui.text(id + ".name")
        .position(x + kEnvInset, y + 8.0f)
        .size(nameW, core::theme::scaled(24.0f))
        .text(env.envName)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(core::theme::scaled(24.0f))
        .fontWeight(600)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .build();
    ui.text(id + ".window")
        .position(x + kEnvInset, y + 32.0f)
        .size(nameW, core::theme::scaled(16.0f))
        .text(formatTimeWindow(env.startMinute, env.endMinute))
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(core::theme::scaled(16.0f))
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(overdue ? t.colors.errorText : t.colors.textTertiary)
        .build();

    const std::vector<const TaskService*> services = liveServices(state, env);
    std::vector<float> serviceTops;
    std::vector<float> serviceHeights;
    serviceTops.reserve(services.size());
    serviceHeights.reserve(services.size());
    float serviceY = y + kEnvHeaderH;
    for (std::size_t i = 0; i < services.size(); ++i) {
        const TaskService& svc = *services[i];
        serviceTops.push_back(serviceY);
        const float cardHeight = serviceCardHeight(state, svc);
        serviceHeights.push_back(cardHeight);
        drawServiceCard(ui, id + ".svc." + std::to_string(svc.id),
                        x + kEnvInset, serviceY, width - kEnvInset * 2.0f, env, svc,
                        state, readOnly, configurable, true, nowMinute, t, themeTokens,
                        store);
        serviceY += cardHeight + kServiceGap;
    }

    // 服务拖拽的插入位指示线（卡片间），与被拖卡片的半透明 veil 共同表达落点。
    if (configurable && !services.empty() &&
        state.drag.kind == ReleaseDetailDragKind::ServiceOrder && state.drag.active &&
        state.drag.envId == env.id) {
        const int source = state.drag.sourceIndex;
        const int target = state.drag.targetIndex;
        if (source >= 0 && target >= 0 && source < static_cast<int>(services.size())) {
            const int boundary = target < source ? target : target + 1;
            float lineY = 0.0f;
            if (boundary < static_cast<int>(services.size())) {
                lineY = serviceTops[static_cast<std::size_t>(boundary)] -
                        kServiceGap * 0.5f - kDragLineH * 0.5f;
            } else {
                const std::size_t last = services.size() - 1;
                lineY = serviceTops[last] + serviceHeights[last] + kServiceGap * 0.5f -
                        kDragLineH * 0.5f;
            }
            ui.rect(id + ".drag.insert")
                .position(x + kEnvInset, lineY)
                .size(std::max(0.0f, width - kEnvInset * 2.0f), kDragLineH)
                .color(t.colors.primary)
                .radius(1.0f)
                .build();
        }
    }
}

} // namespace detail
} // namespace release
