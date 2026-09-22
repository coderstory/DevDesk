// tests/test_modules/test_todo_ui.cpp
// Todo UI 补完（P7a 键盘移动 / 右键菜单 / 列管理 / datepicker）测试：
//   1. 纯函数：computeKeyboardMove（上移/下移/边界/带空洞 position）
//   2. 纯函数：ymdToEpoch / epochToYmd 往返
//   3. 无头 compose 冒烟 + 回调交互：右键菜单开合、菜单项点击落库、
//      卡片点击选中 → Ctrl+↑/↓ 键盘移动落库、datepicker 打开渲染
// 证据链 = 代码路径 + 本无头 compose 测试；真实鼠标点验留给用户人工确认
#include "doctest.h"

#include "core/database.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_todo_module();

namespace {

std::filesystem::path tempDbPath() {
    static int counter = 0;
    ++counter;
    return std::filesystem::temp_directory_path() /
           ("devdesk_test_todoui_" + std::to_string(counter) + ".db");
}

// RAII：临时库 + 注册全局 appDatabase（todo 模块 onStart 依赖）
// 析构时一定解除注册，防悬垂污染后续用例
struct TestEnv {
    std::filesystem::path path;
    std::unique_ptr<core::Database> db;
    std::unique_ptr<module::todo::TodoService> svc;
    bool ready = false;

    TestEnv() {
        path = tempDbPath();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) return;
        if (r.unsafeValue().migrate().isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        ::core::registerAppDatabase(&db->db());
        svc = std::make_unique<module::todo::TodoService>(db->db());
        ready = true;
    }

    ~TestEnv() {
        ::core::registerAppDatabase(nullptr);
        svc.reset();
        db.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
};

module::todo::Task makeTask(int64_t id, int64_t col, int pos) {
    module::todo::Task t{};
    t.id = id;
    t.columnId = col;
    t.position = pos;
    return t;
}

// 无头 compose 一帧，返回构建后的 Ui（调用方负责生命周期）
struct Frame {
    eui::Ui ui;
    eui::Screen screen;
    ::core::Module* mod = nullptr;

