// release_detail_draw.cpp
// 从 release_detail_ui.cpp 拆出（2026-10-31）：绘制层——度量、卡片、步骤行、
// 工单卡、环境分区，以及校验 / 配置编辑 / 确认弹窗。只读 state 与 logic 层查询。

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

float measureText(const std::string& text, float fontSize, int weight) {
    return core::TextPrimitive::measureTextWidth(text, {}, fontSize, weight);
}

float chipWidth(const std::string& text, float fontSize) {
    return measureText(text, fontSize, 600) + 20.0f;
}

ChipColors statusChipColors(TaskStatus status, const Tokens& t) {
    switch (status) {
        case TaskStatus::Draft:
            return {t.colors.hover, t.colors.textSecondary};
        case TaskStatus::Pending:
            return {eui::mixColor(t.colors.surface, t.colors.info, 0.12f), t.colors.info};
        case TaskStatus::Running:
            return {eui::mixColor(t.colors.surface, t.colors.warning, 0.14f),
                    t.colors.warning};
        case TaskStatus::Done:
            return {eui::mixColor(t.colors.surface, t.colors.success, 0.12f),
                    t.colors.success};
        case TaskStatus::Voided:
            return {t.colors.hover, t.colors.textDisabled};
    }
    return {t.colors.hover, t.colors.textSecondary};
}

float drawChip(eui::Ui& ui, const std::string& id, float x, float y,
               const std::string& text, const eui::Color& bg, const eui::Color& fg,
               const Tokens& t) {
    const float fontSize = t.sizes.fontSizeSmall - 1.0f;
    const float width = chipWidth(text, fontSize);
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
        .fontSize(fontSize)
        .lineHeight(kChipH)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .build();
    return width;
}

void drawCardBackground(eui::Ui& ui, const std::string& id, float x, float y,
                        float w, float h, const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens,
                        bool subtle) {
    // 卡片统一 12 圆角 + 1px #E5E7EB 描边（视觉契约）；嵌套服务卡只用浅底区分层级，
    // 不改圆角档，避免同页出现 8/12 两套卡圆角。
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(subtle ? t.colors.background : t.colors.surface)
        .border(1.0f, t.colors.borderDefault)
        .radius(t.sizes.radiusLarge)
        .shadow(subtle ? core::Shadow{}
                       : components::theme::shadow(themeTokens, 3.0f, 1.0f, 0.04f, 0.02f))
        .build();
}

void drawSectionTitle(eui::Ui& ui, const std::string& id, float x, float y,
                      const std::string& text, const Tokens& t) {
    ui.text(id)
        .position(x, y)
        .size(600.0f, kSectionTitleH)
        .text(text)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(kSectionTitleH)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
}

// 拖拽手柄（⋮⋮）：透明底 + hover 浅染；命中区与图标分离，图标在下层之后绘制。
// 手柄自身承担 press/drag/release，行点击命中区在其下方，点手柄不会误触行。

void drawDragHandle(eui::Ui& ui, const std::string& id, float x, float y,
                    float width, float height, const Tokens& t,
                    const std::function<void(const eui::PointerEvent&, const eui::Rect&)>&
                        onPress,
                    const std::function<void(const core::dsl::DragEvent&)>& onDrag,
                    const std::function<void(const eui::PointerEvent&,
                                             const eui::Rect&)>& onRelease) {
    eui::Color hover = t.colors.textPrimary;
    hover.a = 0.06f;
    eui::Color pressed = t.colors.textPrimary;
    pressed.a = 0.10f;
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f}, hover, pressed)
        .radius(4.0f)
        .onPress(onPress)
        .onDrag(onDrag)
        .onRelease(onRelease)
        .build();
    ui.text(id + ".icon")
        .position(x, y)
        .size(width, height)
        .text("⋮⋮")
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(height)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
}

void drawStepDot(eui::Ui& ui, const std::string& id, float x, float y,
                 bool done, bool skipped, const Tokens& t) {
    // 步骤状态与勾选框同一套自绘形状：未做空心、已完成 success 实底 + 勾、
    // 跳过灰底 + 横线（行首标签另有划线，在 drawStepRow 内画）。
    drawCheckboxGlyph(ui, id + ".dot", x, y, kStepDot, done, skipped, true, t, true);
}

void drawReadOnlyValue(eui::Ui& ui, const std::string& id, float x, float y,
                       float w, float h, const std::string& value,
                       const std::string& placeholder, bool multiline,
                       const Tokens& t) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.hover)
        .border(1.0f, t.colors.borderDefault)
        .radius(10.0f)
        .build();
    ui.text(id + ".text")
        .position(x + 12.0f, y + (multiline ? 8.0f : 0.0f))
        .size(std::max(0.0f, w - 24.0f), multiline ? std::max(0.0f, h - 16.0f) : h)
        .text(value.empty() ? placeholder : value)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(multiline ? 20.0f : h)
        .maxWidth(std::max(0.0f, w - 24.0f))
        .wrap(multiline)
        .overflow(core::TextOverflow::Clip)
        .verticalAlign(multiline ? eui::VerticalAlign::Top : eui::VerticalAlign::Center)
        .color(value.empty() ? t.colors.textTertiary : t.colors.textSecondary)
        .build();
}

// ==================== 任务级前置卡 ====================

const char* prereqTitle(PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return "提测单";
        case PrereqKey::Cab: return "CAB 单";
        case PrereqKey::Nacos: return "nacos 工单";
        case PrereqKey::Db: return "数据库工单";
        case PrereqKey::Announce: return "上线公告";
    }
    return "前置项";
}

const char* prereqCode(PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return "pretest";
        case PrereqKey::Cab: return "cab";
        case PrereqKey::Nacos: return "nacos";
        case PrereqKey::Db: return "db";
        case PrereqKey::Announce: return "announce";
    }
    return "";
}

const char* prereqCheckLabel(PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest:
        case PrereqKey::Cab: return "已完成";
        case PrereqKey::Nacos:
        case PrereqKey::Db: return "已审批";
        case PrereqKey::Announce: return "已发";
    }
    return "已完成";
}

std::string prereqValue(const ReleaseDetailUiState& state, PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return state.task.pretestNo;
        case PrereqKey::Cab: return state.task.cabNo;
        case PrereqKey::Nacos: return state.task.ticketNacosNo;
        case PrereqKey::Db: return state.task.ticketDbNo;
        case PrereqKey::Announce: return state.task.announceText;
    }
    return {};
}

void setPrereqValue(ReleaseDetailUiState& state, PrereqKey key, const std::string& value) {
    switch (key) {
        case PrereqKey::Pretest:
            state.task.pretestNo = value;
            break;
        case PrereqKey::Cab:
            state.task.cabNo = value;
            break;
        case PrereqKey::Nacos:
            state.task.ticketNacosNo = value;
            break;
        case PrereqKey::Db:
            state.task.ticketDbNo = value;
            break;
        case PrereqKey::Announce:
            state.task.announceText = value;
            break;
    }
}

bool prereqChecked(const ReleaseDetailUiState& state, PrereqKey key) {
    switch (key) {
        case PrereqKey::Pretest: return state.task.pretestOk;
        case PrereqKey::Cab: return state.task.cabOk;
        case PrereqKey::Nacos: return state.task.ticketNacosOk;
        case PrereqKey::Db: return state.task.ticketDbOk;
        case PrereqKey::Announce: return state.task.announceSentAt != 0;
    }
    return false;
}

bool prereqGateOpen(const ReleaseDetailUiState& state, PrereqKey key) {
    if (key == PrereqKey::Announce) return !trimmed(state.task.announceText).empty();
    return !trimmed(prereqValue(state, key)).empty();
}

