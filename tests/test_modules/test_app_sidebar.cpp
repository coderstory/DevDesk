// tests/test_modules/test_app_sidebar.cpp
// 侧边栏单测：sidebarCollapsed 持久化 / setSidebarCollapsed 内存态+落盘 / 无头 compose 几何 /
// 页面区原点写入 Router / 注册表变化的当帧反映。
// 跨文件前提：
//   - 配置落在 app_config 表，生产代码经 core::appDatabase() 取连接；TestEnv 必须 registerAppDb，
//     否则 store.save() 因「库未就绪」如实失败。
//   - app::setSidebarCollapsed 是进程级全局态，改动会跨用例存活，用例尾部必须复位。
//   - app::compose 只建树不排版；读任何 frame 之前必须自己调 ui.layout()，否则 frame 全是 0。
#include "doctest.h"
#include "test_support.h"

#include "app_sidebar.h"
#include "terminal_fullscreen_bridge.h"  // 全屏 0 宽用例：桥 handler 注入（形态同生产装配）
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"  // 顶部留白断言直接比 spacingXXLarge token
#include "core/window_effect_state.h"  // 侧栏底 alpha 按窗口效果档位（UAT2 B 批）

#include <eui/dsl.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>  // need172 粒子生命期（480ms）壁钟快进

namespace {

// 桩模块：compose 记录拿到的 Screen（内容区宽度跟随断言用）
class RecordingModule : public ::core::Module {
public:
    RecordingModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return eui::utf8(0xF015); }
    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        lastScreenWidth = screen.width;
        lastScreenHeight = screen.height;
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }

    static float lastScreenWidth;
    static float lastScreenHeight;

private:
    ::core::PageId page_;
    std::string name_;
};

float RecordingModule::lastScreenWidth = 0.0f;
float RecordingModule::lastScreenHeight = 0.0f;
// 每个用例独立的临时配置文件，统一走 test_support::tempConfigFile 工厂
constexpr const char* kCfgPrefix = "devdesk_test_sidebar_";

// 手改过的配置文本按顶层键分发成 app_config 表的各行。
// 返回 bool：doctest 断言宏自带 `return false`，void 会编译不过（-Wreturn-mismatch = -Werror）。
bool writeRawConfig(const std::string& json) {
    return test_support::seedConfigJsonRows(
        ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(),
        json);
}

// 桩 Router（Home = 当前页，compose 落到 RecordingModule）
::core::Router makeStubRouter(RecordingModule& home, RecordingModule& md) {
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&md);
    return router;
}

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }

// 动态背景（UAT6 主窗活体流质）测试共用的装配：桩 Router + 可见态钉住。
// headless 下平台可见查询按「可见」降级（EnumWindows 无命中），用例按钉住值
// 锁语义，不随测试运行平台漂移。
struct FluidComposeFixture {
    RecordingModule home{::core::PageId::Home, "首页"};
    RecordingModule md{::core::PageId::Markdown, "笔记"};
    ::core::Router router;
    test_support::ScopedAppRouter scoped;
    eui::Screen screen;
    eui::Ui ui;
    int frames = 0;

    FluidComposeFixture()
        : router(makeStubRouter(home, md)), scoped(&router) {
        screen.width = 1600.0f;
        screen.height = 1000.0f;
    }

    void frame() {
        ui.begin("fluid" + std::to_string(frames++));
        app::compose(ui, screen);
        ui.end();
        // compose 只建树不排版（本文件头部前提）：读任何 frame 前必须 layout
        ui.layout(screen.width, screen.height);
    }
};

} // namespace

// macOS：内容区延伸进标题栏（FSCV）后，系统标题栏那一条（红绿灯）浮在窗口左上角，
// 内容需让出它 —— 否则压住侧栏品牌块。Windows 走自绘标题栏，自带 40px 占位。
#ifdef __APPLE__
constexpr float kExpectedBodyTop = 30.0f;
#else
constexpr float kExpectedBodyTop = 0.0f;
#endif

REGISTER_TEST(sidebar_config_default_and_roundtrip,
              "sidebar: sidebarCollapsed 默认 false + 存取往返", {
    test_support::TestEnv cfgDbEnv("devdesk_test_sidebar_", "sbcfg1", /*registerAppDb=*/true);
    const std::filesystem::path file = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    const module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    module::settings::SettingsConfig cfg = store.load();
    cfg.sidebarCollapsed = true;
    DOCTEST_CHECK(store.save(cfg).isOk());
    DOCTEST_CHECK(store.load().sidebarCollapsed);
    cfg = store.load();
    cfg.sidebarCollapsed = false;
    DOCTEST_CHECK(store.save(cfg).isOk());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    return true;
})

REGISTER_TEST(sidebar_config_legacy_missing_key,
              "sidebar: 老配置无 sidebarCollapsed 键 → 默认展开", {
    test_support::TestEnv cfgDbEnv("devdesk_test_sidebar_", "sbcfg2", /*registerAppDb=*/true);
    const std::filesystem::path file = test_support::tempConfigFile(kCfgPrefix, "legacy");
    DOCTEST_REQUIRE(writeRawConfig(R"({"theme": "light"})"));
    const module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    DOCTEST_REQUIRE(writeRawConfig(
        R"({"theme": "light", "sidebarCollapsed": true})"));
    DOCTEST_CHECK(store.load().sidebarCollapsed);
    return true;
})

REGISTER_TEST(sidebar_toggle_persists,
              "sidebar: setSidebarCollapsed 翻内存态并落盘（保留其他字段）", {
    test_support::TestEnv cfgDbEnv("devdesk_test_sidebar_", "sbcfg3", /*registerAppDb=*/true);
    const std::filesystem::path file = test_support::tempConfigFile(kCfgPrefix, "toggle");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    // 预置一份带主题的配置，折叠切换后主题必须原样保留
    module::settings::SettingsStore store(::core::appDatabase());
    module::settings::SettingsConfig base;
    base.darkTheme = false;
    DOCTEST_CHECK(store.save(base).isOk());

    app::setSidebarCollapsed(true, true);
    DOCTEST_CHECK(app::sidebarCollapsed());
    {
        const auto cfg = store.load();
        DOCTEST_CHECK(cfg.sidebarCollapsed);
        DOCTEST_CHECK_FALSE(cfg.darkTheme);
    }

    app::setSidebarCollapsed(false, true);
    DOCTEST_CHECK_FALSE(app::sidebarCollapsed());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);

    // 落盘路径为空 = 只改内存；这是启动恢复路径（配置尚未就绪时）
    app::setSidebarCollapsed(true);
    DOCTEST_CHECK(app::sidebarCollapsed());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    // 复位：sidebarCollapsed 是进程级全局态，不还原会污染后续用例
    app::setSidebarCollapsed(false);
    return true;
})

// ── 动态背景（UAT6 主窗活体流质）──
// 锁三层：① 开关开+窗可见 → 4 层晕 + 每层 2 卫星 + onFrame 帧泵在树（正圆手法
// 与强度保守上限）；② 相位钟只在泵回调推进（dt 钳 0.1s，模态循环/休眠不跳变）；
// 「不挂泵不推进」的不可见侧见 fluid_background_gate_zero_cost。
REGISTER_TEST(fluid_background_compose_elements,
              "fluid: 开关开+窗可见 → 14 晕+卫星+帧泵在树；相位只在泵回调推进", {
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "fluidon", true);
    app::resetFluidBackgroundForTests();       // 隔离前序用例的进程级镜像
    app::setFluidWindowVisibleMode(true);      // 钉可见（语义与平台查询解耦）
    app::setFluidBackgroundEnabled(true);

    FluidComposeFixture fx;
    fx.frame();

    // 14 层晕 + 每层 3 卫星（B5 2026-11-02：kHalos 7→14，卫星总数 42）
    const int kHalos = 14;
    for (int j = 0; j < kHalos; ++j) {
        const auto* halo = fx.ui.find("app.fluid.halo." + std::to_string(j));
        DOCTEST_REQUIRE_MESSAGE(halo != nullptr, "缺晕 halo." + std::to_string(j));
        // 正圆（splash 墨染同款手法）：宽=高=2r、radius=r
        DOCTEST_CHECK(floatEq(halo->frame.width, halo->frame.height));
        DOCTEST_CHECK(floatEq(halo->radius, halo->frame.width * 0.5f));
        // kMaxAlpha 0.19（重构前 0.11 叠上半透卡面后透不出色，玻璃感失效）
        DOCTEST_CHECK(halo->opacity <= 0.19f + 0.001f);
        for (int b = 0; b < 3; ++b) {
            DOCTEST_CHECK(fx.ui.find("app.fluid.sat." + std::to_string(j) +
                                      "." + std::to_string(b)) != nullptr);
        }
    }
    // 数量判据：恰好 14 晕，第 15 个不存在
    DOCTEST_CHECK(fx.ui.find("app.fluid.halo." + std::to_string(kHalos)) == nullptr);
    const auto* driver = fx.ui.find("app.fluid.anim.driver");
    DOCTEST_REQUIRE(driver != nullptr);
    DOCTEST_REQUIRE(driver->onFrame != nullptr);

    // 相位钟由壁钟驱动（H13 fix）：onFrame 泵仍在树提供帧回调，但相位不依赖 dt 累加
    const float before = app::fluidPhaseSeconds();
    driver->onFrame(0.04f);
    driver->onFrame(2.0f);
    const float after = app::fluidPhaseSeconds();
    DOCTEST_CHECK(after >= before);
    DOCTEST_CHECK(before > 0.0f);

    app::resetFluidBackgroundForTests();  // 复位：进程级状态不还原会污染后续用例
    return true;
})