    Frame(::core::Module* m, float w = 1600.0f, float h = 1000.0f) : mod(m) {
        screen.width = w;
        screen.height = h;
        refresh();
    }
    void refresh() {
        ui.begin("todo_frame");
        mod->compose(ui, screen);
        ui.end();
    }
};

// 构造 Ctrl+方向 键事件
eui::KeyEvent ctrlArrow(eui::InputKey key) {
    eui::KeyEvent e;
    e.key = key;
    e.action = eui::KeyAction::Press;
    e.modifiers.control = true;
    return e;
}

} // namespace

// ---- 1. computeKeyboardMove 纯函数 ----

REGISTER_TEST(todo_ui_move_logic,
              "todo_ui: computeKeyboardMove 上移/下移/边界/空洞", {
    using module::todo::ui::computeKeyboardMove;
    // 注意：REGISTER_TEST 宏体内不能出现裸逗号，vector 用 push_back 构造
    std::vector<module::todo::Task> tasks;
    tasks.push_back(makeTask(1, 10, 0));
    tasks.push_back(makeTask(2, 10, 1));
    tasks.push_back(makeTask(3, 10, 2));
    tasks.push_back(makeTask(4, 20, 0));  // 另一列，不应干扰

    // 中间任务上移：目标 position = 上方任务的 position（它会挤下去）
    auto up = computeKeyboardMove(tasks, 2, true);
    DOCTEST_REQUIRE(up.has_value());
    DOCTEST_CHECK_EQ(up->columnId, static_cast<int64_t>(10));
    DOCTEST_CHECK_EQ(up->position, 0);

    // 中间任务下移：目标 position = 下方任务 position + 1（落到其后）
    auto down = computeKeyboardMove(tasks, 2, false);
    DOCTEST_REQUIRE(down.has_value());
    DOCTEST_CHECK_EQ(down->position, 3);

    // 边界：顶部上移 / 底部下移 → nullopt
    DOCTEST_CHECK(!computeKeyboardMove(tasks, 1, true).has_value());
    DOCTEST_CHECK(!computeKeyboardMove(tasks, 3, false).has_value());

    // 不存在的任务 → nullopt
    DOCTEST_CHECK(!computeKeyboardMove(tasks, 99, true).has_value());

    // 单元素列：两个方向都不可移
    DOCTEST_CHECK(!computeKeyboardMove(tasks, 4, true).has_value());
    DOCTEST_CHECK(!computeKeyboardMove(tasks, 4, false).has_value());

    // position 带空洞（moveTask 留洞后的真实形态）：按真实 position 计算
    std::vector<module::todo::Task> gappy;
    gappy.push_back(makeTask(11, 30, 0));
    gappy.push_back(makeTask(12, 30, 2));
    gappy.push_back(makeTask(13, 30, 5));
    auto gUp = computeKeyboardMove(gappy, 12, true);
    DOCTEST_REQUIRE(gUp.has_value());
    DOCTEST_CHECK_EQ(gUp->position, 0);           // 挪到 pos 0 任务之前
    auto gDown = computeKeyboardMove(gappy, 12, false);
    DOCTEST_REQUIRE(gDown.has_value());
    DOCTEST_CHECK_EQ(gDown->position, 6);         // pos 5 任务之后
    return true;
})

// ---- 2. 日期换算纯函数 ----

REGISTER_TEST(todo_ui_date_roundtrip,
              "todo_ui: ymdToEpoch/epochToYmd 往返", {
    using module::todo::ui::epochToYmd;
    using module::todo::ui::ymdToEpoch;
    const int64_t e = ymdToEpoch(2026, 9, 25);
    int y = 0;
    int m = 0;
    int d = 0;
    DOCTEST_CHECK(epochToYmd(e, y, m, d));
    DOCTEST_CHECK_EQ(y, 2026);
    DOCTEST_CHECK_EQ(m, 9);
    DOCTEST_CHECK_EQ(d, 25);

    // 跨月 / 闰年 2 月底
    const int64_t leap = ymdToEpoch(2028, 2, 29);
    DOCTEST_CHECK(epochToYmd(leap, y, m, d));
    DOCTEST_CHECK_EQ(y, 2028);
    DOCTEST_CHECK_EQ(m, 2);
    DOCTEST_CHECK_EQ(d, 29);

    // 无截止日期
    DOCTEST_CHECK(!epochToYmd(0, y, m, d));
    DOCTEST_CHECK(!epochToYmd(-1, y, m, d));
    return true;
})

// ---- 3. 无头 compose：卡片渲染 + 右键菜单 + 键盘移动 + datepicker ----

REGISTER_TEST(todo_ui_compose_and_context_menu,
              "todo_ui: compose 渲染 + 右键菜单交互（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    auto c1 = env.svc->createColumn("右键源列");
    auto c2 = env.svc->createColumn("右键目标列");
    DOCTEST_REQUIRE(c1.isOk());
    DOCTEST_REQUIRE(c2.isOk());
    auto t1 = env.svc->createTask(c1.value().id, "任务A");
    auto t2 = env.svc->createTask(c1.value().id, "任务B");
    DOCTEST_REQUIRE(t1.isOk());
    DOCTEST_REQUIRE(t2.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        // 卡片渲染（迁移默认 4 列 + 新建 2 列；「右键源列」= col.4）
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.bg") != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.1.bg") != nullptr);
        // 卡片和列头都接了右键回调
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.bg")->onContextMenu != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.col.4.head")->onContextMenu != nullptr);

        // 右键任务A → 菜单打开并渲染（上移/下移/移动到各列）
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.card.0.bg")->onContextMenu(pe, rect);
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.ctx.level.0.item.0") != nullptr);   // 上移
        DOCTEST_CHECK(frame.ui.find("todo.ctx.level.0.item.1") != nullptr);   // 下移

        // 按 text 找到「移动到「右键目标列」」菜单项并点击 → 落库验证
        const core::dsl::Element* moveTo = nullptr;
        for (int i = 2; i < 12 && moveTo == nullptr; ++i) {
            const auto* item = frame.ui.find("todo.ctx.level.0.item." + std::to_string(i));
            if (item == nullptr || item->onClick == nullptr) break;
            const auto* label = frame.ui.find("todo.ctx.level.0.label." + std::to_string(i));
            if (label != nullptr && label->text == "移动到「右键目标列」") {
                moveTo = label;
                item->onClick();
            }
        }
        DOCTEST_REQUIRE(moveTo != nullptr);
        auto inProgress = env.svc->listTasks(c2.value().id);
        DOCTEST_REQUIRE(inProgress.isOk());
        DOCTEST_CHECK_EQ(inProgress.value().size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(inProgress.value()[0].title, std::string("任务A"));
    }

    {
        // 右键列头 → 列管理菜单；点「重命名列」→ 弹窗出现
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.head")->onContextMenu(pe, rect);
        frame.refresh();
        const auto* rename = frame.ui.find("todo.ctx.level.0.item.0");
        DOCTEST_REQUIRE(rename != nullptr);
        rename->onClick();
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.dialog.rename.input") != nullptr);
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_keyboard_move,
              "todo_ui: 点击选中 + Ctrl+↑/↓ 键盘移动落库（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    auto col = env.svc->createColumn("键盘列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "甲");
    auto b = env.svc->createTask(col.value().id, "乙");
    auto c = env.svc->createTask(col.value().id, "丙");
    DOCTEST_REQUIRE(a.isOk() && b.isOk() && c.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 未选中时 Ctrl+↓ 不消费
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlArrow(eui::InputKey::Down)));

    {
        Frame frame(mod.get());
        // 点击「甲」（第 0 张卡）→ 打开详情 = 选中
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }

    // Ctrl+↓ → 甲移到乙之下；Ctrl+↓ 再移到丙之下
    DOCTEST_CHECK(mod->onKeyEvent(ctrlArrow(eui::InputKey::Down)));
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("乙"));
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("甲"));
        DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("丙"));
    }
    DOCTEST_CHECK(mod->onKeyEvent(ctrlArrow(eui::InputKey::Down)));
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("甲"));
    }
    // 已在底部：仍消费（给出提示）但顺序不变
    DOCTEST_CHECK(mod->onKeyEvent(ctrlArrow(eui::InputKey::Down)));
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("甲"));
    }
    // Ctrl+↑ 回到顶部
    DOCTEST_CHECK(mod->onKeyEvent(ctrlArrow(eui::InputKey::Up)));
    DOCTEST_CHECK(mod->onKeyEvent(ctrlArrow(eui::InputKey::Up)));
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("甲"));
    }

    // 无修饰键 / 非 ↑↓ 键不消费
    eui::KeyEvent plain;
    plain.key = eui::InputKey::Down;
    plain.action = eui::KeyAction::Press;
    DOCTEST_CHECK(!mod->onKeyEvent(plain));
    eui::KeyEvent ctrlX = ctrlArrow(eui::InputKey::Left);
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlX));

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_column_ops_and_datepicker,
              "todo_ui: 列左移/右移/删除 + datepicker 渲染（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    auto c1 = env.svc->createColumn("列一");
    auto c2 = env.svc->createColumn("列二");
    DOCTEST_REQUIRE(c1.isOk() && c2.isOk());
    auto t = env.svc->createTask(c1.value().id, "将删列的任务");
    DOCTEST_REQUIRE(t.isOk());
    auto c3 = env.svc->createColumn("日期列");
    DOCTEST_REQUIRE(c3.isOk());
    auto tk = env.svc->createTask(c3.value().id, "带日期");
    DOCTEST_REQUIRE(tk.isOk());
    // 数据必须在模块 onStart 前就位（模块只在自身写操作后 reloadAll）
    // 列布局：[0..3]=迁移默认列，4=列一，5=列二，6=日期列

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        // 「列二」右移（下面还有「日期列」）→ 与其交换
        frame.ui.find("todo.col.5.head")->onContextMenu(pe, rect);
        frame.refresh();
        const auto* shiftRight = frame.ui.find("todo.ctx.level.0.item.2");
        DOCTEST_REQUIRE(shiftRight != nullptr);
        shiftRight->onClick();
        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        DOCTEST_CHECK_EQ(cols.value()[5].title, std::string("日期列"));
        DOCTEST_CHECK_EQ(cols.value()[6].title, std::string("列二"));

        // 「列二」（现在是最右 idx6）右移 → 已在最右，顺序不变
        frame.refresh();  // 上一轮 swap 后重画，col.6 才是「列二」
        frame.ui.find("todo.col.6.head")->onContextMenu(pe, rect);
        frame.refresh();
        const auto* shiftRight1 = frame.ui.find("todo.ctx.level.0.item.2");
        DOCTEST_REQUIRE(shiftRight1 != nullptr);
        shiftRight1->onClick();
        cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        DOCTEST_CHECK_EQ(cols.value()[6].title, std::string("列二"));  // 顺序不变
    }

    // 删除「列一」（idx4，含任务）：菜单 → 删除列 → 确认弹窗 → 确认
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.head")->onContextMenu(pe, rect);  // 列一还在 idx4
        frame.refresh();
        const auto* del = frame.ui.find("todo.ctx.level.0.item.3");
        DOCTEST_REQUIRE(del != nullptr);
        del->onClick();
        frame.refresh();
        const auto* confirm = frame.ui.find("todo.dialog.delcol.confirm.bg");
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_REQUIRE(confirm->onClick != nullptr);
        confirm->onClick();

        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        bool found = false;
        for (const auto& c : cols.value()) {
            if (c.title == "列一") found = true;
        }
        DOCTEST_CHECK(!found);
        // 列下任务 CASCADE 删除
        auto tasks = env.svc->listAllTasks();
        DOCTEST_REQUIRE(tasks.isOk());
        bool taskGone = true;
        for (const auto& tk : tasks.value()) {
            if (tk.title == "将删列的任务") taskGone = false;
        }
        DOCTEST_CHECK(taskGone);
    }

    // datepicker：打开详情 → 点「选择日期」→ datepicker 面板渲染
    // 删除列一后列布局：[0..3]默认列，4=日期列，5=列二
    {
        Frame frame(mod.get());
        frame.refresh();
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onClick();  // 打开详情（选中）
        frame.refresh();
        const auto* pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        DOCTEST_REQUIRE(pick->onClick != nullptr);
        pick->onClick();
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.datepicker.panel") != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.datepicker.column.0.bg") != nullptr);  // 月滚轮
        DOCTEST_CHECK(frame.ui.find("todo.datepicker.column.2.bg") != nullptr);  // 年滚轮
    }

    mod->onStop();
    return true;
})

