// tests/test_modules/test_ui_modern.cpp
// UI 现代化无头断言：Home 统计卡 / sparkline 接线、速率采样环护栏、统一锚点换算、
// 右键菜单、virtualList / card 布局不变式、barChart 数据接线、md 树与编辑器右键、about 页。
// 跨文件前提：
//   - 无头 compose：没有 Router，页面原点 0；没有 GLFW，剪贴板/输入法等系统调用是
//     安全 no-op，这些只能钉到「派发/可见面」为止。
//   - 元素 frame 需显式 ui.layout(...) 后才有值，compose 不给几何。
//   - sparkline 数据源细节用例见 tests/test_core/test_proxy_stats.cpp
//     （proxy_stats_rate_series_*）；本文件管「空 → {0.0f}」护栏与 Home compose 接线。
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
#include "core/call_rate.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "core/ui_geometry.h"
#include "core/window_layout.h"  // 最小窗尺寸经同一函数推导（about 页最小窗复核）
#include "module_about/about_anim.h"  // uiAboutGlow：光晕状态机直接驱动
#include "module_claude_proxy/claude_service.h"
#include "module_home/home_service.h"
#include "module_markdown/md_ids.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/metrics.h"  // kHeaderH（板几何断言）
#include <algorithm>
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

// 假时钟 RAII：析构自动 reset（防污染后续用例）。确定性速率采样需要假时钟。
struct FakeClock {
    int64_t now = 1000000;
    FakeClock() { core::proxy_stats::setTestClock([this] { return now; }); }
    ~FakeClock() { core::proxy_stats::resetForTest(); }
};

// todo 无头环境
constexpr const char* kTodoDbPrefix = "devdesk_test_uimodern_todo_";

struct TodoEnv : test_support::TestEnv {
    std::unique_ptr<module::todo::TodoService> svc;

