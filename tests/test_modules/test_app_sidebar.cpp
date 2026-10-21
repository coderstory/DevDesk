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

} // namespace

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
    // (0, 40, 72, 56)；UAT5 logo 36→48 放大、kBrandH 32→56 随行加高——
    // .mark 在行内居中：(72-48)/2=12、(56-48)/2=4（48 瓦上下各留 4px，无溢出）
    const auto* collapsedBrand = ui.find("app.sidebar.brand");
    DOCTEST_CHECK(collapsedBrand != nullptr);
    const auto* collapsedMark = ui.find("app.sidebar.brand.mark");
    DOCTEST_CHECK(collapsedMark != nullptr);
    DOCTEST_CHECK(floatEq(collapsedMark->x, 12.0f));
    DOCTEST_CHECK(floatEq(collapsedMark->y, 4.0f));
    DOCTEST_CHECK(floatEq(collapsedMark->width.value, 48.0f));

    // 顶部留白 = spacingXXLarge(40)，与页面主区上 padding 同档；折叠/展开两态必须同值，
    // 否则切换时 logo 会纵向跳
    DOCTEST_CHECK(floatEq(collapsedBrand->y, 40.0f));
    const auto* firstHit = ui.find("app.sidebar.item.Home.hit");
    DOCTEST_CHECK(firstHit != nullptr);
    DOCTEST_CHECK(floatEq(firstHit->y, 124.0f));  // 40 + 品牌高 56 + 品牌/导航间距 28

    // 折叠态不渲染 label（模块名只走 tooltip）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.label") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Markdown.label") == nullptr);

    // 图标在 72 内水平居中：(72 - 28) / 2
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 22.0f));

    // 折叠开关同样居中：(72 - 36) / 2
    const auto* toggle = ui.find("app.sidebar.toggle");
    DOCTEST_CHECK(toggle != nullptr);
    DOCTEST_CHECK(floatEq(toggle->x, 18.0f));

    // 模块内容区宽度跟随侧边栏实际占宽
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight, 1000.0f));

    app::setSidebarCollapsed(false);  // 复位进程级全局态
    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

REGISTER_TEST(sidebar_expanded_compose,
              "sidebar: 展开态 208px、brand 只留居中 logo、内容区 -208", {
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
    DOCTEST_CHECK(floatEq(sidebar->width.value, 208.0f));

    // UAT3 #9 + 图标批 2026-10-20：brand 行展开态 = 折叠态同款居中几何 logo
    //（.mark 矢量瓦，同 ui_kit::drawDevDeskLogo 契约），「DevDesk」文本改由自绘
    // 标题栏承载（app.titlebar.brand）；品牌行 y=40 防跳动。UAT5 logo 36→48、
    // kBrandH 32→56：(208-48)/2=80、(56-48)/2=4（48 瓦上下各留 4px，无溢出）
    const auto* brand = ui.find("app.sidebar.brand");
    DOCTEST_CHECK(brand != nullptr);
    DOCTEST_CHECK(floatEq(brand->y, 40.0f));
    const auto* mark = ui.find("app.sidebar.brand.mark");
    DOCTEST_CHECK(mark != nullptr);
    DOCTEST_CHECK(floatEq(mark->x, 80.0f));  // (208-48)/2，行内水平居中
    DOCTEST_CHECK(floatEq(mark->y, 4.0f));  // (56-48)/2，行内垂直居中
    DOCTEST_CHECK(floatEq(mark->width.value, 48.0f));
    DOCTEST_CHECK(ui.find("app.sidebar.brand.logo") == nullptr);  // 旧展开态 logo 子元素已并
    // 导航列表整体下移到 40 + 56 + 28
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Home.hit")->y, 124.0f));
    const auto* label = ui.find("app.sidebar.item.Home.label");
    DOCTEST_CHECK(label != nullptr);
    DOCTEST_CHECK(label->text == "首页");
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 28.0f));  // padX 16 + inset 12

    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.tip") == nullptr);
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->x, 16.0f));  // 开关左对齐 padX
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1392.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1392.0f));

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
    DOCTEST_CHECK(floatEq(page->frame.x, 208.0f));
    DOCTEST_CHECK(floatEq(page->frame.y, 0.0f));
    DOCTEST_CHECK(floatEq(page->frame.width, 1392.0f));
    DOCTEST_CHECK(floatEq(page->frame.height, 1000.0f));
    // 模块拿到的页面 Screen 高仍是整窗高（不因侧边栏留白缩水）
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1392.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight, 1000.0f));

    // ② 侧边栏容器 / 分隔线从 y=0 起、铺满整高（留白区仍属侧边栏区域）
    DOCTEST_CHECK(floatEq(sidebar->frame.y, 0.0f));
    DOCTEST_CHECK(floatEq(sidebar->frame.height, 1000.0f));
    DOCTEST_CHECK(floatEq(divider->frame.y, 0.0f));
    DOCTEST_CHECK(floatEq(divider->frame.height, 1000.0f));

    // 留白值 = spacingXXLarge(40)，且落在容器内部（0 ≤ brand.y < 首个导航项 y）
    const float space = ::core::theme::current().sizes.spacingXXLarge;
    DOCTEST_CHECK(floatEq(space, 40.0f));
    DOCTEST_CHECK(floatEq(brand->frame.y, space));
    DOCTEST_CHECK(brand->frame.y >= sidebar->frame.y);

    app::setCustomTitlebarMode(std::nullopt);  // 复位：跟平台走
    return true;
})

