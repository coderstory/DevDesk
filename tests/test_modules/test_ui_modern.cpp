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
#include "core/window_effect_state.h"  // about 页底 alpha 按窗口效果档位（UAT2 B 批）
// W10: about_anim.h 已清空为空壳，不再 include（光晕移除）
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
        // 相对柱高 == 相对 token 数（UAT6 零滚动收口后图表卡 220 → wrap 156 →
        // plot 44：峰值柱满高 44，100/600 的柱子 ≈ 44/6）
        const auto* bar0 = ui.find("home.api.chart.bars.bar.0");
        const auto* bar3 = ui.find("home.api.chart.bars.bar.3");
        const auto* bar8 = ui.find("home.api.chart.bars.bar.8");
        DOCTEST_REQUIRE(bar0 != nullptr);
        DOCTEST_REQUIRE(bar3 != nullptr);
        DOCTEST_REQUIRE(bar8 != nullptr);
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar8->frame.height - 44.0f) < 0.5f,
            "bar8 h=" + std::to_string(bar8->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar0->frame.height - 44.0f / 6.0f) < 0.5f,
            "bar0 h=" + std::to_string(bar0->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar3->frame.height - 22.0f) < 0.5f,
            "bar3 h=" + std::to_string(bar3->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar0->frame.height - 44.0f / 6.0f) < 0.5f,
            "bar0 h=" + std::to_string(bar0->frame.height) + " y=" +
                std::to_string(bar0->frame.y));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar3->frame.height - 22.0f) < 0.5f,
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
// core::window_layout::minWindowSizeForWorkArea 推导（设计值换档后手写屏高
// 不再代表任何真实档，恒经同一函数取值）。
namespace {
// 与 main.cpp 同一条决策链的最小窗尺寸（macOS 档：工作区 1470×833 点、
// windowScaleForPoint 恒 1.0、外框 {0,28}）→ {1000, 640}（设计 640 ≤ 工作区可用
// 805，不钳；UAT3 D-R3-2 起两平台有效下限 = 设计下限）。显式传参
// 而非取本机实时工作区：测试要确定性，且这一档正是「用户机器上最小窗能缩到多大」的真实值。
core::window_layout::WindowSize minWindowForMacWorkArea() {
    return core::window_layout::minWindowSizeForWorkArea(
        core::window_layout::InitialWindowRequest{1600, 1000, 1000, 640},
        core::window_layout::WorkArea{0, 0, 1470, 833}, 1.0f,
        core::window_layout::FrameInsets{0, 28});
}

// 布局帧比较容差：逻辑像素差 ≤0.5 视为相等（布局引擎可能带亚像素误差）
bool nearPx(float a, float b) { return std::abs(a - b) <= 0.5f; }
}  // namespace

