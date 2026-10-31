// release_detail_draw.cpp
// 从 release_detail_ui.cpp 拆出（2026-10-31）：绘制层——度量、卡片、步骤行、
// 工单卡、环境分区，以及校验 / 配置编辑 / 确认弹窗。只读 state 与 logic 层查询。

#include "release_detail_internal.h"

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

void drawMiniArrow(eui::Ui& ui, const std::string& id, float x, float y,
                   bool up, bool enabled, const Tokens& t,
                   const std::function<void()>& onClick) {
    auto rect = ui.rect(id)
                    .position(x, y)
                    .size(kArrowW, kArrowW)
                    .color(t.colors.hover)
                    .radius(t.sizes.radiusSmall)
                    .opacity(enabled ? 1.0f : 0.4f);
    if (enabled && onClick) rect.onClick(onClick);
    rect.build();
    ui.text(id + ".icon")
        .position(x, y)
        .size(kArrowW, kArrowW)
        .icon(up ? 0xF077u : 0xF078u)
        .fontSize(core::theme::scaled(10.0f))
        .lineHeight(kArrowW)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .opacity(enabled ? 1.0f : 0.4f)
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

int prereqColumns(float availableWidth, int count) {
    if (count <= 0) return 1;
    const float columns = std::floor((availableWidth + kPrereqGap) /
                                     (kPrereqCardMinW + kPrereqGap));
    return std::clamp(static_cast<int>(columns), 1, count);
}

float prereqSectionHeight(float width, int count) {
    if (count <= 0) return kSectionTitleH;
    const int cols = prereqColumns(width, count);
    const int rows = (count + cols - 1) / cols;
    return kSectionTitleH + kSectionGap +
           static_cast<float>(rows) * kPrereqCardH +
           static_cast<float>(rows - 1) * kPrereqGap;
}

void drawPrereqSection(eui::Ui& ui, float width, ReleaseDetailUiState& state,
                       bool readOnly, int nowMinute, const Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens,
                       ReleaseStore& store) {
    const TicketNeeds needs = scanTicketNeeds(state);
    std::vector<PrereqView> cards;
    const PrereqKey baseKeys[2] = {PrereqKey::Pretest, PrereqKey::Cab};
    for (PrereqKey key : baseKeys) {
        PrereqView view;
        view.key = key;
        view.title = prereqTitle(key);
        view.checkLabel = prereqCheckLabel(key);
        view.checked = prereqChecked(state, key);
        view.value = prereqValue(state, key);
        view.gateOpen = prereqGateOpen(state, key);
        cards.push_back(std::move(view));
    }
    if (needs.nacos) {
        PrereqView view;
        view.key = PrereqKey::Nacos;
        view.title = prereqTitle(PrereqKey::Nacos);
        view.checkLabel = prereqCheckLabel(PrereqKey::Nacos);
        view.checked = prereqChecked(state, PrereqKey::Nacos);
        view.value = prereqValue(state, PrereqKey::Nacos);
        view.gateOpen = prereqGateOpen(state, PrereqKey::Nacos);
        cards.push_back(std::move(view));
    }
    if (needs.db) {
        PrereqView view;
        view.key = PrereqKey::Db;
        view.title = prereqTitle(PrereqKey::Db);
        view.checkLabel = prereqCheckLabel(PrereqKey::Db);
        view.checked = prereqChecked(state, PrereqKey::Db);
        view.value = prereqValue(state, PrereqKey::Db);
        view.gateOpen = prereqGateOpen(state, PrereqKey::Db);
        cards.push_back(std::move(view));
    }
    {
        PrereqView view;
        view.key = PrereqKey::Announce;
        view.title = prereqTitle(PrereqKey::Announce);
        view.checkLabel = prereqCheckLabel(PrereqKey::Announce);
        view.checked = prereqChecked(state, PrereqKey::Announce);
        view.value = prereqValue(state, PrereqKey::Announce);
        view.gateOpen = prereqGateOpen(state, PrereqKey::Announce);
        cards.push_back(std::move(view));
    }

    const std::int64_t baseline = taskBaseline(state.envs);
    // 到期状态只读 release_logic 的聚合结果，不在 UI 重算「基准 − 60」的触发时刻。
    // 卡片标题是 dueItems 标题的子串（提测单 / CAB 单 / nacos 工单 / 数据库工单 /
    // 上线公告），据此把条目对回卡片；已完成的项不会出现在 dueItems 里。
    // 走 canonical 6 参入口：state.steps 是本任务全部步骤的汇总（loadDetail 按服务
    // 收集），与提醒引擎同源——步骤全做完的环境不再因空 steps 在详情页假红。
    const std::vector<DueItem> due = dueItems(state.task, state.envs, state.services,
                                              state.configs, state.steps, nowMinute);
    const int cols = prereqColumns(width, static_cast<int>(cards.size()));
    const float cardW = cols > 0
        ? (width - kPrereqGap * static_cast<float>(cols - 1)) / static_cast<float>(cols)
        : width;
    const float sectionH = prereqSectionHeight(width, static_cast<int>(cards.size()));

    ui.stack("release.detail.prereq.section")
        .size(width, sectionH)
        .content([&] {
            drawSectionTitle(ui, "release.detail.prereq.title", 0.0f, 0.0f, "任务前置", t);
            for (std::size_t i = 0; i < cards.size(); ++i) {
                PrereqView& card = cards[i];
                const int col = static_cast<int>(i) % cols;
                const int row = static_cast<int>(i) / cols;
                const float x = static_cast<float>(col) * (cardW + kPrereqGap);
                const float y = kSectionTitleH + kSectionGap +
                                static_cast<float>(row) * (kPrereqCardH + kPrereqGap);
                const bool announce = card.key == PrereqKey::Announce;
                const bool doc = !announce;

                card.overdue = false;
                if (!card.checked) {
                    for (const auto& item : due) {
                        if (item.scope == "task" &&
                            containsAsciiCaseInsensitive(item.title, card.title)) {
                            card.overdue = true;
                            break;
                        }
                    }
                }
                if (baseline < 0) {
                    card.dueText = "到期时间未定义";
                } else if (announce) {
                    // 公告到期时间 = 当天最早环境的开始时间（taskBaseline）。
                    card.dueText = (card.checked ? "已确认 · " : "到期 ") +
                                   formatPlanMinute(static_cast<int>(baseline)) +
                                   "（最早环境）";
                } else {
                    card.dueText = "到期：时间基准前 60 分钟";
                }

                const std::string id = "release.detail.prereq." + std::to_string(i);
                drawCardBackground(ui, id + ".bg", x, y, cardW, kPrereqCardH, t,
                                   themeTokens, false);
                ui.text(id + ".title")
                    .position(x + 16.0f, y + 14.0f)
                    .size(std::max(0.0f, cardW - 32.0f), 20.0f)
                    .text(card.title)
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(20.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".due")
                    .position(x + 16.0f, y + 36.0f)
                    .size(std::max(0.0f, cardW - 32.0f), 16.0f)
                    .text(card.dueText)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(16.0f)
                    .maxWidth(std::max(0.0f, cardW - 32.0f))
                    .overflow(core::TextOverflow::Ellipsis)
                    .color(card.overdue ? t.colors.errorText : t.colors.textTertiary)
                    .build();

                const float inputX = x + 16.0f;
                const float inputY = y + 58.0f;
                const float inputW = std::max(0.0f, cardW - 32.0f);
                const float inputH = announce ? 60.0f : 40.0f;
                if (readOnly) {
                    drawReadOnlyValue(ui, id + ".input", inputX, inputY, inputW, inputH,
                                      card.value,
                                      doc ? "未填写单号" : "未填写公告内容", announce, t);
                } else {
                    components::input(ui, id + ".input")
                        .position(inputX, inputY)
                        .size(inputW, inputH)
                        .value(card.value)
                        .placeholder(doc ? "填写单号" : "填写上线公告")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(announce)
                        .scrollbar(announce)
                        .inset(announce ? 8.0f : 12.0f)
                        .theme(themeTokens)
                        .onChange([&state, key = card.key](const std::string& value) {
                            setPrereqValue(state, key, value);
                        })
                        .onFocus([&store, &state](bool focused) {
                            if (!focused) saveTask(store, state);
                        })
                        .onEnter([&store, &state]() { saveTask(store, state); })
                        .onCancel([&store, &state]() { saveTask(store, state); })
                        .build();
                }

                const float checkY = y + (announce ? 126.0f : 112.0f);
                // 单据号未填时禁用：灰态、不可点，标签位置改为提示文案。
                const bool showHint = !readOnly && !card.checked && !card.gateOpen;
                ReleaseCheckboxOptions check;
                check.checked = card.checked;
                check.enabled = !readOnly && (card.gateOpen || card.checked);
                check.label = card.checkLabel;
                check.hint = showHint
                                 ? (doc ? "请先填写单据号" : "请先填写公告内容")
                                 : std::string();
                check.boxSize = kCheckSize;
                check.fontSize = t.sizes.fontSizeSmall;
                check.width = std::max(0.0f, cardW - 32.0f);
                check.height = kCheckSize;
                check.inset = 0.0f;
                check.gap = 8.0f;
                std::function<void()> onCheck;
                if (!readOnly) {
                    const std::string code = prereqCode(card.key);
                    const std::string label = card.checkLabel;
                    onCheck = [&store, &state, key = card.key, code, label]() {
                        if (isReadOnly(state.displayStatus)) return;
                        if (prereqChecked(state, key)) {
                            openRevokePrereq(state, code, label,
                                             key == PrereqKey::Announce);
                            return;
                        }
                        if (key == PrereqKey::Announce) {
                            markAnnouncementSent(store, state);
                        } else {
                            markPrereqCompleted(store, state, code, label);
                        }
                    };
                }
                drawReleaseCheckboxAt(ui, id + ".check", inputX, checkY, check,
                                      onCheck, t);
            }
        })
        .build();
}

// ==================== 环境 / 服务 ====================

float serviceCardHeight(const ReleaseDetailUiState& state, const TaskService& svc) {
    const float rows = static_cast<float>(3 + orderedConfigs(state, svc).size());
    return 68.0f + rows * kStepRowH + (rows - 1.0f) * kStepRowGap;
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

void drawStepRow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const StepRowView& row, bool current, bool readOnly,
                 bool configurable, bool readOnlyViewAllowed, const Tokens& t,
                 const std::function<void()>& onRowClick,
                 const std::function<void()>& onView,
                 const std::function<void()>& onMoveUp,
                 const std::function<void()>& onMoveDown,
                 const std::function<void()>& onSkipToggle) {
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

    const float viewX = rowX + rowW - 8.0f - kViewButtonW;
    const float arrowsX = viewX - kArrowGap - (kArrowW * 2.0f + kArrowGap);
    // 可排序的配置行在最左侧预留拖拽手柄位（⋮⋮）；手柄由 drawServiceCard 叠加绘制。
    const bool showDragHandle = row.config && configurable;
    const float nameX = dotX + kStepDot +
                        (showDragHandle ? kDragHandleW + kDragHandleGap : 10.0f);
    // 「跳过」勾选框（未完成配置行，readOnly 时 onSkipToggle 为空不出现）：
    // 可排序时靠排序箭头左侧，执行中（不可排序）靠「查看」按钮左侧。
    ReleaseCheckboxOptions skipOptions;
    float skipToggleW = 0.0f;
    float skipToggleX = 0.0f;
    if (row.config && !row.done && onSkipToggle) {
        skipOptions.checked = row.skipped;
        skipOptions.enabled = true;
        skipOptions.label = "跳过";
        skipOptions.boxSize = kCheckSize;
        skipOptions.fontSize = t.sizes.fontSizeSmall;
        skipOptions.height = kStepRowH;
        skipOptions.inset = 0.0f;
        skipOptions.gap = 6.0f;
        skipToggleW = checkbox_detail::stackWidth(skipOptions);
        const float anchor = configurable ? arrowsX : viewX;
        skipToggleX = anchor - kArrowGap - skipToggleW;
    }
    float nameRight = rowX + rowW - 8.0f;
    if (row.config) {
        nameRight = readOnly ? viewX - 8.0f
                             : (skipToggleW > 0.0f ? skipToggleX - 8.0f
                                                   : arrowsX - 8.0f);
    }
    const float nameW = std::max(0.0f, nameRight - nameX);

    const std::string label = row.invalid ? row.label + " · 内容为空" : row.label;
    eui::Color labelColor = row.done ? t.colors.textPrimary
                             : (row.skipped ? t.colors.textDisabled
                                            : (current ? t.colors.textPrimary
                                                       : t.colors.textSecondary));
    if (row.invalid) labelColor = t.colors.errorText;
    ui.text(id + ".label")
        .position(nameX, y)
        .size(nameW, kStepRowH)
        .text(label)
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
        const float skipX = nameX + labelW + 8.0f;
        const float skipW = std::max(0.0f, nameRight - skipX);
        if (skipW > 28.0f) {
            ui.text(id + ".skip")
                .position(skipX, y)
                .size(skipW, kStepRowH)
                .text("已跳过")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(kStepRowH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        }
    }

    if (row.config && readOnlyViewAllowed && onView) {
        ui_kit::ButtonOptions options;
        options.onClick = onView;
        ui_kit::button(ui, id + ".view", "查看", ui_kit::ButtonLevel::Text, viewX, y,
                       kViewButtonW, kStepRowH, t, options);
    }
    if (row.config && configurable) {
        const bool canUp = row.configIndex > 0;
        const bool canDown = row.configIndex + 1 < row.configCount;
        drawMiniArrow(ui, id + ".up", arrowsX, y + (kStepRowH - kArrowW) * 0.5f, true,
                      canUp, t, onMoveUp);
        drawMiniArrow(ui, id + ".down", arrowsX + kArrowW + kArrowGap,
                      y + (kStepRowH - kArrowW) * 0.5f, false, canDown, t, onMoveDown);
    }
    // 最后画勾选框，保证命中区盖在行点击命中区之上。
    if (skipToggleW > 0.0f) {
        drawReleaseCheckboxAt(ui, id + ".skip.toggle", skipToggleX, y, skipOptions,
                              onSkipToggle, t);
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
    const bool hasBranch = !svc.branch.empty();
    const bool branchFlagged = serviceBranchFlagged(state, svc.serviceName);
    if (branchFlagged) {
        ui.rect(id + ".branch.flag")
            .position(nameX - 4.0f, branchY - 2.0f)
            .size(nameW + 8.0f, 20.0f)
            .color(eui::mixColor(t.colors.surface, t.colors.error, 0.10f))
            .radius(4.0f)
            .build();
    }
    ui.text(id + ".branch")
        .position(nameX, branchY)
        .size(nameW, 16.0f)
        .text(hasBranch ? "分支 " + svc.branch
                        : (branchFlagged ? "分支未填写 · 提交校验未通过"
                                         : "分支未填写"))
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
        .maxWidth(nameW)
        .overflow(core::TextOverflow::Ellipsis)
        .color(branchFlagged || !hasBranch ? t.colors.errorText : t.colors.textTertiary)
        .build();

    std::vector<StepRowView> rows;
    {
        StepRowView merge;
        merge.id = "merge";
        merge.label = stepLabel(StepKind::MergeBranch);
        merge.done = stepDone(state, svc.id, StepKind::MergeBranch);
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
            rows.push_back(std::move(row));
        }

        StepRowView deploy;
        deploy.id = "deploy";
        deploy.label = stepLabel(StepKind::Deploy);
        deploy.done = stepDone(state, svc.id, StepKind::Deploy);
        deploy.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
        deploy.stepKind = StepKind::Deploy;
        rows.push_back(std::move(deploy));

        StepRowView verify;
        verify.id = "verify";
        verify.label = stepLabel(StepKind::Verify);
        verify.done = stepDone(state, svc.id, StepKind::Verify);
        verify.timeReady = nowMinute >= 0 && nowMinute >= env.startMinute;
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
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const StepRowView& row = rows[i];
        const float rowY = y + kServiceRowStart +
                           static_cast<float>(i) * (kStepRowH + kStepRowGap);
        const std::string rowId = id + ".step." + row.id;

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
            // 配置查看 / 编辑 = 页内自绘全屏编辑层（不再经 actions.viewConfig）。
            onView = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                openConfigEditor(store, state, serviceId, kind);
            };
        }

        std::function<void()> onMoveUp;
        std::function<void()> onMoveDown;
        if (row.config) {
            onMoveUp = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                moveConfig(store, state, serviceId, kind, -1);
            };
            onMoveDown = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                moveConfig(store, state, serviceId, kind, 1);
            };
        }

        std::function<void()> onSkipToggle;
        if (row.config && !readOnly) {
            onSkipToggle = [&store, &state, serviceId = svc.id, kind = row.configKind] {
                toggleConfigSkip(store, state, serviceId, kind);
            };
        }
        drawStepRow(ui, rowId, x, rowY, width, row, i == currentIndex, readOnly,
                    configurable, true, t, onRowClickWithRevoke, onView, onMoveUp,
                    onMoveDown, onSkipToggle);

        if (row.config && configurable) {
            const float handleX = x + 8.0f + 10.0f + kStepDot + 4.0f;
            const float handleY = rowY + (kStepRowH - kDragHandleH) * 0.5f;
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

    // 配置拖拽的插入位指示线：画在目标边界（行间）。
    if (configurable && state.drag.kind == ReleaseDetailDragKind::ConfigOrder &&
        state.drag.active && state.drag.serviceId == svc.id) {
        const std::vector<TaskConfig> allConfigs = orderedConfigs(state, svc);
        const int source = state.drag.sourceIndex;
        const int target = state.drag.targetIndex;
        if (!allConfigs.empty() && source >= 0 && target >= 0 &&
            source < static_cast<int>(allConfigs.size())) {
            const int boundary = target < source ? target : target + 1;
            const float step = kStepRowH + kStepRowGap;
            const float firstConfigY = y + kServiceRowStart + step;
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
                    ReleaseStore& store) {
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

void drawPageHeader(eui::Ui& ui, float width, const Tokens& t,
                    ReleaseDetailUiState& state, ReleaseStore& store, int nowMinute,
                    const ReleaseDetailActions& actions) {
    const float headerH = ui_kit::kHeaderH;
    const TaskStatus status = state.displayStatus;
    const std::string weekday = releaseWeekdayLabel(state.task.planDate);
    const Progress progress =
        taskProgress(state.task, state.envs, state.services, state.configs, state.steps);
    const bool readOnly = isReadOnly(status);

    // 「← 返回」由模块注入 onBack 才出现（默认空 = 不占位）；Esc 返回是模块层既有路径。
    const auto backFn = actions.onBack;
    float titleX = 0.0f;
    if (backFn) {
        ui_kit::ButtonOptions backOptions;
        backOptions.onClick = backFn;
        ui_kit::button(ui, "release.detail.back", "← 返回", ui_kit::ButtonLevel::Text, 0.0f,
                       0.0f, 72.0f, headerH, t, backOptions);
        titleX = 72.0f + 8.0f;
    }

    float right = width;
    float voidX = 0.0f;
    float restoreX = 0.0f;
    float nextX = 0.0f;
    float submitX = 0.0f;
    bool showVoid = false;
    bool showRestore = false;
    bool showNext = false;
    // 「提交」只在配置中可用（Draft→Pending 的唯一入口）；其余状态不出按钮。
    const bool showSubmit = status == TaskStatus::Draft;
    if (status == TaskStatus::Voided) {
        restoreX = right - 80.0f;
        right = restoreX - 8.0f;
        showRestore = true;
    } else {
        voidX = right - 80.0f;
        right = voidX - 8.0f;
        showVoid = true;
        if (!readOnly) {
            nextX = right - 96.0f;
            right = nextX - 8.0f;
            showNext = true;
            if (showSubmit) {
                submitX = right - 88.0f;
                right = submitX - 8.0f;
            }
        }
    }
    // 「校验」任何状态可见（独立只读校验）；Done / Voided 整单只读时禁用。
    const float validateX = right - 72.0f;
    right = validateX - 8.0f;

    const std::string progressText =
        std::to_string(progress.done) + "/" + std::to_string(progress.total);
    ui.text("release.detail.progress")
        .position(right - 56.0f, 0.0f)
        .size(56.0f, headerH)
        .text(progressText)
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(headerH)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(readOnly ? t.colors.textTertiary : t.colors.textSecondary)
        .build();
    right -= 56.0f + 8.0f;

    const ChipColors colors = statusChipColors(status, t);
    const std::string statusText = releaseTaskStatusText(status);
    const float chipTextW = chipWidth(statusText, t.sizes.fontSizeSmall - 1.0f);
    drawChip(ui, "release.detail.status", right - chipTextW, (headerH - kChipH) * 0.5f,
             statusText, colors.bg, colors.fg, t);
    right -= chipTextW + 12.0f;

    // 标题宽按字号档实测（130% 字号下固定 96 会裁掉最后一个字）。
    const float titleW = measureText("上线任务", t.sizes.fontSizeTitle, 700) + 8.0f;
    const float metaX = titleX + titleW + 8.0f;
    ui.text("release.detail.title")
        .position(titleX, 0.0f)
        .size(titleW, headerH)
        .text("上线任务")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(headerH)
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    const std::string meta = state.task.planDate + (weekday.empty() ? "" : "  " + weekday);
    ui.text("release.detail.meta")
        .position(metaX, 0.0f)
        .size(std::max(0.0f, right - metaX), headerH)
        .text(meta)
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(headerH)
        .maxWidth(std::max(0.0f, right - metaX))
        .overflow(core::TextOverflow::Ellipsis)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .build();

    if (showNext) {
        ui_kit::ButtonOptions options;
        options.onClick = [&store, &state, nowMinute] {
            performNextStep(store, state, nowMinute);
        };
        // 同一视口主按钮唯一：配置中「提交」是主动作，「下一步」降为次要按钮。
        ui_kit::button(ui, "release.detail.next", "下一步",
                       showSubmit ? ui_kit::ButtonLevel::Secondary
                                  : ui_kit::ButtonLevel::Primary,
                       nextX, 0.0f, 96.0f, headerH, t, options);
    }
    if (showSubmit) {
        ui_kit::ButtonOptions options;
        options.onClick = [&store, &state] { submitTask(store, state); };
        ui_kit::button(ui, "release.detail.submit", "提交", ui_kit::ButtonLevel::Primary,
                       submitX, 0.0f, 88.0f, headerH, t, options);
    }
    if (showVoid) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state] { state.confirm = ReleaseDetailConfirm::VoidTask; };
        ui_kit::button(ui, "release.detail.void", "作废", ui_kit::ButtonLevel::Danger,
                       voidX, 0.0f, 80.0f, headerH, t, options);
    }
    if (showRestore) {
        ui_kit::ButtonOptions options;
        options.onClick = [&state] {
            state.confirm = ReleaseDetailConfirm::RestoreTask;
        };
        ui_kit::button(ui, "release.detail.restore", "恢复",
                       ui_kit::ButtonLevel::Secondary, restoreX, 0.0f, 80.0f, headerH, t,
                       options);
    }
    // 独立校验：只读动作，不改任务状态；只读整单时置灰且无点击动作。
    ui_kit::ButtonOptions validateOptions;
    if (!readOnly) validateOptions.onClick = [&state] { runValidation(state); };
    if (readOnly) {
        validateOptions.hasTextColorOverride = true;
        validateOptions.textColorOverride = t.colors.textDisabled;
    }
    ui_kit::button(ui, "release.detail.validate", "校验",
                   readOnly ? ui_kit::ButtonLevel::Text : ui_kit::ButtonLevel::Secondary,
                   validateX, 0.0f, 72.0f, headerH, t, validateOptions);
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
