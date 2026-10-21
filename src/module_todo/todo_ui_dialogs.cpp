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
#include <components/dialog.h>
#include <components/input.h>
#include <components/scrollview.h>

#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace module::todo::ui {

namespace {

// —— UAT5 弹窗重设计共用小件（视觉基准 = 用户拍板的 demo
// task-new-dialog-redesign.html，尺寸/圆角/色值照抄；调用方仅限本 TU 的三个弹窗）——

// 弹性宽契约（用户口述定案）：demo 里的 px 是最小档；实际宽 = 按窗口比例放缩
// ——窗口的一半（clamp 下限 = demo 最小宽，无上限；窄于最小档不缩）。行/列/
// textarea 由此按 innerW / 双列各半 比例分宽。
float elasticDialogW(float screenW, float demoMinW) {
    return std::max(demoMinW, screenW * 0.5f);
}

// segment 选项钮（demo .seg .op）：未选 = pageBg 底 + dividerPrimary 边 + textPrimary
// 字；选中 = 主绿底 + 白字（主按钮同语言）。iconCode 0 = 无图标（截止档传
// 0xF133 fa-calendar——demo 的 📅，test_module_icons 钉住 .icon 节点）。
// 只读/禁用态 = 调用方传空 onClick
void drawSegOp(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
               const std::string& label, bool on, const core::theme::Tokens& t,
               std::function<void()> onClick, unsigned iconCode = 0) {
    auto btn = components::button(ui, id)
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(t.sizes.fontSizeSmall - 1.0f);   // demo .op 12px
    if (iconCode != 0) {
        btn.icon(iconCode);
    }
    btn
        .colors(on ? t.colors.wechat.primary : t.colors.wechat.pageBg,
                on ? t.colors.wechat.primaryHover : t.colors.wechat.secondaryButtonBg,
                on ? t.colors.wechat.primaryPressed : t.colors.wechat.selectedBg)
        .textColor(on ? t.colors.wechat.onPrimary : t.colors.wechat.textPrimary)
        .radius(8.0f)   // demo .op border-radius:8px
        .border(1.0f, on ? t.colors.wechat.primary : t.colors.wechat.dividerPrimary)
        .onClick(std::move(onClick))
        .build();
}

// 行标签（demo .lbl：12px 灰）
void drawFieldLabel(eui::Ui& ui, const std::string& id, float x, float y, float w,
                    const std::string& text, const core::theme::Tokens& t) {
    ui.text(id)
        .position(x, y)
        .size(w, 16.0f)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
        .color(t.colors.textTertiary)
        .build();
}

// 弹窗内标签 chip（demo .chip.pri：浅绿底 #E8F7EE + 深绿字 #067036 + 绿边；
// 右缘内嵌 × 删除钮——整枚是一枚胶囊）。宽 = 文字估宽 + × 槽（22 钮 + 2 缝）。
void drawDialogChip(eui::Ui& ui, const std::string& id, float x, float y,
                    const core::theme::Tokens& t, const std::string& text,
                    std::function<void()> onRemove) {
    const float textW = chipWidth(text);
    const float w = textW + 24.0f;
    const float h = 28.0f;
    const eui::Color bg = eui::mixColor(t.colors.wechat.cardBg,
                                        t.colors.wechat.primary, 0.12f);
    const eui::Color border = eui::mixColor(t.colors.wechat.cardBg,
                                            t.colors.wechat.primary, 0.24f);
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, h)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .border(1.0f, border)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(textW, h)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(h)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.wechat.primaryText)
        .build();
    components::button(ui, id + ".x")
        .position(x + textW + 2.0f, y + 3.0f)
        .size(22.0f, 22.0f)
        .text("×")
        .fontSize(t.sizes.fontSizeSmall)
        .colors(t.colors.wechat.cardBg, t.colors.wechat.secondaryButtonBg,
                t.colors.wechat.selectedBg)
        .textColor(t.colors.wechat.primaryText)
        .radius(11.0f)
        .onClick(std::move(onRemove))
        .build();
}

