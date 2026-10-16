// tests/test_modules/test_todo_ui.cpp
// Todo UI 单测：键盘移动与日期换算纯函数 / 无头 compose 冒烟与回调交互 / 拖放条几何 /
// 优先级色底 / 标签字号 / 列表头重命名与新建的重名与空名校验。
// 跨文件前提：
//   - 无头环境下没有真实输入与焦点，键盘移动与菜单点击只能靠直接调回调 + 断言落库结果。
//   - 拖放条是覆盖层不是流式内容：悬停行下方的占位条必须压在两卡之间的底缝，卡片不得被下推。
#include "doctest.h"
#include "test_support.h"

#include "core/database.h"
#include "core/event_bus.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/metrics.h"  // kHeaderH：看板几何断言基准高

#include <eui/dsl.h>
#include <eui/types.h>
#include <components/input.h>  // InputState：程序赋值路径须显式置光标
#include "core/input/input_types.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_todo_module();

namespace {

// 临时 db 文件名前缀（共享工厂 test_support::TestEnv 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_todoui_";

// 优先级色底 / 标签字号断言

// 颜色近似相等（eui::Color 无 operator==；tint 断言不比位、比分量差）
bool colorNear(const eui::Color& a, const eui::Color& b, float eps = 0.02f) {
    return std::fabs(a.r - b.r) <= eps && std::fabs(a.g - b.g) <= eps &&
           std::fabs(a.b - b.b) <= eps && std::fabs(a.a - b.a) <= eps;
}

// 单通道最大差（0..1）：判「两色看得出不同」「低优先级 tint 有多浅」
float maxChannelDelta(const eui::Color& a, const eui::Color& b) {
    return std::max({std::fabs(a.r - b.r), std::fabs(a.g - b.g),
                     std::fabs(a.b - b.b)});
}

// WCAG 相对亮度（sRGB 线性化）
float relLuminance(const eui::Color& c) {
    auto channel = [](float v) {
        return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * channel(c.r) + 0.7152f * channel(c.g) +
           0.0722f * channel(c.b);
}

// WCAG 对比度（1..21）：文字底色改 tint 后「读不读得清」的数值判据（AA = 4.5）
float contrastRatio(const eui::Color& a, const eui::Color& b) {
    const float la = relLuminance(a);
    const float lb = relLuminance(b);
    const float hi = std::max(la, lb);
    const float lo = std::min(la, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

// RAII：临时库（共享 test_support::TestEnv；注册全局 appDatabase，todo 模块
// onStart 依赖，析构时一定解除注册防悬垂污染后续用例）+ TodoService
// 模块 glue，keep-local：共享 TestEnv 基座 + TodoService glue。
struct TestEnv : test_support::TestEnv {
    std::unique_ptr<module::todo::TodoService> svc;

    // 视图状态存在 app_config 表的一行（键 todo_view_state），基类注册的临时库
    // 让无头用例不碰真实数据根。不要再加回 DEVDESK_TODO_VIEW_STATE_PATH 之类的
    // 文件缝：视图状态是库不是文件，两处并存必然漂移。
    TestEnv() : test_support::TestEnv(kDbPrefix, "", true) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
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

// 构造 Ctrl+方向 键事件（修饰位按平台取：macOS = Cmd，其余 = Ctrl）
eui::KeyEvent ctrlArrow(eui::InputKey key) {
    eui::KeyEvent e;
    e.key = key;
    e.action = eui::KeyAction::Press;
    test_support::setShortcutModifier(e.modifiers);
    return e;
}

} // namespace

// computeKeyboardMove 纯函数

REGISTER_TEST(todo_ui_move_logic,
              "todo_ui: computeKeyboardMove 上移/下移/边界/空洞", {
    using module::todo::ui::computeKeyboardMove;
    std::vector<module::todo::Task> tasks = {
        makeTask(1, 10, 0),
        makeTask(2, 10, 1),
        makeTask(3, 10, 2),
        makeTask(4, 20, 0),   // 另一列，不应干扰
    };

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
    std::vector<module::todo::Task> gappy = {
        makeTask(11, 30, 0),
        makeTask(12, 30, 2),
        makeTask(13, 30, 5),
    };
    auto gUp = computeKeyboardMove(gappy, 12, true);
    DOCTEST_REQUIRE(gUp.has_value());
    DOCTEST_CHECK_EQ(gUp->position, 0);           // 挪到 pos 0 任务之前
    auto gDown = computeKeyboardMove(gappy, 12, false);
    DOCTEST_REQUIRE(gDown.has_value());
    DOCTEST_CHECK_EQ(gDown->position, 6);         // pos 5 任务之后
    return true;
})

// 日期换算纯函数

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

// 无头 compose：卡片渲染 + 右键菜单 + 键盘移动 + datepicker

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

// computeAppendPosition

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

// computeInsertPosition 拖拽按行插入（rowIndex 越界 = 追加）
REGISTER_TEST(todo_ui_insert_position,
              "todo_ui: computeInsertPosition 行插入/越界追加", {
    using module::todo::ui::computeInsertPosition;
    using module::todo::ui::computeAppendPosition;
    std::vector<module::todo::Task> tasks;
    tasks.push_back(makeTask(1, 10, 0));
    tasks.push_back(makeTask(2, 10, 2));   // 空洞：position 1 不存在
    tasks.push_back(makeTask(3, 10, 5));
    tasks.push_back(makeTask(4, 20, 9));   // 另一列，不应干扰
    // 行 0 = 插到首任务前（用其真实 position，即使有空洞）
    DOCTEST_CHECK_EQ(computeInsertPosition(tasks, 10, 0), 0);
    // 行 1 = 插到第 2 张卡（id=2, position=2）之前
    DOCTEST_CHECK_EQ(computeInsertPosition(tasks, 10, 1), 2);
    // 行 2 = 插到第 3 张卡（id=3, position=5）之前
    DOCTEST_CHECK_EQ(computeInsertPosition(tasks, 10, 2), 5);
    // 行 == 列卡数 或越界 → 追加（同 computeAppendPosition）
    DOCTEST_CHECK_EQ(computeInsertPosition(tasks, 10, 3),
                     computeAppendPosition(tasks, 10));
    DOCTEST_CHECK_EQ(computeInsertPosition(tasks, 10, 9), 6);
    // 空列 → 0
    DOCTEST_CHECK_EQ(computeInsertPosition(tasks, 30, 0), 0);
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
    // 丙仍在列尾且 position 保留：保存时硬编码 position=0 会把它顶到列首
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
    // Element::frame（LayoutRect）→ 回调参数的 Rect
    auto boundsOf = [](const core::dsl::Element* e) {
        return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
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
        // 目标列出现插入位占位条（按悬停行）
        DOCTEST_CHECK(ui.find("todo.col.5.dropbar") != nullptr);

        // release（PointerAction::Release）→ moveTask 落库
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        ui.find("todo.col.4.card.0.bg")->onRelease(up, eui::Rect{});
        composeLayout();
        // 拖拽结束后视觉复位
        DOCTEST_CHECK_EQ(ui.find("todo.col.5.bg")->border.width, 1.0f);
        DOCTEST_CHECK(ui.find("todo.col.5.dropbar") == nullptr);  // 占位条随拖拽结束消失
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
        card->onRelease(up, eui::Rect{});  // 同一帧内 press→drag→release，元素指针有效
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

        // 拖出又拖回松手 → 落回源列 = 取消，详情弹窗不因松手打开。
        // 模块不得在 onRelease 里自开详情。
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

// 拖拽按悬停行插入（占位条预览 + 落库位置一致，headless）
// 列布局：[0..3] 种子列，4=插入源列，5=插入目标列
REGISTER_TEST(todo_ui_drag_insert_position,
              "todo_ui: 拖拽插入目标行（占位条 + 按行落库）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto src = env.svc->createColumn("插入源列");
    auto dst = env.svc->createColumn("插入目标列");
    DOCTEST_REQUIRE(src.isOk());
    DOCTEST_REQUIRE(dst.isOk());
    auto x1 = env.svc->createTask(dst.value().id, "甲");
    auto x2 = env.svc->createTask(dst.value().id, "乙");
    auto mover = env.svc->createTask(src.value().id, "被移动任务");
    auto mover2 = env.svc->createTask(src.value().id, "再移动任务");
    DOCTEST_REQUIRE(x1.isOk() && x2.isOk() && mover.isOk() && mover2.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto composeLayout = [&]() {
        ui.begin("todo_drag_insert_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    auto boundsOf = [](const core::dsl::Element* e) {
        return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
    };

    // 场景 1：拖到目标列内容区**顶部**（列头下方 = 首卡前）松手 → 插到 甲 之前
    {
        composeLayout();
        const auto* card = ui.find("todo.col.4.card.0.bg");  // 被移动任务
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        const auto* dstBg = ui.find("todo.col.5.bg");
        DOCTEST_REQUIRE(dstBg != nullptr);
        core::dsl::DragEvent ev;
        ev.x = dstBg->frame.x + dstBg->frame.width / 2.0f;
        ev.y = dstBg->frame.y + 56.0f + 4.0f;  // 列头下方 → 行 0（首卡前）
        card->onDrag(ev);
        composeLayout();
        // 占位条走 ignoreLayout 跳流式，frame 为绝对坐标（板面顶边是页面值，
        // 不写死）：row 0 → 条贴在卡 0 顶。
        // 若把占位条改回流式布局，条会贴内容顶、整列卡片被下推 16px、松手弹回：
        // position 在 column 流式里不生效，必须保持 overlay x/y 绝对定位。
        const auto* dropBar = ui.find("todo.col.5.dropbar");
        DOCTEST_REQUIRE(dropBar != nullptr);  // 占位条在目标列
        const auto* card0Wrap = ui.find("todo.col.5.card.0.wrap");
        DOCTEST_REQUIRE(card0Wrap != nullptr);
        DOCTEST_CHECK_EQ(dropBar->frame.y, card0Wrap->frame.y);  // row 0 首卡位
        // 不推流式：卡 0 顶 == 列顶 + 列头高（旧缺陷 = 再 +16）
        DOCTEST_CHECK_EQ(card0Wrap->frame.y,
                         ui.find("todo.col.5.bg")->frame.y + 56.0f);
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        // 中间插了 mid-drag composeLayout（重建元素树）——release 必须重新
        // find，旧指针的回调被 move 走、直调会 bad_function_call
        const auto* rel = ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(3));
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("被移动任务"));  // 插到行 0
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("甲"));
        DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("乙"));
    }

    // 场景 2：源列剩「再移动任务」→ 拖到目标列**底部**（卡 2 下缝附近）→ 追加
    {
        composeLayout();
        const auto* card = ui.find("todo.col.4.card.0.bg");  // 再移动任务
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        const auto* dstBg = ui.find("todo.col.5.bg");
        DOCTEST_REQUIRE(dstBg != nullptr);
        core::dsl::DragEvent ev;
        ev.x = dstBg->frame.x + dstBg->frame.width / 2.0f;
        // 卡 2 底 = 缝中心（内容坐标 2*92+88 = 272）
        ev.y = dstBg->frame.y + 56.0f + 272.0f;
        card->onDrag(ev);
        composeLayout();
        // 悬停行 3（列尾追加）→ 条在卡 2 底缝，卡片未被流式下推。
        // 不可改成流式布局：那样条会被压到内容顶（= 卡 0 顶），与悬停行完全对不上。
        const auto* dropBar = ui.find("todo.col.5.dropbar");
        DOCTEST_REQUIRE(dropBar != nullptr);  // 尾部占位条
        const auto* card1Wrap = ui.find("todo.col.5.card.1.wrap");
        DOCTEST_REQUIRE(card1Wrap != nullptr);
        const auto* card2Wrap = ui.find("todo.col.5.card.2.wrap");
        DOCTEST_REQUIRE(card2Wrap != nullptr);
        // 条在卡 2 顶前 8px 缝内（3*92-8 内容位，绝对坐标 = 列顶+56+268）
        DOCTEST_CHECK_EQ(dropBar->frame.y, card2Wrap->frame.y + 84.0f);
        DOCTEST_CHECK_EQ(dropBar->frame.y,
                         ui.find("todo.col.5.bg")->frame.y + 56.0f + 268.0f);
        // 不推流式：卡 1 顶 = 列内容区顶 + 92
        DOCTEST_CHECK_EQ(card1Wrap->frame.y,
                         ui.find("todo.col.5.bg")->frame.y + 56.0f + 92.0f);
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        // mid-drag composeLayout 重建元素树——release 前重新 find
        const auto* rel = ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(4));
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("被移动任务"));
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("甲"));
        DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("乙"));
        DOCTEST_CHECK_EQ(ts.value()[3].title, std::string("再移动任务"));  // 追加列尾
    }

    // 场景 3：同列拖拽受支持——悬停行照常预览（占位条进源列），落点仍在自身
    // 缝位（row 2 = 甲 与 乙 之间的缝，含被拖卡自身的行序）→ 换算后插回原位
    // = no-op：顺序不变、不落库。
    // 不要退回「同列拖拽 = 取消、不预览」：那会让占位条这条路径失去覆盖。
    {
        composeLayout();
        const auto* card = ui.find("todo.col.5.card.1.bg");  // 甲（同列拖）
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        const auto* srcBg = ui.find("todo.col.5.bg");
        DOCTEST_REQUIRE(srcBg != nullptr);
        core::dsl::DragEvent ev;
        ev.x = srcBg->frame.x + 40.0f;
        ev.y = srcBg->frame.y + 56.0f + 190.0f;  // relY=190 ∈ 卡 2 区间 → row 2
        card->onDrag(ev);
        composeLayout();
        const auto* dropBar = ui.find("todo.col.5.dropbar");
        DOCTEST_REQUIRE(dropBar != nullptr);  // 同列 = 重排：源列也预览
        const auto* card2Wrap = ui.find("todo.col.5.card.2.wrap");
        DOCTEST_REQUIRE(card2Wrap != nullptr);
        DOCTEST_CHECK_EQ(dropBar->frame.y, card2Wrap->frame.y - 8.0f);  // 落在缝里
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        // mid-drag composeLayout 重建元素树——release 前重新 find（源列已空，
        // 不能按 col.4 找；任何卡的 release 回调都是模块级处理，落点坐标定成败）
        const auto* rel = ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(4));  // 未动
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("甲"));  // 插回原位 = no-op
    }

    // 场景 4：同列拖到列尾 → 甲 移到最末
    // 列序此刻 = [被移动任务, 甲, 乙, 再移动任务]；甲 ownIdx=1，悬停行 4
    //（relY ≥ 3*92+84 = 360 → 钳到 taskCount=4）→ 换算行 3（摘出自身后
    // 的列尾）→ 落库后 = [被移动任务, 乙, 再移动任务, 甲]
    {
        composeLayout();
        const auto* card = ui.find("todo.col.5.card.1.bg");  // 甲
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        const auto* srcBg = ui.find("todo.col.5.bg");
        DOCTEST_REQUIRE(srcBg != nullptr);
        core::dsl::DragEvent ev;
        ev.x = srcBg->frame.x + 40.0f;
        ev.y = srcBg->frame.y + 56.0f + 400.0f;  // 列尾空白区 → row 4（追加位）
        card->onDrag(ev);
        composeLayout();
        const auto* dropBar = ui.find("todo.col.5.dropbar");
        const auto* card3Wrap = ui.find("todo.col.5.card.3.wrap");
        DOCTEST_REQUIRE(dropBar != nullptr);
        DOCTEST_REQUIRE(card3Wrap != nullptr);
        DOCTEST_CHECK_EQ(dropBar->frame.y, card3Wrap->frame.y + 84.0f);  // 末卡底缝
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        const auto* rel = ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(4));
        DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("被移动任务"));
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("乙"));
        DOCTEST_CHECK_EQ(ts.value()[2].title, std::string("再移动任务"));
        DOCTEST_CHECK_EQ(ts.value()[3].title, std::string("甲"));  // 同列重排真落库
    }

    // 场景 5：同列拖拽但指针在**列外**（列间隙）松手 → target = -1 → 取消
    //（「拖出列外松手 = 取消」语义保持；列外也不预览占位条）
    // 列序此刻 = [被移动任务, 乙, 再移动任务, 甲]；乙 ownIdx=1
    {
        composeLayout();
        const auto* card = ui.find("todo.col.5.card.1.bg");  // 乙
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        const auto* srcBg = ui.find("todo.col.5.bg");
        DOCTEST_REQUIRE(srcBg != nullptr);
        core::dsl::DragEvent ev;
        ev.x = srcBg->frame.x - 8.0f;  // 列 4 与列 5 之间的列间隙
        ev.y = srcBg->frame.y + 56.0f + 400.0f;
        card->onDrag(ev);
        composeLayout();
        DOCTEST_CHECK(ui.find("todo.col.5.dropbar") == nullptr);  // 列外不预览
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        const auto* rel = ui.find("todo.col.5.card.1.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }
    {
        auto ts = env.svc->listTasks(dst.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(4));  // 未动
        DOCTEST_CHECK_EQ(ts.value()[1].title, std::string("乙"));
    }

    mod->onStop();
    return true;
})

