// tests/test_modules/test_home_anim.cpp
// need506 定稿（点阵波纹 × 打字机）：桥缝门控/泵合流/点阵几何降档/打字机状态机边界。
// 跨文件前提：app_fluid.cpp 直编进本目标（tests/CMakeLists 直编清单），home_anim
// 实现随 devdesk_home_anim 链接；配置走 test_support::seedConfigJsonRows。
#include "doctest.h"
#include "test_support.h"

#include "app_sidebar.h"
#include "home_anim_bridge.h"
#include "settings_config.h"
#include "ui_kit/home_anim.h"

#include "core/module_interface.h"
#include "core/router.h"

#include <eui/dsl.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

// 桩 Router：Home 当前页（pumpWanted 门控真路径需要 appRouter 就位）
struct HomeRouterFixture {
    class Stub : public ::core::Module {
    public:
        ::core::PageId pageId() const override { return ::core::PageId::Home; }
        std::string displayName() const override { return "首页"; }
        std::string iconGlyph() const override { return eui::utf8(0xF015); }
        void compose(eui::Ui&, const eui::Screen&) override {}
    };
    Stub home;
    ::core::Router router;
    test_support::ScopedAppRouter scoped;
    HomeRouterFixture() : scoped(&router) { router.registerModule(&home); }
};

constexpr const char* kCfgPrefix = "devdesk_test_homeanim_";

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }

}  // namespace

// ── 桥缝：开关默认/持久化读回/门控（定稿 §5 泵纪律）──
REGISTER_TEST(home_anim_bridge_gate,
              "home_anim: 默认开；库值读回；pumpWanted 随开关/页/减动效翻转", {
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "gate", true);
    HomeRouterFixture routerFx;
    home_anim::resetForTests();
    home_anim::setReducedMotionOverride(std::nullopt);
    DOCTEST_CHECK(home_anim::enabled() == true);  // 缺键默认开（组合动效）

    DOCTEST_CHECK(test_support::seedConfigJsonRows(
        ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(),
        R"({"homeAnimEnabled": false})"));
    home_anim::resetForTests();
    DOCTEST_CHECK_FALSE(home_anim::enabled());  // 库值关 → 读回 false

    home_anim::setEnabled(true);
    DOCTEST_CHECK(::home_anim::pumpWanted());  // 开 + Home 当前 + 窗可见

    home_anim::setEnabled(false);
    DOCTEST_CHECK_FALSE(::home_anim::pumpWanted());  // 关 → 门控 false
    home_anim::setEnabled(true);
    DOCTEST_CHECK(::home_anim::pumpWanted());

    home_anim::setReducedMotionOverride(true);
    DOCTEST_CHECK(home_anim::reducedMotion());
    DOCTEST_CHECK_FALSE(::home_anim::pumpWanted());  // 减动效 → 停帧源（§5 降级）
    home_anim::setReducedMotionOverride(std::nullopt);
    home_anim::resetForTests();
    return true;
})

// ── 泵合流：流质关 + 留白动画开 → 泵活且源自首页；全关 → 泵闲置；切页停 ──
REGISTER_TEST(home_anim_pump_merge,
              "home_anim: 流质关+留白开 → 泵活(首页源)；全关/切页 → 泵停", {
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "pump", true);
    HomeRouterFixture routerFx;
    app::resetFluidBackgroundForTests();
    app::resetFluidPumpSourcesForTests();
    home_anim::resetForTests();
    home_anim::setReducedMotionOverride(std::nullopt);
    app::setFluidWindowVisibleMode(true);
    app::setFluidBackgroundEnabled(false);  // 流质本体关（不动其行为）
    home_anim::setEnabled(true);

    eui::Ui ui;
    {
        eui::Screen screen{1600.0f, 1000.0f, 1.0f};
        ui.begin("pumpframe");
        app::drawFluidBackground(ui, screen);
        ui.end();
    }
    DOCTEST_CHECK_FALSE(app::fluidPumpIdleForTests());  // 泵活着（首页源）
    DOCTEST_CHECK(app::fluidPumpHomeSourceForTests());  // 唯一活源 = 首页

    home_anim::setEnabled(false);
    {
        eui::Screen screen{1600.0f, 1000.0f, 1.0f};
        ui.begin("pumpframe2");
        app::drawFluidBackground(ui, screen);
        ui.end();
    }
    DOCTEST_CHECK(app::fluidPumpIdleForTests());  // 双源全关 → 泵零唤醒

    app::resetFluidPumpSourcesForTests();
    app::resetFluidBackgroundForTests();
    home_anim::resetForTests();
    return true;
})