// ASCII 大小写不敏感子串（中文按字节原样比较）。用于把 dueItems 的条目对回卡片。

bool containsAsciiCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            unsigned char a = static_cast<unsigned char>(haystack[i + j]);
            unsigned char b = static_cast<unsigned char>(needle[j]);
            if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// 前置区（F-12 单据式表单流，编辑态专属）：一行一单据，label 定宽 + 输入 +
// 行尾勾；「填」是主体、「勾」是收尾。单号未填 → 勾禁用 + 提示（既有门禁）。
// 到期提示不逐行常驻（设计 §5.2）：由已完成聚合 / 提醒引擎 / 轨道到期承载。

float prereqSectionHeight(const ReleaseDetailUiState& state) {
    const TicketNeeds needs = scanTicketNeeds(state);
    const int docRows = 2 + (needs.nacos ? 1 : 0) + (needs.db ? 1 : 0);
    const float rowH = kStepRowH;
    const float inner = 16.0f + 18.0f + 8.0f +
                        static_cast<float>(docRows) * (rowH + 8.0f) - 8.0f +
                        16.0f + 18.0f + 8.0f + 88.0f + 8.0f + 22.0f + 16.0f;
    return kSectionTitleH + kSectionGap + inner;
}

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
                .size(width - inset * 2.0f, 18.0f)
                .text("审批单据")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(18.0f)
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
                    ui.text(id + ".gate")
                        .position(inputX, y)
                        .size(std::max(0.0f, inputW - 120.0f), rowH)
                        .text("请先填写单据号")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(rowH)
                        .horizontalAlign(eui::HorizontalAlign::Right)
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
                onCheck = [&store, &state, key, code, label]() {
                    if (isReadOnly(state.displayStatus)) return;
                    if (prereqChecked(state, key)) {
                        openRevokePrereq(state, code, label, false);
                        return;
                    }
                    markPrereqCompleted(store, state, code, prereqTitle(key));
                };
                drawReleaseCheckboxAt(ui, id + ".check",
                                      width - inset - checkW, y + (rowH - kCheckSize) * 0.5f,
                                      check, onCheck, t);
                y += rowH + 8.0f;
            }

            y += 8.0f;
            ui.text("release.detail.prereq.group.announce")
                .position(inset, y)
                .size(width - inset * 2.0f, 18.0f)
                .text("上线公告")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();
            y += 18.0f + 8.0f;
            const std::string aid = "release.detail.prereq.announce";
            const bool announceChecked = prereqChecked(state, PrereqKey::Announce);
            const bool announceGate = prereqGateOpen(state, PrereqKey::Announce);
            const bool announceFlagged = prereqFlagged(state, PrereqKey::Announce);
            components::input(ui, aid + ".input")
                .position(inset, y)
                .size(width - inset * 2.0f - 84.0f, 88.0f)
                .value(state.task.announceText)
                .placeholder("填写上线公告")
                .fontSize(t.sizes.fontSizeBody)
                .multiline(true)
                .scrollbar(true)
                .inset(8.0f)
                .theme(themeTokens)
                .onChange([&state](const std::string& value) {
                    setPrereqValue(state, PrereqKey::Announce, value);
                })
                .onFocus([&store, &state](bool focused) {
                    if (!focused) saveTask(store, state);
                })
                .onEnter([&store, &state]() { saveTask(store, state); })
                .onCancel([&store, &state]() { saveTask(store, state); })
                .build();
            ui.text(aid + ".count")
                .position(inset, y + 88.0f + 8.0f)
                .size(200.0f, 22.0f)
                .text("共 " + std::to_string(utf8CharCount(state.task.announceText)) +
                      " 字")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(22.0f)
                .color(announceFlagged && !announceChecked ? t.colors.errorText
                                                           : t.colors.textTertiary)
                .build();
            ReleaseCheckboxOptions acheck;
            acheck.checked = announceChecked;
            acheck.enabled = announceGate || announceChecked;
            acheck.label = prereqCheckLabel(PrereqKey::Announce);
            acheck.boxSize = kCheckSize;
            acheck.fontSize = t.sizes.fontSizeSmall;
            acheck.width = 72.0f;
            acheck.height = kCheckSize;
            acheck.inset = 0.0f;
            acheck.gap = 6.0f;
            std::function<void()> aOnCheck = [&store, &state]() {
                if (isReadOnly(state.displayStatus)) return;
                if (prereqChecked(state, PrereqKey::Announce)) {
                    openRevokePrereq(state, "announce", prereqCheckLabel(PrereqKey::Announce),
                                     true);
                    return;
                }
                markAnnouncementSent(store, state);
            };
            drawReleaseCheckboxAt(ui, aid + ".check", width - inset - 72.0f,
                                  y + 88.0f + 8.0f + (22.0f - kCheckSize) * 0.5f,
                                  acheck, aOnCheck, t);
        })
        .build();
}

// ==================== 环境 / 服务 ====================

float serviceCardHeight(const ReleaseDetailUiState& state, const TaskService& svc) {
    const float rows = static_cast<float>(3 + orderedConfigs(state, svc).size());
    // 步骤行「···」次行展开时占一行（键前缀 = 服务 id，见 drawServiceCard）。
    const std::string prefix = std::to_string(svc.id) + ":";
    const float menuExtra = state.openMenuRowKey.rfind(prefix, 0) == 0
                                ? kStepRowH + kStepRowGap
                                : 0.0f;
    // 池 54（F-54-1）：编辑态分支行从 16px 只读行升级为 kStepRowH 表单行（+28）；
    // 发布/归档态维持只读行高度。
    const float branchExtra = isEditMode(state.displayStatus) ? kStepRowH - 16.0f : 0.0f;
    return 68.0f + branchExtra + rows * kStepRowH + (rows - 1.0f) * kStepRowGap + menuExtra;
}

float envCardHeight(const ReleaseDetailUiState& state, const TaskEnv& env) {
    const std::vector<const TaskService*> services = liveServices(state, env);
    float height = kEnvHeaderH + kEnvInset;
    for (std::size_t i = 0; i < services.size(); ++i) {
        height += serviceCardHeight(state, *services[i]);
        if (i + 1 < services.size()) height += kServiceGap;
    }
    return height;
}

float envSectionHeight(const ReleaseDetailUiState& state, float width) {
    (void)width;
    if (state.envs.empty()) return kSectionTitleH + kSectionGap + 48.0f;
    float height = kSectionTitleH + kSectionGap;
    for (std::size_t i = 0; i < state.envs.size(); ++i) {
        height += envCardHeight(state, state.envs[i]);
        if (i + 1 < state.envs.size()) height += kPrereqGap;
    }
    return height;
}

// 步骤行（F-13 四要素）：状态点 + 名称 + 状态字 + 完成（整行点击）；行尾只留
// 「填写/查看」（配置行）与「···」（编辑态配置行，上移/下移/跳过收进次行，
// 由 drawServiceCard 在本行下方绘制）。原 40px 六类控件布局退役。