// ---- 4. W7 position 缺陷回归 ----

REGISTER_TEST(todo_ui_append_position,
              "todo_ui: computeAppendPosition 空列/已有列/空洞", {
    using module::todo::ui::computeAppendPosition;
    std::vector<module::todo::Task> tasks;
    // 空列 → 0
    DOCTEST_CHECK_EQ(computeAppendPosition(tasks, 10), 0);
    tasks.push_back(makeTask(1, 10, 0));
    tasks.push_back(makeTask(2, 10, 2));   // position 带空洞（moveTask 留洞形态）
    tasks.push_back(makeTask(3, 20, 7));   // 另一列，不应干扰
    // 已有列 → max + 1（按真实 position，非 rank）
    DOCTEST_CHECK_EQ(computeAppendPosition(tasks, 10), 3);
    DOCTEST_CHECK_EQ(computeAppendPosition(tasks, 20), 8);
    return true;
})

REGISTER_TEST(todo_ui_edit_keeps_order,
              "todo_ui: 编辑保存不改序（position 保留，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("编辑列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "甲");
    auto b = env.svc->createTask(col.value().id, "乙");
    auto c = env.svc->createTask(col.value().id, "丙");
    DOCTEST_REQUIRE(a.isOk() && b.isOk() && c.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 打开「丙」详情 → 直接点保存（标题未改，仍走完整 updateTask 链路）
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.4.card.2.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame frame(mod.get());
        const auto* save = frame.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_REQUIRE(save->onClick != nullptr);
        save->onClick();
    }
    // 回归断言：丙仍在列尾且 position 保留（旧缺陷：保存硬编码 position=0 → 丙顶到列首）
    auto ts = env.svc->listTasks(col.value().id);
    DOCTEST_REQUIRE(ts.isOk());
    DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("甲"));
    DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("乙"));
    DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("丙"));
    DOCTEST_CHECK_EQ(ts.value()[2].position, c.value().position);

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_move_appends_to_end,
              "todo_ui: 拖拽/右键移动追加到目标列尾（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto src = env.svc->createColumn("移动源列");
    auto dst = env.svc->createColumn("移动目标列");
    DOCTEST_REQUIRE(src.isOk());
    DOCTEST_REQUIRE(dst.isOk());
    auto keeper = env.svc->createTask(dst.value().id, "目标列原任务");
    auto mover = env.svc->createTask(src.value().id, "被移动任务");
    DOCTEST_REQUIRE(keeper.isOk());
    DOCTEST_REQUIRE(mover.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 无头 layout 帧辅助：compose + layout 后元素 frame 即指针空间坐标（dpi=1）
    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto composeLayout = [&]() {
        ui.begin("todo_drag_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    // Element::frame（LayoutRect）→ 回调参数的 Rect（宏体内不能出现裸逗号，逐字段赋值）
    auto boundsOf = [](const core::dsl::Element* e) {
        eui::Rect r;
        r.x = e->frame.x;
        r.y = e->frame.y;
        r.width = e->frame.width;
        r.height = e->frame.height;
        return r;
    };

    // 拖拽（W8 真 drop 语义）：press 记锚点 → drag 中间态 → release 按落点落库。
    // col.4=移动源列 → 拖到 col.5（移动目标列）中央松手 → 落到列尾
    {
        composeLayout();
        const auto* card = ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onPress != nullptr);
        DOCTEST_REQUIRE(card->onDrag != nullptr);
        DOCTEST_REQUIRE(card->onRelease != nullptr);

        // press：锚点 = 卡片真实 bounds（headless dpi=1，layout 帧即指针空间）
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));

        // drag：指针移到目标列中央（中间态：被拖卡半透明 + 目标列高亮）
        const auto* dstBg = ui.find("todo.col.5.bg");
        DOCTEST_REQUIRE(dstBg != nullptr);
        core::dsl::DragEvent ev;
        ev.x = dstBg->frame.x + dstBg->frame.width / 2.0f;
        ev.y = dstBg->frame.y + dstBg->frame.height / 2.0f;
        card->onDrag(ev);
        composeLayout();
        DOCTEST_CHECK_EQ(ui.find("todo.col.4.card.0.bg")->opacity, 0.45f);
        DOCTEST_CHECK_EQ(ui.find("todo.col.5.bg")->border.width, 2.0f);
        DOCTEST_CHECK_EQ(ui.find("todo.col.4.bg")->border.width, 1.0f);

        // release（PointerAction::Release）→ moveTask 落库
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        ui.find("todo.col.4.card.0.bg")->onRelease(up, eui::Rect{});
        composeLayout();
        // 拖拽结束后视觉复位
        DOCTEST_CHECK_EQ(ui.find("todo.col.5.bg")->border.width, 1.0f);
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(2));
        // 追加到列尾：原任务在前（旧缺陷：position=0 → 被移动任务插到列首）
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("目标列原任务"));
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("被移动任务"));
    }

    // 拖出板外 = 取消：把「被移动任务」（现在 col.5.card.1）拖向板左外松手 → 不动作
    {
        composeLayout();
        const auto* card = ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        core::dsl::DragEvent ev;
        ev.x = -100.0;  // 板外
        ev.y = 500.0;
        card->onDrag(ev);
        eui::PointerEvent up;
        up.x = -100.0;
        up.y = 500.0;
        up.action = eui::PointerAction::Release;
        card->onRelease(up, eui::Rect{});
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value().size(), static_cast<size_t>(2));  // 未动
    }

    // 松手落回源列 = 取消；且拖拽松手回落卡上不触发详情弹窗（onClick 抑制）
    {
        composeLayout();
        const auto* card = ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        core::dsl::DragEvent ev;  // 超阈值拖动后又拖回卡上
        ev.x = card->frame.x + card->frame.width / 2.0f;
        ev.y = card->frame.y + card->frame.height / 2.0f;
        ev.totalX = 500.0;
        card->onDrag(ev);
        // 真实派发顺序：onClick 先于 onRelease（同一次松手）
        card->onClick();
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        card->onRelease(up, eui::Rect{});
        composeLayout();
        // 详情弹窗未打开（dialog 组件常驻 compose，open 只控 opacity → 断言 panel 透明）
        const auto* detailPanel = ui.find("todo.dialog.detail.panel");
        DOCTEST_REQUIRE(detailPanel != nullptr);
        DOCTEST_CHECK_EQ(detailPanel->opacity, 0.0f);
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value().size(), static_cast<size_t>(2));  // 未动
    }

    // 右键菜单「移动到」同样追加列尾：把「被移动任务」移回源列（此时源列已空 → 追加位 0）
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const core::dsl::Element* moveTo = nullptr;
        for (int i = 2; i < 12 && moveTo == nullptr; ++i) {
            const auto* item = frame.ui.find("todo.ctx.level.0.item." + std::to_string(i));
            if (item == nullptr || item->onClick == nullptr) break;
            const auto* label = frame.ui.find("todo.ctx.level.0.label." + std::to_string(i));
            if (label != nullptr && label->text == "移动到「移动源列」") {
                moveTo = label;
                item->onClick();
            }
        }
        DOCTEST_REQUIRE(moveTo != nullptr);
        auto ts = env.svc->listTasks(src.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("被移动任务"));
        DOCTEST_CHECK_EQ(ts.value()[0].position, 0);
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_drag_hit_pure,
              "todo_ui: dragDropTarget 命中/列隙/板外/滚动/dpi（纯函数）", {
    using module::todo::ui::DragHitInput;
    using module::todo::ui::dragDropTarget;
    // 锚定卡在列 1 第 0 张：板面（dpi=1）originX=80，列 [80,280][300,500][520,720]，
    // boardTop=144，boardBottom=644
    DragHitInput in;
    in.fromColIndex = 1;
    in.colCount = 3;
    in.pressX = 300.0f;
    in.pressY = 200.0f;
    in.pressWidth = 200.0f;
    in.cardWidth = 200.0f;
    in.colWidth = 200.0f;
    in.colGap = 20.0f;
    in.boardHeight = 500.0f;
    in.scrollOffset = 0.0f;
    in.cardIndex = 0;

    DOCTEST_CHECK_EQ(dragDropTarget(in, 400.0, 300.0), 1);   // 源列
    DOCTEST_CHECK_EQ(dragDropTarget(in, 350.0, 300.0), 1);   // 源列内偏左
    DOCTEST_CHECK_EQ(dragDropTarget(in, 550.0, 300.0), 2);   // 右列（跨列拖）
    DOCTEST_CHECK_EQ(dragDropTarget(in, 100.0, 300.0), 0);   // 左列
    DOCTEST_CHECK_EQ(dragDropTarget(in, 290.0, 300.0), -1);  // 列间隙
    DOCTEST_CHECK_EQ(dragDropTarget(in, 50.0, 300.0), -1);   // 板左外
    DOCTEST_CHECK_EQ(dragDropTarget(in, 400.0, 100.0), -1);  // 板上方
    DOCTEST_CHECK_EQ(dragDropTarget(in, 400.0, 700.0), -1);  // 板下方

    // HiDPI（dpi=2）：指针空间坐标全翻倍，命中结果不变
    DragHitInput hi = in;
    hi.pressX = 600.0f;
    hi.pressY = 400.0f;
    hi.pressWidth = 400.0f;
    DOCTEST_CHECK_EQ(dragDropTarget(hi, 700.0, 600.0), 1);   // 源列
    DOCTEST_CHECK_EQ(dragDropTarget(hi, 1100.0, 600.0), 2);  // 右列

    // 列内滚动 100：boardTop 随可视上移下推到 244
    DragHitInput sc = in;
    sc.scrollOffset = 100.0f;
    DOCTEST_CHECK_EQ(dragDropTarget(sc, 400.0, 200.0), -1);  // 原 200 在板内，滚动后板上方
    DOCTEST_CHECK_EQ(dragDropTarget(sc, 400.0, 300.0), 1);

    // 退化输入 → 取消
    DOCTEST_CHECK_EQ(dragDropTarget(DragHitInput{}, 0.0, 0.0), -1);
    DragHitInput bad = in;
    bad.cardWidth = 0.0f;
    DOCTEST_CHECK_EQ(dragDropTarget(bad, 400.0, 300.0), -1);
    return true;
})