REGISTER_TEST(uiAboutLayout,
              "about: W10 微信风格—居中白卡 760（窄屏自适应）/ 信息列表两列 / 版权 footer / 极矮窗 clamp", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    const auto& t = ::core::theme::current();
    // 与 about_module.cpp 同值镜像（UAT3 放大：580→760 / pad 32→48 / 上下 40→60 /
    // 图标 56→72 / 行高 40→48）——两边漂移时本用例红
    constexpr float kCardW = 760.0f;
    constexpr float kCardPadX = 48.0f;
    constexpr float kCardPadTop = 60.0f;
    constexpr float kCardPadBottom = 60.0f;
    constexpr float kIconSize = 72.0f;
    constexpr float kIconGap = 10.0f;
    constexpr float kTitleH = 30.0f;
    constexpr float kTitleGap = 28.0f;
    constexpr float kRowH = 48.0f;
    constexpr int kRowCount = 6;
    constexpr float kFooterH = 20.0f;
    constexpr float kFooterGap = 16.0f;
    constexpr float kLabelW = 120.0f;
    const float cardH = kCardPadTop + kIconSize + kIconGap + kTitleH +
                        kTitleGap + kRowCount * kRowH + kCardPadBottom;
    eui::Ui ui;
    eui::Screen screen{1600.0f, 1000.0f};
    // 宽度自适应公式镜像（UAT3：窄窗 clamp，与 about_module.cpp 同式）
    const auto cardWidthAt = [=](float screenWidth) {
        return std::max(560.0f, std::min(kCardW, screenWidth - 2.0f * 24.0f));
    };
    const float cardW = cardWidthAt(screen.width);
    const float cardX = (screen.width - cardW) / 2.0f;
    const float footerY = screen.height - t.sizes.spacingXXLarge - kFooterH;
    const float cardY = t.sizes.spacingXXLarge;
    const float valueX = kCardPadX + kLabelW + t.sizes.spacing;
    ui.begin("about_layout");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    auto px = [](const char* what, const core::dsl::Element* e) {
        return std::string(what) + " 实测 (" +
               std::to_string(e->frame.x) + ", " +
               std::to_string(e->frame.y) + ", " +
               std::to_string(e->frame.width) + "x" +
               std::to_string(e->frame.height) + ")";
    };
    {
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(bg->frame.x, 0.0f), px("about.bg", bg));
        DOCTEST_CHECK_MESSAGE(nearPx(bg->frame.y, 0.0f), px("about.bg", bg));
        DOCTEST_CHECK_MESSAGE(nearPx(bg->frame.width, screen.width), px("about.bg", bg));
        DOCTEST_CHECK_MESSAGE(nearPx(bg->frame.height, screen.height), px("about.bg", bg));
    }
    {
        const auto* card = ui.find("about.card");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(card->frame.x, cardX), px("about.card.x", card));
        DOCTEST_CHECK_MESSAGE(nearPx(card->frame.y, cardY), px("about.card.y", card));
        DOCTEST_CHECK_MESSAGE(nearPx(card->frame.width, cardW), px("about.card.w", card));
        DOCTEST_CHECK_MESSAGE(nearPx(card->frame.height, cardH), px("about.card.h", card));
        DOCTEST_CHECK(card->frame.y + card->frame.height + kFooterGap <= footerY);
    }
    {
        const auto* cbg = ui.find("about.card.bg");
        DOCTEST_REQUIRE(cbg != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(cbg->frame.width, cardW), px("about.card.bg", cbg));
    }
    {
        const auto* icon = ui.find("about.card.icon");
        DOCTEST_REQUIRE(icon != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(icon->frame.x, cardX + (cardW - kIconSize) / 2.0f),
                              px("about.card.icon", icon));
        DOCTEST_CHECK_MESSAGE(nearPx(icon->frame.y, cardY + kCardPadTop),
                              px("about.card.icon", icon));
        DOCTEST_CHECK_MESSAGE(nearPx(icon->frame.width, kIconSize),
                              px("about.card.icon", icon));
    }
    {
        const auto* title = ui.find("about.card.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.x, cardX), px("about.card.title", title));
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.y, cardY + kCardPadTop + kIconSize + kIconGap),
                              px("about.card.title", title));
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.width, cardW),
                              px("about.card.title", title));
    }
    {
        const float rowsY = cardY + kCardPadTop + kIconSize + kIconGap + kTitleH + kTitleGap;
        for (int i = 0; i < kRowCount; ++i) {
            const float rowY = rowsY + i * kRowH;
            const auto* label = ui.find("about.card.label." + std::to_string(i));
            const auto* value = ui.find("about.card.value." + std::to_string(i));
            DOCTEST_REQUIRE(label != nullptr);
            DOCTEST_REQUIRE(value != nullptr);
            DOCTEST_CHECK_MESSAGE(nearPx(label->frame.x, cardX + kCardPadX),
                                  px(("about.card.label." + std::to_string(i)).c_str(), label));
            DOCTEST_CHECK_MESSAGE(nearPx(label->frame.y, rowY),
                                  px(("about.card.label." + std::to_string(i)).c_str(), label));
            DOCTEST_CHECK_MESSAGE(nearPx(value->frame.x, cardX + valueX),
                                  px(("about.card.value." + std::to_string(i)).c_str(), value));
            DOCTEST_CHECK_MESSAGE(nearPx(value->frame.y, rowY),
                                  px(("about.card.value." + std::to_string(i)).c_str(), value));
            if (i > 0) {
                const auto* div = ui.find("about.card.divider." + std::to_string(i));
                DOCTEST_REQUIRE(div != nullptr);
            }
        }
        const auto* copy0 = ui.find("about.card.copy.0");
        const auto* copy1 = ui.find("about.card.copy.1");
        const auto* hit0 = ui.find("about.card.hit.0");
        const auto* hit1 = ui.find("about.card.hit.1");
        DOCTEST_REQUIRE(copy0 != nullptr);
        DOCTEST_REQUIRE(copy1 != nullptr);
        DOCTEST_REQUIRE(hit0 != nullptr);
        DOCTEST_REQUIRE(hit1 != nullptr);
        DOCTEST_CHECK(ui.find("about.card.copy.2") == nullptr);
        DOCTEST_CHECK(ui.find("about.card.hit.2") == nullptr);
    }
    {
        const auto* footer = ui.find("about.footer");
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(footer->frame.y, footerY),
                              px("about.footer", footer));
        DOCTEST_CHECK_MESSAGE(
            nearPx(footer->frame.y + footer->frame.height,
                   screen.height - t.sizes.spacingXXLarge),
            px("about.footer", footer));
    }
    {
        const core::window_layout::WindowSize minWin = minWindowForMacWorkArea();
        eui::Ui mini;
        eui::Screen minScreen{minWin.width - 208.0f,
                              static_cast<float>(minWin.height)};
        mini.begin("about_layout_min");
        mod->compose(mini, minScreen);
        mini.end();
        mini.layout(minScreen.width, minScreen.height);
        const auto* c = mini.find("about.card");
        DOCTEST_REQUIRE(c != nullptr);
        DOCTEST_CHECK(c->frame.x >= 0.0f);
        DOCTEST_CHECK(c->frame.x + c->frame.width <= minScreen.width);
        // 窄窗卡宽 clamp 公式镜像（UAT3 宽度自适应）
        DOCTEST_CHECK(nearPx(c->frame.width, cardWidthAt(minScreen.width)));
        DOCTEST_CHECK(nearPx(c->frame.x, (minScreen.width - cardWidthAt(minScreen.width)) / 2.0f));
        const auto* footer = mini.find("about.footer");
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK(nearPx(footer->frame.y + footer->frame.height,
                             minScreen.height - t.sizes.spacingXXLarge));
        DOCTEST_CHECK(c->frame.y + c->frame.height + kFooterGap <= footer->frame.y);
    }
    {
        // 极矮窗 clamp 档：UAT3 cardH=616，窗 640（页高 600）时 cardY 触 0 后
        // cardH 仍超 footer，code 侧第二步 cardH clamp 到 564 使不叠（旧仅推
        // cardY 不行——616 卡高本身已超 footerY=580）
        eui::Ui tiny;
        eui::Screen tinyScreen{1200.0f, 640.0f};
        tiny.begin("about_layout_tiny");
        mod->compose(tiny, tinyScreen);
        tiny.end();
        tiny.layout(tinyScreen.width, tinyScreen.height);
        const auto* card = tiny.find("about.card");
        const auto* footer = tiny.find("about.footer");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(card->frame.y, 0.0f),
                              px("about.card.tiny", card));
        DOCTEST_CHECK(card->frame.y + card->frame.height <= footer->frame.y);
    }
    {
        // UAT3 窄屏宽度自适应：最小窗宽拟降至 1000（content≈712 < 名义 760），
        // 卡宽 clamp 到 min(760, 712−48)=664（>560 下限），居中 x=24、不出屏；
        // 回退成定宽 760 即溢出断言红
        eui::Ui narrow;
        eui::Screen narrowScreen{712.0f, 800.0f};
        narrow.begin("about_layout_narrow");
        mod->compose(narrow, narrowScreen);
        narrow.end();
        narrow.layout(narrowScreen.width, narrowScreen.height);
        const auto* c = narrow.find("about.card");
        DOCTEST_REQUIRE(c != nullptr);
        const float expectW = cardWidthAt(narrowScreen.width);
        DOCTEST_CHECK_MESSAGE(nearPx(c->frame.width, expectW),
                              px("about.card.narrow", c));
        DOCTEST_CHECK(c->frame.x >= 0.0f);
        DOCTEST_CHECK(c->frame.x + c->frame.width <= narrowScreen.width);
        DOCTEST_CHECK_MESSAGE(
            nearPx(c->frame.x, (narrowScreen.width - expectW) / 2.0f),
            px("about.card.narrow.x", c));
    }
    mod->onStop();
    return true;
})