// 铁律门控（用户拍板：最小化/藏托盘时帧泵必须停）：不可见帧零元素零泵零推进，
// 恢复可见元素与泵回归（相位从冻结处继续）；开关关 = 同款零成本。
// 平台层的零成本第一道由 EUI 主循环保证（glfw_app_main hiddenToTray/iconified
// 两分支不跑主窗 update/compose/render），本用例锁第二道：即便 compose 被调用
//（测试直泵路径），门控也不建元素不挂泵。
REGISTER_TEST(fluid_background_gate_zero_cost,
              "fluid: 窗不可见/开关关 → 零元素零泵零推进；恢复可见继续（铁律）", {
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "fluidgate", true);
    app::resetFluidBackgroundForTests();
    app::setFluidBackgroundEnabled(true);

    FluidComposeFixture fx;

    // 不可见（最小化/藏托盘语义钉住）：整树不建（含泵）
    app::setFluidWindowVisibleMode(false);
    fx.frame();
    DOCTEST_CHECK(fx.ui.find("app.fluid.halo.0") == nullptr);
    DOCTEST_CHECK(fx.ui.find("app.fluid.sat.0.0") == nullptr);
    DOCTEST_CHECK(fx.ui.find("app.fluid.anim.driver") == nullptr);

    // 恢复可见：元素与帧泵回归；不可见期间相位零推进（从冻结处继续）
    const float phaseFrozen = app::fluidPhaseSeconds();
    app::setFluidWindowVisibleMode(true);
    fx.frame();
    DOCTEST_CHECK(fx.ui.find("app.fluid.halo.0") != nullptr);
    const auto* driver = fx.ui.find("app.fluid.anim.driver");
    DOCTEST_REQUIRE(driver != nullptr);
    DOCTEST_REQUIRE(driver->onFrame != nullptr);
    DOCTEST_CHECK(floatEq(app::fluidPhaseSeconds(), phaseFrozen));

    // 开关关：同样零成本（背景恢复静态 = 流质整体不画）
    app::setFluidBackgroundEnabled(false);
    fx.frame();
    DOCTEST_CHECK(fx.ui.find("app.fluid.halo.0") == nullptr);
    DOCTEST_CHECK(fx.ui.find("app.fluid.anim.driver") == nullptr);

    app::resetFluidBackgroundForTests();
    return true;
})

REGISTER_TEST(sidebar_collapsed_compose,
              "sidebar: 折叠态 72px、label 隐藏、图标居中、tooltip、内容区 +136", {
    // UAT3 D-R3-1：compose 几何断言必须确定性，不随测试运行平台漂移——本文件
    // 既有用例一律钉「系统标题栏路径」（customTitlebarActive=false = macOS 实际
    // 运行态、几何与旧版一致）；自绘标题栏路径由 sidebar_custom_titlebar_compose 钉 true 专测。
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(true);  // 空落盘路径：不能动真实用户配置
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_collapsed");
    app::compose(ui, screen);
    ui.end();

    // 侧边栏宽 72；分隔线落在 x=71（宽 1，贴右边）
    const auto* sidebar = ui.find("app.sidebar");
    DOCTEST_CHECK(sidebar != nullptr);
    DOCTEST_CHECK(floatEq(sidebar->width.value, 72.0f));
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.divider")->x, 71.0f));

    // brand 折叠 = 只显示居中几何 logo（图标批 2026-10-20 起 fa-laptop-code 字形退役，
    // 换 ui_kit::drawDevDeskLogo 矢量瓦，几何契约同 assets/icon.ico）。行 box 是
    // (0, 40, 72, 56)；§10.1 logo 三档：H=1000 落正常档 → 48，两态一致——
    // .mark 行内居中：(72-48)/2=12、(56-48)/2=4（上下各留 4px）
    const auto* collapsedBrand = ui.find("app.sidebar.brand");
    DOCTEST_CHECK(collapsedBrand != nullptr);
    const auto* collapsedMark = ui.find("app.sidebar.brand.mark");
    DOCTEST_CHECK(collapsedMark != nullptr);
    DOCTEST_CHECK(floatEq(collapsedMark->x, 12.0f));  // (72-48)/2
    DOCTEST_CHECK(floatEq(collapsedMark->y, 4.0f));   // (56-48)/2
    DOCTEST_CHECK(floatEq(collapsedMark->width.value, 48.0f));

    // 顶部留白 = spacingLarge×2(32)，与页面主区上 padding 同档（S4.2 弹性）；折叠/展开两态必须同值，
    // 否则切换时 logo 会纵向跳
    DOCTEST_CHECK(floatEq(collapsedBrand->y, 32.0f));
    const auto* firstHit = ui.find("app.sidebar.item.Home.hit");
    DOCTEST_CHECK(firstHit != nullptr);
    // §11：导航行进滚动画布，builder y = 画布坐标（= 旧侧栏 y − navStart 116）；
    // 首行贴视口顶（offset=0）→ 画布 y=0，侧栏坐标仍 32+56+28=116
    DOCTEST_CHECK(floatEq(firstHit->y, 0.0f));

    // 折叠态不渲染 label（模块名只走 tooltip）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.label") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Markdown.label") == nullptr);

    // 图标在 72 内水平居中：(72 - 28) / 2
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 22.0f));

    // 折叠开关 need172 v2（§10.2）：正常档直径 32 正圆（radius=16）、常态 1px
    // 圆框描边、48 预留区内垂直居中 toggleY=H−40；折叠态水平居中：(72-32)/2=20。
    // 热区 = 绘制区外扩 4px（§10.2）：40×40，交互回调所在
    const auto* toggle = ui.find("app.sidebar.toggle");
    DOCTEST_CHECK(toggle != nullptr);
    DOCTEST_CHECK(floatEq(toggle->x, 20.0f));
    DOCTEST_CHECK(floatEq(toggle->y, 960.0f));  // 1000 − 40
    DOCTEST_CHECK(floatEq(toggle->width.value, 32.0f));
    DOCTEST_CHECK(floatEq(toggle->height.value, 32.0f));
    DOCTEST_CHECK(floatEq(toggle->radius, 16.0f));
    DOCTEST_CHECK(floatEq(toggle->border.width, 1.0f));
    DOCTEST_CHECK(floatEq(toggle->border.color.r,
                          ::core::theme::current().colors.borderDefault.r));
    const auto* hot = ui.find("app.sidebar.toggle.hot");
    DOCTEST_REQUIRE(hot != nullptr);
    DOCTEST_CHECK(floatEq(hot->x, 16.0f));  // 20 − 4
    DOCTEST_CHECK(floatEq(hot->y, 956.0f));  // 960 − 4
    DOCTEST_CHECK(floatEq(hot->width.value, 40.0f));  // 32 + 8
    DOCTEST_CHECK(floatEq(hot->height.value, 40.0f));
    DOCTEST_CHECK(hot->onClick != nullptr);  // 点击回调在热区（视觉缩小不损命中）

    // 模块内容区宽度跟随侧边栏实际占宽
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight,
                          1000.0f - kExpectedBodyTop));

    app::setSidebarCollapsed(false);  // 复位进程级全局态
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