REGISTER_TEST(todo_ui_column_scroll_geometry,
              "todo_ui: 列内滚动——第 12 张起不裁切 + 列几何正确（headless layout）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("长列");
    DOCTEST_REQUIRE(col.isOk());
    for (int i = 1; i <= 15; ++i) {
        auto r = env.svc->createTask(col.value().id, "任务" + std::to_string(i));
        DOCTEST_REQUIRE(r.isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto layoutFrame = [&]() {
        ui.begin("todo_scroll_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    layoutFrame();

    // 15 张卡全部构建（旧缺陷：第 ~8 张起因列高 break 消失）
    for (int i = 0; i < 15; ++i) {
        const auto* card = ui.find("todo.col.4.card." + std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(card != nullptr);
    }
    // 滚动容器存在
    DOCTEST_CHECK(ui.find("todo.col.4.scroll") != nullptr);

    // 列几何回归（stack 包装修复）：列头与列背景同 x 起点，卡片在列内纵向排列。
    // 旧缺陷：position() 在 row 流式布局里被忽略，列头/卡片横向排开（col 头 x 偏移一整列宽）
    const auto* colBg = ui.find("todo.col.4.bg");
    const auto* colHead = ui.find("todo.col.4.head");
    const auto* colTitle = ui.find("todo.col.4.title");
    DOCTEST_REQUIRE(colBg != nullptr);
    DOCTEST_REQUIRE(colHead != nullptr);
    DOCTEST_REQUIRE(colTitle != nullptr);
    DOCTEST_CHECK_EQ(colHead->frame.x, colBg->frame.x);
    DOCTEST_CHECK_EQ(colHead->frame.y, colBg->frame.y);
    DOCTEST_CHECK_EQ(colTitle->frame.x, colBg->frame.x + 16.0f);
    const auto* card0 = ui.find("todo.col.4.card.0.bg");
    const auto* card1 = ui.find("todo.col.4.card.1.bg");
    const auto* card14 = ui.find("todo.col.4.card.14.bg");
    DOCTEST_REQUIRE(card0 != nullptr);
    DOCTEST_REQUIRE(card1 != nullptr);
    DOCTEST_REQUIRE(card14 != nullptr);
    DOCTEST_CHECK_EQ(card0->frame.x, colBg->frame.x);        // 卡片在列内（不是横向流式排开）
    DOCTEST_CHECK_EQ(card1->frame.x, colBg->frame.x);
    DOCTEST_CHECK_EQ(card1->frame.y - card0->frame.y, 84.0f + 8.0f);  // 卡高 + 间距
    DOCTEST_CHECK(card14->frame.y > card1->frame.y);          // 超出视口的卡继续向下排

    // 多列不重叠：col.4 与相邻 col.3 背景横向相邻（x 差 = 列宽 + 间距）
    const auto* prevBg = ui.find("todo.col.3.bg");
    DOCTEST_REQUIRE(prevBg != nullptr);
    DOCTEST_CHECK(colBg->frame.x > prevBg->frame.x);
    DOCTEST_CHECK(colBg->frame.x - prevBg->frame.x < colBg->frame.width * 2.0f);

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_error_state_and_retry,
              "todo_ui: db 未就绪错误态 + 重试恢复（headless）", {
    // 前置：确保全局无注册数据库（TestEnv 析构后即此状态，显式再清一次防串扰）
    ::core::registerAppDatabase(nullptr);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "devdesk_test_todo_retry.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();  // db null → service_ 未初始化，只 WARN 不崩

    {
        Frame frame(mod.get());
        // 错误态可见：文案 + 重试按钮（不是静默空白）
        const auto* errText = frame.ui.find("todo.error.text");
        DOCTEST_REQUIRE(errText != nullptr);
        DOCTEST_CHECK(errText->text.find("尚未就绪") != std::string::npos);
        const auto* retry = frame.ui.find("todo.error.retry.bg");
        DOCTEST_REQUIRE(retry != nullptr);
        DOCTEST_REQUIRE(retry->onClick != nullptr);
        // 看板不渲染（无列背景）
        DOCTEST_CHECK(frame.ui.find("todo.col.0.bg") == nullptr);

        // 数据库晚注册 → 点重试 → 恢复
        std::error_code ec2;
        std::filesystem::remove(path, ec2);
        auto r = ::core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.unsafeValue().migrate().isOk());
        auto db = std::make_unique<::core::Database>(std::move(r.unsafeValue()));
        ::core::registerAppDatabase(&db->db());
        retry->onClick();
    }
    {
        Frame frame(mod.get());
        DOCTEST_CHECK(frame.ui.find("todo.error.text") == nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.col.0.bg") != nullptr);  // 默认 4 列已加载
    }

    mod->onStop();
    ::core::registerAppDatabase(nullptr);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return true;
})

REGISTER_TEST(todo_ui_ctx_edit_and_delete,
              "todo_ui: 右键菜单「编辑」开详情 / 「删除」带确认（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("菜单列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "待编辑任务");
    auto b = env.svc->createTask(col.value().id, "陪衬任务");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 右键第 0 张卡 → item.0 = 编辑 → 详情弹窗打开且标题正确
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const auto* edit = frame.ui.find("todo.ctx.level.0.item.0");
        DOCTEST_REQUIRE(edit != nullptr);
        DOCTEST_REQUIRE(edit->onClick != nullptr);
        const auto* label0 = frame.ui.find("todo.ctx.level.0.label.0");
        DOCTEST_REQUIRE(label0 != nullptr);
        DOCTEST_CHECK_EQ(label0->text, std::string("编辑"));
        edit->onClick();
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.input.title") != nullptr);
    }

    // 右键同一张卡 → 末项 = 删除 → 确认弹窗（任务模式，文案含任务标题）→ 确认 → 落库删除
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.card.0.bg")->onContextMenu(pe, rect);
        frame.refresh();
        // 5 列时任务菜单：0=编辑 1=上移 2=下移 3..6=移动到 7=删除
        const auto* del = frame.ui.find("todo.ctx.level.0.item.7");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        const auto* label7 = frame.ui.find("todo.ctx.level.0.label.7");
        DOCTEST_REQUIRE(label7 != nullptr);
        DOCTEST_CHECK_EQ(label7->text, std::string("删除"));
        del->onClick();
        frame.refresh();
        const auto* body = frame.ui.find("todo.dialog.delcol.body");
        DOCTEST_REQUIRE(body != nullptr);
        DOCTEST_CHECK(body->text.find("待编辑任务") != std::string::npos);
        const auto* confirm = frame.ui.find("todo.dialog.delcol.confirm.bg");
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_REQUIRE(confirm->onClick != nullptr);
        confirm->onClick();
    }
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("陪衬任务"));
    }

    mod->onStop();
    return true;
})

