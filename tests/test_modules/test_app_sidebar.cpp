// tests/test_modules/test_app_sidebar.cpp
// 侧边栏折叠/展开（2026-09-23）单测：
//   1. SettingsConfig.sidebarCollapsed 持久化：默认值 / 往返 / 老配置缺键兼容
//   2. setSidebarCollapsed：内存态 + 落盘（含其他字段不被覆盖）
//   3. 无头 compose（真实 app::compose + 桩 Router）：
//      折叠态宽 72 / label 不渲染 / 图标水平居中 / tooltip 挂载 / 模块内容区
//      宽度 = 窗口宽 - 72；展开态 208 / label 回来 / tooltip 无 / 宽 - 208
#include "doctest.h"
#include "test_support.h"

#include "app_sidebar.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"  // 顶部留白 = spacingXXLarge（token 断言）

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
// 每个用例独立的临时配置文件 —— 共享工厂（TIN-06b，原三份 tempConfigFile
// 合流），调用点统一 test_support::tempConfigFile(kCfgPrefix, name)
constexpr const char* kCfgPrefix = "devdesk_test_sidebar_";

void writeRawConfig(const std::filesystem::path& file,
                    const std::string& json) {
    std::ofstream f(file, std::ios::trunc);
    f << json;
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

// ============ 1. SettingsConfig.sidebarCollapsed 持久化 ============

REGISTER_TEST(sidebar_config_default_and_roundtrip,
              "sidebar: sidebarCollapsed 默认 false + 存取往返", {
    const std::filesystem::path file = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    const module::settings::SettingsStore store(file);
    // 默认值（无文件）
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    // 往返 true
    module::settings::SettingsConfig cfg = store.load();
    cfg.sidebarCollapsed = true;
    DOCTEST_CHECK(store.save(cfg).isOk());
    DOCTEST_CHECK(store.load().sidebarCollapsed);
    // 往返 false
    cfg = store.load();
    cfg.sidebarCollapsed = false;
    DOCTEST_CHECK(store.save(cfg).isOk());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    return true;
})

REGISTER_TEST(sidebar_config_legacy_missing_key,
              "sidebar: 老配置无 sidebarCollapsed 键 → 默认展开", {
    const std::filesystem::path file = test_support::tempConfigFile(kCfgPrefix, "legacy");
    // 老格式：只有 theme（其余字段缺省）
    writeRawConfig(file, R"({"theme": "light"})");
    const module::settings::SettingsStore store(file);
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    // 显式 true 能读回
    writeRawConfig(file, R"({"theme": "light", "sidebarCollapsed": true})");
    DOCTEST_CHECK(store.load().sidebarCollapsed);
    return true;
})

// ============ 2. setSidebarCollapsed：内存态 + 落盘不覆盖其他字段 ============

REGISTER_TEST(sidebar_toggle_persists,
              "sidebar: setSidebarCollapsed 翻内存态并落盘（保留其他字段）", {
    const std::filesystem::path file = test_support::tempConfigFile(kCfgPrefix, "toggle");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    // 预置一份带主题的配置，折叠切换后主题必须原样保留
    module::settings::SettingsStore store(file);
    module::settings::SettingsConfig base;
    base.darkTheme = false;  // light
    DOCTEST_CHECK(store.save(base).isOk());

    app::setSidebarCollapsed(true, file);
    DOCTEST_CHECK(app::sidebarCollapsed());
    {
        const auto cfg = store.load();
        DOCTEST_CHECK(cfg.sidebarCollapsed);
        DOCTEST_CHECK_FALSE(cfg.darkTheme);  // load→改→save 没有吃掉主题
    }

    app::setSidebarCollapsed(false, file);
    DOCTEST_CHECK_FALSE(app::sidebarCollapsed());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);

    // 空路径 = 只改内存不落盘（启动恢复路径）
    app::setSidebarCollapsed(true);
    DOCTEST_CHECK(app::sidebarCollapsed());
    DOCTEST_CHECK_FALSE(store.load().sidebarCollapsed);
    // 还原默认，不污染后续用例
    app::setSidebarCollapsed(false);
    return true;
})

// ============ 3. 无头 compose：折叠态 / 展开态 ============