void drawStepRow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const StepRowView& row, bool current, bool readOnly,
                 bool configurable, bool readOnlyViewAllowed, bool menuOpen,
                 const Tokens& t,
                 const std::function<void()>& onRowClick,
                 const std::function<void()>& onView,
                 const std::function<void()>& onToggleMenu) {
    (void)menuOpen;  // 次行由调用方绘制；此参仅供命中避让扩展
    const float rowX = x + 8.0f;
    const float rowW = std::max(0.0f, width - 16.0f);
    if (current) {
        ui.rect(id + ".current")
            .position(rowX, y - 1.0f)
            .size(rowW, kStepRowH + 2.0f)
            .color(eui::mixColor(t.colors.surface, t.colors.primary, 0.08f))
            .border(1.5f, t.colors.primary)
            .radius(t.sizes.radius)
            .build();
    }
    if (row.invalid) {
        ui.rect(id + ".invalid")
            .position(rowX, y)
            .size(rowW, kStepRowH)
            .color(eui::mixColor(t.colors.surface, t.colors.error, 0.10f))
            .radius(t.sizes.radius)
            .build();
    }
    if (row.dragging) {
        eui::Color veil = t.colors.surface;
        veil.a = 0.55f;
        ui.rect(id + ".drag.veil")
            .position(rowX, y)
            .size(rowW, kStepRowH)
            .color(veil)
            .radius(t.sizes.radius)
            .build();
    }
    if (!readOnly && !row.skipped && onRowClick) {
        ui.rect(id + ".hit")
            .position(rowX, y)
            .size(rowW, kStepRowH)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(onRowClick)
            .build();
    }

    const float dotX = rowX + 10.0f;
    const float dotY = y + (kStepRowH - kStepDot) * 0.5f;
    drawStepDot(ui, id + ".status", dotX, dotY, row.done, row.skipped, t);

    // 行尾操作区（右缘向左排）：···(28) + 填写/查看(56)。
    const bool showView = row.config && onView != nullptr;
    const bool showMore = row.config && configurable && !row.done;  // 跳过行保留入口（取消跳过）
    const float moreW = showMore ? 28.0f : 0.0f;
    const float viewX = showView ? rowX + rowW - 8.0f - kViewButtonW -
                                       (showMore ? moreW + 4.0f : 0.0f)
                                 : rowX + rowW;
    // 可排序的配置行在最左侧预留拖拽手柄位（⋮⋮）；手柄由 drawServiceCard 叠加绘制。
    const bool showDragHandle = row.config && configurable;
    const float nameX = dotX + kStepDot +
                        (showDragHandle ? kDragHandleW + kDragHandleGap : 10.0f);
    const float nameRight = viewX - (showView ? 8.0f : 0.0f) - moreW;
    const float statusW = 88.0f;
    const float nameW = std::max(0.0f, nameRight - statusW - 8.0f - nameX);

    eui::Color labelColor = row.done ? t.colors.textPrimary
                             : (row.skipped ? t.colors.textDisabled
                                            : (current ? t.colors.textPrimary
                                                       : t.colors.textSecondary));
    if (row.invalid) labelColor = t.colors.errorText;
    ui.text(id + ".label")
        .position(nameX, y)
        .size(nameW, kStepRowH)
        .text(row.label)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(kStepRowH)
        .maxWidth(nameW)
        .wrap(false)
        .overflow(core::TextOverflow::Ellipsis)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(labelColor)
        .build();
    if (row.skipped) {
        const float labelW =
            std::min(nameW, measureText(row.label, t.sizes.fontSizeBody, 400));
        ui.rect(id + ".strike")
            .position(nameX, y + kStepRowH * 0.5f)
            .size(labelW, 1.0f)
            .color(t.colors.textDisabled)
            .build();
    }
    // 状态字：未完成 / 已填 n 字 / 已跳过 / 内容为空 / 已完成（StepRowView 组装）。
    const float statusX = nameRight - statusW;
    ui.text(id + ".state")
        .position(statusX, y)
        .size(statusW, kStepRowH)
        .text(row.invalid ? "内容为空" : row.statusText)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(kStepRowH)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(row.invalid ? t.colors.errorText : t.colors.textTertiary)
        .build();

    if (showView) {
        ui_kit::ButtonOptions options;
        options.onClick = onView;
        ui_kit::button(ui, id + ".view",
                       readOnly ? "查看" : "填写", ui_kit::ButtonLevel::Text, viewX, y,
                       kViewButtonW, kStepRowH, t, options);
    }
    if (showMore && onToggleMenu) {
        ui_kit::ButtonOptions options;
        options.onClick = onToggleMenu;
        ui_kit::button(ui, id + ".more", "···", ui_kit::ButtonLevel::Text,
                       rowX + rowW - 8.0f - moreW, y, moreW, kStepRowH, t, options);
    }
}

void drawServiceCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
                     const TaskEnv& env, const TaskService& svc,
                     ReleaseDetailUiState& state, bool readOnly, bool configurable,
                     int nowMinute, const Tokens& t,
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
    // 服务顺序拖拽只在配置中 / 待执行可用；单服务没有排序意义，不画手柄。
    const bool showOrderHandle = configurable && countLiveServices(state, env) > 1;
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
        .size(nameW, 20.0f)
        .text(svc.serviceName)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .fontWeight(600)
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
            .color(branchFlagged ? t.colors.errorText : t.colors.textPrimary)
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
        const std::string menuKey = std::to_string(svc.id) + ":" + row.id;
        const bool menuOpen = state.openMenuRowKey == menuKey;

        std::function<void()> onRowClick;
        if (!row.done) {
            if (row.config) {
                onRowClick = [&store, &state, serviceId = svc.id, kind = row.configKind,
                              timeReady = row.timeReady, envCopy = env,
                              envName = env.envName, nowMinute] {
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
                    markConfigCompleted(store, state, serviceId, kind, envCopy, nowMinute);
                };
            } else {
                onRowClick = [&store, &state, serviceId = svc.id, kind = row.stepKind,
                              timeReady = row.timeReady, envCopy = env,
                              envName = env.envName, nowMinute] {
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
                    markStepCompleted(store, state, serviceId, kind, envCopy, nowMinute);
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

        std::function<void()> onToggleMenu;
        std::function<void()> onMoveUp;
        std::function<void()> onMoveDown;
        std::function<void()> onSkipToggle;
        if (row.config && !readOnly) {
            onToggleMenu = [&state, menuKey] {
                state.openMenuRowKey = state.openMenuRowKey == menuKey
                                           ? std::string()
                                           : menuKey;
            };
            onMoveUp = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                moveConfig(store, state, serviceId, kind, -1);
            };
            onMoveDown = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                moveConfig(store, state, serviceId, kind, 1);
            };
            onSkipToggle = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                toggleConfigSkip(store, state, serviceId, kind);
            };
        }
        drawStepRow(ui, rowId, x, rowY, width, row, i == currentIndex, readOnly,
                    configurable, true, menuOpen, t, onRowClickWithRevoke, onView,
                    onToggleMenu);
        rowY += kStepRowH + kStepRowGap;

        // 「···」次行：上移 / 下移 / 跳过（编辑态配置行收纳处，F-13）。
        if (menuOpen) {
            const float menuX = x + 8.0f;
            const float menuW = std::max(0.0f, width - 16.0f);
            ui.rect(rowId + ".menu.bg")
                .position(menuX, rowY)
                .size(menuW, kStepRowH)
                .color(t.colors.hover)
                .radius(t.sizes.radius)
                .build();
            float btnX = menuX + 8.0f;
            const auto menuBtn = [&](const std::string& btnId, const std::string& text,
                                     const std::function<void()>& onClick,
                                     bool danger = false) {
                ui_kit::ButtonOptions options;
                options.onClick = onClick;
                if (danger) {
                    options.hasTextColorOverride = true;
                    options.textColorOverride = t.colors.errorText;
                }
                ui_kit::button(ui, rowId + ".menu." + btnId, text,
                               ui_kit::ButtonLevel::Text, btnX,
                               rowY + (kStepRowH - 24.0f) * 0.5f, 72.0f, 24.0f, t,
                               options);
                btnX += 80.0f;
            };
            menuBtn("up", "上移", onMoveUp);
            menuBtn("down", "下移", onMoveDown);
            menuBtn("skip", row.skipped ? "取消跳过" : "跳过此项", onSkipToggle, true);
            rowY += kStepRowH + kStepRowGap;
        }

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
    const float topRight = right - (removeW > 0.0f ? removeW + 8.0f : 0.0f);
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
        .size(countW, 16.0f)
        .text(std::to_string(progress.done) + "/" + std::to_string(progress.total))
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(16.0f)
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
        .size(nameW, 24.0f)
        .text(env.envName)
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(24.0f)
        .fontWeight(600)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .build();
    ui.text(id + ".window")
        .position(x + kEnvInset, y + 32.0f)
        .size(nameW, 16.0f)
        .text(formatTimeWindow(env.startMinute, env.endMinute))
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
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
                        state, readOnly, configurable, nowMinute, t, themeTokens, store);
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