// 详情弹窗「删除」也走确认弹窗（与右键菜单路径行为统一）
REGISTER_TEST(todo_ui_detail_delete_confirms,
              "todo_ui: 详情弹窗删除先弹确认（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("详情删除列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "详情待删任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 右键卡片 → 编辑 → 打开详情弹窗
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const auto* edit = frame.ui.find("todo.ctx.level.0.item.0");
        DOCTEST_REQUIRE(edit != nullptr);
        edit->onClick();
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.dialog.detail.input.title") != nullptr);

        // 点详情里的「删除」→ 不应直接删，而是切到确认弹窗（文案含任务标题）
        const auto* del = frame.ui.find("todo.dialog.detail.delete.bg");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        del->onClick();
        frame.refresh();
        const auto* body = frame.ui.find("todo.dialog.delcol.body");
        DOCTEST_REQUIRE(body != nullptr);
        DOCTEST_CHECK(body->text.find("详情待删任务") != std::string::npos);

        // 确认弹窗点「取消」→ 任务仍在
        const auto* cancel = frame.ui.find("todo.dialog.delcol.cancel.bg");
        DOCTEST_REQUIRE(cancel != nullptr);
        DOCTEST_REQUIRE(cancel->onClick != nullptr);
        cancel->onClick();
    }
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value().size(), 1u);
    }

    mod->onStop();
    return true;
})