REGISTER_TEST(sidebar_fold_reserves_toggle_zone,
              "sidebar: 溢出滚到底最后一行完整可见且不进按钮预留区（141 缝 §11.6 转义）", {
    // 141 回归缝（need186 §11.6 机制转义）：旧「越线整行不画」藏行退役，改由
    // 滚动区 viewport 裁剪保障「行永不绘制进按钮预留区」，被裁行滚动触达。
    // 矮窗（body 420，极紧档起点 80/行高 36/行距 2）+ 8 个平铺项：
    //   contentH = 8×(36+2)−2 = 302；viewportH = 420−44−80 = 296 → 溢出 6px，
    //   maxOffset = 6；滚到底（offset=6）最后一行（Mcp，画布 y 266..302）完整可见，
    //   侧栏坐标行底 = 80+302 = 382 ≤ toggle 上缘 420−36 = 384（不进按钮区）。
    // 下文全部 y 是**侧栏局部坐标**（compose 内部口径）。macOS 系统标题栏让位
    // 30px 在 compose 入口扣掉（kSystemTitleBarInset），窗口高要把它加回去
    //（60caabaa 双机同步口径）。
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();  // 折叠/分组展开/滚动 offset 回厂，不依赖前序用例
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    RecordingModule todo(::core::PageId::Todo, "待办");
    RecordingModule schedule(::core::PageId::Schedule, "日程");
    RecordingModule proxy(::core::PageId::ClaudeProxy, "代理");
    RecordingModule agent(::core::PageId::Agent, "智能体");
    RecordingModule memory(::core::PageId::Memory, "记忆");
    RecordingModule mcp(::core::PageId::Mcp, "MCP");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&md);
    router.registerModule(&todo);
    router.registerModule(&schedule);
    router.registerModule(&proxy);
    router.registerModule(&agent);
    router.registerModule(&memory);
    router.registerModule(&mcp);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 420.0f + kExpectedBodyTop;
    // 滚到底：offset 预置超界值 → compose 按 [0, contentH−viewportH] clamp（§11.2）
    app::setNavScrollOffsetForTests(9999.0f);
    eui::Ui ui;
    ui.begin("sidebar_fold");
    app::compose(ui, screen);
    ui.end();
    // compose 只建树不排版：读任何 frame 前必须 layout（本文件头部前提）
    ui.layout(screen.width, screen.height);

    const float contentH = 8.0f * 38.0f - 2.0f;   // 302
    const float viewportH = 420.0f - 44.0f - 80.0f;  // 296

    // 藏行退役：8 行全部在树（含旧口径越线不画的 Mcp），滚动触达
    DOCTEST_CHECK(ui.find("app.sidebar.item.Memory.hit") != nullptr);
    const auto* mcpHit = ui.find("app.sidebar.item.Mcp.hit");
    DOCTEST_REQUIRE(mcpHit != nullptr);
    DOCTEST_CHECK(floatEq(mcpHit->y, 266.0f));  // 画布坐标 7×38（行全保留）

    // offset clamp 收敛：9999 → 6 = contentH − viewportH（§11.6 建议②）
    const auto* nav = ui.find("app.sidebar.nav");
    DOCTEST_REQUIRE(nav != nullptr);
    DOCTEST_CHECK(floatEq(nav->scrollOffset, 6.0f));
    DOCTEST_CHECK(floatEq(nav->scrollMaxOffset, contentH - viewportH));

    // viewport 下界锚 = H − kToggleZoneH（守卫公式转义，§11.2）。
    // 注意坐标口径：builder y 是侧栏局部坐标，frame 是窗口绝对坐标（差
    // kExpectedBodyTop）——相对算术一律用 builder 值
    DOCTEST_CHECK(floatEq(nav->y, 80.0f));  // navStart（极紧档）
    DOCTEST_CHECK(floatEq(nav->frame.height, viewportH));  // 296

    // 滚到底最后一行完整可见（画布 y 266..302，视口内 260..296 ⊆ [0,296]）
    DOCTEST_CHECK(mcpHit->y + 36.0f - nav->scrollOffset <= viewportH + 0.01f);
    DOCTEST_CHECK(mcpHit->y - nav->scrollOffset >= -0.01f);
    // 行永不进按钮预留区：内容底 ≤ toggle 上缘（141 结论保留，机制换 viewport）
    const auto* toggle = ui.find("app.sidebar.toggle");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_CHECK(floatEq(toggle->y, 384.0f));  // H−36（§10.2 极紧档）
    DOCTEST_CHECK(nav->y + contentH <= toggle->y + 0.01f);  // 382 ≤ 384

    app::setNavScrollOffsetForTests(0.0f);  // offset 复位（进程级态）
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

REGISTER_TEST(sidebar_expanded_compose,
              "sidebar: 展开态 220px、brand 只留居中 logo、内容区 -220", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_expanded");
    app::compose(ui, screen);
    ui.end();

    const auto* sidebar = ui.find("app.sidebar");
    DOCTEST_CHECK(sidebar != nullptr);
    DOCTEST_CHECK(floatEq(sidebar->width.value, 220.0f));

    // UAT3 #9 + 图标批 2026-10-20：brand 行展开态 = 折叠态同款居中几何 logo
    //（.mark 矢量瓦，同 ui_kit::drawDevDeskLogo 契约），「DevDesk」文本改由自绘
    // 标题栏承载（app.titlebar.brand）；品牌行 y=32 防跳动（正常档 topPad）。
    // §10.1 logo 三档：H=1000 正常档 → 48，两态一致——(220-48)/2=86、(56-48)/2=4
    const auto* brand = ui.find("app.sidebar.brand");
    DOCTEST_CHECK(brand != nullptr);
    DOCTEST_CHECK(floatEq(brand->y, 32.0f));
    const auto* mark = ui.find("app.sidebar.brand.mark");
    DOCTEST_CHECK(mark != nullptr);
    DOCTEST_CHECK(floatEq(mark->x, 86.0f));  // (220-48)/2，行内水平居中
    DOCTEST_CHECK(floatEq(mark->y, 4.0f));   // (56-48)/2，行内垂直居中
    DOCTEST_CHECK(floatEq(mark->width.value, 48.0f));
    DOCTEST_CHECK(ui.find("app.sidebar.brand.logo") == nullptr);  // 旧展开态 logo 子元素已并
    // 导航起点仍 40+56+28=116；§11 行 y 为画布坐标（−navStart），首行贴视口顶
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Home.hit")->y, 0.0f));
    const auto* label = ui.find("app.sidebar.item.Home.label");
    DOCTEST_CHECK(label != nullptr);
    DOCTEST_CHECK(label->text == "首页");
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 28.0f));  // padX 16 + inset 12

    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.tip") == nullptr);
    // §10.2 正常档：钮 32、toggleY=H−40=960；水平 padX=16 对齐导航左缘
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->x, 16.0f));
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->y, 960.0f));
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1380.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1380.0f));

    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// 顶部留白只能作用于侧边栏**自己子树内部**。改留白位置的三种典型错误：
