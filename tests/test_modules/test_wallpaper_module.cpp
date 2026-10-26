// tests/test_modules/test_wallpaper_module.cpp
// 壁纸模块（P1-A）契约：枚举序、注册表行、默认关闭、门控、持久化、开关即时生效。
// 跨文件前提：
//   - 枚举序 == kModuleRegistry 表序 == 侧边栏顺序，wallpaper 插在 Agent 与 Settings 之间。
//   - 平台能力判据只有一个：platform::wallpaper::supported()；false 时工厂返回 nullptr，
//     行仍在表里（开关键/短名/图标可查），只是装配层拿到空实例。
#include "doctest.h"
#include "test_support.h"

#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "platform/wallpaper_capability.h"

#include <eui/dsl.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

namespace {

// dispatchModuleToggle 的 ModuleLookup 是裸函数指针（无捕获）→ 桩与查找只能走文件级静态。
class StubPageModule final : public ::core::Module {
public:
    explicit StubPageModule(::core::PageId page) : page_(page) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return "stub"; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui&, const eui::Screen&) override {}
    void onStart() override { ++starts; }
    void onStop() override { ++stops; }

    int starts = 0;
    int stops = 0;

private:
    ::core::PageId page_;
};

StubPageModule g_wallpaperStub(::core::PageId::Wallpaper);

::core::Module* wallpaperLookup(::core::PageId page) {
    return page == ::core::PageId::Wallpaper ? &g_wallpaperStub : nullptr;
}

constexpr const char* kCfgPrefix = "devdesk_test_wallpaper_";

// 表里的 wallpaper 行（找不到返回 nullptr；表序断言先钉总数再定位）
const ::app::registry::ModuleEntry* wallpaperRow() {
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page == ::core::PageId::Wallpaper) return &e;
    }
    return nullptr;
}

} // namespace

// 防：枚举插错位置导致侧栏/分发顺序漂移。
REGISTER_TEST(wallpaper_pageid_order,
              "wallpaper: PageId 序 Agent < Wallpaper < Settings 且 pageIdToString 为 Wallpaper", {
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Agent) <
                  static_cast<int>(::core::PageId::Wallpaper));
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Wallpaper) <
                  static_cast<int>(::core::PageId::Settings));
    DOCTEST_CHECK_EQ(
        std::string(::core::pageIdToString(::core::PageId::Wallpaper)),
        std::string("Wallpaper"));
    return true;
})

// 防：表行与模块虚函数双副本漂移（短名/图标改一处漏另一处）。
REGISTER_TEST(wallpaper_registry_row,
              "wallpaper: 表序==枚举序，行 gatingKey/shortName/icon 与工厂实例一致，不支持平台工厂为空", {
    constexpr std::size_t kRegistrySize =
        sizeof(::app::registry::kModuleRegistry) /
        sizeof(::app::registry::kModuleRegistry[0]);
    const ::core::PageId kEnumOrder[] = {
        ::core::PageId::Home,      ::core::PageId::Markdown,
        ::core::PageId::Todo,      ::core::PageId::ClaudeProxy,
        ::core::PageId::Agent,     ::core::PageId::Wallpaper,
        ::core::PageId::Settings,  ::core::PageId::About,
    };
    DOCTEST_REQUIRE_EQ(kRegistrySize, static_cast<std::size_t>(8));
    for (std::size_t i = 0; i < kRegistrySize; ++i) {
        DOCTEST_CHECK(::app::registry::kModuleRegistry[i].page == kEnumOrder[i]);
    }

    const ::app::registry::ModuleEntry* row = wallpaperRow();
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_CHECK_EQ(std::string(row->gatingKey),
                     std::string(module::settings::kModuleIdWallpaper));
    DOCTEST_CHECK_EQ(std::string(row->shortName), std::string("壁纸"));
    DOCTEST_CHECK_EQ(std::string(row->iconGlyph), std::string("\xEF\x80\xBE"));

    std::unique_ptr<::core::Module> mod(row->factory());
    if (!::platform::wallpaper::supported()) {
        // 平台能力不支持：工厂按契约返回 nullptr（页面不注册）
        DOCTEST_CHECK(mod == nullptr);
        return true;
    }
    DOCTEST_REQUIRE(mod != nullptr);
    DOCTEST_CHECK(mod->pageId() == ::core::PageId::Wallpaper);
    DOCTEST_CHECK_EQ(mod->shortName(), std::string("壁纸"));
    DOCTEST_CHECK_EQ(mod->iconGlyph(), std::string("\xEF\x80\xBE"));
    return true;
})