// 列内卡片边距（卡左缘 = 列左缘 + 16，两侧同源）+ 拖拽占位条与卡槽同宽同位
//（条不再右边留空）
// 窄列口径：16px inset 两侧各一份 ⇒ 卡宽 = 内容宽 - 32（contentCardWidth），
// 8 列最小窗（colW 126）时 = 94，仍 > 0 且 chip 自适应不越出卡右缘。
REGISTER_TEST(todo_ui_card_inset_and_dropbar_width,
              "todo_ui: 卡片左边距/占位条同源宽度（缺陷 1/2 几何回归）", {
    using module::todo::ui::contentCardWidth;
    using module::todo::ui::kColumnContentInset;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto src = env.svc->createColumn("边距源列");
    auto dst = env.svc->createColumn("边距目标列");
    DOCTEST_REQUIRE(src.isOk() && dst.isOk());
    // 再造 2 列凑满 8 列（最小窗 1200 下 colW = (1120-7*16)/8 = 126 的窄列口径）
    DOCTEST_REQUIRE(env.svc->createColumn("窄列甲").isOk());
    DOCTEST_REQUIRE(env.svc->createColumn("窄列乙").isOk());
    auto mover = env.svc->createTask(src.value().id, "被拖卡");
    auto t1 = env.svc->createTask(dst.value().id, "甲");
    auto t2 = env.svc->createTask(dst.value().id, "乙");
    DOCTEST_REQUIRE(mover.isOk() && t1.isOk() && t2.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto composeLayout = [&]() {
        ui.begin("todo_card_inset_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    auto boundsOf = [](const core::dsl::Element* e) {
        return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
    };

    composeLayout();
    const auto* col5 = ui.find("todo.col.5.bg");
    const auto* card0 = ui.find("todo.col.5.card.0.bg");
    const auto* wrap0 = ui.find("todo.col.5.card.0.wrap");
    DOCTEST_REQUIRE(col5 != nullptr);
    DOCTEST_REQUIRE(card0 != nullptr);
    DOCTEST_REQUIRE(wrap0 != nullptr);
    // (1) 左内边距：卡左缘 - 列左缘 == kColumnContentInset（缺陷形态 = 0）
    DOCTEST_CHECK_EQ(card0->frame.x - col5->frame.x, kColumnContentInset);
    // (2) 右内边距 == 左内边距（非滚动列：内容宽 == 列宽，两侧同源派生）。
    // 旧形态 = 左 0 / 右 16（宽度写死「内容宽 - 16」而 x 恒 0）
    const float rightGap =
        (col5->frame.x + col5->frame.width) - (card0->frame.x + card0->frame.width);
    DOCTEST_CHECK_EQ(rightGap, kColumnContentInset);
    DOCTEST_CHECK_EQ(rightGap, card0->frame.x - col5->frame.x);
    // (3) 卡宽 == contentCardWidth(列宽)（唯一来源；不是「列宽 - 16」）
    DOCTEST_CHECK_EQ(card0->frame.width, contentCardWidth(col5->frame.width));
    // (4) wrap 仍占满内容宽（流式子元素，卡在 wrap 内落位）：右缘对齐列右缘
    DOCTEST_CHECK_EQ(wrap0->frame.x, col5->frame.x);
    DOCTEST_CHECK_EQ(wrap0->frame.width, col5->frame.width);

    // (5) 占位条：拖到目标列 → 条与卡槽同宽同位（缺陷 2：条右边留空 = 条宽
    // 写「内容宽 - 16」且 x=0，与它覆盖的卡槽不同源）
    {
        const auto* card = ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        card->onPress(pe, boundsOf(card));
        core::dsl::DragEvent ev;
        ev.x = col5->frame.x + col5->frame.width / 2.0f;
        ev.y = col5->frame.y + 56.0f + 4.0f;  // 行 0
        card->onDrag(ev);
        composeLayout();
        const auto* bar = ui.find("todo.col.5.dropbar");
        const auto* col5After = ui.find("todo.col.5.bg");
        const auto* card0After = ui.find("todo.col.5.card.0.bg");
        DOCTEST_REQUIRE(bar != nullptr);
        DOCTEST_REQUIRE(col5After != nullptr);
        DOCTEST_REQUIRE(card0After != nullptr);
        DOCTEST_CHECK_EQ(bar->frame.x, col5After->frame.x + kColumnContentInset);
        DOCTEST_CHECK_EQ(bar->frame.width, contentCardWidth(col5After->frame.width));
        // 与卡槽逐值同宽同位（同源判据：条覆盖的就是卡要落的位置）
        DOCTEST_CHECK_EQ(bar->frame.x, card0After->frame.x);
        DOCTEST_CHECK_EQ(bar->frame.width, card0After->frame.width);
        // 右边距对称：条右缘到列右缘 == kColumnContentInset（旧形态 = 16 但
        // 左缘贴列边 → 视觉"右边有空余"）
        DOCTEST_CHECK_EQ(
            (col5After->frame.x + col5After->frame.width) -
                (bar->frame.x + bar->frame.width),
            kColumnContentInset);
        eui::PointerEvent up;
        up.x = ev.x;
        up.y = ev.y;
        up.action = eui::PointerAction::Release;
        const auto* rel = ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }

    // (6) 窄列（8 列 @1200 最小窗，colW=126）不溢出：卡宽 = 126-32 = 94 > 0，
    // 卡右缘 ≤ 列右缘 - 16；占位条同宽同位
    {
        eui::Screen narrow;
        narrow.width = 1200.0f;
        narrow.height = 720.0f;
        ui.begin("todo_card_inset_narrow");
        mod->compose(ui, narrow);
        ui.end();
        ui.layout(narrow);
        const auto* colN = ui.find("todo.col.5.bg");
        const auto* cardN = ui.find("todo.col.5.card.0.bg");
        DOCTEST_REQUIRE(colN != nullptr);
        DOCTEST_REQUIRE(cardN != nullptr);
        DOCTEST_CHECK_EQ(cardN->frame.x - colN->frame.x, kColumnContentInset);
        DOCTEST_CHECK(cardN->frame.width >= 0.0f);
        DOCTEST_CHECK(cardN->frame.x + cardN->frame.width <=
                      colN->frame.x + colN->frame.width);
        DOCTEST_CHECK_EQ(cardN->frame.width, contentCardWidth(colN->frame.width));
    }

    mod->onStop();
    return true;
})

// 卡内文本居中
// 三种卡形态全覆盖：纯标题（组 = 标题独占整卡居中）/ 有截止 chip / 有 tags
// chip（组 = 标题行 + chip 行整块居中）。断言口径：
//   水平 = 标题框中心与卡中心偏差 ≤1px + horizontalAlign Center 机制位在位
//   （框对称只保证框居中，字形居中靠该位，二者缺一即红）；
//   垂直 = 内容组 [顶, 底] 中点与卡内容区中心偏差 ≤1px，组顶与 todo_ui_logic.h
//   的 cardContentGroupTop 同源（居中算式没被旁路）；
//   chip 行 x 不拽歪（仍 = 卡左缘 + 16）、组内排版保持（标题行底 → chip 顶 =
//   kCardTitleToChipGap）。
// 变异自检：把 drawCard 的 groupTop / 标题 x 改回左上角固定原点（14 / 16，
// 无 horizontalAlign）→ 本用例水平与垂直断言同时变红（记录见提交说明）。
REGISTER_TEST(todo_ui_card_text_centered,
              "todo_ui: 卡内文本居中——内容组中心=卡内容区中心 ≤1px（headless）", {
    using module::todo::ui::cardContentGroupTop;
    using module::todo::ui::kCardTitleToChipGap;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("居中列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "纯标题卡");
    auto b = env.svc->createTask(col.value().id, "带截止卡");
    auto c = env.svc->createTask(col.value().id, "带标签卡");
    DOCTEST_REQUIRE(a.isOk() && b.isOk() && c.isOk());
    {
        module::todo::Task t2 = b.value();
        t2.dueAt = 2000000000;  // 未来时间，未逾期
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }
    {
        module::todo::Task t3 = c.value();
        t3.tags = {"重要", "前端"};
        DOCTEST_REQUIRE(env.svc->updateTask(t3).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());
    frame.ui.layout(1600.0f, 1000.0f);

    const char* cardIds[3] = {"todo.col.4.card.0", "todo.col.4.card.1",
                              "todo.col.4.card.2"};
    const bool expectChipRow[3] = {false, true, true};
    const char* chipIds[3] = {"", ".due.bg", ".tag.0.bg"};
    for (int i = 0; i < 3; ++i) {
        const std::string base = cardIds[i];
        const auto* cardBg = frame.ui.find(base + ".bg");
        const auto* title = frame.ui.find(base + ".title");
        DOCTEST_REQUIRE(cardBg != nullptr);
        DOCTEST_REQUIRE(title != nullptr);
        const float cardCX = cardBg->frame.x + cardBg->frame.width / 2.0f;
        const float cardCY = cardBg->frame.y + cardBg->frame.height / 2.0f;

        // 水平：主文本（标题）框中心 = 卡中心（≤1px），字形居中机制位在位
        DOCTEST_CHECK_EQ(title->horizontalAlign, eui::HorizontalAlign::Center);
        DOCTEST_CHECK(std::abs((title->frame.x + title->frame.width / 2.0f) -
                               cardCX) <= 1.0f);
        // 水平让位（kCardTitleBoxInset = 40）：标题框右缘必须让开完成角标区间
        // [w-36, w-12]（drawCard 的 .done 角标 x = width-36），否则标题文字压角标
        DOCTEST_CHECK(title->frame.x + title->frame.width <=
                      cardBg->frame.x + cardBg->frame.width - 36.0f);

        // 垂直：内容组 [顶, 底] 中点 = 卡内容区中心（≤1px）。
        // 组顶 = 标题顶；组底 = chip 底（有 chip 行）或标题底（纯标题卡）
        const float groupTop = title->frame.y;
        float groupBottom = title->frame.y + title->frame.height;
        if (expectChipRow[i]) {
            const auto* chipBg = frame.ui.find(base + chipIds[i]);
            DOCTEST_REQUIRE(chipBg != nullptr);
            // 组内排版保持：标题行底 → chip 行顶 = 原间距 12
            DOCTEST_CHECK_EQ(chipBg->frame.y - (title->frame.y + title->frame.height),
                             kCardTitleToChipGap);
            // chip 行水平不拽歪：x 仍 = 卡左缘 + 16（卡内左内边距）
            DOCTEST_CHECK_EQ(chipBg->frame.x - cardBg->frame.x, 16.0f);
            groupBottom = chipBg->frame.y + chipBg->frame.height;
        }
        DOCTEST_CHECK(std::abs(((groupTop + groupBottom) / 2.0f) - cardCY) <= 1.0f);
        // 组顶与同源常量一致（居中算式本身没被旁路 / 写死）
        DOCTEST_CHECK_EQ(groupTop - cardBg->frame.y,
                         cardContentGroupTop(expectChipRow[i]));
    }

    mod->onStop();
    return true;
})

// 卡内 chip 胶囊（drawChip）文本垂直居中
// drawChip（todo_ui_common.h，卡片优先级/截止/标签 pill 与详情弹窗标签区共用）
// 的文本盒与胶囊底同 y 同高（kChipHeight）、lineHeight 也 == kChipHeight，缺
// VerticalAlign::Center 时 runtime 把 ink 从行框顶沿起排（runtime_render.h:1068
// y=frame.y）→ 胶囊里的字看着偏上。
// 变异自检：删 drawChip 里的 .verticalAlign(Center) → 本用例 verticalAlign
// 断言红（frame 数值不变，只有渲染锚点回 frame 顶）。
REGISTER_TEST(todo_ui_chip_text_centered,
              "todo_ui: 卡内 chip 文字垂直居中（锚点 = 胶囊底中心）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("chip列");
    DOCTEST_REQUIRE(col.isOk());
    auto c = env.svc->createTask(col.value().id, "带标签卡");
    DOCTEST_REQUIRE(c.isOk());
    {
        module::todo::Task t3 = c.value();
        t3.tags = {"重要", "前端"};
        DOCTEST_REQUIRE(env.svc->updateTask(t3).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());
    frame.ui.layout(1600.0f, 1000.0f);  // 几何断言前必须 layout 出 frame

    // 迁移默认 4 列 + 新建 1 列 → 「chip列」= col.4，首卡 = card.0，
    // 首个标签 chip id = card.0.tag.0（drawChip 后缀 .bg / .text）
    const std::string chip = "todo.col.4.card.0.tag.0";
    const auto* txt = frame.ui.find(chip + ".text");
    const auto* bg = frame.ui.find(chip + ".bg");
    DOCTEST_REQUIRE(txt != nullptr);
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_CHECK_EQ(txt->text, std::string("重要"));
    DOCTEST_CHECK_MESSAGE(
        txt->verticalAlign == eui::VerticalAlign::Center,
        "chip 文本未挂 VerticalAlign::Center（ink 从行框顶沿起排 → 胶囊内偏上）");
    DOCTEST_CHECK_MESSAGE(
        std::fabs((txt->frame.y + txt->frame.height * 0.5f) -
                  (bg->frame.y + bg->frame.height * 0.5f)) <= 0.5f,
        "chip 文本 frame 中心 y=" +
            std::to_string(txt->frame.y + txt->frame.height * 0.5f) +
            " != 胶囊底中心 y=" +
            std::to_string(bg->frame.y + bg->frame.height * 0.5f));
    // 胶囊几何不变（高 = kChipHeight 单源常量）
    DOCTEST_CHECK_EQ(bg->frame.height, module::todo::ui::kChipHeight);
    DOCTEST_CHECK_EQ(txt->lineHeight, module::todo::ui::kChipHeight);

    mod->onStop();
    return true;
})

// 同列重排行号换算 + 落库插入位（纯函数）
REGISTER_TEST(todo_ui_reorder_pure_helpers,
              "todo_ui: 同列重排——行号换算与插入位（纯函数）", {
    using module::todo::ui::computeReorderInsertPosition;
    using module::todo::ui::computeReorderRow;
    // 列 = [A(pos 0), B(pos 1), C(pos 2)]，被拖卡 ownIndex = 0（A）
    // 行号语义（含自身）：0=首卡前 1=A|B 缝 2=B|C 缝 3=列尾
    DOCTEST_CHECK(!computeReorderRow(0, 0).has_value());  // 首卡前 = 原位
    DOCTEST_CHECK(!computeReorderRow(0, 1).has_value());  // 自己下面缝 = 原位
    DOCTEST_REQUIRE(computeReorderRow(0, 2).has_value());
    DOCTEST_CHECK_EQ(*computeReorderRow(0, 2), 1);        // 插到 C 前
    DOCTEST_REQUIRE(computeReorderRow(0, 3).has_value());
    DOCTEST_CHECK_EQ(*computeReorderRow(0, 3), 2);        // 列尾追加
    // ownIndex = 1（B）：上行插到 A 前；下行（row 3）= 列尾
    DOCTEST_REQUIRE(computeReorderRow(1, 0).has_value());
    DOCTEST_CHECK_EQ(*computeReorderRow(1, 0), 0);
    DOCTEST_CHECK(!computeReorderRow(1, 1).has_value());
    DOCTEST_CHECK(!computeReorderRow(1, 2).has_value());  // B|C 缝 = 原位（B 就在缝上）
    DOCTEST_REQUIRE(computeReorderRow(1, 3).has_value());
    DOCTEST_CHECK_EQ(*computeReorderRow(1, 3), 2);
    // 退化输入 → nullopt
    DOCTEST_CHECK(!computeReorderRow(-1, 2).has_value());
    DOCTEST_CHECK(!computeReorderRow(0, -1).has_value());

    // 落库插入位：行序**排除被拖卡自身**（moveTask 先摘出再插入）
    std::vector<module::todo::Task> tasks = {
        makeTask(1, 10, 0),   // A
        makeTask(2, 10, 1),   // B
        makeTask(3, 10, 2),   // C
        makeTask(4, 20, 0),   // 另一列，不干扰
    };
    // 拖 A（id=1）：行 1（摘出自身后）= 插到 C 前 → C 的 position = 2
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(tasks, 10, 1, 1), 2);
    // 行 2（列尾）→ 排除自身后 max position（C=2）+ 1 = 3
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(tasks, 10, 1, 2), 3);
    // 行 0 → A 的插入位 = 摘出自身后首个任务 B 的 position = 1
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(tasks, 10, 1, 0), 1);
    // 带空洞 position（moveTask 真实形态）：拖 C（id=3）
    std::vector<module::todo::Task> gappy = {
        makeTask(11, 30, 0),   // D
        makeTask(12, 30, 2),   // E
        makeTask(13, 30, 5),   // F（被拖）
    };
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(gappy, 30, 13, 0), 0);  // 插到 D 前
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(gappy, 30, 13, 1), 2);  // 插到 E 前
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(gappy, 30, 13, 2), 3);  // 列尾 = max(0,2)+1
    // 越界行号钳到列尾；空列（排除自身后无人）→ 0
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(tasks, 10, 1, 99), 3);
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(tasks, 10, 1, -1), 3);
    // 单卡列（排除自身后无人）→ 0（该形态经 computeReorderRow 恒为 no-op，
    // 走不到落库；此处只锁函数自身的退化行为不越界）
    std::vector<module::todo::Task> solo = {makeTask(9, 40, 0)};
    DOCTEST_CHECK_EQ(computeReorderInsertPosition(solo, 40, 9, 0), 0);
    return true;
})

