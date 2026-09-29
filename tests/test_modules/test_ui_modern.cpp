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
#include "core/window_layout.h"  // 最小窗尺寸经同一函数推导（about 页最小窗复核）
#include "module_about/about_anim.h"  // uiAboutRingAnim：浮环状态机直接驱动
#include "module_claude_proxy/claude_service.h"
#include "module_home/home_service.h"
#include "module_markdown/md_ids.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/metrics.h"  // kHeaderH（板几何断言）
#include <cstdlib>
#include <chrono>
#include <cmath>
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
extern "C" ::core::Module* create_about_module();

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
        // 2026-09-28 根因修：空闲态不再挂 onTimer（消整页 2Hz 重绘 +
        // 常驻 animating_ 反复打断 hover 过渡；仅流式活动期挂 1s）
        DOCTEST_CHECK(ui.find("home.stat.tokens.timer") == nullptr);

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
})REGISTER_TEST(uiModernMdModeSwitchNoFade,
              "uiModern: md 四态切换硬切（1cdd36f 已删整页 fade，root 常驻 opacity 1）", {
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

        // 稳态：root opacity = 1（默认值）。1cdd36f 已删 md.root 整页 fade
        // 过渡（bug e42ac9e：整页溶解 0.15s 诡异闪烁），transition 必须保持关闭
        const auto* root = ui.find("md.root");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_CHECK_EQ(root->opacity, 1.0f);
        DOCTEST_CHECK(!root->transition.enabled);
    }

    // Ctrl+Shift+P 切态 → setMode（与顶栏 segmented onChange 同一收口）；
    // 过渡已删，切换即硬切
    eui::KeyEvent key = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    DOCTEST_CHECK(mod->onKeyEvent(key));

    {
        eui::Ui ui;
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        ui.begin("md_m2_b");
        mod->compose(ui, screen);
        ui.end();

        // 切换后首帧：root opacity 保持 1.0（过渡已删，无淡出）
        const auto* root = ui.find("md.root");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_CHECK_EQ(root->opacity, 1.0f);
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

    // 跟手期间（press → drag）：容器 opacity 由 dragActive 门控 → 0.6 调暗
    //（审计修复前靠 columnFadeIn_ 标志恒 1，松手时 1.0→0.6→1.0 闪暗）
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
        DOCTEST_CHECK(cards->opacity < 1.0f);  // 跟手中整列 0.6 调暗
    }

    // 拖拽结束（cancel 路径同样走 cardReleased 跟手收口）→ dragActive 归 false，
    // 容器目标回 1.0：0.6→1.0 平滑淡回（不再先暗到 0.6 再淡回）
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
        DOCTEST_CHECK_EQ(cards->opacity, 1.0f);
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
    // 打开弹窗（CPX-06：后台取数；有界等待当页 20 行到位）。
    // 2026-09-28：调用记录已抽成详情面板第 4 页签——先切 tab（该帧生效），
    // 再点「查看全部 →」开弹窗
    {
        compose();
        const auto* tab = ui.find("claude.detail.tabs.hit.3");
        DOCTEST_REQUIRE(tab != nullptr);
        DOCTEST_REQUIRE(tab->onClick != nullptr);
        tab->onClick();
    }
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
    test_support::TestEnv env("devdesk_test_uimodern_home_", "apichart",
                              true);  // 注册 appDatabase（2026-09-28 起全零走
                                      // 空态不画假条，需真实数据才能断言柱子）
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
        mod->onStart();  // 挂真实服务（否则 stats 未加载 → 24 点全零 → 空态）
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

// ---- 设置页主体底板：已删（2026-09-28 用户真机反馈「设置页面最外层的卡片
// 还没删除啊，让你删除好多次了」）。M3-4（2026-09-28）曾在设置页主体最下层
// 垫 surface → surfaceElevated 垂直渐变底（settings.body.gradient >
// settings.body.bg），即用户眼中的「最外层卡片」；本次删除后设置页回到 frost
// spec 口径（docs/superpowers/specs/2026-09-22-eui-window-frost-design.md:53
// 「各模块页面要么靠 clearColor 当底、要么自铺不透明 surface 色」）——三档
// 窗口效果下底色分别由 clearColor（关）/ 磨砂背景（半透、磨砂）承担。
// 断言方向**取反**（存在即红）：底板被加回时本用例立刻红；「设置页仍在渲染」
// 由第三条断言反证，避免整页 compose 早退时 find 恒 nullptr 造成的**假绿**。
REGISTER_TEST(uiModernSettingsBodyGradientRemoved,
              "uiModern: 设置页无主体底板（settings.body.bg 必须不存在）", {
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
        DOCTEST_CHECK(ui.find("settings.body.bg") == nullptr);
        DOCTEST_CHECK(ui.find("settings.body.gradient") == nullptr);
        // 反证（防假绿）：默认 tab 的卡仍在 → 页面确实 compose 过
        DOCTEST_CHECK(ui.find("settings.card.appearance") != nullptr);
        mod->onStop();
    }
    test_support::setEnvVar("DEVDESK_CONFIG_PATH", "");
    ::core::registerAppDatabase(nullptr);
    std::filesystem::remove(cfgPath, ec);
    std::filesystem::remove(dbPath, ec);
    return true;
})

