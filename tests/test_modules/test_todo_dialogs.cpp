// tests/test_modules/test_todo_dialogs.cpp
// 并行批 B 弹窗域守卫（B-2/B-5/B-6/C-10/C-11）。直接驱动 todo_ui_dialogs 的
// 自由绘制函数（不经模块），弹窗状态显式喂 DialogSet / DialogHost，逐元素按 id 定位。
//
// 覆盖：
//   B-2  逾期弹窗不再 8 行截断：>8 条时高封顶 552、行区滚动、第 9 条起仍在 UI 树；
//   C-10 逾期天数三位数不溢出（天数列加宽 + Ellipsis 兜底）；
//   C-11 关联文档选择器列表高按壳夹后内容区算（小窗 640 不伸进底呼吸隙）；
//   B-5  右键菜单按状态过滤：归档卡不显示上移/下移/编辑（改「查看」），
//        显示序→模块契约索引经映射表回传；
//   B-6  新建弹窗草稿守卫：遮罩关闭首次只警告、二次才关；取消=显式放弃；
//        详情只读输入框 readOnly（粘贴/输入不改内部文本）。
#include "doctest.h"
#include "test_support.h"

#include "core/theme.h"
#include "module_todo/todo_dialogs.h"
#include "ui_kit/dialog_shell.h"

#include <eui/dsl.h>

#include <ctime>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

// 一帧：begin → draw → end → layout（无模块，纯弹窗层）
void compose(eui::Ui& ui, const eui::Screen& screen,
             const std::function<void()>& draw) {
    ui.begin("todo_dialogs_test");
    draw();
    ui.end();
    ui.layout(screen.width, screen.height);
}

eui::Screen makeScreen(float w = 1600.0f, float h = 1000.0f) {
    eui::Screen s;
    s.width = w;
    s.height = h;
    return s;
}

module::todo::Column col(int64_t id, std::string title, std::string kind) {
    return module::todo::Column{id, std::move(title), static_cast<int>(id),
                                std::nullopt, std::move(kind)};
}

module::todo::Task overdueTask(int64_t id, int64_t columnId, std::string title,
                               int64_t dueAt) {
    module::todo::Task t;
    t.id = id;
    t.columnId = columnId;
    t.title = std::move(title);
    t.dueAt = dueAt;
    t.position = static_cast<int>(id);
    return t;
}

module::todo::ui::DialogHost makeHost(
    const std::vector<module::todo::Column>& columns,
    const std::vector<module::todo::Task>& tasks,
    std::function<void(const module::todo::Task&)> openDetail = {}) {
    module::todo::ui::DialogHost host;
    host.columns = &columns;
    host.tasks = &tasks;
    host.openDetail = std::move(openDetail);
    return host;
}

// 逾期弹窗标题元素文本（label.0 之类菜单项取 text 用）
std::string menuLabel(eui::Ui& ui, int index) {
    const auto* el =
        ui.find("todo.ctx.level.0.label." + std::to_string(index));
    return el != nullptr ? el->text : std::string();
}

} // namespace

// B-2（重要）：12 条逾期 > 旧 8 行截断。高封顶 552，行区可滚动；前 8 条与第 9
// 条（下标 11，旧实现根本不存在）的 row/title/col/due/days 都必须在 UI 树里。
REGISTER_TEST(todo_dialogs_overdue_scrolls_past_eight,
              "todo_dialogs: 逾期弹窗 >8 条不截断、高封顶 552、第 9 条起行元素可达", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen();

    const std::vector<module::todo::Column> columns = {col(1, "待办", "todo")};
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::vector<module::todo::Task> tasks;
    for (int i = 0; i < 12; ++i) {
        tasks.push_back(overdueTask(i + 1, 1, "逾期任务" + std::to_string(i),
                                    now - 86400 * 2));
    }

    bool opened = false;
    auto host = makeHost(columns, tasks,
                         [&opened](const module::todo::Task&) { opened = true; });

    module::todo::ui::DialogSet d;
    d.overdue.tasks = tasks;
    d.popups.open(module::todo::ui::Popup::Overdue);

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawOverdueDialog(ui, screen, t, d, host);
    });

    const auto* panel = ui.find("todo.dialog.overdue.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK(panel->frame.height <= 552.0f + 0.5f);
    DOCTEST_CHECK(panel->frame.height > 500.0f);   // 12 行确实把弹窗撑到封顶

    const auto* scroll = ui.find("todo.dialog.overdue.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_CHECK(scroll->frame.height > 300.0f);
    DOCTEST_CHECK(scroll->frame.height < 372.0f);  // 552−182 行区封顶

    for (int i : {0, 7, 11}) {
        const std::string base =
            "todo.dialog.overdue.row." + std::to_string(i);
        DOCTEST_REQUIRE(ui.find(base) != nullptr);
        DOCTEST_REQUIRE(ui.find(base + ".title") != nullptr);
        DOCTEST_REQUIRE(ui.find(base + ".col") != nullptr);
        DOCTEST_REQUIRE(ui.find(base + ".due") != nullptr);
        DOCTEST_REQUIRE(ui.find(base + ".days") != nullptr);
        DOCTEST_CHECK_EQ(ui.find(base + ".title")->text,
                         std::string("逾期任务") + std::to_string(i));
    }
    const auto* row11 = ui.find("todo.dialog.overdue.row.11");
    DOCTEST_REQUIRE(row11 != nullptr);
    DOCTEST_REQUIRE(row11->onClick != nullptr);
    row11->onClick();
    DOCTEST_CHECK(opened);
    DOCTEST_CHECK(!d.popups.isOpen(module::todo::ui::Popup::Overdue));
    return true;
})