// 弹窗底部按钮对（demo .btn：高 34 / 圆角 8，右对齐自然宽；取消 = 浅灰描边钮，
// 确认 = 微信绿主钮）。h = 弹窗高
void drawDialogFooterButtons(eui::Ui& ui, const std::string& idBase, float w, float h,
                             const std::string& confirmLabel,
                             std::function<void()> onCancel,
                             std::function<void()> onConfirm,
                             const core::theme::Tokens& t) {
    const float bw = 96.0f;
    const float bh = 34.0f;
    const float y = h - 58.0f;
    components::button(ui, idBase + ".cancel")
        .position(w - ui_kit::kDialogBodyInsetX - bw * 2.0f - 8.0f, y)
        .size(bw, bh)
        .text("取消")
        .fontSize(t.sizes.fontSizeSmall)
        .colors(t.colors.wechat.pageBg, t.colors.wechat.secondaryButtonBg,
                t.colors.wechat.selectedBg)
        .textColor(t.colors.wechat.textPrimary)
        .radius(8.0f)
        .border(1.0f, t.colors.wechat.dividerPrimary)
        .onClick(std::move(onCancel))
        .build();
    components::button(ui, idBase + ".confirm")
        .position(w - ui_kit::kDialogBodyInsetX - bw, y)
        .size(bw, bh)
        .text(confirmLabel)
        .fontSize(t.sizes.fontSizeSmall)
        .colors(t.colors.wechat.primary, t.colors.wechat.primaryHover,
                t.colors.wechat.primaryPressed)
        .textColor(t.colors.wechat.onPrimary)
        .radius(8.0f)
        .onClick(std::move(onConfirm))
        .build();
}

// 优先级 segment 短文案（demo：无/低/中/高——不带看板卡 priorityLabel 的 ● 前缀）
const char* prioritySegLabel(Priority p) {
    switch (p) {
        case Priority::None: return "无";
        case Priority::Low: return "低";
        case Priority::Medium: return "中";
        case Priority::High: return "高";
    }
    return "?";
}

// 今天/明天的本地日历日 epoch（供截止快选；day 溢出由 mktime 归一）
int64_t dayShiftEpoch(int dayOffset) {
    std::tm tmv{};
    const std::time_t now = std::time(nullptr);
    core::time::localTime(now, &tmv);
    return ymdToEpoch(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday + dayOffset);
}

} // namespace

// 提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；成功 → 关窗。
// 方案 A 富创建：描述/优先级/截止随标题一次落库（NewTaskAttributes），标签建成
// 后由模块侧补一次 update（见 DialogHost::createTask 注释）。
void NewTaskDialog::submit(PopupStack& popups, const DialogHost& host) {
    const int64_t col = columnId;
    const std::string text = title;
    NewTaskAttributes attrs;
    if (!description.empty()) attrs.description = description;
    attrs.priority = priority;
    attrs.dueAt = dueAt;
    const std::string err = host.createTask(col, text, attrs, tags);
    if (!err.empty()) {
        error = err;
        return;
    }
    popups.close(Popup::NewTask);
}

