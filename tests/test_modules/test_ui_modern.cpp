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
// 证据链 = 代码路径 + 本无头 compose 测试；真实鼠标点验留给用户人工确认
#include "doctest.h"
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
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/metrics.h"  // kHeaderH（板几何断言）
#include <cstdlib>
#include <filesystem>
#include <map>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();

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
})