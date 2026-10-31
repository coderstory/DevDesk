// Todo 看板区绘制：drawHeader/drawBoard/drawColumn/drawColumnContent/drawCard（数据显式入参 BoardView + BoardActions 回写）
#include "todo_ui_board.h"

#include "todo_ui_common.h"

#include <core/component_theme.h>
#include <core/theme.h>

#include <components/button.h>
#include <components/card.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/tooltip.h>
#include <ui_kit/card.h>
#include <ui_kit/empty_state.h>
#include <ui_kit/selection_bar.h>  // kSelectionBarAccentWidth（§4.3 选中态 2px 竖线宽度）

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
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

// ---- UAT6 misc2：卡内水平内边距收紧（真机反馈：标题/标签可视字符太少）----
// 只动水平，垂直排版零改动（groupTop/行高/行距不动）。旧值：标题双侧 40
// （todo_ui_logic.h kCardTitleBoxInset）、chip 行左 16 / 右 12、完成角标 24px。
// 让位公式不变：标题右内边距 = chip 右边距 + 角标宽 + 4px 间隙
// （旧 40 = 12 + 24 + 4；新 28 = 8 + 16 + 4），标题框右缘永不压完成角标。
// 常量收敛在本文件（卡片专属几何），不动 todo_ui_logic.h——该头与详情弹窗批共用。
constexpr float kCardPadX = 12.0f;         // 标题框左右内边距（S3.3 收紧至 12）
constexpr float kCardChipPadX = 12.0f;    // chip 行左内边距
constexpr float kCardChipPadRight = 12.0f; // chip 行/完成角标右内边距
constexpr float kCardDoneBadgeW = 16.0f;  // 完成角标宽（旧 24，右缘对齐 chip 右边距）

// ---- H16（2026-10-23 用户拍板覆盖 S6#5「禁止整卡涂色」）：卡面优先级浅染 ----
// 真机 0.5.0 实测卡面全白、优先级色丢失。surface 白底向优先级色混 kTodoTintAlpha
// 恢复轻微彩色半透质感（量级 0.10-0.15，可调）。色源按用户指定板：
// 高=error #EF4444 / 中=warning #F59E0B / 低=success #10B981；None 档无 tint 保持纯白。
// 与左侧 3px 色条（priorityColor 表）并存：色条 = 硬标识，浅染 = 卡面氛围；
// 两色源独立（色条 Low 走 primary 薄荷绿 #36D399，浅染 Low 走 success 祖母绿 #10B981）。
constexpr float kTodoTintAlpha = 0.12f;

// 归档折叠竖条的 hover / focus 态：回调写、compose 读（同 app_sidebar 关闭钮先例）。
// 只是视觉态不是模块状态；同一时刻至多一根归档列，单 bool 足够。
bool gArchiveCompactHovered = false;
bool gArchiveCompactFocused = false;

inline eui::Color cardFaceColor(Priority p, const core::theme::Tokens& t) {
    const eui::Color* src = nullptr;
    switch (p) {
        case Priority::Low: src = &t.colors.success; break;
        case Priority::Medium: src = &t.colors.warning; break;
        case Priority::High: src = &t.colors.error; break;
        case Priority::None: break;
    }
    // H15：基底 surface 叠加 kCardGlassAlpha 轻微半透，H16 浅染在其上混色
    return src == nullptr ? ui_kit::cardSurface(t)
                          : eui::mixColor(ui_kit::cardSurface(t), *src, kTodoTintAlpha);
}

// ---- H20：动作区按真实控件重算（不再留 560 死值）----
// 搜索框 240×40 → 逾期 chip 88×28 → 「新建任务」120×32，控件间 2×spacingSmall。
// 「+ 列」入口按固定四列设计删除（列模型不可增删）。
constexpr float kHeaderSearchW = 240.0f;
constexpr float kHeaderOverdueW = 88.0f;
constexpr float kHeaderNewTaskW = 120.0f;

// 归档展开态渲染上限（最近 20 张），超出部分只在列底提示一行。
constexpr size_t kArchiveRenderLimit = 20;
constexpr float kArchiveLimitH = 44.0f;
// 列容器圆角（原型 --radius-col: 12px）
constexpr float kColumnRadius = 12.0f;