// 拖入空列 → 占位条在卡区首卡位（列头下方），不贴列顶
REGISTER_TEST(todo_ui_drag_empty_column_dropbar,
              "todo_ui: 拖入空列占位条位于卡区首卡位（不贴列顶）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto src = env.svc->createColumn("空列源");
    auto empty = env.svc->createColumn("空目标列");
    DOCTEST_REQUIRE(src.isOk() && empty.isOk());
    auto mover = env.svc->createTask(src.value().id, "被移动任务");
    DOCTEST_REQUIRE(mover.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto composeLayout = [&]() {
        ui.begin("todo_drag_empty_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    auto boundsOf = [](const core::dsl::Element* e) {
        return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
    };

    // col.4 = 源列（1 卡）、col.5 = 空目标列；拖到空列内容区中央
    composeLayout();
    const auto* card = ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(card != nullptr);
    eui::PointerEvent pe;
    card->onPress(pe, boundsOf(card));
    const auto* dstBg = ui.find("todo.col.5.bg");
    DOCTEST_REQUIRE(dstBg != nullptr);
    core::dsl::DragEvent ev;
    ev.x = dstBg->frame.x + dstBg->frame.width / 2.0f;
    ev.y = dstBg->frame.y + dstBg->frame.height / 2.0f;
    card->onDrag(ev);
    composeLayout();
    const auto* dropBar = ui.find("todo.col.5.dropbar");
    DOCTEST_REQUIRE(dropBar != nullptr);  // 空列也预览插入位
    const auto* colBg = ui.find("todo.col.5.bg");
    DOCTEST_REQUIRE(colBg != nullptr);
    // 空列 hoverRow 恒 0 → 条在卡区首卡位 = 列顶 + 列头高
    // （旧缺陷：y=0 画在列顶、叠列头区域；并保证与非空列 row 0 槽位
    // 绝对坐标一致——两路径同语义）
    DOCTEST_CHECK_EQ(dropBar->frame.y, colBg->frame.y + 56.0f);
    DOCTEST_CHECK(ui.find("todo.col.5.empty") != nullptr);  // 空态提示不受影响

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_drag_hit_pure,
              "todo_ui: dragDropTarget 命中/列隙/板外/滚动/dpi（纯函数）", {
    using module::todo::ui::DragHitInput;
    using module::todo::ui::dragAnchorScale;
    using module::todo::ui::dragDropTarget;
    using module::todo::ui::kColumnContentInset;
    // 锚定卡在列 1 第 0 张：板面（dpi=1）originX=80，列 [80,280][300,500][520,720]，
    // boardTop=144，boardBottom=644。
    // 按下锚点 = 卡片左缘 = 列左缘 + 列内容左内边距（kColumnContentInset，
    // 卡不再贴列左缘）→ pressX = 300 + 16 = 316；
    // dragDropTarget 内部必须同源减掉该内边距还原列/板面原点：漏减会让命中
    // 整体右移 16px，列左缘 16px 带内的落点被误判成左侧列/板外。
    DragHitInput in;
    in.fromColIndex = 1;
    in.colCount = 3;
    in.pressX = 300.0f + kColumnContentInset;
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
    // 卡左内边距补偿的判别断言（漏减 inset 时命中框整体右移 16px → 这两条
    // 会红）：列 1 左缘内 16px 带（300..316，卡占不到但仍是本列落区）必须
    // 命中列 1；列 0 左缘内 8px（80..88）必须命中列 0
    DOCTEST_CHECK_EQ(dragDropTarget(in, 305.0, 300.0), 1);   // 列 1 左缘 + 5
    DOCTEST_CHECK_EQ(dragDropTarget(in, 84.0, 300.0), 0);    // 列 0 左缘 + 4
    DOCTEST_CHECK_EQ(dragDropTarget(in, 299.0, 300.0), -1);  // 列 1 左缘前 1px = 间隙

    // HiDPI（dpi=2）：指针空间坐标全翻倍，命中结果不变
    // 尺度由调用点显式声明（dpiScale 字段）；探针值同源
    DragHitInput hi = in;
    hi.pressX = (300.0f + kColumnContentInset) * 2.0f;
    hi.pressY = 400.0f;
    hi.pressWidth = 400.0f;
    hi.dpiScale = 2.0f;
    DOCTEST_CHECK_EQ(dragAnchorScale(hi), 2.0f);  // 探针仍与显式尺度一致
    DOCTEST_CHECK_EQ(dragDropTarget(hi, 700.0, 600.0), 1);   // 源列
    DOCTEST_CHECK_EQ(dragDropTarget(hi, 1100.0, 600.0), 2);  // 右列

    // 页面区原点（侧边栏展开/折叠）在指针与锚点上同减 → 命中结果不变
    // 拖拽换算吃 compose 空间，pageOriginX 两侧对消
    DragHitInput orig = in;
    orig.pageOriginX = 208.0f;
    DOCTEST_CHECK_EQ(dragAnchorScale(orig), 1.0f);
    DOCTEST_CHECK_EQ(dragDropTarget(orig, 400.0, 300.0), 1);
    DOCTEST_CHECK_EQ(dragDropTarget(orig, 550.0, 300.0), 2);
    DOCTEST_CHECK_EQ(dragDropTarget(orig, 290.0, 300.0), -1);  // 列间隙仍为间隙

    // 探针退化（无卡宽）→ 0（调用方 hitDropColumn 侧跳过断言）
    DragHitInput noProbe = in;
    noProbe.cardWidth = 0.0f;
    DOCTEST_CHECK_EQ(dragAnchorScale(noProbe), 0.0f);

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

    // dragDropRowIndex 悬停行（同几何：boardTop=144，列内容
    // 顶 = 200，卡 i 占 [i*92, i*92+84]，缝中心 = 插入位）
    using module::todo::ui::dragDropRowIndex;
    // 指针不在目标列（x 落列外）→ -1
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 200.0, 300.0, 0.0f, 2), -1);
    // 板上方 / 板下方 → -1
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 100.0, 0.0f, 2), -1);
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 700.0, 0.0f, 2), -1);
    // 退化输入 → -1
    DOCTEST_CHECK_EQ(dragDropRowIndex(DragHitInput{}, 2, 600.0, 300.0, 0.0f, 2), -1);
    // 列头区/首卡内（relY<84）→ 行 0（首卡前）；relY=84（卡 0 底 = 缝隙）→ 行 1
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 250.0, 0.0f, 2), 0);  // relY=50
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 283.0, 0.0f, 2), 0);  // relY=83
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 284.0, 0.0f, 2), 1);  // relY=84（卡0底/缝）
    // 卡 0 与卡 1 之间缝隙 → 行 1
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 290.0, 0.0f, 2), 1);  // relY=90
    // 卡 1 内（relY=100）→ 行 1（插其前）；卡 1 底（relY=176）→ 行 2
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 300.0, 0.0f, 2), 1);  // relY=100
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 376.0, 0.0f, 2), 2);  // relY=176
    // 列尾空白区 → 钳制到列卡数（追加）
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 500.0, 0.0f, 2), 2);  // relY=300
    // 目标列滚动 100：内容坐标 = 视口 + 滚动
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 250.0, 100.0f, 2), 1);  // relY=150
    // 空列（taskCount=0）→ 追加位 0
    DOCTEST_CHECK_EQ(dragDropRowIndex(in, 2, 600.0, 300.0, 0.0f, 0), 0);
    return true;
})

REGISTER_TEST(todo_ui_drag_scale_from_framework,
              "todo_ui: 拖拽尺度取自框架（与探针路径等价 + 不一致可观测）", {
    using module::todo::ui::DragHitInput;
    using module::todo::ui::dragAnchorScale;
    using module::todo::ui::dragDropTarget;
    using module::todo::ui::dragScaleMismatch;
    // U1 尺度契约：调用点（todo_module.cpp hitDropColumn）已改为喂框架值
    // app::contentScale()，探针降级为断言。本用例锁两件事：
    //  ① 显式框架值路径与旧"探针反推"路径命中列**逐位一致**（含 100/125/150）
    //  ② dragScaleMismatch 只在"探针自称非 1 却对不上框架"时命中（WARN 可观测）
    // 板面几何同 todo_ui_drag_hit_pure：compose 空间列 [80,280][300,500][520,720]、
    // boardTop=144、boardBottom=644（scale 只作用在指针空间，compose 侧不变）
    const auto makeInput = [](float scale) {
        DragHitInput in;
        in.fromColIndex = 1;
        in.colCount = 3;
        // 指针空间 = compose 空间 * scale（契约：物理 = 逻辑 * scale）。
        // 锚点 = 卡片左缘 = 源列左缘 + kColumnContentInset（缺陷 1 起卡不再
        // 贴列左缘；缺内边距补偿时下面的列隙/边界断言会红）
        in.pressX = (300.0f + module::todo::ui::kColumnContentInset) * scale;
        in.pressY = 200.0f * scale;      // compose 200 = 列头 + 第 0 张 - 无滚动
        in.pressWidth = 200.0f * scale;  // 探针分子：按下 bounds（指针空间）
        in.cardWidth = 200.0f;           // 探针分母：compose 卡宽
        in.colWidth = 200.0f;
        in.colGap = 20.0f;
        in.boardHeight = 500.0f;
        in.dpiScale = scale;             // 调用点喂的框架值（原为探针反推）
        in.cardIndex = 0;
        return in;
    };

    // ① 三档缩放：探针 == 框架值（旧路径取的就是它）→ 命中列一致
    for (float scale : {1.0f, 1.25f, 1.5f}) {
        const DragHitInput fw = makeInput(scale);
        const float probe = dragAnchorScale(fw);
        DOCTEST_CHECK_EQ(probe, scale);  // 探针独立复现框架值
        DOCTEST_CHECK_EQ(dragScaleMismatch(fw.dpiScale, probe), false);  // 一致 → 不告警
        // 旧路径复刻：dpiScale 取探针值，命中列与框架值路径逐位相同
        DragHitInput legacy = fw;
        legacy.dpiScale = probe;
        DOCTEST_CHECK_EQ(dragDropTarget(fw, 400.0 * scale, 300.0 * scale),
                         dragDropTarget(legacy, 400.0 * scale, 300.0 * scale));
        // 且落在预期列（列 [80,280][300,500][520,720]，板 [144,644]）
        DOCTEST_CHECK_EQ(dragDropTarget(fw, 400.0 * scale, 300.0 * scale), 1);   // 源列
        DOCTEST_CHECK_EQ(dragDropTarget(fw, 550.0 * scale, 300.0 * scale), 2);   // 右列
        DOCTEST_CHECK_EQ(dragDropTarget(fw, 100.0 * scale, 300.0 * scale), 0);   // 左列
        DOCTEST_CHECK_EQ(dragDropTarget(fw, 290.0 * scale, 300.0 * scale), -1);  // 列间隙
    }

    // ② 探针自称非 1 尺度（2.0）却对不上框架值（1.25）→ 断言命中 = 打 WARN
    DragHitInput stale = makeInput(1.25f);
    stale.pressWidth = 400.0f;  // 探针实测 400/200 = 2.0
    DOCTEST_CHECK_EQ(dragAnchorScale(stale), 2.0f);
    DOCTEST_CHECK_EQ(dragScaleMismatch(stale.dpiScale, dragAnchorScale(stale)), true);

    // ③ 无信号两态不告警：探针 == 1（100% 档）、探针 == 0（无卡宽退化）
    DragHitInput one = makeInput(1.0f);
    DOCTEST_CHECK_EQ(dragScaleMismatch(one.dpiScale, dragAnchorScale(one)), false);
    DragHitInput noProbe = makeInput(1.5f);
    noProbe.cardWidth = 0.0f;
    DOCTEST_CHECK_EQ(dragAnchorScale(noProbe), 0.0f);
    DOCTEST_CHECK_EQ(dragScaleMismatch(noProbe.dpiScale, dragAnchorScale(noProbe)), false);
    // （无卡宽时 dragDropTarget 自身即退化返回 -1，故此处不锁命中——
    //   换算吃的是框架值，探针退化只让断言失去信号）
    return true;
})

REGISTER_TEST(todo_ui_drag_hit_scroll_same_frame,
              "todo_ui: 拖拽 pressY/scrollOffset 同帧快照（tech-debt B 边缘场景锁）", {
    using module::todo::ui::DragHitInput;
    using module::todo::ui::dragDropTarget;
    namespace u = module::todo::ui;
    // 场景：列 0 第 4 张卡（cardIndex=3），按下时源列已滚动 100。
    // bounds.y（pressY）= 板面真实顶边 + (列头 + 3×步长 - 100) × dpi。
    const float boardTopPx = 150.0f;  // 板面真实顶边（屏幕像素，拖拽全程不动）
    const float stride = u::kCardHeight + u::kCardGap;
    DragHitInput in;
    in.fromColIndex = 0;
    in.colCount = 2;
    in.colWidth = 200.0f;
    in.colGap = 20.0f;
    in.cardWidth = 200.0f;
    in.pressWidth = 200.0f;  // dpi = 1
    in.boardHeight = 500.0f;
    in.cardIndex = 3;
    in.scrollOffset = 100.0f;  // 按下时刻快照（DragState.pressScrollOffset 语义）
    // 锚点 x = 列 0 左缘 + 卡左内边距（缺陷 1 起卡在列内有 16px 内边距，
    // dragDropTarget 内同源扣减还原列左缘）
    in.pressX = 80.0f + u::kColumnContentInset;
    in.pressY = boardTopPx + (u::kColumnHeaderHeight + 3.0f * stride - 100.0f);

    // 同帧（快照值）：还原出的板顶 = 真实板顶 150 → 边界精确
    DOCTEST_CHECK_EQ(dragDropTarget(in, 180.0, 150.0), 0);   // 板顶线上 = 板内
    DOCTEST_CHECK_EQ(dragDropTarget(in, 180.0, 149.0), -1);  // 板上方 1px
    DOCTEST_CHECK_EQ(dragDropTarget(in, 180.0, 300.0), 0);   // 源列
    DOCTEST_CHECK_EQ(dragDropTarget(in, 400.0, 300.0), 1);   // 跨列

    // 反例（tech-debt B 修前的 bug 形态）：拖拽中途列又被滚下 50（实际滚动
    // = 150），若命中时改读"当下" scrollOffset，pressY（旧帧）与新帧混值 →
    // 还原板顶被误推到 200，板顶线附近（真实板内）被误判板外。
    DragHitInput stale = in;
    stale.scrollOffset = 150.0f;
    DOCTEST_CHECK_EQ(dragDropTarget(stale, 180.0, 160.0), -1);
    // 修复后调用方始终传按下快照 → 命中框仍钉在按下时刻的真实板面上
    DOCTEST_CHECK_EQ(dragDropTarget(in, 180.0, 160.0), 0);
    return true;
})