    // 视图状态由基类注册的临时库承担：状态是 app_config 表的一行（键
    // todo_view_state，见 todo_view_state.h），没有独立的状态文件
    TodoEnv() : test_support::TestEnv(kTodoDbPrefix, "", true) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
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

// claude 无头环境
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

// Home 统计卡 / sparkline 护栏
REGISTER_TEST(uiModernSparkSeriesGuard,
              "uiModern: rateSeries 空 → {0.0f}（lineChart demo 数据护栏）", {
    core::proxy_stats::resetForTest();
    FakeClock clock;

    // 无任何 usage 事件：sparkline 数据源必须兜底 {0.0f}，绝不填 lineChart
    // 内置演示序列
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
        // 空闲态不挂 onTimer：常驻会 2Hz 整页重绘并反复打断 hover 过渡；
        // 仅流式活动期挂 1s
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

        // 统计卡根节点（card 组件）：根 id 不变
        DOCTEST_CHECK(ui.find("home.stat.notes") != nullptr);
        // card 组件内部结构：.bg 携带 shadow（panelShadow 随 CardStyle 注入）；
        // .content 为内容容器
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

// 统一锚点换算纯测
REGISTER_TEST(uiModernAnchorPure,
              "uiModern: 统一锚点换算（含 Y、含滚动偏移、dpi 退化）", {
    // 基准：窗口 (100, 200)，页面原点 x=20，dpi=1.0（onContextMenu 通道口径）
    const ::core::MenuAnchor a0 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 1.0f);
    DOCTEST_CHECK_EQ(a0.x, 80.0f);
    DOCTEST_CHECK_EQ(a0.y, 200.0f);
    // 滚动偏移（锚点基于非滚动几何）：
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

// todo 看板右键锚点 = 板几何经统一换算
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

    // 迁移默认 4 列 + 新建 1 列（「右键源列」= col.4）：板几何按 5 列推算，
    // 与 drawContextMenu 内 host.columns->size() 一致
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

// claude profile 列表右键菜单
REGISTER_TEST(uiModernClaudeProfileCtxMenu,
              "uiModern claude_module: 列表右键菜单五项（编辑/建副本/复制配置/删除/设为当前）", {
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

    // 右键 profile.1（中转）→ 菜单打开，五项 + 锚点 = 指针坐标（无 Router →
    // 页面原点 0，dpi=1 → (400, 300) 原样）
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
        for (int i = 0; i < 5; ++i) {
            DOCTEST_CHECK(ui.find("claude.list.ctx.level.0.item." +
                                  std::to_string(i)) != nullptr);
        }
        const auto* l0 = ui.find("claude.list.ctx.level.0.label.0");
        const auto* l1 = ui.find("claude.list.ctx.level.0.label.1");
        const auto* l2 = ui.find("claude.list.ctx.level.0.label.2");
        const auto* l3 = ui.find("claude.list.ctx.level.0.label.3");
        const auto* l4 = ui.find("claude.list.ctx.level.0.label.4");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_REQUIRE(l1 != nullptr);
        DOCTEST_REQUIRE(l2 != nullptr);
        DOCTEST_REQUIRE(l3 != nullptr);
        DOCTEST_REQUIRE(l4 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, std::string("编辑"));
        DOCTEST_CHECK_EQ(l1->text, std::string("建副本"));
        DOCTEST_CHECK_EQ(l2->text, std::string("复制配置"));
        DOCTEST_CHECK_EQ(l3->text, std::string("删除"));
        DOCTEST_CHECK_EQ(l4->text, std::string("设为当前"));
    }

    // 点「复制配置」→ 派发 requestCopyConfig → doCopyConfig
    //（headless：GLFW 未 init，setClipboardText 经 _GLFW_REQUIRE_INIT 早退 =
    // 安全 no-op；故只钉派发的可见面 = toast，剪贴板真写以真机验收为准，
    // 不写假绿断言）
    {
        auto* item = ui.find("claude.list.ctx.level.0.item.2");
        DOCTEST_REQUIRE(item != nullptr);
        item->onClick();
        compose();
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("剪贴板") != std::string::npos);
        // 菜单已关（onSelect 置关；常驻构建但 opacity 收起）
        const auto* menu = ui.find("claude.list.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK_EQ(menu->opacity, 0.0f);
    }

    // 右键重开 → 点「设为当前」→ 激活位变化（与既有按钮同源：requestSetActive）
    {
        eui::PointerEvent pe;
        eui::Rect rect;
        pe.x = 400;
        pe.y = 300;
        compose();
        ui.find("claude.profile.1.wrap")->onContextMenu(pe, rect);
        compose();
        auto* item = ui.find("claude.list.ctx.level.0.item.4");
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

    // 再右键 → 点「删除」→ 弹出删除确认（与既有按钮同源：requestDelete 委托）
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
        auto* item = ui.find("claude.list.ctx.level.0.item.3");
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

        // 稳态：root opacity = 1（默认值）。md.root 不带整页 fade 过渡——
        // 整页溶解 0.15s 造成诡异闪烁，transition 必须保持关闭
        const auto* root = ui.find("md.root");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_CHECK_EQ(root->opacity, 1.0f);
        DOCTEST_CHECK(!root->transition.enabled);
    }

    // Ctrl+Shift+P 切态 → setMode（与顶栏 segmented onChange 同一收口）；
    // 无过渡，切换即硬切
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

        // 切换后首帧：root opacity 保持 1.0（无淡出）
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

    // 全部弹窗走 ui_kit::drawDialogFrame → components::dialog（EUI 已内建
    // 「常驻构建 + opacity 收起」0.16s OutCubic 过渡），toast 同型，DevDesk 侧
    // 不重复加。关闭态元素仍构建、opacity 0 且 disabled（防误触）。
    // 反重构：不要加回「if (!open) return」懒守卫——元素被移除时收起动画会随之消失。
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

    // 迁移默认 4 列 + 新建 1 列 = col.4；卡片区容器常驻（落位淡入载体）
    const auto* cards = frame.ui.find("todo.col.4.cards");
    DOCTEST_REQUIRE(cards != nullptr);
    DOCTEST_CHECK_EQ(cards->opacity, 1.0f);

    // 跟手期间（press → drag）：容器 opacity 由 dragActive 门控 → 0.6 调暗。
    // 不可改回「恒 1 的标志位」：松手时 1.0→0.6→1.0 会闪暗。
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
    // 容器目标回 1.0：0.6→1.0 平滑淡回
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

// md 子项：树/编辑器右键统一换算 + 编辑器直挂
// openFileContextMenu 走 menuAnchorForPointerInPage；x 扣页面区原点的既有回归
// 在 test_markdown.cpp（md_ctx_menu_page_origin），本文件锁「无 Router → 原点 0」
// 形态 + 隐形菜单回归。编辑器 input 链直挂 onContextMenu（不做逐帧 find hack），
// 由 .hit 回调非空 + 锚点断言锁。

REGISTER_TEST(uiModernMdTreeNoGhost,
              "uiModern md: 空路径右键早退不留隐形菜单态（4523e09 回归锁）", {
    // 空笔记目录：树无文件行 → ctxMenuPath_ 恒空；drawNoteContextMenu
    // 每帧强制置关（open 标志与「存在右键目标」同生共灭，直接赋值、不回
    // onOpenChange 回调）。连续两帧断言菜单节点绝不构建（无幽灵菜单）
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
              "uiModern md: 树行/编辑器右键锚点 = 指针坐标统一换算", {
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
    // 编辑器右键：打开笔记进入编辑态 → .hit 节点直挂 onContextMenu，锚点同口径
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

// virtualList / todo card / barChart / 渐变 / recent card
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

    // 列表根 = virtualList 节点（.slot. 行重建语义）；行内容 id 用绝对下标
    compose();
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.0.name") != nullptr);
    // slot 窗口生效：25 行不整段渲染，视口外行不存在（virtualList 裁剪）
    DOCTEST_CHECK(ui.find("claude.profile.15.name") == nullptr);

    // 滚动回退语义：onScrollOffsetChanged 写 profilesScrollOffset_ →
    // 重 compose 视口随偏移前移。共享配置片段列表限高 2 条后区块高 94→204，
    // 列表视口少 110px → 偏移取 1500，让行 17 落进「偏移 ± 1 视口 overscan」渲染窗
    const auto* root = ui.find("claude.profiles.scroll");
    DOCTEST_REQUIRE(root != nullptr);
    DOCTEST_REQUIRE(root->onScrollOffsetChanged != nullptr);
    root->onScrollOffsetChanged(1500.0f);
    compose();
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.17.name") != nullptr);
    // 行命中节点 .bg 保留（选中/右键挂点行内）。offset 1500 下第 0 行已被 slot
    // 窗口裁掉，故在已渲染的行 17 上验 .bg
    DOCTEST_CHECK(ui.find("claude.profile.17.bg") != nullptr);

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernVirtualLogRows,
              "uiModern claude: 调用记录 virtualList 行重建（slot 结构保留）", {
    ClaudeEnv env;
    // 种子：1 个 profile + 25 条调用（FK profile_id 必须真实存在；每页 20 行）
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
    // 打开弹窗（后台取数；有界等待每页 20 行到位）。调用记录在详情页签的第 5 个
    //（idx 4）——先切 tab（该帧生效），再点「查看全部 →」开弹窗
    {
        compose();
        const auto* tab = ui.find("claude.detail.tabs.hit.4");
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
    // 列表根 = virtualList 节点（slot 重建语义）；id 用绝对行号
    DOCTEST_CHECK(ui.find("claude.dialog.calls.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.dialog.calls.row.0.wrap") != nullptr);
    DOCTEST_CHECK(ui.find("claude.dialog.calls.row.19.wrap") != nullptr);
    DOCTEST_CHECK(ui.find("claude.dialog.calls.row.20.wrap") == nullptr);

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
    // 命中层是 .bg（不是 .card 骨架）：事件挂点全在它身上，别挪到骨架上
    const auto* bg = frame.ui.find("todo.col.4.card.0.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_CHECK(bg->onClick != nullptr);
    DOCTEST_CHECK(bg->onContextMenu != nullptr);
    DOCTEST_CHECK(bg->onPress != nullptr);
    DOCTEST_CHECK(bg->onDrag != nullptr);
    DOCTEST_CHECK(bg->onRelease != nullptr);

    mod->onStop();
    return true;
})

REGISTER_TEST(uiModernHomeApiBarChart,
              "uiModern: Home Token 分布（每小时 token 合计 + 去头去尾 + 最近速率卡）", {
    test_support::TestEnv env("devdesk_test_uimodern_home_", "apichart",
                              true);  // 注册 appDatabase：全零走空态不画假条，
                                      // 需真实数据才能断言柱子
    DOCTEST_REQUIRE(env.ready);
    // 种子（纵轴 = 每小时 output_tokens 合计）：
    //   1 个 profile（FK 依赖）
    //   · 9 时  output 100 / 2000ms  → 速率 50.0
    //   · 12 时 output 300 / 1000ms  → 速率 300.0
    //   · 17 时 output 600 / 4000ms  → 速率 150.0（最近一次「成功」调用）
    //   · 6 时  output_tokens=0      → 不贡献总量、不扩 x 轴区间（去头）
    //   · 20 时 status=500（output NULL）→ 非 2xx，不参与速率、不扩区间（去尾）
    //   · 昨天 2 条（整天过滤掉）
    // 手算：逐时合计 = {9:100, 12:300, 17:600}，其余 0；
    //       去头去尾区间 = [9, 17]（闭区间，9 根柱），峰值 = 600
    const int64_t dayStart = module::home::startOfDayEpoch(std::time(nullptr));
    DOCTEST_CHECK(env.db->db().exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
        " api_key_check, model, is_active, created_at, "
        " updated_at) VALUES ('测试', 'https://api.anthropic.com', "
        " 'sk-', 'CHECK', 'claude-sonnet-4-5', 1, "
        " 1000, 1000)").isOk());
    const std::string sql =
        "INSERT INTO claude_call_logs (profile_id, method, path, status_code, "
        " latency_ms, output_tokens, request_bytes, response_bytes, error, "
        " started_at) VALUES "
        "(1, 'POST', '/v1/messages', 200, 2000, 100, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 9) + "), "
        "(1, 'POST', '/v1/messages', 200, 1000, 300, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 12) + "), "
        "(1, 'POST', '/v1/messages', 200, 4000, 600, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 17) + "), "
        "(1, 'POST', '/v1/messages', 200, 500, 0, 100, 200, '', " +
        std::to_string(dayStart + 3600 * 6) + "), "
        "(1, 'POST', '/v1/messages', 500, 500, NULL, 100, 200, 'boom', " +
        std::to_string(dayStart + 3600 * 20) + "), "
        "(1, 'GET', '/v1/models', 200, 5, NULL, 20, 40, '', " +
        std::to_string(dayStart - 3600) + "), "
        "(1, 'GET', '/v1/models', 200, 5, NULL, 20, 40, '', " +
        std::to_string(dayStart - 7200) + ")";
    DOCTEST_CHECK(env.db->db().exec(sql).isOk());

    // 服务层：24 点恒非空、逐时 token 合计 == 手算值、日界过滤（昨天不进）
    {
        module::home::HomeService svc(env.db->db());
        auto series = svc.getTodayHourlyTokenTotals(24);
        DOCTEST_REQUIRE(series.isOk());
        DOCTEST_CHECK_EQ(series.value().size(), 24u);
        int64_t total = 0;
        for (const int64_t v : series.value()) total += v;
        DOCTEST_CHECK_EQ(total, 1000);            // 100 + 300 + 600
        DOCTEST_CHECK_EQ(series.value()[9], 100);
        DOCTEST_CHECK_EQ(series.value()[12], 300);
        DOCTEST_CHECK_EQ(series.value()[17], 600);
        DOCTEST_CHECK_EQ(series.value()[6], 0);   // 0 token 行不贡献
        DOCTEST_CHECK_EQ(series.value()[20], 0);  // 非 2xx 行不贡献
        DOCTEST_CHECK_EQ(series.value()[13], 0);  // 区间内的空小时
    }

    // 纯函数：去头去尾 → 只画 [9, 17]
    {
        std::vector<int64_t> hourly(24, 0);
        hourly[9] = 100;
        hourly[17] = 600;
        const auto r = module::home::hourlyDataRange(hourly);
        DOCTEST_CHECK(r.hasData);
        DOCTEST_CHECK_EQ(r.first, static_cast<size_t>(9));
        DOCTEST_CHECK_EQ(r.last, static_cast<size_t>(17));
        // 整天无数据 → 空态（不画空轴）
        const std::vector<int64_t> empty(24, 0);
        DOCTEST_CHECK(!module::home::hourlyDataRange(empty).hasData);
        // 头尾相接（只有 1 个小时有数据）→ first == last
        std::vector<int64_t> one(24, 0);
        one[3] = 7;
        const auto r1 = module::home::hourlyDataRange(one);
        DOCTEST_CHECK(r1.hasData);
        DOCTEST_CHECK_EQ(r1.first, static_cast<size_t>(3));
        DOCTEST_CHECK_EQ(r1.last, static_cast<size_t>(3));
    }

    // 服务层：最近一次成功调用 = 17 时那条 → 600 / 4s = 150.0 tok/s
    //（20 时的 500 行更晚但不是成功调用 → 不参与；6 时 output 0 → 不参与）
    {
        module::home::HomeService svc(env.db->db());
        auto lr = svc.getLastCallRate();
        DOCTEST_REQUIRE(lr.isOk());
        DOCTEST_CHECK(lr.value().hasValue);
        DOCTEST_CHECK_EQ(lr.value().outputTokens, static_cast<int64_t>(600));
        DOCTEST_CHECK_EQ(lr.value().latencyMs, static_cast<int64_t>(4000));
        DOCTEST_CHECK(std::fabs(lr.value().tps - 150.0) < 0.001);
        DOCTEST_CHECK_EQ(core::call_rate::formatTps(lr.value().tps),
                         std::string("150.0"));
    }

    // UI 接线 + 数值断言：去头去尾后的柱数/标签/相对柱高 + ① 的卡片数字
    {
        std::unique_ptr<::core::Module> mod(create_home_module());
        mod->onStart();  // 挂真实服务（否则 stats 未加载 → 空态）
        eui::Screen screen{1200.0f, 720.0f};
        eui::Ui ui;
        ui.begin("home_api_chart");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);  // 元素 frame 需 layout 后才有值
        DOCTEST_CHECK(ui.find("home.stat.api") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars") != nullptr);
        // 去头去尾：9 根柱（9..17 闭区间），第 10 根起不存在（回退成 24 根
        // 或「按小时号绝对下标」都会让下面这条红）
        DOCTEST_CHECK(ui.find("home.api.chart.bars.bar.0") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars.bar.8") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars.bar.9") == nullptr);
        // x 轴首尾小时标签 = 9 时 / 17 时
        const auto textOf = [](eui::Ui& u, const std::string& id) {
            const auto* el = u.find(id);
            return el != nullptr ? el->text : std::string();
        };
        DOCTEST_CHECK_EQ(textOf(ui, "home.api.chart.bars.label.0"),
                         std::string("9时"));
        DOCTEST_CHECK_EQ(textOf(ui, "home.api.chart.bars.label.8"),
                         std::string("17时"));
        // 柱顶数值 = 该小时 token 合计（不再是百分比）
        DOCTEST_CHECK_EQ(textOf(ui, "home.api.chart.bars.val.0"),
                         std::string("100"));
        DOCTEST_CHECK_EQ(textOf(ui, "home.api.chart.bars.val.3"),
                         std::string("300"));
        DOCTEST_CHECK_EQ(textOf(ui, "home.api.chart.bars.val.8"),
                         std::string("600"));
        // 相对柱高 == 相对 token 数（峰值柱满高 84，100/600 的柱子 ≈ 14）
        const auto* bar0 = ui.find("home.api.chart.bars.bar.0");
        const auto* bar3 = ui.find("home.api.chart.bars.bar.3");
        const auto* bar8 = ui.find("home.api.chart.bars.bar.8");
        DOCTEST_REQUIRE(bar0 != nullptr);
        DOCTEST_REQUIRE(bar3 != nullptr);
        DOCTEST_REQUIRE(bar8 != nullptr);
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar8->frame.height - 84.0f) < 0.5f,
            "bar8 h=" + std::to_string(bar8->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar0->frame.height - 14.0f) < 0.5f,
            "bar0 h=" + std::to_string(bar0->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar3->frame.height - 42.0f) < 0.5f,
            "bar3 h=" + std::to_string(bar3->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar0->frame.height - 14.0f) < 0.5f,
            "bar0 h=" + std::to_string(bar0->frame.height) + " y=" +
                std::to_string(bar0->frame.y));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar3->frame.height - 42.0f) < 0.5f,
            "bar3 h=" + std::to_string(bar3->frame.height) + " y=" +
                std::to_string(bar3->frame.y));
        // 柱高对齐基线：三根柱底沿相等（y + height 同值）
        DOCTEST_CHECK(std::fabs((bar0->frame.y + bar0->frame.height) -
                                (bar8->frame.y + bar8->frame.height)) < 0.5f);
        // 卡片数字 = 最近一次成功调用的速率（150.0），副标题不再写「累计」
        DOCTEST_CHECK_EQ(textOf(ui, "home.stat.tokens.number"),
                         std::string("150.0"));
        DOCTEST_CHECK_EQ(textOf(ui, "home.stat.tokens.badge"),
                         std::string("最近一次成功调用"));
        mod->onStop();
    }
    return true;
})