//   ① 把整页往下推 / 动窗口内容几何 —— ① 组断言立刻红（自绘标题栏路径的占位
//      属例外：那条路径由 sidebar_custom_titlebar_compose 专测，本用例钉 false）
//   ② 让侧边栏容器或分隔线不铺满整高 —— ② 组断言立刻红
//   ③ 给侧边栏加局部不透明底后忘了覆盖顶部留白区，露出"半透档透出桌面"的带
//      —— 侧边栏当前**没有**自己的底（底色 = 主窗口 clearColor）才天然免疫，
//      今后加底时必须连带处理留白区
REGISTER_TEST(sidebar_top_padding_stays_inside_sidebar,
              "sidebar: 顶部留白只在侧边栏内部（页面几何不变 + 无透明带）", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_top_padding");
    app::compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);  // 上面几条要读 frame，必须显式排版

    const auto* sidebar = ui.find("app.sidebar");
    const auto* page = ui.find("app.page");
    const auto* brand = ui.find("app.sidebar.brand");
    const auto* divider = ui.find("app.sidebar.divider");
    DOCTEST_REQUIRE(sidebar != nullptr);
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_REQUIRE(brand != nullptr);
    DOCTEST_REQUIRE(divider != nullptr);

    // ① 主内容区几何：页面 y 被推下去、或窗口内容高被 titlebar 吃掉，这里立刻红
    DOCTEST_CHECK(floatEq(page->frame.x, 220.0f));
    DOCTEST_CHECK(floatEq(page->frame.y, kExpectedBodyTop));
    DOCTEST_CHECK(floatEq(page->frame.width, 1380.0f));
    DOCTEST_CHECK(floatEq(page->frame.height, 1000.0f - kExpectedBodyTop));
    // 模块拿到的页面 Screen 高仍是整窗高（不因侧边栏留白缩水）
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1380.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight,
                          1000.0f - kExpectedBodyTop));

    // ② 侧边栏容器 / 分隔线从 y=0 起、铺满整高（留白区仍属侧边栏区域）
    DOCTEST_CHECK(floatEq(sidebar->frame.y, kExpectedBodyTop));
    DOCTEST_CHECK(floatEq(sidebar->frame.height, 1000.0f - kExpectedBodyTop));
    DOCTEST_CHECK(floatEq(divider->frame.y, kExpectedBodyTop));
    DOCTEST_CHECK(floatEq(divider->frame.height, 1000.0f - kExpectedBodyTop));

    // 留白值 = 正常档 topPad 32（need171 分档常量，H=1000 落正常档；数值恰与
    // spacingLarge×2 同值但已与字号 token 脱钩），且落在容器内部
    //（0 ≤ brand.y < 首个导航项 y）
    DOCTEST_CHECK(floatEq(brand->frame.y, 32.0f + kExpectedBodyTop));
    DOCTEST_CHECK(brand->frame.y >= sidebar->frame.y);

    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