// 拖拽跟手：锚点换算纯函数。锚点 = 按下时卡片位置（指针空间）
// + 按下点起累计位移（DragEvent::totalX/totalY，物理像素），换算因子与
// dragDropTarget 同一份 dpiScale。
REGISTER_TEST(todo_ui_drag_ghost_anchor_pure,
              "todo_ui: dragGhostAnchor 跟手锚点换算（纯函数）", {
    using module::todo::ui::dragGhostAnchor;
    // 100% 缩放、页面原点 0：锚点 = 按下位置 + 逐像素位移
    auto a = dragGhostAnchor(300.0f, 200.0f, 40.0, -25.0, 0.0f, 1.0f);
    DOCTEST_CHECK_EQ(a.x, 340.0f);
    DOCTEST_CHECK_EQ(a.y, 175.0f);
    // 位移为 0（刚按下）→ 锚点 = 卡片原位置（副本与原卡重合）
    auto still = dragGhostAnchor(300.0f, 200.0f, 0.0, 0.0, 0.0f, 1.0f);
    DOCTEST_CHECK_EQ(still.x, 300.0f);
    DOCTEST_CHECK_EQ(still.y, 200.0f);
    // HiDPI（dpi=2）：同一物理位移翻倍、锚点翻倍 → compose 结果与 100% 逐位一致
    // （尺度因子若漏除或只除一边，这里会立刻红）
    auto hi = dragGhostAnchor(600.0f, 400.0f, 80.0, -50.0, 0.0f, 2.0f);
    DOCTEST_CHECK_EQ(hi.x, 340.0f);
    DOCTEST_CHECK_EQ(hi.y, 175.0f);
    // 页面区原点（侧边栏展开）只在锚点上减一次，位移里不减（对消口径同
    // dragDropTarget：指针与锚点同空间）
    auto orig = dragGhostAnchor(608.0f, 200.0f, 40.0, 0.0, 208.0f, 1.0f);
    DOCTEST_CHECK_EQ(orig.x, 440.0f);  // 608 - 208 + 40
    DOCTEST_CHECK_EQ(orig.y, 200.0f);
    // 退化 dpiScale（0）按同尺度处理（windowToCompose 同口径），不产生 inf
    auto degen = dragGhostAnchor(300.0f, 200.0f, 40.0, 0.0, 0.0f, 0.0f);
    DOCTEST_CHECK_EQ(degen.x, 340.0f);
    DOCTEST_CHECK_EQ(degen.y, 200.0f);
    return true;
})