// W10：渐变光晕已移除——反证断言（光斑/扫带/ring 全部 nullptr + bg 无 onTimer）
REGISTER_TEST(uiAboutGlow,
              "about: W10 光晕移除反证—glow/sweep/ring 元素不存在 + bg 无 onTimer/onMove", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    eui::Ui ui;
    eui::Screen screen{1392.0f, 1000.0f};
    ui.begin("about_glow_removed");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    // 反证①：光斑元素全部不存在（6 光斑 × 3 层 = 18 个）
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < 3; ++k) {
            DOCTEST_CHECK(ui.find("about.glow." + std::to_string(i) + "." +
                                  std::to_string(k)) == nullptr);
        }
    }
    // 反证②：扫带元素不存在（2 半）
    for (int half = 0; half < 2; ++half) {
        DOCTEST_CHECK(ui.find("about.sweep." + std::to_string(half)) == nullptr);
    }
    // 反证③：浮环不存在
    DOCTEST_CHECK(ui.find("about.ring.0") == nullptr);

    // 反证④：about.bg 存在但无 onTimer（无动画驱动）+ 无 onMove（无指针追踪）
    {
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(bg->onTimer == nullptr);
        DOCTEST_CHECK(bg->onMove == nullptr);
    }

    // 反证⑤：about.anim state 不存在（about_anim.h 已清空为空壳）

    // 反证⑥：about.bg 颜色 RGB = wechat.pageBg（RGB 不随效果档位变；档位 alpha
    // 的行为断言见 uiAboutPageBgEffectAlpha）
    {
        const auto& t = ::core::theme::current();
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(std::abs(bg->color.r - t.colors.wechat.pageBg.r) < 1e-3f);
        DOCTEST_CHECK(std::abs(bg->color.g - t.colors.wechat.pageBg.g) < 1e-3f);
        DOCTEST_CHECK(std::abs(bg->color.b - t.colors.wechat.pageBg.b) < 1e-3f);
    }

    // 反证⑦：渐变背景元素不存在（about.bg 无 gradient）
    {
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(!bg->gradient.enabled);
    }

    mod->onStop();
    return true;
})

