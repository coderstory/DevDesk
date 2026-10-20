// Todo 浮层区绘制 + 提交：6 个对话框 + 右键菜单 + datepicker。
// 每个对话框的「表单字段 → 提交」与其绘制紧邻定义（入参 DialogSet/DialogHost）。
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
#include <map>
#include <string>
#include <vector>

namespace module::todo::ui {

// 提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；成功 → 关窗。
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
    const auto themeTokens = core::theme::componentTokensWeChat(t);
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
                    d.newTask.error.clear();  // 错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.newTask.submit(d.popups, host); })
                .onCancel([&d] { d.popups.close(Popup::NewTask); })
                .build();

            // 内嵌红字错误行（校验/创建失败就地可见，弹窗不关）
            ui_kit::drawDialogText(ui, "todo.dialog.newtask.error", w, d.newTask.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [&d] { d.popups.close(Popup::NewTask); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "创建";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [&d, &host] { d.newTask.submit(d.popups, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.newtask", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// 新建列弹窗：「+ 列」不即时落库，先走命名弹窗。Input 无程序化 focus/selectAll，
// 改用空值 + placeholder 显示默认名：留空 = 采用「新列 N」，输入 = 按名字建。

// 列名撞名/创建失败 → 弹窗内嵌红字、弹窗不关；留空采用默认名路径不查重。
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
    const auto themeTokens = core::theme::componentTokensWeChat(t);
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
                    d.newColumn.error.clear();  // 错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.newColumn.submit(d.popups, host); })
                // Esc=取消走 onCancel；输入框未聚焦的兜底走 app 级 Module::onKeyEvent
                .onCancel([&d] { d.popups.close(Popup::NewColumn); })
                .build();

            ui_kit::drawDialogText(ui, "todo.dialog.newcol.error", w, d.newColumn.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [&d] { d.popups.close(Popup::NewColumn); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "创建";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [&d, &host] { d.newColumn.submit(d.popups, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.newcol", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// 右键菜单。菜单项点击分发（onContextSelectPath）留在模块：编排各弹窗打开与写操作，非菜单自身状态
void drawContextMenu(eui::Ui& ui, const eui::Screen& screen,
                     const core::theme::Tokens& t, DialogSet& d,
                     const DialogHost& host,
                     const std::map<int64_t, float>& columnScrolls) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
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
        // 改色子选（预设与 ui::columnDotColor 映射表对应）
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
    // 锚点统一经 core::menuAnchorForPointer：x 取列中芯、y 取卡片视觉底（未滚几何），滚动偏移经 scrollOffsetY 叠加
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

// 重命名提交：空名/撞名 → 弹窗内嵌红字 + 弹窗不关。
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
    const auto themeTokens = core::theme::componentTokensWeChat(t);
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
                    d.rename.error.clear();  // 错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.rename.submit(d.popups, host); })
                .onCancel([&d] { d.popups.close(Popup::Rename); })
                .build();

            ui_kit::drawDialogText(ui, "todo.dialog.rename.error", w, d.rename.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 118.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [&d] { d.popups.close(Popup::Rename); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "保存";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [&d, &host] { d.rename.submit(d.popups, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.rename", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// 删除确认弹窗提交：taskId>0 = 删任务；否则 columnId>0 = 删列（外键 CASCADE 连带删列下任务）
void ConfirmDeleteDialog::confirm(DialogSet& d, const DialogHost& host) {
    if (forTask()) {
        const int64_t id = taskId;
        d.popups.close(Popup::Confirm);
        taskId = 0;
        // 详情弹窗发起的删除：确认后才关详情（取消路径不关，回到弹窗）
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
    const auto themeTokens = core::theme::componentTokensWeChat(t);
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
    // 从详情发起的删除确认叠在详情之上（z 由打开顺序给出）
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
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [&d] { d.popups.close(Popup::Confirm); };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "删除";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::DangerThemed;
            confirm.onClick = [&d, &host] { d.confirm.confirm(d, host); };
            ui_kit::drawDialogButtons(ui, "todo.dialog.delcol", w, h, cancel, confirm,
                                      t, themeTokens);
    });
}

// 截止日期选择（datepicker 滚轮，可选任意日期）。
// 「默认当天」机制（UAT5，不动 EUI-NEO 组件、只动本包装层）：
// ① 组件滚轮草稿只在「未激活」时才吃 .date() 提交值（syncDraft 守卫），而本层关闭态
//   根本不建组件 → 草稿 active 恒 true、第二次打开滚轮停在上一次位置而非本卡锚点。
//   → 每个打开会话首帧 releaseStateScope 放掉旧草稿，滚轮按锚点（有 due 用 due、
//   无 due 用当天）重新初始化。
// ② 组件「确定」只在「滚轮草稿 ≠ 提交值」时回调 onChange——首帧喂锚点后直接确定
//   永远不回调（无 due 的卡就此设不上「今天」）。→ 首帧喂锚点，后续帧喂 1900 哨兵
//   提交值（滚轮草稿在打开期间不被 syncDraft 重置，哨兵不进显示），使「打开不滚
//   直接确定」也回调 onChange(滚轮当前值)=当天/原值。
// 帧计数在「关闭态每帧」归零——Esc/确定/遮罩/任何 popups.close 路径全覆盖。
void drawDatePicker(eui::Ui& ui, const eui::Screen& screen,
                    const core::theme::Tokens& t, DialogSet& d,
                    const DialogHost& host) {
    (void)host;
    if (!d.popups.isOpen(Popup::DatePick)) {
        d.datePickDrawn = 0;
        return;
    }
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    int year = 0, month = 0, day = 0;
    if (!epochToYmd(d.detail.dueAt.value_or(0), year, month, day)) {
        epochToYmd(static_cast<int64_t>(std::time(nullptr)), year, month, day);
    }
    const bool firstFrame = d.datePickDrawn == 0;
    ++d.datePickDrawn;
    if (firstFrame) {
        ui.releaseStateScope("todo.datepicker.draft");
    } else {
        year = 1900;   // 哨兵提交值：合法日期（.date() clamp 不炸）且真实任务
        month = 1;     // 永远选不到 1900-01-01 →「确定」必回调 onChange
        day = 1;
    }
    components::datePicker(ui, "todo.datepicker")
        .open(true)
        .screen(screen.width, screen.height)
        .size(380.0f, 260.0f)
        .date(year, month, day)
        .theme(themeTokens)
        .zIndex(d.popups.zOf(Popup::DatePick))  // 由打开顺序给出
        .onChange([&d](int yy, int mm, int dd) {
            d.detail.dueAt = ymdToEpoch(yy, mm, dd);
        })
        .onOpenChange([&d](bool v) { d.popups.setOpen(Popup::DatePick, v); })
        .build();
}

// 逾期任务弹窗。数据源 listOverdue()（模块打开时刷新）；行点击 = 关本弹窗 + 开详情
void drawOverdueDialog(eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t, DialogSet& d,
                       const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);
    const float rowH = 44.0f;
    constexpr float kMaxDialogH = 640.0f;
    // 行数按封顶高度截断（标题保留总数，提示行标注截断）
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
                .icon(0xF071)  // fa-triangle-exclamation
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
                    .states(t.colors.wechat.secondaryButtonBg, t.colors.wechat.dividerSecondary,
                            t.colors.wechat.selectedBg)
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
                    // 省略号三件套：长任务名截断，不压右侧 列名/截止/逾期天 三列
                    .maxWidth(w - 300.0f)
                    .overflow(core::TextOverflow::Ellipsis)
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
                    // 入选按秒级，日历日差可能为 0（1 秒前到期）→ 行文案至少 1 天
                    .text("逾期" + std::to_string(
                               std::max(1, overdueDays(task.dueAt.value_or(0), now))) + "天")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.error)
                    .build();
            }
    });
}

void DetailDialog::reset(const Task& task) {
    taskId = task.id;
    title = task.title;  // 不装载列（详情内无「移动到」）
    description = task.description.value_or("");
    priority = task.priority;
    dueAt = task.dueAt;
    completed = task.completedAt.has_value();
    tags = task.tags;
    tagInput.clear();
    discardArmed = false;
}

// 标签输入回车 → 追加（去空白/去重/空忽略；保存时才落库）
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

// 表单与库中任务是否有差异（backdrop 关闭守卫用）；任务不在内存时按不脏处理
bool DetailDialog::dirty(const std::vector<Task>& tasks) const {
    const Task* orig = findTask(tasks, taskId);
    if (orig == nullptr) return false;
    return orig->title != title ||
           orig->description.value_or("") != description ||
           orig->priority != priority ||
           orig->dueAt != dueAt ||
           orig->completedAt.has_value() != completed ||
           orig->tags != tags;
}

// 表单字段 → Task（保留原列/position——编辑不改序不切列；completedAt 取当前态）
Task DetailDialog::collect(const std::vector<Task>& tasks) const {
    const Task* orig = findTask(tasks, taskId);
    Task updated;
    updated.id = taskId;
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
    // 保留原 position（硬编码 0 会把任务顶到列首）
    updated.position = orig != nullptr ? orig->position : 0;
    return updated;
}

// 表单原样落库；false = 保存失败（已 setStatus 报错），调用方保持弹窗打开
bool DetailDialog::save(const DialogHost& host) const {
    return host.updateTask(collect(*host.tasks));
}

// 详情弹窗关闭统一入口（Esc 兜底与 backdrop 点击共用同一份守卫，避免语义漂移）；false = 被未存改动守卫拦下
bool DetailDialog::requestClose(PopupStack& popups, const DialogHost& host) {
    // 有未存改动时第一次只警告不关，再触发一次才关（dialog 无 dismissable 开关，两击语义是组件能力下的守卫）
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
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);
    // 弹窗高；底部操作区 y 随 h-58 上移。「优先级 | 截止日期」同排双列后的紧凑高度：
    // 内容行收掉一整行（416→352），标签区随之上移，底部按钮带结构不变（18px 底距）。
    // 双列几何：colW = (640-56-16)/2 = 284；左列 x=28，右列 x = 28+284+16 = 328。
    // y 坐标仍是字面量体系（不随字号缩放），与 settings 卡的 roundFs 体系互不混用。
    const float h = 588.0f;
    constexpr float kCol2X = 328.0f;  // 右列（截止日期）x = 左列 28 + colW 284 + 列距 16

    ui_kit::DialogFrame frame;
    frame.id = "todo.dialog.detail";
    frame.width = ui_kit::DialogWidth::Wide;
    frame.height = h;
    frame.open = d.popups.isOpen(Popup::Detail);
    frame.zIndex = d.popups.zOf(Popup::Detail);
    // backdrop 点击（dialog 唯一外部关闭通道）→ 与 Esc 兜底共用 requestClose
    frame.onOpenChange = [&d, &host](bool v) {
        if (!v) d.detail.requestClose(d.popups, host);
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
            const bool ro = d.detail.readOnly;
            ui_kit::drawDialogTitle(ui, "todo.dialog.detail.title", w,
                                    ro ? "任务详情（只读）" : "任务详情",
                                    t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary,
                                    {.y = 22.0f});

            // 归档时间戳（归档列任务显示）；非归档列不占位
            if (ro) {
                const Task* orig = findTask(*host.tasks, d.detail.taskId);
                if (orig != nullptr && orig->archivedAt.has_value()) {
                    const std::string archStr =
                        "归档时间: " + formatArchivedAt(orig->archivedAt.value());
                    ui_kit::drawDialogText(
                        ui, "todo.dialog.detail.archived", w, archStr,
                        t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                        {.y = 50.0f, .height = 18.0f, .lineHeight = 18.0f});
                }
            }

            const float labelOffsetY = ro ? 18.0f : 0.0f;

            ui_kit::drawDialogText(ui, "todo.dialog.detail.tlabel", w, "标题",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 58.0f + labelOffsetY, .width = 80.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            components::input(ui, "todo.dialog.detail.input.title")
                .position(28.0f, 76.0f + labelOffsetY)
                .size(w - 56.0f, 40.0f)
                .value(d.detail.title)
                .placeholder("任务标题…")
                .fontSize(t.sizes.fontSizeBody - 1.0f)
                .theme(themeTokens)
                .onChange(ro ? std::function<void(const std::string&)>{} : [&d](const std::string& text) { d.detail.title = text; })
                .build();

            ui_kit::drawDialogText(ui, "todo.dialog.detail.dlabel", w, "描述",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 126.0f + labelOffsetY, .width = 80.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            components::input(ui, "todo.dialog.detail.input.desc")
                .position(28.0f, 144.0f + labelOffsetY)
                .size(w - 56.0f, 144.0f)
                .value(d.detail.description)
                .placeholder("补充说明…（可多行）")
                .fontSize(t.sizes.fontSizeSmall)
                .multiline(true)
                .scrollbar(true)
                .theme(themeTokens)
                .onChange(ro ? std::function<void(const std::string&)>{} : [&d](const std::string& text) {
                    d.detail.description = text;
                })
                .build();

            ui_kit::drawDialogText(ui, "todo.dialog.detail.plabel", w, "优先级",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 300.0f + labelOffsetY, .width = 80.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            const Priority prios[4] = {Priority::None, Priority::Low,
                                       Priority::Medium, Priority::High};
            for (int i = 0; i < 4; ++i) {
                const bool active = (d.detail.priority == prios[i]);
                // 双列后按钮收窄：4×64 + 3×8 = 280 ≤ colW 284（100 宽四钮 424 会顶进右列）
                const float bw = 64.0f;
                // 按钮底色 = 它代表的优先级色（priorityButtonBg：未选中按强度表 tint、选中 0.65 深 tint，与卡底同源）。
                // 文字用 textPrimary（textSecondary 浅色档 < WCAG AA 4.5:1）；选中/未选中靠底色深浅 + 边框区分
                // （UAT2 收口把选中档提 0.65，与 High 未选中顶档 0.55 拉开、四档两态底色均可辨）；三态基色随米黄迁移走 wechat 次按钮梯度
                components::button(ui, "todo.dialog.detail.prio." + std::to_string(i))
                    .position(28.0f + static_cast<float>(i) * (bw + 8.0f), 320.0f + labelOffsetY)
                    .size(bw, 32.0f)
                    .text(priorityLabel(prios[i]))
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(priorityButtonBg(prios[i], t, active,
                                             t.colors.wechat.secondaryButtonBg),
                            priorityButtonBg(prios[i], t, active,
                                             t.colors.wechat.dividerSecondary),
                            priorityButtonBg(prios[i], t, active,
                                             t.colors.wechat.selectedBg))
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(active ? 2.0f : 1.0f,
                            active ? priorityColor(prios[i], t)
                                   : t.colors.borderDefault)
                    .onClick(ro ? std::function<void()>{} : [&d, i, prios] { d.detail.priority = prios[i]; })
                    .build();
            }

            // 截止日期：清除 + datepicker 滚轮选任意日期（与优先级同排，落右列 x=328）
            ui_kit::drawDialogText(ui, "todo.dialog.detail.dlabel2", w, "截止日期",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.x = kCol2X, .y = 300.0f + labelOffsetY,
                                    .width = 120.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            const bool hasDue = d.detail.dueAt.has_value();
            // 截止行总宽 176（56 + 8 + 112），收在 colW 284 内；pick 占大头让位「选择日期…」+日历图标
            components::button(ui, "todo.dialog.detail.due.clear")
                .position(kCol2X, 320.0f + labelOffsetY)
                .size(56.0f, 32.0f)
                .text(!hasDue ? "未设置" : "清除")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .colors(!hasDue ? t.colors.wechat.selectedBg
                                : t.colors.wechat.secondaryButtonBg,
                        t.colors.wechat.dividerSecondary, t.colors.wechat.selectedBg)
                // UAT3 W2-M2：未设置态绿字落 selectedBg #E5E5E5 仅 1.89:1（12px AA 需
                // 4.5:1）——文字档用 primaryText #067036（4.93:1）；绿边框是装饰不动
                .textColor(!hasDue ? t.colors.wechat.primaryText : t.colors.textSecondary)
                .radius(t.sizes.radius)
                .border(1.0f, !hasDue ? t.colors.wechat.primary
                                     : t.colors.wechat.dividerPrimary)
                .onClick(ro ? std::function<void()>{} : [&d] { d.detail.dueAt = std::nullopt; })
                .build();
            // fa-calendar
            components::button(ui, "todo.dialog.detail.due.pick")
                .position(kCol2X + 64.0f, 320.0f + labelOffsetY)
                .size(112.0f, 32.0f)
                .text(hasDue ? formatDue(d.detail.dueAt.value()) : "选择日期…")
                .icon(0xF133)
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .colors(hasDue ? t.colors.wechat.selectedBg
                               : t.colors.wechat.secondaryButtonBg,
                        t.colors.wechat.dividerSecondary, t.colors.wechat.selectedBg)
                // UAT3 W2-M2：已设日期绿字同落 selectedBg——文字档同上换 primaryText
                .textColor(hasDue ? t.colors.wechat.primaryText : t.colors.textSecondary)
                .radius(t.sizes.radius)
                .border(1.0f, hasDue ? t.colors.wechat.primary
                                     : t.colors.wechat.dividerPrimary)
                .onClick(ro ? std::function<void()>{} : [&d] { d.popups.open(Popup::DatePick); })
                .build();

            // 标签：chip × 删除 + 输入回车添加；保存时走 updateTask 的 JSON 序列化
            //（双列收行后整体上移 64：截止与优先级同排，标签区顶替原截止行位置）
            ui_kit::drawDialogText(ui, "todo.dialog.detail.taglabel", w, "标签",
                                   t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                   {.y = 364.0f + labelOffsetY, .width = 120.0f, .height = 16.0f,
                                    .lineHeight = 16.0f});
            {
                float chipX = 28.0f;
                for (size_t ti = 0; ti < d.detail.tags.size(); ++ti) {
                    const float tw = chipWidth(d.detail.tags[ti]);
                    // 单行不换行，放不下即截断（+N 省略）
                    if (chipX + tw + 22.0f > w - 28.0f) break;
                    const std::string chipId =
                        "todo.dialog.detail.tag." + std::to_string(ti);
                    drawChip(ui, chipId, chipX, 384.0f + labelOffsetY, tw, t, d.detail.tags[ti],
                             t.colors.info, t.colors.wechat.secondaryButtonBg);
                    components::button(ui, chipId + ".x")
                        // 删除按钮高 22 与 chip（kChipHeight）同高，y 对齐 chip 顶
                        .position(chipX + tw + 1.0f, 384.0f + labelOffsetY)
                        .size(20.0f, 22.0f)
                        .text("×")
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.wechat.secondaryButtonBg,
                                t.colors.wechat.dividerSecondary,
                                t.colors.wechat.selectedBg)
                        .textColor(t.colors.textSecondary)
                        .radius(11.0f)
                        .onClick(ro ? std::function<void()>{} : [&d, ti] {
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
                .position(28.0f, 410.0f + labelOffsetY)
                .size(w - 56.0f, 36.0f)
                .value(d.detail.tagInput)
                .placeholder(ro ? "（只读）" : "输入标签，回车添加…")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(themeTokens)
                .onChange(ro ? std::function<void(const std::string&)>{} : [&d](const std::string& text) { d.detail.tagInput = text; })
                .onEnter(ro ? std::function<void()>{} : [&d] { d.detail.addTag(); })
                // Esc=丢弃标签草稿（不关详情弹窗——整窗关断由关闭按钮负责）
                .onCancel(ro ? std::function<void()>{} : [&d] { d.detail.tagInput.clear(); })
                .build();

            const float btnY = h - 58.0f;
            if (ro) {
                // 只读模式：仅「关闭」按钮
                components::button(ui, "todo.dialog.detail.close")
                    .position(w - 28.0f - 160.0f, btnY)
                    .size(160.0f, 40.0f)
                    .text("关闭")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.wechat.primary, t.colors.wechat.primaryHover,
                            t.colors.wechat.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([&d] {
                        d.popups.close(Popup::Detail);
                        d.detail.discardArmed = false;
                    })
                    .build();
            } else {
                components::button(ui, "todo.dialog.detail.delete")
                    .position(28.0f, btnY)
                    .size(96.0f, 40.0f)
                    .text("删除")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.wechat.secondaryButtonBg,
                            t.colors.wechat.dividerSecondary,
                            t.colors.wechat.selectedBg)
                    .textColor(t.colors.error)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    // 与右键菜单删除同路径：先弹确认（Popup::Confirm），不关详情弹窗
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
                    .colors(t.colors.wechat.secondaryButtonBg,
                            t.colors.wechat.dividerSecondary,
                            t.colors.wechat.selectedBg)
                    .textColor(d.detail.completed ? t.colors.textSecondary : t.colors.success)
                    .iconColor(d.detail.completed ? t.colors.textSecondary
                                                  : t.colors.success)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([&d, &host] {
                        const int64_t taskId = d.detail.taskId;
                        const bool wasDone = d.detail.completed;
                        // 先落表单再切完成态（编辑改动不被丢弃）；保存失败保持弹窗
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
                    .colors(t.colors.wechat.primary, t.colors.wechat.primaryHover,
                            t.colors.wechat.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([&d, &host] {
                        // 表单组装 + 落库统一走 DetailDialog::save；保存失败保持弹窗
                        if (!d.detail.save(host)) return;
                        d.popups.close(Popup::Detail);
                        d.detail.discardArmed = false;
                    })
                    .build();
            }
    });
}

} // namespace module::todo::ui