// 完成态按 kind 落列（不靠「已完成」标题）：重命名已完成列后，
// 详情弹窗「标记完成」仍应把任务移进该列（回归：此前按标题找列，
// 重命名后任务静默留在原列）
REGISTER_TEST(todo_ui_complete_by_kind,
              "todo_ui: 完成按 kind 落列，列改名后仍命中（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;    // kind=todo
    const int64_t doneCol = list.value()[2].id;    // kind=done
    // 模块启动前先改名（模块 onStart 才会缓存列）
    DOCTEST_REQUIRE(env.svc->renameColumn(doneCol, "Finished").isOk());
    auto a = env.svc->createTask(todoCol, "kind 回归任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 右键卡片 → 编辑 → 详情 → 点「标记完成」
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const auto* edit = frame.ui.find("todo.ctx.level.0.item.0");
        DOCTEST_REQUIRE(edit != nullptr);
        edit->onClick();
        frame.refresh();
        const auto* toggle = frame.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        toggle->onClick();
    }

    // 任务应落到改名的已完成列且带 completedAt
    {
        auto inDone = env.svc->listTasks(doneCol);
        DOCTEST_REQUIRE(inDone.isOk());
        DOCTEST_REQUIRE_EQ(inDone.value().size(), 1u);
        DOCTEST_CHECK_EQ(inDone.value()[0].title, std::string("kind 回归任务"));
        DOCTEST_CHECK(inDone.value()[0].completedAt.has_value());
        auto inTodo = env.svc->listTasks(todoCol);
        DOCTEST_REQUIRE(inTodo.isOk());
        DOCTEST_CHECK_EQ(inTodo.value().size(), 0u);
    }

    mod->onStop();
    return true;
})