REGISTER_TEST(sidebar_compose_writes_page_origin,
              "sidebar: compose 每帧写入页面区原点 x（展开 208 / 折叠 72）", {
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
    DOCTEST_CHECK(floatEq(router.pageOriginX(), 208.0f));

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
    // 注册态：Agent 排在 Home/Markdown 之后，y = 124 + 2×(40+4)
    {
        eui::Ui ui;
        ui.begin("live_toggle_on");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 212.0f));
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
        DOCTEST_CHECK(floatEq(homeHit->y, 124.0f));
        DOCTEST_CHECK(floatEq(mdHit->y, 168.0f));
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
        DOCTEST_CHECK(floatEq(agentHit->y, 212.0f));
        const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
        const auto* mdHit = ui.find("app.sidebar.item.Markdown.hit");
        DOCTEST_REQUIRE(homeHit != nullptr);
        DOCTEST_REQUIRE(mdHit != nullptr);
        DOCTEST_CHECK(floatEq(homeHit->y, 124.0f));
        DOCTEST_CHECK(floatEq(mdHit->y, 168.0f));
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

    // 只注册 Agent → 它是唯一条目，y=124
    router.registerModule(&agent);
    {
        eui::Ui ui;
        ui.begin("re_register_agent_only");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 124.0f));
    }

    // 再注册 Home、Markdown → 它们插到 Agent 前面，Agent 被挤到 y=212
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
        DOCTEST_CHECK(floatEq(homeHit->y, 124.0f));
        DOCTEST_CHECK(floatEq(mdHit->y, 168.0f));
        DOCTEST_CHECK(floatEq(agentHit->y, 212.0f));
    }

    // unregister → 再 register：y 仍 212（表序位由 pageId 决定，不受反复开关影响）
    router.unregisterModule(&agent);
    router.registerModule(&agent);
    {
        eui::Ui ui;
        ui.begin("re_register_off_on");
        app::compose(ui, screen);
        ui.end();
        const auto* agentHit = ui.find("app.sidebar.item.Agent.hit");
        DOCTEST_REQUIRE(agentHit != nullptr);
        DOCTEST_CHECK(floatEq(agentHit->y, 212.0f));
        const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
        DOCTEST_REQUIRE(homeHit != nullptr);
        DOCTEST_CHECK(floatEq(homeHit->y, 124.0f));
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

    // 侧栏底色块存在（§4.8 D-12 E1：sidebarBgTinted 微绿灰）
    DOCTEST_CHECK(ui.find("app.sidebar.bg") != nullptr);

    // UAT2 B 批：底色 tint 的 alpha 随窗口效果档位（core::window_effect_state 单值，
    // 写口 = settings applyWindowVisuals）——off 实色 1.0 / translucent 0.5 /
    // frosted 0.15 透 backdrop；RGB 恒 sidebarBgTinted。档位本用例内显式设定
    // （进程级单值，与执行顺序无关），尾档复位 off 不污染后续用例。
    // 不用 lambda 收 color：DOCTEST_* 宏展开含裸 return（进值返回 lambda 编译炸），
    // 且 Element 指针系 Ui 内部树节点、Ui 出作用域即悬垂——三个显式块各自持 Ui。
    {
        const auto& t = ::core::theme::current();
        const auto& wc = t.colors.wechat;
        // off 档：实色 1.0，RGB 恒 sidebarBgTinted
        core::window_effect_state::setCurrent("off");
        {
            eui::Ui fx;
            fx.begin("sidebar_w3_fx_off");
            app::compose(fx, screen);
            fx.end();
            const auto* bg = fx.find("app.sidebar.bg");
            DOCTEST_REQUIRE(bg != nullptr);
            DOCTEST_CHECK(std::abs(bg->color.a - 1.0f) < 1e-3f);
            DOCTEST_CHECK(std::abs(bg->color.r - wc.sidebarBgTinted.r) < 1e-3f);
            DOCTEST_CHECK(std::abs(bg->color.g - wc.sidebarBgTinted.g) < 1e-3f);
            DOCTEST_CHECK(std::abs(bg->color.b - wc.sidebarBgTinted.b) < 1e-3f);
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

    // Home（work 组、active）在 y=124；active 项应有 SelectionBar 三件套（§4.3）
    const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
    DOCTEST_REQUIRE(homeHit != nullptr);
    DOCTEST_CHECK(floatEq(homeHit->y, 124.0f));
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.bg") != nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.accent") != nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.text") != nullptr);

    // 组间 divider 在 Home 之下（y=168 = 124 + itemH40 + gap4）
    const auto* groupdiv = ui.find("app.sidebar.groupdiv");
    DOCTEST_REQUIRE(groupdiv != nullptr);
    DOCTEST_CHECK(floatEq(groupdiv->y, 168.0f));

    // Settings（system 组）在 divider 之下（y=173 = 168 + divider1 + gap4）
    const auto* settingsHit = ui.find("app.sidebar.item.Settings.hit");
    DOCTEST_REQUIRE(settingsHit != nullptr);
    DOCTEST_CHECK(floatEq(settingsHit->y, 173.0f));

    // About 在 Settings 之下（y=217 = 173 + itemH40 + gap4）
    const auto* aboutHit = ui.find("app.sidebar.item.About.hit");
    DOCTEST_REQUIRE(aboutHit != nullptr);
    DOCTEST_CHECK(floatEq(aboutHit->y, 217.0f));

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
    // Markdown.y 仍是 168（不被 divider 推走）
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Markdown.hit")->y, 168.0f));

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