// C-10：三位数逾期天数不压出列（天数列 72 宽 + maxWidth + Ellipsis 兜底）
REGISTER_TEST(todo_dialogs_overdue_days_column_width,
              "todo_dialogs: 逾期三位数天数不溢出（列宽/省略号）", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen();

    const std::vector<module::todo::Column> columns = {col(1, "待办", "todo")};
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    // 400 天前（日历日差 400，三位数）
    std::vector<module::todo::Task> tasks = {
        overdueTask(1, 1, "超长逾期", now - 86400 * 400)};

    auto host = makeHost(columns, tasks);
    module::todo::ui::DialogSet d;
    d.overdue.tasks = tasks;
    d.popups.open(module::todo::ui::Popup::Overdue);

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawOverdueDialog(ui, screen, t, d, host);
    });

    const auto* days = ui.find("todo.dialog.overdue.row.0.days");
    DOCTEST_REQUIRE(days != nullptr);
    DOCTEST_CHECK(days->text.find("400") != std::string::npos);
    DOCTEST_CHECK(days->maxWidth >= 68.0f);
    DOCTEST_CHECK_EQ(days->overflow, core::TextOverflow::Ellipsis);
    return true;
})

// C-11：小窗 640 时壳把选择器高夹到 544；列表高必须按夹后内容区算，
// scroll 底不得越过内容区底（h−89），即不伸进底呼吸隙。
REGISTER_TEST(todo_dialogs_docpick_height_within_clamped_shell,
              "todo_dialogs: 小窗 640 关联文档列表不越底呼吸隙", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen(1000.0f, 640.0f);

    std::vector<module::todo::Column> columns = {col(1, "待办", "todo")};
    std::vector<module::todo::Task> tasks = {overdueTask(1, 1, "任务", 0)};
    auto host = makeHost(columns, tasks);

    module::todo::ui::DialogSet d;
    for (int i = 0; i < 12; ++i) {
        d.docPickNotes.push_back(::core::NoteRef{"n/" + std::to_string(i) + ".md",
                                                 "文档 " + std::to_string(i)});
    }
    d.popups.open(module::todo::ui::Popup::DocPick);

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawDocPickerDialog(ui, screen, t, d, host);
    });

    const auto* panel = ui.find("todo.dialog.docpick.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK(panel->frame.height <= screen.height - 96.0f + 0.5f);

    const auto* scroll = ui.find("todo.dialog.docpick.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    // 壳夹高后可用行区 = 544 − 158 − 24 = 362
    DOCTEST_CHECK(scroll->frame.height <= 362.0f + 0.5f);
    const float contentBottom = panel->frame.y + panel->frame.height - 89.0f;
    DOCTEST_CHECK(scroll->frame.y + scroll->frame.height <=
                  contentBottom + 0.5f);
    // 超视口仍可滚动：最后一行仍在树中（不截断）
    DOCTEST_CHECK(ui.find("todo.dialog.docpick.item.11") != nullptr);
    return true;
})

