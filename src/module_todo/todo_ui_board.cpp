// src/module_todo/todo_ui_board.cpp
// Todo 看板区绘制（R5）：drawHeader / drawBoard / drawColumn / drawColumnContent /
// drawCard 逐字搬运自 todo_module.cpp（几何、文案、元素 id、控制流不变），
// 只把「模块成员访问」换成显式入参（BoardView 只读 + BoardActions 回写）。
#include "todo_ui_board.h"

#include "todo_ui_common.h"

#include <core/component_theme.h>
#include <core/theme.h>

#include <components/button.h>
#include <components/input.h>
#include <components/scrollview.h>

#include <string>

namespace module::todo::ui {

// 列 / 列内容 / 卡片的绘制只在文件内部互相调用（对外只有 drawHeader / drawBoard）
void drawColumn(eui::Ui& ui, const std::string& id, const Column& col,
                float width, float height, const core::theme::Tokens& t,
                size_t colIndex, const BoardView& v, BoardPlate& plate);
void drawColumnContent(eui::Ui& ui, const std::string& id, const Column& col,
                       float width, float height, const core::theme::Tokens& t,
                       size_t colIndex, const BoardView& v, BoardPlate& plate);
void drawCard(eui::Ui& ui, const std::string& id, float y, float width,
              const core::theme::Tokens& t, const Task& task,
              int colIndex, int colCount, int cardIndex,
              const BoardView& v, BoardPlate& plate);

// ---------- 顶部 Header ----------
// 右侧动作区 560px：搜索框(240) + 逾期入口(104) + 「+ 列」(72) + 「+ 新建任务」(120) + 间距
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
                    // subtitle 保持静态操作说明（动态反馈走 toast）；
                    // 包 clip 栈：41 字文案实宽 ≈545px，窄窗（≲1400）会压到
                    // 右侧搜索框/按钮（同 md.subtitle.clip 写法）
                    ui.stack("todo.subtitle.clip")
                        .size(contentW - 560.0f, 18.0f)
                        .clip()
                        .content([&] {
                            ui.text("todo.subtitle")
                                .size(contentW - 560.0f, 18.0f)
                                .text("点击卡片查看详情 · Ctrl+↑/↓ 移动选中任务 · 右键卡片/列头更多操作 · 拖动卡片跨列移动")
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
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
                    // 搜索框（真实调用 service_->search；仿 markdown 模块 md.search）
                    // 审计 [10]：Enter 改无副作用（不绑 onEnter，与 md.search 一致——
                    // 搜索已即时生效，回车不该清空）；Esc 清除走 onCancel（[21] 路径，
                    // 不再依赖 Escape→onEnter 旧映射）。
                    // 🔍 emoji 换 FA：包一层 stack 才能在流式 row 里叠 icon 浮层
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

                    // 逾期入口（badge = listOverdue().size()，0 时置灰不可点；
                    // ⚠ emoji 换 fa-triangle-exclamation，0 时随 disabled 自动置灰）
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

                    // + 列
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

                    // + 新建任务
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

// ---------- 看板列 ----------
void drawBoard(eui::Ui& ui, float contentW, float bodyH,
               const core::theme::Tokens& t, const BoardView& v, BoardPlate& plate) {
    // 错误态（服务未就绪 / 读取失败）：与空态区分，给文案 + 重试按钮
    // （仿 Home statsError_ 三态：空 / 错误+重试 / 数据）
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

    // 板面几何快照（W8 拖拽命中换算用；compose 每帧刷新）
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
    // 列内容整体包一层 stack：EUI-NEO 的 position() 只在 stack 内生效，
    // 直接作为 row("todo.body") 的子元素会被流式布局横向排开（列头/卡片错位）
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

    ui.rect(id + ".bg")
        .position(0.0f, 0.0f)
        .size(width, height)
        // 看板列容器是页面结构 → background（米色/暖墨），卡片白只给
        // 列内浮动卡片与按钮（主题层级修正，同侧边栏根因）
        .color(t.colors.background)
        .radius(t.sizes.radiusLarge)
        .border(dropHover ? 2.0f : 1.0f,
                dropHover ? t.colors.primary : t.colors.borderDefault)
        .build();

    auto colTasks = tasksInColumn(v.tasks, col.id);
    // 搜索激活时只显示命中卡（命中列照常渲染；命中集来自 service_->search）
    colTasks = filterBySearch(colTasks, v.searchMatchIds, !v.searchQuery.empty());
    // 归档列语义走 kind（migration v8 起回填 'archive'）：按标题判会在
    // 用户重命名后静默失效
    const bool isArchive = (col.kind == "archive");

    // 列头右键热区（重命名/左移/右移/删除列）。纯命中区：Rect 不设
    // color 时默认不透明白色（Element::color 默认 {1,1,1,1}），深色主题
    // 下会把列头整条刷白——必须显式给全透明
    ui.rect(id + ".head")
        .position(0.0f, 0.0f)
        .size(width, kColumnHeaderHeight)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onContextMenu([cb = v.act.columnMenu, col, colIndex](
                           const eui::PointerEvent&, const eui::Rect&) {
            cb(col, static_cast<int>(colIndex));
        })
        .build();

    // 列颜色点（Column::color 入库后首次渲染；10px 圆点，token→hex 见 ui::columnDotColor）
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
        .text(col.title + " (" + std::to_string(colTasks.size()) + ")")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    // 列重命名入口显性化（审计 [24]）：列头可见 ✎ 按钮直达改名弹窗，
    // 右键菜单「重命名列」入口保留
    components::button(ui, id + ".rename")
        .position(width - 70.0f, 14.0f)
        .size(28.0f, 28.0f)
        .icon(0xF303)  // fa-pencil-alt（对称铅笔；原 fa-edit 0xf044 字形右重偏右，换之）
        .iconSize(t.sizes.fontSizeSmall)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .iconColor(t.colors.textSecondary)
        .radius(14.0f)
        .onClick([cb = v.act.renameColumn, col] { cb(col); })
        .build();

    // 列头 + 按钮
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

    if (colTasks.empty()) {
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

    // 列内滚动（scrollView）：此前直接裁掉超出列高的卡片（第 12 张起消失）。
    // 滚动位置按列索引记忆（reloadAll 时 vector resize 保留旧值）。
    const auto componentTheme = core::theme::componentTokens(t);
    components::scrollView(ui, id + ".scroll")
        .position(0.0f, kColumnHeaderHeight)
        .size(width, height - kColumnHeaderHeight)
        .gap(kCardGap)
        .step(60.0f)
        .theme(componentTheme)
        .contentKey(id + ".scroll." + std::to_string(colTasks.size()))
        .offset(columnScrollOffset(v.columns, v.columnScrolls, colIndex))
        .onChange([cb = v.act.columnScrollChanged, colIndex](float value) {
            cb(colIndex, value);
        })
        .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
            (void)viewportHeight;
            for (size_t i = 0; i < colTasks.size(); ++i) {
                const std::string cardId = id + ".card." + std::to_string(i);
                inner.stack(cardId + ".wrap")
                    .size(contentWidth, kCardHeight)
                    .content([&] {
                        drawCard(inner, cardId, 0.0f, contentWidth - 16.0f, t,
                                 *colTasks[i], static_cast<int>(colIndex),
                                 static_cast<int>(v.columns.size()),
                                 static_cast<int>(i), v, plate);
                    })
                    .build();
            }
        })
        .build();
}

void drawCard(eui::Ui& ui, const std::string& id, float y, float width,
              const core::theme::Tokens& t, const Task& task,
              int colIndex, int colCount, int cardIndex,
              const BoardView& v, BoardPlate& plate) {
    (void)colCount;
    const bool done = task.completedAt.has_value();
    const bool selected = (task.id == v.selectedTaskId);
    // W8 拖拽中间态：被拖卡片半透明（拖动只是记录 + 视觉反馈，松手才落库）
    const bool dragging = v.dragActive && v.dragTaskId == task.id;
    const float cardOpacity = dragging ? 0.45f : 1.0f;
    plate.cardW = width;  // compose 空间卡宽（拖拽命中 dpi 探针）

    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, kCardHeight)
        // 卡片 = surface 层（浅色白 / 深色 #232120，spec §6.5.1 卡片色），
        // 浮在列容器（background）之上；曾用 surfaceElevated 抬高了一级
        .states(t.colors.surface, t.colors.hover, t.colors.active)
        .radius(t.sizes.radius)
        .opacity(cardOpacity)
        .border(selected ? 2.0f : 0.0f,
                selected ? t.colors.primary : t.colors.borderDefault)
        .onClick([cb = v.act.cardClicked, task] { cb(task); })
        .onContextMenu([cb = v.act.cardMenu, task, colIndex, cardIndex](
                           const eui::PointerEvent&, const eui::Rect&) {
            cb(task, colIndex, cardIndex);
        })
        // ---- 真 drop 语义（W8）：拖动中无落库，松手按指针落点决定 ----
        .onPress([cb = v.act.cardPressed, task, colIndex, cardIndex](
                     const eui::PointerEvent&, const eui::Rect& bounds) {
            cb(task, colIndex, cardIndex, bounds);
        })
        .onDrag([cb = v.act.cardDragged](const core::dsl::DragEvent& ev) {
            cb(ev.x, ev.y);
        })
        .onRelease([cb = v.act.cardReleased](
                       const eui::PointerEvent& ev, const eui::Rect&) {
            cb(ev.x, ev.y, ev.action == eui::PointerAction::Release);
        })
        .build();

    // 优先级左边框
    ui.rect(id + ".bar")
        .position(0.0f, y)
        .size(4.0f, kCardHeight)
        .color(priorityColor(task.priority, t))
        .radius(2.0f)
        .opacity(cardOpacity)
        .build();

    // 标题
    ui.text(id + ".title")
        .position(16.0f, y + 14.0f)
        .size(width - 44.0f, 18.0f)
        .text(task.title)
        .fontSize(t.sizes.fontSizeBody - 1.0f)
        .lineHeight(18.0f)
        .fontWeight(500)
        .color(t.colors.textPrimary)
        .opacity(done ? 0.6f : cardOpacity)
        .build();

    // 优先级 chip
    drawChip(ui, id + ".prio", 16.0f, y + 44.0f, 48.0f, t,
             priorityLabel(task.priority), priorityColor(task.priority, t),
             t.colors.surface, cardOpacity);

    // 截止 chip（⏰ emoji 已去掉：逾期时 chip 前景直接用 error 色示意）
    if (task.dueAt.has_value()) {
        const std::string dueText = formatDue(task.dueAt.value());
        drawChip(ui, id + ".due", 70.0f, y + 44.0f, 66.0f, t, dueText,
                 isOverdue(task) ? t.colors.error : t.colors.textTertiary,
                 t.colors.surface, cardOpacity);
    }

    // tags chip（前 2 个 + 超出 +N；完整编辑在详情弹窗）
    if (!task.tags.empty()) {
        float tagX = task.dueAt.has_value() ? 146.0f : 70.0f;
        size_t shown = 0;
        for (size_t ti = 0; ti < task.tags.size() && shown < 2; ++ti) {
            const float tw = chipWidth(task.tags[ti]);
            if (tagX + tw > width - 12.0f) break;
            drawChip(ui, id + ".tag." + std::to_string(ti), tagX, y + 44.0f,
                     tw, t, task.tags[ti], t.colors.info, t.colors.surface,
                     cardOpacity);
            tagX += tw + 6.0f;
            ++shown;
        }
        if (task.tags.size() > shown) {
            const std::string more = "+" + std::to_string(task.tags.size() - shown);
            const float moreW = chipWidth(more);
            // 放不下就不画（旧代码无条件画，chip 右缘可越出卡片 ~19px）
            if (tagX + moreW <= width - 12.0f) {
                drawChip(ui, id + ".tag.more", tagX, y + 44.0f, moreW, t,
                         more, t.colors.textTertiary, t.colors.surface, cardOpacity);
            }
        }
    }

    // 完成打勾
    if (done) {
        ui.text(id + ".done")
            .position(width - 36.0f, y + 14.0f)
            .size(24.0f, 18.0f)
            .icon(0xF00C)  // fa-check（✓ 字符混排正文不可行，走 icon 通道）
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .lineHeight(18.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.success)
            .fontWeight(700)
            .opacity(cardOpacity)
            .build();
    }
}

} // namespace module::todo::ui