REGISTER_TEST(sidebar_compose_writes_page_origin,
              "sidebar: compose 每帧写入页面区原点 x（展开 220 / 折叠 72）", {
    app::setCustomTitlebarMode(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 原点 = 侧边栏占宽；模块把窗口坐标换算成页面坐标时减本值。
    // compose 不写入 → 右键菜单整体右移一个侧边栏宽，跑进右侧详情区。
    app::setSidebarCollapsed(false);
    {
        eui::Ui ui;
        ui.begin("page_origin_expanded");
        app::compose(ui, screen);
        ui.end();
    }
    DOCTEST_CHECK(floatEq(router.pageOriginX(), 220.0f));

    // 折叠态原点随侧边栏收窄
    app::setSidebarCollapsed(true);
    {
        eui::Ui ui;
        ui.begin("page_origin_collapsed");
        app::compose(ui, screen);
        ui.end();
    }
    DOCTEST_CHECK(floatEq(router.pageOriginX(), 72.0f));

    // 未接入 app 布局的 Router：默认 0 = 模块侧换算无偏移
    ::core::Router fresh;
    DOCTEST_CHECK(floatEq(fresh.pageOriginX(), 0.0f));

    app::setSidebarCollapsed(false);  // 复位进程级全局态
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// compose 每帧遍历 router->modules()，注册表变化在下一次 compose 立即反映。
// 三步都要钉：反注册后当帧条目消失、再注册后条目回来、其余条目 y 不被挤压错位
// （条目 y 由 compose 循环变量决定，天然重排；置顶的 Home 条目尤其不许动）。
REGISTER_TEST(sidebar_live_toggle_reflects_registration,
              "sidebar: 注册/反注册后 compose 当帧反映（活门控）", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    RecordingModule agent(::core::PageId::Agent, "智能体");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&md);
    router.registerModule(&agent);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 基础 id（"app.sidebar.item." + pageIdToString(page)）自身不构成元素，
    // 直接 find 必为空；元素本体是 .hit/.icon/.label 三个。
    // 注册态：Agent 排在 Home/Markdown 之后，y = 116 + 2×(40+4)
    {
        eui::Ui ui;
        ui.begin("live_toggle_on");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 88.0f));  // 画布坐标 2×44（侧栏 116+88=204）
        const auto* agentLabel = ui.find("app.sidebar.item.Agent.label");
        DOCTEST_REQUIRE(agentLabel != nullptr);
        DOCTEST_CHECK(agentLabel->text == "智能体");
    }

    // 反注册 → 三件套全部消失，其余条目 y 原样
    router.unregisterModule(&agent);
    {
        eui::Ui ui;
        ui.begin("live_toggle_off");
        app::compose(ui, screen);
        ui.end();
        DOCTEST_CHECK(ui.find("app.sidebar.item.Agent.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.Agent.icon") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.Agent.label") == nullptr);
        const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
        const auto* mdHit = ui.find("app.sidebar.item.Markdown.hit");
        DOCTEST_REQUIRE(homeHit != nullptr);
        DOCTEST_REQUIRE(mdHit != nullptr);
        DOCTEST_CHECK(floatEq(homeHit->y, 0.0f));
        DOCTEST_CHECK(floatEq(mdHit->y, 44.0f));
    }

    // 再注册 → 条目回来，y 仍是 212（表序把它排在末位）
    router.registerModule(&agent);
    {
        eui::Ui ui;
        ui.begin("live_toggle_re_on");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 88.0f));  // 画布坐标 2×44（侧栏 116+88=204）
        const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
        const auto* mdHit = ui.find("app.sidebar.item.Markdown.hit");
        DOCTEST_REQUIRE(homeHit != nullptr);
        DOCTEST_REQUIRE(mdHit != nullptr);
        DOCTEST_CHECK(floatEq(homeHit->y, 0.0f));
        DOCTEST_CHECK(floatEq(mdHit->y, 44.0f));
    }
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// registerModule 按表序插入，compose 遍历序即表序，与注册调用的先后无关。
// 本用例故意乱序注册把「表序」与「注册序」分开：若 registerModule 退回 push_back
// 追加语义，Agent（最先注册）会恒停在第 1 位（y=124），下面第 2 步立刻红。
// 上一个用例注册序恰好等于表序，两序无差，测不出这个区别。
REGISTER_TEST(sidebar_re_register_lands_table_order,
              "sidebar: Agent 先注册、Home/Markdown 后注册 → compose 序仍表序（Agent y=212 回末位）", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule agent(::core::PageId::Agent, "智能体");
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router;
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 只注册 Agent → 它是唯一条目，画布 y=0（侧栏 116）
    router.registerModule(&agent);
    {
        eui::Ui ui;
        ui.begin("re_register_agent_only");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 0.0f));
    }

    // 再注册 Home、Markdown → 它们插到 Agent 前面，Agent 挤到画布 y=88（侧栏 204）
    router.registerModule(&home);
    router.registerModule(&md);
    {
        eui::Ui ui;
        ui.begin("re_register_after_backfill");
        app::compose(ui, screen);
        ui.end();
        const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
        const auto* mdHit = ui.find("app.sidebar.item.Markdown.hit");
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(homeHit != nullptr);
        DOCTEST_REQUIRE(mdHit != nullptr);
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(homeHit->y, 0.0f));
        DOCTEST_CHECK(floatEq(mdHit->y, 44.0f));
        DOCTEST_CHECK(floatEq(agentHit->y, 88.0f));
    }

    // unregister → 再 register：画布 y 仍 88（表序位由 pageId 决定，不受反复开关影响）
    router.unregisterModule(&agent);
    router.registerModule(&agent);
    {
        eui::Ui ui;
        ui.begin("re_register_off_on");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 88.0f));  // 画布坐标 2×44（侧栏 116+88=204）
        const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
        DOCTEST_REQUIRE(homeHit != nullptr);
        DOCTEST_CHECK(floatEq(homeHit->y, 0.0f));
    }
    app::setSidebarCollapsed(false);  // 还原，不污染后续用例
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// W3 导航壳整改：侧栏底色 + work/system 分组 + 组间 divider + active SelectionBar。
// 钉 §4.8 几何契约：work 组在前、system 组在后、组间 divider 在两组之间（仅当两组都非空）。
// 钉 §4.3 active 态视觉：SelectionBar 三件套（.sel.bg/.sel.accent/.sel.text）渲染在 active 项。
REGISTER_TEST(sidebar_w3_grouping_and_selectionbar,
              "sidebar: W3 分组(work/system) + 组间 divider + active SelectionBar (sec 4.8 + 4.3)", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule settings(::core::PageId::Settings, "设置");
    RecordingModule about(::core::PageId::About, "关于");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&settings);
    router.registerModule(&about);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::Home);  // 让 Home = active，验证 SelectionBar 渲染

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_w3_grouping");
    app::compose(ui, screen);
    ui.end();

    // 侧栏底色块存在（§4.8 D-12 E1：sidebarBg 微绿灰）
    DOCTEST_CHECK(ui.find("app.sidebar.bg") != nullptr);

    // UAT2 B 批：底色 tint 的 alpha 随窗口效果档位（core::window_effect_state 单值，
    // 写口 = settings applyWindowVisuals）——off 实色 1.0 / translucent 0.5 /
    // frosted 0.15 透 backdrop；RGB 恒 sidebarBg。档位本用例内显式设定
    // （进程级单值，与执行顺序无关），尾档复位 off 不污染后续用例。
    // 不用 lambda 收 color：DOCTEST_* 宏展开含裸 return（进值返回 lambda 编译炸），
    // 且 Element 指针系 Ui 内部树节点、Ui 出作用域即悬垂——三个显式块各自持 Ui。
    {
        const auto& t = ::core::theme::current();
        // off 档：实色 1.0，RGB 恒 sidebarBg（dark 档 #0F1F18，light 档 #F0FDF7）
        core::window_effect_state::setCurrent("off");
        {
            eui::Ui fx;
            fx.begin("sidebar_w3_fx_off");
            app::compose(fx, screen);
            fx.end();
            const auto* bg = fx.find("app.sidebar.bg");
            DOCTEST_REQUIRE(bg != nullptr);
            DOCTEST_CHECK(std::abs(bg->color.a - 1.0f) < 1e-3f);
            DOCTEST_CHECK(std::abs(bg->color.r - t.colors.sidebarBg.r) < 1e-3f);
            DOCTEST_CHECK(std::abs(bg->color.g - t.colors.sidebarBg.g) < 1e-3f);
            DOCTEST_CHECK(std::abs(bg->color.b - t.colors.sidebarBg.b) < 1e-3f);
        }
        // frosted 档：0.15 透 DWM backdrop（UAT6 起请求 Mica）
        core::window_effect_state::setCurrent("frosted");
        {
            eui::Ui fx;
            fx.begin("sidebar_w3_fx_frosted");
            app::compose(fx, screen);
            fx.end();
            const auto* bg = fx.find("app.sidebar.bg");
            DOCTEST_REQUIRE(bg != nullptr);
            DOCTEST_CHECK(std::abs(bg->color.a - 0.15f) < 1e-3f);
        }
        // translucent 档：0.72 半透（UAT6 调值，见 window_effect_state.h）
        core::window_effect_state::setCurrent("translucent");
        {
            eui::Ui fx;
            fx.begin("sidebar_w3_fx_translucent");
            app::compose(fx, screen);
            fx.end();
            const auto* bg = fx.find("app.sidebar.bg");
            DOCTEST_REQUIRE(bg != nullptr);
            DOCTEST_CHECK(std::abs(bg->color.a - 0.72f) < 1e-3f);
        }
        core::window_effect_state::setCurrent("off");  // 尾档复位
    }

    // Home（work 组、active）首行贴视口顶（画布 y=0，侧栏坐标 116）；
    // active 项应有 SelectionBar 三件套（§4.3）
    const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
    DOCTEST_REQUIRE(homeHit != nullptr);
    DOCTEST_CHECK(floatEq(homeHit->y, 0.0f));
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.bg") != nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.accent") != nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.text") != nullptr);

    // active 态几何：胶囊圆角 + 3×18 竖条垂直居中，且竖条在胶囊外侧的栏边留白里
    //（原型 .nav-item.active::before 同位）。贴着胶囊左缘通高那版读作「表格行高亮」。
    {
        const auto& t = ::core::theme::current();
        const auto* selBg = ui.find("app.sidebar.item.Home.sel.bg");
        const auto* selAccent = ui.find("app.sidebar.item.Home.sel.accent");
        DOCTEST_REQUIRE(selBg != nullptr);
        DOCTEST_REQUIRE(selAccent != nullptr);
        DOCTEST_CHECK(floatEq(selBg->radius, t.sizes.radiusSmall));
        // shape 的尺寸在 width/height（SizeValue），frame 只对走过布局的文字生效
        DOCTEST_CHECK(floatEq(selAccent->width.value, 3.0f));
        DOCTEST_CHECK(floatEq(selAccent->height.value, 18.0f));
        // 垂直居中：(40 − 18) / 2 = 11
        DOCTEST_CHECK(floatEq(selAccent->y - selBg->y, 11.0f));
        // 竖条退到留白区：比胶囊左缘再左 10px
        DOCTEST_CHECK(floatEq(selAccent->x - selBg->x, -10.0f));
        // active 图标/标签统一 primaryText（primary 作 16px 图标 2.38:1，非文本对比也过不了 3:1）
        const auto* icon = ui.find("app.sidebar.item.Home.icon");
        const auto* label = ui.find("app.sidebar.item.Home.label");
        DOCTEST_REQUIRE(icon != nullptr);
        DOCTEST_REQUIRE(label != nullptr);
        // text 的颜色在 textColor（Element::color 只给 rect/image 用）
        DOCTEST_CHECK(floatEq(icon->textColor.g, t.colors.primaryText.g));
        DOCTEST_CHECK(floatEq(label->textColor.r, t.colors.primaryText.r));
        DOCTEST_CHECK(floatEq(label->textColor.g, t.colors.primaryText.g));
    }

    // 组间 divider 在 Home 之下（画布 y=44 = itemH40 + gap4；侧栏坐标 44+navStart116=160）
    const auto* groupdiv = ui.find("app.sidebar.groupdiv");
    DOCTEST_REQUIRE(groupdiv != nullptr);
    DOCTEST_CHECK(floatEq(groupdiv->y, 44.0f));

    // Settings（system 组）在 divider 之下（画布 y=49 = 44 + divider1 + gap4）
    const auto* settingsHit = ui.find("app.sidebar.item.Settings.hit");
    DOCTEST_REQUIRE(settingsHit != nullptr);
    DOCTEST_CHECK(floatEq(settingsHit->y, 49.0f));

    // About 在 Settings 之下（画布 y=93 = 49 + itemH40 + gap4）
    const auto* aboutHit = ui.find("app.sidebar.item.About.hit");
    DOCTEST_REQUIRE(aboutHit != nullptr);
    DOCTEST_CHECK(floatEq(aboutHit->y, 93.0f));

    // Settings/About 是 inactive，不应有 SelectionBar（W3：active 才叠 SelectionBar）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Settings.sel.bg") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.About.sel.bg") == nullptr);

    // 标签文本逐字断言（纪律：分组不换标签字面；test_app_sidebar.cpp:232/:371 文本断言不动）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Settings.label")->text == "设置");
    DOCTEST_CHECK(ui.find("app.sidebar.item.About.label")->text == "关于");
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.label")->text == "首页");

    app::setSidebarCollapsed(false);  // 还原，不污染后续用例
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// W3：work-only 组无 divider（现有 Home+Markdown 测试隐式覆盖，但显式断言 groupdiv 不存在更稳）
REGISTER_TEST(sidebar_w3_no_divider_when_only_work,
              "sidebar: W3 仅 work 组时无 groupdiv（sec 4.8 分组仅两组都非空才画）", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_w3_no_divider");
    app::compose(ui, screen);
    ui.end();

    // 无 system 组项 → 不应画 groupdiv
    DOCTEST_CHECK(ui.find("app.sidebar.groupdiv") == nullptr);
    // Markdown 画布 y=44（不被 divider 推走；侧栏坐标 44+116=160 不变）
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Markdown.hit")->y, 44.0f));

    app::setSidebarCollapsed(false);
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// UAT3 D-R3-1 自绘标题栏（钉 customTitlebarActive=true = Windows 自绘壳运行态）：
// 标题条 40px 占位、根布局与页面内容同起点下移（防标题条盖内容/留缝）、品牌文本由
// 标题栏承载、两钮热区几何、命中回调逐条。系统标题栏路径（false）由上方既有用例钉住。
REGISTER_TEST(sidebar_custom_titlebar_compose,
              "sidebar: 自绘标题栏占位 40px + 标题条元素 + 命中回调 + 页面下移", {
    app::setCustomTitlebarMode(true);   // 钉自绘壳路径
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_titlebar");
    app::compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);  // frame 断言前必须显式排版

    // 标题条：全宽 × 40 高、贴 (0,0)
    const auto* bar = ui.find("app.titlebar");
    DOCTEST_REQUIRE(bar != nullptr);
    DOCTEST_CHECK(floatEq(app::titlebarHeight(), 40.0f));
    DOCTEST_CHECK(floatEq(bar->width.value, 1600.0f));
    DOCTEST_CHECK(floatEq(bar->height.value, 40.0f));
    DOCTEST_CHECK(floatEq(bar->frame.x, 0.0f));
    DOCTEST_CHECK(floatEq(bar->frame.y, 0.0f));

    // 左：品牌文本（UAT4：标题条 20px logo 移除——logo 元素必须不存在，品牌图形
    // 只留侧栏一处；文本起点对齐侧栏 padX=16）；右：两钮从右往左
    // 关闭 / 最小化（每钮 46 宽，wechat 风格；UAT4 最大化钮移除，只留双击路径）
    DOCTEST_CHECK(ui.find("app.titlebar.logo") == nullptr);
    DOCTEST_CHECK(ui.find("app.titlebar.brand")->text == "DevDesk");
    DOCTEST_CHECK(floatEq(ui.find("app.titlebar.brand")->x, 16.0f));
    DOCTEST_CHECK(ui.find("app.titlebar.close.hit") != nullptr);
    DOCTEST_CHECK(ui.find("app.titlebar.maximize.hit") == nullptr);
    DOCTEST_CHECK(ui.find("app.titlebar.minimize.hit") != nullptr);
    DOCTEST_CHECK(floatEq(ui.find("app.titlebar.close.hit")->x, 1600.0f - 46.0f));
    DOCTEST_CHECK(floatEq(ui.find("app.titlebar.minimize.hit")->x, 1600.0f - 92.0f));

    // 根布局下移 40：侧栏与页面同起点（页顶对齐侧栏顶）、页面 Screen 高 960
    const auto* sidebar = ui.find("app.sidebar");
    const auto* page = ui.find("app.page");
    DOCTEST_REQUIRE(sidebar != nullptr);
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_CHECK(floatEq(sidebar->frame.y, 40.0f));
    DOCTEST_CHECK(floatEq(page->frame.y, 40.0f));
    DOCTEST_CHECK(floatEq(page->frame.height, 960.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight, 960.0f));

    // 命中回调（窗口客户区逻辑坐标）：标题条左区拖拽；两钮热区（右缘 2×46）不
    // 拖拽；标题条下方与越界点不拖拽。边缘八向 resize 由 EUI-NEO 层自理，不经本
    // 回调。UAT4 钮区收窄后边界值随动：1508 = 1600-2×46
    DOCTEST_CHECK(app::titlebarCaptionHit({100.0f, 20.0f, 1600.0f, 1000.0f, false}));
    DOCTEST_CHECK(app::titlebarCaptionHit({1507.0f, 20.0f, 1600.0f, 1000.0f, false}));
    DOCTEST_CHECK(!app::titlebarCaptionHit({1508.0f, 20.0f, 1600.0f, 1000.0f, false}));
    DOCTEST_CHECK(!app::titlebarCaptionHit({1555.0f, 20.0f, 1600.0f, 1000.0f, false}));
    DOCTEST_CHECK(!app::titlebarCaptionHit({100.0f, 41.0f, 1600.0f, 1000.0f, false}));
    DOCTEST_CHECK(!app::titlebarCaptionHit({100.0f, -1.0f, 1600.0f, 1000.0f, false}));
    DOCTEST_CHECK(!app::titlebarCaptionHit({1601.0f, 20.0f, 1600.0f, 1000.0f, false}));

    // 系统标题栏路径（false）：无标题条元素、回调恒 false（几何由
    // sidebar_top_padding_stays_inside_sidebar 等既有用例钉，此处不重复断言）
    app::setCustomTitlebarMode(false);
    {
        eui::Ui plain;
        plain.begin("sidebar_titlebar_off");
        app::compose(plain, screen);
        plain.end();
        DOCTEST_CHECK(plain.find("app.titlebar") == nullptr);
    }
    DOCTEST_CHECK(!app::titlebarCaptionHit({100.0f, 20.0f, 1600.0f, 1000.0f, false}));

    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    app::setSidebarCollapsed(false);
    return true;
})

