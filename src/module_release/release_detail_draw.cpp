// release_detail_draw.cpp
// 从 release_detail_ui.cpp 拆出（2026-10-31）：绘制层——度量、卡片、步骤行、
// 工单卡、环境分区，以及校验 / 配置编辑 / 确认弹窗。只读 state 与 logic 层查询。

#include "release_detail_internal.h"

#include "release_checkbox.h"
#include "release_config_ui.h"
#include "release_list_ui.h"

#include <core/component_theme.h>
#include <core/render/text.h>

#include <components/datepicker.h>  // 批次 A-8 改期：复用建单同款滚轮
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

float measureText(const std::string& text, float fontSize, int weight) {
    return core::TextPrimitive::measureTextWidth(text, {}, fontSize, weight);
}

float chipWidth(const std::string& text, float fontSize) {
    return measureText(text, fontSize, 600) + 20.0f;
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
    // 卡面材质（need219 §5.2-1）：非 subtle 主卡走玻璃 itemSurface，subtle 嵌套浅底维持
    // background 层级档不属卡面材质。
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(subtle ? t.colors.background : ui_kit::itemSurface(t))
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

// ==================== 强确认门（need192 §6） ====================

// 完成类动作统一挂门：先把落态动作暂存进 state（二次确认前不写库契约不变），
// 弹 420 逐字「确认」门（drawStepGateDialog），匹配后 gateApply 才执行。
// impact 按步骤类型给「完成后系统将」文案（设计 §6.3 三要素之二）。

void openCompletionGate(ReleaseDetailUiState& state, const std::string& subject,
                        const std::string& impact, const std::string& titleLabel,
                        std::function<void(ReleaseStore&, ReleaseDetailUiState&)> apply) {
    state.confirm = ReleaseDetailConfirm::StepComplete;
    state.gateTitle = "执行确认 · " + titleLabel;
    state.gateSubject = subject;
    state.gateImpact = impact;
    state.gateInput.clear();
    state.gateApply = std::move(apply);
}

void drawStepGateDialog(eui::Ui& ui, const eui::Screen& screen,
                        ReleaseDetailUiState& state, ReleaseStore& store,
                        const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
    const std::string id = "release.detail.stepgate";
    const bool matched = trimmed(state.gateInput) == "确认";
    ui_kit::DialogShellSpec spec;
    spec.id = id;
    spec.width = ui_kit::DialogWidth::Compact;  // 420 档（设计 §6.2）
    spec.height = 340.0f;
    spec.open = true;
    spec.title = state.gateTitle;
    spec.cancel = ui_kit::DialogShellButton{
        "取消", [&state] { state.confirm = ReleaseDetailConfirm::None; }, true};
    spec.confirm = ui_kit::DialogShellButton{
        "确认执行",
        [&store, &state] {
            auto apply = state.gateApply;
            state.confirm = ReleaseDetailConfirm::None;
            state.gateApply = nullptr;
            if (apply) apply(store, state);
        },
        matched};  // 不匹配主钮禁用（规范 §4.3 禁用档）
    spec.onOpenChange = [&state](bool open) {
        if (!open) state.confirm = ReleaseDetailConfirm::None;
    };
    ui_kit::drawDialogShell(ui, screen, spec, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
        float y = area.y + 8.0f;
        ui.text(id + ".lead")
            .position(area.x, y)
            .size(area.w, core::theme::scaled(20.0f))
            .text("即将标记完成：")
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textPrimary)
            .build();
        y += 22.0f;
        ui.text(id + ".subject")
            .position(area.x, y)
            .size(area.w, core::theme::scaled(22.0f))
            .text(state.gateSubject)
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(core::theme::scaled(22.0f))
            .fontWeight(600)
            .maxWidth(area.w)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .color(t.colors.textPrimary)
            .build();
        y += 28.0f;
        ui.text(id + ".impact")
            .position(area.x, y)
            .size(area.w, core::theme::scaled(20.0f))
            .text("完成后系统将：" + state.gateImpact)
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textSecondary)
            .build();
        y += 24.0f;
        ui.text(id + ".risk")
            .position(area.x, y)
            .size(area.w, core::theme::scaled(18.0f))
            .text("⚠ 完成后需撤回才能修改，撤回记入任务日志")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(18.0f))
            .color(t.colors.errorText)
            .build();
        y += 26.0f;
        ui.text(id + ".prompt")
            .position(area.x, y)
            .size(area.w, core::theme::scaled(18.0f))
            .text("输入「确认」以继续")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(18.0f))
            .color(t.colors.textSecondary)
            .build();
        y += 22.0f;
        components::input(ui, id + ".input")
            .position(area.x, y)
            .size(area.w, 36.0f)
            .value(state.gateInput)
            .placeholder("确认")
            .fontSize(core::theme::scaled(14.0f))
            .theme(themeTokens)
            .onChange([&state](const std::string& value) { state.gateInput = value; })
            .onEnter([&store, &state] {
                if (trimmed(state.gateInput) != "确认") return;
                auto apply = state.gateApply;
                state.confirm = ReleaseDetailConfirm::None;
                state.gateApply = nullptr;
                if (apply) apply(store, state);
            })
            .build();
        if (!state.gateInput.empty() && !matched) {
            ui.text(id + ".error")
                .position(area.x, y + 42.0f)
                .size(area.w, core::theme::scaled(18.0f))
                .text("请输入「确认」二字")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.errorText)
                .build();
        }
    });
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