void drawEnvSection(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                    bool readOnly, bool configurable, int nowMinute, const Tokens& t,
                    const components::theme::ThemeColorTokens& themeTokens,
                    ReleaseStore& store, const ReleaseDetailActions& actions) {
    const float sectionH = envSectionHeight(state, width);
    ui.stack("release.detail.env.section")
        .size(width, sectionH)
        .content([&] {
            drawSectionTitle(ui, "release.detail.env.title", 0.0f, 0.0f, "环境与服务", t);
            if (state.envs.empty()) {
                ui.text("release.detail.env.empty")
                    .position(0.0f, kSectionTitleH + kSectionGap)
                    .size(width, 48.0f)
                    .text("任务还没有环境")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(48.0f)
                    .color(t.colors.textTertiary)
                    .build();
                if (actions.onOpenConfig) {
                    ui_kit::ButtonOptions options;
                    options.onClick = actions.onOpenConfig;
                    options.hasTextColorOverride = true;
                    options.textColorOverride = t.colors.primaryText;
                    ui_kit::button(ui, "release.detail.env.openconfig", "去基础配置",
                                   ui_kit::ButtonLevel::Text, 0.0f,
                                   kSectionTitleH + kSectionGap + 44.0f, 88.0f, 28.0f,
                                   t, options);
                }
                return;
            }
            float envY = kSectionTitleH + kSectionGap;
            for (const auto& env : state.envs) {
                drawEnvCard(ui, "release.detail.env." + std::to_string(env.id), 0.0f,
                            envY, width, env, state, readOnly, configurable, nowMinute, t,
                            themeTokens, store);
                envY += envCardHeight(state, env) + kPrereqGap;
            }
        })
        .build();
}

// ==================== 页头 / 弹层 ====================

// 页头（F-11 三分区）：身份区（返回 / 标题承载任务身份 / 模式徽标 / 状态 chip）+
// 反馈进度（发布态迷你进度条）+ 操作区（**每状态主动作唯一**，次级动作收
// 「···」菜单）。文案即软引导（F-04）：「下一步：{当前步名}」。

namespace {

std::string planDateShortLabel(const std::string& planDate) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(planDate.c_str(), "%d-%d-%d", &year, &month, &day) == 3 &&
        month >= 1 && month <= 12 && day >= 1 && day <= 31) {
        return std::to_string(month) + "/" + std::to_string(day);
    }
    return planDate;
}

} // namespace

void drawPageHeader(eui::Ui& ui, float width, const Tokens& t,
                    ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute,
                    const ReleaseDetailActions& actions) {
    const float headerH = ui_kit::kHeaderH;
    const TaskStatus status = state.displayStatus;
    const bool readOnly = isReadOnly(status);
    const bool publish = isPublishMode(status);

    // —— 身份区（左，弹性）——
    const auto backFn = actions.onBack;
    float x = 0.0f;
    if (backFn) {
        ui_kit::ButtonOptions backOptions;
        backOptions.onClick = backFn;
        ui_kit::button(ui, "release.detail.back", "←", ui_kit::ButtonLevel::Text, 0.0f,
                       0.0f, 40.0f, headerH, t, backOptions);
        x = 40.0f + 8.0f;
    }
    const std::string titleText = "上线任务 " + planDateShortLabel(state.task.planDate);
    const float titleW = measureText(titleText, t.sizes.fontSizeTitle, 700) + 8.0f;
    ui.text("release.detail.title")
        .position(x, 0.0f)
        .size(titleW, headerH)
        .text(titleText)
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(headerH)
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    x += titleW;
    const std::string weekday = releaseWeekdayLabel(state.task.planDate);
    if (!weekday.empty()) {
        const float weekW = measureText(weekday, t.sizes.fontSizeBody, 400) + 8.0f;
        ui.text("release.detail.weekday")
            .position(x, 0.0f)
            .size(weekW, headerH)
            .text(weekday)
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(headerH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        x += weekW;
    }
    const char* modeKey = "edit";
    if (status == TaskStatus::Voided) modeKey = "voided";
    else if (status == TaskStatus::Done) modeKey = "archive";
    else if (publish) modeKey = "publish";
    const float badgeW = drawModeBadge(ui, std::string("release.detail.modebadge.") +
                                            modeKey,
                                       x, (headerH - kChipH) * 0.5f, status, t);
    x += badgeW + 8.0f;
    const ChipColors colors = statusChipColors(status, t);
    const std::string statusText = releaseTaskStatusText(status);
    const float chipW = chipWidth(statusText, t.sizes.fontSizeSmall - 1.0f);
    drawChip(ui, "release.detail.status", x, (headerH - kChipH) * 0.5f, statusText,
             colors.bg, colors.fg, t);
    x += chipW + 12.0f;
    ui.text("release.detail.title.budget")
        .position(x, 0.0f)
        .size(std::max(0.0f, width * 0.5f - x), headerH)
        .text("")
        .fontSize(t.sizes.fontSizeSmall)
        .build();

    // —— 操作区（右，定宽收缩）。主按钮文案即「下一步」指向（软引导）。——
    std::string nextLabel;
    if (publish) {
        const std::vector<TrackRowView> rows = collectTrackRows(state, nowMinute);
        for (const auto& row : rows) {
            if (row.kind == TrackRowKind::Step && row.node == TrackNodeState::Current) {
                nextLabel = "下一步：" + row.label;
                break;
            }
        }
        if (nextLabel.empty()) nextLabel = "下一步";
    }
    float right = width;
    const auto placeButton = [&](const std::string& id, const std::string& text,
                                 ui_kit::ButtonLevel level, float w,
                                 const std::function<void()>& onClick) {
        right -= w;
        ui_kit::ButtonOptions options;
        options.onClick = onClick;
        ui_kit::button(ui, id, text, level, right, 0.0f, w, headerH, t, options);
        right -= 8.0f;
    };
    if (status == TaskStatus::Voided) {
        placeButton("release.detail.restore", "恢复", ui_kit::ButtonLevel::Secondary,
                    80.0f, [&state] { state.confirm = ReleaseDetailConfirm::RestoreTask; });
    } else if (status == TaskStatus::Done) {
        const std::string doneText = "已完成";
        const float doneW = measureText(doneText, t.sizes.fontSizeSmall, 400) + 8.0f;
        right -= doneW;
        ui.text("release.detail.done")
            .position(right, 0.0f)
            .size(doneW, headerH)
            .text(doneText)
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(headerH)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.success)
            .build();
        right -= 8.0f;
    } else {
        if (publish) {
            const float nextW =
                std::clamp(measureText(nextLabel, t.sizes.fontSizeBody, 400) + 28.0f,
                           120.0f, 220.0f);
            placeButton("release.detail.next", nextLabel, ui_kit::ButtonLevel::Primary,
                        nextW, [&store, &state, nowMinute] {
                            performNextStep(store, state, nowMinute);
                        });
            if (status == TaskStatus::Pending) {
                placeButton("release.detail.withdraw", "撤回提交",
                            ui_kit::ButtonLevel::Secondary, 88.0f,
                            [&state] { state.confirm = ReleaseDetailConfirm::WithdrawSubmission; });
            }
        } else {
            placeButton("release.detail.submit", "提交发布",
                        ui_kit::ButtonLevel::Primary, 100.0f,
                        [&store, &state] { submitTask(store, state); });
            placeButton("release.detail.validate", "校验",
                        ui_kit::ButtonLevel::Secondary, 72.0f,
                        [&state] { runValidation(state); });
        }
        placeButton("release.detail.menu", "···", ui_kit::ButtonLevel::Text, 40.0f,
                    [&state] { state.menuOpen = !state.menuOpen; });
    }

    // —— 反馈进度（发布态 / 归档态：迷你进度条 + n/n；编辑态不显示）——
    if (publish || readOnly) {
        const Progress progress = taskProgress(state.task, state.envs, state.services,
                                               state.configs, state.steps);
        const std::string progressText =
            std::to_string(progress.done) + "/" + std::to_string(progress.total);
        const float countW = measureText(progressText, t.sizes.fontSizeSmall, 400) + 8.0f;
        right -= countW + 16.0f;
        ui.text("release.detail.progress")
            .position(right, 0.0f)
            .size(countW, headerH)
            .text(progressText)
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(headerH)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        const float barW = 80.0f;
        right -= barW + 4.0f;
        const auto headerComponentTokens = core::theme::componentTokensWeChat(t);
        ui.stack("release.detail.progress.bar")
            .position(right, (headerH - 4.0f) * 0.5f)
            .size(barW, 4.0f)
            .content([&] {
                ui.rect("release.detail.progress.track")
                    .size(barW, 4.0f)
                    .color(t.colors.borderDefault)
                    .radius(2.0f)
                    .build();
                components::progress(ui, "release.detail.progress.fill")
                    .size(barW, 4.0f)
                    .value(progress.total > 0
                               ? static_cast<float>(progress.done) /
                                     static_cast<float>(progress.total)
                               : 1.0f)
                    .theme(headerComponentTokens)
                    .build();
            })
            .build();
    }

    // 「···」菜单浮层（页头固定壳坐标系内，绝对定位）。
    if (state.menuOpen && (publish || status == TaskStatus::Draft)) {
        const float menuW = 148.0f;
        const float itemH = 32.0f;
        const bool draft = status == TaskStatus::Draft;
        const int itemCount = draft ? 1 : 2;
        const float panelH = static_cast<float>(itemCount) * itemH + 8.0f;
        const float panelX = width - menuW - 8.0f;
        const float panelY = headerH + 4.0f;
        ui.rect("release.detail.menu.panel")
            .position(panelX, panelY)
            .size(menuW, panelH)
            .color(t.colors.surface)
            .border(1.0f, t.colors.borderDefault)
            .radius(6.0f)
            .shadow(8.0f, 2.0f, eui::Color{0.0f, 0.0f, 0.0f, 0.10f})
            .build();
        float itemY = panelY + 4.0f;
        const auto menuItem = [&](const std::string& text, bool danger,
                                  const std::function<void()>& onClick) {
            ui_kit::ButtonOptions options;
            options.onClick = [onClick, &state] {
                state.menuOpen = false;
                onClick();
            };
            if (danger) {
                options.hasTextColorOverride = true;
                options.textColorOverride = t.colors.errorText;
            }
            ui_kit::button(ui, "release.detail.menu.item." + text, text,
                           ui_kit::ButtonLevel::Text, panelX + 4.0f, itemY,
                           menuW - 8.0f, itemH, t, options);
            itemY += itemH;
        };
        if (!draft) {
            menuItem("校验", false, [&state] { runValidation(state); });
        }
        menuItem("作废任务", true,
                 [&state] { state.confirm = ReleaseDetailConfirm::VoidTask; });
    }
}

