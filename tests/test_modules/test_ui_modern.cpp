// tests/test_modules/test_ui_modern.cpp
// UI 现代化（plan 2026-09-28-ui-modernization）无头断言。
// M3-1（2026-09-28）：Home 统计卡换 card + Token 速率 sparkline 接线 +
// 速率采样环护栏。sparkline 数据源细节用例见
// tests/test_core/test_proxy_stats.cpp（proxy_stats_rate_series_*），本文件
// 管「空 → {0.0f}」护栏（防 lineChart 对空 values 填演示数据）与 Home
// compose 接线。
//
// M1（2026-09-28，ui/m1-todo）用例并入本文件：
//   1. 统一锚点换算纯测（含 Y、含滚动偏移、dpi 退化）—— M1-1
//   2. todo 看板右键锚点 = 板几何经统一换算（含滚动偏移口径保留）—— M1-2
//   3. claude profile 列表右键菜单四项 + 动作同源 + 隐形卡守卫 —— M1-5
// 三个用例随各自任务（M1-1/M1-2/M1-5）分批加入本文件。
// 后续批次（M2/M3）的用例（virtualList / card 布局不变式、charts 数据接线）
// 随各自任务追加到本文件。
// M1-3/M1-4（2026-09-28，ui/rest）：md 树/编辑器右键统一换算 + 编辑器
// input 直挂 onContextMenu（删逐帧 hack）；4523e09 隐形菜单回归锁。
// M3 剩余（2026-09-28，ui/rest）：profile 列表虚拟化 slot 窗口 + 滚动回退、
// 调用记录 virtualList、todo 卡容器换 card、Home API barChart + recent 换
// card、三处背景渐变。
// 证据链 = 代码路径 + 本无头 compose 测试；真实鼠标点验留给用户人工确认
#include "doctest.h"
#include <eui/types.h>
#include "test_support.h"

#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/theme.h"
#include <eui/dsl.h>
#include <memory>
#include <string>
#include <vector>
#include "test_support.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "core/ui_geometry.h"
#include "module_claude_proxy/claude_service.h"
#include "module_home/home_service.h"
#include "module_markdown/md_ids.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/metrics.h"  // kHeaderH（板几何断言）
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();

using module::markdown::kEditorInputHitId;

namespace {

// 假时钟 RAII：测试结束自动 reset（防污染后续用例），与 test_proxy_stats.cpp
// 同款（确定性速率采样需要假时钟）
struct FakeClock {
    int64_t now = 1000000;
    FakeClock() { core::proxy_stats::setTestClock([this] { return now; }); }
    ~FakeClock() { core::proxy_stats::resetForTest(); }
};

// ---- M1-2 用：todo 无头环境（照 test_todo_ui.cpp 的 TestEnv 形态）----
constexpr const char* kTodoDbPrefix = "devdesk_test_uimodern_todo_";

struct TodoEnv : test_support::TestEnv {
    std::filesystem::path statePath;
    std::unique_ptr<module::todo::TodoService> svc;

    TodoEnv() : test_support::TestEnv(kTodoDbPrefix, "", true) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
        statePath = path.string() + ".view_state.json";
        std::error_code ec;
        std::filesystem::remove(statePath, ec);
        test_support::setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH",
                                statePath.string());
    }

    ~TodoEnv() {
        test_support::setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH", "");
        std::error_code ec;
        std::filesystem::remove(statePath, ec);
    }
};

// 无头 compose 一帧（todo），返回构建后的 Ui
struct TodoFrame {
    eui::Ui ui;
    eui::Screen screen;
    ::core::Module* mod = nullptr;

    TodoFrame(::core::Module* m, float w = 1600.0f, float h = 1000.0f)
        : mod(m) {
        screen.width = w;
        screen.height = h;
        refresh();
    }
    void refresh() {
        ui.begin("todo_uimodern_frame");
        mod->compose(ui, screen);
        ui.end();
    }
};

// ---- M1-5 用：claude 无头环境（照 test_claude_ui.cpp 的 TestEnv 形态）----
constexpr const char* kClaudeDbPrefix = "devdesk_test_uimodern_claude_";
constexpr const char* kClaudeKeychainService = "DevDeskTestUiModernClaude";

struct ClaudeEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    ClaudeEnv() : test_support::TestDb(kClaudeDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(
            db->db(), kClaudeKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~ClaudeEnv() { module::claude::ClaudeService::setGlobal(nullptr); }
};

} // namespace

// ---- M3-1 用例（Home 统计卡 / sparkline 护栏）----

