// Todo 看板区绘制：drawHeader/drawBoard/drawColumn/drawColumnContent/drawCard（数据显式入参 BoardView + BoardActions 回写）
#include "todo_ui_board.h"

#include "todo_ui_common.h"

#include <core/component_theme.h>
#include <core/theme.h>

#include <components/button.h>
#include <components/card.h>
#include <components/input.h>
#include <components/scrollview.h>

#include <algorithm>
#include <string>
#include <vector>

namespace module::todo::ui {

// 列/列内容/卡片绘制仅文件内互调（对外只有 drawHeader/drawBoard）
void drawColumn(eui::Ui& ui, const std::string& id, const Column& col,
                float width, float height, const core::theme::Tokens& t,
                size_t colIndex, const BoardView& v, BoardPlate& plate);
void drawColumnContent(eui::Ui& ui, const std::string& id, const Column& col,
                       float width, float height, const core::theme::Tokens& t,
                       size_t colIndex, const BoardView& v, BoardPlate& plate);
void drawCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
              const core::theme::Tokens& t, const Task& task,
              int colIndex, int colCount, int cardIndex,
              const BoardView& v, BoardPlate& plate);

// 右侧动作区 560px = 搜索框(240)+逾期(104)+「+ 列」(72)+「+ 新建任务」(120)+间距
void drawHeader(eui::Ui& ui, float contentW, float headerH, float bodyH,
                const core::theme::Tokens& t, const BoardView& v) {
    (void)bodyH;
    const auto componentTheme = core::theme::componentTokens(t);
    ui.row("todo.header")
        .size(contentW, headerH)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.column("todo.title")
                .size(contentW - 560.0f, headerH)
                .gap(2.0f)
                .content([&] {
                    ui.text("todo.h1")
                        .size(contentW - 560.0f, 32.0f)
                        .text("Todo 看板")
                        .fontSize(t.sizes.fontSizeTitle)
                        .lineHeight(32.0f)
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                    // subtitle 保持静态操作说明（动态反馈走 toast）；clip 栈作几何兜底（窄窗文案不压右侧动作区）
                    ui.stack("todo.subtitle.clip")
                        .size(contentW - 560.0f, 18.0f)
                        .clip()
                        .content([&] {
                            ui.text("todo.subtitle")
                                .size(contentW - 560.0f, 18.0f)
                                .text("点击卡片查看详情 · Ctrl+↑/↓ 移动选中任务 · 右键卡片/列头更多操作 · 拖动卡片跨列移动")
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
                                .maxWidth(contentW - 560.0f)
                                .overflow(core::TextOverflow::Ellipsis)
                                .color(t.colors.textSecondary)
                                .build();
                        })
                        .build();
                })
                .build();

            ui.row("todo.actions")
                .size(560.0f, 40.0f)
                .justifyContent(eui::Align::END)
                .alignItems(eui::Align::CENTER)
                .gap(t.sizes.spacingSmall)
                .content([&] {
// 搜索框：Enter 无副作用（不绑 onEnter）；包 stack 才能在流式 row 里叠 FA 图标浮层
                    ui.stack("todo.search.wrap")
                        .size(240.0f, 40.0f)
                        .content([&] {
                            components::input(ui, "todo.search")
                                .size(240.0f, 40.0f)
                                .value(v.searchQuery)
                                .placeholder("搜索任务…")
                                .fontSize(t.sizes.fontSizeSmall)
                                .inset(30.0f)
                                .theme(componentTheme)
                                .onChange([cb = v.act.searchChanged](
                                              const std::string& text) {
                                    cb(text);
                                })
                                .onCancel([cb = v.act.searchCancelled] { cb(); })
                                .build();
                            ui.text("todo.search.icon")
                                .position(14.0f, 12.0f)
                                .size(16.0f, 16.0f)
                                .icon(0xF002)  // fa-magnifying-glass
                                .fontSize(12.0f)
                                .lineHeight(16.0f)
                                .color(t.colors.textTertiary)
                                .build();
                        })
                        .build();

                    // 逾期入口：badge = listOverdue().size()，0 时置灰不可点
                    const bool hasOverdue = v.overdueCount > 0;
                    components::button(ui, "todo.btn.overdue")
                        .size(104.0f, 40.0f)
                        .text("逾期 " + std::to_string(v.overdueCount))
                        .icon(0xF071)
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(hasOverdue ? t.colors.error : t.colors.surfaceElevated,
                                hasOverdue ? t.colors.error : t.colors.hover,
                                hasOverdue ? t.colors.error : t.colors.active)
                        .textColor(hasOverdue ? t.colors.onPrimary
                                              : t.colors.textDisabled)
                        .iconColor(hasOverdue ? t.colors.onPrimary
                                              : t.colors.textDisabled)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .onClick([cb = v.act.overdueClicked] { cb(); })
                        .build();

                    components::button(ui, "todo.btn.addcol")
                        .size(72.0f, 40.0f)
                        .text("+ 列")
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                        .textColor(t.colors.textPrimary)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .onClick([cb = v.act.addColumnClicked] { cb(); })
                        .build();

                    components::button(ui, "todo.btn.add")
                        .size(120.0f, 40.0f)
                        .text("+ 新建任务")
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.primary, t.colors.primaryHover,
                                t.colors.primaryPressed)
                        .textColor(t.colors.onPrimary)
                        .radius(t.sizes.radius)
                        .onClick([cb = v.act.addTaskClicked] { cb(); })
                        .build();
                })
                .build();
        })
        .build();
}