// 模式徽标（F-11 几何断言值）：发布中 = 页头唯一绿染（primarySoftBg），
// 其余中性灰；已作废字降级。

float drawModeBadge(eui::Ui& ui, const std::string& id, float x, float y,
                    TaskStatus status, const Tokens& t) {
    const char* label = "准备中";
    unsigned int icon = 0xF044u;  // fa-edit 铅笔
    eui::Color bg = t.colors.hover;
    eui::Color fg = t.colors.textSecondary;
    if (status == TaskStatus::Pending || status == TaskStatus::Running) {
        label = "发布中";
        icon = 0xF024u;  // fa-flag 旗帜
        bg = t.colors.primarySoftBg;
        fg = t.colors.primaryText;
    } else if (status == TaskStatus::Done) {
        label = "已归档";
        icon = 0xF00Cu;  // fa-check 对勾
    } else if (status == TaskStatus::Voided) {
        label = "已作废";
        icon = 0xF05Eu;  // fa-ban 圆内斜杠
        bg = t.colors.hover;
        fg = t.colors.textDisabled;
    }
    const float iconW = 16.0f;
    const float textW = measureText(label, t.sizes.fontSizeSmall - 1.0f, 600);
    const float width = iconW + 6.0f + textW + 16.0f;
    ui.rect(id)
        .position(x, y)
        .size(width, kChipH)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".icon")
        .position(x + 8.0f, y)
        .size(iconW, kChipH)
        .icon(icon)
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(kChipH)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .build();
    ui.text(id + ".text")
        .position(x + 8.0f + iconW + 6.0f, y)
        .size(textW + 4.0f, kChipH)
        .text(label)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(kChipH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .build();
    return width;
}

// 发布态流程轨道（F-14）：前置 + 各环境步骤按 collectTrackRows 固定序铺行；
// 节点四态（当前步唯一绿染 / 可提前勾 / 锁定可见 / 已完成），完成组默认收起。
// 行高与 F-23 滚动偏移共用 kTrackRowH 档（collectTrackRows 注释契约）。