// 方案 A 富创建（UAT5 重设计，demo 最小档 640×470）：
// 标题大输入（回车直接创建）→ 描述可选 → 优先级|截止 双列 segment → 标签行 →
// 底部 hint + 取消|创建。元素 id 沿用旧契约（input/confirm/cancel/error），新字段
// 用 .desc / .prio.N / .due.N / .tag.* 后缀。
void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t, DialogSet& d,
                       const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const float w = elasticDialogW(screen.width, 640.0f);
    const float h = 470.0f;
    const float innerW = w - ui_kit::kDialogBodyInsetX;   // 弹性：随窗口放缩
    const float x0 = ui_kit::kDialogInsetX;

    // 关闭态也常驻构建（EUI 内建 0.16s 收起过渡依赖节点存在；test_ui_modern 钉住）
    components::dialog(ui, "todo.dialog.newtask")
        .open(d.popups.isOpen(Popup::NewTask))
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .zIndex(d.popups.zOf(Popup::NewTask))
        .onOpenChange([&d](bool v) { d.popups.setOpen(Popup::NewTask, v); })
        .content([&] {
            ui_kit::drawDialogTitle(ui, "todo.dialog.newtask.title", w, "新建任务",
                                    t.sizes.fontSizeBody, t.colors.textPrimary,
                                    {.y = 22.0f, .height = 28.0f, .lineHeight = 28.0f});

            std::string colName = "?";
            for (const auto& c : *host.columns) {
                if (c.id == d.newTask.columnId) colName = c.title;
            }
            ui_kit::drawDialogText(ui, "todo.dialog.newtask.col", w,
                                   "将创建在「" + colName + "」列",
                                   t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                                   {.y = 52.0f, .height = 16.0f, .lineHeight = 16.0f});

            // 标题大输入（demo .titleinput）：回车直接创建
            components::input(ui, "todo.dialog.newtask.input")
                .position(x0, 74.0f)
                .size(innerW, 44.0f)
                .value(d.newTask.title)
                .placeholder("任务标题，回车直接创建")
                .fontSize(t.sizes.fontSizeBody + 1.0f)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) {
                    d.newTask.title = text;
                    d.newTask.error.clear();  // 错误随下一次输入清除
                })
                .onEnter([&d, &host] { d.newTask.submit(d.popups, host); })
                .onCancel([&d] { d.popups.close(Popup::NewTask); })
                .build();

            // 描述（可选）textarea
            drawFieldLabel(ui, "todo.dialog.newtask.dlabel", x0, 128.0f, 120.0f,
                           "描述（可选）", t);
            components::input(ui, "todo.dialog.newtask.desc")
                .position(x0, 148.0f)
                .size(innerW, 52.0f)
                .value(d.newTask.description)
                .placeholder("补充说明…")
                .fontSize(t.sizes.fontSizeSmall)
                .multiline(true)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) { d.newTask.description = text; })
                .build();

            // 优先级 | 截止日期 双列（列宽各半按弹窗宽弹性分）
            const float colGap = 16.0f;
            const float colW = (innerW - colGap) * 0.5f;
            const float col2X = x0 + colW + colGap;
            drawFieldLabel(ui, "todo.dialog.newtask.plabel", x0, 212.0f, 120.0f,
                           "优先级", t);
            drawFieldLabel(ui, "todo.dialog.newtask.duelabel", col2X, 212.0f, 120.0f,
                           "截止日期", t);

            // 优先级 segment 四选（选中 = 绿底白字；「无」即不设优先级）
            const Priority prios[4] = {Priority::None, Priority::Low,
                                       Priority::Medium, Priority::High};
            {
                constexpr float kGap = 6.0f;   // demo .seg gap:6px
                const float opW = (colW - kGap * 3.0f) / 4.0f;
                for (int i = 0; i < 4; ++i) {
                    drawSegOp(ui, "todo.dialog.newtask.prio." + std::to_string(i),
                              x0 + static_cast<float>(i) * (opW + kGap), 232.0f, opW,
                              32.0f, prioritySegLabel(prios[i]),
                              d.newTask.priority == prios[i], t,
                              [&d, p = prios[i]] { d.newTask.priority = p; });
                }
            }
            // 截止 segment 三选：今天/明天/选择…（选择… = datepicker；再点选中档 = 取消）
            {
                constexpr float kGap = 6.0f;
                const float opW = (colW - kGap * 2.0f) / 3.0f;
                const bool hasDue = d.newTask.dueAt.has_value();
                drawSegOp(ui, "todo.dialog.newtask.due.0",
                          col2X, 232.0f, opW, 32.0f, "今天",
                          hasDue && d.newTask.dueChoice == DueChoice::Today, t,
                          [&d] {
                              if (d.newTask.dueAt.has_value() &&
                                  d.newTask.dueChoice == DueChoice::Today) {
                                  d.newTask.dueAt = std::nullopt;   // 再点 = 取消
                              } else {
                                  d.newTask.dueAt = dayShiftEpoch(0);
                                  d.newTask.dueChoice = DueChoice::Today;
                              }
                          },
                          0xF133);   // fa-calendar（demo 📅 今天）
                drawSegOp(ui, "todo.dialog.newtask.due.1",
                          col2X + opW + kGap, 232.0f, opW, 32.0f, "明天",
                          hasDue && d.newTask.dueChoice == DueChoice::Tomorrow, t,
                          [&d] {
                              if (d.newTask.dueAt.has_value() &&
                                  d.newTask.dueChoice == DueChoice::Tomorrow) {
                                  d.newTask.dueAt = std::nullopt;
                              } else {
                                  d.newTask.dueAt = dayShiftEpoch(1);
                                  d.newTask.dueChoice = DueChoice::Tomorrow;
                              }
                          });
                const std::string pickLabel =
                    hasDue && d.newTask.dueChoice == DueChoice::Custom
                        ? formatDue(d.newTask.dueAt.value())
                        : "选择…";
                drawSegOp(ui, "todo.dialog.newtask.due.2",
                          col2X + (opW + kGap) * 2.0f, 232.0f, opW, 32.0f, pickLabel,
                          hasDue && d.newTask.dueChoice == DueChoice::Custom, t,
                          [&d] {
                              d.datePickOwner = DatePickOwner::NewTask;
                              d.popups.open(Popup::DatePick);
                          });
            }

            // 标签：已选 chips（单行，放不下截断）+ 添加输入行
            drawFieldLabel(ui, "todo.dialog.newtask.taglabel", x0, 280.0f, 120.0f,
                           "标签", t);
            float chipX = x0;
            for (size_t ti = 0; ti < d.newTask.tags.size(); ++ti) {
                const float cw = chipWidth(d.newTask.tags[ti]) + 24.0f;
                if (chipX + cw > w - x0) break;   // 单行不换行，放不下即截断
                const std::string cid =
                    "todo.dialog.newtask.tag." + std::to_string(ti);
                drawDialogChip(ui, cid, chipX, 300.0f, t, d.newTask.tags[ti],
                               [&d, ti] {
                                   if (ti < d.newTask.tags.size()) {
                                       d.newTask.tags.erase(d.newTask.tags.begin() +
                                                            static_cast<long>(ti));
                                   }
                               });
                chipX += cw + 8.0f;
            }
            const float tagInputY =
                d.newTask.tags.empty() ? 300.0f : 336.0f;
            components::input(ui, "todo.dialog.newtask.tag.input")
                .position(x0, tagInputY)
                .size(innerW, 34.0f)
                .value(d.newTask.tagInput)
                .placeholder("+ 添加标签，回车添加")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(themeTokens)
                .onChange([&d](const std::string& text) { d.newTask.tagInput = text; })
                .onEnter([&d] { d.newTask.addTag(); })
                .onCancel([&d] { d.newTask.tagInput.clear(); })
                .build();

            // 内嵌红字错误行（校验/创建失败就地可见，弹窗不关）
            ui_kit::drawDialogText(ui, "todo.dialog.newtask.error", w, d.newTask.error,
                                   t.sizes.fontSizeSmall, t.colors.error,
                                   {.y = 380.0f, .height = 16.0f, .lineHeight = 16.0f,
                                    .skipIfEmpty = true});

            // 底部：hint 左 + 取消|创建 右（demo .dlgf）
            ui_kit::drawDialogText(
                ui, "todo.dialog.newtask.hint", w, "创建后可在详情里继续编辑",
                t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                {.y = h - 52.0f, .height = 16.0f, .lineHeight = 16.0f});
            drawDialogFooterButtons(ui, "todo.dialog.newtask", w, h, "创建",
                                    [&d] { d.popups.close(Popup::NewTask); },
                                    [&d, &host] { d.newTask.submit(d.popups, host); },
                                    t);
        })
        .build();
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