// UAT2 B 批：about 页底 alpha 随窗口效果档位（core::window_effect_state 单值）——
// 磨砂 0.15 透 backdrop / off 实色 1.0；RGB 恒 pageBg 不随档位变。档位本用例内
// 显式设定（进程级单值，与执行顺序无关），尾档复位 off 不污染后续用例。
REGISTER_TEST(uiAboutPageBgEffectAlpha,
              "about: 页底 alpha 随窗口效果档位（UAT2：磨砂 0.15 / off 1.0；RGB 恒 pageBg）", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    const auto& t = ::core::theme::current();
    const auto& wc = t.colors.wechat;

    core::window_effect_state::setCurrent("frosted");
    {
        eui::Ui ui;
        eui::Screen screen{1392.0f, 1000.0f};
        ui.begin("about_bg_alpha_frosted");
        mod->compose(ui, screen);
        ui.end();
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(std::abs(bg->color.a - 0.15f) < 1e-3f);
        DOCTEST_CHECK(std::abs(bg->color.r - wc.pageBg.r) < 1e-3f);
        DOCTEST_CHECK(std::abs(bg->color.g - wc.pageBg.g) < 1e-3f);
        DOCTEST_CHECK(std::abs(bg->color.b - wc.pageBg.b) < 1e-3f);
    }
    core::window_effect_state::setCurrent("translucent");
    {
        eui::Ui ui;
        eui::Screen screen{1392.0f, 1000.0f};
        ui.begin("about_bg_alpha_translucent");
        mod->compose(ui, screen);
        ui.end();
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(std::abs(bg->color.a - 0.5f) < 1e-3f);
    }
    // 尾档 = off：复位 + 验证实色
    core::window_effect_state::setCurrent("off");
    {
        eui::Ui ui;
        eui::Screen screen{1392.0f, 1000.0f};
        ui.begin("about_bg_alpha_off");
        mod->compose(ui, screen);
        ui.end();
        const auto* bg = ui.find("about.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(std::abs(bg->color.a - 1.0f) < 1e-3f);
    }
    mod->onStop();
    return true;
})


// ---- UAT6：关于页构建时间行标准格式（`年-月-日 时:分:秒@设备名@系统`）----
// 纯函数单测（extern 声明，定义在 about_module.cpp，外部链接无头文件）：
// 防「__DATE__ 原样上屏」（"Oct 21 2026 09:32:11" 不是用户可读格式）与
// 「日域双空格/系统归并错档」两类解析事故。
namespace module::about {
std::string formatBuildStamp(const std::string& date, const std::string& time);
std::string normalizeBuildSystemLabel(const std::string& raw);
}  // namespace module::about

