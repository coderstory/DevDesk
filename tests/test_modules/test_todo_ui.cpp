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
#include "core/input/input_types.h"

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
        // 列菜单：0=重命名 1=左移 2=右移 3=改色(子菜单) 4=删除
        const auto* del = frame.ui.find("todo.ctx.level.0.item.4");
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

    // 松手落回源列 = 取消；且拖拽松手回落卡上不触发详情弹窗。
    // 终验复审：无头直接调 onClick/onRelease 回调时 click 从不派发，旧的
    // 「详情弹窗未开」断言恒真。改为驱动 EUI-NEO InteractionState 真实时序
    //（框架派发 click 的唯一仲裁者）：press → 超阈值 drag → release 回卡内
    // → clicked=false；对照组 press → release 原地未动 → clicked=true
    {
        composeLayout();
        const auto* card = ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(card != nullptr);
        const eui::Rect bounds = boundsOf(card);
        const float cx = card->frame.x + card->frame.width / 2.0f;
        const float cy = card->frame.y + card->frame.height / 2.0f;
        auto makeEvent = [](float x, float y, eui::PointerAction action) {
            eui::PointerEvent ev;
            ev.x = x;
            ev.y = y;
            ev.action = action;
            ev.button = core::PointerButton::Left;
            ev.buttons.set(core::PointerButton::Left,
                           action != eui::PointerAction::Release);
            return ev;
        };
        // 拖拽时序：press → 拖出 500px（超阈值）→ 拖回卡内松手
        core::InteractionState dragState;
        dragState.update(bounds, makeEvent(cx, cy, eui::PointerAction::Press),
                         true, core::PointerButton::Left);
        DOCTEST_CHECK(dragState.pressStarted);
        dragState.update(bounds, makeEvent(cx + 500.0f, cy, eui::PointerAction::Move),
                         true, core::PointerButton::Left);
        DOCTEST_CHECK(dragState.dragMoved);  // 超阈值拖动激活过
        dragState.update(bounds, makeEvent(cx, cy, eui::PointerAction::Release),
                         true, core::PointerButton::Left);
        DOCTEST_CHECK(dragState.released);
        DOCTEST_CHECK(!dragState.clicked);  // 拖过又拖回 ≠ 点击 → onClick 不派发

        // 对照组：press → 原地 release → clicked=true（这条路径才开详情弹窗）
        core::InteractionState clickState;
        clickState.update(bounds, makeEvent(cx, cy, eui::PointerAction::Press),
                          true, core::PointerButton::Left);
        clickState.update(bounds, makeEvent(cx, cy, eui::PointerAction::Release),
                          true, core::PointerButton::Left);
        DOCTEST_CHECK(clickState.clicked);

        // 模块 drop 路径照旧走真实回调：拖出又拖回松手 → 落回源列 = 取消，
        // 详情弹窗不因松手打开（回归护栏：模块不得在 onRelease 里自开详情）
        eui::PointerEvent pe;
        card->onPress(pe, bounds);
        core::dsl::DragEvent ev;  // 超阈值拖动后又拖回卡上
        ev.x = cx;
        ev.y = cy;
        ev.totalX = 500.0;
        card->onDrag(ev);
        eui::PointerEvent up;
        up.x = cx;
        up.y = cy;
        up.action = eui::PointerAction::Release;
        card->onRelease(up, eui::Rect{});
        composeLayout();
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

// 状态反馈 toast：超时消失 + subtitle 保持静态说明
// （此前 setStatus 写进 subtitle 永久驻留，静态操作说明被顶掉）
REGISTER_TEST(todo_ui_status_toast_timeout,
              "todo_ui: 状态提示 toast 超时消失，subtitle 静态（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    Frame frame(mod.get());
    // 无选中任务按 Ctrl+↑ → 错误反馈进 toast（而非 subtitle）
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlArrow(eui::InputKey::Up)));
    frame.refresh();
    const auto* title = frame.ui.find("todo.toast.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK(title->text.find("先点击卡片选中任务") != std::string::npos);
    // subtitle 始终是静态操作说明
    const auto* sub = frame.ui.find("todo.subtitle");
    DOCTEST_REQUIRE(sub != nullptr);
    DOCTEST_CHECK(sub->text.find("点击卡片查看详情") != std::string::npos);

    // toast 带超时回调（3.5s 自动消失）→ 触发后 visible 关闭
    const auto* timer = frame.ui.find("todo.toast.timer");
    DOCTEST_REQUIRE(timer != nullptr);
    DOCTEST_CHECK(timer->timerSeconds > 0.0f);
    DOCTEST_REQUIRE(timer->onTimer != nullptr);
    const auto* toastOpen = frame.ui.find("todo.toast");
    DOCTEST_REQUIRE(toastOpen != nullptr);
    DOCTEST_CHECK_EQ(toastOpen->opacity, 1.0f);
    timer->onTimer();
    frame.refresh();
    const auto* toastClosed = frame.ui.find("todo.toast");
    DOCTEST_REQUIRE(toastClosed != nullptr);
    DOCTEST_CHECK_EQ(toastClosed->opacity, 0.0f);

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

// ---- 5. 三件套（W 计划 P2 遗留）：搜索 / 逾期 / tags / 列色点 ----

// 搜索框：input 组件真实 onTextInput 链路 → onChange → service_->search → 卡片过滤
REGISTER_TEST(todo_ui_search_filter,
              "todo_ui: 搜索框过滤卡片 + Esc 清空（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("搜索列");
    DOCTEST_REQUIRE(col.isOk());
    auto hitTask = env.svc->createTask(col.value().id, "搜索命中甲");
    auto missTask = env.svc->createTask(col.value().id, "无关任务乙");
    DOCTEST_REQUIRE(hitTask.isOk() && missTask.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        // 搜索前：两张卡都在
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.bg") != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.1.bg") != nullptr);

        // 输入搜索词（onTextInput → onChange → refreshSearch）
        auto* input = frame.ui.find("todo.search.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        core::TextInputEvent ev;
        ev.text = "命中";
        input->onTextInput(ev);
        frame.refresh();
        // 只显示命中卡（命中列照常渲染）
        const auto* card0 = frame.ui.find("todo.col.4.card.0.title");
        DOCTEST_REQUIRE(card0 != nullptr);
        DOCTEST_CHECK_EQ(card0->text, std::string("搜索命中甲"));
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.1.bg") == nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.col.0.bg") != nullptr);

        // Esc 清空（EUI-NEO input 把 Escape 映射到 onEnter → 清空 query 恢复全部）
        auto* input2 = frame.ui.find("todo.search.hit");
        DOCTEST_REQUIRE(input2 != nullptr);
        core::KeyEvent esc;
        esc.key = core::InputKey::Escape;
        esc.action = core::KeyAction::Press;
        input2->onKeyEvent(esc);
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.1.bg") != nullptr);
    }

    mod->onStop();
    return true;
})