// 拖拽跟手：跟手副本的锚点、层序、半透、松手归零。
REGISTER_TEST(todo_ui_drag_follow_ghost,
              "todo_ui: 拖拽跟手副本（页面顶层 + 半透 + 松手消失）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    // 种子列 0..3 → 源列 = 4、目标列 = 5（同 todo_ui_drag_insert_position）
    auto src = env.svc->createColumn("跟手源列");
    auto dst = env.svc->createColumn("跟手目标列");
    DOCTEST_REQUIRE(src.isOk() && dst.isOk());
    auto mover = env.svc->createTask(src.value().id, "被拖的任务");
    DOCTEST_REQUIRE(mover.isOk());
    {
        // 带优先级拖：副本底色要跟手优先级
        module::todo::Task tm = mover.value();
        tm.priority = module::todo::Priority::High;
        DOCTEST_REQUIRE(env.svc->updateTask(tm).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto composeLayout = [&]() {
        ui.begin("todo_drag_ghost_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    auto boundsOf = [](const core::dsl::Element* e) {
        return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
    };

    composeLayout();
    DOCTEST_CHECK(ui.find("todo.dragghost") == nullptr);  // 未拖动 → 不画副本
    const auto* card = ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(card != nullptr);
    const eui::Rect cardBounds = boundsOf(card);
    eui::PointerEvent pe;
    card->onPress(pe, cardBounds);

    // 拖到目标列：累计位移 (50, -30)（框架给的是按下点起的累计值）
    const auto* dstBg = ui.find("todo.col.5.bg");
    DOCTEST_REQUIRE(dstBg != nullptr);
    core::dsl::DragEvent ev;
    ev.x = dstBg->frame.x + dstBg->frame.width / 2.0f;
    ev.y = dstBg->frame.y + 56.0f + 4.0f;
    ev.totalX = 50.0;
    ev.totalY = -30.0;
    card->onDrag(ev);
    composeLayout();

    const auto* ghost = ui.find("todo.dragghost");
    DOCTEST_REQUIRE(ghost != nullptr);
    // 锚点 = 按下时卡片位置 + 累计位移（无头环境 contentScale=1、页面原点=0）
    DOCTEST_CHECK_EQ(ghost->frame.x, cardBounds.x + 50.0f);
    DOCTEST_CHECK_EQ(ghost->frame.y, cardBounds.y - 30.0f);
    // 尺寸 = 本帧卡宽（与真卡同宽同高，跟手时不会忽大忽小）
    DOCTEST_CHECK_EQ(ghost->frame.width, cardBounds.width);
    DOCTEST_CHECK_EQ(ghost->frame.height, cardBounds.height);
    // 坑一/坑四：副本是页面**顶层 root**（不在源列 .cards 调暗容器内），
    // 且绘制序排在 todo.root 之后 = 压在所有列之上（跨列拖不被目标列卡片盖住）
    const auto& roots = ui.orderedRoots();
    auto rootIndexOf = [&roots](const core::dsl::Element* target) {
        for (size_t i = 0; i < roots.size(); ++i) {
            if (roots[i] == target) return static_cast<int>(i);
        }
        return -1;
    };
    const auto* boardRoot = ui.find("todo.root");
    DOCTEST_REQUIRE(boardRoot != nullptr);
    const int ghostIdx = rootIndexOf(ghost);
    const int boardIdx = rootIndexOf(boardRoot);
    DOCTEST_CHECK(ghostIdx >= 0);           // 副本自身就是 root（不在列内）
    DOCTEST_CHECK(ghostIdx > boardIdx);     // 排在 todo.root 之后 = 画在其上
    DOCTEST_CHECK(ghost->zIndex > 0);       // z 压过看板子树（列滚动条 z+1）
    // 坑二：半透（< 1）——目标列插入位占位条能从副本底下透出，不整块吃掉
    DOCTEST_CHECK(ghost->opacity < 1.0f);
    // 副本底色 = 该卡的优先级底（与真卡同一条 priorityCardBg）——
    // 拖到一半优先级指示不许消失（回退成 t.colors.surface 即红）
    {
        const auto* ghostBg = ui.find("todo.dragghost.bg");
        DOCTEST_REQUIRE(ghostBg != nullptr);
        const auto& t = core::theme::current();
        DOCTEST_CHECK(colorNear(
            ghostBg->color,
            eui::mixColor(t.colors.surface, t.colors.error, 0.30f)));
    }
    // 跟手期间源列原卡照旧半透明 + 源列调暗（门控关系未反：被调暗的是原卡
    // 与源列，副本全亮跟手）
    const auto* srcCards = ui.find("todo.col.4.cards");
    DOCTEST_REQUIRE(srcCards != nullptr);
    DOCTEST_CHECK_EQ(srcCards->opacity, 0.6f);
    const auto* srcCard = ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(srcCard != nullptr);
    DOCTEST_CHECK_EQ(srcCard->opacity, 0.45f);
    // 目标列占位条仍在（跟手副本不改既有插入位预览）
    DOCTEST_CHECK(ui.find("todo.col.5.dropbar") != nullptr);

    // 松手（落回源列 = 取消）：副本消失、偏移归零、源列恢复全亮
    {
        eui::PointerEvent up;
        up.x = ui.find("todo.col.4.bg")->frame.x + 40.0f;
        up.y = cardBounds.y + 20.0f;
        up.action = eui::PointerAction::Release;
        // mid-drag composeLayout 重建元素树——release 前重新 find
        const auto* rel = ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(rel != nullptr);
        rel->onRelease(up, eui::Rect{});
    }
    composeLayout();
    DOCTEST_CHECK(ui.find("todo.dragghost") == nullptr);  // 偏移/副本归零
    const auto* srcCardsAfter = ui.find("todo.col.4.cards");
    DOCTEST_REQUIRE(srcCardsAfter != nullptr);
    DOCTEST_CHECK_EQ(srcCardsAfter->opacity, 1.0f);  // 落位淡回起点（既有逻辑未破）
    const auto* srcCardAfter = ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(srcCardAfter != nullptr);
    DOCTEST_CHECK_EQ(srcCardAfter->opacity, 1.0f);
    // 取消（未跨列）→ 任务留在源列
    auto ts = env.svc->listTasks(src.value().id);
    DOCTEST_REQUIRE(ts.isOk());
    DOCTEST_REQUIRE_EQ(ts.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(ts.value()[0].title, std::string("被拖的任务"));

    mod->onStop();
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

    // 列头与列背景同 x 起点，卡片在列内纵向排列。
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
    // 卡片在列内（不是横向流式排开）。卡左缘 = 列左缘 + 列内容左内边距
    // kColumnContentInset（16px），两侧 inset 同源派生（见 contentCardWidth）：
    // 钉成 0 会退回「卡贴列左缘、左零边距」的缺陷形态。
    DOCTEST_CHECK_EQ(card0->frame.x,
                     colBg->frame.x + module::todo::ui::kColumnContentInset);
    DOCTEST_CHECK_EQ(card1->frame.x,
                     colBg->frame.x + module::todo::ui::kColumnContentInset);
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
        test_temp::tempPath("devdesk_test_todo_retry.db");
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
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto db = std::make_unique<::core::Database>(std::move(r.value()));
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

// 完成态按 kind 落列（不靠「已完成」标题）：重命名已完成列后，详情弹窗
// 「标记完成」仍应把任务移进该列——按标题找列会让重命名后任务静默留在原列
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

// 详情弹窗「移动到」已删除（改拖拽/右键菜单）——6 列场景下
// 不再渲染任何移动到按钮/标签；切列路径由右键菜单（本用例）与拖拽
//（todo_ui_drag_insert_position）分别覆盖
REGISTER_TEST(todo_ui_detail_move_removed,
              "todo_ui: 详情弹窗移除「移动到」（改拖拽/右键菜单，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;
    // 补 2 列 → 共 6 列（覆盖旧「全列遍历」用例的列数场景）
    DOCTEST_REQUIRE(env.svc->createColumn("第五列").isOk());
    DOCTEST_REQUIRE(env.svc->createColumn("第六列").isOk());
    auto a = env.svc->createTask(todoCol, "跨列移动任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 打开详情 → 6 列下不再渲染任何 move.* 按钮与「移动到」标签
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
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.panel") != nullptr);
        for (int i = 0; i < 6; ++i) {
            DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.move." +
                                        std::to_string(i)) == nullptr);
        }
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.mlabel") == nullptr);
        // 编辑能力不受影响（标题输入仍在）
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.input.title") != nullptr);
    }
    // 右键菜单「移动到」路径仍然在 → 任务可移到第六列
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const core::dsl::Element* moveTo = nullptr;
        for (int i = 2; i < 14 && moveTo == nullptr; ++i) {
            const auto* item = frame.ui.find("todo.ctx.level.0.item." + std::to_string(i));
            if (item == nullptr || item->onClick == nullptr) break;
            const auto* label = frame.ui.find("todo.ctx.level.0.label." + std::to_string(i));
            if (label != nullptr && label->text == "移动到「第六列」") {
                moveTo = label;
                item->onClick();
            }
        }
        DOCTEST_REQUIRE(moveTo != nullptr);
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

// 状态反馈 toast：超时消失 + subtitle 保持静态说明。
// setStatus 若写进 subtitle 会永久驻留，把静态操作说明顶掉。
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
    // 正文留空（todo 的 setStatus 只有标题）：空串必须原样落到 .message 节点，
    // 不得回落 EUI-NEO 组件自带的英文占位正文——那会让 toast 显示
    // "Short status message for non-blocking feedback."。回退 toast.cpp 的
    // 空判断即红。
    const auto* body = frame.ui.find("todo.toast.message");
    DOCTEST_REQUIRE(body != nullptr);
    DOCTEST_CHECK(body->text.empty());
    DOCTEST_CHECK(body->text.find("Short status message") == std::string::npos);
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

// 深色主题列区域白底
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

    // 列头热区必须是全透明（默认白是 bug）
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

// 三件套：搜索 / 逾期 / tags / 列色点

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

        // Esc 清空走 onCancel 路径
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

// 搜索框 Enter 无副作用（与 md.search 一致），清除只留 Esc（onCancel）
REGISTER_TEST(todo_ui_search_enter_noop,
              "todo_ui: 搜索框 Enter 无副作用（[10]）+ Esc 清除保留", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("回车列");
    DOCTEST_REQUIRE(col.isOk());
    auto hitTask = env.svc->createTask(col.value().id, "回车的命中");
    auto missTask = env.svc->createTask(col.value().id, "回车的无关");
    DOCTEST_REQUIRE(hitTask.isOk() && missTask.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        auto* input = frame.ui.find("todo.search.hit");
        DOCTEST_REQUIRE(input != nullptr);
        core::TextInputEvent ev;
        ev.text = "命中";
        input->onTextInput(ev);
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.1.bg") == nullptr);

        // Enter：无副作用 —— 搜索词保留、过滤态不变（md.search 同款）
        auto* inputEnter = frame.ui.find("todo.search.hit");
        DOCTEST_REQUIRE(inputEnter != nullptr);
        DOCTEST_REQUIRE(inputEnter->onKeyEvent != nullptr);
        core::KeyEvent enter;
        enter.key = core::InputKey::Enter;
        enter.action = core::KeyAction::Press;
        inputEnter->onKeyEvent(enter);
        frame.refresh();
        DOCTEST_CHECK_EQ(frame.ui.find("todo.col.4.card.0.title")->text,
                         std::string("回车的命中"));
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.1.bg") == nullptr);

        // Esc：清除保留（onCancel 路径），未命中卡恢复
        auto* inputEsc = frame.ui.find("todo.search.hit");
        DOCTEST_REQUIRE(inputEsc != nullptr);
        core::KeyEvent esc;
        esc.key = core::InputKey::Escape;
        esc.action = core::KeyAction::Press;
        inputEsc->onKeyEvent(esc);
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

// THS-13：入选按秒级（due_at < now），日历日差却可能是 0（1 秒前到期）——
// 行文案必须至少 1 天，不再显示「逾期0天」
REGISTER_TEST(todo_ui_overdue_min_one_day,
              "todo_ui: 逾期 1 秒仍显示「逾期1天」（max(1,…)，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;

    auto t = env.svc->createTask(todoCol, "刚过期");
    DOCTEST_REQUIRE(t.isOk());
    {
        module::todo::Task u = t.value();
        u.dueAt = static_cast<int64_t>(std::time(nullptr)) - 1;  // 1 秒前到期
        DOCTEST_REQUIRE(env.svc->updateTask(u).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        const auto* badge = frame.ui.find("todo.btn.overdue.text");
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK(badge->text.find("1") != std::string::npos);  // 计数 = 1
        const auto* btn = frame.ui.find("todo.btn.overdue.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
        frame.refresh();
        const auto* rowTitle = frame.ui.find("todo.dialog.overdue.row.0.title");
        const auto* rowDays = frame.ui.find("todo.dialog.overdue.row.0.days");
        DOCTEST_REQUIRE(rowTitle != nullptr);
        DOCTEST_REQUIRE(rowDays != nullptr);
        DOCTEST_CHECK_EQ(rowTitle->text, std::string("刚过期"));
        DOCTEST_CHECK_EQ(rowDays->text, std::string("逾期1天"));
    }

    mod->onStop();
    return true;
})

// 逾期查询失败不得伪装成「逾期 0」：openOverdueDialog 若把失败当空结果开弹窗
// （「逾期任务（0）」），refreshOverdueCount 就会静默改写为 0
REGISTER_TEST(todo_ui_overdue_query_fail,
              "todo_ui: 逾期查询失败 → 报错不开伪空弹窗（THS-06，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;
    auto t = env.svc->createTask(todoCol, "逾期任务甲");
    DOCTEST_REQUIRE(t.isOk());
    {
        module::todo::Task u = t.value();
        u.dueAt = static_cast<int64_t>(std::time(nullptr)) - 2 * 86400;
        DOCTEST_REQUIRE(env.svc->updateTask(u).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        const auto* badge = frame.ui.find("todo.btn.overdue.text");
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK(badge->text.find("1") != std::string::npos);  // 计数 = 1

        // 注入查询失败：删任务表 → listOverdue 的 SELECT 失败
        DOCTEST_REQUIRE(env.db->db().exec("DROP TABLE todo_tasks").isOk());

        // ① 点 badge：失败 → 不开弹窗（旧行为开成「逾期任务（0）」）+ 报错
        const auto* btn = frame.ui.find("todo.btn.overdue.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
        frame.refresh();
        const auto* panel = frame.ui.find("todo.dialog.overdue.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
        const auto* toast = frame.ui.find("todo.toast.title");
        DOCTEST_REQUIRE(toast != nullptr);
        DOCTEST_CHECK(toast->text.find("逾期任务加载失败") != std::string::npos);

        // ② 刷新路径（列右移成功 → reloadAll → refreshOverdueCount 失败）：
        //    失败不得静默把 badge 改写为 0，须报错且保留上次计数
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* head = frame.ui.find("todo.col.0.head");
        DOCTEST_REQUIRE(head != nullptr);
        head->onContextMenu(pe, rect);
        frame.refresh();
        const auto* shiftRight = frame.ui.find("todo.ctx.level.0.item.2");
        DOCTEST_REQUIRE(shiftRight != nullptr);
        shiftRight->onClick();
        frame.refresh();
        const auto* toast2 = frame.ui.find("todo.toast.title");
        DOCTEST_REQUIRE(toast2 != nullptr);
        DOCTEST_CHECK(toast2->text.find("逾期任务查询失败") != std::string::npos);
        const auto* badge2 = frame.ui.find("todo.btn.overdue.text");
        DOCTEST_REQUIRE(badge2 != nullptr);
        DOCTEST_CHECK(badge2->text.find("1") != std::string::npos);  // 保留计数
    }

    mod->onStop();
    return true;
})

// 完成态流转的「列迁移」失败不得提示成功：失败只 LOG_WARN 后被吞的话，用户看到
// 「任务已完成」而卡片留在原列，再点一次也看不出原因
REGISTER_TEST(todo_ui_complete_move_fail,
              "todo_ui: 完成后列迁移失败 → 报错不报成功（THS-06，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t todoCol = list.value()[0].id;
    auto a = env.svc->createTask(todoCol, "列迁移失败任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

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

        // 注入列迁移失败：只挡 moveTask 的 SET column_id（状态迁移不经该列，
        // 详情表单保存的 updateTask 也不经它）
        DOCTEST_REQUIRE(
            env.db->db()
                .exec("CREATE TRIGGER ths06_block_move BEFORE UPDATE OF column_id "
                      "ON todo_tasks BEGIN SELECT RAISE(ABORT, 'THS-06 injected'); END")
                .isOk());

        const auto* toggle = frame.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        toggle->onClick();
        frame.refresh();

        const auto* toast = frame.ui.find("todo.toast.title");
        DOCTEST_REQUIRE(toast != nullptr);
        DOCTEST_CHECK(toast->text.find("移入已完成列失败") != std::string::npos);
    }

    // 诚实的中途态：状态迁移已生效（已完成），卡片仍留原列——但用户已被明确告知
    {
        auto inTodo = env.svc->listTasks(todoCol);
        DOCTEST_REQUIRE(inTodo.isOk());
        DOCTEST_REQUIRE_EQ(inTodo.value().size(), 1u);
        DOCTEST_CHECK(inTodo.value()[0].completedAt.has_value());
    }

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

    // 搜索过滤：命中 id 集内保留且顺序不变；未激活 → 全保留。
    // 命中表为 unordered_set——过滤只取决于「是否命中」，结果集与顺序无关
    //（下面双序构建锁这一点）
    std::vector<module::todo::Task> tasks = {
        makeTask(1, 10, 0),
        makeTask(2, 10, 1),
        makeTask(3, 20, 0),
    };
    std::vector<const module::todo::Task*> ptrs = {&tasks[0], &tasks[1], &tasks[2]};
    std::unordered_set<int64_t> matched;
    matched.insert(3);
    matched.insert(2);
    const auto out = filterBySearch(ptrs, matched, true);
    DOCTEST_REQUIRE_EQ(out.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(out[0]->id, static_cast<int64_t>(2));
    DOCTEST_CHECK_EQ(out[1]->id, static_cast<int64_t>(3));
    DOCTEST_CHECK_EQ(filterBySearch(ptrs, matched, false).size(),
                     static_cast<size_t>(3));
    // 命中表插入序无关（set 语义 = 旧 find 语义的等价点：成员判定不因序变）
    std::unordered_set<int64_t> matchedRev;
    matchedRev.insert(2);
    matchedRev.insert(3);
    const auto out2 = filterBySearch(ptrs, matchedRev, true);
    DOCTEST_REQUIRE_EQ(out2.size(), out.size());
    DOCTEST_CHECK_EQ(out2[0]->id, out[0]->id);
    DOCTEST_CHECK_EQ(out2[1]->id, out[1]->id);
    // 空集 + 全命中两个边界
    std::unordered_set<int64_t> empty;
    DOCTEST_CHECK_EQ(filterBySearch(ptrs, empty, true).size(),
                     static_cast<size_t>(0));
    std::unordered_set<int64_t> all;
    all.insert(1);
    all.insert(2);
    all.insert(3);
    DOCTEST_CHECK_EQ(filterBySearch(ptrs, all, true).size(),
                     static_cast<size_t>(3));

    return true;
})

// 优先级直接改卡底色（不加色块 pill）
// 回退任一条即红：把 .prio 色块画回来 → 元素存在断言红；把卡底算式换回
// t.colors.surface → 「无 vs 高 背景色不同」与 tint 取值断言红；把 hover 态
// 换回中性 token → 三态叠加断言红；调低/调高对比度 → ≥4.5:1 断言红。
REGISTER_TEST(todo_ui_priority_card_tint,
              "todo_ui: 优先级直接改卡底色（色块已删 / 三态叠加 / 对比度 ≥ AA）", {
    using module::todo::Priority;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("底色列");
    DOCTEST_REQUIRE(col.isOk());
    const Priority kOrder[4] = {Priority::None, Priority::Low, Priority::Medium,
                                Priority::High};
    for (int i = 0; i < 4; ++i) {
        auto a = env.svc->createTask(col.value().id, "底色卡" + std::to_string(i));
        DOCTEST_REQUIRE(a.isOk());
        module::todo::Task t2 = a.value();
        t2.priority = kOrder[i];
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }
    // 期望值独立写死在用例里（不引用被测常量表）：改强度表/色源即红
    const float kStrength[4] = {0.00f, 0.16f, 0.22f, 0.30f};

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());
    frame.ui.layout(1600.0f, 1000.0f);
    const auto& t = core::theme::current();

    // 判据 a：优先级色块元素已不存在（元素 id 查不到）
    for (int i = 0; i < 4; ++i) {
        const std::string cid = "todo.col.4.card." + std::to_string(i);
        DOCTEST_CHECK(frame.ui.find(cid + ".prio.bg") == nullptr);
        DOCTEST_CHECK(frame.ui.find(cid + ".prio.text") == nullptr);
        DOCTEST_CHECK(frame.ui.find(cid + ".bar") == nullptr);
    }

    const auto* noneBg = frame.ui.find("todo.col.4.card.0.bg");
    const auto* lowBg = frame.ui.find("todo.col.4.card.1.bg");
    const auto* midBg = frame.ui.find("todo.col.4.card.2.bg");
    const auto* highBg = frame.ui.find("todo.col.4.card.3.bg");
    DOCTEST_REQUIRE(noneBg != nullptr);
    DOCTEST_REQUIRE(lowBg != nullptr);
    DOCTEST_REQUIRE(midBg != nullptr);
    DOCTEST_REQUIRE(highBg != nullptr);

    // 判据 b：底色 = 卡三态基色 × 强度表的 tint（None 档强度 0 = 默认卡底）
    const eui::Color kPrio[4] = {t.colors.textTertiary, t.colors.info,
                                 t.colors.warning, t.colors.error};
    const core::dsl::Element* cards[4] = {noneBg, lowBg, midBg, highBg};
    for (int i = 0; i < 4; ++i) {
        DOCTEST_CHECK(colorNear(
            cards[i]->color,
            eui::mixColor(t.colors.surface, kPrio[i], kStrength[i])));
    }
    // 判据 b'：无优先级卡与高优先级卡背景色不同（用户原始判据 1）
    DOCTEST_CHECK(!colorNear(noneBg->color, highBg->color, 0.01f));
    // 判据 b'：四档两两不同（含 None ↔ Low：None 灰 vs 低优先级蓝）
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            DOCTEST_CHECK(maxChannelDelta(cards[i]->color, cards[j]->color) >
                          0.02f);
        }
    }
    // 判据 b'：低优先级「不刺眼」= 与 surface 的色差落在浅档；高优先级看得出
    DOCTEST_CHECK(maxChannelDelta(lowBg->color, t.colors.surface) <= 32.0f / 255.0f);
    DOCTEST_CHECK(maxChannelDelta(highBg->color, t.colors.surface) >= 45.0f / 255.0f);

    // 判据 c：hover / active 与优先级底色**叠加**（不被中性态色顶掉），
    // 且仍与 base 不同（既有 hover 反馈没被底色覆盖）
    for (int i = 0; i < 4; ++i) {
        DOCTEST_CHECK(colorNear(
            cards[i]->hoverColor,
            eui::mixColor(t.colors.hover, kPrio[i], kStrength[i])));
        DOCTEST_CHECK(colorNear(
            cards[i]->pressedColor,
            eui::mixColor(t.colors.active, kPrio[i], kStrength[i])));
        DOCTEST_CHECK(maxChannelDelta(cards[i]->hoverColor, cards[i]->color) >
                      0.02f);
    }
    // 判据 c'：选中态边框仍在（2px primary 只在选中卡上；未被底色盖住）
    DOCTEST_CHECK_EQ(noneBg->border.width, 1.0f);
    DOCTEST_CHECK(colorNear(highBg->border.color, t.colors.borderDefault));
    {
        Frame f2(mod.get());
        const auto* card = f2.ui.find("todo.col.4.card.3.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();  // 选中高优先级卡（下一帧看选中态）
        Frame f3(mod.get());
        const auto* sel = f3.ui.find("todo.col.4.card.3.bg");
        DOCTEST_REQUIRE(sel != nullptr);
        DOCTEST_CHECK_EQ(sel->border.width, 2.0f);
        DOCTEST_CHECK(colorNear(sel->border.color, t.colors.primary));
        // 选中不改变优先级底色（边框与底色两通道独立）
        DOCTEST_CHECK(colorNear(
            sel->color,
            eui::mixColor(t.colors.surface, t.colors.error, 0.30f)));
    }

    // 判据 d：底色改了，卡片文字对比度不掉（WCAG AA 4.5:1）
    for (int i = 0; i < 4; ++i) {
        DOCTEST_CHECK(contrastRatio(cards[i]->color, t.colors.textPrimary) >= 4.5f);
    }
    // 浅色主题复查（默认深色）：底色随 surface 走，浅底 + 深字方向不变
    ::core::theme::setLight();
    const auto& tl = core::theme::current();
    Frame light(mod.get());
    light.ui.layout(1600.0f, 1000.0f);
    const auto* lNone = light.ui.find("todo.col.4.card.0.bg");
    const auto* lHigh = light.ui.find("todo.col.4.card.3.bg");
    DOCTEST_REQUIRE(lNone != nullptr);
    DOCTEST_REQUIRE(lHigh != nullptr);
    DOCTEST_CHECK(colorNear(lNone->color, tl.colors.surface));
    DOCTEST_CHECK(colorNear(
        lHigh->color, eui::mixColor(tl.colors.surface, tl.colors.error, 0.30f)));
    DOCTEST_CHECK(!colorNear(lNone->color, lHigh->color, 0.01f));
    DOCTEST_CHECK(contrastRatio(lNone->color, tl.colors.textPrimary) >= 4.5f);
    DOCTEST_CHECK(contrastRatio(lHigh->color, tl.colors.textPrimary) >= 4.5f);
    ::core::theme::setDark();  // 恢复默认（本用例前提 = 深色，防污染后续用例）

    mod->onStop();
    return true;
})

// 详情弹窗优先级按钮自身带该优先级色底
// 同源判据：按钮底 = priorityButtonBg（与卡底同一张 priorityColor 表 + 同一张
// 强度表），未选中 = 强度表浅 tint / 选中 = 0.55 深 tint（"实色感"）。
REGISTER_TEST(todo_ui_priority_button_tint,
              "todo_ui: 详情优先级按钮各自带该档色底（选中/未选中可区分）", {
    using module::todo::Priority;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("按钮列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "按钮卡");
    DOCTEST_REQUIRE(a.isOk());
    {
        // 起始优先级给「高」：详情弹窗打开时 pic.3 是选中态，其余三档是未选中
        // （默认 None 起始则 prio.0 选中，会把「None 未选中档」的取值问句混淆）
        module::todo::Task t2 = a.value();
        t2.priority = module::todo::Priority::High;
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    const auto& t = core::theme::current();
    // 期望值独立写死（改色源 / 改强度表 / 改选中档强度即红）
    const eui::Color kPrio[4] = {t.colors.textTertiary, t.colors.info,
                                 t.colors.warning, t.colors.error};
    const float kUnsel[4] = {0.12f, 0.16f, 0.22f, 0.30f};
    const float kPicked = 0.55f;

    Frame f1(mod.get());
    const auto* card = f1.ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(card->onClick != nullptr);
    card->onClick();  // 打开详情（弹窗状态留在模块侧，后续帧持续渲染）

    // 起始态（任务优先级 = 高）：0/1/2 档未选中、3 档选中
    eui::Color unsel[4] = {};
    bool observed[4] = {false, false, false, false};
    {
        Frame f(mod.get());
        for (int j = 0; j < 3; ++j) {
            const auto* bg = f.ui.find("todo.dialog.detail.prio." +
                                       std::to_string(j) + ".bg");
            DOCTEST_REQUIRE(bg != nullptr);
            unsel[j] = bg->color;
            observed[j] = true;
            DOCTEST_CHECK(colorNear(
                bg->color, eui::mixColor(t.colors.surfaceElevated, kPrio[j],
                                         kUnsel[j])));
            DOCTEST_CHECK_EQ(bg->border.width, 1.0f);
            DOCTEST_CHECK(contrastRatio(bg->color, t.colors.textPrimary) >= 4.5f);
        }
        const auto* picked = f.ui.find("todo.dialog.detail.prio.3.bg");
        DOCTEST_REQUIRE(picked != nullptr);
        DOCTEST_CHECK(colorNear(
            picked->color,
            eui::mixColor(t.colors.surfaceElevated, kPrio[3], kPicked)));
    }

    // 逐档点选（i = 被点中的那档）：
    //   选中按钮 = 深一档 tint（0.55）+ 2px 该档色边框 + 与自己的未选中态可区分
    //   其余三档 = 强度表浅 tint + 1px 边框（四档的未选中变体在循环里全覆盖）
    for (int i = 0; i < 4; ++i) {
        Frame f(mod.get());
        const auto* btn = f.ui.find("todo.dialog.detail.prio." +
                                    std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
        Frame g(mod.get());
        const auto* sel = g.ui.find("todo.dialog.detail.prio." +
                                    std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(sel != nullptr);
        DOCTEST_CHECK(colorNear(
            sel->color,
            eui::mixColor(t.colors.surfaceElevated, kPrio[i], kPicked)));
        DOCTEST_CHECK(contrastRatio(sel->color, t.colors.textPrimary) >= 4.5f);
        DOCTEST_CHECK_EQ(sel->border.width, 2.0f);
        DOCTEST_CHECK(colorNear(sel->border.color, kPrio[i]));
        for (int j = 0; j < 4; ++j) {
            const auto* other = g.ui.find("todo.dialog.detail.prio." +
                                          std::to_string(j) + ".bg");
            DOCTEST_REQUIRE(other != nullptr);
            if (j == i) continue;
            unsel[j] = other->color;
            observed[j] = true;
            // 其余按钮底色 == 各自档位的浅 tint（None 档也有自己的 0.12 灰）
            DOCTEST_CHECK(colorNear(
                other->color, eui::mixColor(t.colors.surfaceElevated, kPrio[j],
                                            kUnsel[j])));
            DOCTEST_CHECK_EQ(other->border.width, 1.0f);
            // 未选中底色上的文字对比度
            DOCTEST_CHECK(contrastRatio(other->color, t.colors.textPrimary) >=
                          4.5f);
        }
        // 选中 ≠ 自己的未选中态（同档对比）
        DOCTEST_REQUIRE(observed[i]);
        DOCTEST_CHECK(!colorNear(sel->color, unsel[i], 0.02f));
        // 不同优先级按钮底色互不相同（四档两两）
        for (int x = 0; x < 4; ++x) {
            for (int y = x + 1; y < 4; ++y) {
                DOCTEST_REQUIRE(observed[x] && observed[y]);
                DOCTEST_CHECK(maxChannelDelta(unsel[x], unsel[y]) > 0.03f);
            }
        }
    }

    mod->onStop();
    return true;
})

// 卡片标签文本大一档（chip 高随之适配，不裁不溢出）
// 回退字号（12 → 10）→ fontSize 断言红；回退 chip 高（22 → 18）→ height 断言红；
// 只调字号不调 chip 高 → 内边距断言红；估宽不跟着放大 → 溢出卡宽断言红。
REGISTER_TEST(todo_ui_tag_chip_font_bigger,
              "todo_ui: 标签 chip 字号 10→12 / 高 18→22（不裁字、不溢出卡宽）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("标签列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "带标签卡");
    DOCTEST_REQUIRE(a.isOk());
    const std::vector<std::string> kTags = {"重要", "前端"};
    {
        module::todo::Task t2 = a.value();
        t2.tags = kTags;
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }
    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());
    frame.ui.layout(1600.0f, 1000.0f);
    const auto& t = core::theme::current();

    const auto* cardBg = frame.ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(cardBg != nullptr);
    for (int i = 0; i < 2; ++i) {
        const std::string cid =
            "todo.col.4.card.0.tag." + std::to_string(i);
        const auto* chipBg = frame.ui.find(cid + ".bg");
        const auto* chipText = frame.ui.find(cid + ".text");
        DOCTEST_REQUIRE(chipBg != nullptr);
        DOCTEST_REQUIRE(chipText != nullptr);
        // 调后字号 12（= fontSizeSmall - 1；调前 10 = fontSizeSmall - 3）
        DOCTEST_CHECK_EQ(chipText->fontSize, t.sizes.fontSizeSmall - 1.0f);
        DOCTEST_CHECK_EQ(chipText->fontSize, 12.0f);
        // chip 高跟着适配（调前 18），lineHeight 同高 → 文字不裁行
        DOCTEST_CHECK_EQ(chipBg->frame.height, 22.0f);
        DOCTEST_CHECK_EQ(chipText->lineHeight, chipBg->frame.height);
        // chip 高 ≥ 字号 + 上下各 5px 内边距
        DOCTEST_CHECK(chipBg->frame.height >= chipText->fontSize + 2.0f * 5.0f);
        // 不溢出卡片：chip 右缘 ≤ 卡右缘、下缘 ≤ 卡下缘
        DOCTEST_CHECK(chipBg->frame.x + chipBg->frame.width <=
                      cardBg->frame.x + cardBg->frame.width);
        DOCTEST_CHECK(chipBg->frame.y + chipBg->frame.height <=
                      cardBg->frame.y + cardBg->frame.height);
        // chip 宽 = 估宽（chipWidth 已随字号同步放大：20 + 6.6/字节；
        // 评宽 ≥ 真实字形宽在 test_font_em_layout.cpp 用真实字体复核）
        DOCTEST_CHECK_EQ(chipBg->frame.width,
                         module::todo::ui::chipWidth(kTags[static_cast<size_t>(i)]));
    }

    mod->onStop();
    return true;
})

// 窄列固定 66px 截止 chip 越出卡右缘 → 自适应缩窄
// 列宽无下限（colW = (contentW - gap*(n-1)) / n）。删掉 42px 优先级 chip 后
// chipX 起点从 58 回到 16，8 列（卡宽 94 → 剩余 66）不再触发缩窄，故本用例取
// 9 列（1200 窗：colW = (1120-8*16)/9 ≈ 110.2，卡宽 ≈ 78.2 → 剩余 ≈ 50.2 < 66
// 仍走缩窄分支）；宽窗不缩（保持 66px 原观感）
REGISTER_TEST(todo_card_due_chip_adapts_narrow_column,
              "todo_ui: 窄列截止 chip 自适应缩窄不越出卡右缘（审计修复）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    std::vector<int64_t> colIds;
    for (int i = 0; i < 5; ++i) {  // 迁移默认 4 列之上再造 5 列 = 9 列
        auto col = env.svc->createColumn("窄列" + std::to_string(i));
        DOCTEST_REQUIRE(col.isOk());
        colIds.push_back(col.value().id);
    }
    auto a = env.svc->createTask(colIds.back(), "窄列卡");
    DOCTEST_REQUIRE(a.isOk());
    {
        module::todo::Task t2 = a.value();
        t2.priority = module::todo::Priority::High;
        t2.dueAt = 2000000000;  // 未来时间，未逾期
        auto up = env.svc->updateTask(t2);
        DOCTEST_REQUIRE(up.isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    // 最小窗 1200×720：9 列 → colW = (1120-8*16)/9 ≈ 110.2，卡宽 ≈ 78.2
    Frame frame(mod.get(), 1200.0f, 720.0f);
    frame.ui.layout(1200.0f, 720.0f);  // 元素 frame 需显式 layout（Frame 不跑）
    const auto* cardBg = frame.ui.find("todo.col.8.card.0.bg");
    const auto* dueBg = frame.ui.find("todo.col.8.card.0.due.bg");
    DOCTEST_REQUIRE(cardBg != nullptr);
    DOCTEST_REQUIRE(dueBg != nullptr);
    // 优先级 chip 已删：此处不再有 .prio 元素
    DOCTEST_CHECK(frame.ui.find("todo.col.8.card.0.prio.bg") == nullptr);
    // 截止 chip 右缘不越出卡右缘（修复前固定 66px 越界）
    DOCTEST_CHECK(dueBg->frame.x + dueBg->frame.width <=
                  cardBg->frame.x + cardBg->frame.width);
    // chip 缩窄到「卡宽 - 左右留白」且仍在（≥ 最小宽 24）
    DOCTEST_CHECK(dueBg->frame.width >= 24.0f);
    DOCTEST_CHECK(dueBg->frame.width < 66.0f);
    DOCTEST_CHECK_EQ(dueBg->frame.width,
                     std::min(66.0f, cardBg->frame.width - 12.0f - 16.0f));
    // 宽窗 1600×1000 9 列：colW = (1520-128)/9 ≈ 154.7 → 不缩
    Frame wide(mod.get(), 1600.0f, 1000.0f);
    wide.ui.layout(1600.0f, 1000.0f);
    const auto* wideDue = wide.ui.find("todo.col.8.card.0.due.bg");
    DOCTEST_REQUIRE(wideDue != nullptr);
    DOCTEST_CHECK_EQ(wideDue->frame.width, 66.0f);

    mod->onStop();
    return true;
})

// 详情弹窗动作先存表单 / 删除确认保留弹窗 / backdrop 守卫

REGISTER_TEST(todo_ui_detail_actions_save_form,
              "todo_ui: 详情动作按钮先存表单 + 删除取消保留弹窗 + backdrop 守卫", {
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

    // 列序：4 个种子列 [0..3] + 编辑列(4) + 目标列(5)（详情无「移动到」按钮）

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
        // input.h 程序赋值未聚焦时光标置 0（d1f55a3）；显式置文末让 onTextInput 追加而非前插
        {
            auto& st = f2.ui.state<components::input_detail::InputModel::InputState>("todo.dialog.detail.input.title");
            st.cursor = st.selectionStart = st.selectionEnd = (int)st.text.size();
        }
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
            if (tk.id == a.value().id) {
                found = true;
                DOCTEST_CHECK_EQ(tk.title, std::string("旧标题NEW"));  // 编辑未丢（修复前会丢）
                DOCTEST_CHECK(tk.completedAt.has_value());             // 动作照常执行
            }
        }
        DOCTEST_CHECK(found);
    }

    // B. 详情「移动到」已删（改拖拽/右键菜单）——改标题 →
    //    保存 → 标题落库且**列不变**（弹窗不再提供切列，列身份以库中为准）；
    //    详情内不得再渲染 move.* 按钮
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
        Frame f2(mod.get());
        const auto* input = f2.ui.find("todo.dialog.detail.input.title.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        // input.h 程序赋值未聚焦时光标置 0（d1f55a3）；显式置文末让 onTextInput 追加而非前插
        {
            auto& st = f2.ui.state<components::input_detail::InputModel::InputState>("todo.dialog.detail.input.title");
            st.cursor = st.selectionStart = st.selectionEnd = (int)st.text.size();
        }
        core::TextInputEvent tie;
        tie.text = "M";
        input->onTextInput(tie);
        // 整改后：不再有「移动到」按钮（拖拽/右键菜单承担切列）
        DOCTEST_CHECK(f2.ui.find("todo.dialog.detail.move.5.bg") == nullptr);
        const auto* saveBtn = f2.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(saveBtn != nullptr);
        DOCTEST_REQUIRE(saveBtn->onClick != nullptr);
        saveBtn->onClick();

        auto got = env.svc->listAllTasks();
        DOCTEST_REQUIRE(got.isOk());
        for (const auto& tk : got.value()) {
            if (tk.id == b.value().id) {
                DOCTEST_CHECK_EQ(tk.title, std::string("移动前编辑M"));  // 表单已落库
                DOCTEST_CHECK_EQ(tk.columnId, col.value().id);   // 列不变（编辑不切列）
            }
        }
    }

    // C. 详情内删除：确认取消后回到详情弹窗（编辑现场保留）；确认删除才落库关窗
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.1.bg");  // 删除目标（B 段保存后 b 仍占 0，c 是 1）
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
            if (tk.id == c.value().id) stillThere = true;
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
            if (tk.id == c.value().id) gone = false;
        }
        DOCTEST_CHECK(gone);
    }

    // D. backdrop 守卫：有未存改动第一次点遮罩只警告不关不落库，再点一次放弃关闭
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.1.bg");  // 遮罩守卫（C 删 c 后 [b@0, e@1]）
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
            if (tk.id == e.value().id && tk.title == std::string("遮罩守卫")) {
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
            if (tk.id == e.value().id && tk.title == std::string("遮罩守卫")) {
                discarded = true;  // 放弃语义：改动不入库
            }
        }
        DOCTEST_CHECK(discarded);
    }

    mod->onStop();
    return true;
})

// 「+ 列」命名弹窗 + 列头 ✎ 重命名入口（headless）

// 点「+ 列」不即时落库：先弹命名弹窗，确认才落库；Esc（输入框聚焦走 input
// onCancel，未聚焦走 app 级兜底）取消不落库；留空确认采用默认名；输入名字按名字建。
// 改名入口显性化为列头 ✎。
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

    // 1) 点「+ 列」→ 只开弹窗，不即时落库
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

    // 3) Esc（焦点在输入框：input 内置 Escape→onCancel=取消）→ 取消不落库。
    //    Esc 走 input 的 onCancel，不要再加模块级 passthrough；若 onCancel 被删，
    //    内置会把 Esc 回退到 onEnter 提交并建出新列 → colCount 变
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
    test_support::setShortcutModifier(selectAll.modifiers);
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

// 创建/校验失败反馈 → 弹窗内嵌红字 formError

REGISTER_TEST(todo_ui_dialog_form_error,
              "todo_ui: 新建列/重命名列/新建任务校验失败 → 内嵌红字 + 弹窗不关 + 输入清除（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto seed = env.svc->listColumns();
    DOCTEST_REQUIRE(seed.isOk());
    DOCTEST_REQUIRE(seed.value().size() >= 2);
    const std::string dupTitle = seed.value()[1].title;   // 已存在列名，撞名素材

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());

    auto findTitle = [&](const std::string& want) -> bool {
        auto l = env.svc->listColumns();
        if (!l.isOk()) return false;
        for (const auto& c : l.value()) {
            if (c.title == want) return true;
        }
        return false;
    };
    auto taskTitleExists = [&](const std::string& want) -> bool {
        auto l = env.svc->listAllTasks();
        if (!l.isOk()) return false;
        for (const auto& tk : l.value()) {
            if (tk.title == want) return true;
        }
        return false;
    };

    eui::KeyEvent selectAll;
    selectAll.key = eui::InputKey::A;
    selectAll.action = eui::KeyAction::Press;
    test_support::setShortcutModifier(selectAll.modifiers);
    eui::KeyEvent del;
    del.key = eui::InputKey::Delete;
    del.action = eui::KeyAction::Press;

    // 注意：refresh 重建元素树，所有旧 Element* 悬垂——每次交互前重新 find

    // A. 重命名列空名 → 内嵌红字 + 弹窗不关
    auto* pen = frame.ui.find("todo.col.0.rename.bg");
    DOCTEST_REQUIRE(pen != nullptr);
    pen->onClick();
    frame.refresh();
    auto* rinput = frame.ui.find("todo.dialog.rename.input.hit");
    DOCTEST_REQUIRE(rinput != nullptr);
    DOCTEST_REQUIRE(rinput->onKeyEvent != nullptr);
    rinput->onKeyEvent(selectAll);   // Ctrl+A 全选
    rinput->onKeyEvent(del);         // Delete 清空（onTextInput 空文本是 no-op）
    frame.refresh();
    auto* rsave = frame.ui.find("todo.dialog.rename.confirm.bg");
    DOCTEST_REQUIRE(rsave != nullptr);
    DOCTEST_REQUIRE(rsave->onClick != nullptr);
    rsave->onClick();
    frame.refresh();
    const auto* rerr = frame.ui.find("todo.dialog.rename.error");
    DOCTEST_REQUIRE(rerr != nullptr);   // 错误行只随错误态存在
    DOCTEST_CHECK_EQ(rerr->text, std::string("列标题不能为空"));
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.rename.panel")->opacity, 1.0f);  // 弹窗不关

    // B. 重命名撞名 → 红字；下一次输入清除；改唯一名成功关窗落库
    rinput = frame.ui.find("todo.dialog.rename.input.hit");
    DOCTEST_REQUIRE(rinput != nullptr);
    rinput->onKeyEvent(selectAll);
    eui::TextInputEvent t1;
    t1.text = dupTitle;
    rinput->onTextInput(t1);            // 打成已存在的列名
    frame.refresh();
    rsave = frame.ui.find("todo.dialog.rename.confirm.bg");
    DOCTEST_REQUIRE(rsave != nullptr);
    rsave->onClick();
    frame.refresh();
    rerr = frame.ui.find("todo.dialog.rename.error");
    DOCTEST_REQUIRE(rerr != nullptr);
    DOCTEST_CHECK_EQ(rerr->text, std::string("该列名已存在，请换一个"));
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.rename.panel")->opacity, 1.0f);
    rinput = frame.ui.find("todo.dialog.rename.input.hit");
    rinput->onKeyEvent(selectAll);
    eui::TextInputEvent t2;
    t2.text = "唯一新名";
    rinput->onTextInput(t2);            // 下一次输入 → 错误清除
    frame.refresh();
    DOCTEST_CHECK(frame.ui.find("todo.dialog.rename.error") == nullptr);
    rsave = frame.ui.find("todo.dialog.rename.confirm.bg");
    DOCTEST_REQUIRE(rsave != nullptr);
    rsave->onClick();
    frame.refresh();
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.rename.panel")->opacity, 0.0f);  // 成功才关
    DOCTEST_CHECK(findTitle("唯一新名"));

    // C. 新建列显式输入撞名 → 红字 + 不关（留空默认名路径不受影响）
    auto* addcol = frame.ui.find("todo.btn.addcol.bg");
    DOCTEST_REQUIRE(addcol != nullptr);
    addcol->onClick();
    frame.refresh();
    auto* chit = frame.ui.find("todo.dialog.newcol.input.hit");
    DOCTEST_REQUIRE(chit != nullptr);
    DOCTEST_REQUIRE(chit->onTextInput != nullptr);
    eui::TextInputEvent t3;
    t3.text = "唯一新名";
    chit->onTextInput(t3);
    frame.refresh();
    auto* cconf = frame.ui.find("todo.dialog.newcol.confirm.bg");
    DOCTEST_REQUIRE(cconf != nullptr);
    cconf->onClick();
    frame.refresh();
    const auto* cerr = frame.ui.find("todo.dialog.newcol.error");
    DOCTEST_REQUIRE(cerr != nullptr);
    DOCTEST_CHECK_EQ(cerr->text, std::string("该列名已存在，请换一个"));
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.newcol.panel")->opacity, 1.0f);
    chit = frame.ui.find("todo.dialog.newcol.input.hit");
    DOCTEST_REQUIRE(chit != nullptr);
    chit->onKeyEvent(selectAll);
    eui::TextInputEvent t4;
    t4.text = "迭代列B";
    chit->onTextInput(t4);
    frame.refresh();
    DOCTEST_CHECK(frame.ui.find("todo.dialog.newcol.error") == nullptr);  // 输入清除
    cconf = frame.ui.find("todo.dialog.newcol.confirm.bg");
    DOCTEST_REQUIRE(cconf != nullptr);
    cconf->onClick();
    frame.refresh();
    DOCTEST_CHECK(findTitle("迭代列B"));
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.newcol.panel")->opacity, 0.0f);

    // D. 新建任务空标题 → 红字 + 不关（错误 toast 在弹窗内，不弹到窗外）
    auto* add = frame.ui.find("todo.btn.add.bg");
    DOCTEST_REQUIRE(add != nullptr);
    add->onClick();
    frame.refresh();
    auto* tconf = frame.ui.find("todo.dialog.newtask.confirm.bg");
    DOCTEST_REQUIRE(tconf != nullptr);
    tconf->onClick();          // 空标题直接提交
    frame.refresh();
    const auto* terr = frame.ui.find("todo.dialog.newtask.error");
    DOCTEST_REQUIRE(terr != nullptr);
    DOCTEST_CHECK_EQ(terr->text, std::string("任务标题不能为空"));
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.newtask.panel")->opacity, 1.0f);
    auto* tinput = frame.ui.find("todo.dialog.newtask.input.hit");
    DOCTEST_REQUIRE(tinput != nullptr);
    DOCTEST_REQUIRE(tinput->onTextInput != nullptr);
    eui::TextInputEvent t5;
    t5.text = "任务甲";
    tinput->onTextInput(t5);
    frame.refresh();
    DOCTEST_CHECK(frame.ui.find("todo.dialog.newtask.error") == nullptr);  // 输入清除
    tconf = frame.ui.find("todo.dialog.newtask.confirm.bg");
    DOCTEST_REQUIRE(tconf != nullptr);
    tconf->onClick();
    frame.refresh();
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.newtask.panel")->opacity, 0.0f);
    DOCTEST_CHECK(taskTitleExists("任务甲"));
    return true;
})

// 创建/编辑弹窗 Esc=取消（onCancel，不是误提交）

REGISTER_TEST(todo_ui_esc_cancel_dialogs,
              "todo_ui: 新建任务/重命名列 Esc=取消不落库；详情标签 Esc=清草稿（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    auto taskExists = [&](const std::string& title) -> bool {
        auto ts = env.svc->listAllTasks();
        if (!ts.isOk()) return false;
        for (const auto& t : ts.value()) {
            if (t.title == title) return true;
        }
        return false;
    };
    auto columnTitle = [&](size_t idx) -> std::string {
        auto cols = env.svc->listColumns();
        if (!cols.isOk() || idx >= cols.value().size()) return std::string("<缺失>");
        return cols.value()[idx].title;
    };
    const std::string escTask = "Esc不应建的任务";

    Frame frame(mod.get());

    // 1) 新建任务：输入标题 → Esc（onCancel）→ 弹窗关、不落库
    {
        auto* add = frame.ui.find("todo.btn.add.bg");
        DOCTEST_REQUIRE(add != nullptr);
        add->onClick();
        frame.refresh();
        auto* hit = frame.ui.find("todo.dialog.newtask.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = escTask;
        hit->onTextInput(t);
        frame.refresh();
        hit = frame.ui.find("todo.dialog.newtask.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(hit->onKeyEvent(esc));   // 被 onCancel 消费
        frame.refresh();
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.newtask.panel")->opacity, 0.0f);
        DOCTEST_CHECK(!taskExists(escTask));   // 旧行为：Esc→onEnter=submitNewTask 会落库
    }

    // 2) 重命名列：改标题 → Esc → 弹窗关、标题不变
    {
        const std::string before = columnTitle(0);
        auto* pen = frame.ui.find("todo.col.0.rename.bg");
        DOCTEST_REQUIRE(pen != nullptr);
        pen->onClick();
        frame.refresh();
        auto* hit = frame.ui.find("todo.dialog.rename.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = "Esc不应改的名";
        hit->onTextInput(t);
        frame.refresh();
        hit = frame.ui.find("todo.dialog.rename.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(hit->onKeyEvent(esc));
        frame.refresh();
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.rename.panel")->opacity, 0.0f);
        DOCTEST_CHECK_EQ(columnTitle(0), before);
    }

    // 3) 详情标签输入框：打半截 → Esc → 草稿清空（不误提交成标签），弹窗不关
    {
        // 任务经「新建任务」弹窗 UI 路径创建（直接调 service 不进模块内存态）
        auto* add = frame.ui.find("todo.btn.add.bg");
        DOCTEST_REQUIRE(add != nullptr);
        add->onClick();
        frame.refresh();
        auto* nhit = frame.ui.find("todo.dialog.newtask.input.hit");
        DOCTEST_REQUIRE(nhit != nullptr);
        eui::TextInputEvent t0;
        t0.text = "详情标签任务";
        nhit->onTextInput(t0);
        frame.refresh();
        auto* nconf = frame.ui.find("todo.dialog.newtask.confirm.bg");
        DOCTEST_REQUIRE(nconf != nullptr);
        nconf->onClick();
        frame.refresh();
        frame.refresh();   // 列表重载（提交后下一帧才见卡片）
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 打开详情弹窗
        frame.refresh();
        auto* thit = frame.ui.find("todo.dialog.detail.tag.input.hit");
        DOCTEST_REQUIRE(thit != nullptr);
        DOCTEST_REQUIRE(thit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = "半截标签";
        thit->onTextInput(t);
        frame.refresh();
        const auto* shown = frame.ui.find("todo.dialog.detail.tag.input.text");
        DOCTEST_REQUIRE(shown != nullptr);
        DOCTEST_CHECK_EQ(shown->text, std::string("半截标签"));
        auto* thit2 = frame.ui.find("todo.dialog.detail.tag.input.hit");
        DOCTEST_REQUIRE(thit2 != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(thit2->onKeyEvent(esc));    // onCancel=清草稿
        frame.refresh();
        // 草稿清空 → 输入框回 placeholder 显示；详情弹窗保持打开；标签未进草稿
        const auto* ph = frame.ui.find("todo.dialog.detail.tag.input.text");
        DOCTEST_REQUIRE(ph != nullptr);
        DOCTEST_CHECK_EQ(ph->text, std::string("输入标签，回车添加…"));
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.tag.0.bg") == nullptr);  // 草稿未成 chip
    }

    mod->onStop();
    return true;
})

// Esc 关最上层弹窗（模块级兜底）+ 焦点输入框优先让位
// 通道 = Module::onKeyEvent（app 级 onKeyEvent → Router 派发到当前页，与
// todo_ui_keyboard_move 同款直调）。派发序见上游 core/runtime/runtime_input.h
// updateKeyInput：焦点元素 onKeyEvent 返回 true 则不再回调本通道。
REGISTER_TEST(todo_ui_esc_closes_topmost_dialog,
              "todo_ui: Esc 关最上层弹窗（确认类无输入框）+ 输入框聚焦让位 onCancel", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("Esc列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "Esc待删任务");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    auto escPress = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::Escape;
        e.action = eui::KeyAction::Press;
        return e;
    };

    // 0) 无弹窗：不消费（兜底不越权吃键）
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    // 1) 确认类弹窗（无输入框，Esc 修复前完全无效）：右键卡片 → 删除 → 确认
    //    弹窗 → Esc 关闭 = 取消，任务不落删
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onContextMenu != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const auto* del = frame.ui.find("todo.ctx.level.0.item.7");  // 5 列时末项=删除
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        del->onClick();
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.dialog.delcol.panel") != nullptr);
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.delcol.panel")->opacity, 1.0f);
        DOCTEST_CHECK(mod->onKeyEvent(escPress()));   // 兜底消费
        frame.refresh();
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.delcol.panel")->opacity, 0.0f);
    }
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value().size(), static_cast<size_t>(1));  // Esc=取消，未删
    }

    // 2) 输入框聚焦：Esc 走 input.onCancel（清标签草稿），模块兜底让位不重复
    //    —— 详情弹窗保持打开（若两条路径都跑了 = 重复处理，这里会看到弹窗被关）
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 打开详情弹窗
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.dialog.detail.panel") != nullptr);
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);
        auto* thit = frame.ui.find("todo.dialog.detail.tag.input.hit");
        DOCTEST_REQUIRE(thit != nullptr);
        DOCTEST_REQUIRE(thit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = "半截草稿";
        thit->onTextInput(t);
        frame.refresh();
        thit = frame.ui.find("todo.dialog.detail.tag.input.hit");
        DOCTEST_REQUIRE(thit != nullptr);
        // 模拟 runtime 派发序：焦点元素优先，未消费才轮到模块兜底
        const bool consumed = thit->onKeyEvent(escPress());
        DOCTEST_CHECK(consumed);   // input 消费（onCancel 清草稿）
        if (!consumed) mod->onKeyEvent(escPress());
        frame.refresh();
        const auto& st =
            frame.ui.state<components::input_detail::InputModel::InputState>(
                "todo.dialog.detail.tag.input");
        DOCTEST_CHECK_EQ(st.text, std::string(""));   // 草稿确由 onCancel 清掉
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);
    }

    // 3) 详情弹窗无未存改动：Esc = 兜底关最上层
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    {
        Frame frame(mod.get());
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 0.0f);
    }

    mod->onStop();
    return true;
})