void drawPublishTrack(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                      int nowMinute, const Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens,
                      ReleaseStore& store) {
    const std::vector<TrackRowView> rows = collectTrackRows(state, nowMinute);
    float totalH = 0.0f;
    for (const auto& row : rows) {
        if (!row.hidden) totalH += row.height + kTrackRowGap;
    }
    totalH = std::max(totalH, kSectionTitleH + 48.0f);

    ui.stack("release.detail.track")
        .size(width, totalH)
        .content([&] {
            float y = 0.0f;
            std::string lastGroup;
            for (const auto& row : rows) {
                if (row.hidden) continue;
                const std::string base = "release.detail.track." + std::to_string(&row - rows.data());
                if (row.kind == TrackRowKind::EnvGroupHeader) {
                    // 组头：展开态标题 + 时间窗 + 超时 chip + 进度 + 折叠箭头。
                    ui.rect(base + ".hit")
                        .position(0.0f, y)
                        .size(width, row.height)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .onClick([&state, groupId = row.groupId] {
                            const bool wasCollapsed =
                                state.trackGroupState.count(groupId)
                                    ? state.trackGroupState.at(groupId)
                                    : false;
                            state.trackGroupState[groupId] = !wasCollapsed;
                        })
                        .build();
                    ui.text(base + ".title")
                        .position(0.0f, y)
                        .size(width * 0.4f, row.height)
                        .text(row.title)
                        .fontSize(t.sizes.fontSizeLarge)
                        .lineHeight(row.height)
                        .fontWeight(600)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textPrimary)
                        .build();
                    float hx = measureText(row.title, t.sizes.fontSizeLarge, 600) + 12.0f;
                    const TaskEnv* env = row.envId != 0 ? findEnv(state, row.envId) : nullptr;
                    if (env != nullptr) {
                        const std::string windowText = formatTimeWindow(env->startMinute, env->endMinute);
                        const float wW = measureText(windowText, t.sizes.fontSizeSmall - 1.0f, 400) + 8.0f;
                        ui.text(base + ".window")
                            .position(hx, y)
                            .size(wW, row.height)
                            .text(windowText)
                            .fontSize(t.sizes.fontSizeSmall - 1.0f)
                            .lineHeight(row.height)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        hx += wW;
                    }
                    if (row.groupOverdue) {
                        drawChip(ui, base + ".overdue", hx, y + (row.height - kChipH) * 0.5f,
                                 "已超时", eui::mixColor(t.colors.surface, t.colors.error, 0.12f),
                                 t.colors.errorText, t);
                    }
                    ui.text(base + ".progress")
                        .position(width - 60.0f, y)
                        .size(52.0f, row.height)
                        .text(row.progressText)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(row.height)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(row.groupDone ? t.colors.success : t.colors.textSecondary)
                        .build();
                    lastGroup = row.groupId;
                    y += row.height + kTrackRowGap;
                    continue;
                }
                if (row.kind == TrackRowKind::ServiceLabel) {
                    ui.text(base + ".title")
                        .position(28.0f, y)
                        .size(std::max(0.0f, width - 28.0f), row.height)
                        .text(row.title)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(row.height)
                        .color(t.colors.textTertiary)
                        .build();
                    y += row.height + kTrackRowGap;
                    continue;
                }

                // 步骤行：左缘连线 + 节点 + 名称/值 + 行尾状态/确认。
                const bool inGroup = row.groupId == lastGroup || row.groupId == "prereq";
                if (inGroup) {
                    ui.rect(base + ".line")
                        .position(18.0f, y - kTrackRowGap)
                        .size(2.0f, row.height + kTrackRowGap * 2.0f)
                        .color(row.node == TrackNodeState::Done
                                   ? t.colors.primary
                                   : t.colors.borderDefault)
                        .build();
                }
                if (row.node == TrackNodeState::Current) {
                    ui.rect(base + ".current.bg")
                        .position(0.0f, y)
                        .size(width, row.height)
                        .color(t.colors.primarySoftBg)
                        .radius(t.sizes.radius)
                        .build();
                    ui.rect(base + ".current.bar")
                        .position(0.0f, y)
                        .size(3.0f, row.height)
                        .color(t.colors.primary)
                        .radius(1.5f)
                        .build();
                }
                // 节点圆 14px（x=12）。
                const float nodeX = 12.0f;
                const float nodeY = y + (row.height - 14.0f) * 0.5f;
                const float labelX = 36.0f;
                if (row.node == TrackNodeState::Done) {
                    ui.rect(base + ".node")
                        .position(nodeX, nodeY)
                        .size(14.0f, 14.0f)
                        .color(t.colors.primary)
                        .radius(7.0f)
                        .build();
                    ui.text(base + ".node.check")
                        .position(nodeX, nodeY)
                        .size(14.0f, 14.0f)
                        .text("✓")
                        .fontSize(core::theme::scaled(9.0f))
                        .lineHeight(14.0f)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.surface)
                        .build();
                } else if (row.node == TrackNodeState::Skipped) {
                    ui.rect(base + ".node")
                        .position(nodeX, nodeY)
                        .size(14.0f, 14.0f)
                        .color(t.colors.hover)
                        .radius(7.0f)
                        .build();
                } else {
                    ui.rect(base + ".node")
                        .position(nodeX, nodeY)
                        .size(14.0f, 14.0f)
                        .color(row.node == TrackNodeState::Current ? t.colors.surface
                                                                   : t.colors.surface)
                        .border(row.node == TrackNodeState::Current ? 2.0f : 1.5f,
                                row.node == TrackNodeState::Current
                                    ? t.colors.primary
                                    : (row.node == TrackNodeState::Locked
                                           ? t.colors.borderDefault
                                           : t.colors.textTertiary))
                        .radius(7.0f)
                        .build();
                    if (row.node == TrackNodeState::Current) {
                        ui.rect(base + ".node.dot")
                            .position(nodeX + 4.0f, nodeY + 4.0f)
                            .size(6.0f, 6.0f)
                            .color(t.colors.primary)
                            .radius(3.0f)
                            .build();
                    }
                }
                // 名称 + 只读值。
                float tx = labelX;
                const float nameW = std::max(
                    0.0f, measureText(row.label, t.sizes.fontSizeBody, 400) + 8.0f);
                ui.text(base + ".label")
                    .position(tx, y)
                    .size(nameW, row.height)
                    .text(row.label)
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(row.height)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(row.node == TrackNodeState::Locked ? t.colors.textTertiary
                                                              : t.colors.textPrimary)
                    .build();
                tx += nameW;
                if (!row.valueText.empty()) {
                    const float valueTop = width - 150.0f;
                    ui.text(base + ".value")
                        .position(tx, y)
                        .size(std::max(0.0f, valueTop - tx), row.height)
                        .text(row.valueText)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(row.height)
                        .maxWidth(std::max(0.0f, valueTop - tx))
                        .overflow(core::TextOverflow::Ellipsis)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textSecondary)
                        .build();
                }
                // 行尾：确认钮 / 锁定提示 / 状态字。
                if (row.confirmable) {
                    const TaskEnv* env = findEnv(state, row.envId);
                    ui_kit::ButtonOptions options;
                    if (row.isAnnounce) {
                        options.onClick = [&store, &state] {
                            markAnnouncementSent(store, state);
                        };
                    } else if (row.serviceId == 0) {
                        options.onClick = [&store, &state, key = row.prereqKey] {
                            markPrereqCompleted(store, state, prereqCode(key),
                                                prereqTitle(key));
                        };
                    } else if (row.config && env != nullptr) {
                        options.onClick = [&store, &state, serviceId = row.serviceId,
                                           kind = row.configKind, envCopy = *env,
                                           nowMinute] {
                            markConfigCompleted(store, state, serviceId, kind, envCopy,
                                                nowMinute);
                        };
                    } else if (env != nullptr) {
                        options.onClick = [&store, &state, serviceId = row.serviceId,
                                           kind = row.stepKind, envCopy = *env,
                                           nowMinute] {
                            markStepCompleted(store, state, serviceId, kind, envCopy,
                                              nowMinute);
                        };
                    }
                    if (options.onClick) {
                        ui_kit::button(ui, base + ".confirm", "确认",
                                       ui_kit::ButtonLevel::Text, width - 8.0f - 56.0f,
                                       y, 56.0f, row.height, t, options);
                    }
                } else if (!row.statusText.empty()) {
                    const bool locked = row.node == TrackNodeState::Locked;
                    const float iconW = locked ? 16.0f : 0.0f;
                    if (locked) {
                        ui.text(base + ".lock")
                            .position(width - 8.0f - 16.0f -
                                          (measureText(row.statusText, t.sizes.fontSizeSmall - 1.0f, 400) + 8.0f) - 4.0f,
                                      y)
                            .size(16.0f, row.height)
                            .icon(0xF023u)
                            .fontSize(core::theme::scaled(10.0f))
                            .lineHeight(row.height)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                    }
                    ui.text(base + ".state")
                        .position(width - 8.0f - iconW - 4.0f -
                                      (measureText(row.statusText, t.sizes.fontSizeSmall - 1.0f, 400) + 8.0f),
                                  y)
                        .size(measureText(row.statusText, t.sizes.fontSizeSmall - 1.0f, 400) + 8.0f,
                              row.height)
                        .text(row.statusText)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(row.height)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                }
                // 整行命中：确认（可勾）或撤回确认（已完成，Done 前任意步）。
                if (row.confirmable || row.revocable) {
                    ui.rect(base + ".hit")
                        .position(0.0f, y)
                        .size(width, row.height)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .onClick([&store, &state, row, nowMinute]() {
                            const TaskEnv* env = row.envId != 0 ? findEnv(state, row.envId) : nullptr;
                            if (row.revocable) {
                                if (row.serviceId == 0) {
                                    openRevokePrereq(state, prereqCode(row.prereqKey),
                                                     prereqCheckLabel(row.prereqKey),
                                                     row.isAnnounce);
                                } else if (row.config) {
                                    openRevokeConfig(state, row.serviceId, row.configKind,
                                                     findService(state, row.serviceId) != nullptr
                                                         ? findService(state, row.serviceId)->serviceName
                                                         : "服务");
                                } else {
                                    openRevokeStep(state, row.serviceId, row.stepKind,
                                                   findService(state, row.serviceId) != nullptr
                                                       ? findService(state, row.serviceId)->serviceName
                                                       : "服务");
                                }
                                return;
                            }
                            if (env == nullptr) return;
                            if (row.isAnnounce) {
                                markAnnouncementSent(store, state);
                            } else if (row.serviceId == 0) {
                                markPrereqCompleted(store, state, prereqCode(row.prereqKey),
                                                    prereqTitle(row.prereqKey));
                            } else if (row.config) {
                                markConfigCompleted(store, state, row.serviceId,
                                                    row.configKind, *env, nowMinute);
                            } else {
                                markStepCompleted(store, state, row.serviceId,
                                                  row.stepKind, *env, nowMinute);
                            }
                        })
                        .build();
                }
                y += row.height + kTrackRowGap;
            }
        })
        .build();
    (void)themeTokens;
}

