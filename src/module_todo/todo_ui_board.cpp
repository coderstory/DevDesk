// src/module_todo/todo_ui_board.cpp
// Todo 看板区绘制（R5）：drawHeader / drawBoard / drawColumn / drawColumnContent /
// drawCard 逐字搬运自 todo_module.cpp（几何、文案、元素 id、控制流不变），
// 只把「模块成员访问」换成显式入参（BoardView 只读 + BoardActions 回写）。
#include "todo_ui_board.h"

#include "todo_ui_common.h"

#include <core/component_theme.h>
#include <core/theme.h>

#include <components/button.h>
#include <components/card.h>
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
void drawCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
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

    // UI 整改 13：拖拽插入位占位条（虚框高亮用半透主色条代替——EUI-NEO
    // 无虚线边框原语）。barY 在滚动内容坐标系（与卡片同帧，滚动跟手）；
    // 行号跨列/同列都有（模块侧按命中置位），条落在「row-1 与 row 张卡之间」
    // 的缝里，row=0 顶格贴内容顶、row=列卡数 = 列尾追加。
    // 条的 x / 宽与卡片同源（kColumnContentInset + contentCardWidth，传参
    // barW 由调用点用同一算式得出）——2026-09-28 真机缺陷 2：条宽原写
    // 「内容宽 - 16」且 x=0，比它覆盖的卡槽短一截、右边留空。
    const bool showDropBar = v.dragActive &&
        v.dragHoverColumn == static_cast<int>(colIndex) && v.dragHoverRow >= 0;
    auto drawDropBar = [&](eui::Ui& inner, float barY, float barW) {
        eui::Color barColor = t.colors.primary;
        barColor.a = 0.55f;  // 高亮占位（半透主色，非硬编码白）
        // ignoreLayout 跳流式（假修根因修）：scrollView 内容列是 Column，
        // 非 ignoreLayout 子元素按流式排布、position 完全无效——条永远贴
        // 内容顶 + 整列卡片被下推（bar 高 + gap 共 16px）、松手弹回。置位
        // 后走 overlay 绝对定位（x/y 生效，layout.h layoutOverlayChildren），
        // 空列路径的 Stack 同样走 overlay，两路径行为一致
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
        .radius(t.sizes.radiusLarge)
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
        // 空列：无滚动容器，占位条直接画在列内容层首卡位（空列滚动必为 0
        // ——内容高 0，scrollView maxOffset 被钳制为 0）
        if (v.dragActive && v.dragHoverColumn == static_cast<int>(colIndex) &&
            v.dragHoverRow >= 0) {
            // 空列 hoverRow 恒 0（dragDropRowIndex 按 taskCount=0 钳制）：
            // 条落卡区首卡位 = 非空列 row 0 同槽位（列头下方），不贴列顶；
            // 宽度与非空列卡槽同源（contentCardWidth，见 kColumnContentInset）
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

    // 列内滚动（scrollView）：此前直接裁掉超出列高的卡片（第 12 张起消失）。
    // 滚动位置按列索引记忆（reloadAll 时 vector resize 保留旧值）。
    const auto componentTheme = core::theme::componentTokens(t);
    // M2-3（2026-09-28，UI 现代化）：列内容容器 = 卡片落位淡入载体。
    // 跨模块剩余审计修：opacity 直接门控 dragActive——跟手中整列 0.6 调暗
    //（反馈 + 为淡回建立 0.6 起点），松手目标回 1.0 → 平滑 0.6→1.0 淡回
    //（0.12s OutQuad）。修复前靠模块侧标志 + 复位计时器：松手瞬间先暗到
    // 0.6 再淡回（1.0→0.6→1.0 闪暗）且有 0.12s 延迟。原 scrollView 的绝对
    // 定位整体下移进本容器（位置/尺寸逐值等价，列头按钮等其余元素不受影响）
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
                .contentKey(id + ".scroll." + std::to_string(colTasks.size()))
                .offset(columnScrollOffset(v.columns, v.columnScrolls, colIndex))
                .onChange([cb = v.act.columnScrollChanged, colIndex](float value) {
                    cb(colIndex, value);
                })
                .content([&](eui::Ui& inner, float contentWidth,
                             float viewportHeight) {
                    (void)viewportHeight;
                    // 卡槽几何（卡与占位条的唯一来源）：宽 = 内容宽 - 双侧
                    // inset，x = inset（见 kColumnContentInset）——2026-09-28
                    // 真机缺陷 1/2：旧算式只减一次 16 且 x=0，卡贴列左缘、
                    // 条右边空余；现在卡/条同宽同位
                    const float cardW = contentCardWidth(contentWidth);
                    // 占位条与卡片同帧布局（滚动内容坐标系，随列滚动跟手）
                    if (showDropBar) {
                        const float barY = std::max(
                            0.0f, static_cast<float>(v.dragHoverRow) *
                                      (kCardHeight + kCardGap) - kCardGap);
                        drawDropBar(inner, barY, cardW);
                    }
                    for (size_t i = 0; i < colTasks.size(); ++i) {
                        const std::string cardId =
                            id + ".card." + std::to_string(i);
                        // wrap 仍占满内容宽（流式子元素，position 无效），
                        // 卡在 wrap 内按 inset 落位；子元素几何不变
                        inner.stack(cardId + ".wrap")
                            .size(contentWidth, kCardHeight)
                            .content([&] {
                                drawCard(inner, cardId, kColumnContentInset,
                                         0.0f, cardW, t,
                                         *colTasks[i],
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

// x = 卡左缘在「列内容坐标系」里的位置（调用点传 kColumnContentInset：列内容
// 左内边距，见 todo_ui_logic.h）。**x 只作用于卡片容器**（card 组件的
// .position）——卡内元素（.bg / .title / chips / .done）坐标一律相对**卡内容
// 原点**（= 卡原点，padding(0)），不得再加 x：卡内容 origin 本来就在卡内，
// 重复平移 = 双倍偏移（第一版修法踩过：卡内元素全多加了一次 x，断言
// card.bg.x - col.x 得到 32 而非 16，被 todo_ui_card_inset_and_dropbar_width
// 抓住）。y 现恒 0（纵向由 scrollView 流式排布），保留形参让卡内装饰层与旧
// 实现同构。
void drawCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
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

    // M3-3（2026-09-28，UI 现代化）：卡片容器换 card 组件——骨架由 card
    // 画（surface 底 + radius + panelShadow 阴影随 CardStyle）；UI 整改 16：
    // 选中/未选中边框从 card 的 .border 移到卡内交互层（原挂在 card 上会被
    // 卡内不透明 .bg 整块盖住、选中态边框不可见——用户把左侧优先级竖条
    // 误认成选中指示）。原 .bg 作为卡内交互层原样保留（states 三态 + 0.1s
    // OutQuad 过渡 + onPress/onDrag/onRelease/onClick/onContextMenu 全部
    // 事件挂点随行；边框条件换算到 .bg 的 .border：未选中 1px 默认边框 /
    // 选中 2px 主色加粗）。卡内容坐标以 .padding(0) 的 content 原点为
    // 基准（与旧 wrap content 原点一致，装饰层位置不变）
    components::card(ui, id + ".card")
        .position(x, y)
        .size(width, kCardHeight)
        .radius(t.sizes.radius)
        .theme(core::theme::componentTokens(t))
        .padding(0.0f)  // 卡内坐标以 content 原点为基准（同旧 wrap content）
        .border(0.0f, t.colors.borderDefault)  // 边框职责已移到内部交互层
        .content([&] {

    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, kCardHeight)
        // 交互层（卡内首层）：基色 = surface（与 card 骨架同 token，视觉
        // 无差），hover/active 由 states 过渡合成；边框条件在 .bg 上：
        // 未选中 1px borderDefault / 选中 2px primary 加粗（UI 整改 16）
        .states(t.colors.surface, t.colors.hover, t.colors.active)
        .border(selected ? 2.0f : 1.0f,
                selected ? t.colors.primary : t.colors.borderDefault)
        // M2（2026-09-28，UI 现代化）：hover/按下颜色过渡（列表项基调
        // 0.1s OutQuad；拖拽半透明 opacity 不含在掩码里，跟手不打架）
        .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Color | core::AnimProperty::Opacity)
        .radius(t.sizes.radius)
        .opacity(cardOpacity)
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
            // totalX/totalY（按下点起累计位移）随行转交：跟手副本锚点要吃它，
            // 换算在模块侧（ui::dragGhostAnchor），本单元不做像素→compose
            cb(ev.x, ev.y, ev.totalX, ev.totalY);
        })
        .onRelease([cb = v.act.cardReleased](
                       const eui::PointerEvent& ev, const eui::Rect&) {
            cb(ev.x, ev.y, ev.action == eui::PointerAction::Release);
        })
        .build();

    // 标题（16 = 卡内左内边距，相对卡内容原点）
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

    // UI 整改 12：优先级从「●高」文字 chip 换成底色 pill（高=警示色底/
    // 中=中性底/低=弱底，走 theme token tint），去「高/中/低」字样；
    // 原先的左侧优先级竖条（.bar）随 16 一并删除。chip 从左往右顺序排
    // 放（prio → due → tags），None 不画 pill、后续 chip 顺移，不留空位。
    const auto pillBg = priorityPillBg(task.priority, t);
    // chipX 保持**卡内相对**坐标（16 = 卡内左内边距）；下面的宽度自适应算式
    // 比较的是「相对卡右缘的剩余空间」，同坐标系
    float chipX = 16.0f;
    // 跨模块剩余审计修（2026-09-28）：列宽无下限（≥6-7 列时 colW 跌破
    // 130），固定 36/66px 的优先级/截止 chip 会越出卡右缘——chip 宽自适应
    // 剩余空间，放不下最小宽度就跳过（完整信息在详情弹窗）；标签 chip 的
    // 原有自适应保留（chipX + tw > width-12 即 break）
    if (pillBg.has_value() && chipX + 36.0f <= width - 12.0f) {
        drawChip(ui, id + ".prio", chipX, y + 44.0f, 36.0f, t, "",
                 *pillBg, *pillBg, cardOpacity);
        chipX += 42.0f;  // 36 + 6 间距
    }

    // 截止 chip（⏰ emoji 已去掉：逾期时 chip 前景直接用 error 色示意）
    if (task.dueAt.has_value()) {
        const std::string dueText = formatDue(task.dueAt.value());
        const float dueMaxW = std::max(0.0f, width - 12.0f - chipX);
        const float dueW = std::min(66.0f, dueMaxW);
        if (dueW >= 24.0f) {
            drawChip(ui, id + ".due", chipX, y + 44.0f, dueW, t, dueText,
                     isOverdue(task) ? t.colors.error : t.colors.textTertiary,
                     t.colors.surface, cardOpacity);
            chipX += dueW + 6.0f;  // 自适应宽 + 6 间距
        }
    }

    // tags chip（前 2 个 + 超出 +N；完整编辑在详情弹窗）
    if (!task.tags.empty()) {
        size_t shown = 0;
        for (size_t ti = 0; ti < task.tags.size() && shown < 2; ++ti) {
            const float tw = chipWidth(task.tags[ti]);
            if (chipX + tw > width - 12.0f) break;
            drawChip(ui, id + ".tag." + std::to_string(ti), chipX, y + 44.0f,
                     tw, t, task.tags[ti], t.colors.info, t.colors.surface,
                     cardOpacity);
            chipX += tw + 6.0f;
            ++shown;
        }
        if (task.tags.size() > shown) {
            const std::string more = "+" + std::to_string(task.tags.size() - shown);
            const float moreW = chipWidth(more);
            // 放不下就不画（旧代码无条件画，chip 右缘可越出卡片 ~19px）
            if (chipX + moreW <= width - 12.0f) {
                drawChip(ui, id + ".tag.more", chipX, y + 44.0f, moreW, t,
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

        })
        .build();
}

// ---------- 拖拽跟手副本 ----------

// 副本整体透明度：**不是为了好看，是为了坑二**——插入位占位条（目标列内的
// .dropbar，0.55 alpha 主色条）与跟手副本在几何上必然重叠（条的位置由指针
// 行号决定，副本跟着指针）。副本是页面顶层 root（见下），列内元素越不过它，
// 全不透明会把条整块吃掉。0.85 让条从卡片底下透出可辨的暖色带，同时保住
// 卡片"实体被拎起来"的观感。不另画第二根条（会与列内条重复指示）。
constexpr float kDragGhostAlpha = 0.85f;
// 副本 z：必须 > 看板子树内最大 z（列滚动条的 zIndex+1，见 components/
// scrollview.h:121），< 弹窗栈底（PopupStack::kBaseZ = 1000）——否则要么被
// 列盖住（跨列拖的主用例直接失效），要么压住弹窗。取中间值 100。
constexpr int kDragGhostZIndex = 100;

// 为什么是"页面级副本"而不是给列内卡片加 translate（两种方案都验证过）：
//   · 列内容在 components::scrollView 的根 stack 里，该 stack 无条件 .clip()
//     （components/scrollview.h:88）；clip 在渲染侧对整个子树做 scissor 求交
//     （core/runtime/runtime_render.h:223-230）→ 列内任何 translate 都被裁到
//     列内容框，跨列拖时卡片在列边界被切掉，而跨列正是主用例；
//   · 副本画在页面顶层 root（Doc 序在 todo.root 之后 + 显式 zIndex），一步
//     解决"被目标列卡片盖住"（坑一）与"被源列 0.6 调暗吃掉"（坑四）——副本
//     不在源列 .cards 容器内，天然全亮。
// 只画底 + 标题（不复制 drawCard 的 chips）：副本要的是"一眼认出是哪张卡"，
// 复制 chips 会把 drawCard 的窄列自适应布局逻辑养成双份；且副本不可交互
// （不挂任何事件），避免拖拽中多一个命中目标。
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
            // 骨架复用 card 组件（与真卡同一份 radius/panelShadow/token），
            // 视觉上是"同一张卡被拎起来"而不是另一种控件
            components::card(ui, "todo.dragghost.card")
                .position(0.0f, 0.0f)
                .size(w, kCardHeight)
                .radius(t.sizes.radius)
                .theme(core::theme::componentTokens(t))
                .padding(0.0f)
                .border(0.0f, t.colors.borderDefault)
                .content([&] {
                    // 底 + 主色描边：与真卡的"选中态"同款视觉（2px primary），
                    // 一眼区分"这是被拎着的那张"
                    ui.rect("todo.dragghost.bg")
                        .position(0.0f, 0.0f)
                        .size(w, kCardHeight)
                        .color(t.colors.surface)
                        .border(2.0f, t.colors.primary)
                        .radius(t.sizes.radius)
                        .build();
                    ui.text("todo.dragghost.title")
                        .position(16.0f, 14.0f)
                        .size(w - 44.0f, 18.0f)
                        .text(task->title)
                        .fontSize(t.sizes.fontSizeBody - 1.0f)
                        .lineHeight(18.0f)
                        .fontWeight(500)
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace module::todo::ui