void drawHeader(eui::Ui& ui, float contentW, float headerH, float bodyH,
                const core::theme::Tokens& t, const BoardView& v) {
    (void)bodyH;
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    const float actionsW = kHeaderSearchW + kHeaderOverdueW + kHeaderNewTaskW +
                           t.sizes.spacingSmall * 2.0f;
    // 标题区可用宽 = 内容宽 − 动作区实际宽；极窄窗口钳到 0 防负宽
    const float titleW = std::max(0.0f, contentW - actionsW);
    ui.row("todo.header")
        .size(contentW, headerH)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.column("todo.title")
                .size(titleW, headerH)
                .gap(2.0f)
                .content([&] {
                    // UAT5 全局顶部收紧：headerH 64→40，标题行 24
                    ui.text("todo.h1")
                        .size(titleW, 24.0f)
                        .text("Todo 看板")
                        .fontSize(t.sizes.fontSizeTitle)
                        .lineHeight(24.0f)
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();

            ui.row("todo.actions")
                .size(actionsW, 40.0f)
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
                                .fontSize(core::theme::scaled(12.0f))
                                .lineHeight(core::theme::scaled(16.0f))
                                .color(t.colors.textTertiary)
                                .build();
                        })
                        .build();

                    // 逾期入口：chip 小标签（S3.3 #5）；0 时置灰不可点；有逾期时微信绿高亮可点
                    const bool hasOverdue = v.overdueCount > 0;
                    ui.stack("todo.btn.overdue.wrap")
                        .size(88.0f, 28.0f)
                        .content([&] {
                            ui.rect("todo.btn.overdue.bg")
                                .position(0.0f, 0.0f)
                                .size(88.0f, 28.0f)
                                .color(hasOverdue ? t.colors.primarySoftBg : t.colors.hover)
                                .radius(14.0f)
                                .build();
                            ui.text("todo.btn.overdue.text")
                                .position(0.0f, 0.0f)
                                .size(88.0f, 28.0f)
                                .text("逾期 " + std::to_string(v.overdueCount))
                                .fontSize(core::theme::scaled(12.0f))
                                .lineHeight(core::theme::scaled(28.0f))
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(hasOverdue ? t.colors.primary : t.colors.textTertiary)
                                .fontWeight(600)
                                .build();
                            if (hasOverdue) {
                                ui.rect("todo.btn.overdue.hit")
                                    .position(0.0f, 0.0f)
                                    .size(88.0f, 28.0f)
                                    .color(eui::Color{0,0,0,0})
                                    .onClick([cb = v.act.overdueClicked] { cb(); })
                                    .build();
                            }
                        })
                        .build();

                    // H20：唯一创建入口（列头 + / 空列添加入口已删除），走富创建弹窗
                    components::button(ui, "todo.btn.add")
                        .size(kHeaderNewTaskW, 32.0f)
                        .text("新建任务")
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.primary, t.colors.primaryHover,
                                t.colors.primaryPressed)
                        .textColor(t.colors.onPrimary)
                        .radius(6.0f)
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
                    .color(t.colors.errorText)
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
        plate.columnX.clear();
        plate.columnWidth.clear();
        ui.stack("todo.empty")
            .size(contentW, bodyH)
            .content([&] {
                ui.text("todo.empty.text")
                    .position(0.0f, bodyH / 2.0f - 12.0f)
                    .size(contentW, 24.0f)
                    .text("还没有任何列")
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
    // H20 列宽派生：展开 W=(contentW−3×gap)/4；归档 = lerp(W×25%, W, archiveT)；
    // 其余三列 = (contentW−3×gap−archiveW)/3。位置由 row 流式重排，随插值宽度逐帧生效。
    int archiveIndex = -1;
    for (size_t ci = 0; ci < v.columns.size(); ++ci) {
        if (v.columns[ci].kind == "archive") {
            archiveIndex = static_cast<int>(ci);
            break;
        }
    }
    const BoardColumnWidths widths = boardColumnWidths(
        contentW, colGap, v.columns.size(), archiveIndex, v.archiveT);

    // 板面几何快照（拖拽命中换算用；compose 每帧刷新）。
    // 逐列实际几何（含归档折叠动画宽度）写入 columnX/columnWidth
    plate.height = bodyH;
    plate.columnX.resize(v.columns.size());
    plate.columnWidth.resize(v.columns.size());
    float cursorX = 0.0f;
    for (size_t ci = 0; ci < v.columns.size(); ++ci) {
        const float colW = (static_cast<int>(ci) == archiveIndex)
                               ? widths.archive
                               : widths.normal;
        plate.columnX[ci] = cursorX;
        plate.columnWidth[ci] = colW;
        cursorX += colW + colGap;
    }

    ui.row("todo.body")
        .size(contentW, bodyH)
        .gap(colGap)
        .content([&] {
            for (size_t ci = 0; ci < v.columns.size(); ++ci) {
                drawColumn(ui, "todo.col." + std::to_string(ci),
                           v.columns[ci], plate.columnWidth[ci], bodyH, t, ci, v, plate);
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
        barColor.a = 0.55f;  // 高亮占位（半透微信绿，§4 拖拽绿色占位线）
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
        // 列容器 = 页面结构 → 微信 pageBg 浅灰底（§4.14 白卡浮起于浅灰底）；卡片白只给列内浮动卡片与按钮。
        // H20：归档折叠态就是同一根普通列收窄，底色/边/圆角与其他列一致，不做独立白条/阴影。
        // 拖拽悬停目标列 = 浅绿底 + 主色边（原型 .column.is-drop-target）。
        .color(dropHover ? t.colors.primarySoftBg : t.colors.background)
        .radius(kColumnRadius)
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

    // ---- 归档折叠动画：archiveT 是唯一进度输入（模块侧驱动）----
    // 窄态 <0.5；展开列头 0.5→0.65 淡入。
    const float archiveT = std::clamp(v.archiveT, 0.0f, 1.0f);
    const float compactAlpha =
        isArchive ? 1.0f - smoothStep(0.35f, 0.5f, archiveT) : 0.0f;
    const float headAlpha =
        isArchive ? smoothStep(0.5f, 0.65f, archiveT) : 1.0f;
    // archiveT 已含 easeOutCubic；反解出线性时间 p（展开 240ms / 收起 200ms），
    // 内容淡入按 p∈[0.4,0.9]、收起按 p<0.3 淡出，与原型时长对齐。
    const float easedT = std::clamp(
        v.archiveExpanded ? archiveT : 1.0f - archiveT, 0.0f, 1.0f);
    const float linearP = 1.0f - std::cbrt(1.0f - easedT);
    const float contentAlpha =
        isArchive ? (v.archiveExpanded ? smoothStep(0.4f, 0.9f, linearP)
                                       : 1.0f - smoothStep(0.0f, 0.3f, linearP))
                  : 1.0f;
    // 结构口径（目标态为真时即便动画帧未推进也保留展开结构，供无头用例/首帧落位）：
    const bool archiveHeaderOn =
        !isArchive || v.archiveExpanded || archiveT >= 0.5f;
    const bool archiveCardsOn =
        !isArchive || v.archiveExpanded || archiveT >= 0.4f;
    // 窄态元素在交叉淡化后移除时框架不会补发 hover(false)：此处主动复位，
    // 防下次折叠回来文字停在主色 hover 态。
    if (isArchive && compactAlpha <= 0.01f) {
        gArchiveCompactHovered = false;
        gArchiveCompactFocused = false;
    }

    // 列头（归档折叠窄态不画普通列头，整列让位给竖排条）
    if (archiveHeaderOn) {
        // 列头热区保留；透明底防深色主题刷白（右键菜单入口已随固定四列删除）。
        // 归档列整条列头即折叠开关（2026-10-31 反馈：原先只有右侧 32px 箭头可点）；
        // 箭头保留自身热区与 hover，命中优先级更高（顶层优先），不会双触发。
        auto head = ui.rect(id + ".head")
            .position(0.0f, 0.0f)
            .size(width, kColumnHeaderHeight)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .radius(t.sizes.radiusSmall);
        if (isArchive) {
            head.hoverColor(t.colors.primarySoftBg)
                .onClick([cb = v.act.archiveToggle] { cb(); });
        }
        head.build();

        auto drawHeadContent = [&](eui::Ui& target) {
            // 列颜色点（10px 圆点，token→hex 见 ui::columnDotColor）
            const auto colDot = columnDotColor(col.color);
            if (colDot.has_value()) {
                target.rect(id + ".dot")
                    .position(16.0f, 22.0f)
                    .size(8.0f, 8.0f)
                    .color(*colDot)
                    .radius(4.0f)
                    .build();
            }
            const float titleX = colDot.has_value() ? 34.0f : 16.0f;
            // 归档列右侧留 32px 收起钮 + 12px 内边距；普通列只留 16px
            const float titleW = width - titleX - (isArchive ? 44.0f : 16.0f);
            target.text(id + ".title")
                .position(titleX, 16.0f)
                .size(titleW, 20.0f)
                .text(col.title + " (" + std::to_string(taskCount) + ")")
                .fontSize(t.sizes.fontSizeWeChat)
                .lineHeight(20.0f)
                .fontWeight(600)
                // 省略号三件套：列名 + 计数超宽时截断，不压右侧按钮
                .maxWidth(titleW)
                .overflow(core::TextOverflow::Ellipsis)
                .color(t.colors.textPrimary)
                .build();

            if (isArchive) {
                // 同一个 chevron 旋转 180°：折叠竖条的 chevron-left → 展开态指向右
                target.rect(id + ".archivetoggle.bg")
                    .position(width - 44.0f, 14.0f)
                    .size(32.0f, 32.0f)
                    .color(eui::Color{0, 0, 0, 0})
                    .hoverColor(t.colors.primarySoftBg)
                    .radius(6.0f)
                    .onClick([cb = v.act.archiveToggle] { cb(); })
                    .build();
                target.text(id + ".archivetoggle.icon")
                    .position(width - 44.0f, 14.0f)
                    .size(32.0f, 32.0f)
                    .icon(0xF053)  // fa-chevron-left
                    .rotate(3.14159265f)
                    .fontSize(core::theme::scaled(14.0f))
                    .lineHeight(core::theme::scaled(14.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primary)
                    .build();
            }
        };
        if (isArchive) {
            // 归档列头与窄态按 alpha 交叉淡化：整块随 headAlpha 渐显
            ui.stack(id + ".headwrap")
                .position(0.0f, 0.0f)
                .size(width, kColumnHeaderHeight)
                .opacity(headAlpha)
                .content([&] { drawHeadContent(ui); })
                .build();
        } else {
            drawHeadContent(ui);
        }
    }

    // 归档折叠窄态：竖排「已归档 · N」+ 计数 pill + chevron-left；整列可点、hover 浅绿底 + 主色字。
    // 容器仍是同一根普通列（列 bg 已画），这里只是列内内容层，不做独立白条/阴影。
    if (isArchive && compactAlpha > 0.01f) {
        const bool hover = gArchiveCompactHovered;
        const eui::Color labelColor =
            hover ? t.colors.primary : t.colors.textSecondary;
        const eui::Color countColor =
            hover ? t.colors.primary : t.colors.textPrimary;

        // 整列命中层：hover 浅绿底，键盘 Enter/Space 切换，focus 画 2px 主色框
        ui.rect(id + ".expand.bg")
            .position(0.0f, 0.0f)
            .size(width, height)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .hoverColor(t.colors.primarySoftBg)
            .radius(kColumnRadius)
            .border(gArchiveCompactFocused ? 2.0f : 0.0f, t.colors.primary)
            .onHover([](bool hovered) { gArchiveCompactHovered = hovered; })
            .onFocusChanged([](bool focused) { gArchiveCompactFocused = focused; })
            .onKeyEvent([cb = v.act.archiveToggle](const eui::KeyEvent& ev) {
                if (ev.action != eui::KeyAction::Press) return false;
                if (ev.key != eui::InputKey::Enter && ev.key != eui::InputKey::Space) {
                    return false;
                }
                cb();
                return true;
            })
            .onClick([cb = v.act.archiveToggle] { cb(); })
            .build();

        // EUI 无 writing-mode:vertical-rl —— 逐字居中堆叠，复现原型竖排标签
        static constexpr const char* kCompactGlyphs[] = {"已", "归", "档", "·"};
        float glyphY = 16.0f;
        for (size_t gi = 0; gi < std::size(kCompactGlyphs); ++gi) {
            ui.text(id + ".compact.label." + std::to_string(gi))
                .position(0.0f, glyphY)
                .size(width, 18.0f)
                .text(kCompactGlyphs[gi])
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .fontWeight(600)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(labelColor)
                .opacity(compactAlpha)
                .build();
            glyphY += 18.0f;
        }

        // 计数 pill（#E6FBF2 底圆角）：宽随位数走，窄列钳到列宽 - 6
        const std::string countText = std::to_string(taskCount);
        const float pillW = std::min(
            std::max(18.0f, 12.0f + static_cast<float>(countText.size()) * 7.0f),
            width - 6.0f);
        const float pillX = (width - pillW) / 2.0f;
        const float pillY = glyphY + 6.0f;
        ui.rect(id + ".compact.pill")
            .position(pillX, pillY)
            .size(pillW, 18.0f)
            .color(t.colors.primarySoftBg)
            .radius(9.0f)
            .opacity(compactAlpha)
            .build();
        ui.text(id + ".compact.count")
            .position(pillX, pillY)
            .size(pillW, 18.0f)
            .text(countText)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(18.0f))
            .fontWeight(600)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(countColor)
            .opacity(compactAlpha)
            .build();

        // 底部 chevron-left（14px 主色）；展开列头的收起按钮是同一图标旋转 180°
        ui.text(id + ".compact.chev")
            .position(0.0f, std::max(0.0f, height - 28.0f))
            .size(width, 14.0f)
            .icon(0xF053)  // fa-chevron-left
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(core::theme::scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.primary)
            .opacity(compactAlpha)
            .build();

        components::tooltip(ui, id + ".compact.tooltip")
            .theme(core::theme::componentTokensWeChat(t))
            .source(id + ".expand.bg")
            .anchor(width * 0.5f, 36.0f)
            .bounds(width, height)
            .value("展开归档")
            .build();

        if (!archiveCardsOn) return;
    }


    if (taskCount == 0) {
        // 空列无滚动容器，占位条直接画在列内容层首卡位（内容高 0，滚动恒 0）
        if (v.dragActive && v.dragHoverColumn == static_cast<int>(colIndex) &&
            v.dragHoverRow >= 0) {
            // 空列 hoverRow 恒 0：条落卡区首卡位（列头下方），宽与非空列卡槽同源
            drawDropBar(ui, kColumnHeaderHeight, contentCardWidth(width));
        }
        // S2.8：空态走 ui_kit::emptyState 极简文字档（14px/textSecondary 居中）
        ui_kit::emptyState(ui, id + ".empty",
                           v.searchQuery.empty()
                               ? (isArchive ? "暂无归档" : "暂无任务")
                               : "无命中任务",
                           std::nullopt, 0.0f, height / 2.0f - 24.0f, width, 48.0f,
                           t);
        // H20：空列「+ 添加任务」入口删除——创建入口唯一（页头「新建任务」）
        return;
    }

    // 列内滚动（scrollView）；滚动位置按列索引记忆（vector resize 保留旧值）
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    // 归档超出 20 张时列底留一行提示（计入容器高度，不遮卡片）
    const bool showArchiveLimit =
        isArchive && archiveCardsOn && taskCount > kArchiveRenderLimit;
    const float cardsH =
        height - kColumnHeaderHeight - (showArchiveLimit ? kArchiveLimitH : 0.0f);
    // 列内容容器 = 卡片落位淡入载体：opacity 门控 dragActive（跟手中整列 0.6 调暗，松手 0.6→1.0 淡回），
    // 归档列再乘 contentAlpha（线性时间口径的淡入淡出）
    ui.stack(id + ".cards")
        .position(0.0f, kColumnHeaderHeight)
        .size(width, cardsH)
        .opacity((v.dragActive ? 0.6f : 1.0f) * contentAlpha)
        .transition(core::Transition::make(0.12f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Opacity)
        .content([&] {
            components::scrollView(ui, id + ".scroll")
                .position(0.0f, 0.0f)
                .size(width, cardsH)
                .gap(kCardGap)
                .step(60.0f)
                .theme(componentTheme)
                .scrollbarOverlay()  // UAT6 K 批：覆盖式不占内容宽
                .contentKey(id + ".scroll." + std::to_string(taskCount))
                .offset(columnScrollOffset(v.columns, v.columnScrolls, colIndex))
                .onChange([cb = v.act.columnScrollChanged, colIndex](float value) {
                    cb(colIndex, value);
                })
                .content([&](eui::Ui& inner, float contentWidth,
                             float viewportHeight) {
                    // 卡槽几何（卡与占位条唯一来源）：宽 = 内容宽 - 双侧 inset，x = inset
                    const float cardW = contentCardWidth(contentWidth);
                    // 归档展开最多渲染最近 20 张；其余列全量进入虚拟化窗口
                    const size_t renderLimit = (isArchive && archiveCardsOn)
                                                   ? std::min(taskCount, kArchiveRenderLimit)
                                                   : (isArchive ? size_t(0) : taskCount);
                    // H20 虚拟化：卡片定高 pitch，只构建可视区 + 上下各 3 张缓冲。
                    // 内容总高仍按 taskCount×pitch（归档展开按 20 张）由占位元素撑出，
                    // 让 scrollView 的滚动范围不受本帧构建多少张影响。
                    const auto visibleCards = virtualCardRange(
                        renderLimit,
                        columnScrollOffset(v.columns, v.columnScrolls, colIndex),
                        viewportHeight, kCardHeight + kCardGap);
                    const size_t firstCard = visibleCards.first;
                    const size_t lastCard = visibleCards.second;

                    // 占位：唯一参与流式布局的子元素，高度 = 全量卡槽高（不含末尾 gap）
                    inner.stack(id + ".vspacer")
                        .size(contentWidth, columnContentHeight(renderLimit))
                        .build();

                    // 占位条与卡片同帧布局（随列滚动跟手）
                    if (showDropBar) {
                        const float barY = std::max(
                            0.0f, static_cast<float>(v.dragHoverRow) *
                                      (kCardHeight + kCardGap) - kCardGap);
                        drawDropBar(inner, barY, cardW);
                    }
                    for (size_t i = firstCard; i < lastCard; ++i) {
                        const std::string cardId =
                            id + ".card." + std::to_string(i);
                        // overlay 定位（ignoreLayout）：卡片不参与流式布局，y 按绝对槽位；
                        // 容器宽仍占满内容宽，卡在 wrap 内按 inset 落位
                        inner.stack(cardId + ".wrap")
                            .position(0.0f, cardSlotY(i))
                            .ignoreLayout()
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

    if (showArchiveLimit) {
        const std::string limitText =
            "仅展示最近 " + std::to_string(kArchiveRenderLimit) + " 张，另有 " +
            std::to_string(taskCount - kArchiveRenderLimit) +
            " 张较早归档未展示";
        ui.rect(id + ".archivelimit.bg")
            .position(12.0f, height - kArchiveLimitH + 6.0f)
            .size(width - 24.0f, 32.0f)
            .color(t.colors.hover)
            .radius(7.0f)
            .opacity(contentAlpha)
            .build();
        ui.text(id + ".archivelimit.text")
            .position(12.0f, height - kArchiveLimitH + 6.0f)
            .size(width - 24.0f, 32.0f)
            .text(limitText)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(16.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .opacity(contentAlpha)
            .build();
    }
}

// x 只作用于卡片容器——卡内元素坐标一律相对卡内容原点，不得再加 x（重复平移 = 双倍偏移）
void drawCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
              const core::theme::Tokens& t, const Task& task,
              int colIndex, int colCount, int cardIndex,
              const BoardView& v, BoardPlate& plate) {
    (void)colCount;
    const bool done = task.completedAt.has_value();
    // 拖拽中间态：被拖卡片半透明（拖动只记录，松手才落库）
    const bool dragging = v.dragActive && v.dragTaskId == task.id;
    const float cardOpacity = dragging ? 0.45f : 1.0f;
    plate.cardW = width;  // compose 空间卡宽（拖拽命中 dpi 探针）
    const auto componentTheme = core::theme::componentTokensWeChat(t);

    // 卡片骨架用 card 组件；选中/未选中边框挂卡内交互层 .bg（挂 card 上会被不透明 .bg 盖住）
    // 2026-10-31：骨架底必须透明——card 组件默认底是不透明 cardColor，会把内层 0.92 半透卡面
    // 「垫实」成纯白（用户反馈的半透丢失）。卡面唯一载体 = 内层 .bg（kCardGlassAlpha + H16 浅染）。
    components::card(ui, id + ".card")
        .position(x, y)
        .size(width, kCardHeight)
        .radius(t.sizes.radius)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .theme(core::theme::componentTokensWeChat(t))
        .padding(0.0f)  // 卡内坐标以 content 原点为基准
        .border(0.0f, t.colors.borderDefault)  // 边框职责已移到内部交互层
        .content([&] {

    // S3.3：玻璃层退役 + 白底优先级色条替代整卡涂色（§S6.1 玻璃退役 / §S6.5 优先级色条）；
    // H16 覆盖：卡面白底改为优先级浅染（cardFaceColor），色条保留作硬标识
    // 归档卡灰化：透明度 0.7（§4 已归档卡片），与拖拽半透明叠加（浅染随卡整体变灰）
    const bool isArchiveCard = colIndex >= 0 &&
        static_cast<size_t>(colIndex) < v.columns.size() &&
        v.columns[static_cast<size_t>(colIndex)].kind == "archive";
    const bool isDoneCard = colIndex >= 0 &&
        static_cast<size_t>(colIndex) < v.columns.size() &&
        v.columns[static_cast<size_t>(colIndex)].kind == "done";
    // 归档/已完成列不参与逾期着色（与页头逾期计数口径一致）
    const bool overdueTint = !isArchiveCard && !isDoneCard && isOverdue(task);
    const float finalCardOpacity = cardOpacity * (isArchiveCard ? 0.7f : 1.0f);
    const float menuAnchorX =
        t.sizes.spacingLarge * 2.0f +
        plate.columnX[static_cast<size_t>(colIndex)] +
        plate.columnWidth[static_cast<size_t>(colIndex)] * 0.5f;
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, kCardHeight)
        .color(cardFaceColor(task.priority, t))
        .shadow(components::theme::shadow(componentTheme, 3.0f, 1.0f, 0.06f, 0.03f))
        .border(1.0f, t.colors.borderDefault)
        .radius(8.0f)
        .opacity(finalCardOpacity)
        .onClick([cb = v.act.cardClicked, task] { cb(task); })
        .onContextMenu([cb = v.act.cardMenu, task, colIndex, cardIndex, menuAnchorX](
                           const eui::PointerEvent&, const eui::Rect&) {
            cb(task, colIndex, cardIndex, menuAnchorX);
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

    // S3.3 #2 / S6.5：优先级色条（白底左侧 3px 色条替代整卡涂色）
    eui::Color barColor = priorityColor(task.priority, t);
    ui.rect(id + ".priobar")
        .position(0.0f, y)
        .size(3.0f, kCardHeight)
        .color(barColor)
        .opacity(finalCardOpacity)
        .build();


    // 卡内文本居中（S3.3：选中/逾期竖线已由优先级色条替代）；chip 行先算后画（自适应宽与绘制共用同一清单），chipX 保持卡内相对坐标
    struct PendingChip {
        std::string id;
        float w;
        std::string text;
        eui::Color fg;
    };
    std::vector<PendingChip> pendingChips;
    float chipX = kCardChipPadX;

    // H31 关联文档小图标：有关联才显示；仅提示不可点（跳转只从详情进）。
    // 占 chip 行首槽（16 宽 + 6 缝），极窄列放不下则不画图标、chips 照常。
    constexpr float kDocIconW = 16.0f;
    constexpr float kDocIconGap = 6.0f;
    const bool hasDocIcon = !task.docPath.empty();
    const bool docIconFits =
        hasDocIcon && chipX + kDocIconW + kCardChipPadRight <= width;
    if (docIconFits) {
        chipX += kDocIconW + kDocIconGap;
    }

    // 截止 chip：列宽无下限（≥6-7 列时 colW 跌破 130），固定 66px 会越出卡右缘——自适应剩余空间，放不下就跳过
    if (task.dueAt.has_value()) {
        const std::string dueText = formatDue(task.dueAt.value());
        const float dueMaxW = std::max(0.0f, width - kCardChipPadRight - chipX);
        const float dueW = std::min(66.0f, dueMaxW);
        if (dueW >= 24.0f) {
            pendingChips.push_back({id + ".due", dueW, dueText,
                                    overdueTint ? t.colors.errorText
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
        // 原型 chip 格式：MM-DD 归档（formatDue 复用同一日期口径）
        const std::string archText = formatDue(task.archivedAt.value()) + " 归档";
        const float archW = chipWidth(archText);
        const float avail = std::max(0.0f, width - kCardChipPadRight - chipX);
        if (archW <= avail) {
            pendingChips.push_back({id + ".archived", archW, archText,
                                    t.colors.textSecondary});
        }
    }
    if (!task.tags.empty()) {
        size_t shown = 0;
        for (size_t ti = 0; ti < task.tags.size() && shown < 2; ++ti) {
            const float tw = chipWidth(task.tags[ti]);
            if (chipX + tw > width - kCardChipPadRight) break;
            pendingChips.push_back({id + ".tag." + std::to_string(ti), tw,
                                    task.tags[ti], t.colors.info});
            chipX += tw + 6.0f;
            ++shown;
        }
        if (task.tags.size() > shown) {
            const std::string more = "+" + std::to_string(task.tags.size() - shown);
            const float moreW = chipWidth(more);
            // 放不下就不画
            if (chipX + moreW <= width - kCardChipPadRight) {
                pendingChips.push_back({id + ".tag.more", moreW, more,
                                        t.colors.textTertiary});
            }
        }
    }

    // 内容组垂直居中：组 = 标题行 +（有 chip 或文档图标时）chip 行（常量全走 todo_ui_logic.h）
    const bool chipRow = !pendingChips.empty() || docIconFits;
    const float groupTop = cardContentGroupTop(chipRow);
    const float chipY = y + groupTop + kCardTitleLineH + kCardTitleToChipGap;

    // 文档小图标（chip 行首）：纯提示，不挂 onClick（跳转只从详情弹窗进）
    if (docIconFits) {
        ui.text(id + ".docicon")
            .position(kCardChipPadX, chipY)
            .size(kDocIconW, kChipHeight)
            .icon(0xF15B)  // fa-file
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .lineHeight(kChipHeight)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .opacity(finalCardOpacity)
            .build();
    }

    // 序号徽章已移除（§4 卡片构成 = 标题+优先级+截止，不含序号）；标题可用全宽
    //（UAT6 misc2：左右内边距 40→28，双侧减 12px 让给标题字符；Ellipsis 截断机制不变）
    ui.text(id + ".title")
        .position(kCardPadX, y + groupTop)
        .size(width - 2.0f * kCardPadX, kCardTitleLineH)
        .text(task.title)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(kCardTitleLineH)
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .maxWidth(width - 2.0f * kCardPadX)
        .overflow(core::TextOverflow::Ellipsis)
        // S3.3：逾期标题红；归档标题二级灰（textSecondary）；常规标题走主题一级文字
        .color(isArchiveCard ? t.colors.textSecondary
                             : (overdueTint ? t.colors.errorText
                                            : t.colors.textPrimary))
        // done 0.78：textPrimary 压白卡 7.2:1（0.6 仅 4.04，低于正文 4.5 档）
        .opacity(done ? 0.78f : finalCardOpacity)
        .build();

    // chip 行按清单绘制（x 从左往右，y 随内容组；左内边距 16→10）
    float drawX = kCardChipPadX;
    for (const auto& chip : pendingChips) {
        drawChip(ui, chip.id, drawX, chipY, chip.w, t, chip.text, chip.fg,
                 t.colors.hover, finalCardOpacity);
        drawX += chip.w + 6.0f;  // 自适应宽 + 6 间距
    }

    // 完成打勾（标题行尾角标：y 随标题行走内容组顶）。UAT6 misc2：角标 24→16px、
    // 右缘随 chip 右边距右移（width-36 → width-24）；标题框右缘（width-28）到
    // 角标左缘（width-24）保持 4px 间隙，让位公式同文件头常量注释。
    if (done) {
        ui.text(id + ".done")
            .position(width - kCardChipPadRight - kCardDoneBadgeW, y + groupTop)
            .size(kCardDoneBadgeW, kCardTitleLineH)
            .icon(0xF00C)  // fa-check（✓ 字符混排正文不可行，走 icon 通道）
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .lineHeight(18.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.success)
            .fontWeight(700)
            .opacity(finalCardOpacity)
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
            // 骨架底透明：卡面唯一载体 = 内层 .bg（否则不透明默认底会把半透垫实）
            components::card(ui, "todo.dragghost.card")
                .position(0.0f, 0.0f)
                .size(w, kCardHeight)
                .radius(t.sizes.radius)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .theme(core::theme::componentTokensWeChat(t))
                .padding(0.0f)
                .border(0.0f, t.colors.borderDefault)
                .content([&] {
            // S3.3：副本匹配新卡风——白底 + 3px 优先级色条 + Sm 阴影（同 drawCard）；
            // H16：副本底同真卡走优先级浅染（cardFaceColor）
                    const auto ghostComponentTheme = core::theme::componentTokensWeChat(t);
                    ui.rect("todo.dragghost.bg")
                        .position(0.0f, 0.0f)
                        .size(w, kCardHeight)
                        .color(cardFaceColor(task->priority, t))
                        .shadow(components::theme::shadow(ghostComponentTheme, 3.0f, 1.0f, 0.06f, 0.03f))
                        .border(1.0f, t.colors.borderDefault)
                        .radius(8.0f)
                        .build();
                    eui::Color ghostBarColor = priorityColor(task->priority, t);
                    ui.rect("todo.dragghost.priobar")
                        .position(0.0f, 0.0f)
                        .size(3.0f, kCardHeight)
                        .color(ghostBarColor)
                        .build();
                    // 标题同真卡居中形态（副本不画 chips = 无 chip 卡：独占整卡居中）。
                    // 省略号三件套同真卡；序号徽章已移除，真卡与副本标题宽度一致（同 kCardPadX，
                    // UAT6 misc2 随真卡 40→28 收紧）
                    ui.text("todo.dragghost.title")
                        .position(kCardPadX, cardContentGroupTop(false))
                        .size(w - 2.0f * kCardPadX, kCardTitleLineH)
                        .text(task->title)
                        .fontSize(t.sizes.fontSizeBody - 1.0f)
                        .lineHeight(kCardTitleLineH)
                        .fontWeight(600)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .maxWidth(w - 2.0f * kCardPadX)
                        .overflow(core::TextOverflow::Ellipsis)
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace module::todo::ui