REGISTER_TEST(sidebar_collapsed_compose,
              "sidebar: 折叠态 72px、label 隐藏、图标居中、tooltip、内容区 +136", {
    app::setSidebarCollapsed(true);  // 只改内存，不动真实 config.json
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

    // 侧边栏宽 72（含分隔线位置）
    const auto* sidebar = ui.find("app.sidebar");
    DOCTEST_CHECK(sidebar != nullptr);
    DOCTEST_CHECK(floatEq(sidebar->width.value, 72.0f));
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.divider")->x, 71.0f));

    // brand 折叠 = 只显示 logo 图标（icon 模式：text 存 glyph 的 UTF-8 串）
    const auto* collapsedBrand = ui.find("app.sidebar.brand");
    DOCTEST_CHECK(collapsedBrand != nullptr);
    DOCTEST_CHECK(collapsedBrand->text == eui::utf8(0xF5FC));  // fa-laptop-code

    // 顶部留白（2026-09-28 用户真机反馈「侧边栏加一点上边距」）：品牌块上缘 =
    // spacingXXLarge(40)，与页面主区上 padding 同档；折叠态与展开态同一纵向位置
    //（只差横向分支），切换时 logo 不跳
    DOCTEST_CHECK(floatEq(collapsedBrand->y, 40.0f));
    // 导航列表随品牌块整体下移：40 + 品牌高 32 + 品牌/导航间距 28 = 100
    const auto* firstHit = ui.find("app.sidebar.item.Home.hit");
    DOCTEST_CHECK(firstHit != nullptr);
    DOCTEST_CHECK(floatEq(firstHit->y, 100.0f));

    // label 不渲染（折叠态模块名只走 tooltip）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.label") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Markdown.label") == nullptr);

    // 图标水平居中：(72 - 28) / 2 = 22
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 22.0f));

    // 折叠开关：居中 (72 - 36) / 2 = 18
    const auto* toggle = ui.find("app.sidebar.toggle");
    DOCTEST_CHECK(toggle != nullptr);
    DOCTEST_CHECK(floatEq(toggle->x, 18.0f));

    // 模块内容区宽度跟随：1600 - 72 = 1528（展开 1392 → 折叠 +136）
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight, 1000.0f));

    app::setSidebarCollapsed(false);  // 还原，不污染后续用例
    return true;
})

REGISTER_TEST(sidebar_expanded_compose,
              "sidebar: 展开态 208px、label/icon 现状不变、tooltip 无、内容区 -208", {
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

    // brand 文字不变 + logo 图标在文字左侧（2026-09-24）
    DOCTEST_CHECK(ui.find("app.sidebar.brand")->text == "DevDesk");
    const auto* logo = ui.find("app.sidebar.brand.logo");
    DOCTEST_CHECK(logo != nullptr);
    DOCTEST_CHECK(floatEq(logo->x, 16.0f));  // padX
    // 顶部留白（2026-09-28 用户真机反馈「侧边栏加一点上边距」）：logo 与品牌文字
    // 同 y = spacingXXLarge(40)，与页面主区上 padding 齐平
    DOCTEST_CHECK(floatEq(logo->y, 40.0f));
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.brand")->y, 40.0f));
    // 导航列表随之整体下移：40 + 32 + 28 = 100
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.item.Home.hit")->y, 100.0f));
    const auto* label = ui.find("app.sidebar.item.Home.label");
    DOCTEST_CHECK(label != nullptr);
    DOCTEST_CHECK(label->text == "首页");
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 28.0f));  // padX 16 + inset 12

    // 展开态不挂 tooltip
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.tip") == nullptr);

    // 开关：左对齐 padX
    DOCTEST_CHECK(floatEq(ui.find("app.sidebar.toggle")->x, 16.0f));

    // 内容区宽度 1600 - 208 = 1392
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1392.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1392.0f));
    return true;
})