void drawNotice(eui::Ui& ui, float width, const ReleaseDetailUiState& state,
                const Tokens& t) {
    const eui::Color bg = state.noticeIsError
        ? eui::mixColor(t.colors.surface, t.colors.error, 0.10f)
        : t.colors.primarySoftBg;
    ui.stack("release.detail.notice")
        .size(width, kNoticeH)
        .content([&] {
            ui.rect("release.detail.notice.bg")
                .size(width, kNoticeH)
                .color(bg)
                .radius(t.sizes.radius)
                .build();
            ui.text("release.detail.notice.text")
                .position(12.0f, 0.0f)
                .size(std::max(0.0f, width - 24.0f), kNoticeH)
                .text(state.notice)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(kNoticeH)
                .maxWidth(std::max(0.0f, width - 24.0f))
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(state.noticeIsError ? t.colors.errorText : t.colors.primaryText)
                .build();
        })
        .build();
}

// 校验清单（「提交」与页头「校验」共用）：内容 = release_logic::validateTask
// 的原样输出；只读不改库。

void drawValidationDialog(eui::Ui& ui, const eui::Screen& screen,
                          ReleaseDetailUiState& state, const Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens) {
    const float margin = themeTokens.metrics.spacing.overlay;
    const float panelW = std::min(560.0f, std::max(0.0f, screen.width - margin));
    const float panelH = std::min(420.0f, std::max(0.0f, screen.height - margin));
    auto dialog = components::dialog(ui, "release.detail.validation")
                      .open(true)
                      .screen(screen.width, screen.height)
                      .size(panelW, panelH)
                      .theme(themeTokens)
                      .zIndex(1300)
                      .onOpenChange([&state](bool open) {
                          if (!open) state.confirm = ReleaseDetailConfirm::None;
                      });
    dialog.content([&, panelW, panelH] {
        ui.text("release.detail.validation.title")
            .position(24.0f, 16.0f)
            .size(std::max(0.0f, panelW - 48.0f), 26.0f)
            .text("校验未通过")
            .fontSize(t.sizes.fontSizeLarge)
            .lineHeight(26.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();
        ui.text("release.detail.validation.hint")
            .position(24.0f, 46.0f)
            .size(std::max(0.0f, panelW - 48.0f), 20.0f)
            .text("以下 " + std::to_string(state.validationMissing.size()) +
                  " 项不完整，补齐后再继续：")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();

        const float listY = 74.0f;
        const float listH =
            std::max(0.0f, panelH - listY - ui_kit::kButtonHeight - 40.0f);
        const float itemFontSize = t.sizes.fontSizeSmall;
        const eui::Color itemColor = t.colors.errorText;
        components::scrollView(ui, "release.detail.validation.scroll")
            .position(24.0f, listY)
            .size(std::max(0.0f, panelW - 48.0f), listH)
            .gap(2.0f)
            .theme(themeTokens)
            .scrollbarOverlay()
            .content([&state, itemFontSize, itemColor](eui::Ui& inner, float innerWidth,
                                                       float) {
                for (std::size_t i = 0; i < state.validationMissing.size(); ++i) {
                    inner.text("release.detail.validation.item." + std::to_string(i))
                        .size(innerWidth, 22.0f)
                        .text("• " + state.validationMissing[i])
                        .fontSize(itemFontSize)
                        .lineHeight(22.0f)
                        .maxWidth(innerWidth)
                        .wrap(false)
                        .overflow(core::TextOverflow::Ellipsis)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(itemColor)
                        .build();
                }
            })
            .build();

        ui_kit::ButtonOptions okOptions;
        okOptions.onClick = [&state] { state.confirm = ReleaseDetailConfirm::None; };
        ui_kit::button(ui, "release.detail.validation.ok", "知道了",
                       ui_kit::ButtonLevel::Primary,
                       std::max(24.0f, panelW - 24.0f - 96.0f),
                       std::max(16.0f, panelH - 24.0f - ui_kit::kButtonHeight), 96.0f,
                       ui_kit::kButtonHeight, t, okOptions);
    });
    dialog.build();
}

// ==================== 配置编辑层（页内全屏） ====================

// 页内自绘全屏配置编辑层（不经模块回调）：多行大文本框 + 字数统计 + 保存 / 取消。
// 已跳过或整单只读（Done / Voided）时只读展示并提示原因。保存只写 content，
// 内容变化的自动撤回走 loadDetail → applyContentRevocations 的既有对账路径。

void drawConfigEditor(eui::Ui& ui, const eui::Screen& screen,
                      ReleaseDetailUiState& state, ReleaseStore& store,
                      const Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens) {
    const TaskService* svc = findService(state, state.editorServiceId);
    const TaskConfig* cfg = findConfig(state, state.editorServiceId, state.editorKind);
    if (svc == nullptr || cfg == nullptr) {
        // 目标已随重读消失（如任务被切走）：直接关层。
        state.editorOpen = false;
        return;
    }
    const bool readOnly = isReadOnly(state.displayStatus);
    const bool editable = !readOnly && !cfg->skipped;

    const float panelW = screen.width;
    const float panelH = screen.height;
    const float inset = 24.0f;
    const float titleH = 28.0f;
    const float hintH = editable ? 0.0f : 30.0f;
    const float footerH = ui_kit::kButtonHeight;
    const float footerY = panelH - inset - footerH;
    const float countH = 20.0f;
    const float inputY = inset + titleH + 12.0f + (editable ? 0.0f : hintH + 8.0f);
    const float inputH = std::max(0.0f, footerY - 8.0f - countH - 8.0f - inputY);
    const std::string title = svc->serviceName + " · " + configLabel(state.editorKind);

    auto dialog = components::dialog(ui, "release.detail.config.editor")
                      .open(true)
                      .screen(screen.width, screen.height)
                      .size(panelW, panelH)
                      .theme(themeTokens)
                      .zIndex(1300)
                      .onOpenChange([&state](bool open) {
                          if (!open) state.editorOpen = false;
                      });
    dialog.content([&, panelW, inset, titleH, hintH, inputY, inputH, footerY,
                    countH, title] {
        ui.text("release.detail.config.editor.title")
            .position(inset, inset)
            .size(std::max(0.0f, panelW - inset * 2.0f), titleH)
            .text(title)
            .fontSize(t.sizes.fontSizeLarge)
            .lineHeight(titleH)
            .fontWeight(600)
            .maxWidth(std::max(0.0f, panelW - inset * 2.0f))
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .color(t.colors.textPrimary)
            .build();

        if (!editable) {
            const std::string hint = cfg->skipped
                ? "该项本次已跳过，取消跳过后可编辑"
                : "任务已只读，配置内容仅可查看";
            const eui::Color bg =
                cfg->skipped ? eui::mixColor(t.colors.surface, t.colors.warning, 0.14f)
                             : t.colors.hover;
            const eui::Color fg =
                cfg->skipped ? t.colors.warning : t.colors.textTertiary;
            const float hintY = inset + titleH + 12.0f;
            ui.rect("release.detail.config.editor.hint.bg")
                .position(inset, hintY)
                .size(panelW - inset * 2.0f, hintH)
                .color(bg)
                .radius(t.sizes.radius)
                .build();
            ui.text("release.detail.config.editor.hint.text")
                .position(inset + 12.0f, hintY)
                .size(std::max(0.0f, panelW - inset * 2.0f - 24.0f), hintH)
                .text(hint)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(hintH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(fg)
                .build();
        }

        if (editable) {
            components::input(ui, "release.detail.config.editor.input")
                .position(inset, inputY)
                .size(panelW - inset * 2.0f, inputH)
                .value(state.editorText)
                .placeholder("粘贴或填写本次配置内容")
                .multiline(true)
                .scrollbar(true)
                .fontSize(t.sizes.fontSizeBody)
                .inset(10.0f)
                .theme(themeTokens)
                .onChange(
                    [&state](const std::string& value) { state.editorText = value; })
                // 输入框聚焦时 Esc 被输入框消费：与「取消」同语义（丢弃草稿关层）。
                .onCancel([&state] { state.editorOpen = false; })
                .build();
        } else {
            drawReadOnlyValue(ui, "release.detail.config.editor.view", inset, inputY,
                              panelW - inset * 2.0f, inputH, cfg->content,
                              "未填写配置内容", true, t);
        }

        ui.text("release.detail.config.editor.count")
            .position(inset, footerY - 8.0f - countH)
            .size(240.0f, countH)
            .text("共 " + std::to_string(utf8CharCount(state.editorText)) + " 字")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(countH)
            .color(t.colors.textTertiary)
            .build();

        ui_kit::ButtonOptions cancelOptions;
        cancelOptions.onClick = [&state] { state.editorOpen = false; };
        ui_kit::button(ui, "release.detail.config.editor.cancel",
                       editable ? "取消" : "关闭", ui_kit::ButtonLevel::Secondary,
                       panelW - inset - 88.0f - 8.0f - 96.0f, footerY, 88.0f, footerH,
                       t, cancelOptions);
        if (editable) {
            ui_kit::ButtonOptions saveOptions;
            saveOptions.onClick = [&store, &state] { saveConfigEditor(store, state); };
            ui_kit::button(ui, "release.detail.config.editor.save", "保存",
                           ui_kit::ButtonLevel::Primary, panelW - inset - 96.0f,
                           footerY, 96.0f, footerH, t, saveOptions);
        }
    });
    dialog.build();
}

void drawConfirmDialogs(eui::Ui& ui, const eui::Screen& screen,
                        ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute,
                        const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
    if (state.confirm == ReleaseDetailConfirm::None) return;
    if (state.confirm == ReleaseDetailConfirm::ValidationReport) {
        drawValidationDialog(ui, screen, state, t, themeTokens);
        return;
    }

    std::string title;
    std::string message;
    std::string primaryText;
    bool danger = false;
    if (state.confirm == ReleaseDetailConfirm::Revoke) {
        title = "撤回确认";
        message = "撤回后「" + state.revokeStepLabel + "」回到未完成，进度会重新计算。确定撤回？";
        primaryText = "确定撤回";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::VoidTask) {
        title = "作废任务";
        message = "作废后停止提醒、整单只读；之后可随时恢复。确定作废？";
        primaryText = "确定作废";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::RemoveService) {
        title = "移除服务";
        message =
            "确定把「" + state.removeName + "」从本次任务移除？进度与提醒不再包含它";
        primaryText = "确定移除";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::WithdrawSubmission) {
        // 撤回提交（F-02 兜底出路）：状态回退非数据清理，文案如实声明保留项。
        title = "撤回提交";
        message =
            "撤回后任务回到准备中（草稿），已确认的步骤与操作记录保留；修改后可重新提交。确定撤回？";
        primaryText = "确定撤回";
        danger = true;
    } else if (state.confirm == ReleaseDetailConfirm::RemoveEnv) {
        title = "移除环境";
        message = "确定把「" + state.removeName +
                  "」从本次任务移除？环境内服务、进度与提醒不再包含它";
        primaryText = "确定移除";
        danger = true;
    } else {
        title = "恢复任务";
        message = "恢复后按当前进度与时间回到对应状态。确定恢复？";
        primaryText = "确定恢复";
    }

    components::DialogStyle style(themeTokens);
    if (danger) {
        style.primary = t.colors.error;
        style.primaryHover = eui::mixColor(t.colors.error, t.colors.textPrimary, 0.12f);
        style.primaryPressed = eui::mixColor(t.colors.error, t.colors.textPrimary, 0.24f);
    }
    auto dialog = components::dialog(ui, "release.detail.confirm")
                      .open(true)
                      .screen(screen.width, screen.height)
                      .size(460.0f, 220.0f)
                      .theme(themeTokens)
                      .style(style)
                      .zIndex(1300)
                      .title(title)
                      .message(message)
                      .primaryText(primaryText)
                      .secondaryText("取消")
                      .onOpenChange([&state](bool open) {
                          if (!open) state.confirm = ReleaseDetailConfirm::None;
                      });
    if (state.confirm == ReleaseDetailConfirm::RestoreTask) {
        dialog.onPrimary([&store, &state, nowMinute] {
            applyRestore(store, state, nowMinute);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::WithdrawSubmission) {
        dialog.onPrimary([&store, &state] {
            applyWithdrawSubmission(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::VoidTask) {
        dialog.onPrimary([&store, &state] {
            applyVoid(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::RemoveService) {
        dialog.onPrimary([&store, &state] {
            applyRemoveService(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else if (state.confirm == ReleaseDetailConfirm::RemoveEnv) {
        dialog.onPrimary([&store, &state] {
            applyRemoveEnv(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    } else {
        dialog.onPrimary([&store, &state] {
            applyRevoke(store, state);
            state.confirm = ReleaseDetailConfirm::None;
        });
    }
    dialog.onSecondary([&state] { state.confirm = ReleaseDetailConfirm::None; });
    dialog.build();
}

// ==================== 数据加载 ====================

// 配置内容 / 服务分支保存后（宿主置 state.dirty → 本页重读）对比旧快照：内容变了
// 旧确认作废——对应步骤已完成则自动撤回并记 action=revoke 日志（不弹二次确认，
// 因为内容已经改了）；未完成不动。只对同一任务的重读生效，切换任务不是修改。
// 配置步骤以 TaskConfig.doneAt 记账（release_logic 忽略 TaskStep 的 Config 行）。

}  // namespace detail
}  // namespace module::release
