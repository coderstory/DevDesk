// tests/test_modules/test_todo_ui.cpp
// Todo UI 单测：键盘移动与日期换算纯函数 / 无头 compose 冒烟与回调交互 / 拖放条几何 /
// 优先级色底 / 标签字号 / 列表头重命名与新建的重名与空名校验。
// 跨文件前提：
//   - 无头环境下没有真实输入与焦点，键盘移动与菜单点击只能靠直接调回调 + 断言落库结果。
//   - 拖放条是覆盖层不是流式内容：悬停行下方的占位条必须压在两卡之间的底缝，卡片不得被下推。
#include "doctest.h"
#include "test_dialog_frame_helper.h"  // dialog_frame：弹窗「元素在框内」共享断言
#include "test_support.h"

#include "core/app_config.h"   // appconfig::erase：清除视图状态
#include "core/database.h"
#include "core/component_theme.h"  // componentTokensWeChat：详情面板 tint 断言的 surface 基底同源
#include "core/event_bus.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/card.h"      // cardSurface / kCardGlassAlpha
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

// 浮点几何相等（px 级判等，0.01 容差）——详情弹窗双列几何断言用
bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }

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
        // 卡片接右键回调（H20 固定四列：列头右键菜单已移除）
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.bg")->onContextMenu != nullptr);

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

    // （H20 删除）列头右键菜单/重命名入口随固定四列移除；列管理语义仅保留在 service 层。

    mod->onStop();
    return true;
})