// need171（2026-11-03 设计文档 §2.2）：侧栏可用高驱动的离散三档间距——
// 正常档 H≥712（topPad 32/brandNavGap 28/行高 40/行距 4）、紧凑档 680≤H<712
//（16/16/40/4）、极紧档 H<680（12/12/36/2）。本文件用例钉 customTitlebarActive
//= false，Windows 路径 bodyTop=0 → H 恰等于 screen.height。导航起点连锁：
// 116 / 88 / 80；品牌行 56 与 logo 44 三档恒不缩（锚点不塌）。
REGISTER_TEST(sidebar_height_adaptive_tiers,
              "sidebar: 高度三档间距（H=1000 正常/H=700 紧凑/H=660 极紧）", {
    app::setCustomTitlebarMode(false);
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    // 正常档：起点 116、步进 44；logo 48、钮 32/区 48（§10.1/§10.2）
    {
        eui::Ui ui;
        ui.begin("tier_normal");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        app::compose(ui, screen);
        ui.end();
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand")->y, 32.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand.mark")->width.value, 48.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Home.hit")->y, 0.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Markdown.hit")->y, 44.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->y, 960.0f));   // H−40
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->width.value, 32.0f));
    }
    // 紧凑档：topPad/brandNavGap 16 → 起点 88；行高/行距不变 → 步进仍 44；
    // logo 44、钮 28/区 44
    {
        eui::Ui ui;
        ui.begin("tier_compact");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 700.0f;
        app::compose(ui, screen);
        ui.end();
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand")->y, 16.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand.mark")->width.value, 44.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Home.hit")->y, 0.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Markdown.hit")->y, 44.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->y, 664.0f));   // H−36
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->width.value, 28.0f));
    }
    // 极紧档：12/12/行高 36/行距 2 → 起点 80、步进 38；logo 40、钮 28/区 44
    {
        eui::Ui ui;
        ui.begin("tier_tight");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 660.0f;
        app::compose(ui, screen);
        ui.end();
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand")->y, 12.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand.mark")->width.value, 40.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Home.hit")->y, 0.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Markdown.hit")->y, 38.0f));
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->y, 624.0f));   // H−36
        DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->width.value, 28.0f));
    }
    app::setSidebarCollapsed(false);  // 复位进程级全局态
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// §10.1（need165 v2）：logo 三档 48/44/40 随可用高 H 离散切换，且展开/折叠两态
// 同值不回退（bug165 裁决保持）；档位切换离散无过渡。
REGISTER_TEST(sidebar_logo_three_tier_two_state,
              "sidebar: logo 三档 48/44/40 两态一致（§10.5-5）", {
    app::setCustomTitlebarMode(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    const float heights[3] = {1000.0f, 700.0f, 660.0f};  // 正常/紧凑/极紧
    const float logos[3] = {48.0f, 44.0f, 40.0f};
    for (int tier = 0; tier < 3; ++tier) {
        const bool states[2] = {false, true};  // 展开/折叠
        for (int s = 0; s < 2; ++s) {
            app::setSidebarCollapsed(states[s]);
            eui::Ui ui;
            ui.begin("logo_tier" + std::to_string(tier) +
                     "_state" + std::to_string(s));
            eui::Screen screen;
            screen.width = 1600.0f;
            screen.height = heights[tier];
            app::compose(ui, screen);
            ui.end();
            const float w = static_cast<float>(states[s] ? 72.0f : 220.0f);  // states: false=展开 / true=折叠
            const auto* mark = ui.find("app.sidebar.brand.mark");
            DOCTEST_REQUIRE_MESSAGE(mark != nullptr, "缺 brand.mark");
            DOCTEST_CHECK(floatEq(mark->width.value, logos[tier]));
            DOCTEST_CHECK(floatEq(mark->width.value, mark->height.value));
            // 行内居中：折叠 (72−logo)/2 / 展开 (220−logo)/2
            DOCTEST_CHECK(floatEq(mark->x, (w - logos[tier]) * 0.5f));
            DOCTEST_CHECK(floatEq(mark->y, (56.0f - logos[tier]) * 0.5f));
        }
    }
    app::setSidebarCollapsed(false);  // 复位进程级全局态
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// need172 v2（2026-11-04 设计文档 §10.2/§10.3）：收缩按钮态视觉 + 加强弥散绿粒子。
// hover/press 驱动走热区（app.sidebar.toggle.hot）onHoverChanged/onPress/onRelease
// 回调手调（无头无指针，与真指针共用同一接线）。判据：常态 1px borderDefault 圆框
// scale 1 且零粒子零泵；hover → primaryHover 描边/primary 图标/primarySoftBg 底/
// scale 1.04 + 12 颗正圆粒子（直径 4~6、opacity≤0.75、800ms+hash 延迟 0~80ms）
// + 生命泵；hover 停留超过粒子生命（≤880ms）→ 粒子自止消失（迸发一次不循环）；
// 移出再进 → 重触发；按压 scale 0.96 + primary 描边；码点 fontSizeSmall(12)。
REGISTER_TEST(sidebar_toggle_button_states_and_particles,
              "sidebar: 收缩按钮 hover/按压态 + 粒子 v2 一次性迸发/自止/重触发", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();  // 按钮 hover/粒子态回厂（不依赖前序用例）
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    const auto& colors = ::core::theme::current().colors;
    eui::Ui ui;

    // F1 常态：零粒子零泵、1px borderDefault 圆框、scale 1、透明底、码点 12
    ui.begin("toggle_idle");
    app::compose(ui, screen);
    ui.end();
    const auto* idle = ui.find("app.sidebar.toggle");
    DOCTEST_REQUIRE(idle != nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.0") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.driver") == nullptr);
    DOCTEST_CHECK(floatEq(idle->border.width, 1.0f));
    DOCTEST_CHECK(floatEq(idle->border.color.r, colors.borderDefault.r));
    DOCTEST_CHECK(floatEq(idle->transform.scale.x, 1.0f));
    DOCTEST_CHECK(idle->color.a < 1e-3f);  // 常态透明底
    const auto* idleIcon = ui.find("app.sidebar.toggle.icon");
    DOCTEST_REQUIRE(idleIcon != nullptr);
    DOCTEST_CHECK(floatEq(idleIcon->fontSize, 12.0f));  // §10.2 fontSizeSmall
    const auto* idleHot = ui.find("app.sidebar.toggle.hot");
    DOCTEST_REQUIRE(idleHot != nullptr);
    DOCTEST_CHECK(idleHot->onHoverChanged != nullptr);  // 交互回调在热区

    // hover 进入沿（热区）→ F2：hover 目标值 + 粒子起爆 + 生命泵。
    // 每颗 hash 延迟 0~80ms，先泵 120ms 确保全数起爆再计数（确定性判据）
    idleHot->onHoverChanged(true);
    ui.begin("toggle_hover");
    app::compose(ui, screen);
    ui.end();
    const auto* hov = ui.find("app.sidebar.toggle");
    DOCTEST_REQUIRE(hov != nullptr);
    DOCTEST_CHECK(floatEq(hov->border.color.r, colors.primaryHover.r));
    DOCTEST_CHECK(floatEq(hov->transform.scale.x, 1.04f));
    const auto* hovIcon = ui.find("app.sidebar.toggle.icon");
    DOCTEST_REQUIRE(hovIcon != nullptr);
    DOCTEST_CHECK(floatEq(hovIcon->textColor.r, colors.primary.r));
    DOCTEST_CHECK(floatEq(hov->color.g, colors.primarySoftBg.g));  // 绿底
    DOCTEST_REQUIRE(ui.find("app.sidebar.toggle.particle.driver") != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ui.begin("toggle_hover_full");
    app::compose(ui, screen);
    ui.end();
    int particleCount = 0;
    for (int i = 0; i < 12; ++i) {
        const auto* p =
            ui.find("app.sidebar.toggle.particle." + std::to_string(i));
        DOCTEST_REQUIRE_MESSAGE(p != nullptr,
                                "缺粒子 " + std::to_string(i));
        ++particleCount;
        DOCTEST_CHECK(floatEq(p->width.value, p->height.value));  // 正圆
        DOCTEST_CHECK(p->width.value >= 4.0f * 0.7f - 0.01f);   // 直径 4~6（末端缩 0.7）
        DOCTEST_CHECK(p->width.value <= 6.0f + 0.01f);
        DOCTEST_CHECK(floatEq(p->radius, p->width.value * 0.5f));
        DOCTEST_CHECK(p->opacity <= 0.75f + 1e-3f);
        DOCTEST_CHECK(p->opacity > 0.0f);
    }
    DOCTEST_CHECK(particleCount == 12);

    // 按压 → F3：scale 0.96 + primary 描边（pressed 描边比 hover 深一档）
    const auto* hotNow = ui.find("app.sidebar.toggle.hot");
    DOCTEST_REQUIRE(hotNow != nullptr);
    hotNow->onPress(core::PointerEvent{}, core::Rect{});
    ui.begin("toggle_pressed");
    app::compose(ui, screen);
    ui.end();
    const auto* pressed = ui.find("app.sidebar.toggle");
    DOCTEST_REQUIRE(pressed != nullptr);
    DOCTEST_CHECK(floatEq(pressed->transform.scale.x, 0.96f));
    DOCTEST_CHECK(floatEq(pressed->border.color.r, colors.primary.r));

    // 松开 → F4：回 hover 视觉；粒子不受按压影响（迸发一次未被重置）
    const auto* hotReleased = ui.find("app.sidebar.toggle.hot");
    DOCTEST_REQUIRE(hotReleased != nullptr);
    hotReleased->onRelease(core::PointerEvent{}, core::Rect{});
    ui.begin("toggle_released");
    app::compose(ui, screen);
    ui.end();
    const auto* released = ui.find("app.sidebar.toggle");
    DOCTEST_REQUIRE(released != nullptr);
    DOCTEST_CHECK(floatEq(released->transform.scale.x, 1.04f));
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.0") != nullptr);

    // hover 停留超过粒子生命（800ms+延迟 80ms ≤880ms）→ F5：粒子自止消失、零泵
    //（不循环判据；hover 仍保持 true——若每帧重触发这里会重现粒子）
    std::this_thread::sleep_for(std::chrono::milliseconds(920));
    ui.begin("toggle_hold_after_life");
    app::compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.0") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.driver") == nullptr);
    const auto* stillHover = ui.find("app.sidebar.toggle");
    DOCTEST_REQUIRE(stillHover != nullptr);
    DOCTEST_CHECK(floatEq(stillHover->transform.scale.x, 1.04f));  // hover 视觉保持

    // 移出再进（false→true 沿）→ F6：粒子重新触发
    const auto* hotRe = ui.find("app.sidebar.toggle.hot");
    DOCTEST_REQUIRE(hotRe != nullptr);
    hotRe->onHoverChanged(false);
    hotRe->onHoverChanged(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ui.begin("toggle_re_enter");
    app::compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.0") != nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.toggle.particle.driver") != nullptr);

    // 复位：hover/粒子/进程级态不还原会污染后续用例
    const auto* lastHot = ui.find("app.sidebar.toggle.hot");
    if (lastHot != nullptr && lastHot->onHoverChanged) lastHot->onHoverChanged(false);
    app::resetSidebarGlobals();
    app::setCustomTitlebarMode(std::nullopt);
    return true;
})