// 设置页主体底板：不存在。
// 设置页按 frost 口径「各模块页面要么靠 clearColor 当底、要么自铺不透明 surface 色」
// 渲染。断言方向**取反**（存在即红）。另有一条反证断言（默认 tab 的卡仍在 →
// 页面确实 compose 过），防整页 compose 早退时 find 恒 nullptr 造成的假绿。
REGISTER_TEST(uiModernSettingsBodyGradientRemoved,
              "uiModern: 设置页无主体底板（settings.body.bg 必须不存在）", {
    const std::filesystem::path dbPath = test_temp::tempPath(
        "devdesk_test_uimodern_settings_grad.db");
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    auto r = ::core::Database::open(dbPath);
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<::core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    ::core::registerAppDatabase(&db->db());
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
    ::core::registerAppDatabase(nullptr);
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
        // W8：渐变已移除（§8 #4 禁大面积渐变），改为 flat pageBg。
        DOCTEST_CHECK(!bg->gradient.enabled);
        mod->onStop();
    }
    return true;
})

// about 页布局：内容区加宽 / 顶部留白 / 删 hero 重复三行 / 卡片上下堆叠 /
// footer 贴页面底部。只钉可客观断言的部分；观感留人工确认。页面宽 = 窗口宽 −
// 侧边栏（app_sidebar.cpp pageContentWidth，展开 208）。最小窗尺寸不手写：由
// core::window_layout::minWindowSizeForWorkArea 推导（手写的 845 已失义——
// macOS 工作区 833 点会把最小窗高钳到 805）。
namespace {
// 与 main.cpp 同一条决策链的最小窗尺寸（macOS 档：工作区 1470×833 点、
// windowScaleForPoint 恒 1.0、外框 {0,28}）→ {1330, 805}。显式传参而非取本机
// 实时工作区：测试要确定性，且这一档正是「用户机器上最小窗能缩到多大」的真实值。
core::window_layout::WindowSize minWindowForMacWorkArea() {
    return core::window_layout::minWindowSizeForWorkArea(
        core::window_layout::InitialWindowRequest{1600, 1000, 1330, 845},
        core::window_layout::WorkArea{0, 0, 1470, 833}, 1.0f,
        core::window_layout::FrameInsets{0, 28});
}

// 布局帧比较容差：逻辑像素差 ≤0.5 视为相等（布局引擎可能带亚像素误差）
bool nearPx(float a, float b) { return std::abs(a - b) <= 0.5f; }
}  // namespace

