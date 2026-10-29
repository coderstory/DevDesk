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
        // UAT6 阴影统一反向锁：根修只关按钮（ButtonStyle 默认 flat），卡片
        // 容器的 panelShadow 必须保留——按钮去阴影不得殃及卡片立体感。
        DOCTEST_CHECK(ui.find("home.stat.tokens.bg")->shadow.enabled);
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

// todo 看板右键锚点 = 被右键卡片自身 frame（不依赖 boardGeom 均匀列宽）
REGISTER_TEST(uiModernTodoMenuAnchor,
              "uiModern todo: 卡片右键菜单锚点 = 卡片中芯/卡底（headless）", {
    TodoEnv env;
    DOCTEST_REQUIRE(env.ready);
    // 固定四列：用默认「待办」列（col.0）造一张任务，不新建第 5 列（与固定四列冲突）
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(!cols.value().empty());
    const int64_t col0Id = cols.value()[0].id;
    auto t = env.svc->createTask(col0Id, "任务A");
    DOCTEST_REQUIRE(t.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    {
        TodoFrame frame(mod.get());
        frame.ui.layout(frame.screen);  // 卡片/列 frame 需 layout 后才有几何
        const auto* col0 = frame.ui.find("todo.col.0.bg");
        DOCTEST_REQUIRE(col0 != nullptr);
        const auto* card = frame.ui.find("todo.col.0.card.0.bg");
        DOCTEST_REQUIRE(card != nullptr);
        // 锚点取真实列几何（归档折叠时列宽随动画变化）：x = 列中芯、y = 卡片视觉底。
        // 菜单在卡底下方留 4px 视觉间隙（drawContextMenu 的 +4.0f）。
        const float expectX = col0->frame.x + col0->frame.width * 0.5f;
        const float expectY = card->frame.y + card->frame.height + 4.0f;
        const float cardX = card->frame.x;
        const float cardW = card->frame.width;
        const float col0X = col0->frame.x;
        const float col0W = col0->frame.width;
        eui::PointerEvent pe;
        eui::Rect rect;
        card->onContextMenu(pe, rect);
        frame.refresh();  // 重 compose：菜单已开，menu->x/y 在 compose 期落位
        const auto* menu = frame.ui.find("todo.ctx.level.0");
        DOCTEST_REQUIRE(menu != nullptr);
        DOCTEST_CHECK_MESSAGE(menu->x == expectX,
            ("menu.x=" + std::to_string(menu->x) +
             " menu.y=" + std::to_string(menu->y) +
             " expectX=" + std::to_string(expectX) +
             " expectY=" + std::to_string(expectY) +
             " card.x=" + std::to_string(cardX) +
             " card.w=" + std::to_string(cardW) +
             " col0.x=" + std::to_string(col0X) +
             " col0.w=" + std::to_string(col0W)).c_str());
        DOCTEST_CHECK_MESSAGE(menu->y == expectY,
            ("menu.x=" + std::to_string(menu->x) +
             " menu.y=" + std::to_string(menu->y) +
             " expectX=" + std::to_string(expectX) +
             " expectY=" + std::to_string(expectY) +
             " card.x=" + std::to_string(cardX) +
             " card.w=" + std::to_string(cardW) +
             " col0.x=" + std::to_string(col0X) +
             " col0.w=" + std::to_string(col0W)).c_str());
    }
    // 列头 onContextMenu 已随固定四列删除（H20），不再测列头右键。

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
    // slot 窗口生效：25 行不整段渲染，视口外行不存在（virtualList 裁剪）。
    // 1600×1000 下列表视口 782px、行距 68px、overscan 一屏 → 渲染窗 24 行：
    // 行 23 在窗内，行 24（最后一帧可见区外）被裁掉。
    DOCTEST_CHECK(ui.find("claude.profile.23.name") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.23") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.24.name") == nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.24") == nullptr);

    // 滚动回退语义：onScrollOffsetChanged 写 profilesScrollOffset_ →
    // 重 compose 视口随偏移前移。偏移取 600 让行 19 落入渲染窗（56px×19≈1064）
    const auto* root = ui.find("claude.profiles.scroll");
    DOCTEST_REQUIRE(root != nullptr);
    DOCTEST_REQUIRE(root->onScrollOffsetChanged != nullptr);
    root->onScrollOffsetChanged(600.0f);
    compose();
    DOCTEST_CHECK(ui.find("claude.profiles.scroll.slot.0") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.19.name") != nullptr);
    // 行命中节点 .bg 保留（选中/右键挂点行内）。offset 600 下第 0 行已被 slot
    // 窗口裁掉，故在已渲染的行 19 上验 .bg
    DOCTEST_CHECK(ui.find("claude.profile.19.bg") != nullptr);

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
        // 相对柱高 == 相对 token 数（H18 纵向弹性 1200×720 档，fs=1.0；批L 顶距后垂直内边距 0）：
        // baseContentH = 页头40 + 统计96（5694df44 瘦身 144→96）+ gap×3(60) + 图220 + 操作44 + 快捷底边距16 = 476；
        // extraH = 720 − 476 − 0.5 = 243.5；statsGrow = 243.5×0.18 = 43.83；
        // chartH = 220 + 243.5 − 43.83 = 419.67 → wrap = 419.67 − 64 = 355.67
        // → plot = 355.67 − 112 = 243.67。
        // 峰值柱（600）= plot，100/600 柱 = plot/6 ≈ 40.612，300/600 柱 = plot/2 = 121.835。
        constexpr float kElasticPlotH = 243.67f;
        const auto* bar0 = ui.find("home.api.chart.bars.bar.0");
        const auto* bar3 = ui.find("home.api.chart.bars.bar.3");
        const auto* bar8 = ui.find("home.api.chart.bars.bar.8");
        DOCTEST_REQUIRE(bar0 != nullptr);
        DOCTEST_REQUIRE(bar3 != nullptr);
        DOCTEST_REQUIRE(bar8 != nullptr);
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar8->frame.height - kElasticPlotH) < 0.5f,
            "bar8 h=" + std::to_string(bar8->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar0->frame.height - kElasticPlotH / 6.0f) < 0.5f,
            "bar0 h=" + std::to_string(bar0->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar3->frame.height - kElasticPlotH / 2.0f) < 0.5f,
            "bar3 h=" + std::to_string(bar3->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar0->frame.height - kElasticPlotH / 6.0f) < 0.5f,
            "bar0 h=" + std::to_string(bar0->frame.height) + " y=" +
                std::to_string(bar0->frame.y));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bar3->frame.height - kElasticPlotH / 2.0f) < 0.5f,
            "bar3 h=" + std::to_string(bar3->frame.height) + " y=" +
                std::to_string(bar3->frame.y));
        // 柱高对齐基线：三根柱底沿相等（y + height 同值）
        DOCTEST_CHECK(std::fabs((bar0->frame.y + bar0->frame.height) -
                                (bar8->frame.y + bar8->frame.height)) < 0.5f);
        // 卡片数字 = 最近一次成功调用的速率（150.0），副标题不再写「累计」
        DOCTEST_CHECK_EQ(textOf(ui, "home.stat.tokens.number"),
                         std::string("150.0"));
        // 5694df44 起 badge 并入 label 同行；窄卡超宽时丢 badge（弱信息，
        // 文案全量在 home 域宽窗用例守），这里钉「不冒充累计」
        DOCTEST_CHECK(textOf(ui, "home.stat.tokens.label").find("累计") ==
                      std::string::npos);
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
        // W8：渐变已移除（§8 #4 禁大面积渐变），改为 flat background。
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
              "about: 批J 平铺版——页标题+分组直铺页面底（无卡片）/ 分组行几何 / footer / 平铺内容不越界", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    // 与 about_module.cpp 同值镜像（批J 平铺：页边距 32 / 标题 32 / 组头 24 / 行 32 / 组间 24）
    constexpr float kPagePad = 32.0f;   // 水平边距
    constexpr float kPageTop = 0.0f;    // 顶距：标题顶格（批L）
    constexpr float kTitleH = 32.0f;
    constexpr float kGroupHdrH = 24.0f;
    constexpr float kGroupGap = 24.0f;
    constexpr float kRowH = 32.0f;
    constexpr int kGroupCount = 3;
    constexpr int kRowCount = 9;   // 3+4+2=9 行
    constexpr float kFooterH = 20.0f;
    constexpr float kLabelW = 100.0f;
    const float valueX = kPagePad + kLabelW + 16.0f;

    // 组 gi 的组头 y（平铺流式：首组顶 = 页边距+标题+组间距；组间 = 组头+行+组间距）
    auto flatY = [&](int gi) {
        float y = kPageTop + kTitleH + kGroupGap;
        for (int g = 0; g < gi; ++g)
            y += kGroupHdrH + (g == 0 ? 3 : 4) * kRowH + kGroupGap;
        return y;
    };
    const float lastRowBottom = kPageTop + kTitleH + kGroupGap +
                                kGroupCount * kGroupHdrH + kRowCount * kRowH +
                                (kGroupCount - 1) * kGroupGap;  // 末组后无组间距

    eui::Ui ui;
    eui::Screen screen{1600.0f, 1000.0f};
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
        // 页标题（同 agent.title 款式）
        const auto* title = ui.find("about.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.x, kPagePad), px("about.title.x", title));
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.y, kPageTop), px("about.title.y", title));
        DOCTEST_CHECK_MESSAGE(nearPx(title->frame.width, screen.width - 2.0f * kPagePad),
                              px("about.title.w", title));
        DOCTEST_CHECK(title->text == "关于");
    }
    {
        // 卡片载体不得回潮（批J 去卡片化）
        DOCTEST_CHECK(ui.find("about.card") == nullptr);
        DOCTEST_CHECK(ui.find("about.card.bg") == nullptr);
        // Logo 时代残留仍不存在
        DOCTEST_CHECK(ui.find("about.card.icon") == nullptr);
        DOCTEST_CHECK(ui.find("about.card.title") == nullptr);
    }
    {
        const float contentW = screen.width - 2.0f * kPagePad;
        const int gRowCount[kGroupCount] = {3, 4, 2};
        for (int gi = 0; gi < kGroupCount; ++gi) {
            for (int i = 0; i < gRowCount[gi]; ++i) {
                const std::string sid = std::to_string(gi) + "." + std::to_string(i);
                const auto* label = ui.find("about.info.label." + sid);
                const auto* value = ui.find("about.info.value." + sid);
                DOCTEST_REQUIRE(label != nullptr);
                DOCTEST_REQUIRE(value != nullptr);
                const float rowY = flatY(gi) + kGroupHdrH + i * kRowH;
                DOCTEST_CHECK_MESSAGE(nearPx(label->frame.x, kPagePad),
                                      px(("about.info.label." + sid).c_str(), label));
                DOCTEST_CHECK_MESSAGE(nearPx(label->frame.y, rowY),
                                      px(("about.info.label." + sid).c_str(), label));
                DOCTEST_CHECK_MESSAGE(nearPx(value->frame.x, valueX),
                                      px(("about.info.value." + sid).c_str(), value));
                DOCTEST_CHECK_MESSAGE(nearPx(value->frame.y, rowY),
                                      px(("about.info.value." + sid).c_str(), value));
            }
            if (gi > 0) {
                const auto* div = ui.find("about.info.divider." + std::to_string(gi));
                DOCTEST_REQUIRE(div != nullptr);
                DOCTEST_CHECK_MESSAGE(nearPx(div->frame.y, flatY(gi) - kGroupGap * 0.5f - 0.5f),
                                      px(("about.info.divider." + std::to_string(gi)).c_str(), div));
                DOCTEST_CHECK_MESSAGE(nearPx(div->frame.width, contentW),
                                      px(("about.info.divider." + std::to_string(gi)).c_str(), div));
            }
        }
        // 扁平旧 id 不得回潮（批B 前的跨组覆盖载体）
        DOCTEST_CHECK(ui.find("about.info.label.0") == nullptr);
        DOCTEST_CHECK(ui.find("about.info.value.0") == nullptr);
        // 复制图标/命中区仅组0 highlight 行存在
        DOCTEST_REQUIRE(ui.find("about.info.copy.0.0") != nullptr);
        DOCTEST_REQUIRE(ui.find("about.info.copy.0.1") != nullptr);
        DOCTEST_REQUIRE(ui.find("about.info.copy.0.2") != nullptr);
        DOCTEST_REQUIRE(ui.find("about.info.hit.0.0") != nullptr);
        DOCTEST_REQUIRE(ui.find("about.info.hit.0.1") != nullptr);
        DOCTEST_REQUIRE(ui.find("about.info.hit.0.2") != nullptr);
        DOCTEST_CHECK(ui.find("about.info.copy.1.0") == nullptr);
        DOCTEST_CHECK(ui.find("about.info.hit.1.0") == nullptr);
        // 开源组件行 Ghost 链接热区
        DOCTEST_REQUIRE(ui.find("about.info.link.2.1") != nullptr);
    }
    {
        const auto* footer = ui.find("about.footer");
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(footer->frame.y, screen.height - kPagePad - kFooterH),
                              px("about.footer", footer));
        DOCTEST_CHECK_MESSAGE(
            nearPx(footer->frame.y + footer->frame.height, screen.height - kPagePad),
            px("about.footer", footer));
        DOCTEST_CHECK(lastRowBottom <= footer->frame.y);  // 顶格后内容仍不叠 footer
    }
    {
        // 最小窗：平铺内容固定高（lastRowBottom=496），640 窗不叠 footer、不出屏
        const core::window_layout::WindowSize minWin = minWindowForMacWorkArea();
        eui::Ui mini;
        eui::Screen minScreen{minWin.width - 208.0f,
                              static_cast<float>(minWin.height)};
        mini.begin("about_layout_min");
        mod->compose(mini, minScreen);
        mini.end();
        mini.layout(minScreen.width, minScreen.height);
        const auto* title = mini.find("about.title");
        const auto* footer = mini.find("about.footer");
        const auto* lastValue = mini.find("about.info.value.2.1");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_REQUIRE(footer != nullptr);
        DOCTEST_REQUIRE(lastValue != nullptr);
        DOCTEST_CHECK(title->frame.x >= kPagePad - 0.5f);
        DOCTEST_CHECK_MESSAGE(nearPx(lastValue->frame.y + lastValue->frame.height, lastRowBottom),
                              px("about.min.lastRow", lastValue));
        DOCTEST_CHECK(lastRowBottom <= footer->frame.y);  // 内容不叠 footer
        DOCTEST_CHECK(nearPx(footer->frame.y + footer->frame.height,
                             minScreen.height - kPagePad));
    }
    {
        // 窄窗 712：内容宽收缩，行不溢出屏幕
        eui::Ui narrow;
        eui::Screen narrowScreen{712.0f, 800.0f};
        narrow.begin("about_layout_narrow");
        mod->compose(narrow, narrowScreen);
        narrow.end();
        narrow.layout(narrowScreen.width, narrowScreen.height);
        const auto* v = narrow.find("about.info.value.2.1");
        const auto* link = narrow.find("about.info.link.2.1");  // 开源组件行 = 链接热区（非复制行）
        DOCTEST_REQUIRE(v != nullptr);
        DOCTEST_REQUIRE(link != nullptr);
        DOCTEST_CHECK_MESSAGE(nearPx(v->frame.width,
                                     (narrowScreen.width - 2.0f * kPagePad) - 116.0f - 24.0f),
                              px("about.narrow.value", v));
        DOCTEST_CHECK(v->frame.x + v->frame.width <= narrowScreen.width);
        DOCTEST_CHECK(link->frame.x + link->frame.width <= narrowScreen.width);
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

    // 反证④（批K）：about.bg 自铺底已拆（统一不自铺底），onTimer/onMove 断言随之作废
    DOCTEST_CHECK(ui.find("about.bg") == nullptr);

    // 反证⑤：about.anim state 不存在（about_anim.h 已清空为空壳）

    // 反证⑥⑦（批K）：about 页不再自铺底色（UAT2 磨砂时代遗留已拆）——与其他页
    // 统一走主窗 clearColor，about.bg 元素不得存在，渐变载体随之消失

    mod->onStop();
    return true;
})