REGISTER_TEST(uiModernMdPreviewGradient,
              "uiModern: md 预览区垫底渐变（Ctrl+Shift+P 双栏）", {
    // md 预览区垫底（Ctrl+Shift+P ×2 → 双栏，同 theme_hierarchy）
    {
        test_support::MdHeadlessEnv env("devdesk_test_ui_m3_", "mdgrad", "m3.db");
        {
            std::ofstream ofs(env.dir / "g.md", std::ios::binary);
            ofs << "# 渐变\n";
        }
        std::unique_ptr<::core::Module> mod(create_markdown_module());
        mod->onStart();
        eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
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

// ---- about 页布局（2026-09-28 用户真机反馈：内容区加宽 / 顶部留白 / 删 hero
//      重复三行 / 卡片上下堆叠 / footer 贴页面底部）----
// 只钉**可客观断言**的部分（内容区宽、hero 起点、卡片宽高与堆叠链、footer 底边、
// 最小窗与大窗都不溢出/不重叠）；观感（呼吸感 / hero 存在感 / 三档窗口效果下的
// 底色）留人工确认。页面宽 = 窗口宽 − 侧边栏（app_sidebar.cpp pageContentWidth，
// 展开 208）：最小窗宽 1330 → 页面宽 1122，内容区 900 只余 111px/侧（仍居中、
// 不溢出，见下）。**最小窗尺寸不再手写**（2026-09-28 组 A：手写的 845 已失义——
// macOS 工作区 833 点会把最小窗高钳到 805），改由
// core::window_layout::minWindowSizeForWorkArea 推导；页面宽只依赖最小窗宽，
// 本文件不硬编码高。
namespace {
// 与 main.cpp 同一条决策链的最小窗尺寸（macOS 实测档：工作区 1470×833 点、
// windowScaleForPoint 恒 1.0、外框 {0,28}）→ {1330, 805}。显式传参而非取本机
// 实时工作区：测试要确定性，且这一档正是「用户机器上最小窗能缩到多大」的真实值。
core::window_layout::WindowSize minWindowForMacWorkArea() {
    return core::window_layout::minWindowSizeForWorkArea(
        core::window_layout::InitialWindowRequest{1600, 1000, 1330, 845},
        core::window_layout::WorkArea{0, 0, 1470, 833}, 1.0f,
        core::window_layout::FrameInsets{0, 28});
}

// 布局帧比较容差：逻辑像素差 ≤0.5 视为相等（布局引擎可能带亚像素误差，与
// probes 的 0.5f 容差同口径）
bool nearPx(float a, float b) { return std::abs(a - b) <= 0.5f; }
}  // namespace

REGISTER_TEST(uiAboutLayout,
              "about: 内容区 900 / 内容块贴底（底边=页脚顶−间距）/ 极矮窗 clamp 回 topPad / footer 贴页面底部（真机反馈）", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    const auto& t = ::core::theme::current();
    constexpr float kContentW = 900.0f;   // 2026-09-29 1040→900（两卡缩一档）
    constexpr float kCardGap = 8.0f;
    constexpr float kTitleH = 40.0f;
    constexpr float kCardH = 24.0f + 28.0f + 6.0f * 28.0f + 24.0f;  // 244（6 行卡）
    const float topPad = t.sizes.spacingXXLarge;                   // 40（仅极矮窗兜底）
    const float iconSize = t.sizes.spacingXXLarge + t.sizes.spacingLarge;  // 56
    const float heroToCards = t.sizes.spacingXLarge;                       // 24
    const float footerH = 20.0f;
    // 内容块总高（hero 图标 + 图标/标题 8 + 标题 40 + 标题到卡一档 XLarge
    // + 卡1 + 卡距 + 卡2 = 624）与「内容块底边 = 页脚顶 − 一档间距」的贴底口径
    // （2026-09-29）：起点 = max(topPad, 页脚顶 − heroToCards − contentH)
    const float contentH = iconSize + 8.0f + kTitleH + heroToCards +
                           kCardH + kCardGap + kCardH;                    // 624
    eui::Ui ui;
    eui::Screen screen{1600.0f, 1000.0f};
    const float footerTop = screen.height - t.sizes.spacingXXLarge - footerH;  // 940
    const float contentTop = std::max(topPad, footerTop - heroToCards - contentH);  // 292
    const float cardsTop = contentTop + iconSize + 8.0f + kTitleH + heroToCards;    // 420
    const float depsTop = cardsTop + kCardH + kCardGap;                             // 672
    ui.begin("about_layout");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);  // 元素 frame 需显式布局（同 test_settings）
    const float x = (screen.width - kContentW) / 2.0f;  // 350
    // 帧诊断串：几何断言失败时直接看到实测值（fail-fast 只报一条，无第二条线索）
    auto px = [](const char* what, const core::dsl::Element* e) {
        return std::string(what) + " 实测 (" +
               std::to_string(e->frame.x) + ", " +
               std::to_string(e->frame.y) + ", " +
               std::to_string(e->frame.width) + "x" +
               std::to_string(e->frame.height) + ")";
    };
    {
        const auto* hero = ui.find("about.hero.icon");
        DOCTEST_REQUIRE(hero != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(hero->frame.y, contentTop),
                              px("hero.frame.y", hero));  // 内容块起点（贴底对齐）
        DOCTEST_CHECK_MESSAGE(nearPx(hero->frame.width, iconSize),
                              px("hero.frame.width", hero));  // hero 图标尺寸
        // 居中于内容区
        DOCTEST_CHECK_MESSAGE(
            nearPx(hero->frame.x, x + (kContentW - iconSize) / 2.0f),
            px("hero.frame.x", hero));
    }
    // hero 副标题三行已删（版本行 / 构建时刻 / commit+编译器 与「应用信息」卡
    // 逐项重复，用户真机反馈"卡片里已经有版本号之类的东西"）：id 不得再存在
    DOCTEST_CHECK(ui.find("about.hero.sub") == nullptr);
    DOCTEST_CHECK(ui.find("about.hero.build") == nullptr);
    DOCTEST_CHECK(ui.find("about.hero.commit") == nullptr);
    {
        const auto* title = ui.find("about.hero.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.y, contentTop + iconSize + 8.0f),
                              px("hero.title", title));
    }
    const auto* c1 = ui.find("about.card.app");
    const auto* c2 = ui.find("about.card.deps");
    DOCTEST_REQUIRE(c1 != nullptr);
    DOCTEST_REQUIRE(c2 != nullptr);
    // 卡片上下堆叠（原左右并排 516 宽）：同宽（占满内容区 900）、同 x、各 244 高；
    // 卡 1 顶 = 内容块起点 + hero 图标 + 8 + 标题 40 + 一档 XLarge（420），
    // 卡 2 隔一档 cardGap 在卡 1 正下方
    DOCTEST_CHECK_MESSAGE(nearPx(c1->frame.width, kContentW),
                          px("about.card.app", c1));  // 900
    DOCTEST_CHECK_MESSAGE(nearPx(c2->frame.width, kContentW),
                          px("about.card.deps", c2));
    DOCTEST_CHECK_MESSAGE(nearPx(c1->frame.height, kCardH), px("about.card.app", c1));
    DOCTEST_CHECK_MESSAGE(nearPx(c1->frame.x, x), px("about.card.app.x", c1));
    DOCTEST_CHECK_MESSAGE(nearPx(c2->frame.x, x), px("about.card.deps.x", c2));
    DOCTEST_CHECK_MESSAGE(nearPx(c1->frame.y, cardsTop), px("about.card.app", c1));
    DOCTEST_CHECK_MESSAGE(nearPx(c2->frame.y, depsTop), px("about.card.deps", c2));
    // 贴底对齐核心断言（2026-09-29）：内容块底边 = 页脚顶 − 一档间距
    //（hero + 两卡整体贴向页脚上方，而非顶部对齐；回退成 topPad 起点即红）
    DOCTEST_CHECK_MESSAGE(
        nearPx(c2->frame.y + c2->frame.height, footerTop - heroToCards),
        px("about.card.deps 内容块底", c2) + " 期望 " +
            std::to_string(footerTop - heroToCards));
    {
        const auto* footer = ui.find("about.footer");
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(footer->frame.x, x),
                              px("about.footer.x", footer));
        // footer 在最下卡（卡 2）下方（中间是留白区，不再是流式紧跟卡底）
        DOCTEST_CHECK_MESSAGE(
            footer->frame.y > c2->frame.y + c2->frame.height,
            px("about.footer", footer) + " 卡底 " +
                std::to_string(c2->frame.y + c2->frame.height));
        // 贴页面底部：底边 = screen.height − 页面底 padding（spacingXXLarge）
        DOCTEST_CHECK_MESSAGE(
            nearPx(footer->frame.y + footer->frame.height,
                   screen.height - t.sizes.spacingXXLarge),
            px("about.footer", footer) + " 期望底边 " +
                std::to_string(screen.height - t.sizes.spacingXXLarge));
    }
    // 最小窗复核（2026-09-28 组 A：字面量 845 已失义，改经同一函数推导）：
    // minWindowSizeForWorkArea → {1330, 805} → 页面宽 1330 − 208 = 1122 →
    // 内容区 900 不溢出、仍居中（左右各余 111）；页面高 805 下贴底口径仍成立
    //（页脚顶 745 → 内容块底 721、起点 97 ≥ topPad 40，不触发 clamp）——本页
    // 无滚动也放得下
    {
        const core::window_layout::WindowSize minWin = minWindowForMacWorkArea();
        DOCTEST_CHECK_EQ(minWin.width, 1330);
        DOCTEST_CHECK_EQ(minWin.height, 805);  // 工作区 833 − 标题栏 28
        eui::Ui mini;
        eui::Screen minScreen{minWin.width - 208.0f,
                              static_cast<float>(minWin.height)};
        mini.begin("about_layout_min");
        mod->compose(mini, minScreen);
        mini.end();
        mini.layout(minScreen.width, minScreen.height);
        const auto* c = mini.find("about.card.deps");
        DOCTEST_REQUIRE(c != nullptr);
        DOCTEST_CHECK(c->frame.x >= 0.0f);
        DOCTEST_CHECK(c->frame.x + c->frame.width <= minScreen.width);
        // 居中：右卡右缘 = 内容区右缘
        DOCTEST_CHECK(nearPx(c->frame.x + c->frame.width,
                             (minScreen.width - kContentW) / 2.0f + kContentW));
        const auto* footer = mini.find("about.footer");
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK(nearPx(footer->frame.y + footer->frame.height,
                             minScreen.height - t.sizes.spacingXXLarge));
        // 最小窗（页面高 805）下贴底口径同样成立：内容块底边 = 页脚顶 − 一档间距
        DOCTEST_CHECK_MESSAGE(
            nearPx(c->frame.y + c->frame.height,
                   (minScreen.height - t.sizes.spacingXXLarge - footerH) - heroToCards),
            px("about.card.deps.min", c) + " 期望底 " +
                std::to_string(minScreen.height - t.sizes.spacingXXLarge - footerH -
                               heroToCards));
        // 卡底 + 卡距 ≤ 页脚顶：最小窗高下卡片与页脚不重叠（页脚已贴底）
        DOCTEST_CHECK_MESSAGE(
            nearPx(footer->frame.y, minScreen.height - t.sizes.spacingXXLarge - 20.0f),
            px("about.footer.min", footer));
        DOCTEST_CHECK_MESSAGE(
            c->frame.y + c->frame.height + kCardGap <= footer->frame.y,
            px("about.card.deps.min", c) + " 页脚顶 " +
                std::to_string(footer->frame.y));
    }
    // 极矮窗复核（2026-09-29 贴底护栏）：内容块 + 一档间距放不进「页脚上方」
    // 这块高度时，起点 clamp 回 topPad（退回顶部对齐），且卡与页脚不得重叠
    //（本页无滚动 → 页脚侧的 max 护栏把页脚压到卡下方）
    {
        eui::Ui tiny;
        eui::Screen tinyScreen{1200.0f, 640.0f};  // 页脚顶 580；580−24−624 < 0
        tiny.begin("about_layout_tiny");
        mod->compose(tiny, tinyScreen);
        tiny.end();
        tiny.layout(tinyScreen.width, tinyScreen.height);
        const auto* hero = tiny.find("about.hero.icon");
        const auto* c = tiny.find("about.card.deps");
        const auto* footer = tiny.find("about.footer");
        DOCTEST_REQUIRE(hero != nullptr);
        DOCTEST_REQUIRE(c != nullptr);
        DOCTEST_REQUIRE(footer != nullptr);
        // 无护栏时起点会是负数（贴底反推值恒 < topPad）→ 断言 = topPad 即证 clamp
        DOCTEST_CHECK_MESSAGE(nearPx(hero->frame.y, topPad),
                              px("hero.frame.y.tiny", hero));
        DOCTEST_CHECK_MESSAGE(
            c->frame.y + c->frame.height + kCardGap <= footer->frame.y,
            px("about.card.deps.tiny", c) + " 页脚顶 " +
                std::to_string(footer->frame.y));
    }
    // 大窗复核（默认窗以上的高页面）：内容块整体贴底（不是顶部对齐、也不随
    // 高度拉伸），页脚仍贴底、仍不重叠
    {
        eui::Ui big;
        eui::Screen bigScreen{2560.0f, 1520.0f};
        const float bigFooterTop =
            bigScreen.height - t.sizes.spacingXXLarge - footerH;      // 1460
        const float bigContentTop =
            std::max(topPad, bigFooterTop - heroToCards - contentH);  // 812
        const float bigDepsTop =
            bigContentTop + iconSize + 8.0f + kTitleH + heroToCards + kCardH + kCardGap;
        big.begin("about_layout_big");
        mod->compose(big, bigScreen);
        big.end();
        big.layout(bigScreen.width, bigScreen.height);
        const auto* c = big.find("about.card.deps");
        const auto* footer = big.find("about.footer");
        DOCTEST_REQUIRE(c != nullptr);
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(c->frame.y, bigDepsTop), px("about.card.deps.big", c));
        DOCTEST_CHECK_MESSAGE(nearPx(c->frame.x, (bigScreen.width - kContentW) / 2.0f),
                              px("about.card.deps.big", c));
        DOCTEST_CHECK_MESSAGE(
            nearPx(c->frame.y + c->frame.height, bigFooterTop - heroToCards),
            px("about.card.deps.big", c) + " 期望底 " +
                std::to_string(bigFooterTop - heroToCards));
        DOCTEST_CHECK_MESSAGE(
            nearPx(footer->frame.y + footer->frame.height,
                   bigScreen.height - t.sizes.spacingXXLarge),
            px("about.footer.big", footer));
        DOCTEST_CHECK(c->frame.y + c->frame.height + kCardGap <= footer->frame.y);
    }
    mod->onStop();
    return true;
})