REGISTER_TEST(uiAboutLayout,
              "about: 内容区 900 / 内容块贴底（底边=页脚顶−间距）/ 极矮窗 clamp 回 topPad / footer 贴页面底部（真机反馈）", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    const auto& t = ::core::theme::current();
    constexpr float kContentW = 900.0f;
    constexpr float kCardGap = 8.0f;
    constexpr float kTitleH = 40.0f;
    constexpr float kCardH = 24.0f + 28.0f + 6.0f * 28.0f + 24.0f;  // 244（6 行卡）
    const float topPad = t.sizes.spacingXXLarge;                   // 40（仅极矮窗兜底）
    const float iconSize = t.sizes.spacingXXLarge + t.sizes.spacingLarge;  // 56
    const float heroToCards = t.sizes.spacingXLarge;                       // 24
    const float footerH = 20.0f;
    // 内容块总高（hero 图标 + 图标/标题 8 + 标题 40 + 标题到卡一档 XLarge
    // + 卡1 + 卡距 + 卡2 = 624）与贴底口径：起点 = max(topPad,
    // 页脚顶 − heroToCards − contentH)
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
    // hero 副标题三行不存在（版本行 / 构建时刻 / commit+编译器 与「应用信息」卡
    // 逐项重复）
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
    // 卡片上下堆叠（非左右并排）：同宽（占满内容区 900）、同 x、各 244 高；
    // 卡 1 顶 = 内容块起点 + hero 图标 + 8 + 标题 40 + 一档 XLarge，
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
    // 贴底对齐核心断言：内容块底边 = 页脚顶 − 一档间距（hero + 两卡整体贴向
    // 页脚上方，而非顶部对齐；回退成 topPad 起点即红）
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
    // 最小窗复核：minWindowSizeForWorkArea → {1330, 805} → 页面宽
    // 1330 − 208 = 1122 → 内容区 900 不溢出、仍居中；页面高 805 下贴底口径仍成立
    //（页脚顶 745 → 内容块底 721、起点 97 ≥ topPad 40，不触发 clamp）
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
    // 极矮窗复核（贴底护栏）：内容块 + 一档间距放不进「页脚上方」这块高度时，
    // 起点 clamp 回 topPad（退回顶部对齐），且卡与页脚不得重叠
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

// about 页动态光晕：光斑同心圆叠层 + 利萨如漂移 + 呼吸 + 聚光扫过带。
// 钉六件事：① 元素闸门（光斑 6×3 层 + 扫带 2 = 20 ≤ 20）；② 直径 240-400；
// ③ 层 alpha 剖面 0.15 → 0.05；④ 呼吸 ±35%（在时间轴上扫出真实极值）；
// ⑤ 扫带周期 14s 且真的从左到右；⑥ 漂移真在动 + 空闲 120s 后休眠（不挂
// onTimer）且指针移动可唤醒。
// **一律用字面量**：拿头文件常量当期望值等于自己——把常量调淡/调回去断言仍真。
REGISTER_TEST(uiAboutGlow,
              "about: 光晕 6 光斑/直径 240-400/呼吸 ±35%/扫带 14s/元素 ≤20/漂移与休眠", {
    using Anim = module::about::AboutAnimState;
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    eui::Ui ui;
    eui::Screen screen{1392.0f, 1000.0f};  // 默认窗 1600 − 侧边栏 208
    ui.begin("about_glow");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    auto& anim = ui.state<Anim>("about.anim");
    DOCTEST_REQUIRE(anim.initialized);

    // ① 元素闸门：每帧新增绘制元素 ≤ 20（光斑 6×3 + 扫带 2 = 20）
    constexpr int kExpectBlobCount = 6;   // 3→6
    constexpr int kExpectLayers = 3;      // 数量翻倍靠层数减半守住闸门
    constexpr int kExpectElementCount = 20;
    constexpr int kElementGate = 20;
    DOCTEST_CHECK_EQ(Anim::kBlobCount, kExpectBlobCount);
    DOCTEST_CHECK_EQ(Anim::kLayersPerBlob, kExpectLayers);
    int drawn = 0;
    for (int i = 0; i < kExpectBlobCount; ++i) {
        for (int k = 0; k < kExpectLayers; ++k) {
            if (ui.find("about.glow." + std::to_string(i) + "." + std::to_string(k)) !=
                nullptr) {
                ++drawn;
            }
        }
    }
    for (int half = 0; half < 2; ++half) {
        if (ui.find("about.sweep." + std::to_string(half)) != nullptr) {
            ++drawn;
        }
    }
    DOCTEST_CHECK_MESSAGE(drawn == kExpectElementCount,
                          "光晕元素数 " + std::to_string(drawn));
    DOCTEST_CHECK_MESSAGE(drawn <= kElementGate,
                          "元素闸门 ≤ 20，实为 " + std::to_string(drawn));
    // 7 个 1px 浮环已下线，避免与光晕叠成双层背景噪声
    DOCTEST_CHECK(ui.find("about.ring.0") == nullptr);

    // ② 直径区间 240-400（字面量，不引用头文件常量）
    constexpr float kExpectDiameterMin = 240.0f;
    constexpr float kExpectDiameterMax = 400.0f;
    bool diameterOk = true;
    std::string diameterDetail;
    for (int i = 0; i < kExpectBlobCount; ++i) {
        const float d = anim.blobs[static_cast<std::size_t>(i)].radius * 2.0f;
        if (d < kExpectDiameterMin - 1e-3f || d > kExpectDiameterMax + 1e-3f) {
            diameterOk = false;
            diameterDetail = "blob" + std::to_string(i) + " 直径 " +
                             std::to_string(d);
        }
    }
    DOCTEST_CHECK_MESSAGE(diameterOk, diameterDetail);

    // ③ 层 alpha 剖面：0.15（最内）→ 0.05（最外），单调递减（3 层线性插值）
    constexpr float kExpectLayerAlphaMax = 0.15f;
    constexpr float kExpectLayerAlphaMin = 0.05f;
    DOCTEST_CHECK(std::abs(module::about::blobLayerAlpha(0) -
                           kExpectLayerAlphaMax) < 1e-4f);
    DOCTEST_CHECK(std::abs(module::about::blobLayerAlpha(kExpectLayers - 1) -
                           kExpectLayerAlphaMin) < 1e-4f);
    bool alphaMonotonic = true;
    for (int k = 1; k < kExpectLayers; ++k) {
        alphaMonotonic = alphaMonotonic &&
                         module::about::blobLayerAlpha(k) <
                             module::about::blobLayerAlpha(k - 1);
    }
    DOCTEST_CHECK(alphaMonotonic);

    // ④ 呼吸 ±35%：在时间轴上扫两个呼吸周期取真实极值
    constexpr float kExpectBreathDepth = 0.35f;
    Anim probe = anim;  // 副本：位置/呼吸/扫带都是时间的纯函数，改 time 即可取样
    float breathMin = 2.0f;
    float breathMax = 0.0f;
    for (int i = 0; i < kExpectBlobCount; ++i) {
        const float period = anim.blobs[static_cast<std::size_t>(i)].breathPeriod;
        DOCTEST_CHECK(period >= 6.0f - 1e-3f && period <= 9.0f + 1e-3f);
        for (int stepIndex = 0; stepIndex <= 400; ++stepIndex) {
            probe.time = period * 2.0f * static_cast<float>(stepIndex) / 400.0f;
            const float b = module::about::blobBreath(probe, i);
            breathMin = std::min(breathMin, b);
            breathMax = std::max(breathMax, b);
        }
    }
    DOCTEST_CHECK_MESSAGE(breathMax > 1.0f + kExpectBreathDepth - 0.02f &&
                              breathMax < 1.0f + kExpectBreathDepth + 0.02f,
                          "呼吸峰值 " + std::to_string(breathMax));
    DOCTEST_CHECK_MESSAGE(breathMin < 1.0f - kExpectBreathDepth + 0.02f &&
                              breathMin > 1.0f - kExpectBreathDepth - 0.02f,
                          "呼吸谷值 " + std::to_string(breathMin));
    // 合成 alpha 落在 [0.65, 1.35] × 层基准，且严格 > 0（可见）
    const float composedAlpha =
        module::about::blobLayerAlpha(0) * module::about::blobBreath(anim, 0);
    DOCTEST_CHECK(composedAlpha > 0.0f && composedAlpha <= 0.21f + 1e-4f);

    // ⑤ 漂移：利萨如周期 18-26s；1.2s 内真的挪动（真机 1.2s 抓两帧的口径）
    constexpr float kExpectPeriodMin = 18.0f;
    constexpr float kExpectPeriodMax = 26.0f;
    bool periodOk = true;
    for (int i = 0; i < kExpectBlobCount; ++i) {
        const auto& blob = anim.blobs[static_cast<std::size_t>(i)];
        periodOk = periodOk && blob.periodX >= kExpectPeriodMin - 1e-3f &&
                   blob.periodX <= kExpectPeriodMax + 1e-3f;
        // 周期语义自洽：t 与 t+periodX 的 x 相同、t 与 t+periodY 的 y 相同
        probe = anim;
        probe.time = 0.0f;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        module::about::blobCenter(probe, i, screen.width, screen.height, x0, y0);
        probe.time = blob.periodX;
        module::about::blobCenter(probe, i, screen.width, screen.height, x1, y1);
        DOCTEST_CHECK(std::abs(x1 - x0) < 1e-2f);
        probe.time = blob.periodY;
        module::about::blobCenter(probe, i, screen.width, screen.height, x1, y1);
        DOCTEST_CHECK(std::abs(y1 - y0) < 1e-2f);
        // 1.2s 位移（正交两轴之和 → 一定 > 0）
        probe.time = 0.0f;
        module::about::blobCenter(probe, i, screen.width, screen.height, x0, y0);
        probe.time = 1.2f;
        module::about::blobCenter(probe, i, screen.width, screen.height, x1, y1);
        const float moved = std::abs(x1 - x0) + std::abs(y1 - y0);
        DOCTEST_CHECK_MESSAGE(moved > 1.0f,
                              "blob" + std::to_string(i) + " 1.2s 位移 " +
                                  std::to_string(moved));
    }
    DOCTEST_CHECK(periodOk);

    // ⑥ 扫带：周期 14s / 宽 220 / 倾角 18° / 峰值 alpha 0.05；一个周期回到起点，
    //    周期内单调右移（左→右扫过）
    constexpr float kExpectSweepPeriod = 14.0f;
    constexpr float kExpectSweepWidth = 220.0f;
    constexpr float kExpectSweepAlpha = 0.05f;
    constexpr float kExpectSweepTiltRad = 0.31415926f;  // 18°
    DOCTEST_CHECK(std::abs(Anim::kSweepPeriod - kExpectSweepPeriod) < 1e-4f);
    DOCTEST_CHECK(std::abs(Anim::kSweepWidth - kExpectSweepWidth) < 1e-4f);
    DOCTEST_CHECK(std::abs(Anim::kSweepAlpha - kExpectSweepAlpha) < 1e-4f);
    DOCTEST_CHECK(std::abs(Anim::kSweepTiltRad - kExpectSweepTiltRad) < 1e-5f);
    probe.time = 0.0f;
    const float sweepStart = module::about::sweepLeft(probe, screen.width);
    probe.time = 3.0f;
    const float sweepMid = module::about::sweepLeft(probe, screen.width);
    probe.time = 6.0f;
    const float sweepLate = module::about::sweepLeft(probe, screen.width);
    probe.time = kExpectSweepPeriod;
    const float sweepWrap = module::about::sweepLeft(probe, screen.width);
    DOCTEST_CHECK_MESSAGE(sweepMid > sweepStart && sweepLate > sweepMid,
                          "扫带应左→右：" + std::to_string(sweepStart) + " → " +
                              std::to_string(sweepMid) + " → " +
                              std::to_string(sweepLate));
    DOCTEST_CHECK(std::abs(sweepWrap - sweepStart) < 1e-2f);
    DOCTEST_CHECK(sweepStart <= -kExpectSweepWidth + 1e-3f);  // 起点在左界外
    // 扫带在 1.2s 内跨过 > 100px（真机 1.2s 抓两帧的动效来源之一）
    probe.time = 4.0f;
    const float sweepA = module::about::sweepLeft(probe, screen.width);
    probe.time = 5.2f;
    const float sweepB = module::about::sweepLeft(probe, screen.width);
    DOCTEST_CHECK_MESSAGE(sweepB - sweepA > 100.0f,
                          "扫带 1.2s 位移 " + std::to_string(sweepB - sweepA));

    // 端到端 compose：元素颜色 alpha = 层基准 × 呼吸；驱动是 onTimer(1/30)
    const auto* bg = ui.find("about.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_REQUIRE(bg->onTimer != nullptr);  // 唤醒期：挂节拍驱动
    DOCTEST_CHECK(std::abs(bg->timerSeconds - 1.0f / 30.0f) < 1e-5f);
    const auto* layerInner = ui.find("about.glow.0.0");
    DOCTEST_REQUIRE(layerInner != nullptr);
    DOCTEST_CHECK_MESSAGE(
        std::abs(layerInner->color.a - composedAlpha) < 1e-3f,
        "内层 alpha " + std::to_string(layerInner->color.a) + " 期望 " +
            std::to_string(composedAlpha));
    const auto* layerOuter = ui.find("about.glow.0.2");
    DOCTEST_REQUIRE(layerOuter != nullptr);
    // 最外圈挂霓虹 shadow（blur 48 / primary@0.35）
    DOCTEST_CHECK(layerOuter->shadow.enabled);
    DOCTEST_CHECK(std::abs(layerOuter->shadow.blur - 48.0f) < 1e-3f);
    DOCTEST_CHECK(std::abs(layerOuter->shadow.color.a - 0.35f) < 1e-3f);
    // 内层挂同色柔边 shadow（blur 56）：抹平同心圆台阶，别被静默删掉——层数少时
    // 台阶更高，blur 必须守住「单位像素梯度不升」
    const auto* layerMid = ui.find("about.glow.0.1");
    DOCTEST_REQUIRE(layerMid != nullptr);
    DOCTEST_CHECK(layerMid->shadow.enabled);
    DOCTEST_CHECK(std::abs(layerMid->shadow.blur - 56.0f) < 1e-3f);
    // 节拍回调真的推进状态（onTimer → step(kFrameSeconds)）
    const float timeBefore = anim.time;
    bg->onTimer();
    DOCTEST_CHECK(std::abs(anim.time - (timeBefore + 1.0f / 30.0f)) < 1e-5f);
    const float x0 = layerInner->frame.x;
    const float y0 = layerInner->frame.y;
    for (int f = 0; f < 90; ++f) {  // 3s 节拍（= 90/30）
        bg->onTimer();
    }
    ui.begin("about_glow");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    const auto* layerInner2 = ui.find("about.glow.0.0");
    DOCTEST_REQUIRE(layerInner2 != nullptr);
    const float moved = std::abs(layerInner2->frame.x - x0) +
                        std::abs(layerInner2->frame.y - y0);
    DOCTEST_CHECK_MESSAGE(moved > 1.0f,
                          "光斑 3s 位移 " + std::to_string(moved));

    // ⑧ 空闲 120s → 休眠（compose 不再挂 onTimer）；指针移动唤醒
    constexpr float kExpectWakeHold = 120.0f;
    DOCTEST_CHECK(std::abs(Anim::kWakeHoldSeconds - kExpectWakeHold) < 1e-3f);
    const auto* bgAwake = ui.find("about.bg");
    DOCTEST_REQUIRE(bgAwake != nullptr);
    for (int f = 0; f < static_cast<int>(kExpectWakeHold / (1.0f / 30.0f)) + 2;
         ++f) {
        bgAwake->onTimer();
    }
    DOCTEST_CHECK(!anim.awake());
    ui.begin("about_glow");
    mod->compose(ui, screen);
    ui.end();
    const auto* bgAsleep = ui.find("about.bg");
    DOCTEST_REQUIRE(bgAsleep != nullptr);
    DOCTEST_CHECK(bgAsleep->onTimer == nullptr);  // 休眠：零合成零重绘
    DOCTEST_REQUIRE(bgAsleep->onMove != nullptr);
    (void)bgAsleep->onMove(eui::PointerEvent{}, eui::Rect{});
    DOCTEST_CHECK(anim.awake());
    ui.begin("about_glow");
    mod->compose(ui, screen);
    ui.end();
    const auto* bgWoken = ui.find("about.bg");
    DOCTEST_REQUIRE(bgWoken != nullptr);
    DOCTEST_CHECK(bgWoken->onTimer != nullptr);  // 唤醒：重新挂驱动
    mod->onStop();
    return true;
})