// need186 §11.6 建议断言：①最小窗 H=680（紧凑档）收起态零滚动条（§11.7-13 前半）；
// ②H<585（极紧收起态）溢出且 offset 下界 = contentH − viewportH；④分组展开滚动条
// 出现、收起后即时消失（contentKey 重测）。
REGISTER_TEST(sidebar_nav_scroll_overflow_matrix,
              "sidebar: 滚动溢出矩阵——680 收起零滚/展开滚/收起即隐/560 offset 下界", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();  // 分组展开/滚动 offset 回厂
    // 默认 12 行注册（9 平铺 + Release 分组父项 + Settings + About，分组收起）
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    RecordingModule todo(::core::PageId::Todo, "待办");
    RecordingModule schedule(::core::PageId::Schedule, "日程");
    RecordingModule proxy(::core::PageId::ClaudeProxy, "代理");
    RecordingModule agent(::core::PageId::Agent, "智能体");
    RecordingModule memory(::core::PageId::Memory, "记忆");
    RecordingModule wallpaper(::core::PageId::Wallpaper, "壁纸");
    RecordingModule mcp(::core::PageId::Mcp, "MCP");
    RecordingModule releaseTasks(::core::PageId::ReleaseTasks, "任务");
    RecordingModule releaseConfig(::core::PageId::ReleaseConfig, "基础配置");
    RecordingModule settings(::core::PageId::Settings, "设置");
    RecordingModule about(::core::PageId::About, "关于");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&md);
    router.registerModule(&todo);
    router.registerModule(&schedule);
    router.registerModule(&proxy);
    router.registerModule(&agent);
    router.registerModule(&memory);
    router.registerModule(&wallpaper);
    router.registerModule(&mcp);
    router.registerModule(&releaseTasks);
    router.registerModule(&releaseConfig);
    router.registerModule(&settings);
    router.registerModule(&about);
    test_support::ScopedAppRouter scoped(&router);

    // ① H=680 紧凑档：viewportH = 680−44−88 = 548，contentH = 12×44+1 = 529
    //   → 未溢出，滚动条零绘制（§11.3 非溢出零绘制；最小窗承诺保持）
    {
        eui::Ui ui;
        ui.begin("scroll_680_collapsed");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 680.0f;
        app::compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const auto* nav = ui.find("app.sidebar.nav");
        DOCTEST_REQUIRE(nav != nullptr);
        DOCTEST_CHECK(floatEq(nav->y, 88.0f));  // navStart（紧凑档）
        DOCTEST_CHECK(floatEq(nav->frame.height, 548.0f));  // 680−44−88
        DOCTEST_CHECK(ui.find("app.sidebar.nav.scroll") == nullptr);
    }
    // ④ 展开 ReleaseGroup（+2 子行）→ 14 行 contentH = 14×44+1 = 617 > 548
    //   → 滚动组件出现（bug188：零宽不可见）、offset 下界 = 69
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    {
        eui::Ui ui;
        ui.begin("scroll_680_expanded");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 680.0f;
        app::compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const auto* nav = ui.find("app.sidebar.nav");
        DOCTEST_REQUIRE(nav != nullptr);
        // bug188：滚动条视觉全无——scroll/thumb 组件存在但零宽；滚动能力保留
        //（scrollMaxOffset>0 = 内容可达）
        const auto* sb = ui.find("app.sidebar.nav.scroll");
        const auto* thumb = ui.find("app.sidebar.nav.scroll.thumb");
        DOCTEST_REQUIRE(sb != nullptr);
        DOCTEST_REQUIRE(thumb != nullptr);
        DOCTEST_CHECK(floatEq(sb->frame.width, 0.0f));
        DOCTEST_CHECK(floatEq(thumb->frame.width, 0.0f));
        DOCTEST_CHECK(floatEq(nav->scrollMaxOffset, 617.0f - 548.0f));
    }
    // ④ 收起 → contentKey 重测 12 行 → 滚动条即时消失、offset clamp 回 0
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    {
        eui::Ui ui;
        ui.begin("scroll_680_recollapsed");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 680.0f;
        app::compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        DOCTEST_CHECK(ui.find("app.sidebar.nav.scroll") == nullptr);
        const auto* nav = ui.find("app.sidebar.nav");
        DOCTEST_REQUIRE(nav != nullptr);
        DOCTEST_CHECK(floatEq(nav->scrollOffset, 0.0f));
        DOCTEST_CHECK(floatEq(nav->scrollMaxOffset, 0.0f));
    }
    // ② H=560 极紧收起态：contentH = 12×38+1 = 457，viewportH = 560−44−80 = 436
    //   → 溢出；offset 预置超界值 → clamp 到 21 = contentH − viewportH（§11.2）
    app::setNavScrollOffsetForTests(9999.0f);
    {
        eui::Ui ui;
        ui.begin("scroll_560_overflow");
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 560.0f;
        app::compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const auto* nav = ui.find("app.sidebar.nav");
        DOCTEST_REQUIRE(nav != nullptr);
        DOCTEST_CHECK(floatEq(nav->y, 80.0f));  // navStart（极紧档）
        DOCTEST_CHECK(floatEq(nav->frame.height, 436.0f));
        // bug188：滚动条零宽（视觉全无），滚动能力保留（offset/maxOffset 下方断言）
        const auto* sb = ui.find("app.sidebar.nav.scroll");
        const auto* thumb = ui.find("app.sidebar.nav.scroll.thumb");
        DOCTEST_REQUIRE(sb != nullptr);
        DOCTEST_REQUIRE(thumb != nullptr);
        DOCTEST_CHECK(floatEq(sb->frame.width, 0.0f));
        DOCTEST_CHECK(floatEq(thumb->frame.width, 0.0f));
        DOCTEST_CHECK(floatEq(nav->scrollMaxOffset, 457.0f - 436.0f));
        DOCTEST_CHECK(floatEq(nav->scrollOffset, 21.0f));  // clamp 收敛
        // 滚到底最后一行（About，画布 y 421..457）完整可见：
        // 视口内 385..421+36 ⊆ [0,436]，且内容底 457 不进按钮预留区（H−44=516）
        const auto* aboutHit = ui.find("app.sidebar.item.About.hit");
        DOCTEST_REQUIRE(aboutHit != nullptr);
        DOCTEST_CHECK(floatEq(aboutHit->y + 36.0f, 457.0f));  // 内容底=末行底
    }
    app::setNavScrollOffsetForTests(0.0f);  // offset 复位（进程级态）
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

