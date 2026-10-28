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
#include "settings_ui_cards.h"
#include "settings_ui_logic.h"

#include "core/database.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "platform/wallpaper_capability.h"

#include <eui/dsl.h>

#include <ui_kit/columns.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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

extern "C" ::core::Module* create_settings_module();

constexpr const char* kCfgPrefix = "devdesk_test_wallpaper_";

// tab 点击助手（本文件无 test_settings.cpp 的文件内 helper 可见性）：无头等价真实
// 点击——onClick 把 tab 索引写进设置模块成员。与被测生产逻辑无关。
bool clickSettingsTab(::core::Module& mod, const eui::Screen& screen, int tab) {
    eui::Ui ui;
    ui.begin("wallpaper_settings_tab");
    mod.compose(ui, screen);
    ui.end();
    const auto* hit =
        ui.find("settings.tabs." + std::to_string(tab) + ".hit");
    if (hit == nullptr || hit->onClick == nullptr) return false;
    hit->onClick();
    return true;
}

// 模块 tab 一帧：返回元素树存活到调用方作用域（Ui 必须由调用方持有）。
// 用法：eui::Ui ui; composeModulesTab(*mod, screen, ui);
void composeModulesTab(::core::Module& mod, const eui::Screen& screen,
                       eui::Ui& ui) {
    ui.begin("wallpaper_settings_modules");
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

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
              "wallpaper: PageId 序 Agent < Memory < Wallpaper < Mcp < Settings 且 pageIdToString 为 Wallpaper", {
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Agent) <
                  static_cast<int>(::core::PageId::Memory));
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Memory) <
                  static_cast<int>(::core::PageId::Wallpaper));
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Wallpaper) <
                  static_cast<int>(::core::PageId::Mcp));
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Mcp) <
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
        ::core::PageId::Agent,     ::core::PageId::Memory,
        ::core::PageId::Wallpaper, ::core::PageId::Mcp,
        ::core::PageId::Settings,
        ::core::PageId::About,
    };
    DOCTEST_REQUIRE_EQ(kRegistrySize, static_cast<std::size_t>(10));
    for (std::size_t i = 0; i < kRegistrySize; ++i) {
        DOCTEST_CHECK(::app::registry::kModuleRegistry[i].page == kEnumOrder[i]);
    }

    const ::app::registry::ModuleEntry* row = wallpaperRow();
    DOCTEST_REQUIRE(row != nullptr);
    // 先判空再构造 std::string：std::string(nullptr) 是 UB（clang -Wnonnull 直接拦）
    DOCTEST_REQUIRE(row->gatingKey != nullptr);
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
              "wallpaper: 默认清单 6 项不含 wallpaper、moduleEnabled false、sanitized 不自动补", {
    using namespace module::settings;
    const SettingsConfig cfg;
    DOCTEST_CHECK_EQ(cfg.enabledModules.size(), static_cast<std::size_t>(6));
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
              "wallpaper: 启用后 moduleEnabled true；all-off 3 条、all-on 10 条且 wallpaper 在 Memory 与 Mcp 之间", {
    using namespace module::settings;
    constexpr std::size_t kRegistrySize =
        sizeof(::app::registry::kModuleRegistry) /
        sizeof(::app::registry::kModuleRegistry[0]);

    SettingsConfig allOn;
    allOn.enabledModules = {kModuleIdHome,    kModuleIdMarkdown, kModuleIdTodo,
                            kModuleIdClaudeProxy, kModuleIdAgent,
                            kModuleIdMemory, kModuleIdWallpaper,
                            kModuleIdMcp,
                            kModuleIdSettings};
    DOCTEST_CHECK(moduleEnabled(allOn, kModuleIdWallpaper));
    const auto gated = app::assembly::buildGatedEntries(
        allOn, ::app::registry::kModuleRegistry, kRegistrySize);
    DOCTEST_REQUIRE_EQ(gated.size(), static_cast<std::size_t>(10));
    DOCTEST_CHECK(gated[4].page == ::core::PageId::Agent);
    DOCTEST_CHECK(gated[5].page == ::core::PageId::Memory);
    DOCTEST_CHECK(gated[6].page == ::core::PageId::Wallpaper);
    DOCTEST_CHECK(gated[7].page == ::core::PageId::Mcp);
    DOCTEST_CHECK(gated[8].page == ::core::PageId::Settings);

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

// 平台双分支（同一用例、运行期判据，不用 #ifdef）：
//   Windows（supported=true）：模块 tab 出现壁纸卡与 44×24 开关，默认 cfg 下为关；
//   非 Windows（supported=false）：整卡不建（不是禁用态）、工厂返回 nullptr。
// 防：macOS CI 单分支断言必红 / 卡在不支持平台错误出现。
REGISTER_TEST(wallpaper_settings_card_visible,
              "wallpaper: 设置模块 tab 壁纸格（模块插拔卡第 5 格）——supported 平台存在且默认关；不支持平台整格与工厂均无", {
    namespace ms = module::settings;
    test_support::TestEnv env(kCfgPrefix, "cardvisible", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "cardvisible_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, ms::kTabModules));
    eui::Ui ui;
    composeModulesTab(*mod, screen, ui);

    // 2026-10-27 用户真机拍板：独立全宽卡废除，壁纸 = 插拔卡内第 5 格；
    // 格无独立根元素，存在性锚 = 格 name / 开关 track（格根 id 保留 wallpaper 前缀）。
    const auto* cell = ui.find("settings.card.wallpaper.name");
    const auto* track = ui.find("settings.card.wallpaper.sw.track");
    if (!::platform::wallpaper::supported()) {
        DOCTEST_CHECK(cell == nullptr);
        DOCTEST_CHECK(track == nullptr);
        std::unique_ptr<::core::Module> wallpaperPage(create_wallpaper_module());
        DOCTEST_CHECK(wallpaperPage == nullptr);
        mod->onStop();
        return true;
    }
    DOCTEST_REQUIRE(cell != nullptr);
    DOCTEST_REQUIRE(track != nullptr);
    DOCTEST_CHECK_EQ(track->frame.width, 44.0f);
    DOCTEST_CHECK_EQ(track->frame.height, 24.0f);
    // 默认配置不含 wallpaper → 开关为关：wswitch 关态滑块贴左（padding 2）。
    const auto* thumb = ui.find("settings.card.wallpaper.sw.thumb");
    DOCTEST_REQUIRE(thumb != nullptr);
    DOCTEST_CHECK(std::fabs(thumb->x - 2.0f) < 0.01f);
    mod->onStop();
    return true;
})

// 开关链路（仅 supported 平台）：点 track → applyModuleEnabled → 落库；
// 再点一次恢复关并从 enabledModules 移除。
// 防：开关只改画面不落库（重启后壁纸又关掉）/ 关不掉（清单项未移除）。
REGISTER_TEST(wallpaper_settings_card_toggle_persists,
              "wallpaper: 壁纸卡开关走 applyModuleEnabled——开启落库、再点关闭并从清单移除", {
    namespace ms = module::settings;
    if (!::platform::wallpaper::supported()) {
        return true;   // 不支持平台无卡可点（可见性用例已断言整卡不建）
    }
    test_support::TestEnv env(kCfgPrefix, "cardtoggle", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "cardtoggle_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, ms::kTabModules));

    eui::Ui ui;
    composeModulesTab(*mod, screen, ui);
    const auto* track = ui.find("settings.card.wallpaper.sw.track");
    DOCTEST_REQUIRE(track != nullptr);
    DOCTEST_REQUIRE(track->onClick != nullptr);
    track->onClick();   // off → on（wswitch 回调 = applyModuleEnabled(wallpaper, true)）

    {
        ms::SettingsStore store(::core::appDatabase());
        const ms::SettingsConfig loaded = store.load();
        DOCTEST_CHECK(ms::moduleEnabled(loaded, ms::kModuleIdWallpaper));
        DOCTEST_CHECK(std::find(loaded.enabledModules.begin(),
                                loaded.enabledModules.end(),
                                ms::kModuleIdWallpaper) !=
                      loaded.enabledModules.end());
    }

    // 再渲染：开关显示 on（滑块贴右 44−20−2=22），再点 → off 且清单移除
    eui::Ui ui2;
    composeModulesTab(*mod, screen, ui2);
    const auto* track2 = ui2.find("settings.card.wallpaper.sw.track");
    const auto* thumb2 = ui2.find("settings.card.wallpaper.sw.thumb");
    DOCTEST_REQUIRE(track2 != nullptr);
    DOCTEST_REQUIRE(thumb2 != nullptr);
    DOCTEST_CHECK(std::fabs(thumb2->x - 22.0f) < 0.01f);
    DOCTEST_REQUIRE(track2->onClick != nullptr);
    track2->onClick();   // on → off

    {
        ms::SettingsStore store(::core::appDatabase());
        const ms::SettingsConfig loaded = store.load();
        DOCTEST_CHECK(!ms::moduleEnabled(loaded, ms::kModuleIdWallpaper));
        DOCTEST_CHECK(std::find(loaded.enabledModules.begin(),
                                loaded.enabledModules.end(),
                                ms::kModuleIdWallpaper) ==
                      loaded.enabledModules.end());
    }
    mod->onStop();
    return true;
})

// 三档几何（直绘插拔卡，壁纸第 6 格；平台门控在格内——不支持平台整格不建，
// 见 wallpaper_settings_card_visible）：格随常规格机制（name 18/rowdesc 16/开关
// 44×24 行内 +6），末行（右列，记忆格同行）须整体包在卡高内、行底留边 ≥16、130% 档不裁切；
// 点 track 回调带对模块 id。2026-10-27 用户真机拍板：独立全宽卡废除。
// 防：卡高没跟 roundFs 派生 → 130% 档开关被裁 / 开关尺寸漂移 / 格错行错列。
REGISTER_TEST(wallpaper_card_geometry,
              "wallpaper: 壁纸第 6 格（插拔卡内）85/100/130% 几何——末行右列、44×24 开关右贴格、行底留边≥16、回调带 id", {
    namespace ms = module::settings;
    if (!::platform::wallpaper::supported()) {
        return true;   // 不支持平台无第 5 格（可见性用例已断言整格不建）
    }
    struct FontScaleGuard {
        float prev;
        explicit FontScaleGuard(float s) : prev(::core::theme::fontScale()) {
            ::core::theme::setFontScale(s);
        }
        ~FontScaleGuard() { ::core::theme::setFontScale(prev); }
    };

    for (const float scale : {0.85f, 1.0f, 1.3f}) {
        FontScaleGuard g{scale};
        ms::SettingsConfig cfg;
        std::string draft;
        std::vector<std::string> tail;
        ms::SettingsCardsIn in{cfg, draft, tail};
        int toggles = 0;
        bool lastOn = false;
        std::string lastId;
        ms::SettingsCardActions a;
        a.applyModuleEnabled = [&](const std::string& id, bool v) {
            ++toggles;
            lastId = id;
            lastOn = v;
        };

        const float w = 1312.0f;   // 默认窗 1600 的内容宽
        const float h = ms::roundFs(ms::kModulesCardHeightWallpaper);
        eui::Ui ui;
        ui.begin(scale < 0.9f   ? "wpg85"
                 : scale > 1.2f ? "wpg130"
                                : "wpg100");
        ms::CardDrawArgs args{ui, "settings.card.modules", w, h,
                              ::core::theme::current(), 0.0f, 1000.0f, in, a};
        ms::drawModulesCard(args);
        ui.end();
        ui.layout(w, h);

        const auto* card = ui.find("settings.card.modules");
        const auto* name = ui.find("settings.card.wallpaper.name");
        const auto* desc = ui.find("settings.card.wallpaper.rowdesc");
        const auto* track = ui.find("settings.card.wallpaper.sw.track");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE(name != nullptr);
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_REQUIRE(track != nullptr);

        // 末行右列（第 3 行 col 1；格序不变量：kToggleable 表序 + 壁纸恒末格）：
        // 格 y = 68（字面锚）+ 2×roundFs(56)，x = 28 + cell2X
        //（与常规格同源：rowY = 68.0f + row×rowPitch，锚不随字号缩放）
        DOCTEST_CHECK(std::fabs(
            name->frame.y -
            (68.0f + 2.0f * ms::roundFs(56.0f))) < 0.5f);
        float cellW = 0.0f;
        float cell2X = 0.0f;
        ui_kit::columnX(w - 56.0f, 16.0f, &cellW, &cell2X);
        DOCTEST_CHECK(std::fabs(name->frame.x - (28.0f + cell2X)) < 0.5f);

        // 格三行都包在卡高内（0.5 容差）
        DOCTEST_CHECK(name->frame.y + name->frame.height <=
                      card->frame.y + card->frame.height + 0.5f);
        DOCTEST_CHECK(desc->frame.y + desc->frame.height <=
                      card->frame.y + card->frame.height + 0.5f);
        DOCTEST_CHECK(track->frame.y + track->frame.height <=
                      card->frame.y + card->frame.height + 0.5f);
        // 下边距契约：开关底 ≤ 卡底 − roundFs(16)
        DOCTEST_CHECK(track->frame.y + track->frame.height <=
                      card->frame.y + card->frame.height -
                          ms::roundFs(16.0f) + 0.5f);
        // 44×24 字面值 + 右贴格右缘（cellW/cell2X = columnX(w−56, 16) 单一来源）
        DOCTEST_CHECK_EQ(track->frame.width, 44.0f);
        DOCTEST_CHECK_EQ(track->frame.height, 24.0f);
        DOCTEST_CHECK(std::fabs(track->frame.x -
                                (28.0f + cell2X + cellW - 44.0f)) < 0.5f);
        // 开关行内偏移 +6：track y = 格 y + 6
        DOCTEST_CHECK(std::fabs(track->frame.y - name->frame.y - 6.0f) <
                      0.5f);

        // 回调接线：off → on，带壁纸模块 id
        DOCTEST_REQUIRE(track->onClick != nullptr);
        track->onClick();
        DOCTEST_CHECK_EQ(toggles, 1);
        DOCTEST_CHECK_EQ(lastId, std::string(ms::kModuleIdWallpaper));
        DOCTEST_CHECK(lastOn);
    }
    ::core::theme::setDark();
    return true;
})