// 批K（2026-10-25 用户拍板「背景色的动态不协调」）：about 页与其他页统一——
// 不自铺底色（home/markdown 同规注释：页面底色 = 主窗口 clearColor，半透/磨砂档
// 由窗口合成透出）。UAT2 的页底 alpha 随档位动态行为随自铺底一并拆除，本用例
// 反向锁「不得回潮」。
REGISTER_TEST(uiAboutPageBgEffectAlpha,
              "about: 页面不自铺底色（与其他页统一走主窗 clearColor，UAT2 自铺 alpha 已拆）", {
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    // 两个效果档位下都不许出现自铺底
    for (const char* tier : {"frosted", "off"}) {
        core::window_effect_state::setCurrent(tier);
        eui::Ui ui;
        eui::Screen screen{1392.0f, 1000.0f};
        ui.begin(std::string("about_no_bg_") + tier);
        mod->compose(ui, screen);
        ui.end();
        DOCTEST_CHECK_MESSAGE(ui.find("about.bg") == nullptr,
                              std::string("about.bg 自铺底在档位 ") + tier +
                                  " 下不得存在（统一不自铺底）");
    }
    core::window_effect_state::setCurrent("off");  // 尾档复位不污染后续用例
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
              "about: S3.8 分组批B修复后 value.1.3 = C++ 标准行 + 构建时间行存活（ID 组维度验证）", {
    // S3.8 三组：版本信息 3 行 / 构建信息 4 行（构建时间/编译器/平台/C++ 标准）/ 许可与致谢 2 行。
    // 批B 修复前：构建时间行因跨组 id 重复被许可组覆盖（该 bug 已修，本用例锁定修复行为）。
    std::unique_ptr<::core::Module> mod(create_about_module());
    DOCTEST_REQUIRE(mod != nullptr);
    mod->onStart();
    eui::Ui ui;
    eui::Screen screen{1392.0f, 1000.0f};
    ui.begin("about_cpp_line_fmt");
    mod->compose(ui, screen);
    ui.end();
    const auto* buildTime = ui.find("about.info.value.1.0");   // 构建信息组行0 = 构建时间
    DOCTEST_REQUIRE(buildTime != nullptr);                     // 批B 前此行被覆盖不可查
    const auto* cppValue = ui.find("about.info.value.1.3");    // S3.8: 构建信息组行3 = C++ 标准
    DOCTEST_REQUIRE(cppValue != nullptr);
    const std::string& s = cppValue->text;
    DOCTEST_CHECK_MESSAGE(s.rfind("C++", 0) == 0,
                          "C++ 标准行应以 C++ 开头，实测: " + s);
    DOCTEST_CHECK_MESSAGE(s.size() >= 5 && s.size() <= 6,
                          "C++ 标准应为 C++NN 格式（5-6 字符），实测: " + s);
    mod->onStop();
    return true;
})