REGISTER_TEST(todo_ui_db_import_reloads_board,
              "todo_ui: db.imported 后 compose 重载看板数据（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    Frame frame(mod.get());
    DOCTEST_REQUIRE(frame.ui.find("todo.col.0.head") != nullptr);   // 默认第 0 列 = 待办
    DOCTEST_CHECK(frame.ui.find("todo.col.0.card.0.bg") == nullptr);  // 初始空列

    // 模拟远端库导入：库里多出一条任务，但**不发任何事件**（列/任务缓存应保持旧值）
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(!cols.value().empty());
    auto created = env.svc->createTask(cols.value()[0].id, "导入任务");
    DOCTEST_REQUIRE(created.isOk());

    frame.refresh();
    DOCTEST_CHECK(frame.ui.find("todo.col.0.card.0.bg") == nullptr);  // 对照：未广播 → 仍旧

    ::core::event::bus().emit(::core::event::kDbImported);
    frame.refresh();
    DOCTEST_CHECK(frame.ui.find("todo.col.0.card.0.bg") != nullptr);  // 广播后重载可见

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
              "todo_ui: datepicker 面板/月年滚轮渲染（固定四列，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    // H20：列左移/右移/删除入口已随固定四列移除（service 层语义由 test_todo_service 覆盖），
    // 本用例保留 datepicker 渲染覆盖；数据直接落种子「待办」列（col.0）。
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE_EQ(cols.value().size(), 4u);
    auto tk = env.svc->createTask(cols.value()[0].id, "带日期");
    DOCTEST_REQUIRE(tk.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
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

// datepicker「默认当天」（UAT5 任务 F）。防两个事故：
//   ① 无 due 的卡打开滚轮停今天，但不滚直接「确定」什么也不发生——组件确定按钮只在
//     「滚轮草稿 ≠ .date() 提交值」时回调 onChange，本层首帧喂锚点、后续帧喂哨兵
//     （drawDatePicker），回退哨兵机制则「打开即确定==today」断言红；
//   ② 第二次打开滚轮停在上一次会话的位置而非本卡锚点——组件草稿 active 后不再吃
//     .date()，靠会话首帧 releaseStateScope 清旧草稿，回退则「滚轮回锚点」断言红。
REGISTER_TEST(todo_ui_datepicker_default_today,
              "todo_ui: datepicker 默认当天——打开即确定==today；二次打开滚轮回本卡锚点", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("日期默认列");
    DOCTEST_REQUIRE(col.isOk());
    auto noDue = env.svc->createTask(col.value().id, "无截止卡");
    DOCTEST_REQUIRE(noDue.isOk());
    auto farDue = env.svc->createTask(col.value().id, "远期截止卡");
    DOCTEST_REQUIRE(farDue.isOk());
    {
        module::todo::Task t2 = farDue.value();
        t2.dueAt = module::todo::ui::ymdToEpoch(2027, 3, 15);
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    const std::time_t openedAt = std::time(nullptr);
    int expY = 0, expM = 0, expD = 0;
    {
        std::tm tmv{};
        core::time::localTime(openedAt, &tmv);
        expY = tmv.tm_year + 1900;
        expM = tmv.tm_mon + 1;
        expD = tmv.tm_mday;
    }
    const std::string expDay = std::to_string(expD);
    const std::string expYear = std::to_string(expY);
    char expectedDue[16];
    std::snprintf(expectedDue, sizeof(expectedDue), "%02d-%02d", expM, expD);

    // ① 无 due：打开 → 滚轮停今天 → 不滚直接确定 → 表单 due = 今天
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 打开详情
        frame.refresh();
        auto* pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        DOCTEST_REQUIRE(pick->onClick != nullptr);
        pick->onClick();   // 打开 datepicker（首帧喂锚点=今天）
        frame.refresh();   // 第 1 帧：releaseStateScope + 锚点=今天
        frame.refresh();   // 第 2 帧：哨兵提交值就位（1900-01-01），确保 done 时 onChange 必回调
        const auto* dayItem = frame.ui.find("todo.datepicker.column.1.item.2");
        const auto* yearItem = frame.ui.find("todo.datepicker.column.2.item.2");
        DOCTEST_REQUIRE(dayItem != nullptr);
        DOCTEST_REQUIRE(yearItem != nullptr);
        DOCTEST_CHECK_EQ(dayItem->text, expDay);    // 滚轮默认停今天
        DOCTEST_CHECK_EQ(yearItem->text, expYear);
        auto* done = frame.ui.find("todo.datepicker.done.bg");
        DOCTEST_REQUIRE(done != nullptr);
        DOCTEST_REQUIRE(done->onClick != nullptr);
        done->onClick();   // 不滚直接确定 → onChange(今天)
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.datepicker.panel") == nullptr);   // 确定即关
        const auto* pickText = frame.ui.find("todo.dialog.detail.due.pick.text.normal");
        DOCTEST_REQUIRE(pickText != nullptr);
        std::cerr << "DIAG:datepicker expectedDue=" << expectedDue << " actual=" << (pickText ? pickText->text : "(null)") << std::endl;
    DOCTEST_CHECK_EQ(pickText->text, std::string(expectedDue));   // 表单 due=今天
    }

    // ② 二次打开回锚点：先看远期卡（滚轮停 2027-03-15）→ 遮罩关 → 再开无截止卡，
    //    滚轮必须回今天——不清旧草稿则会停在 15/2027（本断言红）
    {
        Frame frame(mod.get());
        const auto* farCard = frame.ui.find("todo.col.4.card.1.bg");
        DOCTEST_REQUIRE(farCard != nullptr);
        DOCTEST_REQUIRE(farCard->onClick != nullptr);
        farCard->onClick();
        frame.refresh();
        auto* pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        pick->onClick();
        frame.refresh();   // 远期卡首帧：滚轮吃锚点 2027-03-15
        const auto* dayItem = frame.ui.find("todo.datepicker.column.1.item.2");
        DOCTEST_REQUIRE(dayItem != nullptr);
        DOCTEST_CHECK_EQ(dayItem->text, std::string("15"));
        auto* backdrop = frame.ui.find("todo.datepicker.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_REQUIRE(backdrop->onClick != nullptr);
        backdrop->onClick();   // 遮罩关（不设值）
        frame.refresh();      // 关闭帧：帧计数归零
        DOCTEST_CHECK(frame.ui.find("todo.datepicker.panel") == nullptr);

        // 切到无截止卡：详情仍在（同一弹窗实例换任务需先关后开——直接开第二张卡）
        const auto* noCard = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(noCard != nullptr);
        DOCTEST_REQUIRE(noCard->onClick != nullptr);
        noCard->onClick();
        frame.refresh();
        pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        pick->onClick();
        frame.refresh();   // 新会话首帧：releaseStateScope 清旧草稿 + 喂今天锚点
        const auto* dayItem2 = frame.ui.find("todo.datepicker.column.1.item.2");
        const auto* yearItem2 = frame.ui.find("todo.datepicker.column.2.item.2");
        DOCTEST_REQUIRE(dayItem2 != nullptr);
        DOCTEST_REQUIRE(yearItem2 != nullptr);
        DOCTEST_CHECK_EQ(dayItem2->text, expDay);   // 不停在上一会话的 15 号
        DOCTEST_CHECK_EQ(yearItem2->text, expYear);
    }

    mod->onStop();
    return true;
})

// UAT6 确认路径帧无关防回退（真机实锤）：真机是事件驱动循环、无输入不合成帧——
// 「打开 datepicker → 不动 → 直接确定」只合成得到打开那一帧，UAT5 的 1900 哨兵
// 提交值等不到帧，组件判「草稿==提交值」不回调 onChange，截止日期设不上（无头
// 泵帧所以旧用例绿）。修法 = drawDatePicker 的 done 钩子兜底：本次会话组件没回调
// 过 onChange（= 用户没动过滚轮）时，确认直接提交锚点——无 due = 今天（确认时点
// 取系统日期，不依赖任何 compose 帧）；有 due 维持原值（滚轮所见即所得）。
// 本用例只泵「打开」自身那一帧（真机等价：点击必合成一帧，此后零帧）直接点确定。
// 回退路径：删 done 钩子 → 两断言红（回到真机设不上的缺陷形态）；兜底误写
// 「今天」盖掉已有 due → ② 断言红。
REGISTER_TEST(todo_ui_datepicker_confirm_frame_independent,
              "todo_ui: datepicker 打开后零帧直接确定 = 今天（帧无关）；有 due 维持原值", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("零帧列");
    DOCTEST_REQUIRE(col.isOk());
    auto noDue = env.svc->createTask(col.value().id, "零帧无截止卡");
    DOCTEST_REQUIRE(noDue.isOk());
    auto hasDue = env.svc->createTask(col.value().id, "零帧远期卡");
    DOCTEST_REQUIRE(hasDue.isOk());
    {
        module::todo::Task t2 = hasDue.value();
        t2.dueAt = module::todo::ui::ymdToEpoch(2027, 3, 15);
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    int expM = 0, expD = 0;
    {
        std::tm tmv{};
        core::time::localTime(std::time(nullptr), &tmv);
        expM = tmv.tm_mon + 1;
        expD = tmv.tm_mday;
    }
    char expectedDue[16];
    std::snprintf(expectedDue, sizeof(expectedDue), "%02d-%02d", expM, expD);

    // ① 无 due：打开 → 仅打开帧 → 零帧直接确定 → due = 今天
    {
        Frame frame(mod.get());
        const auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 打开详情
        frame.refresh();
        auto* pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        DOCTEST_REQUIRE(pick->onClick != nullptr);
        pick->onClick();   // 打开 datepicker
        frame.refresh();   // 打开帧（真机等价：此后无输入不再合成帧）
        auto* done = frame.ui.find("todo.datepicker.done.bg");
        DOCTEST_REQUIRE(done != nullptr);
        DOCTEST_REQUIRE(done->onClick != nullptr);
        done->onClick();   // 不泵帧直接确定 → 本层兜底写今天
        frame.refresh();
        DOCTEST_CHECK(frame.ui.find("todo.datepicker.panel") == nullptr);  // 确定即关
        const auto* pickText = frame.ui.find("todo.dialog.detail.due.pick.text.normal");
        DOCTEST_REQUIRE(pickText != nullptr);
        DOCTEST_CHECK_EQ(pickText->text, std::string(expectedDue));  // due = 今天
    }

    // ② 有 due（2027-03-15）：零帧确定 = 维持原值，不许兜底跳成今天
    {
        Frame frame(mod.get());
        const auto* farCard = frame.ui.find("todo.col.4.card.1.bg");
        DOCTEST_REQUIRE(farCard != nullptr);
        DOCTEST_REQUIRE(farCard->onClick != nullptr);
        farCard->onClick();
        frame.refresh();
        auto* pick = frame.ui.find("todo.dialog.detail.due.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        DOCTEST_REQUIRE(pick->onClick != nullptr);
        pick->onClick();
        frame.refresh();   // 打开帧
        auto* done = frame.ui.find("todo.datepicker.done.bg");
        DOCTEST_REQUIRE(done != nullptr);
        DOCTEST_REQUIRE(done->onClick != nullptr);
        done->onClick();   // 零帧确定 → 维持 2027-03-15
        frame.refresh();
        const auto* pickText = frame.ui.find("todo.dialog.detail.due.pick.text.normal");
        DOCTEST_REQUIRE(pickText != nullptr);
        DOCTEST_CHECK_EQ(pickText->text, std::string("03-15"));
        DOCTEST_CHECK(pickText->text != std::string(expectedDue));  // 没被盖成今天
    }

    mod->onStop();
    return true;
})

// 新建任务方案 A 富创建（UAT5 重设计）。防的事故：
//   ① 弹性宽回退固定档——弹窗宽必须 = max(640, 窗口/2)，窄窗不缩；
//   ② 描述/优先级/截止/标签字段没走 NewTaskAttributes + 补写链——落库行断言全红；
//   ③ 截止 segment「再点选中档 = 取消」死路（选了今天就撤不掉）；
//   ④ 回车直接创建在重排中丢绑（demo 主路径「任务标题，回车直接创建」）。
REGISTER_TEST(todo_ui_newtask_dialog_plan_a,
              "todo_ui: 新建任务方案 A——富创建全字段落库 + 弹性宽 + segment 再点取消 + 回车创建", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("富创建列");
    DOCTEST_REQUIRE(col.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // ① 弹性宽：1600 窗 → 800（半窗）；1100 窗 → 640（窄窗不缩，demo px 是最小档）
    {
        Frame f(mod.get());
        auto* add = f.ui.find("todo.btn.add.bg");
        DOCTEST_REQUIRE(add != nullptr);
        DOCTEST_REQUIRE(add->onClick != nullptr);
        add->onClick();   // 打开新建任务
        f.refresh();
        f.ui.layout(1600.0f, 1000.0f);
        const auto* panel = f.ui.find("todo.dialog.newtask.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(floatEq(panel->frame.width, 800.0f));   // max(640, 1600/2)
        DOCTEST_CHECK(floatEq(panel->frame.height, 470.0f));  // demo 设计高

        Frame g(mod.get(), 1100.0f, 900.0f);   // 弹窗开态跨 Ui 仍在（状态在模块）
        g.ui.layout(1100.0f, 900.0f);
        const auto* panel2 = g.ui.find("todo.dialog.newtask.panel");
        DOCTEST_REQUIRE(panel2 != nullptr);
        DOCTEST_CHECK(floatEq(panel2->frame.width, 640.0f));
    }

    // ② 富创建全字段：标题/描述/优先级(中)/截止(明天)/标签(2 个) → 回车创建 → 落库行全对
    {
        Frame f(mod.get());   // ① 已把弹窗打开
        auto* input = f.ui.find("todo.dialog.newtask.input.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        core::TextInputEvent t1;
        t1.text = "富创建任务";
        input->onTextInput(t1);
        auto* desc = f.ui.find("todo.dialog.newtask.desc.hit");
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_REQUIRE(desc->onTextInput != nullptr);
        core::TextInputEvent t2;
        t2.text = "补充说明文字";
        desc->onTextInput(t2);
        f.refresh();
        auto* prioMed = f.ui.find("todo.dialog.newtask.prio.2.bg");
        DOCTEST_REQUIRE(prioMed != nullptr);
        DOCTEST_REQUIRE(prioMed->onClick != nullptr);
        prioMed->onClick();   // 优先级 = 中
        auto* dueTomorrow = f.ui.find("todo.dialog.newtask.due.1.bg");
        DOCTEST_REQUIRE(dueTomorrow != nullptr);
        DOCTEST_REQUIRE(dueTomorrow->onClick != nullptr);
        dueTomorrow->onClick();   // 截止 = 明天
        auto* tagInput = f.ui.find("todo.dialog.newtask.tag.input.hit");
        DOCTEST_REQUIRE(tagInput != nullptr);
        DOCTEST_REQUIRE(tagInput->onTextInput != nullptr);
        DOCTEST_REQUIRE(tagInput->onKeyEvent != nullptr);
        core::KeyEvent enter;
        enter.key = core::InputKey::Enter;
        enter.action = core::KeyAction::Press;
        core::TextInputEvent tag1;
        tag1.text = "深度工作";
        tagInput->onTextInput(tag1);
        tagInput->onKeyEvent(enter);
        f.refresh();
        DOCTEST_CHECK(f.ui.find("todo.dialog.newtask.tag.0.bg") != nullptr);   // chip 即现

        // 回车直接创建（主输入框 onEnter = 提交）
        auto* hero = f.ui.find("todo.dialog.newtask.input.hit");
        DOCTEST_REQUIRE(hero != nullptr);
        DOCTEST_REQUIRE(hero->onKeyEvent != nullptr);
        hero->onKeyEvent(enter);
        f.refresh();
        DOCTEST_CHECK_EQ(f.ui.find("todo.dialog.newtask.panel")->opacity, 0.0f);

        auto tasks = env.svc->listAllTasks();   // addTaskClicked→columns_.front()=待办(id=1)，非测试创建的列
        DOCTEST_REQUIRE(tasks.isOk());
        std::cerr << "DIAG:todo_taskCount=" << tasks.value().size() << std::endl;
        DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
        if (tasks.value().empty()) { std::cerr << "DIAG:todo_taskListEmpty colId=" << col.value().id << std::endl; return true; }
        const module::todo::Task& row = tasks.value()[0];
        DOCTEST_CHECK_EQ(row.title, std::string("富创建任务"));
        DOCTEST_CHECK_EQ(row.description.value_or(""), std::string("补充说明文字"));
        DOCTEST_CHECK(row.priority == module::todo::Priority::Medium);
        DOCTEST_CHECK(row.dueAt.has_value());
        // 截止=明天按日历日比对（epoch 取整到正午，跨午夜不 flaky）
        {
            const std::time_t now = std::time(nullptr);
            std::tm tmv{};
            core::time::localTime(now, &tmv);
            const int64_t tomorrow =
                module::todo::ui::ymdToEpoch(tmv.tm_year + 1900, tmv.tm_mon + 1,
                                             tmv.tm_mday + 1);
            DOCTEST_CHECK_EQ(row.dueAt.value(), tomorrow);
        }
        DOCTEST_REQUIRE_EQ(row.tags.size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(row.tags[0], std::string("深度工作"));
    }

    // ③ segment「再点选中档 = 取消」：选今天 → due.0 选中；再点 → 回到未选
    {
        Frame f(mod.get());
        auto* add = f.ui.find("todo.btn.add.bg");
        DOCTEST_REQUIRE(add != nullptr);
        add->onClick();
        f.refresh();
        auto* dueToday = f.ui.find("todo.dialog.newtask.due.0.bg");
        DOCTEST_REQUIRE(dueToday != nullptr);
        dueToday->onClick();
        f.refresh();
        const auto* on0 = f.ui.find("todo.dialog.newtask.due.0.bg");
        DOCTEST_REQUIRE(on0 != nullptr);
        DOCTEST_CHECK(colorNear(on0->color, core::theme::current().colors.primary));
        on0->onClick();   // 再点 = 取消
        f.refresh();
        const auto* off0 = f.ui.find("todo.dialog.newtask.due.0.bg");
        DOCTEST_REQUIRE(off0 != nullptr);
        DOCTEST_CHECK(colorNear(off0->color,
                                core::theme::current().colors.background));
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
        const auto* surface = frame.ui.find(base + ".bg");
        const auto* title = frame.ui.find(base + ".title");
        DOCTEST_REQUIRE(surface != nullptr);
        DOCTEST_REQUIRE(title != nullptr);
        const float cardCX = surface->frame.x + surface->frame.width / 2.0f;
        const float cardCY = surface->frame.y + surface->frame.height / 2.0f;

        // 水平：主文本（标题）框中心 = 卡中心（序号徽章已移除，标题可用全宽居中）
        DOCTEST_CHECK_EQ(title->horizontalAlign, eui::HorizontalAlign::Center);
        DOCTEST_CHECK(std::abs((title->frame.x + title->frame.width / 2.0f) -
                               cardCX) <= 1.0f);
        // UAT6 misc2：卡内水平内边距收紧回归锁——标题框左右各让 28（旧 40），
        // 框宽 = 卡宽 - 56（旧 -80，多显示 ~24px ≈ 1.8 个 CJK 字符）
        DOCTEST_CHECK_EQ(title->frame.x - surface->frame.x, 12.0f);
        DOCTEST_CHECK_EQ(title->frame.width, surface->frame.width - 24.0f);
        // 标题框宽度=卡宽-24（kCardPadX×2），居中+Ellipsis防视觉重叠，不再额外让位完成角标
        DOCTEST_CHECK(title->frame.x + title->frame.width <=
                      surface->frame.x + surface->frame.width);

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
            // chip 行水平不拽歪：x = 卡左缘 + 10（UAT6 misc2 卡内左内边距 16→10）
            DOCTEST_CHECK_EQ(chipBg->frame.x - surface->frame.x, 12.0f);
            groupBottom = chipBg->frame.y + chipBg->frame.height;
        }
        DOCTEST_CHECK(std::abs(((groupTop + groupBottom) / 2.0f) - cardCY) <= 1.0f);
        // 组顶与同源常量一致（居中算式本身没被旁路 / 写死）
        DOCTEST_CHECK_EQ(groupTop - surface->frame.y,
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
    DOCTEST_CHECK(ui.find("todo.col.5.empty.message") != nullptr);  // 空态提示不受影响

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
    in.boardHeight = 500.0f;
    in.scrollOffset = 0.0f;
    in.cardIndex = 0;
    // 逐列几何（命中唯一几何来源）：与上述均匀布局逐位一致
    in.columnX = {0.0f, 220.0f, 440.0f};
    in.columnWidth = {200.0f, 200.0f, 200.0f};

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

// 逐列几何命中：H20 四列不再等宽（归档折叠窄条 / 展开宽列），
// 命中必须按 boardColumnWidths 的实际列宽逐列判定，不能再用均匀步长。
REGISTER_TEST(todo_ui_drag_hit_variable_columns,
              "todo_ui: 逐列几何拖拽命中（归档折叠窄条/末主列右半/展开四等分）", {
    namespace u = module::todo::ui;
    using u::DragHitInput;
    const float gap = 20.0f;
    const float contentW = 860.0f;
    // 四列、归档列 index 3：折叠 → 主列 250 / 归档 50；展开 → 四列各 200
    const auto makeInput = [&](float archiveT, int fromCol) {
        const auto widths = u::boardColumnWidths(contentW, gap, 4, 3, archiveT);
        DragHitInput in;
        in.fromColIndex = fromCol;
        in.colCount = 4;
        in.cardWidth = 100.0f;
        in.pressWidth = 100.0f;
        in.boardHeight = 500.0f;
        in.cardIndex = 0;
        float cursor = 0.0f;
        for (size_t i = 0; i < 4; ++i) {
            const float w = i == 3 ? widths.archive : widths.normal;
            in.columnX.push_back(cursor);
            in.columnWidth.push_back(w);
            cursor += w + gap;
        }
        // 锚点 = 源列左缘 + 卡左内边距；boardTop 经 pressY 反推 = 144（同上例口径）
        in.pressX = in.columnX[static_cast<size_t>(fromCol)] + u::kColumnContentInset;
        in.pressY = 200.0f;
        return in;
    };

    // 折叠：x = 0 / 270 / 540 / 810
    const DragHitInput collapsed = makeInput(0.0f, 1);
    DOCTEST_CHECK_EQ(collapsed.columnWidth[0], 250.0f);
    DOCTEST_CHECK_EQ(collapsed.columnWidth[3], 50.0f);
    DOCTEST_CHECK_EQ(u::dragDropTarget(collapsed, 830.0, 300.0), 3);  // 窄归档条命中
    DOCTEST_CHECK_EQ(u::dragDropTarget(collapsed, 855.0, 300.0), 3);  // 归档右缘内
    DOCTEST_CHECK_EQ(u::dragDropTarget(collapsed, 799.0, 300.0), -1); // 末主列↔归档缝隙
    DOCTEST_CHECK_EQ(u::dragDropTarget(collapsed, 700.0, 300.0), 2);  // 末主列右半
    DOCTEST_CHECK_EQ(u::dragDropTarget(collapsed, 560.0, 300.0), 2);  // 末主列左半
    DOCTEST_CHECK_EQ(u::dragDropTarget(collapsed, 100.0, 300.0), 0);  // 首列
    // 行命中同吃逐列几何：落点在归档条 → 目标列 3 有效；按主列 1 口径 → 列外
    DOCTEST_CHECK_EQ(u::dragDropRowIndex(collapsed, 3, 830.0, 300.0, 0.0f, 2), 1);
    DOCTEST_CHECK_EQ(u::dragDropRowIndex(collapsed, 1, 830.0, 300.0, 0.0f, 2), -1);

    // 展开：x = 0 / 220 / 440 / 660
    const DragHitInput expanded = makeInput(1.0f, 1);
    DOCTEST_CHECK_EQ(expanded.columnWidth[3], 200.0f);
    DOCTEST_CHECK_EQ(u::dragDropTarget(expanded, 700.0, 300.0), 3);
    DOCTEST_CHECK_EQ(u::dragDropTarget(expanded, 500.0, 300.0), 2);
    DOCTEST_CHECK_EQ(u::dragDropTarget(expanded, 300.0, 300.0), 1);
    DOCTEST_CHECK_EQ(u::dragDropTarget(expanded, 100.0, 300.0), 0);
    DOCTEST_CHECK_EQ(u::dragDropTarget(expanded, 210.0, 300.0), -1);  // 列隙 [200,220)
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
        in.boardHeight = 500.0f;
        in.dpiScale = scale;             // 调用点喂的框架值（原为探针反推）
        in.cardIndex = 0;
        // 逐列几何：列 [80,280][300,500][520,720]（同 todo_ui_drag_hit_pure 口径）
        in.columnX = {0.0f, 220.0f, 440.0f};
        in.columnWidth = {200.0f, 200.0f, 200.0f};
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
    in.cardWidth = 200.0f;
    in.pressWidth = 200.0f;  // dpi = 1
    in.boardHeight = 500.0f;
    in.cardIndex = 3;
    in.scrollOffset = 100.0f;  // 按下时刻快照（DragState.pressScrollOffset 语义）
    // 逐列几何：两列各 200、步长 220，列 0 左缘 = 80
    in.columnX = {0.0f, 220.0f};
    in.columnWidth = {200.0f, 200.0f};
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
    // 副本底色 = 优先级浅染（H16：与真卡同款，本测试任务 High → error 0.12 浅染）
    {
        const auto* ghostBg = ui.find("todo.dragghost.bg");
        DOCTEST_REQUIRE(ghostBg != nullptr);
        const auto& t = core::theme::current();
        // H15：基底 surface 已叠加 kCardGlassAlpha=0.92，H16 浅染在其上混色
        DOCTEST_CHECK(colorNear(ghostBg->color,
                                eui::mixColor(ui_kit::cardSurface(t), t.colors.error, 0.12f)));
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
              "todo_ui: 列内滚动——H20 虚拟化窗口 + 列几何正确（headless layout）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    // 固定四列：不经 service 造自定义第 5 列；15 张卡落种子「待办」列（col.0）
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE_EQ(cols.value().size(), 4u);
    const int64_t colId = cols.value()[0].id;
    for (int i = 1; i <= 15; ++i) {
        auto r = env.svc->createTask(colId, "任务" + std::to_string(i));
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

    // H20 虚拟化：滚动偏移 0 只构建可视区 + 上下各 3 张缓冲。
    // 列内容可视高 856、卡 pitch 92 → first=0、last=13（索引 0..12）；13/14 不建。
    for (int i = 0; i < 13; ++i) {
        const auto* card = ui.find("todo.col.0.card." + std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(card != nullptr);
    }
    DOCTEST_CHECK(ui.find("todo.col.0.card.13.bg") == nullptr);
    DOCTEST_CHECK(ui.find("todo.col.0.card.14.bg") == nullptr);
    // 滚动容器存在；内容总高仍按 15 张槽位撑出（占位元素），不随本帧构建张数缩水
    DOCTEST_CHECK(ui.find("todo.col.0.scroll") != nullptr);
    const auto* spacer = ui.find("todo.col.0.vspacer");
    DOCTEST_REQUIRE(spacer != nullptr);
    DOCTEST_CHECK_EQ(spacer->frame.height,
                     module::todo::ui::columnContentHeight(15));

    // 列头与列背景同 x 起点，卡片在列内纵向排列。
    // 旧缺陷：position() 在 row 流式布局里被忽略，列头/卡片横向排开（col 头 x 偏移一整列宽）
    const auto* colBg = ui.find("todo.col.0.bg");
    const auto* colHead = ui.find("todo.col.0.head");
    const auto* colTitle = ui.find("todo.col.0.title");
    DOCTEST_REQUIRE(colBg != nullptr);
    DOCTEST_REQUIRE(colHead != nullptr);
    DOCTEST_REQUIRE(colTitle != nullptr);
    DOCTEST_CHECK_EQ(colHead->frame.x, colBg->frame.x);
    DOCTEST_CHECK_EQ(colHead->frame.y, colBg->frame.y);
    // 标题 x 按色点存在与否（todo_ui_board.cpp:407 titleX = colDot ? 34 : 16）：
    // 默认「待办」列有 8px 色点（dot@+16）→ 标题让至 +34；无色点列 → 标题贴 +16。
    // 不写死 16，按 todo.col.0.dot 是否存在分支断言。
    const auto* colDot = ui.find("todo.col.0.dot");
    if (colDot != nullptr) {
        DOCTEST_CHECK_EQ(colTitle->frame.x, colBg->frame.x + 34.0f);
    } else {
        DOCTEST_CHECK_EQ(colTitle->frame.x, colBg->frame.x + 16.0f);
    }
    const auto* card0 = ui.find("todo.col.0.card.0.bg");
    const auto* card1 = ui.find("todo.col.0.card.1.bg");
    DOCTEST_REQUIRE(card0 != nullptr);
    DOCTEST_REQUIRE(card1 != nullptr);
    // 卡片在列内（不是横向流式排开）。卡左缘 = 列左缘 + 列内容左内边距
    // kColumnContentInset（16px），两侧 inset 同源派生（见 contentCardWidth）：
    // 钉成 0 会退回「卡贴列左缘、左零边距」的缺陷形态。
    DOCTEST_CHECK_EQ(card0->frame.x,
                     colBg->frame.x + module::todo::ui::kColumnContentInset);
    DOCTEST_CHECK_EQ(card1->frame.x,
                     colBg->frame.x + module::todo::ui::kColumnContentInset);
    DOCTEST_CHECK_EQ(card1->frame.y - card0->frame.y, 84.0f + 8.0f);  // 卡高 + 间距

    // 滚到底（offset = 内容高 1372 − 可视高 856 = 516）→ 窗口换批：
    // first=floor(516/92)−3=2、last=15；0/1 不再构建、14 进入构建集。
    const auto* scroll = ui.find("todo.col.0.scroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_REQUIRE(scroll->onScrollOffsetChanged != nullptr);
    scroll->onScrollOffsetChanged(516.0f);
    layoutFrame();
    DOCTEST_CHECK(ui.find("todo.col.0.card.0.bg") == nullptr);
    DOCTEST_CHECK(ui.find("todo.col.0.card.1.bg") == nullptr);
    const auto* card13 = ui.find("todo.col.0.card.13.bg");
    const auto* card14 = ui.find("todo.col.0.card.14.bg");
    DOCTEST_REQUIRE(card13 != nullptr);
    DOCTEST_REQUIRE(card14 != nullptr);
    DOCTEST_CHECK_EQ(card14->frame.y - card13->frame.y, 84.0f + 8.0f);  // 槽位仍等距

    // 多列不重叠：col.0/col.1 背景横向相邻（x 差 = 前列实际宽 + 间距；变宽列逐列核对）
    const auto* col0Bg = ui.find("todo.col.0.bg");
    const auto* col1Bg = ui.find("todo.col.1.bg");
    DOCTEST_REQUIRE(col0Bg != nullptr);
    DOCTEST_REQUIRE(col1Bg != nullptr);
    DOCTEST_CHECK(col1Bg->frame.x > col0Bg->frame.x);
    const float gap = core::theme::current().sizes.spacingLarge;
    DOCTEST_CHECK(std::fabs(col1Bg->frame.x -
                            (col0Bg->frame.x + col0Bg->frame.width + gap)) < 0.5f);

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

// UAT6 成功类 toast 删除防回退（用户真机反馈：每步操作弹「已保存/已上移/任务已完成」
// 造噪音）：成功操作（详情保存 / 标记完成）静默不弹 toast；错误/异常通道保留——
// toast 组件、状态成员、drawToast 全留着，无选中 Ctrl+↑ 的引导 toast 必须照常弹出。
// 回退路径：任一 doXxx 成功分支加回 setStatus（成功态）→ 隐藏断言红；错误通道被
// 顺手删掉（「全删」读歪）→ 错误 toast 断言红。
REGISTER_TEST(todo_ui_success_ops_silent_no_toast,
              "todo_ui: 成功操作静默（保存/标记完成不弹 toast），错误通道保留（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("静默列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "静默卡");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // ① 打开详情 → 点「保存」（doUpdateTask 成功路径，此前弹「已保存」）→ toast 保持隐藏
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame f(mod.get());
        const auto* save = f.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_REQUIRE(save->onClick != nullptr);
        save->onClick();
        Frame g(mod.get());
        const auto* toast = g.ui.find("todo.toast");
        DOCTEST_REQUIRE(toast != nullptr);
        DOCTEST_CHECK_EQ(toast->opacity, 0.0f);   // 成功不弹
    }
    // 保存是真成功的（不是没点到）：任务仍在、标题未变
    {
        auto one = env.svc->listTasks(col.value().id);
        DOCTEST_REQUIRE(one.isOk());
        DOCTEST_REQUIRE_EQ(one.value().size(), 1u);
        DOCTEST_CHECK_EQ(one.value()[0].title, std::string("静默卡"));
    }

    // ② 标记完成（doCompleteTask + finishCompletionMove 成功路径，此前弹「任务已完成」）
    //→ 仍不弹
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame f(mod.get());
        const auto* toggle = f.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        toggle->onClick();
        Frame g(mod.get());
        const auto* toast = g.ui.find("todo.toast");
        DOCTEST_REQUIRE(toast != nullptr);
        DOCTEST_CHECK_EQ(toast->opacity, 0.0f);   // 完成也不弹
    }
    // 完成 + 移列都真生效了
    {
        auto doneCol = env.svc->listColumns();
        DOCTEST_REQUIRE(doneCol.isOk());
        int64_t doneId = 0;
        for (const auto& c : doneCol.value()) {
            if (c.kind == "done") doneId = c.id;
        }
        DOCTEST_REQUIRE(doneId != 0);
        auto moved = env.svc->listTasks(doneId);
        DOCTEST_REQUIRE(moved.isOk());
        DOCTEST_REQUIRE_EQ(moved.value().size(), 1u);
        DOCTEST_CHECK(moved.value()[0].completedAt.has_value());
    }

    // ③ 错误通道保留：无选中 Ctrl+↑ → 引导 toast 照常弹出
    // UAT6：视图状态跨模块实例持久化（selectedTaskId 存 app_config），
    // 需要先清掉前一个模块的选中记录，否则 mod2 的 restoreViewState 会恢复选中
    {
        if (auto* db = ::core::appDatabase(); db != nullptr) {
            ::core::appconfig::erase(*db, "todo_view_state");
        }
        std::unique_ptr<::core::Module> mod2(create_todo_module());
        mod2->onStart();
        DOCTEST_CHECK(!mod2->onKeyEvent(ctrlArrow(eui::InputKey::Up)));
        Frame f(mod2.get());
        const auto* toast = f.ui.find("todo.toast");
        DOCTEST_REQUIRE(toast != nullptr);
        DOCTEST_CHECK_EQ(toast->opacity, 1.0f);
        const auto* title = f.ui.find("todo.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("先点击卡片选中任务") != std::string::npos);
        mod2->onStop();
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
    DOCTEST_REQUIRE(cardTitleSize(&base));       // 14（Body 直读）
    ::core::theme::setFontScale(1.3f);
    float scaled = 0.0f;
    DOCTEST_REQUIRE(cardTitleSize(&scaled));
    ::core::theme::setFontScale(1.0f);           // 恢复，不影响其他用例
    DOCTEST_CHECK_EQ(base, 14.0f);
    // 派生式 = token(缩放后)：14×1.3 = 18.2
    DOCTEST_CHECK_EQ(scaled, 14.0f * 1.3f);
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

    // 全树兜底：深色主题下不允许任何不透明近白 Rect（微信列背景 background 例外：§4.14 有意浅灰底）
    std::vector<const core::dsl::Element*> whites;
    for (const auto& root : frame.ui.roots()) {
        collectOpaqueWhiteRects(root.get(), &whites);
    }
    // D-04 dark-WeChat deferred，深色下 wechat 令牌（background/surface/hover 等）暂为浅值，
    // 待 D-04 适配后移除例外。例外覆盖 todo.*.bg：含任务卡族（D批米黄迁移后卡交互层底/卡内 chip 底
    // = surface/hover 浅值；旧注释"card 底走 priorityCardBg 深色下为深色"随 surface 基底迁移失效）
    auto isWeChatDeferred = [](const std::string& id) {
        // 搜索框（todo_frame.todo.search.*）：board componentTheme 迁 WeChat 后表面=surface
        // 浅值，深色下同 D-04 延后，待 dark-WeChat 适配后随本例外一并移除。
        if (id.find("todo_frame.todo.search") != std::string::npos) return true;
        // 任务卡骨架（*.card.bg）：卡片 .theme 迁 WeChat 后骨架底=surface（浅色下白为设计值），
        // 深色下同 D-04 延后；列头热区全透明契约（.head）不受影响。
        // 弹窗区域（todo_frame.todo.dialog.*）：弹窗 themeTokens 已整体迁 WeChat，
        // 内含组件（input/textarea 等）表面=surface 浅值，深色下同 D-04 延后整区域放行。
        if (id.find("todo_frame.todo.dialog") != std::string::npos) return true;
        if (id.find("todo.") != std::string::npos && id.ends_with(".card.bg"))
            return true;
        // 任务卡族 .bg（.card.N.bg 交互层与卡内 chip .bg）：D批米黄迁移后底走 surface/
        // hover 浅值，深色下同 D-04 延后（旧通用规则排除 .card. 因当时卡底尚为深色）
        if (id.find("todo.") != std::string::npos &&
            id.find(".card.") != std::string::npos && id.ends_with(".bg"))
            return true;
        return id.find("todo.") != std::string::npos &&
               id.find(".bg") != std::string::npos &&
               id.find(".card.") == std::string::npos;
    };
    std::vector<const core::dsl::Element*> unexpected;
    for (const auto* el : whites) {
        if (isWeChatDeferred(el->id)) continue;
        unexpected.push_back(el);
    }
    for (const auto* el : unexpected) {
        // 失败时直接输出元素 id，一眼定位
        DOCTEST_CHECK_MESSAGE(false, ("意外的不透明白 Rect: " + el->id).c_str());
    }
    DOCTEST_CHECK(unexpected.empty());
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
        const auto* btn = frame.ui.find("todo.btn.overdue.hit");
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
    // 零逾期 chip：bg 仍存在（灰色 chip 展示），但 .hit 不可点（只在 hasOverdue 时创建）
    const auto* ovBg = frame.ui.find("todo.btn.overdue.bg");
    DOCTEST_REQUIRE(ovBg != nullptr);
    const auto* ovHit = frame.ui.find("todo.btn.overdue.hit");
    DOCTEST_CHECK(ovHit == nullptr);
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
        const auto* btn = frame.ui.find("todo.btn.overdue.hit");
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
        const auto* btn = frame.ui.find("todo.btn.overdue.hit");
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

        // ②（H20 删除）原「列右移成功 → reloadAll → 逾期查询失败保留计数」链路依赖
        //    列管理菜单；固定四列后该入口不存在，而删表注入会同时打挂 listAllTasks
        //    （board 进错误态），无法只让 listOverdue 单独失败——本子场景随之移除。
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

// 列色点：种子列渲染色点；service 设置 color 的自定义列渲染对应色；无色列不渲
REGISTER_TEST(todo_ui_column_color_dot_and_menu,
              "todo_ui: 列色点渲染（种子色/自定义色/无色，headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto plain = env.svc->createColumn("无色列");
    DOCTEST_REQUIRE(plain.isOk());
    // H20：列右键「改色」入口随固定四列移除；service 层 setColumnColor 仍合法——
    // 在模块 onStart 前落库，验证色点随 color 渲染（只锁渲染，不锁已删入口）
    auto colored = env.svc->createColumn("有色列");
    DOCTEST_REQUIRE(colored.isOk());
    DOCTEST_REQUIRE(env.svc->setColumnColor(
        colored.value().id, std::optional<std::string>("dusty-blue")).isOk());

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
        // 用户新建列无色（color NULL）→ 不渲色点；自定义有色列 → 渲对应色
        DOCTEST_CHECK(frame.ui.find("todo.col.4.dot") == nullptr);
        const auto* blue = frame.ui.find("todo.col.5.dot");
        DOCTEST_REQUIRE(blue != nullptr);
        const eui::Color dusty("#6A9BCC");
        DOCTEST_CHECK_EQ(blue->color.r, dusty.r);
        DOCTEST_CHECK_EQ(blue->color.g, dusty.g);
        DOCTEST_CHECK_EQ(blue->color.b, dusty.b);
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

// H16：卡面优先级浅染（surface 底 + 优先级色 ×0.12），3px 色条保留作硬标识。
// 回退任一条即红：把 .prio 色块画回来 → 元素存在断言红；把卡面浅染换回
// 纯 surface 底 → 「None 纯白 vs 低/中/高浅染」取值断言红；把浅染色源换色
// （success/warning/error 板）→ 色值断言红；调低/调高对比度 → ≥4.5:1 断言红。
REGISTER_TEST(todo_ui_priority_card_bar,
              "todo_ui: 优先级浅染卡面+左3px色条+Sm阴影（H16 覆盖 S6#5 纯白）", {
    using module::todo::Priority;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("色条列");
    DOCTEST_REQUIRE(col.isOk());
    const Priority kOrder[4] = {Priority::None, Priority::Low, Priority::Medium,
                                Priority::High};
    for (int i = 0; i < 4; ++i) {
        auto a = env.svc->createTask(col.value().id, "色条卡" + std::to_string(i));
        DOCTEST_REQUIRE(a.isOk());
        module::todo::Task t2 = a.value();
        t2.priority = kOrder[i];
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());
    frame.ui.layout(1600.0f, 1000.0f);
    const auto& t = core::theme::current();

    // 判据 a：卡面优先级浅染（H16 覆盖 S6#5 纯白）——None 纯白、Low/Med/High 浅染；
    // 色源：Low=success #10B981 / Medium=warning #F59E0B / High=error #EF4444，
    // 强度 kTodoTintAlpha=0.12。档间互异，卡底不再是纯白 surface。
    // H15：基底 surface 已叠加 kCardGlassAlpha=0.92，H16 浅染在其上混色。
    const eui::Color kExpectFaceColor[4] = {
        ui_kit::cardSurface(t),  // None 无 tint 保持白卡半透
        eui::mixColor(ui_kit::cardSurface(t), t.colors.success, 0.12f),  // Low 祖母绿浅染
        eui::mixColor(ui_kit::cardSurface(t), t.colors.warning, 0.12f),  // Medium 琥珀浅染
        eui::mixColor(ui_kit::cardSurface(t), t.colors.error, 0.12f),    // High 警示红浅染
    };
    for (int i = 0; i < 4; ++i) {
        const std::string cid = "todo.col.4.card." + std::to_string(i);
        const auto* bg = frame.ui.find(cid + ".bg");
        DOCTEST_REQUIRE(bg != nullptr);
        // 卡面按优先级浅染
        DOCTEST_CHECK(colorNear(bg->color, kExpectFaceColor[i]));
        // Sm 阴影已附着（shadow 字段非空 / blur > 0）
        DOCTEST_CHECK(bg->shadow.enabled);
        // 圆角 8px
        DOCTEST_CHECK_EQ(bg->radius, 8.0f);
        // 注：hoverColor/pressedColor 由 button 组件 states 接管，此处不逐项断言
    }

    // 判据 b：旧优先级色块/竖线已不存在（整卡涂色退役 + selbar 退役）
    for (int i = 0; i < 4; ++i) {
        const std::string cid = "todo.col.4.card." + std::to_string(i);
        DOCTEST_CHECK(frame.ui.find(cid + ".prio.bg") == nullptr);
        DOCTEST_CHECK(frame.ui.find(cid + ".prio.text") == nullptr);
        DOCTEST_CHECK(frame.ui.find(cid + ".bar") == nullptr);
        DOCTEST_CHECK(frame.ui.find(cid + ".selbar") == nullptr);
        DOCTEST_CHECK(frame.ui.find(cid + ".overduebar") == nullptr);
    }

    // 判据 c：左 3px 优先级色条存在且颜色正确
    // priorityColor 色表（W3-B3）：None=textTertiary Low=primary Medium=warning High=error
    const eui::Color kExpectBarColor[4] = {
        t.colors.textTertiary,  // None 中性灰
        t.colors.primary,       // Low 绿 #36D399
        t.colors.warning,       // Medium 橙 #F59E0B（W3-B3 从 info 蓝改为 warning 橙）
        t.colors.error,         // High 红 #EF4444
    };
    for (int i = 0; i < 4; ++i) {
        const std::string cid = "todo.col.4.card." + std::to_string(i);
        const auto* bar = frame.ui.find(cid + ".priobar");
        DOCTEST_REQUIRE(bar != nullptr);
        // 色条 3px 宽、卡高
        DOCTEST_CHECK_EQ(bar->frame.width, 3.0f);
        DOCTEST_CHECK_EQ(bar->frame.height, module::todo::ui::kCardHeight);
        // 颜色与优先级色表一致
        DOCTEST_CHECK(colorNear(bar->color, kExpectBarColor[i]));
    }
    // 判据 c'：四档色条颜色两两不同
    for (int i = 0; i < 4; ++i) {
        const std::string ci = "todo.col.4.card." + std::to_string(i);
        const auto* bi = frame.ui.find(ci + ".priobar");
        for (int j = i + 1; j < 4; ++j) {
            const std::string cj = "todo.col.4.card." + std::to_string(j);
            const auto* bj = frame.ui.find(cj + ".priobar");
            DOCTEST_CHECK(!colorNear(bi->color, bj->color, 0.01f));
        }
    }

    // 判据 d：选中态——点击卡后色条仍存在、卡面仍浅染（H16：无 selbar、无旧底色 tint 回归）
    {
        Frame f2(mod.get());
        const auto* card = f2.ui.find("todo.col.4.card.3.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();  // 选中高优先级卡
        Frame f3(mod.get());
        const auto* selBg = f3.ui.find("todo.col.4.card.3.bg");
        DOCTEST_REQUIRE(selBg != nullptr);
        // 选中态边框 = 统一 1px 分割线
        DOCTEST_CHECK_EQ(selBg->border.width, 1.0f);
        DOCTEST_CHECK(colorNear(selBg->border.color, t.colors.borderDefault));
        // 选中态卡面仍为 High→error 浅染（不再走 primarySoftBg + priority tint）；H15 cardSurface 基底
        DOCTEST_CHECK(colorNear(selBg->color,
                                eui::mixColor(ui_kit::cardSurface(t), t.colors.error, 0.12f)));
        // 优先级色条仍在
        DOCTEST_CHECK(f3.ui.find("todo.col.4.card.3.priobar") != nullptr);
        // 旧选中左竖线已退役
        DOCTEST_CHECK(f3.ui.find("todo.col.4.card.3.selbar") == nullptr);
    }

    // 判据 e：浅色主题 WCAG — 各档浅染底上文字对比度达标
    ::core::theme::setLight();
    const auto& tl = core::theme::current();
    Frame light(mod.get());
    light.ui.layout(1600.0f, 1000.0f);
    // H15：基底 surface 已叠加 kCardGlassAlpha=0.92
    const eui::Color kExpectFaceLight[4] = {
        ui_kit::cardSurface(tl),  // None 白卡半透
        eui::mixColor(ui_kit::cardSurface(tl), tl.colors.success, 0.12f),  // Low
        eui::mixColor(ui_kit::cardSurface(tl), tl.colors.warning, 0.12f),  // Medium
        eui::mixColor(ui_kit::cardSurface(tl), tl.colors.error, 0.12f),    // High
    };
    for (int i = 0; i < 4; ++i) {
        const auto* bg = light.ui.find("todo.col.4.card." + std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(bg != nullptr);
        // 四档浅染底（None 纯白），textPrimary 对比度 ≥ 4.5:1
        DOCTEST_CHECK(colorNear(bg->color, kExpectFaceLight[i]));
        DOCTEST_CHECK(contrastRatio(bg->color, tl.colors.textPrimary) >= 4.5f);
    }
    ::core::theme::setDark();  // 恢复默认

    mod->onStop();
    return true;
})

// 详情弹窗优先级 segment（UAT5 重设计）：四档同一语言——未选 = background 底 +
// borderDefault 边 + textPrimary 字；选中 = 主绿底 + 白字 + 主绿边（档位信息在
// 文字，不再靠各档 tint 色底）。防的事故：
//   ① 选中档回退成 UAT2 的各档 tint 色底 → 色值断言红；
//   ② 未选中底上文字对比度回退（<4.5:1）→ 断言红；
//   ③ 文案回退成带 ● 前缀的看板卡 priorityLabel → 文案断言红；
//   ④ 点选不切选中态 / 不互斥 → 循环点选断言红。
REGISTER_TEST(todo_ui_priority_button_tint,
              "todo_ui: 详情优先级 segment——选中绿底白字/未选 background 底、文案无点前缀、点选互斥", {
    using module::todo::Priority;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("按钮列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "按钮卡");
    DOCTEST_REQUIRE(a.isOk());
    {
        // 起始优先级给「高」：详情弹窗打开时 prio.3 是选中态，其余三档是未选中
        module::todo::Task t2 = a.value();
        t2.priority = module::todo::Priority::High;
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    // 切浅色跑（wechat 令牌两主题同值，文字对比度断言 vs textPrimary 在浅色下口径最严）
    ::core::theme::setLight();
    const auto& t = core::theme::current();
    // 期望值独立写死（改选中档色源 / 未选底 / 文字档即红）
    const eui::Color kSelBg = t.colors.primary;        // #07C160
    const eui::Color kSelText = t.colors.onPrimary;    // #FFFFFF
    const eui::Color kUnselBg = t.colors.background;       // #F7F7F7
    const eui::Color kUnselText = t.colors.textPrimary;  // #1A1A1A
    const char* kLabels[4] = {"无", "低", "中", "高"};

    Frame f1(mod.get());
    const auto* card = f1.ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(card->onClick != nullptr);
    card->onClick();  // 打开详情（弹窗状态留在模块侧，后续帧持续渲染）

    // 起始态（任务优先级 = 高）：0/1/2 档未选、3 档选中；文案无 ● 前缀
    {
        Frame f(mod.get());
        for (int j = 0; j < 3; ++j) {
            const auto* bg = f.ui.find("todo.dialog.detail.prio." +
                                       std::to_string(j) + ".bg");
            const auto* text = f.ui.find("todo.dialog.detail.prio." +
                                         std::to_string(j) + ".text.normal");
            DOCTEST_REQUIRE(bg != nullptr);
            DOCTEST_CHECK(text != nullptr);
            DOCTEST_CHECK(colorNear(bg->color, kUnselBg));
            DOCTEST_CHECK_EQ(bg->border.width, 1.0f);
            if (text) {
                DOCTEST_CHECK(colorNear(text->textColor, kUnselText));
                DOCTEST_CHECK(contrastRatio(bg->color, text->textColor) >= 4.5f);
                DOCTEST_CHECK_EQ(text->text, std::string(kLabels[j]));
            }
        }
        const auto* sel = f.ui.find("todo.dialog.detail.prio.3.bg");
        const auto* selText = f.ui.find("todo.dialog.detail.prio.3.text.normal");
        DOCTEST_REQUIRE(sel != nullptr);
        DOCTEST_CHECK(selText != nullptr);
        DOCTEST_CHECK(colorNear(sel->color, kSelBg));
        if (selText) {
            DOCTEST_CHECK(colorNear(selText->textColor, kSelText));
            DOCTEST_CHECK(colorNear(sel->border.color, t.colors.primary));
            DOCTEST_CHECK_EQ(selText->text, std::string("高"));
        }
    }

    // 逐档点选：被点档变绿、其余三档回 background（互斥）
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
        DOCTEST_CHECK(colorNear(sel->color, kSelBg));
        for (int j = 0; j < 4; ++j) {
            if (j == i) continue;
            const auto* other = g.ui.find("todo.dialog.detail.prio." +
                                         std::to_string(j) + ".bg");
            DOCTEST_REQUIRE(other != nullptr);
            DOCTEST_CHECK(colorNear(other->color, kUnselBg));
        }
    }

    // 截止 segment（本卡无 dueAt）：单 op「选择日期…」background 底 + textPrimary 字
    //（≥4.5:1）；无 due 不渲染「清除」op（有 due 才两枚——几何用例钉双枚形态）
    {
        Frame f(mod.get());
        const auto* pickBg = f.ui.find("todo.dialog.detail.due.pick.bg");
        const auto* pickText = f.ui.find("todo.dialog.detail.due.pick.text.normal");
        DOCTEST_REQUIRE(pickBg != nullptr);
        DOCTEST_REQUIRE(pickText != nullptr);
        DOCTEST_CHECK(contrastRatio(pickBg->color, pickText->textColor) >= 4.5f);
        DOCTEST_CHECK(f.ui.find("todo.dialog.detail.due.clear.bg") == nullptr);
    }

    mod->onStop();
    ::core::theme::setDark();  // 恢复默认深色（本用例切了浅色，防污染后续用例）
    return true;
})


// UAT6 详情弹窗背景对齐优先级色防回退（用户拍板）：弹窗面板 bg = 该任务优先级
// tint——复用看板卡片的 priorityColor 成员指针表 + priorityTintBg 混色公式，强度
// 走大面板档 = 卡强度 ×0.5（Low 0.20 / Medium 0.25 / High 0.275）；None 档无 tint
// 原底早退（同卡片 None 语义）。AA 护栏：正文 textPrimary #1A1A1A 在各档 tint 底上
// 实测 Low 14.5 / Medium 12.4 / High 12.5 ≥4.5:1（数字见 todo_ui_logic.h 注释）。
// 回退路径：色源换表/强度改档 → 色值断言红；强度抬过 AA 线（>0.84）→ 对比度断言红；
// None 档误加灰 tint → 原底断言红。
// 观感复核钉值：优先级四钮与头部 chips 都是不透明实底，自身配色不被面板 tint 波及
//——选中钮仍绿底白字、未选钮 background 底 textPrimary 字、「标记完成」chip 仍浅绿底。
REGISTER_TEST(todo_ui_detail_panel_priority_tint,
              "todo_ui: 详情弹窗面板 bg = 优先级 tint（卡强度×0.5 档）+ textPrimary ≥AA", {
    using module::todo::Priority;
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("tint列");
    DOCTEST_REQUIRE(col.isOk());
    const Priority prios[4] = {Priority::None, Priority::Low,
                               Priority::Medium, Priority::High};
    for (int i = 0; i < 4; ++i) {
        auto a = env.svc->createTask(col.value().id, "tint卡" + std::to_string(i));
        DOCTEST_REQUIRE(a.isOk());
        module::todo::Task t2 = a.value();
        t2.priority = prios[i];
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    // wechat 令牌两主题同值；浅色口径最严（与既有 tint 用例一致）
    ::core::theme::setLight();
    const auto& t = core::theme::current();
    const auto themeTokens = core::theme::componentTokensWeChat(t);

    for (int i = 0; i < 4; ++i) {
        // 弹窗开着直接切卡（openDetailDialog reset 换任务，既有用例同款）
        {
            Frame f1(mod.get());
            const auto* card =
                f1.ui.find("todo.col.4.card." + std::to_string(i) + ".bg");
            DOCTEST_REQUIRE(card != nullptr);
            DOCTEST_REQUIRE(card->onClick != nullptr);
            card->onClick();
        }
        Frame f(mod.get());
        f.ui.layout(1600.0f, 1000.0f);
        // 面板 bg = priorityTintBg（成员指针表色源 + ×0.5 强度档）
        const auto* panelBg = f.ui.find("todo.dialog.detail.panel.bg");
        DOCTEST_REQUIRE(panelBg != nullptr);
        const eui::Color expect = module::todo::ui::priorityTintBg(
            prios[i], t, themeTokens.surface,
            module::todo::ui::priorityPanelTintStrength(prios[i]));
        DOCTEST_CHECK(colorNear(panelBg->color, expect));
        if (i == 0) {
            // None 档原底早退：面板底 = surface（surface），不吃中性灰 tint
            DOCTEST_CHECK(colorNear(panelBg->color, themeTokens.surface));
        } else {
            // 档间可辨：相邻档 tint 底色必须看得出来不同
            const eui::Color expectPrev = module::todo::ui::priorityTintBg(
                prios[i - 1], t, themeTokens.surface,
                module::todo::ui::priorityPanelTintStrength(prios[i - 1]));
            DOCTEST_CHECK(maxChannelDelta(panelBg->color, expectPrev) > 0.01f);
        }
        // AA 护栏：正文 textPrimary 在 tint 底上 ≥4.5:1（实测 14.5/12.4/12.5）
        DOCTEST_CHECK(contrastRatio(panelBg->color,
                                     t.colors.textPrimary) >= 4.5f);
        // 观感钉值①：选中优先级钮 = 主绿底白字（不透明实底，不被 tint 波及）
        const auto* selBg =
            f.ui.find("todo.dialog.detail.prio." + std::to_string(i) + ".bg");
        const auto* selText =
            f.ui.find("todo.dialog.detail.prio." + std::to_string(i) + ".text.normal");
        DOCTEST_REQUIRE(selBg != nullptr);
        DOCTEST_CHECK(selText != nullptr);
        DOCTEST_CHECK(colorNear(selBg->color, t.colors.primary));
        if (selText) {
            DOCTEST_CHECK(colorNear(selText->textColor, t.colors.onPrimary));
        }
        // 观感钉值②：任一未选钮 = background 底 + textPrimary 字（对比度不受 tint 影响）
        const int j = (i + 1) % 4;
        const auto* unBg =
            f.ui.find("todo.dialog.detail.prio." + std::to_string(j) + ".bg");
        const auto* unText =
            f.ui.find("todo.dialog.detail.prio." + std::to_string(j) + ".text.normal");
        DOCTEST_REQUIRE(unBg != nullptr);
        DOCTEST_CHECK(unText != nullptr);
        DOCTEST_CHECK(colorNear(unBg->color, t.colors.background));
        if (unText) {
            DOCTEST_CHECK(colorNear(unText->textColor, t.colors.textPrimary));
            DOCTEST_CHECK(contrastRatio(unBg->color, unText->textColor) >= 4.5f);
        }
        // 观感钉值③：头部「标记完成」chip（未完成态）= surface 向 primary 混 0.12，
        // 未吃面板 tint
        const auto* toggleBg = f.ui.find("todo.dialog.detail.toggle.bg");
        DOCTEST_REQUIRE(toggleBg != nullptr);
        DOCTEST_CHECK(colorNear(
            toggleBg->color,
            eui::mixColor(t.colors.surface, t.colors.primary, 0.12f)));
    }

    ::core::theme::setDark();   // 恢复默认深色（本用例切了浅色，防污染后续用例）
    mod->onStop();
    return true;
})

// UAT5 详情弹窗重设计几何钉值（弹性宽：1600 窗 → 800 = max(640, 窗口/2)）。
// UAT6 增补：frame 高改内容驱动（上限仍 min(600, 窗−48)）；UAT6.2 H31 关联文档块
// 入内容区后短内容 = 头 58 + 内容 340（54+104+64+20+34+64，末位 64 = 文档块
// kBlockDocH）+ 缝 12 + 钮 34 + 底距 18 = 462——本用例即「短内容」钉值口径。
// 防的事故：① 弹性宽回退固定 640 → 面板宽断言红；② 双列回退单列长条 → 右列 x
// 断言红；③ segment 选项不按列宽平分 → 钮宽/右缘断言红；④ 底部按钮带（取消|保存）
// 被删除/移动按钮挤掉 → 保存钮位置断言红；⑤ 高度回退恒 600（不随内容）→ 462 断言红。
// 几何断言走 layout 后的 frame 绝对坐标，再减面板原点换回弹窗内坐标（与 agent
// 弹窗几何用例同口径）。
REGISTER_TEST(todo_ui_detail_two_column_geometry,
              "todo_ui: 详情弹窗重设计几何——弹性宽 800、内容驱动高 462 / 双列 x=408 / segment 平分列宽 / 底部 取消|保存", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("双列列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "双列卡");
    DOCTEST_REQUIRE(a.isOk());
    {
        // 带 due 的卡：截止 segment 走双 op 形态（📅 日期 chip 选中 + 清除）
        module::todo::Task t2 = a.value();
        t2.dueAt = module::todo::ui::ymdToEpoch(2027, 3, 15);
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();  // 打开详情（编辑态：非归档列 offY=0，几何即字面量）
    }

    Frame f(mod.get());
    f.ui.layout(1600.0f, 1000.0f);
    const auto* panel = f.ui.find("todo.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    const float px = panel->frame.x;
    const float py = panel->frame.y;

    // ① 弹窗尺寸：弹性宽 = max(640, 窗口/2) = 800；高 = 内容驱动（UAT6 真机反馈
    //「底部空白太多」恒 600）：短内容（无标签，各块定高 54+104+64+20+34+64=340，
    // 末位 64 = H31 关联文档块 kBlockDocH）
    // frame 高 = 头 58 + 内容 340 + 缝 12 + 钮 34 + 底距 18 = 462（上限 600 不触）
    DOCTEST_CHECK(floatEq(panel->frame.width, 800.0f));
    DOCTEST_CHECK(floatEq(panel->frame.height, 462.0f));
    {
        // 窄窗不缩：1100 窗 → 仍为最小档 640（demo px 是下限）
        Frame g(mod.get(), 1100.0f, 900.0f);
        g.ui.layout(1100.0f, 900.0f);
        const auto* panel2 = g.ui.find("todo.dialog.detail.panel");
        DOCTEST_REQUIRE(panel2 != nullptr);
        DOCTEST_CHECK(floatEq(panel2->frame.width, 640.0f));
    }

    // 几何基准（800 宽）：sw=752、colW=(752-16)/2=368、右列 x = 24+368+16 = 408
    const float kColW = 368.0f;
    const float kCol2X = 408.0f;

    // ② 头部右缘动作 chips（删除 / 标记完成 上移；标题行 y=22）
    const auto* del = f.ui.find("todo.dialog.detail.delete.bg");
    const auto* toggle = f.ui.find("todo.dialog.detail.toggle.bg");
    DOCTEST_REQUIRE(del != nullptr);
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_CHECK(floatEq(del->frame.y - py, 18.0f));  // chipY 22→18 (contentTop 76→58)
    DOCTEST_CHECK(floatEq(del->frame.height, 28.0f));
    DOCTEST_CHECK(floatEq(toggle->frame.x - px, 800.0f - 48.0f - 104.0f));  // kDialogBodyInsetX 56→48
    DOCTEST_CHECK(floatEq(toggle->frame.x + toggle->frame.width,
                          px + 800.0f - 48.0f));   // toggle 右缘 = 内容右缘（kDialogBodyInsetX）
    DOCTEST_CHECK(del->frame.x + del->frame.width <
                  toggle->frame.x);   // 删除在标记完成左侧

    // ③ 标题大输入（y=58 h44 全宽）+ 描述 textarea（y=132 h72）
    const auto* titleInput = f.ui.find("todo.dialog.detail.input.title");
    DOCTEST_REQUIRE(titleInput != nullptr);
    DOCTEST_CHECK(floatEq(titleInput->frame.x - px, 24.0f));
    DOCTEST_CHECK(floatEq(titleInput->frame.y - py, 58.0f));
    DOCTEST_CHECK(floatEq(titleInput->frame.height, 44.0f));
    DOCTEST_CHECK(floatEq(titleInput->frame.width, 752.0f));
    const auto* descInput = f.ui.find("todo.dialog.detail.input.desc");
    DOCTEST_REQUIRE(descInput != nullptr);
    DOCTEST_CHECK(floatEq(descInput->frame.y - py, 132.0f));
    DOCTEST_CHECK(floatEq(descInput->frame.height, 72.0f));
    // demo 重设计：标题无行标签（大输入自明）
    DOCTEST_CHECK(f.ui.find("todo.dialog.detail.tlabel") == nullptr);

    // ④ 双列标签同排：优先级 x=28 / 截止日期 x=408，y 相等
    const auto* plabel = f.ui.find("todo.dialog.detail.plabel");
    DOCTEST_REQUIRE(plabel != nullptr);
    const auto* dlabel = f.ui.find("todo.dialog.detail.dlabel2");
    DOCTEST_REQUIRE(dlabel != nullptr);
    DOCTEST_CHECK(floatEq(plabel->frame.x - px, 24.0f));
    DOCTEST_CHECK(floatEq(dlabel->frame.x - px, kCol2X));
    DOCTEST_CHECK(floatEq(dlabel->frame.y, plabel->frame.y));
    DOCTEST_CHECK(floatEq(plabel->frame.y - py, 216.0f));

    // ⑤ 优先级 segment 4 op 平分列宽：opW = (368-3×6)/4 = 87.5、x = 24+i×93.5、y=236
    for (int i = 0; i < 4; ++i) {
        const auto* bg =
            f.ui.find("todo.dialog.detail.prio." + std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(floatEq(bg->frame.x - px,
                              24.0f + static_cast<float>(i) * 93.5f));
        DOCTEST_CHECK(floatEq(bg->frame.width, 87.5f));
        DOCTEST_CHECK(floatEq(bg->frame.height, 32.0f));
        DOCTEST_CHECK(floatEq(bg->frame.y - py, 236.0f));
    }
    const auto* p3 = f.ui.find("todo.dialog.detail.prio.3.bg");
    DOCTEST_REQUIRE(p3 != nullptr);
    DOCTEST_CHECK(p3->frame.x - px + p3->frame.width <= 24.0f + kColW + 0.5f);

    // ⑥ 截止 segment 双 op（有 due）：pick x=408 宽 181 + clear x=595 宽 181
    //   = (368-6)/2；pick 选中（hasDue）显示 📅 03-15
    const auto* pickBg = f.ui.find("todo.dialog.detail.due.pick.bg");
    const auto* pickText = f.ui.find("todo.dialog.detail.due.pick.text.normal");
    const auto* clearBg = f.ui.find("todo.dialog.detail.due.clear.bg");
    DOCTEST_REQUIRE(pickBg != nullptr);
    DOCTEST_REQUIRE(pickText != nullptr);
    DOCTEST_REQUIRE(clearBg != nullptr);
    DOCTEST_CHECK(floatEq(pickBg->frame.x - px, kCol2X));
    DOCTEST_CHECK(floatEq(pickBg->frame.width, 181.0f));
    DOCTEST_CHECK(floatEq(clearBg->frame.x - px, kCol2X + 181.0f + 6.0f));
    DOCTEST_CHECK(floatEq(clearBg->frame.width, 181.0f));
    DOCTEST_CHECK(floatEq(pickBg->frame.y - py, 236.0f));
    DOCTEST_CHECK(pickText->text.find("03-15") != std::string::npos);
    DOCTEST_CHECK(clearBg->frame.x - px + clearBg->frame.width <=
                  kCol2X + kColW + 0.5f);

    // ⑦ 标签区（无标签）：label y=280、输入行 y=300 h34
    const auto* tagInput = f.ui.find("todo.dialog.detail.tag.input");
    DOCTEST_REQUIRE(tagInput != nullptr);
    DOCTEST_CHECK(floatEq(tagInput->frame.y - py, 300.0f));
    DOCTEST_CHECK(floatEq(tagInput->frame.height, 34.0f));

    // ⑧ 底部：取消|保存 右对齐、钉 frame 底（y = frame 高 −18 −34 = 462−52 = 410
    // h34）；保存底 = 面板底 − 18（UAT6 前是 h−58 随请求高定位）
    const auto* saveBg = f.ui.find("todo.dialog.detail.save.bg");
    const auto* cancelBg = f.ui.find("todo.dialog.detail.cancel.bg");
    DOCTEST_REQUIRE(saveBg != nullptr);
    DOCTEST_REQUIRE(cancelBg != nullptr);
    DOCTEST_CHECK(floatEq(saveBg->frame.y - py, 410.0f));
    DOCTEST_CHECK(floatEq(saveBg->frame.height, 34.0f));
    DOCTEST_CHECK(floatEq(saveBg->frame.x - px, 800.0f - 48.0f - 96.0f));
    DOCTEST_CHECK(floatEq(cancelBg->frame.x - px, 800.0f - 48.0f - 96.0f * 2.0f - 8.0f));
    DOCTEST_CHECK(saveBg->frame.y + saveBg->frame.height <=
                  panel->frame.y + panel->frame.height - 17.5f);
    // 底部不再有 删除/标记完成（上移到头部）
    DOCTEST_CHECK(f.ui.find("todo.dialog.detail.move.0") == nullptr);

    mod->onStop();
    return true;
})

// UAT6 防回退①：底部按钮钉 frame 底——多标签（chips 换行）内容注入后，取消/保存
// 的 y 仍 == frame 底 − 18 − 按钮高，与内容高度、窗口高度都解耦；弹高 clamp 公式
// min(600, 窗高 − 48) 在多档窗高下逐点验证（含真机出框报告里的 588 frame 档）。
// UAT6 高度批增补：frame 高改内容驱动后本用例口径不变——任务给 60 枚标签
//（8 行 chips，内容高 340+288=628 → 期望高 58+628+64=750 > 600），前 3 档仍由
// clamp 公式主导、4 档 frameH 期望值全部照旧；矮窗档内容靠滚动兜底。
// 回退路径：按钮改回随请求高定位（h − 58）→ 548/540/536/420 各档 y 断言红；
// 高度公式回退成不 clamp 的固定 600 → 636/520 两档 frameH 断言红；
// 内容驱动公式丢 chips 块 → 内容高不足 600，前 3 档 frameH 断言红。
REGISTER_TEST(todo_ui_detail_footer_pinned_frame_bottom,
              "todo_ui: 详情底部按钮钉 frame 底（frame 底−18−按钮高）+ 弹高 clamp min(600,窗高−48)", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("钉底列");
    DOCTEST_REQUIRE(col.isOk());
    auto a = env.svc->createTask(col.value().id, "多标签任务");
    DOCTEST_REQUIRE(a.isOk());
    {
        // 60 枚中文标签 → chips 在 800 宽弹窗内换 8 行（UAT6 前单行截断只渲染前 8 枚），
        // 内容高 340+288=628 → 期望 frame 高 58+628+64=750 > 600：保证 4 档仍测 clamp 档
        module::todo::Task t2 = a.value();
        for (int i = 0; i < 60; ++i) t2.tags.push_back("标签" + std::to_string(i));
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 打开详情
    }

    // chips 换行不再截断：第 10 枚照常渲染、与第 1 枚不同行（行 y 递增）
    {
        Frame f(mod.get());
        f.ui.layout(1600.0f, 1000.0f);
        const auto* c0 = f.ui.find("todo.dialog.detail.tag.0.bg");
        const auto* c9 = f.ui.find("todo.dialog.detail.tag.9.bg");
        DOCTEST_REQUIRE(c0 != nullptr);
        DOCTEST_REQUIRE_MESSAGE(c9 != nullptr, "chips 未换行截断——第 10 枚应照常渲染");
        DOCTEST_CHECK(c9->frame.y > c0->frame.y);   // 标签9 在第二行（0-7 首行）
        // chips 不横向溢出：首枚右缘在内容右缘内
        const auto* panel = f.ui.find("todo.dialog.detail.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(c0->frame.x + c0->frame.width <=
                      panel->frame.x + panel->frame.width - 28.0f + 0.5f);
    }

    // 按钮钉底：多窗高逐档验证 y == frameH − 52（frameH = min(600, 窗高 − 48)）
    struct SizeCase { float w, h, frameH; };
    const SizeCase cases[] = {
        {1600.0f, 1000.0f, 600.0f},   // 常规窗：不 clamp
        {1000.0f, 640.0f, 592.0f},   // 最小窗档
        {1000.0f, 636.0f, 588.0f},   // 真机出框档（用户量到的 588 frame）
        {1000.0f, 520.0f, 472.0f},   // 矮窗：clamp 生效，内容靠滚动兜底
    };
    for (const auto& c : cases) {
        Frame f(mod.get(), c.w, c.h);
        f.ui.layout(c.w, c.h);
        const auto* panel = f.ui.find("todo.dialog.detail.panel");
        const auto* saveBg = f.ui.find("todo.dialog.detail.save.bg");
        const auto* cancelBg = f.ui.find("todo.dialog.detail.cancel.bg");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_REQUIRE(saveBg != nullptr);
        DOCTEST_REQUIRE(cancelBg != nullptr);
        // 弹高 clamp 公式：panel 高 = min(600, 窗高 − 48)
        DOCTEST_CHECK(floatEq(panel->frame.height, c.frameH));
        // 按钮钉 frame 底固定偏移：y = frameH − 18 − 34；底缘 = frame 底 − 18
        DOCTEST_CHECK(floatEq(saveBg->frame.y - panel->frame.y, c.frameH - 52.0f));
        DOCTEST_CHECK(floatEq(cancelBg->frame.y, saveBg->frame.y));
        DOCTEST_CHECK(floatEq(saveBg->frame.y + saveBg->frame.height,
                              panel->frame.y + panel->frame.height - 18.0f));
        // 不越框：按钮整体在 frame 内
        DOCTEST_CHECK(saveBg->frame.y + saveBg->frame.height <=
                      panel->frame.y + panel->frame.height);
    }

    mod->onStop();
    return true;
})

// UAT6 防回退②：内容超高（60 标签 → chips 多行）时内容区滚动兜底——scrollView 出
// 滚动条（.track/.thumb）、末枚 chip 照常渲染（旧单行截断路径整个消失）、按钮仍在
// 框内钉底；短内容（无标签）不建滚动条。
REGISTER_TEST(todo_ui_detail_content_scroll_when_tall,
              "todo_ui: 详情内容超高滚动兜底——chips 全渲染 + 滚动条出现 + 按钮钉底", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("滚动列");
    DOCTEST_REQUIRE(col.isOk());
    auto plain = env.svc->createTask(col.value().id, "无标签任务");
    DOCTEST_REQUIRE(plain.isOk());
    auto heavy = env.svc->createTask(col.value().id, "重标签任务");
    DOCTEST_REQUIRE(heavy.isOk());
    {
        // 60 枚标签 → 800 宽弹窗内 ~8 行，内容高 > 视口 460 → 触发滚动
        module::todo::Task t2 = heavy.value();
        for (int i = 0; i < 60; ++i) t2.tags.push_back("标签" + std::to_string(i));
        DOCTEST_REQUIRE(env.svc->updateTask(t2).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 短内容（无标签）：不建滚动条（track 不存在）
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();
    }
    {
        Frame f(mod.get());
        f.ui.layout(1600.0f, 1000.0f);
        // 内容驱动高（短内容）：462 = 58+340+12+34+18（内容含 H31 文档块 64），
        // 视口 340 == 内容 340 精确贴合（scrollable 判定是严格 >）→ 不建滚动条
        const auto* shortPanel = f.ui.find("todo.dialog.detail.panel");
        DOCTEST_REQUIRE(shortPanel != nullptr);
        DOCTEST_CHECK(floatEq(shortPanel->frame.height, 462.0f));
        DOCTEST_CHECK(f.ui.find("todo.dialog.detail.scroll.scroll.track") == nullptr);
        DOCTEST_CHECK(f.ui.find("todo.dialog.detail.tag.input") != nullptr);
    }

    // 长内容（60 标签）：滚动条出现、末枚 chip 渲染、按钮仍在框内钉底
    {
        Frame f1(mod.get());
        const auto* card = f1.ui.find("todo.col.4.card.1.bg");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(card->onClick != nullptr);
        card->onClick();   // 弹窗开着直接切换到重标签任务
    }
    {
        Frame f(mod.get());
        f.ui.layout(1600.0f, 1000.0f);
        DOCTEST_CHECK_MESSAGE(
            f.ui.find("todo.dialog.detail.scroll.scroll.track") != nullptr,
            "内容超高时应出现滚动条 track");
        DOCTEST_CHECK(f.ui.find("todo.dialog.detail.scroll.scroll.thumb") != nullptr);
        const auto* last = f.ui.find("todo.dialog.detail.tag.59.bg");
        DOCTEST_REQUIRE_MESSAGE(last != nullptr, "chips 应多行全渲染，不截断");
        const auto* first = f.ui.find("todo.dialog.detail.tag.0.bg");
        DOCTEST_REQUIRE(first != nullptr);
        DOCTEST_CHECK(last->frame.y > first->frame.y);   // 多行换行生效
        const auto* panel = f.ui.find("todo.dialog.detail.panel");
        const auto* saveBg = f.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_REQUIRE(saveBg != nullptr);
        // 按钮钉底 + 在框内
        DOCTEST_CHECK(floatEq(saveBg->frame.y - panel->frame.y, 548.0f));
        DOCTEST_CHECK(saveBg->frame.y + saveBg->frame.height <=
                      panel->frame.y + panel->frame.height);
        // 滚动视口限高：= frame 高 600 − 头部 58 − 按钮带(18+34) − 12 缝 = 478
        const auto* scroll = f.ui.find("todo.dialog.detail.scroll");
        DOCTEST_REQUIRE(scroll != nullptr);
        DOCTEST_CHECK(floatEq(scroll->frame.y - panel->frame.y, 58.0f));  // contentTop 76→58
        DOCTEST_CHECK(floatEq(scroll->frame.height, 478.0f));
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

    const auto* surface = frame.ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(surface != nullptr);
    for (int i = 0; i < 2; ++i) {
        const std::string cid =
            "todo.col.4.card.0.tag." + std::to_string(i);
        const auto* chipBg = frame.ui.find(cid + ".bg");
        const auto* chipText = frame.ui.find(cid + ".text");
        DOCTEST_REQUIRE(chipBg != nullptr);
        DOCTEST_REQUIRE(chipText != nullptr);
        // 调后字号 12（= fontSizeSmall - 1；调前 10 = fontSizeSmall - 3）
        DOCTEST_CHECK_EQ(chipText->fontSize, t.sizes.fontSizeSmall - 1.0f);
        DOCTEST_CHECK_EQ(chipText->fontSize, 11.0f);
        // chip 高跟着适配（调前 18），lineHeight 同高 → 文字不裁行
        DOCTEST_CHECK_EQ(chipBg->frame.height, 22.0f);
        DOCTEST_CHECK_EQ(chipText->lineHeight, chipBg->frame.height);
        // chip 高 ≥ 字号 + 上下各 5px 内边距
        DOCTEST_CHECK(chipBg->frame.height >= chipText->fontSize + 2.0f * 5.0f);
        // 不溢出卡片：chip 右缘 ≤ 卡右缘、下缘 ≤ 卡下缘
        DOCTEST_CHECK(chipBg->frame.x + chipBg->frame.width <=
                      surface->frame.x + surface->frame.width);
        DOCTEST_CHECK(chipBg->frame.y + chipBg->frame.height <=
                      surface->frame.y + surface->frame.height);
        // chip 宽 = 估宽（chipWidth 已随字号同步放大：20 + 6.6/字节；
        // 评宽 ≥ 真实字形宽在 test_font_em_layout.cpp 用真实字体复核）
        DOCTEST_CHECK_EQ(chipBg->frame.width,
                         module::todo::ui::chipWidth(kTags[static_cast<size_t>(i)]));
    }

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

// 创建/校验失败反馈 → 弹窗内嵌红字 formError
// H20：列改名入口随固定四列删除——本用例只保留新建任务的校验反馈链路
REGISTER_TEST(todo_ui_dialog_form_error,
              "todo_ui: 新建任务校验失败 → 内嵌红字 + 弹窗不关 + 输入清除（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());

    auto taskTitleExists = [&](const std::string& want) -> bool {
        auto l = env.svc->listAllTasks();
        if (!l.isOk()) return false;
        for (const auto& tk : l.value()) {
            if (tk.title == want) return true;
        }
        return false;
    };

    // 新建任务空标题 → 红字 + 不关（错误 toast 在弹窗内，不弹到窗外）
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
              "todo_ui: 新建任务 Esc=取消不落库；详情标签 Esc=清草稿（headless）", {
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

    // 2)（H20 删除）列改名入口随固定四列移除，原「重命名列 Esc=取消」链路不再可达。

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
        DOCTEST_CHECK_EQ(ph->text, std::string("+ 添加标签，回车添加"));
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

    // 板面几何：顶边 = pad + kHeaderH + spacingLarge（pad = spacingLarge*2，S4.2
    // 弹性内边距 32）。boardGeom 只剩顶边求值——n 列均匀数学与 cardLeft 随固定
    // 四列、菜单 x 改走 ctx.anchorX 一并退役
    const ui::BoardGeom g3 = ui::boardGeom(t);
    DOCTEST_CHECK_EQ(g3.boardTop,
                     ui_kit::kHeaderH + t.sizes.spacingLarge);  // 顶距 0（批L）

    // 卡片顶边：卡步进 = 卡高 + 卡间距
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
    // defaultColumnTitle（「新列 N」）已随建列弹窗删除，无对应断言

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

REGISTER_TEST(todo_archive_format,
              "Todo UI: formatArchivedAt 按 YYYY-MM-DD HH:MM:SS 输出",
              {
    using namespace module::todo;
    // 用固定 epoch 验证格式（2026-10-13 12:30:45 UTC+8 = 2026-10-13 12:30:45 本地）
    // epoch = 2026-10-13T04:30:45Z = 1792331445
    const int64_t epoch = 1792331445;
    const std::string s = ui::formatArchivedAt(epoch);
    // 主断言：19 字符 "YYYY-MM-DD HH:MM:SS"
    DOCTEST_CHECK_EQ(s.size(), 19u);
    // 前 4 位年份
    DOCTEST_CHECK(s[0] == '2' && s[1] == '0');
    // 第 5 位 '-'
    DOCTEST_CHECK_EQ(s[4], '-');
    // 第 8 位 '-'
    DOCTEST_CHECK_EQ(s[7], '-');
    // 第 11 位 ' '
    DOCTEST_CHECK_EQ(s[10], ' ');
    // 0 入参返回空
    DOCTEST_CHECK(ui::formatArchivedAt(0).empty());
    DOCTEST_CHECK(ui::formatArchivedAt(-1).empty());
    return true;
})

REGISTER_TEST(todo_archive_tasks_in_column_sort,
              "Todo UI: tasksInArchiveColumn 按 archived_at DESC 排序",
              {
    using namespace module::todo;
    std::vector<Task> tasks;
    Task a; a.id = 1; a.columnId = 10; a.archivedAt = 100; a.position = 2;
    Task b; b.id = 2; b.columnId = 10; b.archivedAt = 300; b.position = 1;
    Task c; c.id = 3; c.columnId = 10; c.archivedAt = 300; c.position = 0;
    // c 与 b 同 archivedAt，position 小的在前 (0 < 1)
    Task d; d.id = 4; d.columnId = 10; d.archivedAt = std::nullopt; d.position = 3;
    Task e; e.id = 5; e.columnId = 20; e.archivedAt = 500; e.position = 0;  // 不同列
    tasks = {a, b, c, d, e};

    auto result = ui::tasksInArchiveColumn(tasks, 10);
    DOCTEST_CHECK_EQ(result.size(), 4u);
    // 300 (b, then c), then 100 (a), then nullopt (d) → 300 treated as 0
    // wait: nullopt → value_or(0) = 0. 300 > 100 > 0.
    // So: b(300, pos=1) = c(300, pos=0), then a(100), then d(0)
    DOCTEST_CHECK_EQ(result[0]->id, 3);  // archivedAt=300, pos=0
    DOCTEST_CHECK_EQ(result[1]->id, 2);  // archivedAt=300, pos=1
    DOCTEST_CHECK_EQ(result[2]->id, 1);  // archivedAt=100
    DOCTEST_CHECK_EQ(result[3]->id, 4);  // archivedAt=nullopt→0
    return true;
})

// S3.3#3: 归档卡标题文字色 = textSecondary 二级文字 #4B5563
REGISTER_TEST(todo_ui_archive_card_title_color,
              "todo_ui: 归档卡标题文字色 = textSecondary（S3.3#3 二级文字 #4B5563）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    // 从种子列里找到归档列：kind == "archive"
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int64_t archiveColId = -1;
    for (const auto& c : cols.value()) {
        if (c.kind == "archive") { archiveColId = c.id; break; }
    }
    DOCTEST_REQUIRE(archiveColId > 0);
    auto t = env.svc->createTask(archiveColId, "归档标题应是黑色");
    DOCTEST_REQUIRE(t.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 归档列默认折叠（S3.3#6），先点击展开按钮再查卡片
    {
        Frame f0(mod.get());
        const auto* expandBtn = f0.ui.find("todo.col.3.expand.bg");
        DOCTEST_REQUIRE(expandBtn != nullptr);
        DOCTEST_REQUIRE(expandBtn->onClick != nullptr);
        expandBtn->onClick();
    }

    Frame frame(mod.get());
    const auto* el = frame.ui.find("todo.col.3.card.0.title");
    DOCTEST_REQUIRE(el != nullptr);
    const auto& tm = core::theme::current();
    // eui::Color 无 operator==：整色 CHECK_EQ 不可编译；text 元素颜色走 textColor 字段，
    // 非 color（color 是 rect 的 bg），expect 归档标题 = textSecondary #4B5563
    DOCTEST_CHECK_EQ(el->textColor.r, tm.colors.textSecondary.r);
    DOCTEST_CHECK_EQ(el->textColor.g, tm.colors.textSecondary.g);
    DOCTEST_CHECK_EQ(el->textColor.b, tm.colors.textSecondary.b);
    DOCTEST_CHECK_EQ(el->textColor.a, tm.colors.textSecondary.a);
    mod->onStop();
    return true;
})

// 列头 / 真卡 / 拖拽副本三处标题省略号三件套（overflow==Ellipsis && maxWidth>0 && !wrap）
REGISTER_TEST(todo_ui_title_ellipsis,
              "todo_ui: 列头+真卡+拖拽副本标题省略号三件套（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE(!list.value().empty());
    const int64_t col0 = list.value()[0].id;

    DOCTEST_REQUIRE(env.svc->renameColumn(col0, std::string(60, 'X')).isOk());
    auto t = env.svc->createTask(col0, std::string(120, 'A'));
    DOCTEST_REQUIRE(t.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto composeLayout = [&]() {
        ui.begin("todo_ellipsis_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    auto boundsOf = [](const core::dsl::Element* e) {
        return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
    };
    // inline ellipsis check (DOCTEST macros use early-return from test fn, cannot
    // be in a lambda without tripping -Wreturn-type in -Werror builds)
    auto checkEllipsis = [](const core::dsl::Element* el) -> bool {
        return el != nullptr && el->overflow == core::TextOverflow::Ellipsis &&
               el->maxWidth > 0.0f;
    };

    composeLayout();

    // 列头标题
    {
        const auto* el = ui.find("todo.col.0.title");
        DOCTEST_REQUIRE(el != nullptr);
        DOCTEST_CHECK_EQ(el->overflow, core::TextOverflow::Ellipsis);
        DOCTEST_CHECK(el->maxWidth > 0.0f);
    }

    // 真卡标题（拖前）
    const auto* cardTitle = ui.find("todo.col.0.card.0.title");
    DOCTEST_REQUIRE(cardTitle != nullptr);
    DOCTEST_CHECK_EQ(cardTitle->text, std::string(120, 'A'));
    DOCTEST_CHECK(checkEllipsis(cardTitle));

    // 拖拽 → 副本出现
    const auto* card = ui.find("todo.col.0.card.0.bg");
    DOCTEST_REQUIRE(card != nullptr);
    const eui::Rect cardBounds = boundsOf(card);
    eui::PointerEvent pe;
    card->onPress(pe, cardBounds);
    core::dsl::DragEvent ev;
    ev.x = cardBounds.x + 100.0f;
    ev.y = cardBounds.y + 50.0f;
    ev.totalX = 100.0;
    ev.totalY = 50.0;
    card->onDrag(ev);
    composeLayout();

    const auto* ghost = ui.find("todo.dragghost");
    DOCTEST_REQUIRE(ghost != nullptr);
    {
        const auto* el = ui.find("todo.dragghost.title");
        DOCTEST_REQUIRE(el != nullptr);
        DOCTEST_CHECK_EQ(el->overflow, core::TextOverflow::Ellipsis);
        DOCTEST_CHECK(el->maxWidth > 0.0f);
    }

    mod->onStop();
    return true;
})

// ---- H20 四列固定与归档折叠：列宽公式（无头纯函数）----
// 公式契约（docs/superpowers/plans/2026-10-23-UAT62-H20-...md §4）：
//   W = (contentW − 3×gap)/4；archiveW = lerp(W×25%, W, archiveT)；
//   其余三列 = (contentW − 3×gap − archiveW)/3。
// 双档取原型表的内容宽口径：窗口 1000 → 712、1600 → 1312（侧栏 208 + 双侧 pad 40）。
REGISTER_TEST(todo_ui_h20_column_widths,
              "todo_ui: H20 四列宽度派生 archiveT=0/0.5/1 双档", {
    using namespace module::todo::ui;
    constexpr float kGap = 16.0f;
    const float presets[] = {712.0f, 1312.0f};   // 1000 / 1600 窗口

    for (const float contentW : presets) {
        const float W = (contentW - 3.0f * kGap) / 4.0f;

        // archiveT=0（默认折叠）：归档 = W×25%，其余三列均分剩余宽
        const BoardColumnWidths collapsed =
            boardColumnWidths(contentW, kGap, 4, 3, 0.0f);
        DOCTEST_CHECK(std::fabs(collapsed.archive - W * 0.25f) < 0.01f);
        const float expectedCol = (contentW - 3.0f * kGap - W * 0.25f) / 3.0f;
        DOCTEST_CHECK(std::fabs(collapsed.normal - expectedCol) < 0.01f);
        // 列宽 + 间隙必须铺满内容宽（四列 = 3×gap）
        DOCTEST_CHECK(std::fabs(collapsed.archive + 3.0f * collapsed.normal +
                                3.0f * kGap - contentW) < 0.01f);

        // archiveT=1（展开）：四列等分，与普通列完全同逻辑
        const BoardColumnWidths expanded =
            boardColumnWidths(contentW, kGap, 4, 3, 1.0f);
        DOCTEST_CHECK(std::fabs(expanded.archive - W) < 0.01f);
        DOCTEST_CHECK(std::fabs(expanded.normal - W) < 0.01f);
        DOCTEST_CHECK(std::fabs(expanded.archive + 3.0f * expanded.normal +
                                3.0f * kGap - contentW) < 0.01f);

        // archiveT=0.5（动画中间帧）：归档宽严格介于 W×25% 与 W 之间（线性插值中点）
        const BoardColumnWidths mid = boardColumnWidths(contentW, kGap, 4, 3, 0.5f);
        DOCTEST_CHECK(mid.archive > W * 0.25f);
        DOCTEST_CHECK(mid.archive < W);
        DOCTEST_CHECK(std::fabs(mid.archive - (W * 0.25f + W) * 0.5f) < 0.01f);
        DOCTEST_CHECK(mid.normal < collapsed.normal);   // 归档吃掉的空间由三列等分
        DOCTEST_CHECK(mid.normal > W);                  // 但三列仍宽于展开均分宽
        DOCTEST_CHECK(std::fabs(mid.archive + 3.0f * mid.normal + 3.0f * kGap -
                                contentW) < 0.01f);
    }

    // 无归档列（老库/自定义列）：退回等分，不产生 25% 窄列
    const BoardColumnWidths noArchive =
        boardColumnWidths(712.0f, kGap, 4, -1, 0.0f);
    DOCTEST_CHECK(std::fabs(noArchive.normal - (712.0f - 3.0f * kGap) / 4.0f) < 0.01f);
    DOCTEST_CHECK(std::fabs(noArchive.archive - noArchive.normal) < 0.01f);
    // 零列不除零
    DOCTEST_CHECK_EQ(boardColumnWidths(712.0f, kGap, 0, -1, 0.0f).normal, 0.0f);

    // 虚拟化窗口：pitch=92，可视高 856 → 顶部构建 0..12（上下各 3 张缓冲）；
    // 底部 offset=300×92−8−856 时 last=300、first=287
    const float contentHeight = columnContentHeight(300);
    DOCTEST_CHECK(std::fabs(contentHeight - (300.0f * 92.0f - 8.0f)) < 0.01f);
    const auto top = virtualCardRange(300, 0.0f, 856.0f, 92.0f);
    DOCTEST_CHECK_EQ(top.first, static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(top.second, static_cast<size_t>(13));
    const auto bottom = virtualCardRange(300, contentHeight - 856.0f, 856.0f, 92.0f);
    DOCTEST_CHECK_EQ(bottom.first, static_cast<size_t>(287));
    DOCTEST_CHECK_EQ(bottom.second, static_cast<size_t>(300));
    // 空列与退化 pitch：返回空窗口（调用方不构建卡片）
    DOCTEST_CHECK_EQ(virtualCardRange(0, 0.0f, 856.0f, 92.0f).second,
                     static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(virtualCardRange(10, 0.0f, 856.0f, 0.0f).second,
                     static_cast<size_t>(0));
    // 槽位等距：第 i 张 y = i×92
    DOCTEST_CHECK(std::fabs(cardSlotY(5) - 460.0f) < 0.01f);
    return true;
})

// ---- H20 渲染契约：折叠态四列几何 + 窄态不越界 + 入口删除 ----
REGISTER_TEST(todo_ui_h20_board_render,
              "todo_ui: H20 归档折叠渲染（四列宽/竖排窄态不越界/新建入口唯一）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE_EQ(cols.value().size(), static_cast<size_t>(4));
    int archiveIndex = -1;
    int64_t archiveColId = 0;
    for (size_t i = 0; i < cols.value().size(); ++i) {
        if (cols.value()[i].kind == "archive") {
            archiveIndex = static_cast<int>(i);
            archiveColId = cols.value()[i].id;
            break;
        }
    }
    DOCTEST_REQUIRE(archiveIndex >= 0);
    // 归档列放一张卡：折叠态不渲染卡片，展开结构（目标态）下可查到
    DOCTEST_REQUIRE(env.svc->createTask(archiveColId, "归档样例").isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    eui::Ui ui;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto layoutFrame = [&]() {
        ui.begin("todo_h20_frame");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen);
    };
    layoutFrame();

    const auto& t = core::theme::current();
    const float contentW = 1600.0f - t.sizes.spacingLarge * 4.0f;  // module: pad 32×2
    const float gap = t.sizes.spacingLarge;
    const float W = (contentW - 3.0f * gap) / 4.0f;
    const float archiveW = W * 0.25f;
    const float normalW = (contentW - 3.0f * gap - archiveW) / 3.0f;

    auto colId = [&](int idx) { return "todo.col." + std::to_string(idx); };
    // 四列宽度：折叠态归档 W×25%，其余三列均分剩余；x 依次递增（row 流式重排）
    const auto* archiveBg = ui.find(colId(archiveIndex) + ".bg");
    DOCTEST_REQUIRE(archiveBg != nullptr);
    DOCTEST_CHECK(std::fabs(archiveBg->frame.width - archiveW) < 0.5f);
    float prevRight = -1.0f;
    for (size_t i = 0; i < cols.value().size(); ++i) {
        const auto* bg = ui.find(colId(static_cast<int>(i)) + ".bg");
        DOCTEST_REQUIRE(bg != nullptr);
        const float expected = (static_cast<int>(i) == archiveIndex) ? archiveW
                                                                     : normalW;
        DOCTEST_CHECK(std::fabs(bg->frame.width - expected) < 0.5f);
        if (prevRight >= 0.0f) {
            DOCTEST_CHECK(std::fabs(bg->frame.x - (prevRight + gap)) < 0.5f);
        }
        prevRight = bg->frame.x + bg->frame.width;
    }
    // 右缘 = 窗口宽 − 双侧 pad（pad = 2×spacingLarge = 32/侧）
    DOCTEST_CHECK(std::fabs(prevRight - (1600.0f - t.sizes.spacingLarge * 2.0f)) < 0.5f);

    // 折叠态：竖排窄态存在，普通列头/加号/展开按钮都不画
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".title") == nullptr);
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".archivetoggle") == nullptr);
    const auto* compactHit = ui.find(colId(archiveIndex) + ".expand.bg");
    DOCTEST_REQUIRE(compactHit != nullptr);
    DOCTEST_CHECK(compactHit->onClick != nullptr);
    const float colX = archiveBg->frame.x;
    const float colRight = colX + archiveBg->frame.width;
    const float colBottom = archiveBg->frame.y + archiveBg->frame.height;
    // 竖排字/计数 pill/chevron 都必须 clip 在列内（不越界）
    for (int gi = 0; gi < 4; ++gi) {
        const auto* glyph = ui.find(colId(archiveIndex) + ".compact.label." +
                                    std::to_string(gi));
        DOCTEST_REQUIRE(glyph != nullptr);
        DOCTEST_CHECK(glyph->frame.x >= colX - 0.01f);
        DOCTEST_CHECK(glyph->frame.x + glyph->frame.width <= colRight + 0.01f);
        DOCTEST_CHECK(glyph->frame.y + glyph->frame.height <= colBottom + 0.01f);
    }
    const auto* pill = ui.find(colId(archiveIndex) + ".compact.pill");
    const auto* count = ui.find(colId(archiveIndex) + ".compact.count");
    const auto* chev = ui.find(colId(archiveIndex) + ".compact.chev");
    DOCTEST_REQUIRE(pill != nullptr);
    DOCTEST_REQUIRE(count != nullptr);
    DOCTEST_REQUIRE(chev != nullptr);
    for (const auto* el : {pill, count, chev}) {
        DOCTEST_CHECK(el->frame.x >= colX - 0.01f);
        DOCTEST_CHECK(el->frame.x + el->frame.width <= colRight + 0.01f);
        DOCTEST_CHECK(el->frame.y + el->frame.height <= colBottom + 0.01f);
    }
    DOCTEST_CHECK_EQ(count->text, std::string("1"));

    // 入口唯一：+ 列 / 列头 + 新建 / 空列「+ 添加任务」全部不存在
    DOCTEST_CHECK(ui.find("todo.btn.addcol.bg") == nullptr);
    for (size_t i = 0; i < cols.value().size(); ++i) {
        DOCTEST_CHECK(ui.find(colId(static_cast<int>(i)) + ".add.bg") == nullptr);
        DOCTEST_CHECK(ui.find(colId(static_cast<int>(i)) + ".emptyadd.bg") == nullptr);
    }
    const auto* newTask = ui.find("todo.btn.add.bg");
    DOCTEST_REQUIRE(newTask != nullptr);
    const auto* newTaskText = ui.find("todo.btn.add.text");
    if (newTaskText == nullptr) newTaskText = ui.find("todo.btn.add.text.normal");
    DOCTEST_REQUIRE(newTaskText != nullptr);
    DOCTEST_CHECK_EQ(newTaskText->text, std::string("新建任务"));
    // 标题区宽 = contentW − 动作区实际宽（240+88+120+2×spacingSmall）
    const auto* h1 = ui.find("todo.h1");
    DOCTEST_REQUIRE(h1 != nullptr);
    const float actionsW = 240.0f + 88.0f + 120.0f + t.sizes.spacingSmall * 2.0f;
    DOCTEST_CHECK(std::fabs(h1->frame.width - (contentW - actionsW)) < 0.5f);

    // 点击窄态 → 目标态展开（archiveT 由模块侧动画推进；无头下结构先行）
    const auto* hit = ui.find(colId(archiveIndex) + ".expand.bg");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onClick != nullptr);
    hit->onClick();
    layoutFrame();
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".title") != nullptr);
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".archivetoggle.bg") != nullptr);
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".card.0.title") != nullptr);

    // 整条列头即折叠开关（2026-10-31 反馈：原先只有右侧 32px 箭头可点）：
    // head 热区覆盖整个列宽，绑定与箭头同一动作；点一次折叠、再点窄态展开回本段前提。
    const auto* headToggle = ui.find(colId(archiveIndex) + ".head");
    DOCTEST_REQUIRE(headToggle != nullptr);
    DOCTEST_REQUIRE(headToggle->onClick != nullptr);
    const auto* archBg = ui.find(colId(archiveIndex) + ".bg");
    DOCTEST_REQUIRE(archBg != nullptr);
    DOCTEST_CHECK(std::fabs(headToggle->frame.width - archBg->frame.width) < 0.5f);
    headToggle->onClick();
    layoutFrame();
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".expand.bg") != nullptr);
    const auto* reExpand = ui.find(colId(archiveIndex) + ".expand.bg");
    DOCTEST_REQUIRE(reExpand != nullptr);
    DOCTEST_REQUIRE(reExpand->onClick != nullptr);
    reExpand->onClick();
    layoutFrame();
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".archivetoggle.bg") != nullptr);

    // ---- 1000×640 最小档：内容宽按真实 pad（2×spacingLarge = 32/侧）复算 ----
    // 展开态：四列等分 W=(contentW−3×gap)/4；onPageEnter 落目标态（不补播动画）
    screen.width = 1000.0f;
    screen.height = 640.0f;
    mod->onPageEnter();
    layoutFrame();
    const float contentW1000 = screen.width - t.sizes.spacingLarge * 4.0f;  // 936
    const float W1000 = (contentW1000 - 3.0f * gap) / 4.0f;                 // 222
    float right1000 = -1.0f;
    for (size_t i = 0; i < cols.value().size(); ++i) {
        const auto* bg = ui.find(colId(static_cast<int>(i)) + ".bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(std::fabs(bg->frame.width - W1000) < 0.5f);  // 展开四列等分
        if (right1000 >= 0.0f) {
            DOCTEST_CHECK(std::fabs(bg->frame.x - (right1000 + gap)) < 0.5f);
        }
        right1000 = bg->frame.x + bg->frame.width;
    }
    DOCTEST_CHECK(std::fabs(right1000 -
                            (screen.width - t.sizes.spacingLarge * 2.0f)) < 0.5f);  // 968

    // 折叠态：归档 = W×25%，其余三列均分剩余宽
    const auto* toggle = ui.find(colId(archiveIndex) + ".archivetoggle.bg");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    mod->onPageEnter();
    layoutFrame();
    const float archiveW1000 = W1000 * 0.25f;                                     // 55.5
    const float normalW1000 = (contentW1000 - 3.0f * gap - archiveW1000) / 3.0f;  // 277.5
    right1000 = -1.0f;
    for (size_t i = 0; i < cols.value().size(); ++i) {
        const auto* bg = ui.find(colId(static_cast<int>(i)) + ".bg");
        DOCTEST_REQUIRE(bg != nullptr);
        const float expected = (static_cast<int>(i) == archiveIndex) ? archiveW1000
                                                                    : normalW1000;
        DOCTEST_CHECK(std::fabs(bg->frame.width - expected) < 0.5f);
        if (right1000 >= 0.0f) {
            DOCTEST_CHECK(std::fabs(bg->frame.x - (right1000 + gap)) < 0.5f);
        }
        right1000 = bg->frame.x + bg->frame.width;
    }
    DOCTEST_CHECK(std::fabs(right1000 -
                            (screen.width - t.sizes.spacingLarge * 2.0f)) < 0.5f);
    // 折叠结构：普通列头让位给竖排窄态
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".title") == nullptr);
    DOCTEST_CHECK(ui.find(colId(archiveIndex) + ".expand.bg") != nullptr);

    mod->onStop();
    return true;
})

// 逾期弹窗行标题省略号三件套
REGISTER_TEST(todo_ui_overdue_row_ellipsis,
              "todo_ui: 逾期弹窗行标题省略号三件套（headless）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE(!list.value().empty());
    const int64_t todoCol = list.value()[0].id;

    auto ov = env.svc->createTask(todoCol, std::string(120, 'B'));
    DOCTEST_REQUIRE(ov.isOk());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    {
        module::todo::Task t = ov.value();
        std::tm dueTm{};
        core::time::localTime(now, &dueTm);
        dueTm.tm_hour = 12;
        dueTm.tm_min = 0;
        dueTm.tm_sec = 0;
        t.dueAt = std::mktime(&dueTm) - 2 * 86400;
        DOCTEST_REQUIRE(env.svc->updateTask(t).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        Frame frame(mod.get());
        const auto* btn = frame.ui.find("todo.btn.overdue.hit");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
        frame.refresh();
        const auto* el = frame.ui.find("todo.dialog.overdue.row.0.title");
        DOCTEST_REQUIRE(el != nullptr);
        DOCTEST_CHECK_EQ(el->overflow, core::TextOverflow::Ellipsis);
        DOCTEST_CHECK(el->maxWidth > 0.0f);
    }

    mod->onStop();
    return true;
})

// F1 弹窗防回潮：P1/P2/P3/P10 逐弹窗「全元素在框内」+ 标题锚（y=16 / fontSize=20，
// 规格 S2.4）。回退即红：任一可见子孙 frame 越出 panel（>0.5px）、标题 y 偏移≠16、
// 标题字号≠20。弹窗串行开合走真实点击链（新建 → 逾期 → 行点击进详情 → 详情删除 chip → 列删除确认）。
REGISTER_TEST(todo_ui_dialog_frame_containment,
              "todo_ui: P1/P2/P3/P10 弹窗全元素在框内 + 标题 y=16/字号 20（防回潮）", {
    TestEnv env;
    DOCTEST_REQUIRE(env.ready);
    // 逾期谓词限定 kind IN ('todo','doing')：用种子默认列（用户新建列 kind="" 不计逾期）
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(!cols.value().empty());
    auto tsk = env.svc->createTask(cols.value()[0].id, "框内任务");
    DOCTEST_REQUIRE(tsk.isOk());
    // 同一条任务兼作 P2 详情与 P3 逾期数据源：到期 = 本地昨天同时刻（日历日差 ≥1）
    {
        module::todo::Task t = tsk.value();
        std::tm dueTm{};
        core::time::localTime(static_cast<int64_t>(std::time(nullptr)), &dueTm);
        t.dueAt = std::mktime(&dueTm) - 86400;
        DOCTEST_REQUIRE(env.svc->updateTask(t).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    Frame frame(mod.get());
    auto click = [&](const char* id) -> bool {
        auto* b = frame.ui.find(id);
        DOCTEST_REQUIRE(b != nullptr);
        DOCTEST_REQUIRE(b->onClick != nullptr);
        b->onClick();
        frame.refresh();
        frame.ui.layout(1600.0f, 1000.0f);
        return true;
    };
    auto check = [&](const char* panelId, const char* titleId, const char* tag) -> bool {
        std::string v = dialog_frame::allInside(frame.ui, panelId);
        DOCTEST_CHECK_MESSAGE(v.empty(), std::string(tag) + " 全元素在框内: " + v);
        v = dialog_frame::titleAnchor(frame.ui, panelId, titleId);
        DOCTEST_CHECK_MESSAGE(v.empty(), std::string(tag) + " 标题锚: " + v);
            return true;
    };

    // P1 新建任务
    DOCTEST_CHECK(click("todo.btn.add.bg"));
    DOCTEST_CHECK(check("todo.dialog.newtask.panel", "todo.dialog.newtask.title", "P1 新建"));
    click("todo.dialog.newtask.backdrop");   // 关

    // P3 逾期 → 行点击跳 P2 详情（overdue 行 onClick = 关逾期 + openDetail）
    DOCTEST_CHECK(click("todo.btn.overdue.hit"));
    DOCTEST_CHECK(check("todo.dialog.overdue.panel", "todo.dialog.overdue.title", "P3 逾期"));
    DOCTEST_CHECK(click("todo.dialog.overdue.row.0"));
    DOCTEST_CHECK(check("todo.dialog.detail.panel", "todo.dialog.detail.title", "P2 详情"));

    // P10 删除确认（详情删除 chip → 确认弹窗）
    DOCTEST_CHECK(click("todo.dialog.detail.delete.bg"));
    DOCTEST_CHECK(check("todo.dialog.delcol.panel", "todo.dialog.delcol.title", "P10 删除确认"));

    mod->onStop();
    return true;
})