// 弹窗打开栈（「谁在最上层」的唯一持有者）
// 断言两件事：① 绘制 z 序由打开顺序给出（不再手挑 zIndex）——与栈序一致；
// ② Esc 关栈顶 = 后开先关：依次开 A=详情 / B=日期 / C=确认，连按 Esc 关序 C→B→A。
// 三层嵌套由各弹窗自己的开函数驱动（无头直调回调，跳过遮罩命中判定）。

namespace {
// 顶层元素的绘制位次（orderedRoots 按子树 z 上界升序排好 = 绘制序，
// 见 EUI-NEO core/dsl.h rebuildOrderedElements；弹窗各自是顶层元素）
int paintOrderOf(const core::dsl::Ui& ui, const core::dsl::Element* e) {
    const auto& order = ui.orderedRoots();
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == e) return static_cast<int>(i);
    }
    return -1;
}
} // namespace

REGISTER_TEST(todo_ui_esc_stack_lifo,
              "todo_ui: 弹窗打开栈 LIFO——z 序随打开顺序，连按 Esc 关序 C→B→A（审计 THS-05）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("栈列");
    DOCTEST_REQUIRE(col.isOk());
    DOCTEST_REQUIRE(env.svc->createTask(col.value().id, "栈任务").isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    auto escPress = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::Escape;
        e.action = eui::KeyAction::Press;
        return e;
    };

    // 0) 栈空：Esc 不消费（兜底不越权吃键）
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    // 目标列索引（模块自建默认列在前，按 service 顺序定位）
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int colIdx = -1;
    for (size_t i = 0; i < cols.value().size(); ++i) {
        if (cols.value()[i].id == col.value().id) colIdx = static_cast<int>(i);
    }
    DOCTEST_REQUIRE(colIdx >= 0);

    Frame frame(mod.get());

    // A = 详情（点卡片）
    {
        const auto* card =
            frame.ui.find("todo.col." + std::to_string(colIdx) + ".card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.dialog.detail.panel") != nullptr);
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);
    }

    // B = 日期选择（详情「选择日期…」）
    {
        auto* pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        DOCTEST_REQUIRE(pick->onClick != nullptr);
        pick->onClick();
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.datepicker.panel") != nullptr);
        DOCTEST_CHECK_EQ(frame.ui.find("todo.datepicker.panel")->opacity, 1.0f);
    }

    // C = 删除确认（详情「删除」）
    {
        auto* del = frame.ui.find("todo.dialog.detail.delete.bg");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        del->onClick();
        frame.refresh();
        DOCTEST_REQUIRE(frame.ui.find("todo.dialog.delcol.panel") != nullptr);
        DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.delcol.panel")->opacity, 1.0f);
    }

    // 三层同时在：z 序 = 打开顺序（A < B < C），且整栈仍在 toast 之下
    const auto* dlgDetail = frame.ui.find("todo.dialog.detail");
    const auto* dlgDate = frame.ui.find("todo.datepicker");
    const auto* dlgConfirm = frame.ui.find("todo.dialog.delcol");
    DOCTEST_REQUIRE(dlgDetail != nullptr);
    DOCTEST_REQUIRE(dlgDate != nullptr);
    DOCTEST_REQUIRE(dlgConfirm != nullptr);
    DOCTEST_CHECK(dlgDetail->zIndex < dlgDate->zIndex);
    DOCTEST_CHECK(dlgDate->zIndex < dlgConfirm->zIndex);
    DOCTEST_CHECK(dlgConfirm->zIndex < 1100);   // toast（EUI-NEO 默认 1100）恒在上

    // 渲染序与 z 序一致（不是「手挑 zIndex」而是同源的栈序）
    frame.ui.layout(1600.0f, 1000.0f);
    const int orderDetail = paintOrderOf(frame.ui, dlgDetail);
    const int orderDate = paintOrderOf(frame.ui, dlgDate);
    const int orderConfirm = paintOrderOf(frame.ui, dlgConfirm);
    DOCTEST_REQUIRE(orderDetail >= 0);
    DOCTEST_REQUIRE(orderDate >= 0);
    DOCTEST_REQUIRE(orderConfirm >= 0);
    DOCTEST_CHECK(orderDetail < orderDate);
    DOCTEST_CHECK(orderDate < orderConfirm);

    // Esc①：关 C（确认删除），B / A 保持打开
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame.refresh();
    DOCTEST_REQUIRE(frame.ui.find("todo.dialog.delcol.panel") != nullptr);
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.delcol.panel")->opacity, 0.0f);
    DOCTEST_CHECK(frame.ui.find("todo.datepicker") != nullptr);   // B 仍在
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);
    {
        auto ts = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value().size(), static_cast<size_t>(1));   // Esc=取消，未删
    }

    // Esc②：关 B（日期选择），A 仍打开
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame.refresh();
    DOCTEST_CHECK(frame.ui.find("todo.datepicker") == nullptr);   // 关后不再入树
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 1.0f);

    // Esc③：关 A（详情；无未存改动 → 一击即关）
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame.refresh();
    DOCTEST_CHECK_EQ(frame.ui.find("todo.dialog.detail.panel")->opacity, 0.0f);

    // Esc④：栈空，回到不消费
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    mod->onStop();
    return true;
})