void drawBoard(eui::Ui& ui, float contentW, float bodyH,
               const core::theme::Tokens& t, const BoardView& v, BoardPlate& plate) {
    // 错误态（服务未就绪/读取失败）：与空态区分，给文案 + 重试按钮
    if (!v.hasService || !v.loadError.empty()) {
        const std::string msg = !v.hasService
            ? "数据库尚未就绪，数据未加载"
            : "数据加载失败：" + v.loadError;
        ui.stack("todo.error")
            .size(contentW, bodyH)
            .content([&] {
                ui.text("todo.error.text")
                    .position(0.0f, bodyH / 2.0f - 36.0f)
                    .size(contentW, 24.0f)
                    .text(msg + "（点击重试）")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(24.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.error)
                    .build();
                components::button(ui, "todo.error.retry")
                    .position(contentW / 2.0f - 60.0f, bodyH / 2.0f)
                    .size(120.0f, 40.0f)
                    .text("重试")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([cb = v.act.retryClicked] { cb(); })
                    .build();
            })
            .build();
        return;
    }

    if (v.columns.empty()) {
        ui.stack("todo.empty")
            .size(contentW, bodyH)
            .content([&] {
                ui.text("todo.empty.text")
                    .position(0.0f, bodyH / 2.0f - 12.0f)
                    .size(contentW, 24.0f)
                    .text("还没有任何列，点击右上角「+ 列」开始")
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(24.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
        return;
    }

    const float colGap = t.sizes.spacingLarge;
    const int n = static_cast<int>(v.columns.size());
    const float colW = (contentW - colGap * static_cast<float>(n - 1)) /
                       static_cast<float>(n);

    // 板面几何快照（拖拽命中换算用；compose 每帧刷新）
    plate.colW = colW;
    plate.colGap = colGap;
    plate.height = bodyH;

    ui.row("todo.body")
        .size(contentW, bodyH)
        .gap(colGap)
        .content([&] {
            for (size_t ci = 0; ci < v.columns.size(); ++ci) {
                drawColumn(ui, "todo.col." + std::to_string(ci),
                           v.columns[ci], colW, bodyH, t, ci, v, plate);
            }
        })
        .build();
}

void drawColumn(eui::Ui& ui, const std::string& id, const Column& col,
                float width, float height, const core::theme::Tokens& t,
                size_t colIndex, const BoardView& v, BoardPlate& plate) {
    // 包一层 stack：position() 只在 stack 内生效，作为 row 子元素会被流式横排（列头/卡片错位）
    ui.stack(id)
        .size(width, height)
        .content([&] {
            drawColumnContent(ui, id, col, width, height, t, colIndex, v, plate);
        })
        .build();
}

void drawColumnContent(eui::Ui& ui, const std::string& id, const Column& col,
                       float width, float height,
                       const core::theme::Tokens& t, size_t colIndex,
                       const BoardView& v, BoardPlate& plate) {
    // 拖动中指针所在列高亮（drop 目标提示）
    const bool dropHover = v.dragActive &&
        v.dragHoverColumn == static_cast<int>(colIndex);

    // 拖拽插入位占位条（半透主色条）。barY 在滚动内容坐标系（与卡片同帧）；row=0 顶格、row=列卡数=列尾追加
    const bool showDropBar = v.dragActive &&
        v.dragHoverColumn == static_cast<int>(colIndex) && v.dragHoverRow >= 0;
    auto drawDropBar = [&](eui::Ui& inner, float barY, float barW) {
        eui::Color barColor = t.colors.primary;
        barColor.a = 0.55f;  // 高亮占位（半透主色）
        // ignoreLayout：scrollView 内容列按流式排布、position 无效；置位后走 overlay 绝对定位
        inner.rect(id + ".dropbar")
            .position(kColumnContentInset, barY)
            .ignoreLayout()
            .size(barW, kCardGap)
            .color(barColor)
            .radius(t.sizes.radiusSmall)
            .build();
    };

    ui.rect(id + ".bg")
        .position(0.0f, 0.0f)
        .size(width, height)
        // 列容器是页面结构 → background；卡片白只给列内浮动卡片与按钮
        .color(t.colors.background)
        .radius(t.sizes.radiusLarge)
        .border(dropHover ? 2.0f : 1.0f,
                dropHover ? t.colors.primary : t.colors.borderDefault)
        .build();

    auto colTasks = tasksInColumn(v.tasks, col.id);
    // 搜索激活时只显示命中卡（命中集来自 service_->search）
    colTasks = filterBySearch(colTasks, v.searchMatchIds, !v.searchQuery.empty());
    // 归档列语义走 kind；按标题判会在重命名后失效
    const bool isArchive = (col.kind == "archive");
    // 归档列排序强制按 archived_at DESC（最近的在最上面），忽略手动 position——列内拖拽排序无效化
    auto archiveTasks = isArchive ? tasksInArchiveColumn(v.tasks, col.id) : std::vector<const Task*>{};
    auto archiveFiltered = isArchive
        ? filterBySearch(archiveTasks, v.searchMatchIds, !v.searchQuery.empty())
        : std::vector<const Task*>{};
    const auto& renderTasks = isArchive ? archiveFiltered : colTasks;
    const size_t taskCount = renderTasks.size();

    // 列头右键热区。纯命中区必须显式给全透明：Rect 默认不透明白色，深色主题会刷白
    ui.rect(id + ".head")
        .position(0.0f, 0.0f)
        .size(width, kColumnHeaderHeight)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .radius(t.sizes.radiusLarge)
        .onContextMenu([cb = v.act.columnMenu, col, colIndex](
                           const eui::PointerEvent&, const eui::Rect&) {
            cb(col, static_cast<int>(colIndex));
        })
        .build();

    // 列颜色点（10px 圆点，token→hex 见 ui::columnDotColor）
    const auto colDot = columnDotColor(col.color);
    if (colDot.has_value()) {
        ui.rect(id + ".dot")
            .position(16.0f, 21.0f)
            .size(10.0f, 10.0f)
            .color(*colDot)
            .radius(5.0f)
            .build();
    }

    ui.text(id + ".title")
        .position(colDot.has_value() ? 34.0f : 16.0f, 16.0f)
        // 右侧留到 width-88：给 ✎ 重命名（width-70 起）和 + 加任务（width-36 起）两个按钮
        .size(width - 88.0f - (colDot.has_value() ? 18.0f : 0.0f), 20.0f)
        .text(col.title + " (" + std::to_string(taskCount) + ")")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    // 列头 ✎ 按钮直达改名弹窗；右键菜单「重命名列」入口保留
    components::button(ui, id + ".rename")
        .position(width - 70.0f, 14.0f)
        .size(28.0f, 28.0f)
        .icon(0xF303)  // fa-pencil-alt
        .iconSize(t.sizes.fontSizeSmall)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .iconColor(t.colors.textSecondary)
        .radius(14.0f)
        .onClick([cb = v.act.renameColumn, col] { cb(col); })
        .build();

    components::button(ui, id + ".add")
        .position(width - 36.0f, 14.0f)
        .size(28.0f, 28.0f)
        .text("+")
        .fontSize(t.sizes.fontSizeBody + 1.0f)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textSecondary)
        .radius(14.0f)
        .onClick([cb = v.act.addTaskToColumn, col] { cb(col.id); })
        .build();

    if (taskCount == 0) {
        // 空列无滚动容器，占位条直接画在列内容层首卡位（内容高 0，滚动恒 0）
        if (v.dragActive && v.dragHoverColumn == static_cast<int>(colIndex) &&
            v.dragHoverRow >= 0) {
            // 空列 hoverRow 恒 0：条落卡区首卡位（列头下方），宽与非空列卡槽同源
            drawDropBar(ui, kColumnHeaderHeight, contentCardWidth(width));
        }
        ui.text(id + ".empty")
            .position(0.0f, height / 2.0f - 12.0f)
            .size(width, 24.0f)
            .text(v.searchQuery.empty()
                      ? (isArchive ? "归档的任务在这里" : "暂无任务")
                      : "无命中任务")
            .fontSize(t.sizes.fontSizeSmall - 1.0f)
            .lineHeight(24.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        return;
    }

    // 列内滚动（scrollView）；滚动位置按列索引记忆（vector resize 保留旧值）
    const auto componentTheme = core::theme::componentTokens(t);
    // 列内容容器 = 卡片落位淡入载体：opacity 门控 dragActive（跟手中整列 0.6 调暗，松手 0.6→1.0 淡回）
    ui.stack(id + ".cards")
        .position(0.0f, kColumnHeaderHeight)
        .size(width, height - kColumnHeaderHeight)
        .opacity(v.dragActive ? 0.6f : 1.0f)
        .transition(core::Transition::make(0.12f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Opacity)
        .content([&] {
            components::scrollView(ui, id + ".scroll")
                .position(0.0f, 0.0f)
                .size(width, height - kColumnHeaderHeight)
                .gap(kCardGap)
                .step(60.0f)
                .theme(componentTheme)
                .contentKey(id + ".scroll." + std::to_string(taskCount))
                .offset(columnScrollOffset(v.columns, v.columnScrolls, colIndex))
                .onChange([cb = v.act.columnScrollChanged, colIndex](float value) {
                    cb(colIndex, value);
                })
                .content([&](eui::Ui& inner, float contentWidth,
                             float viewportHeight) {
                    (void)viewportHeight;
                    // 卡槽几何（卡与占位条唯一来源）：宽 = 内容宽 - 双侧 inset，x = inset
                    const float cardW = contentCardWidth(contentWidth);
                    // 占位条与卡片同帧布局（随列滚动跟手）
                    if (showDropBar) {
                        const float barY = std::max(
                            0.0f, static_cast<float>(v.dragHoverRow) *
                                      (kCardHeight + kCardGap) - kCardGap);
                        drawDropBar(inner, barY, cardW);
                    }
                    for (size_t i = 0; i < taskCount; ++i) {
                        const std::string cardId =
                            id + ".card." + std::to_string(i);
                        // wrap 占满内容宽（流式子元素，position 无效），卡在 wrap 内按 inset 落位
                        inner.stack(cardId + ".wrap")
                            .size(contentWidth, kCardHeight)
                            .content([&] {
                                drawCard(inner, cardId, kColumnContentInset,
                                         0.0f, cardW, t,
                                         *renderTasks[i],
                                         static_cast<int>(colIndex),
                                         static_cast<int>(v.columns.size()),
                                         static_cast<int>(i), v, plate);
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

// x 只作用于卡片容器——卡内元素坐标一律相对卡内容原点，不得再加 x（重复平移 = 双倍偏移）
void drawCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
              const core::theme::Tokens& t, const Task& task,
              int colIndex, int colCount, int cardIndex,
              const BoardView& v, BoardPlate& plate) {
    (void)colCount;
    const bool done = task.completedAt.has_value();
    const bool selected = (task.id == v.selectedTaskId);
    // 拖拽中间态：被拖卡片半透明（拖动只记录，松手才落库）
    const bool dragging = v.dragActive && v.dragTaskId == task.id;
    const float cardOpacity = dragging ? 0.45f : 1.0f;
    plate.cardW = width;  // compose 空间卡宽（拖拽命中 dpi 探针）

    // 卡片骨架用 card 组件；选中/未选中边框挂卡内交互层 .bg（挂 card 上会被不透明 .bg 盖住）
    components::card(ui, id + ".card")
        .position(x, y)
        .size(width, kCardHeight)
        .radius(t.sizes.radius)
        .theme(core::theme::componentTokens(t))
        .padding(0.0f)  // 卡内坐标以 content 原点为基准
        .border(0.0f, t.colors.borderDefault)  // 边框职责已移到内部交互层
        .content([&] {

    // 交互层（卡内首层）：优先级直接改卡底色，hover/active 同走该算式；边框与底色独立，选中态不被盖掉
    // 列色淡染：None 优先级按所在列色 0.12 混底，列区分列（优先级语义优先于列染）
    const auto colTint = colIndex >= 0 &&
        static_cast<size_t>(colIndex) < v.columns.size()
            ? columnDotColor(v.columns[static_cast<size_t>(colIndex)].color)
            : std::optional<eui::Color>{};
    const auto cardBase = t.colors.surface;
    const auto cardHover = t.colors.hover;
    const auto cardActive = t.colors.active;
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, kCardHeight)
        .states(priorityCardBg(task.priority, t, cardBase, colTint),
                priorityCardBg(task.priority, t, cardHover, colTint),
                priorityCardBg(task.priority, t, cardActive, colTint))
        .border(selected ? 2.0f : 1.0f,
                selected ? t.colors.primary : t.colors.borderDefault)
        // hover/按下颜色过渡（0.1s OutQuad；拖拽半透明 opacity 不在掩码里）
        .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Color | core::AnimProperty::Opacity)
        .radius(t.sizes.radius)
        .opacity(cardOpacity)
        .onClick([cb = v.act.cardClicked, task] { cb(task); })
        .onContextMenu([cb = v.act.cardMenu, task, colIndex, cardIndex](
                           const eui::PointerEvent&, const eui::Rect&) {
            cb(task, colIndex, cardIndex);
        })
        // drop 语义：拖动中无落库，松手按指针落点决定
        .onPress([cb = v.act.cardPressed, task, colIndex, cardIndex](
                     const eui::PointerEvent&, const eui::Rect& bounds) {
            cb(task, colIndex, cardIndex, bounds);
        })
        .onDrag([cb = v.act.cardDragged](const core::dsl::DragEvent& ev) {
            // totalX/totalY（按下点起累计位移）转交模块侧换算（ui::dragGhostAnchor）
            cb(ev.x, ev.y, ev.totalX, ev.totalY);
        })
        .onRelease([cb = v.act.cardReleased](
                       const eui::PointerEvent& ev, const eui::Rect&) {
            cb(ev.x, ev.y, ev.action == eui::PointerAction::Release);
        })
        .build();

    // 卡内文本居中；chip 行先算后画（自适应宽与绘制共用同一清单），chipX 保持卡内相对坐标
    struct PendingChip {
        std::string id;
        float w;
        std::string text;
        eui::Color fg;
    };
    std::vector<PendingChip> pendingChips;
    float chipX = 16.0f;

    // 截止 chip：列宽无下限（≥6-7 列时 colW 跌破 130），固定 66px 会越出卡右缘——自适应剩余空间，放不下就跳过
    if (task.dueAt.has_value()) {
        const std::string dueText = formatDue(task.dueAt.value());
        const float dueMaxW = std::max(0.0f, width - 12.0f - chipX);
        const float dueW = std::min(66.0f, dueMaxW);
        if (dueW >= 24.0f) {
            pendingChips.push_back({id + ".due", dueW, dueText,
                                    isOverdue(task) ? t.colors.error
                                                    : t.colors.textTertiary});
            chipX += dueW + 6.0f;  // 自适应宽 + 6 间距
        }
    }

    // tags chip（前 2 个 + 超出 +N；完整编辑在详情弹窗）
    // 归档列卡片显示归档时间戳（muted textTertiary，pill 形）
    const bool showArchivedChip = colIndex >= 0 &&
        static_cast<size_t>(colIndex) < v.columns.size() &&
        v.columns[static_cast<size_t>(colIndex)].kind == "archive" &&
        task.archivedAt.has_value();
    if (showArchivedChip) {
        const std::string archText = formatArchivedAt(task.archivedAt.value());
        const float archW = chipWidth(archText);
        const float avail = std::max(0.0f, width - 12.0f - chipX);
        if (archW <= avail) {
            pendingChips.push_back({id + ".archived", archW, archText,
                                    t.colors.textTertiary});
        }
    }
    if (!task.tags.empty()) {
        size_t shown = 0;
        for (size_t ti = 0; ti < task.tags.size() && shown < 2; ++ti) {
            const float tw = chipWidth(task.tags[ti]);
            if (chipX + tw > width - 12.0f) break;
            pendingChips.push_back({id + ".tag." + std::to_string(ti), tw,
                                    task.tags[ti], t.colors.info});
            chipX += tw + 6.0f;
            ++shown;
        }
        if (task.tags.size() > shown) {
            const std::string more = "+" + std::to_string(task.tags.size() - shown);
            const float moreW = chipWidth(more);
            // 放不下就不画
            if (chipX + moreW <= width - 12.0f) {
                pendingChips.push_back({id + ".tag.more", moreW, more,
                                        t.colors.textTertiary});
            }
        }
    }

    // 内容组垂直居中：组 = 标题行 +（有 chip 时）chip 行（常量全走 todo_ui_logic.h）
    const bool chipRow = !pendingChips.empty();
    const float groupTop = cardContentGroupTop(chipRow);
    const float chipY = y + groupTop + kCardTitleLineH + kCardTitleToChipGap;

    // 序号徽章：每列独立 1..n，纯渲染期计算
    const bool badgeMuted = colIndex >= 0 &&
        static_cast<size_t>(colIndex) < v.columns.size() &&
        (v.columns[static_cast<size_t>(colIndex)].kind == "done" ||
         v.columns[static_cast<size_t>(colIndex)].kind == "archive");
    const eui::Color badgeBg =
        badgeMuted ? t.colors.surface : t.colors.surfaceElevated;
    // 数字中性色：颜色全交给彩环——pink 浅色档 3.6:1 不够 12px 小字 WCAG AA，
    // 数字染彩会破可读性；环色独立承载徽章身份。done/archive 列照旧降档
    const eui::Color badgeText =
        badgeMuted ? t.colors.textTertiary : t.colors.textSecondary;
    const bool overflow = cardIndex >= 99;
    const std::string ordinalText =
        overflow ? "99+" : std::to_string(cardIndex + 1);
    const float badgeFontSize = overflow ? 11.0f : 12.0f;
    // 六色池按 task.id 稳定哈希伪随机——视觉随机 + 跨帧/跨重启不变，禁止逐帧真随机（会闪变）。
    // 不含 error/primary：error 语义因列独立、primary 主色保留给品牌焦点
    static const eui::Color core::theme::ColorTokens::*kBadgeRingColors[] = {
        &core::theme::ColorTokens::purple, &core::theme::ColorTokens::pink,
        &core::theme::ColorTokens::blue,  &core::theme::ColorTokens::info,
        &core::theme::ColorTokens::success, &core::theme::ColorTokens::warning,
    };
    const size_t colorIdx = static_cast<size_t>(task.id) * 2654435761u % 6;
    const eui::Color ringColor = t.colors.*kBadgeRingColors[colorIdx];
    // 正圆底：与 18px 标题行垂直居中（24-18=6 → 上偏 3）
    ui.rect(id + ".badge")
        .position(12.0f, y + groupTop - 3.0f)
        .size(kCardOrdinalBadgeSize, kCardOrdinalBadgeSize)
        .color(badgeBg)
        .radius(kCardOrdinalBadgeSize / 2.0f)
        .border(2.0f, ringColor)
        .opacity(cardOpacity)
        .build();
    // 数字（EUI-NEO verticalAlign::Center 补偿基线偏移，不靠目测）
    ui.text(id + ".badge.text")
        .position(12.0f, y + groupTop - 3.0f)
        .size(kCardOrdinalBadgeSize, kCardOrdinalBadgeSize)
        .text(ordinalText)
        .fontSize(badgeFontSize)
        .lineHeight(kCardOrdinalBadgeSize)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(badgeText)
        .fontWeight(600)
        .opacity(cardOpacity)
        .build();

    // 标题水平居中 = 框对称化（总留白 80、右缘让开完成角标区）+ horizontalAlign Center
    // 徽章占用 kCardOrdinalBadgeHoriz（24+8=32px），标题可用宽度相应缩减。
    // 省略号需 overflow==Ellipsis && maxWidth>0 && !wrap 三件套；右缘角标约束不变——
    // maxWidth 与 size 同宽，已计入完成角标区 [W-36, W-12] 让位
    ui.text(id + ".title")
        .position(kCardTitleBoxInset, y + groupTop)
        .size(width - 2.0f * kCardTitleBoxInset - kCardOrdinalBadgeHoriz,
              kCardTitleLineH)
        .text(task.title)
        .fontSize(t.sizes.fontSizeBody - 1.0f)
        .lineHeight(kCardTitleLineH)
        .fontWeight(500)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .maxWidth(width - 2.0f * kCardTitleBoxInset - kCardOrdinalBadgeHoriz)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .opacity(done ? 0.6f : cardOpacity)
        .build();

    // chip 行按清单绘制（x 从左往右，y 随内容组）
    float drawX = 16.0f;
    for (const auto& chip : pendingChips) {
        drawChip(ui, chip.id, drawX, chipY, chip.w, t, chip.text, chip.fg,
                 t.colors.surface, cardOpacity);
        drawX += chip.w + 6.0f;  // 自适应宽 + 6 间距
    }

    // 完成打勾（标题行尾角标：y 随标题行走内容组顶）
    if (done) {
        ui.text(id + ".done")
            .position(width - 36.0f, y + groupTop)
            .size(24.0f, kCardTitleLineH)
            .icon(0xF00C)  // fa-check（✓ 字符混排正文不可行，走 icon 通道）
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .lineHeight(18.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.success)
            .fontWeight(700)
            .opacity(cardOpacity)
            .build();
    }

        })
        .build();
}

// 副本透明度 0.85：插入位占位条与跟手副本几何重叠，全不透明会把条吃掉；0.85 让条透出且保住「实体被拎起」观感
constexpr float kDragGhostAlpha = 0.85f;
// 副本 z = 100：> 看板子树内最大 z、< 弹窗栈底（PopupStack::kBaseZ = 1000）——否则被列盖住或压住弹窗
constexpr int kDragGhostZIndex = 100;

// 页面级副本而非列内卡片 translate：列内容在 scrollView 根 stack 里无条件 .clip()，列内 translate
// 跨列拖时被切；页面顶层 root 一步解决被目标列盖住与被源列 0.6 调暗吃掉。只画底 + 标题，副本不可交互
void drawDragGhost(eui::Ui& ui, const core::theme::Tokens& t,
                   const BoardView& v, const BoardPlate& plate) {
    if (!v.dragGhostOn) return;
    const float w = plate.cardW;  // 本帧 drawCard 写入的 compose 卡宽
    if (w <= 0.0f) return;        // 无卡片几何（错误态/空板）→ 无可拖对象
    const Task* task = findTask(v.tasks, v.dragTaskId);
    if (task == nullptr) return;

    ui.stack("todo.dragghost")
        .position(v.dragGhostX, v.dragGhostY)
        .size(w, kCardHeight)
        .zIndex(kDragGhostZIndex)
        .opacity(kDragGhostAlpha)
        .content([&] {
            // 骨架复用 card 组件（与真卡同一份 radius/panelShadow/token）
            components::card(ui, "todo.dragghost.card")
                .position(0.0f, 0.0f)
                .size(w, kCardHeight)
                .radius(t.sizes.radius)
                .theme(core::theme::componentTokens(t))
                .padding(0.0f)
                .border(0.0f, t.colors.borderDefault)
                .content([&] {
            // 底 + 2px primary 描边（同真卡选中态）；底色同走 priorityCardBg，否则拖到一半优先级指示消失。
            // 列色淡染：同 drawCard——None 优先级按所在列色区分（拖印副本延续列身份）
                    const auto* ghostCol = findColumn(v.columns, task->columnId);
                    const auto ghostColTint = ghostCol
                        ? columnDotColor(ghostCol->color)
                        : std::optional<eui::Color>{};
                    ui.rect("todo.dragghost.bg")
                        .position(0.0f, 0.0f)
                        .size(w, kCardHeight)
                        .color(priorityCardBg(task->priority, t, t.colors.surface,
                                              ghostColTint))
                        .border(2.0f, t.colors.primary)
                        .radius(t.sizes.radius)
                        .build();
                    // 标题同真卡居中形态（副本不画 chips = 无 chip 卡：独占整卡居中）
                    ui.text("todo.dragghost.title")
                        .position(kCardTitleBoxInset, cardContentGroupTop(false))
                        .size(w - 2.0f * kCardTitleBoxInset, kCardTitleLineH)
                        .text(task->title)
                        .fontSize(t.sizes.fontSizeBody - 1.0f)
                        .lineHeight(kCardTitleLineH)
                        .fontWeight(500)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace module::todo::ui