// B-5：归档卡菜单不显示上移/下移（模块会报「不支持」），「编辑」改「查看」；
// 显示序与模块契约索引不一致时经映射表回传（删除/移动不能错位）。
REGISTER_TEST(todo_dialogs_ctx_menu_archive_selection,
              "todo_dialogs: 归档卡菜单过滤无效项并映射回契约索引", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen();

    std::vector<module::todo::Column> columns = {
        col(1, "待办", "todo"), col(2, "进行中", "doing"),
        col(3, "已完成", "done"), col(4, "已归档", "archive")};
    module::todo::Task archived;
    archived.id = 7;
    archived.columnId = 4;   // 归档列
    archived.title = "归档卡";
    std::vector<module::todo::Task> tasks = {archived};

    std::vector<int> dispatched;
    auto host = makeHost(columns, tasks);
    host.contextSelectPath = [&dispatched](const std::vector<int>& path) {
        if (!path.empty()) dispatched.push_back(path.front());
    };

    module::todo::ui::DialogSet d;
    d.ctx.taskId = archived.id;
    d.ctx.cardIndex = -1;
    d.ctx.anchorX = 400.0f;
    d.popups.open(module::todo::ui::Popup::Ctx);
    const std::map<int64_t, float> scrolls;

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawContextMenu(ui, screen, t, d, host, scrolls);
    });

    // 0 查看 / 1-3 移动到其它三列 / 4 删除；无上移、下移
    DOCTEST_CHECK_EQ(menuLabel(ui, 0), std::string("查看"));
    DOCTEST_CHECK_EQ(menuLabel(ui, 4), std::string("删除"));
    for (int i = 1; i < 4; ++i) {
        DOCTEST_CHECK(menuLabel(ui, i).rfind("移动到", 0) == 0);
    }
    DOCTEST_CHECK(ui.find("todo.ctx.level.0.label.5") == nullptr);
    for (int i = 0; i < 5; ++i) {
        DOCTEST_CHECK(menuLabel(ui, i) != "上移");
        DOCTEST_CHECK(menuLabel(ui, i) != "下移");
    }

    // 显示序 3（最后一个「移动到」）→ 契约 5；显示序 4（删除）→ 契约 6
    const auto* moveItem = ui.find("todo.ctx.level.0.item.3");
    const auto* delItem = ui.find("todo.ctx.level.0.item.4");
    DOCTEST_REQUIRE(moveItem != nullptr);
    DOCTEST_REQUIRE(delItem != nullptr);
    DOCTEST_REQUIRE(moveItem->onClick != nullptr);
    DOCTEST_REQUIRE(delItem->onClick != nullptr);
    moveItem->onClick();
    delItem->onClick();
    DOCTEST_REQUIRE_EQ(dispatched.size(), 2u);
    DOCTEST_CHECK_EQ(dispatched[0], 5);
    DOCTEST_CHECK_EQ(dispatched[1], 6);
    return true;
})

// 对照：普通卡菜单按状态过滤后仍是全量 7 项（编辑/上移/下移/移动×3/删除），
// 删除恒在最末、契约下标不漂移（归档卡隐藏上移/下移 → 5 项，见上一用例）
REGISTER_TEST(todo_dialogs_ctx_menu_normal_unchanged,
              "todo_dialogs: 普通卡菜单 7 项且删除在最末", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen();

    std::vector<module::todo::Column> columns = {
        col(1, "待办", "todo"), col(2, "进行中", "doing"),
        col(3, "已完成", "done"), col(4, "已归档", "archive")};
    module::todo::Task normal;
    normal.id = 9;
    normal.columnId = 1;
    normal.title = "普通卡";
    std::vector<module::todo::Task> tasks = {normal};

    std::vector<int> dispatched;
    auto host = makeHost(columns, tasks);
    host.contextSelectPath = [&dispatched](const std::vector<int>& path) {
        if (!path.empty()) dispatched.push_back(path.front());
    };

    module::todo::ui::DialogSet d;
    d.ctx.taskId = normal.id;
    d.ctx.cardIndex = -1;
    d.ctx.anchorX = 400.0f;
    d.popups.open(module::todo::ui::Popup::Ctx);
    const std::map<int64_t, float> scrolls;

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawContextMenu(ui, screen, t, d, host, scrolls);
    });

    DOCTEST_CHECK_EQ(menuLabel(ui, 0), std::string("编辑"));
    DOCTEST_CHECK_EQ(menuLabel(ui, 1), std::string("上移"));
    DOCTEST_CHECK_EQ(menuLabel(ui, 2), std::string("下移"));
    DOCTEST_CHECK_EQ(menuLabel(ui, 6), std::string("删除"));
    DOCTEST_CHECK(ui.find("todo.ctx.level.0.label.7") == nullptr);
    const auto* delItem = ui.find("todo.ctx.level.0.item.6");
    DOCTEST_REQUIRE(delItem != nullptr);
    DOCTEST_REQUIRE(delItem->onClick != nullptr);
    delItem->onClick();
    DOCTEST_REQUIRE_EQ(dispatched.size(), 1u);
    DOCTEST_CHECK_EQ(dispatched[0], 6);   // 3 + 其他三列
    return true;
})