// 逾期视图：badge 数量（已完成/未到期不计）+ 弹窗四列数据 + 行点击跳详情
REGISTER_TEST(todo_ui_overdue_dialog,
              "todo_ui: 逾期 badge + 弹窗数据 + 行点击跳详情（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;

    // 3 个任务：逾期未完成（3 天 1 小时前）/ 逾期但已完成 / 未到期
    auto ov = env.svc->createTask(todoCol, "逾期任务甲");
    auto doneOv = env.svc->createTask(todoCol, "已完成的逾期");
    auto future = env.svc->createTask(todoCol, "未来任务");
    DOCTEST_REQUIRE(ov.isOk() && doneOv.isOk() && future.isOk());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    {
        module::todo::Task t = ov.value();
        // 本地日历日前天 12:00 到期（日历日差 = 2 天；与测试机时区无关）
        std::tm dueTm{};
        core::time::localTime(now, &dueTm);
        dueTm.tm_hour = 12;
        dueTm.tm_min = 0;
        dueTm.tm_sec = 0;
        t.dueAt = std::mktime(&dueTm) - 2 * 86400;  // 逾期 2 天（日历日差）
        DOCTEST_REQUIRE(env.svc->updateTask(t).isOk());
        module::todo::Task d = doneOv.value();
        d.dueAt = now - 86400;
        d.completedAt = now;
        DOCTEST_REQUIRE(env.svc->updateTask(d).isOk());
        module::todo::Task f = future.value();
        f.dueAt = now + 86400;
        DOCTEST_REQUIRE(env.svc->updateTask(f).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        // badge = 1
        const auto* badge = frame.ui.find("todo.btn.overdue.text");
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK(badge->text.find("1") != std::string::npos);
        // 打开弹窗
        const auto* btn = frame.ui.find("todo.btn.overdue.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
        frame.refresh();
        // 单行数据：任务名 / 所属列 / 逾期天数（截止日期列由 formatDue 渲染）
        const auto* rowTitle = frame.ui.find("todo.dialog.overdue.row.0.title");
        const auto* rowCol = frame.ui.find("todo.dialog.overdue.row.0.col");
        const auto* rowDays = frame.ui.find("todo.dialog.overdue.row.0.days");
        DOCTEST_REQUIRE(rowTitle != nullptr);
        DOCTEST_REQUIRE(rowCol != nullptr);
        DOCTEST_REQUIRE(rowDays != nullptr);
        DOCTEST_CHECK_EQ(rowTitle->text, std::string("逾期任务甲"));
        DOCTEST_CHECK_EQ(rowCol->text, std::string("待办"));
        DOCTEST_CHECK(rowDays->text.find("2") != std::string::npos);
        // 行点击 → 关闭逾期弹窗 + 打开该任务详情
        const auto* row = frame.ui.find("todo.dialog.overdue.row.0");
        DOCTEST_REQUIRE(row != nullptr);
        DOCTEST_REQUIRE(row->onClick != nullptr);
        row->onClick();
        frame.refresh();
        const auto* detailPanel = frame.ui.find("todo.dialog.detail.panel");
        DOCTEST_REQUIRE(detailPanel != nullptr);
        DOCTEST_CHECK_EQ(detailPanel->opacity, 1.0f);
        const auto* overduePanel = frame.ui.find("todo.dialog.overdue.panel");
        DOCTEST_REQUIRE(overduePanel != nullptr);
        DOCTEST_CHECK_EQ(overduePanel->opacity, 0.0f);
    }

    mod->onStop();
    return true;
})

// 无逾期：badge 显示 0 且点击不弹窗
REGISTER_TEST(todo_ui_overdue_zero_grey,
              "todo_ui: 无逾期时 badge 为 0 且不可开（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    Frame frame(mod.get());
    const auto* badge = frame.ui.find("todo.btn.overdue.text");
    DOCTEST_REQUIRE(badge != nullptr);
    DOCTEST_CHECK(badge->text.find("0") != std::string::npos);
    const auto* btn = frame.ui.find("todo.btn.overdue.bg");
    DOCTEST_REQUIRE(btn != nullptr);
    DOCTEST_REQUIRE(btn->onClick != nullptr);
    btn->onClick();
    frame.refresh();
    const auto* panel = frame.ui.find("todo.dialog.overdue.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_EQ(panel->opacity, 0.0f);

    mod->onStop();
    return true;
})

// tags 编辑：回车添加 / × 删除 / 保存往返（JSON 序列化）/ 卡片渲染前 2 个 + 超出 +N
REGISTER_TEST(todo_ui_tags_edit_roundtrip,
              "todo_ui: 详情 tags 编辑往返 + 卡片 chip 渲染（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("标签列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "标签任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 打开详情
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onClick();
    }
    // 加两个标签，删除第一个
    {
        Frame frame(mod.get());
        auto* input = frame.ui.find("todo.dialog.detail.tag.input.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        DOCTEST_REQUIRE(input->onKeyEvent != nullptr);
        core::KeyEvent enter;
        enter.key = core::InputKey::Enter;
        enter.action = core::KeyAction::Press;
        core::TextInputEvent ev;
        ev.text = "urgent";
        input->onTextInput(ev);
        input->onKeyEvent(enter);
        frame.refresh();
        auto* input2 = frame.ui.find("todo.dialog.detail.tag.input.hit");
        DOCTEST_REQUIRE(input2 != nullptr);
        core::TextInputEvent ev2;
        ev2.text = "work";
        input2->onTextInput(ev2);
        input2->onKeyEvent(enter);
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.tag.0.bg") != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.tag.1.bg") != nullptr);
        // 删除第一个 chip（× 按钮）
        const auto* x0 = frame.ui.find("todo.dialog.detail.tag.0.x.bg");
        DOCTEST_REQUIRE(x0 != nullptr);
        DOCTEST_REQUIRE(x0->onClick != nullptr);
        x0->onClick();
        frame.refresh();
        const auto* chip0 = frame.ui.find("todo.dialog.detail.tag.0.text");
        DOCTEST_REQUIRE(chip0 != nullptr);
        DOCTEST_CHECK_EQ(chip0->text, std::string("work"));
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.tag.1.bg") == nullptr);
    }
    // 再加两个到 3 个，保存（验证往返 + 卡片 +N）
    {
        Frame frame(mod.get());
        core::KeyEvent enter;
        enter.key = core::InputKey::Enter;
        enter.action = core::KeyAction::Press;
        for (int i = 0; i < 2; ++i) {
            auto* cur = frame.ui.find("todo.dialog.detail.tag.input.hit");
            DOCTEST_REQUIRE(cur != nullptr);
            core::TextInputEvent ev;
            ev.text = "t" + std::to_string(i);
            cur->onTextInput(ev);
            cur->onKeyEvent(enter);
            frame.refresh();
        }
        const auto* save = frame.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_REQUIRE(save->onClick != nullptr);
        save->onClick();
    }
    {
        // DB 往返：tags = work, t0, t1（urgent 已删）
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value()[0].tags.size(), static_cast<size_t>(3));
        DOCTEST_CHECK_EQ(ts.value()[0].tags[0], std::string("work"));
        DOCTEST_CHECK_EQ(ts.value()[0].tags[1], std::string("t0"));
        DOCTEST_CHECK_EQ(ts.value()[0].tags[2], std::string("t1"));
    }
    {
        // 卡片：前 2 个 tag chip + 超出 +N
        Frame frame(mod.get());
        const auto* tag0 = frame.ui.find("todo.col.4.card.0.tag.0.text");
        DOCTEST_REQUIRE(tag0 != nullptr);
        DOCTEST_CHECK_EQ(tag0->text, std::string("work"));
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.tag.1.bg") != nullptr);
        const auto* more = frame.ui.find("todo.col.4.card.0.tag.more.text");
        DOCTEST_REQUIRE(more != nullptr);
        DOCTEST_CHECK_EQ(more->text, std::string("+1"));
    }

    mod->onStop();
    return true;
})