REGISTER_TEST(uiAboutBuildStampFormat,
              "about: UAT6 构建行标准格式——__DATE__/__TIME__ 解析重组 + 系统归并", {
    using module::about::formatBuildStamp;
    using module::about::normalizeBuildSystemLabel;
    // 常规日（日 ≥10）：Oct 21 2026 → 2026-10-21
    DOCTEST_CHECK_EQ(formatBuildStamp("Oct 21 2026", "09:32:11"),
                     std::string("2026-10-21 09:32:11"));
    // 单数字日（__DATE__ 日域双空格填充）："Oct  3 2026" → 2026-10-03
    DOCTEST_CHECK_EQ(formatBuildStamp("Oct  3 2026", "00:01:09"),
                     std::string("2026-10-03 00:01:09"));
    // 12 个月抽 3 验月名映射边界（1 月/12 月/跨年位不动）
    DOCTEST_CHECK_EQ(formatBuildStamp("Jan  1 2031", "23:59:59"),
                     std::string("2031-01-01 23:59:59"));
    DOCTEST_CHECK_EQ(formatBuildStamp("Dec 31 2026", "13:45:00"),
                     std::string("2026-12-31 13:45:00"));
    // 解析失败不造数据：坏月名/坏时间原样回传
    DOCTEST_CHECK_EQ(formatBuildStamp("Foo 21 2026", "09:32:11"),
                     std::string("Foo 21 2026 09:32:11"));
    DOCTEST_CHECK_EQ(formatBuildStamp("Oct 21 2026", "9:32"),
                     std::string("Oct 21 2026 9:32"));
    // 系统归并：Windows build ≥22000 → 11（本机 26200 实况），<22000 → 10
    DOCTEST_CHECK_EQ(normalizeBuildSystemLabel("Windows 10.0.26200"),
                     std::string("Windows 11"));
    DOCTEST_CHECK_EQ(normalizeBuildSystemLabel("Windows 10.0.19045"),
                     std::string("Windows 10"));
    DOCTEST_CHECK_EQ(normalizeBuildSystemLabel("Windows"),
                     std::string("Windows"));
    // Darwin → macOS（附原版本号）；其余系统原样
    DOCTEST_CHECK_EQ(normalizeBuildSystemLabel("Darwin 24.5.0"),
                     std::string("macOS 24.5.0"));
    DOCTEST_CHECK_EQ(normalizeBuildSystemLabel("Linux 6.8.0"),
                     std::string("Linux 6.8.0"));
    return true;
})

REGISTER_TEST(uiAboutBuildLineUiFormat,
              "about: UAT6 构建行 UI 上屏格式锁——value.2 三段 @ 分隔 + 19 字符时间戳", {
    // 防断链事故：纯函数对了但 UI 行没接上（row 值仍写死 "构建 ..." 前缀即红）。
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    eui::Ui ui;
    eui::Screen screen{1392.0f, 1000.0f};
    ui.begin("about_build_line_fmt");
    mod->compose(ui, screen);
    ui.end();
    const auto* buildValue = ui.find("about.card.value.2");   // 行序 2 = 构建时间
    DOCTEST_REQUIRE(buildValue != nullptr);
    const std::string& s = buildValue->text;
    DOCTEST_CHECK_MESSAGE(s.find("构建 ") == std::string::npos,
                          "构建行不应再带「构建 」前缀（标签列已有），实测: " + s);
    // 三段：19 字符时间戳 @ 设备名 @ 系统标签
    const size_t at1 = s.find('@');
    DOCTEST_REQUIRE_MESSAGE(at1 != std::string::npos, "构建行缺第一个 @: " + s);
    const size_t at2 = s.find('@', at1 + 1);
    DOCTEST_REQUIRE_MESSAGE(at2 != std::string::npos, "构建行缺第二个 @: " + s);
    DOCTEST_CHECK_MESSAGE(s.find('@', at2 + 1) == std::string::npos,
                          "构建行应恰两段 @ 分隔，实测: " + s);
    // 第一段 = YYYY-MM-DD hh:mm:ss（19 字符，位 4/7 为 '-'、10 为空格、13/16 为 ':'）
    DOCTEST_REQUIRE_MESSAGE(at1 == 19, "时间戳段应 19 字符，实测 " +
                                           std::to_string(at1) + ": " + s);
    DOCTEST_CHECK_MESSAGE(s[4] == '-' && s[7] == '-' && s[10] == ' ' &&
                              s[13] == ':' && s[16] == ':',
                          "时间戳分段符位不对，实测: " + s);
    DOCTEST_CHECK_MESSAGE(!s.substr(0, 4).empty() && s[0] == '2' &&
                              s[1] >= '0' && s[1] <= '9',
                          "年份段应以 2 开头，实测: " + s);
    // 后两段非空（CMake 注入缺失时为 "unknown" 也算过——不造数据但可见）
    DOCTEST_CHECK_MESSAGE(at2 > at1 + 1, "设备名段为空，实测: " + s);
    DOCTEST_CHECK_MESSAGE(s.size() > at2 + 1, "系统段为空，实测: " + s);
    mod->onStop();
    return true;
})