// B-6：新建弹窗已填内容时遮罩首次只警告不关，第二次才关；「取消」显式放弃立即关
REGISTER_TEST(todo_dialogs_newtask_draft_guard,
              "todo_dialogs: 新建弹窗草稿守卫（遮罩两击 / 取消立即放弃）", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen();

    std::vector<module::todo::Column> columns = {col(1, "待办", "todo")};
    std::vector<module::todo::Task> tasks;
    int warns = 0;
    auto host = makeHost(columns, tasks);
    host.warn = [&warns](const std::string&) { ++warns; };

    module::todo::ui::DialogSet d;
    d.newTask.reset(1);
    d.newTask.title = "半截草稿";
    d.popups.open(module::todo::ui::Popup::NewTask);

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawNewTaskDialog(ui, screen, t, d, host);
    });
    DOCTEST_REQUIRE(ui.find("todo.dialog.newtask.backdrop") != nullptr);
    DOCTEST_REQUIRE(ui.find("todo.dialog.newtask.backdrop")->onClick != nullptr);
    ui.find("todo.dialog.newtask.backdrop")->onClick();   // 第一击：只警告
    DOCTEST_CHECK(warns == 1);
    DOCTEST_CHECK(d.popups.isOpen(module::todo::ui::Popup::NewTask));

    compose(ui, screen, [&] {
        module::todo::ui::drawNewTaskDialog(ui, screen, t, d, host);
    });
    DOCTEST_REQUIRE(ui.find("todo.dialog.newtask.backdrop") != nullptr);
    ui.find("todo.dialog.newtask.backdrop")->onClick();   // 第二击：关
    DOCTEST_CHECK(!d.popups.isOpen(module::todo::ui::Popup::NewTask));
    DOCTEST_CHECK(warns == 1);

    // 取消 = 文案即行为：有草稿也立即放弃，不弹第二次
    d.newTask.reset(1);
    d.newTask.title = "再填一次";
    d.newTask.discardArmed = false;
    d.popups.open(module::todo::ui::Popup::NewTask);
    compose(ui, screen, [&] {
        module::todo::ui::drawNewTaskDialog(ui, screen, t, d, host);
    });
    DOCTEST_REQUIRE(ui.find("todo.dialog.newtask.cancel.bg") != nullptr);
    DOCTEST_REQUIRE(ui.find("todo.dialog.newtask.cancel.bg")->onClick != nullptr);
    ui.find("todo.dialog.newtask.cancel.bg")->onClick();
    DOCTEST_CHECK(!d.popups.isOpen(module::todo::ui::Popup::NewTask));
    DOCTEST_CHECK(warns == 1);
    return true;
})

// B-6：只读详情输入框必须 readOnly（拦截粘贴/编辑），不能只靠空 onChange 丢弃
REGISTER_TEST(todo_dialogs_detail_readonly_inputs_locked,
              "todo_dialogs: 归档卡详情输入框只读、内建编辑不改文本", {
    const auto& t = core::theme::current();
    const eui::Screen screen = makeScreen();

    std::vector<module::todo::Column> columns = {
        col(1, "待办", "todo"), col(4, "已归档", "archive")};
    module::todo::Task archived;
    archived.id = 3;
    archived.columnId = 4;
    archived.title = "原标题";
    archived.description = std::string("原描述");
    std::vector<module::todo::Task> tasks = {archived};
    auto host = makeHost(columns, tasks);

    module::todo::ui::DialogSet d;
    d.detail.reset(archived);
    d.detail.readOnly = true;
    d.popups.open(module::todo::ui::Popup::Detail);

    eui::Ui ui;
    compose(ui, screen, [&] {
        module::todo::ui::drawDetailDialog(ui, screen, t, d, host);
    });

    auto* hit = ui.find("todo.dialog.detail.input.title.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    eui::TextInputEvent ev;
    ev.text = "被改标题";
    hit->onTextInput(ev);
    compose(ui, screen, [&] {
        module::todo::ui::drawDetailDialog(ui, screen, t, d, host);
    });
    const auto* titleText = ui.find("todo.dialog.detail.input.title.text");
    DOCTEST_REQUIRE(titleText != nullptr);
    DOCTEST_CHECK_EQ(titleText->text, std::string("原标题"));
    DOCTEST_CHECK_EQ(d.detail.title, std::string("原标题"));
    return true;
})