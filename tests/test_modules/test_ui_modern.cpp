// tests/test_modules/test_ui_modern.cpp
// UI 现代化（plan 2026-09-28-ui-modernization）无头断言。
// M3-1（2026-09-28）：Home 统计卡换 card + Token 速率 sparkline 接线 +
// 速率采样环护栏。sparkline 数据源细节用例见
// tests/test_core/test_proxy_stats.cpp（proxy_stats_rate_series_*），本文件
// 管「空 → {0.0f}」护栏（防 lineChart 对空 values 填演示数据）与 Home
// compose 接线。
#include "doctest.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <memory>
#include <string>
#include <vector>

namespace {

// 假时钟 RAII：测试结束自动 reset（防污染后续用例），与 test_proxy_stats.cpp
// 同款（确定性速率采样需要假时钟）
struct FakeClock {
    int64_t now = 1000000;
    FakeClock() { core::proxy_stats::setTestClock([this] { return now; }); }
    ~FakeClock() { core::proxy_stats::resetForTest(); }
};

} // namespace

extern "C" ::core::Module* create_home_module();

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