REGISTER_TEST(uiModernSparkSeriesGuard,
              "uiModern: rateSeries 空 → {0.0f}（lineChart demo 数据护栏）", {
    core::proxy_stats::resetForTest();
    FakeClock clock;

    // 无任何 usage 事件：sparkline 数据源必须兜底 {0.0f}，绝不填 lineChart
    // 内置演示序列（linechart.h build() 对空 values 的行为）
    const auto series = core::proxy_stats::rateSeries(24);
    DOCTEST_CHECK_EQ(series.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(series[0], 0.0f);

    core::proxy_stats::resetForTest();
    return true;
})

REGISTER_TEST(uiModernProxyRateRing,
              "uiModern: 速率采样环写入后序列非空、末点 == 最近采样", {
    core::proxy_stats::resetForTest();
    FakeClock clock;
    const uint64_t id = core::proxy_stats::beginStream();

    // 3 个事件（每 500ms，累计值口径）：最近采样速率 = (150-50)/1s = 100
    core::proxy_stats::updateStream(id, 50);
    clock.now += 500;
    core::proxy_stats::updateStream(id, 100);
    clock.now += 500;
    core::proxy_stats::updateStream(id, 150);

    const auto series = core::proxy_stats::rateSeries(24);
    DOCTEST_CHECK_EQ(series.size(), static_cast<size_t>(3));
    // 首点 = 首个事件处速率（窗口 <100ms → 0）
    DOCTEST_CHECK_EQ(series[0], 0.0f);
    // 末点 == 最近采样（t=1000 处：(150-50)/1s = 100）
    DOCTEST_CHECK(series.back() > 99.9f && series.back() < 100.1f);

    core::proxy_stats::endStream(id);
    core::proxy_stats::resetForTest();
    return true;
})

REGISTER_TEST(uiModernHomeCardSpark,
              "uiModern: Home 统计卡换 card + Token sparkline 接线", {
    // 场景 A：空会话（无采样）→ sparkline 拿兜底 {0.0f} 绘制，不崩
    {
        core::proxy_stats::resetForTest();
        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Screen screen;
        screen.width = 1200.0f; // 最小窗口宽度（卡片 208px 仍可容纳）
        screen.height = 720.0f;
        eui::Ui ui;
        ui.begin("home_m3_empty");
        mod->compose(ui, screen);
        ui.end();

        DOCTEST_CHECK(ui.find("home.stat.tokens") != nullptr);
        DOCTEST_CHECK(ui.find("home.stat.tokens.spark") != nullptr);
        // 保留面：0.5s onTimer 重绘驱动
        DOCTEST_CHECK(ui.find("home.stat.tokens.timer") != nullptr);

        mod->onStop();
        core::proxy_stats::resetForTest();
    }

    // 场景 B：有采样 → sparkline 以真实点绘制；card 内部结构断言
    {
        FakeClock clock;
        const uint64_t id = core::proxy_stats::beginStream();
        core::proxy_stats::updateStream(id, 50);
        clock.now += 500;
        core::proxy_stats::updateStream(id, 100);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Screen screen;
        screen.width = 1200.0f;
        screen.height = 720.0f;
        eui::Ui ui;
        ui.begin("home_m3_streaming");
        mod->compose(ui, screen);
        ui.end();

        // 统计卡根节点（card 组件）：根 id 不变，语义未变
        DOCTEST_CHECK(ui.find("home.stat.notes") != nullptr);
        // card 组件内部结构：.bg 携带 shadow（panelShadow 随 CardStyle 注入，
        // M3-4 复查项）；.content 为内容容器（旧手绘 rect 无此子节点）
        DOCTEST_CHECK(ui.find("home.stat.tokens.bg") != nullptr);
        DOCTEST_CHECK(ui.find("home.stat.tokens.content") != nullptr);
        // sparkline 接线：lineChart 根节点在卡内
        DOCTEST_CHECK(ui.find("home.stat.tokens.spark") != nullptr);
        // sparkline 数据源 = 2 个真实采样点
        const auto series = core::proxy_stats::rateSeries(24);
        DOCTEST_CHECK_EQ(series.size(), static_cast<size_t>(2));

        mod->onStop();
        core::proxy_stats::endStream(id);
        core::proxy_stats::resetForTest();
    }
    return true;
})

// ---- 1. 统一锚点换算纯测（M1-1）----

REGISTER_TEST(uiModernAnchorPure,
              "uiModern: 统一锚点换算（含 Y、含滚动偏移、dpi 退化）", {
    // 基准：窗口 (100, 200)，页面原点 x=20，dpi=1.0（onContextMenu 通道口径）
    const ::core::MenuAnchor a0 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 1.0f);
    DOCTEST_CHECK_EQ(a0.x, 80.0f);
    DOCTEST_CHECK_EQ(a0.y, 200.0f);
    // 滚动偏移（基于非滚动几何的锚点，4523e09 口径）：
    const ::core::MenuAnchor a1 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 36.0f, 1.0f);
    DOCTEST_CHECK_EQ(a1.y, 164.0f);
    // dpi=2.0（onPress/onDrag 通道口径：原始值 / 框架值）：
    const ::core::MenuAnchor a2 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 2.0f);
    DOCTEST_CHECK_EQ(a2.x, 30.0f);   // 100/2 - 20
    DOCTEST_CHECK_EQ(a2.y, 100.0f);
    // 退化 dpi（0/负/NaN）→ 按 1.0 口径（windowToCompose 同款）：
    DOCTEST_CHECK_EQ(
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 0.0f).x,
        80.0f);
    return true;
})

