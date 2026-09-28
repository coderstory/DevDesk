// tests/test_modules/test_ui_modern.cpp
// UI 现代化（plan 2026-09-28-ui-modernization）无头断言。
// M3-1（2026-09-28）：Home 统计卡换 card + Token 速率 sparkline 接线 +
// 速率采样环护栏。sparkline 数据源细节用例见
// tests/test_core/test_proxy_stats.cpp（proxy_stats_rate_series_*），本文件
// 管「空 → {0.0f}」护栏（防 lineChart 对空 values 填演示数据）与 Home
// compose 接线。
// M2（2026-09-28）：全局过渡接入——md 四态切换过渡（md.root 0.15s OutCubic）、
// dialog/toast 关闭态常驻构建 + opacity 0（EUI dialog/toast 已内建过渡，盘点
// 结论：DevDesk 侧不重复加，本组用例是防「懒守卫回归」的护栏）、todo 卡片
// 落位淡入（拖拽跟手期间不动画）。
#include "doctest.h"
#include "test_support.h"

#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/theme.h"
#include "module_todo/todo_service.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();

namespace {

// 假时钟 RAII：测试结束自动 reset（防污染后续用例），与 test_proxy_stats.cpp
// 同款（确定性速率采样需要假时钟）
struct FakeClock {
    int64_t now = 1000000;
    FakeClock() { core::proxy_stats::setTestClock([this] { return now; }); }
    ~FakeClock() { core::proxy_stats::resetForTest(); }
};

// Todo 模块无头环境（同 test_todo_ui.cpp 的 TestEnv：临时库注册为
// appDatabase + 视图状态文件按用例隔离，无头用例不碰真实 %APPDATA%）
struct TodoEnv : test_support::TestEnv {
    std::filesystem::path statePath;
    std::unique_ptr<module::todo::TodoService> svc;

    TodoEnv() : test_support::TestEnv("devdesk_test_ui_m2_", "", true) {
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

// 无头 compose 一帧，返回构建后的 Ui（调用方负责生命周期）
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
        ui.begin("todo_ui_m2_frame");
        mod->compose(ui, screen);
        ui.end();
    }
};

} // namespace

REGISTER_TEST(uiModernMdViewTransition,
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