// 看板几何 / 查找 / chip 估宽纯函数的取值口径。
// 这些函数在 todo_ui_logic.h，这里钉住的是取值口径本身（几何公式 / 查找语义 /
// chip 估宽），不是新增行为。
REGISTER_TEST(todo_ui_r5_pure_helpers,
              "todo_ui: R5 看板几何/查找纯函数取值口径（搬运自匿名 ns）", {
    using namespace module::todo;
    const core::theme::Tokens& t = core::theme::current();

    // 板面几何：pad = spacingXXLarge、列间隙 = spacingLarge、
    // 顶边 = pad + kHeaderH + spacingLarge，剩余宽度按列数均分
    const ui::BoardGeom g3 = ui::boardGeom(1600.0f, 3, t);
    DOCTEST_CHECK_EQ(g3.pad, t.sizes.spacingXXLarge);
    DOCTEST_CHECK_EQ(g3.colGap, t.sizes.spacingLarge);
    const float contentW = 1600.0f - t.sizes.spacingXXLarge * 2.0f;
    DOCTEST_CHECK_EQ(g3.colW, (contentW - t.sizes.spacingLarge * 2.0f) / 3.0f);
    DOCTEST_CHECK_EQ(g3.boardTop, g3.pad + ui_kit::kHeaderH + t.sizes.spacingLarge);
    // 零列不除零（按 1 列算，与 drawBoard 空态分支同款兜底）
    DOCTEST_CHECK_EQ(ui::boardGeom(1600.0f, 0, t).colW, contentW);

    // 列 / 卡片左上角：列步进 = 列宽 + 间隙，卡步进 = 卡高 + 卡间距
    DOCTEST_CHECK_EQ(ui::cardLeft(g3, 0), g3.pad);
    DOCTEST_CHECK_EQ(ui::cardLeft(g3, 2), g3.pad + 2.0f * (g3.colW + g3.colGap));
    DOCTEST_CHECK_EQ(ui::cardTop(g3, 0), g3.boardTop + ui::kColumnHeaderHeight);
    DOCTEST_CHECK_EQ(ui::cardTop(g3, 3),
                     g3.boardTop + ui::kColumnHeaderHeight +
                         3.0f * (ui::kCardHeight + ui::kCardGap));

    // chip 估宽：基础 20 + ASCII 6.6/字节、非 ASCII 4.8/字节（UTF-8 下汉字 3
    // 字节）——字号 10 → 12 后每字节估值同比放大 ×1.2（5.5→6.6 /
    // 4→4.8），基数 20（左右各 10px 留白）不变。期望值写死（不是 chipWidth 内部
    // 常量的复述）：改估值或忘改即红——忘改会小于真实字形宽（font-em 用例复核）
    DOCTEST_CHECK_EQ(ui::chipWidth(""), 20.0f);
    DOCTEST_CHECK_EQ(ui::chipWidth("ab"), 20.0f + 6.6f + 6.6f);
    DOCTEST_CHECK_EQ(ui::chipWidth("中"), 20.0f + 4.8f + 4.8f + 4.8f);

    // 查找辅助：列内任务按 position 升序、任务/列按 id、语义列按 kind
    std::vector<Task> tasks;
    tasks.push_back(makeTask(1, 10, 3));
    tasks.push_back(makeTask(2, 10, 1));
    tasks.push_back(makeTask(3, 20, 0));
    const auto colTasks = ui::tasksInColumn(tasks, 10);
    DOCTEST_REQUIRE(colTasks.size() == 2);
    DOCTEST_CHECK_EQ(colTasks[0]->id, 2);
    DOCTEST_CHECK_EQ(colTasks[1]->id, 1);
    DOCTEST_CHECK(ui::findTask(tasks, 3) != nullptr);
    DOCTEST_CHECK(ui::findTask(tasks, 99) == nullptr);

    std::vector<Column> cols;
    Column c{};
    c.id = 7; c.title = "待办"; c.kind = "todo"; c.position = 0;
    cols.push_back(c);
    c.id = 8; c.title = "已完成"; c.kind = "done"; c.position = 1;
    cols.push_back(c);
    DOCTEST_CHECK(ui::findColumn(cols, 8) != nullptr);
    DOCTEST_CHECK(ui::findColumn(cols, 9) == nullptr);
    DOCTEST_CHECK_EQ(ui::findColumnByKind(cols, "done"), 8);
    DOCTEST_CHECK_EQ(ui::findColumnByKind(cols, "archive"), 0);   // 找不到 = 0
    DOCTEST_CHECK(ui::columnTitleTaken(cols, "待办", 0));
    DOCTEST_CHECK(!ui::columnTitleTaken(cols, "待办", 7));        // 改名为自己不算撞
    DOCTEST_CHECK_EQ(ui::defaultColumnTitle(cols.size()), "新列 3");

    // 列内滚动按列 id 记忆（列重排/删列不串列）；无记录 / 越界 = 0
    std::map<int64_t, float> scrolls;
    scrolls[8] = 42.0f;
    DOCTEST_CHECK_EQ(ui::columnScrollOffset(cols, scrolls, 0), 0.0f);
    DOCTEST_CHECK_EQ(ui::columnScrollOffset(cols, scrolls, 1), 42.0f);
    DOCTEST_CHECK_EQ(ui::columnScrollOffset(cols, scrolls, 2), 0.0f);

    // 优先级标签下标 = 枚举值；越界（脏数据）走 None；无截止日期不逾期
    DOCTEST_CHECK_EQ(std::string(ui::priorityLabel(Priority::High)), "●高");
    DOCTEST_CHECK_EQ(std::string(ui::priorityLabel(static_cast<Priority>(99))), "无");
    DOCTEST_CHECK_EQ(ui::formatDue(0), std::string());
    DOCTEST_CHECK(!ui::isOverdue(Task{}));
    return true;
})