// ---- 2. todo 看板右键锚点 = 板几何经统一换算（M1-2）----

REGISTER_TEST(uiModernTodoMenuAnchor,
              "uiModern todo: 右键菜单锚点 = 板几何经统一换算（卡片 + 列头）", {
    TodoEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto c1 = env.svc->createColumn("右键源列");
    DOCTEST_REQUIRE(c1.isOk());
    auto t1 = env.svc->createTask(c1.value().id, "任务A");
    DOCTEST_REQUIRE(t1.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // 迁移默认 4 列 + 新建 1 列（「右键源列」= col.4，同既有用例口径）：
    // 板几何按 5 列推算（与 drawContextMenu 内 host.columns->size() 一致）
    const auto& tok = ::core::theme::current();
    const ::module::todo::ui::BoardGeom g =
        ::module::todo::ui::boardGeom(1600.0f, 5, tok);
    // 锚点 = 列中芯 / 卡片视觉底；无 Router（headless）→ 页面原点 0，
    // 滚动 0 → 与统一换算结果逐值一致
    const float expectColumnX = ::module::todo::ui::cardLeft(g, 4) + g.colW * 0.5f;
    const float expectCardY = ::module::todo::ui::cardTop(g, 0) +
                              ::module::todo::ui::kCardHeight + 4.0f;

    {
        TodoFrame frame(mod.get());
        DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.bg") != nullptr);
        // 右键任务A → 菜单打开；锚点经统一换算（坐标 = 列中芯 / 卡片底）
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.card.0.bg")->onContextMenu(pe, rect);
        frame.refresh();
        const auto* menu = frame.ui.find("todo.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK_EQ(menu->x, expectColumnX);
        DOCTEST_CHECK_EQ(menu->y, expectCardY);
    }
    {
        TodoFrame frame(mod.get());
        // 右键列头 → 菜单锚点 y = 板顶下方（滚动区外，scrollY = 0）
        eui::PointerEvent pe;
        eui::Rect rect;
        frame.ui.find("todo.col.4.head")->onContextMenu(pe, rect);
        frame.refresh();
        const auto* menu = frame.ui.find("todo.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK_EQ(menu->x, expectColumnX);
        DOCTEST_CHECK_EQ(menu->y, g.boardTop + 4.0f);
    }

    mod->onStop();
    return true;
})

// ---- 3. claude profile 列表右键菜单（M1-5）----

REGISTER_TEST(uiModernClaudeProfileCtxMenu,
              "uiModern claude_module: 列表右键菜单四项（编辑/复制/删除/设为当前）", {
    ClaudeEnv env;
    auto a = env.svc->createProfile("官方", "https://api.anthropic.com",
                                    "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    auto b = env.svc->createProfile("中转", "https://relay.example.com",
                                    "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto compose = [&]() {
        ui.begin("claude_ctx_frame");
        mod->compose(ui, screen);
        ui.end();
    };

    // 卡片行容器挂了右键回调
    {
        compose();
        const auto* wrap = ui.find("claude.profile.1.wrap");
        DOCTEST_REQUIRE(wrap != nullptr);
        DOCTEST_REQUIRE(wrap->onContextMenu != nullptr);
    }

    // 右键 profile.1（中转）→ 菜单打开，四项 + 锚点 = 指针坐标
    //（无 Router → 页面原点 0，dpi=1 → (400, 300) 原样）
    {
        compose();
        eui::PointerEvent pe;
        eui::Rect rect;
        pe.x = 400;
        pe.y = 300;
        ui.find("claude.profile.1.wrap")->onContextMenu(pe, rect);
        compose();
        const auto* menu = ui.find("claude.list.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK_EQ(menu->x, 400.0f);
        DOCTEST_CHECK_EQ(menu->y, 300.0f);
        for (int i = 0; i < 4; ++i) {
            DOCTEST_CHECK(ui.find("claude.list.ctx.level.0.item." +
                                  std::to_string(i)) != nullptr);
        }
        const auto* l0 = ui.find("claude.list.ctx.level.0.label.0");
        const auto* l1 = ui.find("claude.list.ctx.level.0.label.1");
        const auto* l2 = ui.find("claude.list.ctx.level.0.label.2");
        const auto* l3 = ui.find("claude.list.ctx.level.0.label.3");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_REQUIRE(l1 != nullptr);
        DOCTEST_REQUIRE(l2 != nullptr);
        DOCTEST_REQUIRE(l3 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, std::string("编辑"));
        DOCTEST_CHECK_EQ(l1->text, std::string("复制"));
        DOCTEST_CHECK_EQ(l2->text, std::string("删除"));
        DOCTEST_CHECK_EQ(l3->text, std::string("设为当前"));
    }

    // 点「设为当前」→ 激活位变化（与既有按钮同源：requestSetActive → doSetActive）
    {
        auto* item = ui.find("claude.list.ctx.level.0.item.3");
        DOCTEST_REQUIRE(item != nullptr);
        item->onClick();
        // 菜单已关（onSelect 置关；常驻构建但 opacity 收起）
        compose();
        const auto* menu = ui.find("claude.list.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK_EQ(menu->opacity, 0.0f);
        // 激活位：官方 → 中转（service 层真变更 = 动作与按钮同源）
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), static_cast<size_t>(2));
        bool aActive = false;
        bool bActive = false;
        for (const auto& p : list.value()) {
            if (p.id == a.value().id) aActive = p.isActive;
            if (p.id == b.value().id) bActive = p.isActive;
        }
        DOCTEST_CHECK_EQ(aActive, false);
        DOCTEST_CHECK_EQ(bActive, true);  // 「中转」已设为当前
    }

    // 再右键 → 点「删除」→ 弹出删除确认（与既有按钮同源：
    // requestDelete 委托 → deleteConfirmOpen_）
    {
        eui::PointerEvent pe;
        eui::Rect rect;
        pe.x = 400;
        pe.y = 300;
        compose();  // 上一步 compose 对齐后重取元素再触发
        const auto* wrap = ui.find("claude.profile.1.wrap");
        DOCTEST_REQUIRE(wrap != nullptr);
        wrap->onContextMenu(pe, rect);
        compose();
        auto* item = ui.find("claude.list.ctx.level.0.item.2");
        DOCTEST_REQUIRE(item != nullptr);
        item->onClick();
        compose();
        DOCTEST_CHECK(ui.find("claude.dialog.delete") != nullptr);
    }

    mod->onStop();
    test_support::cleanupProfileKey(kClaudeKeychainService, a.value().id);
    test_support::cleanupProfileKey(kClaudeKeychainService, b.value().id);
    return true;
})REGISTER_TEST(uiModernMdViewTransition,
              "uiModern: md 四态切换过渡（md.root 0.15s OutCubic + 切换帧淡出）", {
    test_support::MdHeadlessEnv env("devdesk_test_ui_m2_", "mdtrans", "m2.db");
    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    {
        eui::Ui ui;
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        ui.begin("md_m2_a");
        mod->compose(ui, screen);
        ui.end();

        // 稳态：root 目标 opacity = 1（每帧目标一致不动画）；过渡声明在
        // md.root 上（0.15s OutCubic）
        const auto* root = ui.find("md.root");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_CHECK_EQ(root->opacity, 1.0f);
        DOCTEST_CHECK(root->transition.enabled);
        DOCTEST_CHECK(root->transition.durationSeconds > 0.149f &&
                      root->transition.durationSeconds < 0.151f);
        DOCTEST_CHECK(root->transition.ease == core::Ease::OutCubic);
    }

    // Ctrl+Shift+P 切态 → setMode 触发 beginViewTransition（与顶栏 segmented
    // onChange 同一收口，两路动画行为一致）
    eui::KeyEvent key;
    key.key = eui::InputKey::P;
    key.action = eui::KeyAction::Press;
    key.modifiers.control = true;
    key.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(key));

    {
        eui::Ui ui;
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        ui.begin("md_m2_b");
        mod->compose(ui, screen);
        ui.end();

        // 切换后首帧：root 目标 opacity = 0（淡出起点；真机下 0.15s 后计时器
        // 拨回 1 触发淡入，无头环境无时钟推进，目标保持淡出值）
        const auto* root = ui.find("md.root");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_CHECK(root->opacity < 0.1f);
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernDialogClosedKept,
              "uiModern: dialog/toast 关闭态常驻构建 + opacity 0（EUI 内建过渡护栏）", {
    TodoEnv env;
    DOCTEST_REQUIRE(env.ready);
    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    TodoFrame frame(mod.get());

    // M2-2 盘点结论：全部弹窗走 ui_kit::drawDialogFrame → components::dialog
    // （EUI 已内建「常驻构建 + opacity 收起」0.16s OutCubic 过渡），toast 同型
    // ——DevDesk 侧不重复加。本用例是回归护栏：关闭态元素仍构建、opacity 0
    // 且 disabled（防误触）；若未来重引入懒守卫（if (!open) return），收起
    // 动画会随元素移除消失，此处立即变红。
    const auto* dlg = frame.ui.find("todo.dialog.newtask");
    DOCTEST_REQUIRE(dlg != nullptr);
    const auto* panel = frame.ui.find("todo.dialog.newtask.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
    DOCTEST_CHECK(panel->disabled);
    // 其余弹窗同款容器（抽查一个：详情弹窗也常驻构建）
    DOCTEST_CHECK(frame.ui.find("todo.dialog.detail") != nullptr);
    // toast 根节点关闭态 opacity 0（EUI toast 内建过渡）
    const auto* toast = frame.ui.find("todo.toast");
    DOCTEST_REQUIRE(toast != nullptr);
    DOCTEST_CHECK_EQ(toast->opacity, 0.0f);

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernTodoDropFadeGuard,
              "uiModern: todo 卡片落位淡入（拖拽跟手期间不动画）", {
    TodoEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("落位列");
    DOCTEST_REQUIRE(col.isOk());
    auto t = env.svc->createTask(col.value().id, "落位卡");
    DOCTEST_REQUIRE(t.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    TodoFrame frame(mod.get());

    // 迁移默认 4 列 + 新建 1 列 = col.4；卡片区容器常驻（M2-3 落位淡入载体）
    const auto* cards = frame.ui.find("todo.col.4.cards");
    DOCTEST_REQUIRE(cards != nullptr);
    DOCTEST_CHECK_EQ(cards->opacity, 1.0f);

    // 跟手期间（press → drag）：columnFadeIn_ 不被触碰 → 容器 opacity 恒 1，
    // transition 目标一致不动画
    {
        auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        eui::Rect rect;
        rect.x = 100.0f;
        rect.y = 200.0f;
        rect.width = 200.0f;
        rect.height = 96.0f;
        card->onPress(pe, rect);
        core::dsl::DragEvent dragEv;
        dragEv.x = 400.0;
        dragEv.y = 300.0;
        card->onDrag(dragEv);
        frame.refresh();
        cards = frame.ui.find("todo.col.4.cards");
        DOCTEST_REQUIRE(cards != nullptr);
        DOCTEST_CHECK_EQ(cards->opacity, 1.0f);
    }

    // 拖拽结束（cancel 路径同样走 cardReleased 跟手收口）→ 下一帧容器 0.6 淡回
    {
        auto* card = frame.ui.find("todo.col.4.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        eui::PointerEvent pe;
        pe.action = eui::PointerAction::Cancel;
        eui::Rect rect;
        card->onRelease(pe, rect);
        frame.refresh();
        cards = frame.ui.find("todo.col.4.cards");
        DOCTEST_REQUIRE(cards != nullptr);
        DOCTEST_CHECK(cards->opacity < 1.0f);
    }

    mod->onStop();
    return true;
})

// ---- A. M1 md 子项：树/编辑器右键统一换算 + 编辑器直挂（M1-3/M1-4）----
// M1-3：openFileContextMenu 换 menuAnchorForPointerInPage；x 扣页面区原点
// 的既有回归在 test_markdown.cpp（md_ctx_menu_page_origin），本文件锁
// 「无 Router → 原点 0」形态 + 4523e09 隐形菜单回归。M1-4：input 链直挂
// onContextMenu（删逐帧 find hack）由 .hit 回调非空 + 锚点断言锁。

REGISTER_TEST(uiModernMdTreeNoGhost,
              "uiModern md: 空路径右键早退不留隐形菜单态（4523e09 回归锁）", {
    // 空笔记目录：树无文件行 → ctxMenuPath_ 恒空；drawNoteContextMenu
    // 每帧强制置关（4523e09 语义：open 标志与「存在右键目标」同生共灭，
    // 直接赋值、不回 onOpenChange 回调）。连续两帧断言菜单节点绝不构建
    // （无幽灵菜单、标志不被拨起）
    test_support::MdHeadlessEnv env("devdesk_test_ui_m1_", "mdghost", "m1.db");
    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    for (int f = 0; f < 2; ++f) {
        eui::Ui ui;
        eui::Screen screen{1600.0f, 1000.0f};
        ui.begin("md_m1_ghost_" + std::to_string(f));
        mod->compose(ui, screen);
        ui.end();
        DOCTEST_CHECK(ui.find("md.ctx") == nullptr);
    }
    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernMdCtxAnchor,
              "uiModern md: 树行/编辑器右键锚点 = 指针坐标统一换算（M1-3/M1-4）", {
    test_support::MdHeadlessEnv env("devdesk_test_ui_m1_", "mdanchor", "m1.db");
    {
        std::ofstream ofs(env.dir / "锚.md", std::ios::binary);
        ofs << "# 标题\n正文\n";
    }
    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    auto compose = [&]() {
        ui.begin("md_m1_anchor");
        mod->compose(ui, screen);
        ui.end();
    };

    // 树行右键 → 菜单锚点 = 指针原值（无 Router → 页面原点 0、dpi=1；
    // x/y 同一条换算通道——y 不再裸用）
    {
        compose();
        const auto* fileRow = ui.find("md.tree.node.1.row");
        DOCTEST_REQUIRE(fileRow != nullptr);
        DOCTEST_REQUIRE(fileRow->onContextMenu != nullptr);
        eui::PointerEvent pe;
        pe.x = 300;
        pe.y = 200;
        fileRow->onContextMenu(pe, eui::Rect{});
        compose();
        const auto* menu = ui.find("md.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK(std::fabs(menu->x - 300.0f) < 0.01f);
        DOCTEST_CHECK(std::fabs(menu->y - 200.0f) < 0.01f);
    }
    // 编辑器右键：打开笔记进入编辑态 → .hit 节点直挂 onContextMenu（M1-4
    // 删逐帧 hack 后回调仍在），锚点同口径
    {
        compose();
        const auto* fileRow = ui.find("md.tree.node.1.row");
        DOCTEST_REQUIRE(fileRow != nullptr);
        DOCTEST_REQUIRE(fileRow->onClick != nullptr);
        fileRow->onClick();
        compose();
        const auto* hit = ui.find(kEditorInputHitId);
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onContextMenu != nullptr);
        eui::PointerEvent pe;
        pe.x = 400;
        pe.y = 300;
        hit->onContextMenu(pe, eui::Rect{});
        compose();
        const auto* edMenu = ui.find("md.editor.ctx.level.0");
        DOCTEST_REQUIRE(edMenu != nullptr);
        DOCTEST_CHECK(std::fabs(edMenu->x - 400.0f) < 0.01f);
        DOCTEST_CHECK(std::fabs(edMenu->y - 300.0f) < 0.01f);
    }

    mod->onStop();
    return true;
})

// ---- B. M3 剩余：virtualList / todo card / barChart / 渐变 / recent card ----

REGISTER_TEST(uiModernVirtualProfileRows,
              "uiModern claude: profile 虚拟列表 slot 窗口 + 滚动回退语义", {
    ClaudeEnv env;
    for (int i = 0; i < 25; ++i) {
        auto p = env.svc->createProfile(
            "V" + std::to_string(i),
            "https://v" + std::to_string(i) + ".example.com",
            "sk-ant-v" + std::to_string(i), "m");
        DOCTEST_REQUIRE(p.isOk());
    }
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    auto compose = [&]() {
        ui.begin("claude_vlist_frame");
        mod->compose(ui, screen);
        ui.end();
    };

    // 列表根 = virtualList 节点（.slot. 行重建语义）；行内容 id 沿用绝对
    // 下标（旧断言形态）：profile.0 可见
    compose();
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.0.name") != nullptr);
    // slot 窗口生效：25 行不整段渲染，视口外行不存在（virtualList 裁剪）
    DOCTEST_CHECK(ui.find("claude.profile.15.name") == nullptr);

    // 滚动回退语义：onScrollOffsetChanged 写 profilesScrollOffset_ →
    // 重 compose 视口随偏移前移（原视口外的行进入渲染范围）
    const auto* root = ui.find("claude.profiles.scroll");
    DOCTEST_REQUIRE(root != nullptr);
    DOCTEST_REQUIRE(root->onScrollOffsetChanged != nullptr);
    root->onScrollOffsetChanged(600.0f);
    compose();
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.17.name") != nullptr);
    // 行命中节点 .bg 保留（选中/右键挂点行内，点击语义未动）
    DOCTEST_CHECK(ui.find("claude.profile.0.bg") != nullptr);

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernVirtualLogRows,
              "uiModern claude: 调用记录 virtualList 行重建（slot 结构保留）", {
    ClaudeEnv env;
    // 种子：1 个 profile + 25 条调用（FK profile_id 必须真实存在；当页 20 行）
    auto prof = env.svc->createProfile("日志源", "https://api.anthropic.com",
                                       "sk-ant-log-1", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(prof.isOk());
    for (int i = 0; i < 25; ++i) {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = prof.value().id;
        e.method = "POST";
        e.path = "/v1/messages";
        e.statusCode = 200;
        e.latencyMs = 12;
        e.startedAt = std::time(nullptr) - i * 60;
        DOCTEST_REQUIRE(env.svc->logCall(e).isOk());
    }
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    auto compose = [&]() {
        ui.begin("claude_vlogs_frame");
        mod->compose(ui, screen);
        ui.end();
    };
    // 打开弹窗（CPX-06：后台取数；有界等待当页 20 行到位）
    {
        compose();
        const auto* hit = ui.find("claude.calls.viewall.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onClick != nullptr);
        hit->onClick();
    }
    bool rowsReady = false;
    for (int i = 0; i < 400 && !rowsReady; ++i) {
        compose();
        rowsReady = ui.find("claude.dialog.calls.row.19.wrap") != nullptr;
        if (!rowsReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(rowsReady);
    // 列表根 = virtualList 节点（slot 重建语义）；绝对行号 id 沿用
    DOCTEST_CHECK(ui.find("claude.dialog.calls.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.dialog.calls.row.0.wrap") != nullptr);
    DOCTEST_CHECK(ui.find("claude.dialog.calls.row.19.wrap") != nullptr);
    DOCTEST_CHECK(ui.find("claude.dialog.calls.row.20.wrap") == nullptr);
    // 文本节点 id 沿用（textOf 形态）；当页 20 行整段渲染（overscan）

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernTodoCardContainer,
              "uiModern todo: 卡片容器换 card（骨架节点 + 事件挂点保留）", {
    TodoEnv env;
    DOCTEST_REQUIRE(env.ready);
    auto col = env.svc->createColumn("容器列");
    DOCTEST_REQUIRE(col.isOk());
    auto t = env.svc->createTask(col.value().id, "容器卡");
    DOCTEST_REQUIRE(t.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    TodoFrame frame(mod.get());

    // card 骨架节点（panelShadow 阴影载体 = card 内部 .bg）
    DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.card") != nullptr);
    DOCTEST_CHECK(frame.ui.find("todo.col.4.card.0.card.bg") != nullptr);
    // 交互层保留（原 .bg：命中语义与旧卡根相同）
    const auto* bg = frame.ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // 事件挂点全部随行（拖拽/右键/点击业务通道未动）
    DOCTEST_CHECK(bg->onClick != nullptr);
    DOCTEST_CHECK(bg->onContextMenu != nullptr);
    DOCTEST_CHECK(bg->onPress != nullptr);
    DOCTEST_CHECK(bg->onDrag != nullptr);
    DOCTEST_CHECK(bg->onRelease != nullptr);

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernHomeApiBarChart,
              "uiModern: Home API 调用量 barChart（24 点非空 + 接线 + 服务层分布）", {
    test_support::TestEnv env("devdesk_test_uimodern_home_", "apichart");
    DOCTEST_REQUIRE(env.ready);
    // 种子：1 个 profile（FK 依赖）+ 当天 3 条（9/10/11 时，避开子夜边界）
    // + 昨天 2 条（排除）
    const int64_t dayStart = module::home::startOfDayEpoch(std::time(nullptr));
    DOCTEST_CHECK(env.db->db().exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
        " api_key_check, keychain_account, model, is_active, created_at, "
        " updated_at) VALUES ('测试', 'https://api.anthropic.com', "
        " 'sk-', 'CHECK', 'devdesk-test-uimodern', 'claude-sonnet-4-5', 1, "
        " 1000, 1000)").isOk());
    const std::string sql =
        "INSERT INTO claude_call_logs (profile_id, method, path, status_code, "
        " latency_ms, request_bytes, response_bytes, error, started_at) VALUES "
        "(1, 'POST', '/v1/messages', 200, 12, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 9) + "), "
        "(1, 'POST', '/v1/messages', 200, 12, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 10) + "), "
        "(1, 'POST', '/v1/messages', 200, 12, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 11) + "), "
        "(1, 'GET', '/v1/models', 200, 5, 20, 40, '', " +
        std::to_string(dayStart - 3600) + "), "
        "(1, 'GET', '/v1/models', 200, 5, 20, 40, '', " +
        std::to_string(dayStart - 7200) + ")";
    DOCTEST_CHECK(env.db->db().exec(sql).isOk());

    // 服务层：24 点恒非空、当日恰 3 条、逐时落点正确
    {
        module::home::HomeService svc(env.db->db());
        auto series = svc.getTodayApiCallSeries(24);
        DOCTEST_REQUIRE(series.isOk());
        DOCTEST_CHECK_EQ(series.value().size(), 24u);
        int64_t total = 0;
        for (const int64_t v : series.value()) total += v;
        DOCTEST_CHECK_EQ(total, 3);
        DOCTEST_CHECK_EQ(series.value()[9], 1);
        DOCTEST_CHECK_EQ(series.value()[11], 1);
    }

    // UI 接线：API 卡 + barChart 根 + 24 根柱子（demo 数据只有 4 根：
    // bar.4 存在即证明喂的是真实 24 点序列，无演示泄漏）
    {
        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Screen screen{1200.0f, 720.0f};
        eui::Ui ui;
        ui.begin("home_api_chart");
        mod->compose(ui, screen);
        ui.end();
        DOCTEST_CHECK(ui.find("home.stat.api") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars.bar.4") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars.bar.23") != nullptr);
        mod->onStop();
    }
    return true;
})

REGISTER_TEST(uiModernGradientSpots,
              "uiModern: 两处关键背景渐变（settings 主体 / md 预览）", {
    // 1. 设置页主体区垫底
    {
        const std::filesystem::path cfgPath = test_temp::tempPath(
            "devdesk_test_uimodern_settings_grad_cfg.json");
        const std::filesystem::path dbPath = test_temp::tempPath(
            "devdesk_test_uimodern_settings_grad.db");
        std::error_code ec;
        std::filesystem::remove(cfgPath, ec);
        std::filesystem::remove(dbPath, ec);
        auto r = ::core::Database::open(dbPath);
        DOCTEST_REQUIRE(r.isOk());
        auto db = std::make_unique<::core::Database>(std::move(r.value()));
        DOCTEST_REQUIRE(db->migrate().isOk());
        ::core::registerAppDatabase(&db->db());
        test_support::setEnvVar("DEVDESK_CONFIG_PATH", cfgPath.string());
        {
            std::unique_ptr<::core::Module> mod(create_settings_module());
            mod->onStart();
            eui::Ui ui;
            eui::Screen screen{1600.0f, 1000.0f};
            ui.begin("grad_settings");
            mod->compose(ui, screen);
            ui.end();
            const auto* bg = ui.find("settings.body.bg");
            DOCTEST_CHECK(bg != nullptr);
            if (bg != nullptr) DOCTEST_CHECK(bg->gradient.enabled);
            mod->onStop();
        }
        test_support::setEnvVar("DEVDESK_CONFIG_PATH", "");
        ::core::registerAppDatabase(nullptr);
        std::filesystem::remove(cfgPath, ec);
        std::filesystem::remove(dbPath, ec);
    }
    // 2. md 预览区垫底（Ctrl+Shift+P ×2 → 双栏，同 theme_hierarchy）
    {
        test_support::MdHeadlessEnv env("devdesk_test_ui_m3_", "mdgrad", "m3.db");
        {
            std::ofstream ofs(env.dir / "g.md", std::ios::binary);
            ofs << "# 渐变\n";
        }
        std::unique_ptr<::core::Module> mod(create_markdown_module());
        mod->onStart();
        eui::KeyEvent p;
        p.key = eui::InputKey::P;
        p.action = eui::KeyAction::Press;
        p.modifiers.control = true;
        p.modifiers.shift = true;
        DOCTEST_CHECK(mod->onKeyEvent(p));  // → 实时
        DOCTEST_CHECK(mod->onKeyEvent(p));  // → 双栏
        eui::Ui ui;
        eui::Screen screen{1600.0f, 1000.0f};
        ui.begin("grad_md");
        mod->compose(ui, screen);
        ui.end();
        const auto* bg = ui.find("md.preview.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(bg->gradient.enabled);
        mod->onStop();
    }
    return true;
})