// 详情弹窗「移动到」全列遍历（此前硬编码 i < 4，第 5 列起不可移）
REGISTER_TEST(todo_ui_detail_move_all_columns,
              "todo_ui: 详情弹窗移动到覆盖全部列（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;
    // 补 2 列 → 共 6 列（超过旧的 4 列上限）
    DOCTEST_REQUIRE(env.svc->createColumn("第五列").isOk());
    DOCTEST_REQUIRE(env.svc->createColumn("第六列").isOk());
    auto a = env.svc->createTask(todoCol, "跨列移动任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 打开详情 → 第 5、6 列按钮都存在，点第 6 列按钮任务真的移过去
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const auto* edit = frame.ui.find("todo.ctx.level.0.item.0");
        DOCTEST_REQUIRE(edit != nullptr);
        edit->onClick();
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.dialog.detail.move.3") != nullptr);
        const auto* move5 = frame.ui.find("todo.dialog.detail.move.5.bg");
        DOCTEST_REQUIRE(move5 != nullptr);
        DOCTEST_REQUIRE(move5->onClick != nullptr);
        move5->onClick();
    }
    {
        auto list2 = env.svc->listColumns();
        DOCTEST_REQUIRE(list2.isOk());
        DOCTEST_REQUIRE_EQ(list2.value().size(), 6u);
        auto inSixth = env.svc->listTasks(list2.value()[5].id);
        DOCTEST_REQUIRE(inSixth.isOk());
        DOCTEST_REQUIRE_EQ(inSixth.value().size(), 1u);
        DOCTEST_CHECK_EQ(inSixth.value()[0].title, std::string("跨列移动任务"));
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_fontscale_tokens,
              "todo_ui: 模块字号经 token 派生，全局缩放生效（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("字号列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "字号任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    auto cardTitleSize = [&](float* out) {
        eui::Ui ui;
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        ui.begin("todo_font_frame");
        mod->compose(ui, screen);
        ui.end();
        const auto* title = ui.find("todo.col.4.card.0.title");
        if (title == nullptr) return false;
        *out = title->fontSize;
        return true;
    };
    ::core::theme::setFontScale(1.0f);
    float base = 0.0f;
    DOCTEST_REQUIRE(cardTitleSize(&base));       // 14（Body-1 派生）
    ::core::theme::setFontScale(1.3f);
    float scaled = 0.0f;
    DOCTEST_REQUIRE(cardTitleSize(&scaled));
    ::core::theme::setFontScale(1.0f);           // 恢复，不影响其他用例
    DOCTEST_CHECK_EQ(base, 14.0f);
    // 派生式 = token(缩放后) - 1：15×1.3 - 1 = 18.5（字面量时代这里恒为 14 不动）
    DOCTEST_CHECK_EQ(scaled, 15.0f * 1.3f - 1.0f);
    return true;
})