// 截止日期选择（datepicker 滚轮，可选任意日期）。锚点/写回按 datePickOwner 路由
//（详情「选择日期…」与新建「选择…」两个入口）。
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
    // 锚点与确定写回按归属弹窗路由（详情/新建可同时在栈上，见 DatePickOwner）
    const bool forNewTask = d.datePickOwner == DatePickOwner::NewTask;
    const std::optional<int64_t>& ownerDue =
        forNewTask ? d.newTask.dueAt : d.detail.dueAt;
    int year = 0, month = 0, day = 0;
    if (!epochToYmd(ownerDue.value_or(0), year, month, day)) {
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
        .onChange([&d, forNewTask](int yy, int mm, int dd) {
            const int64_t epoch = ymdToEpoch(yy, mm, dd);
            if (forNewTask) {
                d.newTask.dueAt = epoch;
                d.newTask.dueChoice = DueChoice::Custom;
            } else {
                d.detail.dueAt = epoch;
            }
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

// 任务详情弹窗重设计（UAT5，demo task-new-dialog-redesign.html 详情面板）：
// 删除/标记完成上移头部（危险操作与保存分离）；标题/描述直接可编辑；
// 优先级|截止 segment 双列（与新建弹窗同一语言）；标签 = 可删 chips + 添加输入；
// 底部只留 取消|保存。元素 id 沿用旧契约（input.title / prio.N / due.pick / due.clear /
// tag.N / delete / toggle / save / close——无头用例按 id 定位与触发）。
//
// UAT6 框底钉定（真机 1000×640 偶发「取消/保存出框」根治）：根因是
// components::dialog 内建把 panel 尺寸 clamp 到「窗口 − overlay(48)」，而内容子节点
// 此前按未 clamp 的请求尺寸（高 600 / 弹性宽）定位，且 panel stack 不裁剪——窗口
// 逻辑高被 DPI 缩放/小窗压小后实际 frame < 600，按 600 定位的按钮落在 frame 外。
// 修法：① w/h 在此先按组件同式 clamp，子布局全部按 clamp 后的「实际 frame 尺寸」走；
// ② 取消/保存钉 frame 底固定偏移（frame 底 − 18 − 按钮高），与内容高度彻底解耦；
// ③ 标题/描述/优先级|截止/标签装进限高滚动容器（frame 高 − 头部 − 按钮带 − 缝），
//    chips 多行换行（替掉旧单行截断）、任意内容形态超高都由本区滚动兜底。
void drawDetailDialog(eui::Ui& ui, const eui::Screen& screen,
                      const core::theme::Tokens& t, DialogSet& d,
                      const DialogHost& host) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    // 实际 frame 尺寸 = min(期望, 窗口 − overlay 48)：与 components::dialog 内建
    // clamp 同式同源（metrics.spacing.overlay）；组件里再 clamp 一次结果不变。
    // 宽保持弹性契约 max(640 最小档, 窗口/2)，再夹窗口可用宽。
    const float w = std::max(0.0f, std::min(elasticDialogW(screen.width, 640.0f),
                                           screen.width -
                                               themeTokens.metrics.spacing.overlay));
    const float h = std::max(0.0f, std::min(600.0f,
                                           screen.height -
                                               themeTokens.metrics.spacing.overlay));
    const float x0 = ui_kit::kDialogInsetX;
    // 底部按钮带：贴 frame 底固定偏移（frame 底 − 18 − 按钮高），任何内容高度与
    // 窗口高度下都不越框（UAT6 前随请求高 600 定位，frame 被 clamp 矮后被顶出框）
    constexpr float kFooterLift = 18.0f;   // 按钮底缘 → frame 底
    constexpr float kFooterBtnH = 34.0f;
    constexpr float kFooterBtnW = 96.0f;
    constexpr float kContentGapAboveFooter = 12.0f;   // 内容滚动区底 → 按钮带顶
    const float btnY = h - kFooterLift - kFooterBtnH;

    // 关闭态也常驻构建（收起过渡依赖节点存在）
    components::dialog(ui, "todo.dialog.detail")
        .open(d.popups.isOpen(Popup::Detail))
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .zIndex(d.popups.zOf(Popup::Detail))
        // backdrop 点击（dialog 唯一外部关闭通道）→ 与 Esc 兜底共用 requestClose
        .onOpenChange([&d, &host](bool v) {
            if (!v) d.detail.requestClose(d.popups, host);
        })
        .content([&] {
            const bool ro = d.detail.readOnly;
            // 只读（归档）行整体下移一档给「归档时间」行让位
            const float offY = ro ? 18.0f : 0.0f;

            ui_kit::drawDialogTitle(ui, "todo.dialog.detail.title", w,
                                    ro ? "任务详情（只读）" : "任务详情",
                                    t.sizes.fontSizeBody, t.colors.textPrimary,
                                    {.y = 22.0f, .height = 28.0f, .lineHeight = 28.0f});

            // 头部右缘动作 chips（demo：🗑 删除 灰 / ✔ 标记完成 绿）——从底部按钮区上移，
            // 危险操作不再与取消/保存挤一行；只读态不渲染
            if (!ro) {
                const float chipY = 22.0f;
                const float toggleW = 104.0f;
                const float deleteW = 76.0f;
                components::button(ui, "todo.dialog.detail.delete")
                    .position(w - ui_kit::kDialogBodyInsetX - toggleW - 8.0f - deleteW,
                              chipY)
                    .size(deleteW, 28.0f)
                    .icon(0xF2ED)   // fa-trash
                    .text("删除")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(t.colors.wechat.pageBg, t.colors.wechat.secondaryButtonBg,
                            t.colors.wechat.selectedBg)
                    .textColor(t.colors.wechat.textSecondary)
                    .iconColor(t.colors.wechat.textSecondary)
                    .radius(14.0f)   // 胶囊（demo .chip border-radius:99px）
                    .border(1.0f, t.colors.wechat.dividerPrimary)
                    // 与右键菜单删除同路径：先弹确认（Popup::Confirm），不关详情弹窗
                    .onClick([&d] {
                        d.confirm.openForTask(d.detail.taskId, d.detail.title);
                        d.popups.open(Popup::Confirm);
                    })
                    .build();
                components::button(ui, "todo.dialog.detail.toggle")
                    .position(w - ui_kit::kDialogBodyInsetX - toggleW, chipY)
                    .size(toggleW, 28.0f)
                    .icon(d.detail.completed ? 0xF021 : 0xF00C)  // fa-arrows-rotate / fa-check
                    .text(d.detail.completed ? "取消完成" : "标记完成")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    // 未完成 = demo .chip.pri（浅绿底绿字）；已完成 = 中性灰（弱化）
                    .colors(d.detail.completed
                                ? t.colors.wechat.secondaryButtonBg
                                : eui::mixColor(t.colors.wechat.cardBg,
                                                t.colors.wechat.primary, 0.12f),
                            t.colors.wechat.secondaryButtonBg,
                            t.colors.wechat.selectedBg)
                    .textColor(d.detail.completed ? t.colors.wechat.textSecondary
                                                   : t.colors.wechat.primaryText)
                    .iconColor(d.detail.completed ? t.colors.wechat.textSecondary
                                                   : t.colors.wechat.primaryText)
                    .radius(14.0f)
                    .border(1.0f, d.detail.completed
                                      ? t.colors.wechat.dividerPrimary
                                      : eui::mixColor(t.colors.wechat.cardBg,
                                                      t.colors.wechat.primary, 0.24f))
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
            }

            // 归档时间戳（归档列任务显示）；非归档列不占位
            if (ro) {
                const Task* orig = findTask(*host.tasks, d.detail.taskId);
                if (orig != nullptr && orig->archivedAt.has_value()) {
                    const std::string archStr =
                        "归档时间: " + formatArchivedAt(orig->archivedAt.value());
                    ui_kit::drawDialogText(
                        ui, "todo.dialog.detail.archived", w, archStr,
                        t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                        {.y = 52.0f, .height = 18.0f, .lineHeight = 18.0f});
                }
            }

            // —— 内容区（UAT6 限高滚动容器）——
            // 视口 = frame 高 − 头部(76+offY) − 按钮带 − 12 缝；标题/描述/优先级|截止/
            // 标签全部装进本区（board 列内滚动同款 scrollView 先例），chips 换行、
            // 任意形态内容超高都由本区滚动兜底，按钮带不受内容高度影响。
            const float contentTop = 76.0f + offY;
            const float viewportH =
                std::max(0.0f, btnY - kContentGapAboveFooter - contentTop);
            // 测量缓存键：标签数 + 逐标签字节数（chips 换行数随两者变 → 内容高变）
            std::string scrollKey =
                "todo.dialog.detail.scroll.t" +
                std::to_string(d.detail.tags.size());
            for (const auto& tg : d.detail.tags) {
                scrollKey += "." + std::to_string(tg.size());
            }
            components::scrollView(ui, "todo.dialog.detail.scroll")
                .position(0.0f, contentTop)
                .size(w, viewportH)
                .theme(themeTokens)
                .contentKey(scrollKey)
                .content([&](eui::Ui& inner, float cw, float) {
                    // 内容有效宽（= 旧 innerW 契约：宽 − 双侧 56；出滚动条时 scrollView
                    // 已把 cw 扣减滚动条宽，长内容自动让位）
                    const float sw = std::max(0.0f, cw - ui_kit::kDialogBodyInsetX);
                    const float colGap = 16.0f;
                    const float colW = (sw - colGap) * 0.5f;
                    const float col2X = x0 + colW + colGap;

                    // 流式 wrap stack（board 卡片同款先例）：scrollView 内容列里
                    // .position 会被流式布局接管，块内子元素才用绝对定位；块高含到
                    // 下一块的缝 → 不滚动时各元素绝对 y 与旧版逐像素一致
                    // （76/130/150/234/254/298/318|354）。
                    inner.stack("todo.dialog.detail.scroll.title.w")
                        .size(cw, 54.0f)   // 标题输入 44 + 到描述标签 10 缝
                        .content([&] {
                            // 标题大输入（直接可编辑；demo .titleinput）
                            components::input(inner, "todo.dialog.detail.input.title")
                                .position(x0, 0.0f)
                                .size(sw, 44.0f)
                                .value(d.detail.title)
                                .placeholder("任务标题…")
                                .fontSize(t.sizes.fontSizeBody + 1.0f)
                                .theme(themeTokens)
                                .onChange(ro ? std::function<void(const std::string&)>{}
                                             : [&d](const std::string& text) {
                                                   d.detail.title = text;
                                               })
                                .build();
                        })
                        .build();

                    inner.stack("todo.dialog.detail.scroll.desc.w")
                        .size(cw, 104.0f)   // 标签 16 + 4 缝 + textarea 72 + 12 缝
                        .content([&] {
                            // 描述 textarea（直接可编辑）
                            drawFieldLabel(inner, "todo.dialog.detail.dlabel", x0,
                                           0.0f, 120.0f, "描述", t);
                            components::input(inner, "todo.dialog.detail.input.desc")
                                .position(x0, 20.0f)
                                .size(sw, 72.0f)
                                .value(d.detail.description)
                                .placeholder("补充说明…（可多行）")
                                .fontSize(t.sizes.fontSizeSmall)
                                .multiline(true)
                                .scrollbar(true)
                                .theme(themeTokens)
                                .onChange(ro ? std::function<void(const std::string&)>{}
                                             : [&d](const std::string& text) {
                                                   d.detail.description = text;
                                               })
                                .build();
                        })
                        .build();

                    inner.stack("todo.dialog.detail.scroll.prio.w")
                        .size(cw, 64.0f)   // 双列标签 16 + 4 缝 + segment 32 + 12 缝
                        .content([&] {
                            // 优先级 | 截止日期 双列 segment（列宽各半，随弹窗宽弹性分）
                            drawFieldLabel(inner, "todo.dialog.detail.plabel", x0,
                                           0.0f, 120.0f, "优先级", t);
                            drawFieldLabel(inner, "todo.dialog.detail.dlabel2", col2X,
                                           0.0f, 120.0f, "截止日期", t);

                            // 优先级 segment 四选（选中 = 绿底白字，与新建弹窗同语言）
                            const Priority prios[4] = {Priority::None, Priority::Low,
                                                       Priority::Medium, Priority::High};
                            {
                                constexpr float kGap = 6.0f;
                                const float opW = (colW - kGap * 3.0f) / 4.0f;
                                for (int i = 0; i < 4; ++i) {
                                    drawSegOp(inner,
                                              "todo.dialog.detail.prio." +
                                                  std::to_string(i),
                                              x0 + static_cast<float>(i) * (opW + kGap),
                                              20.0f, opW, 32.0f,
                                              prioritySegLabel(prios[i]),
                                              d.detail.priority == prios[i], t,
                                              ro ? std::function<void()>{}
                                                 : [&d, p = prios[i]] {
                                                       d.detail.priority = p;
                                                   });
                                }
                            }
                            // 截止 segment：有 due = [📅 MM-DD（选中，点开换）| 清除]；
                            // 无 due = [选择日期…]
                            const bool hasDue = d.detail.dueAt.has_value();
                            {
                                constexpr float kGap = 6.0f;
                                const int nOps = hasDue ? 2 : 1;
                                const float opW =
                                    (colW - kGap * static_cast<float>(nOps - 1)) /
                                    static_cast<float>(nOps);
                                const std::string pickLabel =
                                    (hasDue ? formatDue(d.detail.dueAt.value())
                                            : "选择日期…");
                                drawSegOp(inner, "todo.dialog.detail.due.pick", col2X,
                                          20.0f, opW, 32.0f, pickLabel, hasDue, t,
                                          ro ? std::function<void()>{}
                                             : [&d] {
                                                   d.datePickOwner =
                                                       DatePickOwner::Detail;
                                                   d.popups.open(Popup::DatePick);
                                               },
                                          0xF133);   // fa-calendar（demo 📅；
                                                     // module_icons 钉 .icon）
                                if (hasDue) {
                                    drawSegOp(inner, "todo.dialog.detail.due.clear",
                                              col2X + opW + kGap, 20.0f, opW, 32.0f,
                                              "清除", false, t,
                                              ro ? std::function<void()>{}
                                                 : [&d] { d.detail.dueAt = std::nullopt; });
                                }
                            }
                        })
                        .build();

                    inner.stack("todo.dialog.detail.scroll.taglabel.w")
                        .size(cw, 20.0f)   // 标签行标签 16 + 4 缝
                        .content([&] {
                            drawFieldLabel(inner, "todo.dialog.detail.taglabel", x0,
                                           0.0f, 120.0f, "标签", t);
                        })
                        .build();

                    // 标签 chips：多行换行（UAT6 前是单行截断 break——放不下的整尾
                    // 不渲染）；单枚超行宽仍跳过，保留旧「不横向溢出」语义。
                    // y<0 = 该标签被跳过；其余 (x, 行内 y)，行高 28 + 行缝 8。
                    std::vector<std::pair<float, float>> chipXY(d.detail.tags.size(),
                                                                {-1.0f, -1.0f});
                    float chipX = x0;
                    float chipRowY = 0.0f;
                    for (size_t ti = 0; ti < d.detail.tags.size(); ++ti) {
                        const float cw2 = chipWidth(d.detail.tags[ti]) + 24.0f;
                        if (cw2 > sw) continue;
                        if (chipX + cw2 > x0 + sw) {   // 本行放不下 → 换行
                            chipX = x0;
                            chipRowY += 28.0f + 8.0f;
                        }
                        chipXY[ti] = {chipX, chipRowY};
                        chipX += cw2 + 8.0f;
                    }
                    float chipsBottom = 0.0f;
                    size_t chipCount = 0;
                    for (const auto& pos : chipXY) {
                        if (pos.second >= 0.0f) {
                            ++chipCount;
                            chipsBottom = std::max(chipsBottom, pos.second + 28.0f);
                        }
                    }
                    if (chipCount > 0) {
                        // chips 块高 = 末行底 + 到添加输入行的 8 缝（旧 318→354 同律）
                        inner.stack("todo.dialog.detail.scroll.chips.w")
                            .size(cw, chipsBottom + 8.0f)
                            .content([&] {
                                for (size_t ti = 0; ti < d.detail.tags.size(); ++ti) {
                                    if (chipXY[ti].second < 0.0f) continue;
                                    const std::string cid =
                                        "todo.dialog.detail.tag." + std::to_string(ti);
                                    drawDialogChip(inner, cid, chipXY[ti].first,
                                                   chipXY[ti].second, t,
                                                   d.detail.tags[ti],
                                                   ro ? std::function<void()>{}
                                                      : [&d, ti] {
                                                            if (ti <
                                                                d.detail.tags.size()) {
                                                                d.detail.tags.erase(
                                                                    d.detail.tags
                                                                        .begin() +
                                                                    static_cast<long>(
                                                                        ti));
                                                            }
                                                        });
                                }
                            })
                            .build();
                    }

                    inner.stack("todo.dialog.detail.scroll.taginput.w")
                        .size(cw, 34.0f)
                        .content([&] {
                            components::input(inner, "todo.dialog.detail.tag.input")
                                .position(x0, 0.0f)
                                .size(sw, 34.0f)
                                .value(d.detail.tagInput)
                                .placeholder(ro ? "（只读）" : "+ 添加标签，回车添加")
                                .fontSize(t.sizes.fontSizeSmall)
                                .theme(themeTokens)
                                .onChange(ro ? std::function<void(const std::string&)>{}
                                             : [&d](const std::string& text) {
                                                   d.detail.tagInput = text;
                                               })
                                .onEnter(ro ? std::function<void()>{}
                                            : [&d] { d.detail.addTag(); })
                                // Esc=丢弃标签草稿（不关详情弹窗——整窗关断由取消
                                // 按钮/backdrop 负责）
                                .onCancel(ro ? std::function<void()>{}
                                              : [&d] { d.detail.tagInput.clear(); })
                                .build();
                        })
                        .build();
                })
                .build();

            // 底部：只读 = 关闭；编辑 = 取消|保存 右对齐（demo .dlgf justify-end）。
            // 钉 frame 底（btnY 预计算 = frame 高 − 18 − 按钮高），内容再高/窗口再矮
            // 也不越框
            if (ro) {
                components::button(ui, "todo.dialog.detail.close")
                    .position(w - ui_kit::kDialogBodyInsetX - kFooterBtnW, btnY)
                    .size(kFooterBtnW, kFooterBtnH)
                    .text("关闭")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.wechat.primary, t.colors.wechat.primaryHover,
                            t.colors.wechat.primaryPressed)
                    .textColor(t.colors.wechat.onPrimary)
                    .radius(8.0f)
                    .onClick([&d] {
                        d.popups.close(Popup::Detail);
                        d.detail.discardArmed = false;
                    })
                    .build();
            } else {
                components::button(ui, "todo.dialog.detail.cancel")
                    .position(w - ui_kit::kDialogBodyInsetX - kFooterBtnW * 2.0f - 8.0f,
                              btnY)
                    .size(kFooterBtnW, kFooterBtnH)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.wechat.pageBg, t.colors.wechat.secondaryButtonBg,
                            t.colors.wechat.selectedBg)
                    .textColor(t.colors.wechat.textPrimary)
                    .radius(8.0f)
                    .border(1.0f, t.colors.wechat.dividerPrimary)
                    // 与 backdrop/Esc 共用未存改动守卫（先警告一次，再点放弃）
                    .onClick([&d, &host] { d.detail.requestClose(d.popups, host); })
                    .build();
                components::button(ui, "todo.dialog.detail.save")
                    .position(w - ui_kit::kDialogBodyInsetX - kFooterBtnW, btnY)
                    .size(kFooterBtnW, kFooterBtnH)
                    .text("保存")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.wechat.primary, t.colors.wechat.primaryHover,
                            t.colors.wechat.primaryPressed)
                    .textColor(t.colors.wechat.onPrimary)
                    .radius(8.0f)
                    .onClick([&d, &host] {
                        // 表单组装 + 落库统一走 DetailDialog::save；保存失败保持弹窗
                        if (!d.detail.save(host)) return;
                        d.popups.close(Popup::Detail);
                        d.detail.discardArmed = false;
                    })
                    .build();
            }
        })
        .build();
}

} // namespace module::todo::ui