// ── 点阵几何：降档公式 + 点数上限 + 深档点缀确定性 + 波形值域（§2/§5/格子10）──
REGISTER_TEST(home_anim_dot_grid_geometry,
              "home_anim: 间距降档 28/40/52；点数≤600；波峰 scale 1.22/静 0.6；深点缀帧稳定", {
    const auto& t = ::core::theme::current();
    // 格子10：clamp 公式输出 ≤600（1300 自然密度 → 降档后 ≤600）
    const float sp1 = ui_kit::home_anim::gridSpacingFor(1500.0f, 700.0f);
    DOCTEST_CHECK(sp1 > 28.0f);  // 典型区自然 ~1300 点 → 必降档
    const float sp2 = ui_kit::home_anim::gridSpacingFor(400.0f, 200.0f);
    DOCTEST_CHECK(floatEq(sp2, 28.0f));  // 小区自然密度低 → 基准档
    // 波形值域：波峰 scale = 1.22（45% 相位对角 r=c=0 处），静息远离波峰 = 0.6
    const float crest = ui_kit::home_anim::dotScale(1.53f, 0, 0);   // 1530ms 波峰
    const float rest = ui_kit::home_anim::dotScale(0.1f, 0, 0);
    DOCTEST_CHECK(crest > 1.15f);
    DOCTEST_CHECK(rest < 0.75f);
    // 帧间稳定：同相位两次求值全等（splash 契约同款）
    DOCTEST_CHECK(floatEq(ui_kit::home_anim::dotScale(2.7f, 3, 5),
                          ui_kit::home_anim::dotScale(2.7f, 3, 5)));
    // 头less compose：典型 1500×700 → 元素在树、数量 ≤600、正圆、alpha 值域
    {
        eui::Ui ui;
        ui.begin("grid");
        const bool drawn = ui_kit::home_anim::drawDotGrid(ui, 0, 0, 1500, 700,
                                                          1.5f, 1.0f, t);
        ui.end();
        ui.layout(1500, 700);
        DOCTEST_CHECK(drawn);
        int count = 0;
        float maxAlpha = 0.0f;
        for (int r = 0; r < 30; ++r) {
            for (int c = 0; c < 60; ++c) {
                const auto* dot = ui.find("home.anim.dot." + std::to_string(r) +
                                          "." + std::to_string(c));
                if (dot != nullptr) {
                    ++count;
                    maxAlpha = std::max(maxAlpha, dot->opacity);
                    DOCTEST_CHECK(floatEq(dot->frame.width, dot->frame.height));
                }
            }
        }
        DOCTEST_CHECK(count > 100);
        DOCTEST_CHECK(count <= ui_kit::home_anim::kMaxDots);  // 格子1/2
        DOCTEST_CHECK(maxAlpha <= 0.98f + 0.001f);            // 波峰 alpha 红线
        DOCTEST_CHECK(ui.find("home.anim.dot.0.0") != nullptr);
    }
    return true;
})

// ── 打字机状态机：截断/空列表/单条/轮换推进/减动效直显（§3/§4/格子3-6）──
REGISTER_TEST(home_anim_typer_machine,
              "home_anim: 截断26；空列表占位；单条不轮换；轮换打-停-擦-换；减动效直显", {
    using ui_kit::home_anim::Typer;
    using ui_kit::home_anim::truncateTitle;
    // 截断：>26 码点 → 25 + …
    DOCTEST_CHECK(truncateTitle("short") == "short");
    const std::string long_ = "这是一个非常非常非常非常非常非常非常长的笔记标题数字一二三四五六七八";
    const std::string cut = truncateTitle(long_);
    DOCTEST_CHECK(cut.back() == char(0xA6) || cut.find("…") != std::string::npos);
    DOCTEST_CHECK(cut.size() < long_.size());
    // 空库：模块填文案池后照常轮换（UAT 追加令）；池子 10 条且全部 ≤26 字
    {
        DOCTEST_CHECK(ui_kit::home_anim::kQuotePool[0] != nullptr);
        int qn = 0;
        for (const char* q : ui_kit::home_anim::kQuotePool) {
            DOCTEST_CHECK(ui_kit::home_anim::truncateTitle(q) == q);  // 池内均不过截断线
            ++qn;
        }
        DOCTEST_CHECK(qn == 50);  // 池规模=用户令 2026-11-24
        Typer t;
        for (const char* q : ui_kit::home_anim::kQuotePool) t.items.emplace_back(q);
        DOCTEST_CHECK(t.rotating());
        t.lastTick = 0.0;
        t.tick(1.0);
        DOCTEST_CHECK(t.visibleText().size() >= 10);  // 池条目照常打字
    }
    // 单条：不轮换不擦除（格子5：5s 后文本不变）
    {
        Typer t;
        t.items = {"单条笔记"};
        t.tick(10.0);
        t.tick(15.0);
        DOCTEST_CHECK(t.visibleText() == "单条笔记");
        DOCTEST_CHECK_FALSE(t.rotating());
    }
    // 轮换推进：82ms/字 → 1s 后 ≥10 字显示（中英文一致节奏）
    {
        Typer t;
        t.items = {"abcdefghijk", "second"};
        t.lastTick = 0.0;
        // 20fps 泵帧序列：50ms/帧 × 20 帧 = 1s → ≥11 字打完（防「帧距<步长永不推进」回归）
        for (int f = 1; f <= 20; ++f) t.tick(f * 0.05);
        DOCTEST_CHECK(t.visibleText().size() >= 10);
        DOCTEST_CHECK(t.idx == 0);
        // 减动效直显：staticFull 恒整条（格子6）
        DOCTEST_CHECK(t.staticFull() == "abcdefghijk");
        // 光标跟随（UAT 第 5 件）：打字中间帧光标 x ∈ (行起点, 整行宽)
        {
            const auto& th = ::core::theme::current();
            Typer t2;
            t2.items = {"abcdefghijk", "second"};
            t2.lastTick = 0.0;
            for (int f = 1; f <= 6; ++f) t2.tick(f * 0.05);  // ~5 字已打出
            eui::Ui ui;
            ui.begin("cursorframe");
            // phase=0.2：闪烁相位亮段（fmod<0.55）→ 光标节点在树
            ui_kit::home_anim::drawTyper(ui, 400.0f, 200.0f, 800.0f, t2, 0.2f,
                                         "", false, th);
            ui.end();
            ui.layout(800.0f, 400.0f);
            const auto* cur = ui.find("home.anim.typer.cursor");
            DOCTEST_REQUIRE(cur != nullptr);
            // plateW=min(800*0.7+68,560)=560 → 行起点 x = 400-280+34 = 154
            DOCTEST_CHECK(cur->frame.x > 154.0f);
            DOCTEST_CHECK(cur->frame.x < 154.0f + 560.0f - 68.0f);
        }
    }
    return true;
})