// ---- 4. 深色主题列区域白底回归 ----
// 根因：列头右键热区 ui.rect(id + ".head") 没设 color，Element::color 默认
// {1,1,1,1} 不透明白 → 深色主题下每列列头被整条刷白（浅色下白底不可见）。
// 这里全树扫描：深色主题下 Todo 页任何 Rect 元素都不允许是「不透明近白色」。
namespace {

void collectOpaqueWhiteRects(const core::dsl::Element* el,
                             std::vector<const core::dsl::Element*>* out) {
    if (el == nullptr) return;
    if (el->kind == core::dsl::ElementKind::Rect &&
        el->color.a > 0.99f && el->color.r > 0.9f &&
        el->color.g > 0.9f && el->color.b > 0.9f) {
        out->push_back(el);
    }
    for (const auto& child : el->children) {
        collectOpaqueWhiteRects(child.get(), out);
    }
}

} // namespace

REGISTER_TEST(todo_ui_dark_no_white_rects,
              "todo_ui: 深色主题下列区域无白底 Rect（.head 热区回归）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("深色列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "深色任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    ::core::theme::setDark();   // 默认即深色，显式声明本用例前提
    Frame frame(mod.get());

    // 直接回归点：列头热区必须是全透明（默认白是 bug）
    const auto* head = frame.ui.find("todo.col.4.head");
    DOCTEST_REQUIRE(head != nullptr);
    DOCTEST_CHECK_EQ(head->color.a, 0.0f);

    // 全树兜底：深色主题下不允许任何不透明近白 Rect
    std::vector<const core::dsl::Element*> whites;
    for (const auto& root : frame.ui.roots()) {
        collectOpaqueWhiteRects(root.get(), &whites);
    }
    for (const auto* el : whites) {
        // 失败时直接输出元素 id，一眼定位
        DOCTEST_CHECK_MESSAGE(false, ("意外的不透明白 Rect: " + el->id).c_str());
    }
    DOCTEST_CHECK(whites.empty());
    return true;
})
