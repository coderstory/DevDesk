// src/module_todo/todo_ui_dialogs.cpp
// Todo 浮层区绘制 + 提交（R5）：6 个对话框 + 右键菜单 + datepicker。
// 每个对话框的「表单字段 → 提交」与它的绘制紧邻定义；几何、文案、元素 id、
// 控制流逐字搬运自 todo_module.cpp（只把模块成员访问换成 DialogSet/DialogHost 入参）。
#include "todo_dialogs.h"

#include "todo_ui_common.h"

#include <core/component_theme.h>
#include <core/router.h>
#include <core/theme.h>
#include <core/ui_geometry.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>

#include <components/button.h>
#include <components/contextmenu.h>
#include <components/datepicker.h>
#include <components/input.h>

#include <ctime>
#include <string>
#include <vector>

namespace module::todo::ui {

// ============ 新建任务弹窗 ============

// 审计 [19]：提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；
// 成功 → 关窗 + toast 确认。此前「先关弹窗再报 toast」，反馈隔在窗外。
void NewTaskDialog::submit(PopupStack& popups, const DialogHost& host) {
    const int64_t col = columnId;
    const std::string text = title;
    const std::string err = host.createTask(col, text);
    if (!err.empty()) {
        error = err;
        return;
    }
    popups.close(Popup::NewTask);
}

void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t, DialogSet& d,
                       const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokens(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 230.0f;

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.newtask";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::NewTask);
    frame.zIndex = d.popups.zOf(Popup::NewTask);
    frame.onOpenChange = [&d](bool v) { d.popups.setOpen(Popup::NewTask, v); };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "todo.dialog.newtask.title", w, "新建任务",
                                    t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

            // 目标列提示
            std::string colName = "?";
            for (const auto& c : *host.columns) {
                if (c.id == d.newTask.columnId) colName = c.title;
            }
            ui_kit::drawDialogText(ui, "todo.dialog.newtask.col", w,
                                   "将创建在「" + colName + "」列",
                                   t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                                   {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

            components::input(ui, "todo.dialog.newtask.input")
                .position(28.0f, 84.0f)
                .size(w - 56.0f, 44.0f)
                .value(d.newTask.title)
                .placeholder("任务标题…")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) {
                    d.newTask.title = text;
                    d.newTask.error.clear();  // 审计 [19]：错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.newTask.submit(d.popups, host); })
                // 审计 [21]：Esc=取消（关弹窗不落库，同「取消」按钮）
                .onCancel([&d] { d.popups.close(Popup::NewTask); })
                .build();

            // 审计 [19]：内嵌红字错误行（照抄 claude formError_ 渲染——
            // 校验/创建失败就地可见，弹窗不关）
            ui_kit::drawDialogText(ui, "todo.dialog.newtask.error", w, d.newTask.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::Secondary;
            cancel.onClick = [&d] { d.popups.close(Popup::NewTask); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "创建";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::Primary;
            confirm.onClick = [&d, &host] { d.newTask.submit(d.popups, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.newtask", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// ============ 新建列弹窗（审计 [24]）============
// 「+ 列」不再即时落库，先走命名弹窗，与任务/笔记/供应商 Profile 的
// 创建范式统一。审计建议「预填 新列 N 并全选」——EUI-NEO Input 无
// 程序化 focus/selectAll 接口，全选预填产品层做不了；改用与新建任务
// 弹窗同款等价实现：空值 + placeholder 显示默认名，确认时留空=采用
// 「新列 N」、输入名字=按名字建，两条路径用户都无需额外编辑动作。

// 审计 [19]：显式输入的列名撞名 / 创建失败 → 弹窗内嵌红字，弹窗不关。
// 留空采用默认名「新列 N」的便利路径不查重（默认名是序号兜底，撞名
// 也不该把用户困在弹窗里）。失败仍关闭的话与「关弹窗+toast」旧通道同罪。
void NewColumnDialog::submit(PopupStack& popups, const DialogHost& host) {
    const bool defaulted = title.empty();
    const std::string text = defaulted
        ? defaultColumnTitle(host.columns->size())
        : title;
    if (!defaulted && columnTitleTaken(*host.columns, text, 0)) {
        error = "该列名已存在，请换一个";
        return;
    }
    const std::string err = host.createColumn(text);
    if (!err.empty()) {
        error = err;
        return;
    }
    popups.close(Popup::NewColumn);
}

void drawNewColumnDialog(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t, DialogSet& d,
                         const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokens(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 230.0f;

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.newcol";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::NewColumn);
    frame.zIndex = d.popups.zOf(Popup::NewColumn);
    frame.onOpenChange = [&d](bool v) { d.popups.setOpen(Popup::NewColumn, v); };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "todo.dialog.newcol.title", w, "新建列",
                                    t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

            ui_kit::drawDialogText(
                ui, "todo.dialog.newcol.hint", w,
                "输入列标题，留空回车则用「" +
                    defaultColumnTitle(host.columns->size()) + "」",
                t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

            components::input(ui, "todo.dialog.newcol.input")
                .position(28.0f, 84.0f)
                .size(w - 56.0f, 44.0f)
                .value(d.newColumn.title)
                .placeholder(defaultColumnTitle(host.columns->size()))
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) {
                    d.newColumn.title = text;
                    d.newColumn.error.clear();  // 审计 [19]：错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.newColumn.submit(d.popups, host); })
                // 审计 [21]：Esc=取消走 onCancel（原 per-dialog onKeyEvent
                // passthrough 拦截 hack 已由上游 onCancel 取代移除）。
                // 输入框未聚焦的兜底走 app 级 Module::onKeyEvent（见 TodoModule）。
                .onCancel([&d] { d.popups.close(Popup::NewColumn); })
                .build();

            // 审计 [19]：内嵌红字错误行（claude formError_ 同款渲染）
            ui_kit::drawDialogText(ui, "todo.dialog.newcol.error", w, d.newColumn.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::Secondary;
            cancel.onClick = [&d] { d.popups.close(Popup::NewColumn); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "创建";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::Primary;
            confirm.onClick = [&d, &host] { d.newColumn.submit(d.popups, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.newcol", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// ============ 右键菜单 ============
// 菜单项点击分发（onContextSelectPath）留在模块：它编排的是各弹窗的打开与
// 写操作（详情/重命名/删除确认/移动/改色），不是菜单自身的状态。
void drawContextMenu(eui::Ui& ui, const eui::Screen& screen,
                     const core::theme::Tokens& t, DialogSet& d,
                     const DialogHost& host,
                     const std::map<int64_t, float>& columnScrolls) {
    const auto themeTokens = core::theme::componentTokens(t);
    std::vector<components::ContextMenuItem> items;
    if (d.ctx.taskId != 0) {
        const Task* self = findTask(*host.tasks, d.ctx.taskId);
        if (self == nullptr) return;
        items.push_back({"编辑"});
        items.push_back({"上移"});
        items.push_back({"下移"});
        for (const auto& c : *host.columns) {
            if (c.id != self->columnId) {
                items.push_back({"移动到「" + c.title + "」"});
            }
        }
        items.push_back({"删除"});
    } else if (d.ctx.columnId != 0) {
        if (findColumn(*host.columns, d.ctx.columnId) == nullptr) return;
        items.push_back({"重命名列"});
        items.push_back({"左移列"});
        items.push_back({"右移列"});
        // 改色子选（spec §8.2 承诺；预设与 ui::columnDotColor 映射表对应）
        std::vector<components::ContextMenuItem> colorItems;
        for (int i = 0; i < kColorPresetCount; ++i) {
            colorItems.push_back({kColorPresets[i].label});
        }
        items.push_back({"改色", std::move(colorItems)});
        items.push_back({"删除列"});
    } else {
        return;
    }

    const BoardGeom g = boardGeom(screen.width, host.columns->size(), t);
    // 锚点统一经 core::menuAnchorForPointer（UI 现代化 M1-2，spec §3.1）：
    // 板几何 = 页面空间，先还原成窗口坐标（加页面原点）再进统一换算
    // （两边对消，但换算本体唯一、Y 与滚动口径收进函数）；x 取列中芯、
    // y 取卡片视觉底（未滚几何），滚动偏移经 scrollOffsetY 叠加——
    // 4523e09 的「y 减 columnScrollOffset」语义由此保留（列头菜单
    // cardIndex < 0 在滚动区外，scrollY = 0）。宽度交 autoWidth
    // （上游 M4-2 已合入，7164d55），不再写死 190。
    const float x = cardLeft(g, d.ctx.columnIndex) + g.colW * 0.5f;
    const float scrollY = d.ctx.cardIndex >= 0
        ? columnScrollOffset(*host.columns, columnScrolls,
                             static_cast<size_t>(d.ctx.columnIndex))
        : 0.0f;
    const float y = d.ctx.cardIndex >= 0
        ? cardTop(g, d.ctx.cardIndex) + kCardHeight + 4.0f
        : g.boardTop + 4.0f;
    const ::core::Router* router = ::core::appRouter();
    const core::MenuAnchor anchor = core::menuAnchorForPointer(
        x + (router ? router->pageOriginX() : 0.0f), y,
        router ? router->pageOriginX() : 0.0f, scrollY, 1.0f);

    components::contextMenu(ui, "todo.ctx")
        .open(d.popups.isOpen(Popup::Ctx))
        .screen(screen.width, screen.height)
        .position(anchor.x, anchor.y)
        .autoWidth()
        .items(std::move(items))
        .theme(themeTokens)
        .zIndex(d.popups.zOf(Popup::Ctx))
        .onSelectPath([&host](const std::vector<int>& path) {
            host.contextSelectPath(path);
        })
        .onOpenChange([&d](bool v) { d.popups.setOpen(Popup::Ctx, v); })
        .build();
}

// ============ 重命名列弹窗 ============

// 审计 [19]：重命名提交——空名此前静默返回（弹窗已关、零反馈），
// 撞名此前根本不校验；统一为弹窗内嵌红字 + 弹窗不关。
void RenameDialog::submit(PopupStack& popups, const DialogHost& host) {
    const int64_t id = columnId;
    const std::string text = title;
    const std::string err = host.renameColumn(id, text);
    if (!err.empty()) {
        error = err;
        return;
    }
    popups.close(Popup::Rename);
}

void drawRenameDialog(eui::Ui& ui, const eui::Screen& screen,
                      const core::theme::Tokens& t, DialogSet& d,
                      const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokens(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 210.0f;

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.rename";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::Rename);
    frame.zIndex = d.popups.zOf(Popup::Rename);
    frame.onOpenChange = [&d](bool v) { d.popups.setOpen(Popup::Rename, v); };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "todo.dialog.rename.title", w, "重命名列",
                                    t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

            components::input(ui, "todo.dialog.rename.input")
                .position(28.0f, 68.0f)
                .size(w - 56.0f, 44.0f)
                .value(d.rename.title)
                .placeholder("列标题…")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) {
                    d.rename.title = text;
                    d.rename.error.clear();  // 审计 [19]：错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.rename.submit(d.popups, host); })
                // 审计 [21]：Esc=取消（关弹窗不改名，同「取消」按钮）
                .onCancel([&d] { d.popups.close(Popup::Rename); })
                .build();

            // 审计 [19]：内嵌红字错误行（claude formError_ 同款渲染）
            ui_kit::drawDialogText(ui, "todo.dialog.rename.error", w, d.rename.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 118.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::Secondary;
            cancel.onClick = [&d] { d.popups.close(Popup::Rename); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "保存";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::Primary;
            confirm.onClick = [&d, &host] { d.rename.submit(d.popups, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.rename", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// ============ 删除确认弹窗 ============
// 双模式：taskId > 0 = 删任务；否则 columnId > 0 = 删列
//（外键 CASCADE 会连带删掉列下全部任务）
void ConfirmDeleteDialog::confirm(DialogSet& d, const DialogHost& host) {
    if (forTask()) {
        const int64_t id = taskId;
        d.popups.close(Popup::Confirm);
        taskId = 0;
        // 详情弹窗发起的删除：确认后才关详情（取消路径不关，回到弹窗--
        // 审计 2026-09-23 [2]）
        d.popups.close(Popup::Detail);
        d.detail.discardArmed = false;
        host.deleteTask(id);
        return;
    }
    const int64_t id = columnId;
    d.popups.close(Popup::Confirm);
    host.deleteColumn(id);
}

void drawConfirmDeleteDialog(eui::Ui& ui, const eui::Screen& screen,
                             const core::theme::Tokens& t, DialogSet& d,
                             const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokens(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 200.0f;
    const bool delTask = d.confirm.forTask();
    std::string colName = "?";
    int taskCount = 0;
    if (!delTask) {
        if (const Column* col = findColumn(*host.columns, d.confirm.columnId)) {
            colName = col->title;
            taskCount = static_cast<int>(tasksInColumn(*host.tasks, col->id).size());
        }
    }
    const std::string title = delTask ? "删除任务" : "删除列";
    const std::string body = delTask
        ? "将删除任务「" + d.confirm.taskTitle + "」，不可恢复。"
        : "将删除「" + colName + "」列及其中 " +
              std::to_string(taskCount) + " 个任务，不可恢复。";

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.delcol";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::Confirm);
    // 审计 2026-09-23 [2]：从详情弹窗发起的删除确认叠在详情之上
    //（z 由打开顺序给出，见 PopupStack::zOf）
    frame.zIndex = d.popups.zOf(Popup::Confirm);
    frame.onOpenChange = [&d](bool v) { d.popups.setOpen(Popup::Confirm, v); };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "todo.dialog.delcol.title", w, title,
                                    t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);
            ui_kit::drawDialogText(ui, "todo.dialog.delcol.body", w, body,
                                   t.sizes.fontSizeBody - 1.0f, t.colors.textSecondary,
                                   {.y = 60.0f, .height = 40.0f, .lineHeight = 20.0f});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::Secondary;
            cancel.onClick = [&d] { d.popups.close(Popup::Confirm); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "删除";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::Danger;
            confirm.onClick = [&d, &host] { d.confirm.confirm(d, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.delcol", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// ============ 截止日期选择（EUI-NEO datepicker 滚轮，替代 4 个固定预设）============
void drawDatePicker(eui::Ui& ui, const eui::Screen& screen,
                    const core::theme::Tokens& t, DialogSet& d,
                    const DialogHost& host) {
    (void)host;
    if (!d.popups.isOpen(Popup::DatePick)) return;
    const auto themeTokens = core::theme::componentTokens(t);
    int year = 0, month = 0, day = 0;
    if (!epochToYmd(d.detail.dueAt.value_or(0), year, month, day)) {
        epochToYmd(static_cast<int64_t>(std::time(nullptr)), year, month, day);
    }
    components::datePicker(ui, "todo.datepicker")
        .open(true)
        .screen(screen.width, screen.height)
        .size(380.0f, 260.0f)
        .date(year, month, day)
        .theme(themeTokens)
        .zIndex(d.popups.zOf(Popup::DatePick))  // 由打开顺序给出（详情之上）
        .onChange([&d](int yy, int mm, int dd) {
            d.detail.dueAt = ymdToEpoch(yy, mm, dd);
        })
        .onOpenChange([&d](bool v) { d.popups.setOpen(Popup::DatePick, v); })
        .build();
}

// ============ 逾期任务弹窗（三件套 2）============
// 数据源 service_->listOverdue()（模块打开弹窗时刷新）；
// 行点击 = 关闭本弹窗并打开该任务详情
void drawOverdueDialog(eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t, DialogSet& d,
                       const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokens(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);
    const float rowH = 44.0f;
    constexpr float kMaxDialogH = 640.0f;
    // 行数按封顶高度截断（终验复审：≥13 条时高度封顶 640 但行仍全量渲染，
    // 第 13 条起画到弹窗外；标题保留总数，提示行标注截断）
    const size_t maxRows =
        static_cast<size_t>((kMaxDialogH - 96.0f) / rowH);  // = 12
    const size_t shown = std::min(d.overdue.tasks.size(), maxRows);
    const float h =
        std::min(kMaxDialogH, 96.0f + rowH * static_cast<float>(shown));

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.overdue";
    frame.width = ui_kit::DialogWidth::Wide;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::Overdue);
    frame.zIndex = d.popups.zOf(Popup::Overdue);
    frame.onOpenChange = [&d](bool v) { d.popups.setOpen(Popup::Overdue, v); };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui.text("todo.dialog.overdue.title.icon")
                .position(28.0f, 20.0f)
                .size(18.0f, 28.0f)
                .icon(0xF071)  // fa-triangle-exclamation（⚠ 字符换 FA）
                .fontSize(t.sizes.fontSizeLarge + 2.0f)
                .lineHeight(28.0f)
                .color(t.colors.error)
                .build();
            ui_kit::drawDialogTitle(
                ui, "todo.dialog.overdue.title", w,
                "逾期任务（" + std::to_string(d.overdue.tasks.size()) + "）",
                t.sizes.fontSizeLarge + 2.0f, t.colors.error,
                {.x = 50.0f, .y = 20.0f, .inset = 78.0f});
            std::string hint = "任务名 · 所属列 · 截止 · 逾期天数，点击行跳转详情";
            if (d.overdue.tasks.size() > shown) {
                hint += "（仅显示前 " + std::to_string(shown) + " 条）";
            }
            ui_kit::drawDialogText(ui, "todo.dialog.overdue.hint", w, hint,
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 50.0f, .height = 16.0f, .lineHeight = 16.0f});

            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            for (size_t i = 0; i < shown; ++i) {
                const Task& task = d.overdue.tasks[i];
                const float y = 74.0f + static_cast<float>(i) * rowH;
                const std::string rowId =
                    "todo.dialog.overdue.row." + std::to_string(i);
                const Column* col = findColumn(*host.columns, task.columnId);
                ui.rect(rowId)
                    .position(20.0f, y)
                    .size(w - 40.0f, rowH - 6.0f)
                    .states(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .radius(t.sizes.radius)
                    .onClick([&d, &host, taskId = task.id] {
                        d.popups.close(Popup::Overdue);
                        if (const Task* found = findTask(*host.tasks, taskId)) {
                            host.openDetail(*found);
                        }
                    })
                    .build();
                ui.text(rowId + ".title")
                    .position(32.0f, y + 10.0f)
                    .size(w - 300.0f, 20.0f)
                    .text(task.title)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .fontWeight(500)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(rowId + ".col")
                    .position(w - 262.0f, y + 10.0f)
                    .size(120.0f, 20.0f)
                    .text(col != nullptr ? col->title : "?")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.text(rowId + ".due")
                    .position(w - 136.0f, y + 10.0f)
                    .size(60.0f, 20.0f)
                    .text(task.dueAt.has_value() ? formatDue(task.dueAt.value())
                                                 : "-")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.text(rowId + ".days")
                    .position(w - 68.0f, y + 10.0f)
                    .size(48.0f, 20.0f)
                    // 入选按秒级（listOverdue 的 due_at < now），日历日差却
                    // 可能为 0（1 秒前到期）→ 行文案至少 1 天，与「已逾期」一致
                    .text("逾期" + std::to_string(
                               std::max(1, overdueDays(task.dueAt.value_or(0), now))) + "天")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.error)
                    .build();
            }
    });
}

// ============ 任务详情 / 编辑弹窗 ============

void DetailDialog::reset(const Task& task) {
    taskId = task.id;
    title = task.title;  // UI 整改 15：不再装载列（详情内无「移动到」）
    description = task.description.value_or("");
    priority = task.priority;
    dueAt = task.dueAt;
    completed = task.completedAt.has_value();
    tags = task.tags;
    tagInput.clear();
    discardArmed = false;
}

// 标签输入回车 → 追加（去空白 / 去重 / 空忽略；保存时才落库）
void DetailDialog::addTag() {
    std::string tag;
    for (char c : tagInput) {
        if (c != ' ' && c != '\t') tag += c;
    }
    tagInput.clear();
    if (tag.empty()) return;
    for (const auto& existing : tags) {
        if (existing == tag) return;
    }
    tags.push_back(std::move(tag));
}

// 表单与库中任务是否有差异（backdrop 关闭守卫用）。任务不在内存
//（已被别的路径删掉）时无法比较，按不脏处理
bool DetailDialog::dirty(const std::vector<Task>& tasks) const {
    const Task* orig = findTask(tasks, taskId);
    if (orig == nullptr) return false;
    // UI 整改 15：比列已移除——详情内不再能改列（「移动到」删除），
    // 列身份一律以库中为准，不存在「未保存的列改动」
    return orig->title != title ||
           orig->description.value_or("") != description ||
           orig->priority != priority ||
           orig->dueAt != dueAt ||
           orig->completedAt.has_value() != completed ||
           orig->tags != tags;
}

// 表单字段 → Task（含列/position 保留——编辑不改序不切列，UI 整改 15；
// completedAt 取当前完成态）
Task DetailDialog::collect(const std::vector<Task>& tasks) const {
    const Task* orig = findTask(tasks, taskId);
    Task updated;
    updated.id = taskId;
    // 列身份以库中为准（详情内无「移动到」）
    updated.columnId = orig != nullptr ? orig->columnId : 0;
    updated.title = title;
    if (!description.empty()) {
        updated.description = description;
    }
    updated.priority = priority;
    updated.dueAt = dueAt;
    updated.completedAt = completed
        ? std::optional<int64_t>(static_cast<int64_t>(std::time(nullptr)))
        : std::nullopt;
    updated.tags = tags;
    // 保留原 position（编辑不改序；此前硬编码 0 会把任务顶到列首）
    updated.position = orig != nullptr ? orig->position : 0;
    return updated;
}

// 表单原样落库；false = 保存失败（已 setStatus 报错），调用方应保持
// 弹窗打开并放弃后续动作
bool DetailDialog::save(const DialogHost& host) const {
    return host.updateTask(collect(*host.tasks));
}

// 详情弹窗关闭统一入口（审计 M5：Esc 兜底与 backdrop 点击共用同一份守卫，
// 避免两处各写一遍「未存改动先警告」而语义漂移）。
// 返回 true = 本次真的关了；false = 被未存改动守卫拦下（弹窗仍在栈顶）
bool DetailDialog::requestClose(PopupStack& popups, const DialogHost& host) {
    // 产品审计 2026-09-23 [2]：有未存改动时第一次只警告不关，再触发一次
    // = 明确放弃修改才关。EUI-NEO dialog 无 dismissable 开关（中期上游补），
    // 两击语义是当前组件能力下的守卫
    if (dirty(*host.tasks) && !discardArmed) {
        discardArmed = true;
        host.warn("有未保存修改：点「保存」落库，或再点一次遮罩放弃");
        return false;
    }
    discardArmed = false;
    popups.close(Popup::Detail);
    return true;
}

void drawDetailDialog(eui::Ui& ui, const eui::Screen& screen,
                      const core::theme::Tokens& t, DialogSet& d,
                      const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokens(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);
    // UI 整改 15：详情弹窗「移动到」区（标题 + 按钮行 + 列多时每列一行
    // +40px）已删除——切列改走拖拽/右键菜单，高度收掉该段（640 基准 -
    // 标签 16 + 按钮行 32 - 过渡 4 = 588）；底部操作区 y 随 h-58 上移
    const float h = 588.0f;

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.detail";
    frame.width = ui_kit::DialogWidth::Wide;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::Detail);
    frame.zIndex = d.popups.zOf(Popup::Detail);
    // 产品审计 2026-09-23 [2]：backdrop 点击（dialog 唯一外部关闭
    // 通道）→ 与 Esc 兜底共用同一份守卫（DetailDialog::requestClose）
    frame.onOpenChange = [&d, &host](bool v) {
        if (!v) d.detail.requestClose(d.popups, host);
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            ui_kit::drawDialogTitle(ui, "todo.dialog.detail.title", w, "任务详情",
                                    t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary,
                                    {.y = 22.0f});

            // 标题
            ui_kit::drawDialogText(ui, "todo.dialog.detail.tlabel", w, "标题",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 58.0f, .width = 80.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            components::input(ui, "todo.dialog.detail.input.title")
                .position(28.0f, 76.0f)
                .size(w - 56.0f, 40.0f)
                .value(d.detail.title)
                .placeholder("任务标题…")
                .fontSize(t.sizes.fontSizeBody - 1.0f)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) { d.detail.title = text; })
                .build();

            // 描述
            ui_kit::drawDialogText(ui, "todo.dialog.detail.dlabel", w, "描述",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 126.0f, .width = 80.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            components::input(ui, "todo.dialog.detail.input.desc")
                .position(28.0f, 144.0f)
                .size(w - 56.0f, 88.0f)
                .value(d.detail.description)
                .placeholder("补充说明…（可多行）")
                .fontSize(t.sizes.fontSizeSmall)
                .multiline(true)
                .scrollbar(true)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) {
                    d.detail.description = text;
                })
                .build();

            // 优先级
            ui_kit::drawDialogText(ui, "todo.dialog.detail.plabel", w, "优先级",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 244.0f, .width = 80.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            const Priority prios[4] = {Priority::None, Priority::Low,
                                       Priority::Medium, Priority::High};
            for (int i = 0; i < 4; ++i) {
                const bool active = (d.detail.priority == prios[i]);
                const float bw = 100.0f;
                // UI 整改 17 ②：按钮**自己的底色 = 它代表的优先级色**
                // （priorityButtonBg：未选中 = 强度表浅 tint，选中 = 0.55 深
                // tint；与卡片底同源同一张强度表 + 同一张 priorityColor 表）。
                // 文字色改 textPrimary（原未选中用 textSecondary）：浅色档
                // 0.34 tint 的「高」按钮上 textSecondary 实测 4.17:1 < WCAG AA
                // 4.5:1，textPrimary 在同底上 ≥ 8:1；选中档亦 ≥ 6:1（深浅主题
                // 皆然）。选中/未选中靠**底色深浅 + 边框**双通道区分
                // （1px borderDefault → 2px 该优先级色），不再靠 primarySoftBg
                // ——否则「高」是红的、「无」是主色橘，色底语义自相矛盾
                components::button(ui, "todo.dialog.detail.prio." + std::to_string(i))
                    .position(28.0f + static_cast<float>(i) * (bw + 8.0f), 264.0f)
                    .size(bw, 32.0f)
                    .text(priorityLabel(prios[i]))
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(priorityButtonBg(prios[i], t, active,
                                             t.colors.surfaceElevated),
                            priorityButtonBg(prios[i], t, active, t.colors.hover),
                            priorityButtonBg(prios[i], t, active, t.colors.active))
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(active ? 2.0f : 1.0f,
                            active ? priorityColor(prios[i], t)
                                   : t.colors.borderDefault)
                    .onClick([&d, i, prios] { d.detail.priority = prios[i]; })
                    .build();
            }

            // 截止日期：清除 + datepicker 滚轮选任意日期（原 4 个固定预设已替换）
            ui_kit::drawDialogText(ui, "todo.dialog.detail.dlabel2", w, "截止日期",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 308.0f, .width = 120.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            const bool hasDue = d.detail.dueAt.has_value();
            components::button(ui, "todo.dialog.detail.due.clear")
                .position(28.0f, 328.0f)
                .size(100.0f, 32.0f)
                .text(!hasDue ? "未设置" : "清除")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .colors(!hasDue ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(!hasDue ? t.colors.primaryText : t.colors.textSecondary)
                .radius(t.sizes.radius)
                .border(1.0f, !hasDue ? t.colors.primary : t.colors.borderDefault)
                .onClick([&d] { d.detail.dueAt = std::nullopt; })
                .build();
            // 📅 emoji 换 fa-calendar（码点已对 EUI-NEO FA7 cmap 离线核对）
            components::button(ui, "todo.dialog.detail.due.pick")
                .position(136.0f, 328.0f)
                .size(180.0f, 32.0f)
                .text(hasDue ? formatDue(d.detail.dueAt.value()) : "选择日期…")
                .icon(0xF133)
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .colors(hasDue ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                        t.colors.hover, t.colors.active)
                .textColor(hasDue ? t.colors.primaryText : t.colors.textSecondary)
                .radius(t.sizes.radius)
                .border(1.0f, hasDue ? t.colors.primary : t.colors.borderDefault)
                .onClick([&d] { d.popups.open(Popup::DatePick); })
                .build();

            // 标签（三件套 3）：chip × 删除 + 输入回车添加；保存时随
            // detailTags_ 走 updateTask 的 JSON tags 序列化（W9 已确认往返）
            ui_kit::drawDialogText(ui, "todo.dialog.detail.taglabel", w, "标签",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 372.0f, .width = 120.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            {
                float chipX = 28.0f;
                for (size_t ti = 0; ti < d.detail.tags.size(); ++ti) {
                    const float tw = chipWidth(d.detail.tags[ti]);
                    // ponytail: 单行不换行，放不下即截断（+N 省略）；标签很多时再改多行
                    if (chipX + tw + 22.0f > w - 28.0f) break;
                    const std::string chipId =
                        "todo.dialog.detail.tag." + std::to_string(ti);
                    drawChip(ui, chipId, chipX, 392.0f, tw, t, d.detail.tags[ti],
                             t.colors.info, t.colors.surface);
                    components::button(ui, chipId + ".x")
                        // UI 整改 18：chip 高 18→22（kChipHeight），删除按钮
                        // 高 22 与 chip 同高 → y 跟到 392 与 chip 顶对齐
                        // （旧 390 是配 18 高 chip 的居中位，不改会上下错 2px）
                        .position(chipX + tw + 1.0f, 392.0f)
                        .size(20.0f, 22.0f)
                        .text("×")
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.surface, t.colors.hover, t.colors.active)
                        .textColor(t.colors.textSecondary)
                        .radius(11.0f)
                        .onClick([&d, ti] {
                            if (ti < d.detail.tags.size()) {
                                d.detail.tags.erase(d.detail.tags.begin() +
                                                    static_cast<long>(ti));
                            }
                        })
                        .build();
                    chipX += tw + 26.0f;
                }
            }
            components::input(ui, "todo.dialog.detail.tag.input")
                .position(28.0f, 418.0f)
                .size(w - 56.0f, 36.0f)
                .value(d.detail.tagInput)
                .placeholder("输入标签，回车添加…")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) { d.detail.tagInput = text; })
                .onEnter([&d] { d.detail.addTag(); })
                // 审计 [21]：详情弹窗内加标签输入框，Esc=丢弃草稿（不清标签
                // 草稿、不关详情弹窗——整窗关断由「关闭」按钮负责）
                .onCancel([&d] { d.detail.tagInput.clear(); })
                .build();

            // 底部操作区（UI 整改 15：原「移动到」区已删，直接接标签区）
            const float btnY = h - 58.0f;
            components::button(ui, "todo.dialog.detail.delete")
                .position(28.0f, btnY)
                .size(96.0f, 40.0f)
                .text("删除")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                .textColor(t.colors.error)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                // 与右键菜单删除同路径：先弹确认（Popup::Confirm），不再直接落库。
                // 产品审计 2026-09-23 [2]：不关详情弹窗——确认取消后回到
                // 弹窗继续编辑（确认弹窗按打开顺序压在详情之上）
                .onClick([&d] {
                    d.confirm.openForTask(d.detail.taskId, d.detail.title);
                    d.popups.open(Popup::Confirm);
                })
                .build();

            components::button(ui, "todo.dialog.detail.toggle")
                .position(132.0f, btnY)
                .size(132.0f, 40.0f)
                .icon(d.detail.completed ? 0xF021 : 0xF00C)  // fa-arrows-rotate / fa-check
                .text(d.detail.completed ? "取消完成" : "标记完成")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                .textColor(d.detail.completed ? t.colors.textSecondary : t.colors.success)
                .iconColor(d.detail.completed ? t.colors.textSecondary
                                              : t.colors.success)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([&d, &host] {
                    const int64_t taskId = d.detail.taskId;
                    const bool wasDone = d.detail.completed;
                    // 产品审计 2026-09-23 [2]：先落表单再切完成态，
                    // 编辑中的改动不再被静默丢弃；保存失败保持弹窗
                    if (!d.detail.save(host)) return;
                    d.popups.close(Popup::Detail);
                    d.detail.discardArmed = false;
                    if (wasDone) host.uncompleteTask(taskId);
                    else host.completeTask(taskId);
                })
                .build();

            components::button(ui, "todo.dialog.detail.save")
                .position(w - 28.0f - 160.0f, btnY)
                .size(160.0f, 40.0f)
                .text("保存")
                .fontSize(t.sizes.fontSizeBody - 1.0f)
                .colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radius)
                .onClick([&d, &host] {
                    // 表单组装 + 落库统一走 DetailDialog::save（与动作按钮同一份
                    // 语义）；保存失败保持弹窗打开（错误已 setStatus 提示）
                    if (!d.detail.save(host)) return;
                    d.popups.close(Popup::Detail);
                    d.detail.discardArmed = false;
                })
                .build();
    });
}

} // namespace module::todo::ui