// 顶部留白的边界守卫（2026-09-28 用户纠正"改的层次不对"）：留白只能作用于
// 侧边栏**自己子树内部**——不许把整页推下去、不许撑高 titlebar、不许在窗口顶与
// 内容之间造一条"既无底也无内容"的空带。三条断言各对应一种错误表现：
//   ① 右侧主内容区（app.page）几何：x=208 / y=0 / 宽 1392 / 高 1000 —— 与改动前
//      逐项相同（页面 y 若被推下去、或窗口内容高被 titlebar 吃掉，这里立刻红）；
//   ② 侧边栏容器与分隔线都从 y=0 起、铺满整高 —— 新增留白区仍属侧边栏区域；
//   ③ 侧边栏**没有**自己的不透明底（底色 = 主窗口 clearColor，见 drawSidebar 内
//      注释），故留白区不可能变成"半透档透出桌面"的透明带；把这条写成断言，
//      防止今后有人给侧边栏加局部底时漏掉顶部留白区。
REGISTER_TEST(sidebar_top_padding_stays_inside_sidebar,
              "sidebar: 顶部留白只在侧边栏内部（页面几何不变 + 无透明带）", {
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
    ui.layout(screen.width, screen.height);  // 取帧需显式布局（同 test_ui_modern）

    const auto* sidebar = ui.find("app.sidebar");
    const auto* page = ui.find("app.page");
    const auto* brand = ui.find("app.sidebar.brand");
    const auto* divider = ui.find("app.sidebar.divider");
    DOCTEST_REQUIRE(sidebar != nullptr);
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_REQUIRE(brand != nullptr);
    DOCTEST_REQUIRE(divider != nullptr);

    // ① 主内容区几何（= 改动前）——锁住"不得把整页推下去 / 不得动窗口内容几何"
    DOCTEST_CHECK(floatEq(page->frame.x, 208.0f));
    DOCTEST_CHECK(floatEq(page->frame.y, 0.0f));
    DOCTEST_CHECK(floatEq(page->frame.width, 1392.0f));
    DOCTEST_CHECK(floatEq(page->frame.height, 1000.0f));
    // 模块拿到的页面 Screen 同源：高 = 整窗高（不因侧边栏留白缩水）
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1392.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight, 1000.0f));

    // ② 侧边栏容器 / 分隔线仍从 y=0 起、铺满整高（留白区仍在侧边栏区域内）
    DOCTEST_CHECK(floatEq(sidebar->frame.y, 0.0f));
    DOCTEST_CHECK(floatEq(sidebar->frame.height, 1000.0f));
    DOCTEST_CHECK(floatEq(divider->frame.y, 0.0f));
    DOCTEST_CHECK(floatEq(divider->frame.height, 1000.0f));

    // 留白值 = spacingXXLarge(40)，且落在容器内部（0 ≤ brand.y < 首个导航项 y）
    const float space = ::core::theme::current().sizes.spacingXXLarge;
    DOCTEST_CHECK(floatEq(space, 40.0f));
    DOCTEST_CHECK(floatEq(brand->frame.y, space));
    DOCTEST_CHECK(brand->frame.y >= sidebar->frame.y);
    return true;
})

// ============ 4. 页面区原点写入 Router（右键菜单坐标系修复） ============
REGISTER_TEST(sidebar_compose_writes_page_origin,
              "sidebar: compose 每帧写入页面区原点 x（展开 208 / 折叠 72）", {
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    test_support::ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 展开态：页面区原点 = 侧边栏占宽 208（模块把窗口坐标换算成页面
    // 坐标时减本值；不写入 → 菜单右移一个侧边栏宽，跑进右侧详情区）
    app::setSidebarCollapsed(false);
    {
        eui::Ui ui;
        ui.begin("page_origin_expanded");
        app::compose(ui, screen);
        ui.end();
    }
    DOCTEST_CHECK(floatEq(router.pageOriginX(), 208.0f));

    // 折叠态：原点随侧边栏收窄到 72
    app::setSidebarCollapsed(true);
    {
        eui::Ui ui;
        ui.begin("page_origin_collapsed");
        app::compose(ui, screen);
        ui.end();
    }
    DOCTEST_CHECK(floatEq(router.pageOriginX(), 72.0f));

    // 未接入 app 布局的 Router：默认 0 = 模块侧换算无偏移（旧行为）
    ::core::Router fresh;
    DOCTEST_CHECK(floatEq(fresh.pageOriginX(), 0.0f));

    app::setSidebarCollapsed(false);  // 还原，不污染后续用例
    return true;
})