// 列色点：迁移种子渲染 + 新建列无色不渲 + 右键改色落库
REGISTER_TEST(todo_ui_column_color_dot_and_menu,
              "todo_ui: 列色点渲染 + 右键改色子选落库（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto plain = env.svc->createColumn("无色列");
    DOCTEST_REQUIRE(plain.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        // 迁移种子：待办列 color='claude-clay' → 色点且色值正确
        const auto* dot = frame.ui.find("todo.col.0.dot");
        DOCTEST_REQUIRE(dot != nullptr);
        const eui::Color clay("#D97757");
        DOCTEST_CHECK_EQ(dot->color.r, clay.r);
        DOCTEST_CHECK_EQ(dot->color.g, clay.g);
        DOCTEST_CHECK_EQ(dot->color.b, clay.b);
        // 用户新建列无色（color NULL）→ 不渲色点
        DOCTEST_CHECK(frame.ui.find("todo.col.4.dot") == nullptr);

        // 右键列头 → 改色（子选）→ 选第一个预设 Claude 橘
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.head")->onContextMenu(pe, rect);
        frame.refresh();
        const auto* colorItem = frame.ui.find("todo.ctx.level.0.item.3");
        DOCTEST_REQUIRE(colorItem != nullptr);
        DOCTEST_REQUIRE(colorItem->onClick != nullptr);
        colorItem->onClick();  // 打开子菜单（不关闭主菜单）
        frame.refresh();
        const auto* preset0 = frame.ui.find("todo.ctx.level.1.item.0");
        DOCTEST_REQUIRE(preset0 != nullptr);
        DOCTEST_REQUIRE(preset0->onClick != nullptr);
        preset0->onClick();
    }
    {
        // 落库：无色列 → claude-clay，且色点出现
        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        bool checked = false;
        for (const auto& c : cols.value()) {
            if (c.title == "无色列") {
                DOCTEST_REQUIRE(c.color.has_value());
                DOCTEST_CHECK_EQ(*c.color, std::string("claude-clay"));
                checked = true;
            }
        }
        DOCTEST_CHECK(checked);
        Frame frame(mod.get());
        DOCTEST_CHECK(frame.ui.find("todo.col.4.dot") != nullptr);
    }

    mod->onStop();
    return true;
})