// ---- about 页浮环动态背景（2026-09-28 用户真机追问「关于页面的动态效果，
//      你给我做了吗」——做了但太淡、看不出）----
// 钉三件事：① 可见度区间（opacity 0.18-0.28 / 半径 12-34，常量即真相，
// 断言防再被悄悄调淡）；② 真的在动（awake 时 bg 元素挂 onFrame，帧推进 →
// 锚点漂移 + 弹簧跟随 → 环元素几何变化）；③ 静止后休眠（全 settled + 2s
// 超时 → awake()==false → compose 不再挂 onFrame，省电设计）；顺带锁唤醒路径。
REGISTER_TEST(uiAboutRingAnim,
              "about: 浮环可见度 0.18-0.28 / 漂移真在动 / 静止后休眠（真机反馈）", {
    using Anim = module::about::AboutAnimState;
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    eui::Ui ui;
    eui::Screen screen{1392.0f, 1000.0f};  // 默认窗 1600 − 侧边栏 208
    ui.begin("about_ring");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    auto& anim = ui.state<Anim>("about.anim");
    DOCTEST_REQUIRE(anim.initialized);
    DOCTEST_CHECK_EQ(Anim::kRingCount, 7);
    // ① 可见度区间（2026-09-28 从 0.06-0.15 上调；浅底 surface #F5F2EC 上
    //    0.06-0.15 合成后 ΔRGB ≈ 2-4、等同不可见）。**用字面量而非被测量
    //    头文件常量**：拿 kOpacityMin/Max 断言等于自己，把常量调回去时断言
    //    仍真（实测变异验证过）——回归就无人守了
    constexpr float kExpectOpacityMin = 0.18f;
    constexpr float kExpectOpacityMax = 0.28f;
    constexpr float kExpectRadiusMin = 12.0f;
    constexpr float kExpectRadiusMax = 34.0f;
    bool opacityOk = true;
    bool radiusOk = true;
    for (int i = 0; i < Anim::kRingCount; ++i) {
        const auto& ring = anim.rings[i];
        opacityOk = opacityOk && ring.opacity >= kExpectOpacityMin - 1e-6f &&
                    ring.opacity <= kExpectOpacityMax + 1e-6f;
        radiusOk = radiusOk && ring.radius >= kExpectRadiusMin - 1e-6f &&
                   ring.radius <= kExpectRadiusMax + 1e-6f;
    }
    DOCTEST_CHECK(opacityOk);
    DOCTEST_CHECK(radiusOk);
    // ② 驱动机制与真在动：awake 时 bg 挂 onFrame（可手动驱动），帧推进后
    // 重 compose → 环元素几何变化（锚点漂移 + 弹簧跟随的端到端证据）
    const auto* bg = ui.find("about.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_REQUIRE(bg->onFrame != nullptr);
    const auto* ring0 = ui.find("about.ring.0");
    DOCTEST_REQUIRE(ring0 != nullptr);
    const float beforeX = ring0->frame.x;
    const float beforeY = ring0->frame.y;
    for (int f = 0; f < 30; ++f) {  // 0.5s 唤醒期帧（< kWakeHoldSeconds）
        bg->onFrame(1.0f / 60.0f);
    }
    ui.begin("about_ring");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    const auto* ring0b = ui.find("about.ring.0");
    DOCTEST_REQUIRE(ring0b != nullptr);
    const float moved = std::abs(ring0b->frame.x - beforeX) +
                        std::abs(ring0b->frame.y - beforeY);
    DOCTEST_CHECK_MESSAGE(moved > 1.0f, "ring.0 位移 " + std::to_string(moved));
    // ③ 静止 → 全 settled + 超时 → 休眠；compose 不再挂 onFrame
    const auto* bg2 = ui.find("about.bg");
    DOCTEST_REQUIRE(bg2 != nullptr);
    DOCTEST_REQUIRE(bg2->onFrame != nullptr);  // 0.5s < 2s，仍在唤醒期
    for (int f = 0; f < 240; ++f) {  // 再推 4s（总 4.5s > 2s 唤醒窗）
        bg2->onFrame(1.0f / 60.0f);
    }
    DOCTEST_CHECK(!anim.awake());
    ui.begin("about_ring");
    mod->compose(ui, screen);
    ui.end();
    const auto* bg3 = ui.find("about.bg");
    DOCTEST_REQUIRE(bg3 != nullptr);
    DOCTEST_CHECK(bg3->onFrame == nullptr);  // 休眠：不挂帧回调，零开销
    // 唤醒路径：onMove（compose 时挂在 bg 上）复位 wakeHold → 再挂 onFrame
    DOCTEST_REQUIRE(bg3->onMove != nullptr);
    (void)bg3->onMove(eui::PointerEvent{}, eui::Rect{});
    DOCTEST_CHECK(anim.awake());
    ui.begin("about_ring");
    mod->compose(ui, screen);
    ui.end();
    const auto* bg4 = ui.find("about.bg");
    DOCTEST_REQUIRE(bg4 != nullptr);
    DOCTEST_CHECK(bg4->onFrame != nullptr);
    mod->onStop();
    return true;
})