// 复审 P2-1（bug252 / clue92 老板终裁）：全屏 0 宽此前只有产品代码（app_sidebar.cpp
// `sidebarWidth()<=0` 整树早退）无自动化断言。终端页 toggleFullscreen 当帧打桥
//（terminal_page.h → terminal_fullscreen_bridge → main.cpp:656 装配的 handler →
// setSidebarHiddenForFullscreen 打旗标）→ 0 宽 + 侧栏整树不建——被否的 72px 折叠
// 残留形态「半截 logo」正是靠这条早退挡住的。桥默认桩 = 未装配静默丢弃，用例内
// 自行装配（lambda 形态同 main.cpp assembleTerminalBridgeNow），尾部拆桩 + 回厂。
REGISTER_TEST(sidebar_fullscreen_hidden_zero_width,
              "sidebar: 全屏旗标 → 0 宽 + 侧栏整树不建（品牌不画）；退出恢复 220", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    app::terminal_fullscreen::setHandler(
        +[](bool collapsed) { app::setSidebarHiddenForFullscreen(collapsed); });

    // 进全屏：桥当帧打旗标 → 0 宽
    app::terminal_fullscreen::setCollapsed(true);
    DOCTEST_CHECK(floatEq(app::sidebarWidth(), 0.0f));

    // 当帧 compose：整棵侧栏子树早退（容器/品牌行/矢量瓦三层全不建）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_fs_hidden");
    app::compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("app.sidebar") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.brand") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.brand.mark") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.brand.logo") == nullptr);
    // 终端页内容区随之全宽（页宽 = windowWidth − sidebarWidth()）
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1600.0f));

    // 退出全屏：旗标清 + 进屏前快照（展开）恢复 → 220
    app::terminal_fullscreen::setCollapsed(false);
    DOCTEST_CHECK(floatEq(app::sidebarWidth(), 220.0f));

    // 复位（测试隔离）：桥拆回默认桩 + 进程级旗标/折叠态回厂
    app::terminal_fullscreen::setHandler(nullptr);
    app::resetSidebarGlobals();
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})