// 三件套纯函数：columnDotColor / overdueDays / filterBySearch
REGISTER_TEST(todo_ui_feature_pure_helpers,
              "todo_ui: columnDotColor/overdueDays/filterBySearch 纯函数", {
    using module::todo::ui::columnDotColor;
    using module::todo::ui::filterBySearch;
    using module::todo::ui::overdueDays;

    // 色点映射：空/未知 token → nullopt；种子 token → hex
    DOCTEST_CHECK(!columnDotColor(std::nullopt).has_value());
    DOCTEST_CHECK(!columnDotColor(std::optional<std::string>("unknown")).has_value());
    const auto clay = columnDotColor(std::optional<std::string>("claude-clay"));
    DOCTEST_REQUIRE(clay.has_value());
    const eui::Color expect("#D97757");
    DOCTEST_CHECK_EQ(clay->r, expect.r);
    DOCTEST_CHECK_EQ(clay->g, expect.g);
    DOCTEST_CHECK_EQ(clay->b, expect.b);
    const auto sage = columnDotColor(std::optional<std::string>("muted-sage"));
    DOCTEST_REQUIRE(sage.has_value());
    const eui::Color sageExpect("#788C5D");
    DOCTEST_CHECK_EQ(sage->r, sageExpect.r);

    // 逾期天数（本地日历日差：昨天到期今天看 = 1 天；当天/未过期 → 0）。
    // 用 mktime 构造本地时间，断言不依赖测试机时区
    const auto localMidnight = [](int daysAgo) -> int64_t {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        core::time::localTime(now, &tm);
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        return std::mktime(&tm) - static_cast<int64_t>(daysAgo) * 86400;
    };
    DOCTEST_CHECK_EQ(overdueDays(100, 100), 0);   // 同一时刻
    DOCTEST_CHECK_EQ(overdueDays(100, 99), 0);    // 未过期
    // 昨天 23:59 到期、今天 00:01 查看 = 1 天（旧秒差向上取整会虚报 2 天）
    DOCTEST_CHECK_EQ(overdueDays(localMidnight(1) + 86399, localMidnight(0) + 60), 1);
    // 3 个日历日前到期 = 3 天；今天更早时刻到期 = 0 天
    DOCTEST_CHECK_EQ(overdueDays(localMidnight(3), localMidnight(0)), 3);
    DOCTEST_CHECK_EQ(overdueDays(localMidnight(0) + 3600, localMidnight(0) + 7200), 0);

    // 搜索过滤：命中 id 集内保留且顺序不变；未激活 → 全保留
    std::vector<module::todo::Task> tasks;
    tasks.push_back(makeTask(1, 10, 0));
    tasks.push_back(makeTask(2, 10, 1));
    tasks.push_back(makeTask(3, 20, 0));
    std::vector<const module::todo::Task*> ptrs;
    ptrs.push_back(&tasks[0]);
    ptrs.push_back(&tasks[1]);
    ptrs.push_back(&tasks[2]);
    std::vector<int64_t> matched;
    matched.push_back(3);
    matched.push_back(2);
    const auto out = filterBySearch(ptrs, matched, true);
    DOCTEST_REQUIRE_EQ(out.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(out[0]->id, static_cast<int64_t>(2));
    DOCTEST_CHECK_EQ(out[1]->id, static_cast<int64_t>(3));
    DOCTEST_CHECK_EQ(filterBySearch(ptrs, matched, false).size(),
                     static_cast<size_t>(3));

    return true;
})

// ---- 6. 产品审计 2026-09-23 [2]：详情弹窗动作先存表单 / 删除确认保留弹窗 / backdrop 守卫 ----

REGISTER_TEST(todo_ui_detail_actions_save_form,
              "todo_ui: 详情动作按钮先存表单 + 删除取消保留弹窗 + backdrop 守卫（审计 [2]）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("编辑列");
    auto col2 = env.svc->createColumn("目标列");
    DOCTEST_REQUIRE(col.isOk() && col2.isOk());
    auto a = env.svc->createTask(col.value().id, "旧标题");
    auto b = env.svc->createTask(col.value().id, "移动前编辑");
    auto c = env.svc->createTask(col.value().id, "删除目标");
    auto e = env.svc->createTask(col.value().id, "遮罩守卫");
    DOCTEST_REQUIRE(a.isOk() && b.isOk() && c.isOk() && e.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 列序：4 个种子列 [0..3] + 编辑列(4) + 目标列(5)；「移动到」按钮按列序编号

    // A. 「标记完成」先落表单：改标题不点保存 → 点标记完成 → 标题已落库 + 已完成
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();  // 打开「旧标题」详情
        Frame f2(mod.get());
        const auto* input = f2.ui.find("todo.dialog.detail.input.title.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        core::TextInputEvent tie;
        tie.text = "NEW";
        input->onTextInput(tie);  // 真实链路：onTextInput → onChange → detailTitle_
        const auto* toggle = f2.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        toggle->onClick();  // 不点保存直接标记完成

        auto got = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got.isOk());
        bool found = false;
        for (const auto& tk : got.value()) {
            if (tk.id == a.unsafeValue().id) {
                found = true;
                DOCTEST_CHECK_EQ(tk.title, std::string("旧标题NEW"));  // 编辑未丢（修复前会丢）
                DOCTEST_CHECK(tk.completedAt.has_value());             // 动作照常执行
            }
        }
        DOCTEST_CHECK(found);
    }

    // B. 「移动到」先落表单：改标题 → 点移动到目标列 → 标题落库 + 列变更
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");  // 旧标题已完成移走
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
        Frame f2(mod.get());
        const auto* input = f2.ui.find("todo.dialog.detail.input.title.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        core::TextInputEvent tie;
        tie.text = "M";
        input->onTextInput(tie);
        const auto* moveBtn = f2.ui.find("todo.dialog.detail.move.5.bg");  // 目标列
        DOCTEST_REQUIRE(moveBtn != nullptr);
        DOCTEST_REQUIRE(moveBtn->onClick != nullptr);
        moveBtn->onClick();

        auto got = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got.isOk());
        for (const auto& tk : got.value()) {
            if (tk.id == b.unsafeValue().id) {
                DOCTEST_CHECK_EQ(tk.title, std::string("移动前编辑M"));
                DOCTEST_CHECK_EQ(tk.columnId, col2.unsafeValue().id);
            }
        }
    }

    // C. 详情内删除：确认取消后回到详情弹窗（编辑现场保留）；确认删除才落库关窗
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");  // 删除目标
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
        Frame f2(mod.get());
        const auto* del = f2.ui.find("todo.dialog.detail.delete.bg");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        del->onClick();  // 只开确认；详情不再被关掉（修复前 detailOpen_ 先关）
        Frame f3(mod.get());
        DOCTEST_CHECK_EQ(f3.ui.find("todo.dialog.delcol.panel")->opacity, 1.0f);
        DOCTEST_CHECK_EQ(f3.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);
        const auto* cancel = f3.ui.find("todo.dialog.delcol.cancel.bg");
        DOCTEST_REQUIRE(cancel != nullptr);
        DOCTEST_REQUIRE(cancel->onClick != nullptr);
        cancel->onClick();
        Frame f4(mod.get());
        DOCTEST_CHECK_EQ(f4.ui.find("todo.dialog.delcol.panel")->opacity, 0.0f);
        DOCTEST_CHECK_EQ(f4.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);  // 回到弹窗
        bool stillThere = false;
        auto got1 = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got1.isOk());
        for (const auto& tk : got1.value()) {
            if (tk.id == c.unsafeValue().id) stillThere = true;
        }
        DOCTEST_CHECK(stillThere);  // 取消 = 什么都没发生
        // 再删 → 确认 → 任务消失 + 详情关闭
        const auto* del2 = f4.ui.find("todo.dialog.detail.delete.bg");
        DOCTEST_REQUIRE(del2 != nullptr);
        DOCTEST_REQUIRE(del2->onClick != nullptr);
        del2->onClick();
        Frame f5(mod.get());
        const auto* confirm = f5.ui.find("todo.dialog.delcol.confirm.bg");
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_REQUIRE(confirm->onClick != nullptr);
        confirm->onClick();
        Frame f6(mod.get());
        DOCTEST_CHECK_EQ(f6.ui.find("todo.dialog.detail.panel")->opacity, 0.0f);
        bool gone = true;
        auto got2 = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got2.isOk());
        for (const auto& tk : got2.value()) {
            if (tk.id == c.unsafeValue().id) gone = false;
        }
        DOCTEST_CHECK(gone);
    }

    // D. backdrop 守卫：有未存改动第一次点遮罩只警告不关不落库，再点一次放弃关闭
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");  // 遮罩守卫
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
        Frame f2(mod.get());
        const auto* input = f2.ui.find("todo.dialog.detail.input.title.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        core::TextInputEvent tie;
        tie.text = "G";
        input->onTextInput(tie);
        const auto* backdrop = f2.ui.find("todo.dialog.detail.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_REQUIRE(backdrop->onClick != nullptr);
        backdrop->onClick();  // 第一次：脏表单 → 守卫只警告
        Frame f3(mod.get());
        DOCTEST_CHECK_EQ(f3.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);  // 未关
        bool dbUnchanged = false;
        auto got = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got.isOk());
        for (const auto& tk : got.value()) {
            if (tk.id == e.unsafeValue().id && tk.title == std::string("遮罩守卫")) {
                dbUnchanged = true;
            }
        }
        DOCTEST_CHECK(dbUnchanged);  // 警告点击不落库
        const auto* backdrop2 = f3.ui.find("todo.dialog.detail.backdrop");
        DOCTEST_REQUIRE(backdrop2 != nullptr);
        DOCTEST_REQUIRE(backdrop2->onClick != nullptr);
        backdrop2->onClick();  // 第二次：明确放弃修改 → 关闭
        Frame f4(mod.get());
        DOCTEST_CHECK_EQ(f4.ui.find("todo.dialog.detail.panel")->opacity, 0.0f);
        bool discarded = false;
        auto got2 = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got2.isOk());
        for (const auto& tk : got2.value()) {
            if (tk.id == e.unsafeValue().id && tk.title == std::string("遮罩守卫")) {
                discarded = true;  // 放弃语义：改动不入库
            }
        }
        DOCTEST_CHECK(discarded);
    }

    mod->onStop();
    return true;
})