// 前置区（F-12 单据式表单流，编辑态专属）：一行一单据，label 定宽 + 输入 +
// 行尾勾；「填」是主体、「勾」是收尾。单号未填 → 勾禁用 + 提示（既有门禁）。
// 到期提示不逐行常驻（设计 §5.2）：由已完成聚合 / 提醒引擎 / 轨道到期承载。

float prereqSectionHeight(const ReleaseDetailUiState& state) {
    const TicketNeeds needs = scanTicketNeeds(state);
    const int docRows = 2 + (needs.nacos ? 1 : 0) + (needs.db ? 1 : 0);
    const float rowH = kStepRowH;
    const float inner = 16.0f + 18.0f + 8.0f +
                        static_cast<float>(docRows) * (rowH + 8.0f) - 8.0f +
                        16.0f + 44.0f + 16.0f;
    return kSectionTitleH + kSectionGap + inner;
}

// ==================== 环境 / 服务 ====================

float serviceCardHeight(const ReleaseDetailUiState& state, const TaskService& svc) {
    const float rows = static_cast<float>(3 + orderedConfigs(state, svc).size());
    // 池 54（F-54-1）：编辑态分支行从 16px 只读行升级为 kStepRowH 表单行（+28）；
    // 发布/归档态维持只读行高度。need192 §4.5：「···」次行整体裁撤（跳过常显
    // 复选框 + 拖拽排序），无展开高度。
    const float branchExtra = isEditMode(state.displayStatus) ? kStepRowH - 16.0f : 0.0f;
    return 68.0f + branchExtra + rows * kStepRowH + (rows - 1.0f) * kStepRowGap;
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

// 步骤行（F-13 四要素 + need192 §4.4 锚点链）：状态点 + 名称（L2 15px）+ 状态字 +
// 完成（整行点击）；行尾右锚组 = [填写/查看] + [跳过□]（配置行常显复选框，
// need192 §4.5；「···」菜单整体裁撤——上移/下移走拖拽、跳过常显、查看行内钮）。

void drawStepRow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const StepRowView& row, bool current, bool readOnly,
                 bool configurable, bool readOnlyViewAllowed,
                 const Tokens& t,
                 const std::function<void()>& onRowClick,
                 const std::function<void()>& onView,
                 const std::function<void()>& onSkip) {
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

    // 行尾操作区（右缘向左排）：跳过□（配置行常显，need192 §4.5）+ 填写/查看(56)。
    const bool showView = row.config && onView != nullptr;
    const bool showSkip = row.config && !row.done && onSkip != nullptr;
    const float skipW = showSkip ? 76.0f : 0.0f;
    const float viewX = showView ? rowX + rowW - 8.0f - kViewButtonW -
                                       (showSkip ? skipW + 8.0f : 0.0f)
                                 : rowX + rowW;
    // 可排序的配置行在最左侧预留拖拽手柄位（⋮⋮）；手柄由 drawServiceCard 叠加绘制。
    const bool showDragHandle = row.config && configurable;
    const float nameX = dotX + kStepDot +
                        (showDragHandle ? kDragHandleW + kDragHandleGap : 10.0f);
    // 名称/状态区右界 = 行尾控件组左缘（行右缘向左：8 + [跳过□ 76] + 8 +
    // [填写/查看 56]）。原式 `viewX - 8 - skipW` 把 skipW 减了两次（viewX 内已
    // 让位过跳过框），未完成配置行状态字整体左移 76px、与按钮间留死缝；且
    // !showView 分支不减右内距会压到跳过框（#359）。
    const float nameRight = rowX + rowW - 8.0f
                          - (showSkip ? skipW + 8.0f : 0.0f)
                          - (showView ? kViewButtonW + 8.0f : 0.0f);
    const float statusW = 88.0f;
    const float nameW = std::max(0.0f, nameRight - statusW - 8.0f - nameX);

    eui::Color labelColor = row.done ? t.colors.textPrimary
                             : (row.skipped ? t.colors.textDisabled
                                            : (current ? t.colors.textPrimary
                                                       : t.colors.textSecondary));
    if (row.invalid) {
        // need192 §4.3：废止正文标签整名变红——invalid 以浅红底 + 行尾状态字
        //（字段级就近）表达，名字保持常规色。
    }
    ui.text(id + ".label")
        .position(nameX, y)
        .size(nameW, kStepRowH)
        .text(row.label)
        .fontSize(core::theme::scaled(15.0f))  // §4.2 L2：步骤/配置名 13→15
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
    if (showSkip) {
        // 跳过常显复选框：未跳=空心框；已跳=灰底横线+label 划线（release_checkbox
        // skipped 视觉契约）；已完成的行不显示（先取消完成才能跳过，与旧菜单一致）。
        ReleaseCheckboxOptions skip;
        skip.checked = row.skipped;
        skip.skipped = row.skipped;
        skip.label = row.skipped ? "已跳过" : "跳过";
        skip.boxSize = kCheckSize;
        skip.fontSize = t.sizes.fontSizeSmall - 1.0f;
        skip.width = skipW;
        skip.height = kStepRowH;
        skip.inset = 0.0f;
        skip.gap = 6.0f;
        drawReleaseCheckboxAt(ui, id + ".skip", rowX + rowW - 8.0f - skipW, y, skip,
                              onSkip, t);
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
            // 批次 A-8：有环境时也能再加（此前只有空态「去基础配置」出口，
            // 一旦建单漏选就只能作废重建）。编辑态才给入口，只读态不显示。
            if (!state.envs.empty() && !readOnly) {
                ui_kit::ButtonOptions addOpts;
                addOpts.onClick = [&state, &store] {
                    // 打开前算候选：基础配置里有、本任务尚未加入的环境。
                    // 加入与否按环境名判定（TaskEnv 只快照名字，不含 EnvDef id——
                    // 这是「快照与基础配置解耦」的直接后果，改配置不影响已建任务）。
                    state.addEnvCandidates.clear();
                    state.addEnvNames.clear();
                    auto defs = store.listEnvs();
                    if (defs.isOk()) {
                        for (const auto& d : defs.value()) {
                            bool already = false;
                            for (const auto& e : state.envs) {
                                if (!e.removed && e.envName == d.name) {
                                    already = true;
                                    break;
                                }
                            }
                            if (!already) {
                                state.addEnvCandidates.push_back(d.id);
                                state.addEnvNames.push_back(d.name);
                            }
                        }
                    }
                    state.addEnvOpen = true;
                };
                addOpts.hasTextColorOverride = true;
                addOpts.textColorOverride = t.colors.primaryText;
                ui_kit::button(ui, "release.detail.env.add", "+ 添加环境", 
                               ui_kit::ButtonLevel::Text, width - 96.0f, 0.0f,
                               96.0f, kSectionTitleH, t, addOpts);
            }
            if (state.envs.empty()) {
                ui.text("release.detail.env.empty")
                    .position(0.0f, kSectionTitleH + kSectionGap)
                    .size(width, core::theme::scaled(48.0f))
                    .text("任务还没有环境")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(core::theme::scaled(48.0f))
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

// ==================== 焦点式双栏（need192 §1.2，编辑态） ====================
//
// 左节点索引列 264px（一级概览：环境分组头 + 服务精简条目，点击切焦点）+
// 右焦点区（二级明细：当前服务的步骤/配置全量，唯一放大展示区）。节点=服务，
// 环境是分组头。服务排序拖拽在双栏下的入口重设计未定（焦点区单卡无序列感），
// allowServiceDrag 置 false 隐藏把柄；落库契约与逻辑函数保持不变（池 192 断点）。

// 索引条目 mini 进度的装饰档计数：服务内未跳过行中已完成行数。进度判定唯一
// 判据仍是 release_logic::envProgress（环境级），此处不参与任何业务判定。
void serviceRowCounts(const ReleaseDetailUiState& state, const TaskService& svc,
                      int* done, int* total) {
    *done = 0;
    *total = 0;
    if (stepDone(state, svc.id, StepKind::MergeBranch)) ++*done;
    ++*total;
    for (const auto& cfg : orderedConfigs(state, svc)) {
        if (cfg.skipped) continue;
        ++*total;
        if (cfg.doneAt != 0) ++*done;
    }
    if (stepDone(state, svc.id, StepKind::Deploy)) ++*done;
    ++*total;
    if (stepDone(state, svc.id, StepKind::Verify)) ++*done;
    ++*total;
}

void drawFocusLayout(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                     bool readOnly, int nowMinute, const Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     ReleaseStore& store, const ReleaseDetailActions& actions) {
    constexpr float kIndexW = 264.0f;   // §1.2：索引列定宽（>R6 门槛 200）
    constexpr float kColumnGap = 16.0f;
    constexpr float kEntryH = 56.0f;    // §3.1 条目 56 双行
    constexpr float kGroupH = 32.0f;    // §3.1 分组头 32
    constexpr float kFocusX = kIndexW + kColumnGap;

    struct IndexEntry {
        const TaskEnv* env;
        const TaskService* svc;
    };
    std::vector<IndexEntry> entries;
    for (const auto& env : state.envs) {
        for (const TaskService* svc : liveServices(state, env)) {
            entries.push_back({&env, svc});
        }
    }
    // 分组头（环境名 + 移除环境入口）：环境内服务全被移除时仍渲染组头，
    // 「移除环境」入口不随服务清空而消失（节点 id 与旧 envCard 一致）。
    auto drawGroupHeader = [&](const TaskEnv& env, float gy) {
        ui.text("release.detail.index.env." + std::to_string(env.id) + ".label")
            .position(12.0f, gy)
            .size(kIndexW - 130.0f, kGroupH)
            .text(env.envName)
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(kGroupH)
            .maxWidth(kIndexW - 130.0f)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .verticalAlign(eui::VerticalAlign::Center)
            .fontWeight(600)
            .color(t.colors.textSecondary)
            .build();
        if (!readOnly) {
            ui_kit::ButtonOptions options;
            options.onClick = [&state, envId = env.id, name = env.envName] {
                openRemoveEnv(state, envId, name);
            };
            options.hasTextColorOverride = true;
            options.textColorOverride = t.colors.errorText;
            ui_kit::button(ui, "release.detail.env." + std::to_string(env.id) +
                                   ".remove",
                           "移除环境", ui_kit::ButtonLevel::Text, kIndexW - 76.0f,
                           gy + (kGroupH - 24.0f) * 0.5f, 64.0f, 24.0f, t, options);
        }
    };
    if (entries.empty()) {
        const float emptyH = kSectionTitleH + kSectionGap +
                             static_cast<float>(state.envs.size()) * kGroupH + 48.0f;
        ui.stack("release.detail.focus")
            .size(width, emptyH)
            .content([&] {
                drawSectionTitle(ui, "release.detail.focus.title", 0.0f, 0.0f,
                                 "环境与服务", t);
                float y = kSectionTitleH + kSectionGap;
                ui.rect("release.detail.index.bg")
                    .position(0.0f, y)
                    .size(kIndexW, emptyH - y)
                    .color(ui_kit::itemSurface(t))
                    .border(1.0f, t.colors.borderDefault)
                    .radius(t.sizes.radiusLarge)
                    .build();
                y += 8.0f;
                for (const auto& env : state.envs) {
                    drawGroupHeader(env, y);
                    y += kGroupH;
                }
                // 无环境 = 任务级空态（文案与「去基础配置」链接沿用旧契约 id）；
                // 有环境无服务 = 索引空态。
                const bool noEnvs = state.envs.empty();
                ui.text(noEnvs ? "release.detail.env.empty"
                               : "release.detail.focus.empty")
                    .position(12.0f, y)
                    .size(kIndexW - 24.0f, core::theme::scaled(48.0f))
                    .text(noEnvs ? "任务还没有环境" : "暂无服务")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(core::theme::scaled(48.0f))
                    .color(t.colors.textTertiary)
                    .build();
                if (noEnvs && actions.onOpenConfig) {
                    ui_kit::ButtonOptions options;
                    options.onClick = actions.onOpenConfig;
                    options.hasTextColorOverride = true;
                    options.textColorOverride = t.colors.primaryText;
                    ui_kit::button(ui, "release.detail.env.openconfig", "去基础配置",
                                   ui_kit::ButtonLevel::Text, 12.0f, y + 44.0f, 88.0f,
                                   28.0f, t, options);
                }
            })
            .build();
        return;
    }
    // 焦点兜底：focusedServiceId 失效（未选/已移除）→ 首个 live 服务写回。
    bool focused = false;
    for (const auto& e : entries) {
        if (e.svc->id == state.focusedServiceId) focused = true;
    }
    if (!focused) state.focusedServiceId = entries.front().svc->id;
    const TaskEnv* focusEnv = nullptr;
    const TaskService* focusSvc = nullptr;
    for (const auto& e : entries) {
        if (e.svc->id == state.focusedServiceId) {
            focusEnv = e.env;
            focusSvc = e.svc;
            break;
        }
    }

    float leftH = 8.0f;
    {
        std::int64_t lastEnvId = 0;
        for (const auto& e : entries) {
            if (e.env->id != lastEnvId) {
                leftH += kGroupH;
                lastEnvId = e.env->id;
            }
            leftH += kEntryH + 6.0f;
        }
    }
    const float totalH = std::max(std::max(leftH, serviceCardHeight(state, *focusSvc)),
                                  200.0f);

    ui.stack("release.detail.focus")
        .size(width, totalH)
        .content([&] {
            // —— 左：节点索引列 ——
            ui.rect("release.detail.index.bg")
                .position(0.0f, 0.0f)
                .size(kIndexW, totalH)
                .color(ui_kit::itemSurface(t))
                .border(1.0f, t.colors.borderDefault)
                .radius(t.sizes.radiusLarge)
                .build();
            float y = 8.0f;
            std::int64_t lastEnvId = 0;
            for (const auto& e : entries) {
                if (e.env->id != lastEnvId) {
                    lastEnvId = e.env->id;
                    drawGroupHeader(*e.env, y);
                    y += kGroupH;
                }
                const bool selected = e.svc->id == state.focusedServiceId;
                const std::string eid =
                    "release.detail.index.svc." + std::to_string(e.svc->id);
                if (selected) {
                    // 选中态：primarySoftBg + 左 2px 竖条（基线选中态规范）。
                    ui.rect(eid + ".sel")
                        .position(4.0f, y)
                        .size(kIndexW - 8.0f, kEntryH)
                        .color(t.colors.primarySoftBg)
                        .radius(t.sizes.radius)
                        .build();
                    ui.rect(eid + ".sel.bar")
                        .position(4.0f, y)
                        .size(2.0f, kEntryH)
                        .color(t.colors.primary)
                        .build();
                }
                ui.rect(eid + ".hit")
                    .position(4.0f, y)
                    .size(kIndexW - 8.0f, kEntryH)
                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([&state, serviceId = e.svc->id] {
                        state.focusedServiceId = serviceId;
                    })
                    .build();
                int done = 0;
                int total = 0;
                serviceRowCounts(state, *e.svc, &done, &total);
                const bool allDone = total > 0 && done == total;
                ui.text(eid + ".name")
                    .position(16.0f, y + 8.0f)
                    .size(kIndexW - 78.0f, core::theme::scaled(20.0f))
                    .text(e.svc->serviceName)
                    .fontSize(core::theme::scaled(15.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .fontWeight(600)
                    .maxWidth(kIndexW - 78.0f)
                    .wrap(false)
                    .overflow(core::TextOverflow::Ellipsis)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(eid + ".count")
                    .position(kIndexW - 58.0f, y + 8.0f)
                    .size(46.0f, core::theme::scaled(20.0f))
                    .text(allDone ? std::string("已完成")
                                  : std::to_string(done) + "/" + std::to_string(total))
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(allDone ? t.colors.success : t.colors.textSecondary)
                    .build();
                ui.text(eid + ".envname")
                    .position(16.0f, y + 32.0f)
                    .size(120.0f, core::theme::scaled(16.0f))
                    .text(e.env->envName)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(core::theme::scaled(16.0f))
                    .maxWidth(120.0f)
                    .wrap(false)
                    .overflow(core::TextOverflow::Ellipsis)
                    .color(t.colors.textTertiary)
                    .build();
                // mini 进度 32×4（右锚小部件档，R6 合规）。
                ui.stack(eid + ".mini")
                    .position(kIndexW - 48.0f, y + 38.0f)
                    .size(32.0f, 4.0f)
                    .content([&ui, eid, done, total, allDone, &t] {
                        ui.rect(eid + ".mini.track")
                            .size(32.0f, 4.0f)
                            .color(t.colors.borderDefault)
                            .radius(2.0f)
                            .build();
                        ui.rect(eid + ".mini.fill")
                            .size(total > 0
                                      ? 32.0f * static_cast<float>(done) /
                                            static_cast<float>(total)
                                      : 0.0f,
                                  4.0f)
                            .color(allDone ? t.colors.success : t.colors.primary)
                            .radius(2.0f)
                            .build();
                    })
                    .build();
                y += kEntryH + 6.0f;
            }

            // —— 右：焦点区（当前服务全量明细；服务卡节点 id 与旧清单同构，
            // 无头测试行路径不漂移）——
            drawServiceCard(ui, "release.detail.env." + std::to_string(focusEnv->id) +
                                    ".svc." + std::to_string(focusSvc->id),
                            kFocusX, 0.0f, std::max(0.0f, width - kFocusX), *focusEnv,
                            *focusSvc, state, readOnly, !readOnly, false, nowMinute, t,
                            themeTokens, store);
        })
        .build();
}

// 公告弹窗（need192 §4.6，#156 640 Wide 档）：multiline 编辑 + 字数 +
// 取消(次)/保存(主)。draft 缓冲，保存才落 task（saveTask 契约不变）。

void drawAnnounceDialog(eui::Ui& ui, const eui::Screen& screen,
                        ReleaseDetailUiState& state, ReleaseStore& store,
                        const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
    if (!state.announceEditorOpen) return;
    const std::string id = "release.detail.announce";
    ui_kit::DialogShellSpec spec;
    spec.id = id;
    spec.width = ui_kit::DialogWidth::Wide;  // 640 档（设计 §4.6）
    spec.height = 480.0f;
    spec.open = true;
    spec.title = "上线公告";
    spec.cancel = ui_kit::DialogShellButton{
        "取消", [&state] { state.announceEditorOpen = false; }, true};
    spec.confirm = ui_kit::DialogShellButton{
        "保存",
        [&store, &state] {
            state.task.announceText = state.announceEditText;
            state.announceEditorOpen = false;
            saveTask(store, state);
        },
        true};
    spec.onOpenChange = [&state](bool open) {
        if (!open) state.announceEditorOpen = false;
    };
    ui_kit::drawDialogShell(
        ui, screen, spec, t, themeTokens,
        [&](const ui_kit::DialogContentArea& area) {
            components::input(ui, id + ".input")
                .position(area.x, area.y + 4.0f)
                .size(area.w, std::max(200.0f, area.h - 46.0f))
                .value(state.announceEditText)
                .placeholder("填写上线公告")
                .fontSize(core::theme::scaled(14.0f))
                .multiline(true)
                .scrollbar(true)
                .inset(10.0f)
                .theme(themeTokens)
                .onChange([&state](const std::string& value) {
                    state.announceEditText = value;
                })
                .build();
            ui.text(id + ".count")
                .position(area.x, area.y + area.h - 28.0f)
                .size(240.0f, core::theme::scaled(22.0f))
                .text("共 " + std::to_string(utf8CharCount(state.announceEditText)) +
                      " 字")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(core::theme::scaled(22.0f))
                .color(t.colors.textTertiary)
                .build();
        });
}

// 页级警示带（need192 §4.3）：校验未通过时页头下方横贯一条（error 10% 浅染底）
// + 「去处理」文字钮开校验清单；同一时刻只显示一条（notice 反馈优先，见 compose）。

void drawPageWarningBand(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                         const Tokens& t) {
    const std::string text = "校验未通过：还有 " +
                             std::to_string(state.validationMissing.size()) +
                             " 项不完整";
    ui.stack("release.detail.warning")
        .size(width, kNoticeH)
        .content([&] {
            ui.rect("release.detail.warning.bg")
                .size(width, kNoticeH)
                .color(eui::mixColor(t.colors.surface, t.colors.error, 0.10f))
                .radius(t.sizes.radius)
                .build();
            ui.text("release.detail.warning.text")
                .position(12.0f, 0.0f)
                .size(std::max(0.0f, width - 84.0f), kNoticeH)
                .text(text)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(kNoticeH)
                .maxWidth(std::max(0.0f, width - 84.0f))
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.errorText)
                .build();
            ui_kit::ButtonOptions options;
            options.onClick = [&state] {
                state.confirm = ReleaseDetailConfirm::ValidationReport;
            };
            options.hasTextColorOverride = true;
            options.textColorOverride = t.colors.errorText;
            ui_kit::button(ui, "release.detail.warning.go", "去处理",
                           ui_kit::ButtonLevel::Text, width - 60.0f, 0.0f, 52.0f,
                           kNoticeH, t, options);
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
    const float titleW = measureText(titleText, t.sizes.fontSizeTitle, 600) + 8.0f;
    ui.text("release.detail.title")
        .position(x, 0.0f)
        .size(titleW, headerH)
        .text(titleText)
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(headerH)
        .fontWeight(600)
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
    // 批次 A-8：计划上线日此前只能在建单时定，改期 = 作废重建。编辑态给「改期」
    // 文字钮，点开以当前 planDate 为锚点初始化滚轮。
    if (!(status == TaskStatus::Voided || status == TaskStatus::Done) &&
        state.task.id != 0) {
        ui_kit::ButtonOptions replanOpts;
        replanOpts.onClick = [&state] {
            int y = 0, m = 0, d = 0;
            if (std::sscanf(state.task.planDate.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
                y = 2026; m = 1; d = 1;
            }
            state.replanYear = y;
            state.replanMonth = m;
            state.replanDay = d;
            state.replanOpen = true;
        };
        replanOpts.hasTextColorOverride = true;
        replanOpts.textColorOverride = t.colors.primaryText;
        ui_kit::button(ui, "release.detail.replan", "改期", ui_kit::ButtonLevel::Text, x,
                       (headerH - kChipH) * 0.5f, 56.0f, kChipH, t, replanOpts);
        x += 56.0f + 8.0f;
    }
    const char* modeKey = "edit";
    if (status == TaskStatus::Voided) modeKey = "voided";
    else if (status == TaskStatus::Done) modeKey = "archive";
    else if (publish) modeKey = "publish";
    const float badgeW = drawModeBadge(ui, std::string("release.detail.modebadge.") +
                                            modeKey,
                                       x, (headerH - kChipH) * 0.5f, status, t);
    x += badgeW + 8.0f;
    // 113-5：状态 chip 是时间推导态（草稿/待执行/执行中），与「发布中」模式徽标
    // 语义重叠且互相抢主色——中性弱化（hover 底 / textTertiary 字），主色让给徽标。
    const ChipColors colors{t.colors.hover, t.colors.textTertiary};
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
        if (publish) {
            // 菜单裁撤（#361）：原「···」菜单内的「校验」升为直显次钮（编辑态
            // 本就直显，两态对齐）。
            placeButton("release.detail.validate", "校验",
                        ui_kit::ButtonLevel::Secondary, 72.0f,
                        [&state] { runValidation(state); });
        }
        // 作废任务直显（#361 老板令：不进菜单）。危险态 = Text 级 + 错误红字
        //（同服务卡「移除」钮范式）；点击仍只开二次确认，不直接写库。
        {
            const float voidW = 88.0f;
            right -= voidW;
            ui_kit::ButtonOptions voidOptions;
            voidOptions.onClick = [&state] {
                state.confirm = ReleaseDetailConfirm::VoidTask;
            };
            voidOptions.hasTextColorOverride = true;
            voidOptions.textColorOverride = t.colors.errorText;
            ui_kit::button(ui, "release.detail.void", "作废任务",
                           ui_kit::ButtonLevel::Text, right, 0.0f, voidW, headerH, t,
                           voidOptions);
            right -= 8.0f;
        }
    }

    // —— 反馈进度（发布态 / 归档态：迷你进度条 + n/n；编辑态不显示）——
    // 113-4：进度条与计数贴合成组，整组右对齐贴近「作废任务」钮（原计数右留
    // 16px slack、条与计数间只有对齐盒缝，进度条孤悬、四组元素分组关系不清）。
    if (publish || readOnly) {
        const Progress progress = taskProgress(state.task, state.envs, state.services,
                                               state.configs, state.steps);
        const std::string progressText =
            std::to_string(progress.done) + "/" + std::to_string(progress.total);
        const float countW = measureText(progressText, t.sizes.fontSizeSmall, 400) + 8.0f;
        right -= countW;
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
        right -= barW + 8.0f;  // 113-4：条与计数 8px 组内距
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
            bool firstVisibleRow = true;
            for (const auto& row : rows) {
                if (row.hidden) continue;
                const std::string base = "release.detail.track." + std::to_string(&row - rows.data());
                // 裸行分隔口径（need219 §4.4-1）：有容器裸行一律 1px borderMuted、首行不画，
                // 画在行间 gap 中点、行内容之下，行底 hover/current 染色可覆盖。
                if (!firstVisibleRow) {
                    ui.rect(base + ".sep")
                        .position(0.0f, y - kTrackRowGap * 0.5f)
                        .size(width, 1.0f)
                        .color(t.colors.borderMuted)
                        .build();
                }
                firstVisibleRow = false;
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
                        .size(core::theme::scaled(14.0f), core::theme::scaled(14.0f))
                        .text("✓")
                        .fontSize(core::theme::scaled(9.0f))
                        .lineHeight(core::theme::scaled(14.0f))
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
                        options.onClick = [&state] {
                            openCompletionGate(state, "上线公告",
                                               "计入前置完成度并解锁提交校验", "上线公告",
                                               [](ReleaseStore& s,
                                                  ReleaseDetailUiState& st) {
                                                   markAnnouncementSent(s, st);
                                               });
                        };
                    } else if (row.serviceId == 0) {
                        options.onClick = [&state, key = row.prereqKey] {
                            const std::string title = prereqTitle(key);
                            openCompletionGate(state, title,
                                               "计入前置完成度并解锁提交校验", title,
                                               [code = prereqCode(key), title](
                                                   ReleaseStore& s,
                                                   ReleaseDetailUiState& st) {
                                                   markPrereqCompleted(s, st, code,
                                                                       title);
                                               });
                        };
                    } else if (row.config && env != nullptr) {
                        options.onClick = [&state, serviceId = row.serviceId,
                                           kind = row.configKind, envCopy = *env,
                                           nowMinute] {
                            openCompletionGate(
                                state,
                                envCopy.envName + " / " +
                                    (findService(state, serviceId) != nullptr
                                         ? findService(state, serviceId)->serviceName
                                         : "服务") +
                                    " / " + configLabel(kind),
                                "计入任务完成度", configLabel(kind),
                                [serviceId, kind, envCopy, nowMinute](
                                    ReleaseStore& s, ReleaseDetailUiState& st) {
                                    markConfigCompleted(s, st, serviceId, kind, envCopy,
                                                        nowMinute);
                                });
                        };
                    } else if (env != nullptr) {
                        options.onClick = [&state, serviceId = row.serviceId,
                                           kind = row.stepKind, envCopy = *env,
                                           nowMinute] {
                            openCompletionGate(
                                state,
                                envCopy.envName + " / " +
                                    (findService(state, serviceId) != nullptr
                                         ? findService(state, serviceId)->serviceName
                                         : "服务") +
                                    " / " + stepLabel(kind),
                                stepGateImpact(kind), stepLabel(kind),
                                [serviceId, kind, envCopy, nowMinute](
                                    ReleaseStore& s, ReleaseDetailUiState& st) {
                                    markStepCompleted(s, st, serviceId, kind, envCopy,
                                                      nowMinute);
                                });
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
                        .onClick([&state, row, nowMinute]() {
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
                                openCompletionGate(state, "上线公告",
                                                   "计入前置完成度并解锁提交校验",
                                                   "上线公告",
                                                   [](ReleaseStore& s,
                                                      ReleaseDetailUiState& st) {
                                                       markAnnouncementSent(s, st);
                                                   });
                            } else if (row.serviceId == 0) {
                                const std::string title = prereqTitle(row.prereqKey);
                                openCompletionGate(state, title,
                                                   "计入前置完成度并解锁提交校验", title,
                                                   [code = prereqCode(row.prereqKey),
                                                    title](ReleaseStore& s,
                                                           ReleaseDetailUiState& st) {
                                                       markPrereqCompleted(s, st, code,
                                                                           title);
                                                   });
                            } else if (row.config) {
                                openCompletionGate(
                                    state,
                                    env->envName + " / " +
                                        (findService(state, row.serviceId) != nullptr
                                             ? findService(state, row.serviceId)
                                                   ->serviceName
                                             : "服务") +
                                        " / " + configLabel(row.configKind),
                                    "计入任务完成度", configLabel(row.configKind),
                                    [serviceId = row.serviceId,
                                     kind = row.configKind, envCopy = *env, nowMinute](
                                        ReleaseStore& s, ReleaseDetailUiState& st) {
                                        markConfigCompleted(s, st, serviceId, kind,
                                                            envCopy, nowMinute);
                                    });
                            } else {
                                openCompletionGate(
                                    state,
                                    env->envName + " / " +
                                        (findService(state, row.serviceId) != nullptr
                                             ? findService(state, row.serviceId)
                                                   ->serviceName
                                             : "服务") +
                                        " / " + stepLabel(row.stepKind),
                                    stepGateImpact(row.stepKind),
                                    stepLabel(row.stepKind),
                                    [serviceId = row.serviceId,
                                     kind = row.stepKind, envCopy = *env, nowMinute](
                                        ReleaseStore& s, ReleaseDetailUiState& st) {
                                        markStepCompleted(s, st, serviceId, kind,
                                                          envCopy, nowMinute);
                                    });
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
            .size(std::max(0.0f, panelW - 48.0f), core::theme::scaled(26.0f))
            .text("校验未通过")
            .fontSize(t.sizes.fontSizeLarge)
            .lineHeight(core::theme::scaled(26.0f))
            .fontWeight(600)  // 弹窗标题档 600（need322 字重豁免名单：700→600 随迁）
            .color(t.colors.textPrimary)
            .build();
        ui.text("release.detail.validation.hint")
            .position(24.0f, 46.0f)
            .size(std::max(0.0f, panelW - 48.0f), core::theme::scaled(20.0f))
            .text("以下 " + std::to_string(state.validationMissing.size()) +
                  " 项不完整，补齐后再继续：")
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(core::theme::scaled(20.0f))
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
                        .size(innerWidth, core::theme::scaled(22.0f))
                        .text("• " + state.validationMissing[i])
                        .fontSize(itemFontSize)
                        .lineHeight(core::theme::scaled(22.0f))
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

    // 弹窗组件（components/dialog.h build）把面板钳到「视口 − overlay 边距」：
    // 全屏请求实际得到 (screen−48) 的面板。内容坐标系以钳后面板为准；按全屏
    // 假设排布会把底排 保存/取消 与字数行顶出面板底缘、悬到 backdrop 上
    //（#360「按钮跑外面了」）。同文件 drawValidationDialog 同口径。
    const float margin = themeTokens.metrics.spacing.overlay;
    const float panelW = std::max(0.0f, screen.width - margin);
    const float panelH = std::max(0.0f, screen.height - margin);
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

// 「加环境」选层（批次 A-8）：列出基础配置里尚未加入本任务的环境，点一行即
// 按建单同款快照逻辑（EnvDef → TaskEnv + 服务快照）写入并重读详情。
// 为什么复用建单逻辑而不是新写：任务快照与基础配置解耦（改/删基础配置不回灌
// 已建任务），所以加环境也必须做一次快照，口径必须与建单完全一致。
// 「改期」滚轮层（批次 A-8 后半）：复用 components::datePicker（与建单同组件），
// 确认即写库并置 dirty 重读。改期不触碰任何进度字段（任务只是换天，勾选保留）。
void drawReplanDialog(eui::Ui& ui, const eui::Screen& screen,
                      ReleaseDetailUiState& state, ReleaseStore& store, const Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens) {
    (void)t;
    components::datePicker(ui, "release.detail.replan.picker")
        .open(state.replanOpen)
        .screen(screen.width, screen.height)
        .size(380.0f, 260.0f)
        .date(state.replanYear, state.replanMonth, state.replanDay)
        .theme(themeTokens)
        .zIndex(1200)
        .onChange([&state, &store](int year, int month, int day) {
            state.replanYear = year;
            state.replanMonth = month;
            state.replanDay = day;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
            // updateTask 是全字段 UPDATE：改 planDate 后整条回写，其余字段原值
            //（进度、公告、票号都不动——改期只换天，不动任何执行状态）
            ReleaseDetailUiState next = state;
            next.task.planDate = buf;
            auto r = store.updateTask(next.task);
            if (r.isOk()) {
                state.task.planDate = buf;
                state.notice = std::string("已改期到 ") + buf;
                state.noticeIsError = false;
                state.dirty = true;
            } else {
                state.notice = "改期失败：" + r.errorMessage();
                state.noticeIsError = true;
            }
            state.replanOpen = false;
        });
}

void drawAddEnvDialog(eui::Ui& ui, const eui::Screen& screen,
                      ReleaseDetailUiState& state, ReleaseStore& store, const Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens) {
    const std::string id = "release.detail.addenv";
    const float rowH = 40.0f;
    const std::size_t n = state.addEnvCandidates.size();
    const float listH = std::max(rowH, rowH * static_cast<float>(n));
    ui_kit::DialogShellSpec spec;
    spec.id = id;
    spec.width = ui_kit::DialogWidth::Standard;
    spec.height = 200.0f + listH;
    spec.open = state.addEnvOpen;
    spec.title = "添加环境";
    spec.cancel = ui_kit::DialogShellButton{"关闭",
                                           [&state] { state.addEnvOpen = false; }, true};
    spec.onOpenChange = [&state](bool open) { state.addEnvOpen = open; };
    ui_kit::drawDialogShell(ui, screen, spec, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
        if (n == 0) {
            ui.text(id + ".empty")
                .position(area.x, area.y)
                .size(area.w, core::theme::scaled(40.0f))
                .text("基础配置里的环境都已加入本任务")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(core::theme::scaled(40.0f))
                .color(t.colors.textTertiary)
                .build();
            return;
        }
        float y = area.y;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string rowId = id + ".row." + std::to_string(i);
            const std::int64_t envId = state.addEnvCandidates[i];
            ui.stack(rowId)
                .position(area.x, y)
                .size(area.w, rowH)
                .content([&] {
                    ui.rect(rowId + ".bg")
                        .size(area.w, rowH)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radius)
                        .build();
                    // 命中层：整行可点（透明 rect + onClick，与 step 行同款）
                    ui.rect(rowId + ".hit")
                        .position(0.0f, 0.0f)
                        .size(area.w, rowH)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .onClick([&store, &state, envId] {
                            auto result = store.addEnvFromDef(envId, state.taskId);
                            if (result.isOk()) {
                                state.addEnvOpen = false;
                                state.notice = "已添加环境";
                                state.noticeIsError = false;
                                state.dirty = true;  // 重读详情（新环境 + 服务快照）
                            } else {
                                state.notice = "添加失败：" + result.errorMessage();
                                state.noticeIsError = true;
                            }
                        })
                        .build();
                    ui.text(rowId + ".name")
                        .position(12.0f, 0.0f)
                        .size(area.w - 24.0f, rowH)
                        .text(state.addEnvNames[i])
                        .fontSize(t.sizes.fontSizeBody)
                        .lineHeight(rowH)
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();
            y += rowH + 4.0f;
        }
    });
}

void drawAddEnvDialog(eui::Ui& ui, const eui::Screen& screen,
                      ReleaseDetailUiState& state, ReleaseStore& store, const Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens);

void drawConfirmDialogs(eui::Ui& ui, const eui::Screen& screen,
                        ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute,
                        const Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
    if (state.confirm == ReleaseDetailConfirm::None) return;
    if (state.confirm == ReleaseDetailConfirm::ValidationReport) {
        drawValidationDialog(ui, screen, state, t, themeTokens);
        return;
    }
    if (state.confirm == ReleaseDetailConfirm::StepComplete) {
        drawStepGateDialog(ui, screen, state, store, t, themeTokens);
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