// 防：默认被打开，首次启动冒出壁纸页。
REGISTER_TEST(wallpaper_default_off,
              "wallpaper: 默认清单 5 项不含 wallpaper、moduleEnabled false、sanitized 不自动补", {
    using namespace module::settings;
    const SettingsConfig cfg;
    DOCTEST_CHECK_EQ(cfg.enabledModules.size(), static_cast<std::size_t>(5));
    DOCTEST_CHECK(std::find(cfg.enabledModules.begin(), cfg.enabledModules.end(),
                            kModuleIdWallpaper) == cfg.enabledModules.end());
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdWallpaper));

    const SettingsConfig fixed = SettingsStore::sanitized(cfg);
    DOCTEST_CHECK(std::find(fixed.enabledModules.begin(),
                            fixed.enabledModules.end(),
                            kModuleIdWallpaper) == fixed.enabledModules.end());
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdWallpaper));
    return true;
})

// 防：门控键写错/表序错——启用后页面仍不出现，或出现在错误位置。
REGISTER_TEST(wallpaper_enabled_gating,
              "wallpaper: 启用后 moduleEnabled true；all-off 3 条、all-on 8 条且 wallpaper 在 Agent 与 Settings 之间", {
    using namespace module::settings;
    constexpr std::size_t kRegistrySize =
        sizeof(::app::registry::kModuleRegistry) /
        sizeof(::app::registry::kModuleRegistry[0]);

    SettingsConfig allOn;
    allOn.enabledModules = {kModuleIdHome,    kModuleIdMarkdown, kModuleIdTodo,
                            kModuleIdClaudeProxy, kModuleIdAgent,
                            kModuleIdWallpaper, kModuleIdSettings};
    DOCTEST_CHECK(moduleEnabled(allOn, kModuleIdWallpaper));
    const auto gated = app::assembly::buildGatedEntries(
        allOn, ::app::registry::kModuleRegistry, kRegistrySize);
    DOCTEST_REQUIRE_EQ(gated.size(), static_cast<std::size_t>(8));
    DOCTEST_CHECK(gated[4].page == ::core::PageId::Agent);
    DOCTEST_CHECK(gated[5].page == ::core::PageId::Wallpaper);
    DOCTEST_CHECK(gated[6].page == ::core::PageId::Settings);

    SettingsConfig allOff;
    allOff.enabledModules.clear();
    const auto gatedOff = app::assembly::buildGatedEntries(
        allOff, ::app::registry::kModuleRegistry, kRegistrySize);
    DOCTEST_REQUIRE_EQ(gatedOff.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK(gatedOff[0].page == ::core::PageId::Home);
    DOCTEST_CHECK(gatedOff[1].page == ::core::PageId::Settings);
    DOCTEST_CHECK(gatedOff[2].page == ::core::PageId::About);
    return true;
})

// 防：开关不持久化（重启后壁纸又被关掉），或未知 ID 混进清单。
REGISTER_TEST(wallpaper_toggle_persist,
              "wallpaper: SettingsStore save/load 往返保留 wallpaper，未知 ID 仍被过滤", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "persist", /*registerAppDb=*/true);

    SettingsConfig cfg;
    cfg.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdWallpaper};
    const SettingsStore store(::core::appDatabase());
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdWallpaper));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdTodo));

    // 手改配置：wallpaper 保留、未知 ID 过滤、恒启用项补齐（home/settings + wallpaper = 3）
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(), SettingsStore::keyNamespace(),
        "{\"enabledModules\": [\"wallpaper\", \"bogus_module\"]}"));
    const SettingsConfig raw = store.load();
    DOCTEST_CHECK(moduleEnabled(raw, kModuleIdWallpaper));
    DOCTEST_CHECK(!moduleEnabled(raw, "bogus_module"));
    DOCTEST_CHECK_EQ(raw.enabledModules.size(), static_cast<std::size_t>(3));
    return true;
})

// 防：开关链路只改设置不生效（dispatch 找不到行或拼接键写错）。
REGISTER_TEST(wallpaper_dispatch_toggle,
              "wallpaper: dispatchModuleToggle on 注册/off 注销且当前页回 Home", {
    using namespace module::settings;
    g_wallpaperStub.starts = 0;
    g_wallpaperStub.stops = 0;
    app::clearAppNotice();  // 进程级 toast 回厂（本用例只认自己触发的通知）

    ::core::Router router;
    StubPageModule home(::core::PageId::Home);
    router.registerModule(&home);  // 生产形态：Home 恒注册（关闭重定向的落点）

    app::assembly::dispatchModuleToggle(kModuleIdWallpaper, true, router,
                                        &wallpaperLookup);
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Wallpaper));
    DOCTEST_CHECK_EQ(g_wallpaperStub.starts, 1);

    router.navigateTo(::core::PageId::Wallpaper);
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::Wallpaper);

    app::assembly::dispatchModuleToggle(kModuleIdWallpaper, false, router,
                                        &wallpaperLookup);
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::Wallpaper));
    DOCTEST_CHECK_EQ(g_wallpaperStub.stops, 1);
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::Home);

    app::clearAppNotice();  // 收尾回厂，不留给后续用例
    return true;
})