// M4 Task 1：todo 动作 → EventBus 事件。发布点在 module UI 层（doCreateTask /
// doCompleteTask 成功分支），headless 走真实 UI 回调链验证：新建任务弹窗提交 →
// todo.created；详情弹窗「标记完成」→ todo.completed。
REGISTER_TEST(todo_ui_event_wiring,
              "todo_ui: 建待办→todo.created、标记完成→todo.completed（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    int gotCreated = 0, gotCompleted = 0;
    auto tc = ::core::event::bus().subscribe(
        ::core::event::kTodoCreated, [&](const std::string&) { ++gotCreated; });
    auto tk = ::core::event::bus().subscribe(
        ::core::event::kTodoCompleted, [&](const std::string&) { ++gotCompleted; });

    // 1) 新建任务弹窗：输入标题 → 提交 → doCreateTask 成功分支发布 todo.created
    {
        Frame frame(mod.get());
        auto* add = frame.ui.find("todo.btn.add.bg");
        DOCTEST_REQUIRE(add != nullptr);
        add->onClick();
        frame.refresh();
        auto* hit = frame.ui.find("todo.dialog.newtask.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = "事件任务";
        hit->onTextInput(t);
        frame.refresh();
        auto* conf = frame.ui.find("todo.dialog.newtask.confirm.bg");
        DOCTEST_REQUIRE(conf != nullptr);
        conf->onClick();
        frame.refresh();
        DOCTEST_CHECK_EQ(gotCreated, 1);   // UI 路径成功即发布
    }

    // 2) 详情弹窗「标记完成」→ doCompleteTask 成功分支发布 todo.completed
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 打开详情弹窗
        frame.refresh();
        const auto* toggle = frame.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        toggle->onClick();  // 标记完成
        DOCTEST_CHECK_EQ(gotCompleted, 1);
    }
    return true;
})

// 写路径事件补齐：GitHub Sync 的 dirty 触发源要求「待办增删改」每个写路径都发事件
//（只订 created/completed 时，改标题、换列、删任务都不会触发同步）。
// headless 走真实 UI 回调链（与 todo_ui_event_wiring 同款）：详情「保存」→ todo.updated、
// 详情「取消完成」→ todo.uncompleted、右键「移动到」→ todo.moved、删除确认 → todo.deleted。
REGISTER_TEST(todo_ui_event_wiring_write_paths,
              "todo_ui: 编辑保存→todo.updated、取消完成→todo.uncompleted、换列→todo.moved、删除→todo.deleted", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(cols.value().size() >= 3u);
    const int64_t todoCol = cols.value()[0].id;
    const int64_t doneCol = cols.value()[2].id;   // kind=done

    auto a = env.svc->createTask(todoCol, "写路径任务");
    DOCTEST_REQUIRE(a.isOk());
    // 已完成任务：详情里 toggle 走的是「取消完成」分支
    auto b = env.svc->createTask(todoCol, "已完成任务");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(env.svc->completeTask(b.value().id).isOk());
    DOCTEST_REQUIRE(env.svc->moveTask(b.value().id, doneCol, 0).isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    int gotUpdated = 0, gotMoved = 0, gotUncompleted = 0, gotDeleted = 0;
    auto su = ::core::event::bus().subscribe(
        ::core::event::kTodoUpdated, [&](const std::string&) { ++gotUpdated; });
    auto sm = ::core::event::bus().subscribe(
        ::core::event::kTodoMoved, [&](const std::string&) { ++gotMoved; });
    auto sn = ::core::event::bus().subscribe(
        ::core::event::kTodoUncompleted,
        [&](const std::string&) { ++gotUncompleted; });
    auto sd = ::core::event::bus().subscribe(
        ::core::event::kTodoDeleted, [&](const std::string&) { ++gotDeleted; });

    // 1) 详情「保存」（标题未改也走完整 updateTask 链路）→ todo.updated
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
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
        DOCTEST_CHECK_EQ(gotUpdated, 1);
    }

    // 2) 已完成任务详情 toggle = 「取消完成」→ todo.uncompleted（标记完成是另一个事件）
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.2.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame frame(mod.get());
        const auto* toggle = frame.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        toggle->onClick();
        DOCTEST_CHECK_EQ(gotUncompleted, 1);
    }

    // 3) 右键「移动到「<下一列标题>」」→ todo.moved
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        card->onContextMenu(pe, rect);
        frame.refresh();
        const std::string want = "移动到「" + cols.value()[1].title + "」";
        bool clicked = false;
        for (int i = 2; i < 14; ++i) {
            const auto* item =
                frame.ui.find("todo.ctx.level.0.item." + std::to_string(i));
            if (item == nullptr || item->onClick == nullptr) break;
            const auto* label =
                frame.ui.find("todo.ctx.level.0.label." + std::to_string(i));
            if (label != nullptr && label->text == want) {
                item->onClick();
                clicked = true;
                break;
            }
        }
        DOCTEST_REQUIRE_MESSAGE(clicked, "右键菜单里没找到 " + want);
        DOCTEST_CHECK_EQ(gotMoved, 1);
    }

    // 4) 详情「删除」→ 确认弹窗「删除」→ todo.deleted（取消路径不发，见 md 侧同款断言）
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.1.card.0.bg");   // 已移到下一列
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame frame(mod.get());
        const auto* del = frame.ui.find("todo.dialog.detail.delete.bg");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        del->onClick();   // 只弹确认，不直接删
        DOCTEST_CHECK_EQ(gotDeleted, 0);
    }
    {
        Frame frame(mod.get());
        const auto* confirm = frame.ui.find("todo.dialog.delcol.confirm.bg");
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_REQUIRE(confirm->onClick != nullptr);
        confirm->onClick();
        DOCTEST_CHECK_EQ(gotDeleted, 1);
    }

    mod->onStop();
    return true;
})

// 列写路径事件补齐：建列 / 改列名 / 移列 / 改色 / 删列五条都改待办数据，GitHub Sync 的
// dirty 触发源必须覆盖。最高风险 = 删除列：外键 CASCADE 连带删掉列下全部任务，不置 dirty
// 则任务从看板消失而同步端毫不知情。
// 列操作共用 todo.column_changed（没有订阅方需要区分这五种列改动，见 core/event_bus.h）。
REGISTER_TEST(todo_ui_event_wiring_columns,
              "todo_ui: 建列/改列名/移列/改色/删列→todo.column_changed（删列级联任务也置位）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    // 待删列由 service 直接建（不经 UI，免与下面的计数混）：列下挂一个任务验级联
    auto doomed = env.svc->createColumn("级联列");
    DOCTEST_REQUIRE(doomed.isOk());
    const int64_t doomedId = doomed.value().id;
    DOCTEST_REQUIRE(env.svc->createTask(doomedId, "随列消失的任务").isOk());

    auto cols0 = env.svc->listColumns();
    DOCTEST_REQUIRE(cols0.isOk());
    DOCTEST_REQUIRE_EQ(cols0.value().size(), static_cast<size_t>(5));  // 4 默认 + 级联列
    DOCTEST_REQUIRE_EQ(cols0.value()[4].id, doomedId);                 // createColumn 追加在末位

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    int gotColumn = 0;
    auto sc = ::core::event::bus().subscribe(
        ::core::event::kTodoColumnChanged, [&](const std::string&) { ++gotColumn; });

    // 1) 「+ 列」→ 命名弹窗 → 确认 → doCreateColumn 成功分支发布
    {
        Frame frame(mod.get());
        const auto* add = frame.ui.find("todo.btn.addcol.bg");
        DOCTEST_REQUIRE(add != nullptr);
        DOCTEST_REQUIRE(add->onClick != nullptr);
        add->onClick();
    }
    {
        Frame frame(mod.get());
        const auto* hit = frame.ui.find("todo.dialog.newcol.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = "事件列";
        hit->onTextInput(t);
    }
    {
        Frame frame(mod.get());
        const auto* conf = frame.ui.find("todo.dialog.newcol.confirm.bg");
        DOCTEST_REQUIRE(conf != nullptr);
        DOCTEST_REQUIRE(conf->onClick != nullptr);
        conf->onClick();
        DOCTEST_CHECK_EQ(gotColumn, 1);
    }
    {
        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        DOCTEST_REQUIRE_EQ(cols.value().size(), static_cast<size_t>(6));
        DOCTEST_CHECK_EQ(cols.value()[5].title, std::string("事件列"));
    }

    // 2) 列头右键 →「重命名列」→ 弹窗改名 → 确认 → doRenameColumn 成功分支发布
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* head = frame.ui.find("todo.col.0.head");
        DOCTEST_REQUIRE(head != nullptr);
        DOCTEST_REQUIRE(head->onContextMenu != nullptr);
        head->onContextMenu(pe, rect);
        frame.refresh();
        const auto* label = frame.ui.find("todo.ctx.level.0.label.0");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("重命名列"));
        const auto* item = frame.ui.find("todo.ctx.level.0.item.0");
        DOCTEST_REQUIRE(item != nullptr);
        DOCTEST_REQUIRE(item->onClick != nullptr);
        item->onClick();
    }
    {
        Frame frame(mod.get());
        // 弹窗预填原列名且光标在开头：直接打字会插在原名前 → 先全选再输入 = 整段替换
        auto& st = frame.ui.state<components::input_detail::InputModel::InputState>(
            "todo.dialog.rename.input");
        st.cursor = st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        const auto* hit = frame.ui.find("todo.dialog.rename.input.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        eui::TextInputEvent t;
        t.text = "改名列";
        hit->onTextInput(t);
    }
    {
        Frame frame(mod.get());
        const auto* conf = frame.ui.find("todo.dialog.rename.confirm.bg");
        DOCTEST_REQUIRE(conf != nullptr);
        DOCTEST_REQUIRE(conf->onClick != nullptr);
        conf->onClick();
        DOCTEST_CHECK_EQ(gotColumn, 2);
    }
    {
        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        DOCTEST_CHECK_EQ(cols.value()[0].title, std::string("改名列"));  // 真改名了，不是插字
    }

    // 3) 列头右键 →「左移列」→ doShiftColumn 成功分支发布
    //（下标 1 左移确有交换；下标 0 左移是 no-op，走「已经在最左边了」分支不发事件）
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* head = frame.ui.find("todo.col.1.head");
        DOCTEST_REQUIRE(head != nullptr);
        DOCTEST_REQUIRE(head->onContextMenu != nullptr);
        head->onContextMenu(pe, rect);
        frame.refresh();
        const auto* label = frame.ui.find("todo.ctx.level.0.label.1");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("左移列"));
        const auto* item = frame.ui.find("todo.ctx.level.0.item.1");
        DOCTEST_REQUIRE(item != nullptr);
        DOCTEST_REQUIRE(item->onClick != nullptr);
        item->onClick();
        DOCTEST_CHECK_EQ(gotColumn, 3);
    }

    // 4) 列头右键 →「删除列」→ 确认弹窗 → 确认 → doDeleteColumn 发布，级联任务一并消失
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* head = frame.ui.find("todo.col.4.head");  // 级联列（左移只动下标 0/1）
        DOCTEST_REQUIRE(head != nullptr);
        DOCTEST_REQUIRE(head->onContextMenu != nullptr);
        head->onContextMenu(pe, rect);
        frame.refresh();
        const auto* label = frame.ui.find("todo.ctx.level.0.label.4");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("删除列"));
        const auto* item = frame.ui.find("todo.ctx.level.0.item.4");
        DOCTEST_REQUIRE(item != nullptr);
        DOCTEST_REQUIRE(item->onClick != nullptr);
        item->onClick();
        frame.refresh();
        const auto* confirm = frame.ui.find("todo.dialog.delcol.confirm.bg");
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_REQUIRE(confirm->onClick != nullptr);
        confirm->onClick();
        DOCTEST_CHECK_EQ(gotColumn, 4);
    }
    {
        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        bool gone = true;
        for (const auto& c : cols.value()) {
            if (c.id == doomedId) gone = false;
        }
        DOCTEST_CHECK(gone);
        auto ts = env.svc->listTasks(doomedId);
        DOCTEST_REQUIRE(ts.isOk());
        DOCTEST_CHECK_EQ(ts.value().size(), static_cast<size_t>(0));  // CASCADE 连带删掉
    }

    // 5) 列头右键 →「改色」子选 → doSetColumnColor 成功分支发布
    //（改色只写 color 列、不动列与任务结构，故单独走一条，别被「列没变」骗过去）
    {
        Frame frame(mod.get());
        eui::PointerEvent pe;
        eui::Rect rect;
        const auto* head = frame.ui.find("todo.col.0.head");
        DOCTEST_REQUIRE(head != nullptr);
        DOCTEST_REQUIRE(head->onContextMenu != nullptr);
        head->onContextMenu(pe, rect);
        frame.refresh();
        const auto* colorItem = frame.ui.find("todo.ctx.level.0.item.3");
        DOCTEST_REQUIRE(colorItem != nullptr);
        DOCTEST_REQUIRE(colorItem->onClick != nullptr);
        colorItem->onClick();   // 打开改色子菜单（主菜单不关）
        frame.refresh();
        const auto* preset0 = frame.ui.find("todo.ctx.level.1.item.0");
        DOCTEST_REQUIRE(preset0 != nullptr);
        DOCTEST_REQUIRE(preset0->onClick != nullptr);
        preset0->onClick();
        DOCTEST_CHECK_EQ(gotColumn, 5);
    }
    {
        // 真落库（事件不是空发）：下标 0 那列的颜色已变成预设 0 = claude-clay
        auto cols = env.svc->listColumns();
        DOCTEST_REQUIRE(cols.isOk());
        DOCTEST_REQUIRE(cols.value()[0].color.has_value());
        DOCTEST_CHECK_EQ(*cols.value()[0].color, std::string("claude-clay"));
    }

    mod->onStop();
    return true;
})