// ---- 审计 [24]：「+ 列」命名弹窗 + 列头 ✎ 重命名入口（headless） ----

// 旧行为：点「+ 列」直接 doCreateColumn("新列 N") 即时落库。新行为：先弹
// 命名弹窗，确认才落库；Esc（输入框内 passthrough 与 app 级兜底两路）取消
// 不落库；留空确认采用默认名；输入名字按名字建。改名入口显性化为列头 ✎。
REGISTER_TEST(todo_ui_new_column_dialog,
              "todo_ui: + 列命名弹窗创建/Esc 取消/列头 ✎ 重命名（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    Frame frame(mod.get());
    auto colCount = [&](void) -> size_t {
        auto l = env.svc->listColumns();
        return l.isOk() ? l.value().size() : 0;
    };
    auto findTitle = [&](const std::string& want) -> bool {
        auto l = env.svc->listColumns();
        if (!l.isOk()) return false;
        for (const auto& c : l.value()) {
            if (c.title == want) return true;
        }
        return false;
    };

    const size_t seed = colCount();   // 迁移默认 4 列
    DOCTEST_REQUIRE_EQ(seed, 4u);

    // 1) 点「+ 列」→ 只开弹窗，不即时落库（[24] 核心回归）
    auto* addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    DOCTEST_REQUIRE(addcol->onClick != nullptr);
    addcol->onClick();
    frame.refresh();
    DOCTEST_REQUIRE_EQ(colCount(), seed);

    // 2) 留空点「创建」→ 采用默认名「新列 5」，弹窗关闭
    auto* confirm = frame.ui.find("todo.dialog.newcol.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_REQUIRE(confirm->onClick != nullptr);
    confirm->onClick();
    frame.refresh();
    DOCTEST_REQUIRE_EQ(colCount(), seed + 1);
    DOCTEST_CHECK(findTitle("新列 5"));

    // 3) Esc（焦点在输入框：passthrough 拦内置 Escape→onEnter 提交）→ 取消不落库
    //    若 passthrough 被删，内置会把 Esc 当提交并建出新列 → colCount 变
    //    注意：refresh 重建元素树，所有旧 Element* 悬垂——每次点击前重新 find
    addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    addcol->onClick();
    frame.refresh();
    auto* hit = frame.ui.find("todo.dialog.newcol.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK(hit->onKeyEvent(esc));    // 已消费
    frame.refresh();
    DOCTEST_REQUIRE_EQ(colCount(), seed + 1);   // 没有多建

    // 3b) Esc（输入框未聚焦的 app 级兜底：Module::onKeyEvent）→ 取消不落库
    addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    addcol->onClick();
    frame.refresh();
    DOCTEST_CHECK(mod->onKeyEvent(esc));
    frame.refresh();
    DOCTEST_REQUIRE_EQ(colCount(), seed + 1);

    // 3c) 「取消」按钮 → 不落库
    addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    addcol->onClick();
    frame.refresh();
    auto* cancel = frame.ui.find("todo.dialog.newcol.cancel.bg");
    DOCTEST_REQUIRE(cancel != nullptr);
    DOCTEST_REQUIRE(cancel->onClick != nullptr);
    cancel->onClick();
    frame.refresh();
    DOCTEST_REQUIRE_EQ(colCount(), seed + 1);

    // 4) 输入名字再创建：onTextInput 写回 → 「创建」按输入名落库
    addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    addcol->onClick();
    frame.refresh();
    hit = frame.ui.find("todo.dialog.newcol.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onTextInput != nullptr);
    eui::TextInputEvent tie;
    tie.text = "迭代列";
    hit->onTextInput(tie);
    frame.refresh();
    confirm = frame.ui.find("todo.dialog.newcol.confirm.bg");
    DOCTEST_REQUIRE(confirm != nullptr);
    confirm->onClick();
    frame.refresh();
    DOCTEST_REQUIRE_EQ(colCount(), seed + 2);
    DOCTEST_CHECK(findTitle("迭代列"));
    DOCTEST_CHECK(!findTitle("新列 6"));   // 输入名优先于默认名

    // 5) 列头 ✎ 按钮存在且接重命名弹窗：点开 → 输入框 Ctrl+A 全选 →
    //    打新标题 → 保存 → 落库改名（入口显性化的端到端链路）
    auto* pen = frame.ui.find("todo.col.0.rename.bg");
    DOCTEST_REQUIRE(pen != nullptr);
    DOCTEST_REQUIRE(pen->onClick != nullptr);
    pen->onClick();
    frame.refresh();
    auto* rhit = frame.ui.find("todo.dialog.rename.input.hit");
    DOCTEST_REQUIRE(rhit != nullptr);
    DOCTEST_REQUIRE(rhit->onKeyEvent != nullptr);
    DOCTEST_REQUIRE(rhit->onTextInput != nullptr);
    eui::KeyEvent selectAll;
    selectAll.key = eui::InputKey::A;
    selectAll.action = eui::KeyAction::Press;
    selectAll.modifiers.control = true;
    rhit->onKeyEvent(selectAll);          // 内置 Ctrl+A 全选
    eui::TextInputEvent rtie;
    rtie.text = "列头改名";
    rhit->onTextInput(rtie);              // 打字典换选中内容 → onChange 写回
    frame.refresh();
    auto* save = frame.ui.find("todo.dialog.rename.confirm.bg");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_REQUIRE(save->onClick != nullptr);
    save->onClick();
    frame.refresh();
    DOCTEST_CHECK(findTitle("列头改名"));   // ✎ → 弹窗 → 保存 全链路
    return true;
})
