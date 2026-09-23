// tests/test_modules/test_app_sidebar.cpp
// 侧边栏折叠/展开（2026-09-23）单测：
//   1. SettingsConfig.sidebarCollapsed 持久化：默认值 / 往返 / 老配置缺键兼容
//   2. setSidebarCollapsed：内存态 + 落盘（含其他字段不被覆盖）
//   3. 无头 compose（真实 app::compose + 桩 Router）：
//      折叠态宽 72 / label 不渲染 / 图标水平居中 / tooltip 挂载 / 模块内容区
//      宽度 = 窗口宽 - 72；展开态 260 / label 回来 / tooltip 无 / 宽 - 260
#include "doctest.h"

#include "app_sidebar.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/router.h"

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

// scoped appRouter 注入（test_module_gating 同款）：离开作用域复位 null，
// 不污染其他无 Router 假设的用例
class ScopedAppRouter {
public:
    explicit ScopedAppRouter(::core::Router* router) {
        ::core::registerAppRouter(router);
    }
    ~ScopedAppRouter() { ::core::registerAppRouter(nullptr); }
};

std::filesystem::path tempConfigFile(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("devdesk_test_sidebar_" + std::string(name) + ".json");
}

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
    const std::filesystem::path file = tempConfigFile("roundtrip");
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
    const std::filesystem::path file = tempConfigFile("legacy");
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
    const std::filesystem::path file = tempConfigFile("toggle");
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
              "sidebar: 折叠态 72px、label 隐藏、图标居中、tooltip、内容区 +188", {
    app::setSidebarCollapsed(true);  // 只改内存，不动真实 config.json
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    ScopedAppRouter scoped(&router);

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

    // brand 折叠为字母 D
    DOCTEST_CHECK(ui.find("app.sidebar.brand")->text == "D");

    // label 不渲染（折叠态模块名只走 tooltip）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.label") == nullptr);
    DOCTEST_CHECK(ui.find("app.sidebar.item.Markdown.label") == nullptr);

    // 图标水平居中：(72 - 28) / 2 = 22
    const auto* icon = ui.find("app.sidebar.item.Home.icon");
    DOCTEST_CHECK(icon != nullptr);
    DOCTEST_CHECK(floatEq(icon->x, 22.0f));

    // tooltip 挂载（hover 才可见，元素常驻）
    DOCTEST_CHECK(ui.find("app.sidebar.item.Home.tip") != nullptr);

    // 折叠开关：居中 (72 - 36) / 2 = 18
    const auto* toggle = ui.find("app.sidebar.toggle");
    DOCTEST_CHECK(toggle != nullptr);
    DOCTEST_CHECK(floatEq(toggle->x, 18.0f));

    // 模块内容区宽度跟随：1600 - 72 = 1528（展开 1340 → 折叠 +188）
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1528.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenHeight, 1000.0f));

    app::setSidebarCollapsed(false);  // 还原，不污染后续用例
    return true;
})

REGISTER_TEST(sidebar_expanded_compose,
              "sidebar: 展开态 260px、label/icon 现状不变、tooltip 无、内容区 -260", {
    app::setSidebarCollapsed(false);
    RecordingModule home(::core::PageId::Home, "首页");
    RecordingModule md(::core::PageId::Markdown, "笔记");
    ::core::Router router = makeStubRouter(home, md);
    ScopedAppRouter scoped(&router);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("sidebar_expanded");
    app::compose(ui, screen);
    ui.end();

    const auto* sidebar = ui.find("app.sidebar");
    DOCTEST_CHECK(sidebar != nullptr);
    DOCTEST_CHECK(floatEq(sidebar->width.value, 260.0f));

    // brand / label / 图标现状不变
    DOCTEST_CHECK(ui.find("app.sidebar.brand")->text == "DevDesk");
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

    // 内容区宽度 1600 - 260 = 1340
    DOCTEST_CHECK(floatEq(app::pageContentWidth(1600.0f), 1340.0f));
    DOCTEST_CHECK(floatEq(RecordingModule::lastScreenWidth, 1340.0f));
    return true;
})
