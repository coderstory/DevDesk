// tests/test_modules/test_module_gating.cpp
// 模块插拔单测：enabledModules 配置语义（缺省/往返/脏数据/未知 ID）+ 注册门控 + Home 降级渲染。
// 跨文件前提：
//   - home 与 settings 恒启用：moduleEnabled 谓词里硬编码，sanitized 也会强制补齐，
//     两者都不是配置里的普通项，从 enabledModules 里删掉也照样开着。
//   - agent 默认关（跑 LLM 有成本）、wallpaper 默认关。默认清单是 6 项（含 release），
//     改动会让本文件的 size()==6 断言红。
//   - 生产装配在 main.cpp getRouter()，本文件的注册段是它的复制品；改了那边这里不会自动跟。
#include "doctest.h"
#include "test_support.h"
#include "settings_config.h"

#include "core/app_config.h"
#include "core/module_interface.h"
#include "core/router.h"

#include <eui/dsl.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_home_module();

namespace {

class StubModule : public ::core::Module {
public:
    StubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }

private:
    ::core::PageId page_;
    std::string name_;
};

// 每个用例独立的临时配置路径 —— 共享工厂，只造路径不落盘。
// 配置真落在 app_config 表：各用例里那对 tempConfigFile / filesystem::remove 都是空操作，
// 用例间的隔离靠 TestEnv 换库 + seedConfigJsonRows 的整份替换语义。
constexpr const char* kCfgPrefix = "devdesk_test_gating_";

// 把「手改过的配置」按顶层键分发进 app_config 表各行（顶层键 = 行键）。
// 返回 bool 而非直接断言：doctest 宏展开后含 `return false`，只能在返回 bool 的
// 函数里用，所以断言留给调用方。
bool writeRawConfig(const std::string& json) {
    return test_support::seedConfigJsonRows(
        ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(),
        json);
}

// 手改配置若模拟「当前版本」写出的内容，必须带迁移标记——否则 load 按存量升级
// 路径把 release 并入清单（见 gating_module_defaults_migration），下面的过滤/
// 补齐断言会被迁移行为污染。body 是 JSON 对象的其余字段（逗号分隔）。
std::string currentVersionSeed(const std::string& body) {
    return "{\"moduleDefaultsVersion\": " +
           std::to_string(module::settings::kModuleDefaultsVersion) + ", " + body +
           "}";
}

} // namespace

// enabledModules 配置

REGISTER_TEST(gating_config_defaults, "gating: enabledModules 缺省默认清单（agent 默认关）+ 恒启用项", {
    using namespace module::settings;
    const SettingsConfig cfg;  // 默认构造 = 字段缺失的回退语义
    DOCTEST_CHECK_EQ(cfg.enabledModules.size(), static_cast<size_t>(6));
    for (const char* id : {module::settings::kModuleIdHome,
                           module::settings::kModuleIdMarkdown,
                           module::settings::kModuleIdTodo,
                           module::settings::kModuleIdClaudeProxy,
                           module::settings::kModuleIdSettings,
                           module::settings::kModuleIdRelease}) {
        DOCTEST_CHECK(moduleEnabled(cfg, id));
    }
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdAgent));
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdWallpaper));
    // wallpaper 是已知 ID：sanitized 保留显式启用，但绝不自动补进默认清单
    SettingsConfig withWallpaper = cfg;
    withWallpaper.enabledModules.push_back(kModuleIdWallpaper);
    DOCTEST_CHECK(moduleEnabled(SettingsStore::sanitized(withWallpaper),
                                kModuleIdWallpaper));
    SettingsConfig stripped = cfg;
    stripped.enabledModules.assign(1, kModuleIdMarkdown);
    const SettingsConfig fixed = SettingsStore::sanitized(stripped);
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdSettings));
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdWallpaper));
    // 未知 ID 恒 false（防手改 config 的兜底）
    DOCTEST_CHECK(!moduleEnabled(fixed, "bogus_module"));
    return true;
})

// B1（T01 审计）：存量配置升级路径。旧版本写出的清单没有 moduleDefaultsVersion
// 标记，其缺席的 release 代表「从未存在」→ 迁移并入；用户显式关闭一定发生在
// 迁移之后（配置已带当前标记），绝不能被重新打开。
REGISTER_TEST(gating_module_defaults_migration,
              "gating: 存量 5 项配置升级 → release 默认启用并写版本标记；关掉后不再重开", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "migrate", /*registerAppDb=*/true);
    const SettingsStore store(::core::appDatabase());
    const std::string versionKey =
        std::string(SettingsStore::keyNamespace()) + "moduleDefaultsVersion";

    // ① 旧版本形态：5 项清单、无版本标记
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
        "\"claude_proxy\", \"settings\"]}"));
    const SettingsConfig upgraded = store.load();
    DOCTEST_CHECK(moduleEnabled(upgraded, kModuleIdRelease));  // 升级即开
    DOCTEST_CHECK_EQ(upgraded.enabledModules.size(), static_cast<size_t>(6));
    // 迁移标记已回写 → 下次 load 不再迁移
    DOCTEST_CHECK_EQ(test_support::readRawConfigRow(::core::appDatabase(),
                                                    versionKey.c_str()),
                     std::to_string(kModuleDefaultsVersion));

    // ② 用户显式关掉 release 后落盘（save 持久化当前版本标记）→ 再 load 不得重开
    SettingsConfig userOff;
    userOff.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdTodo,
                              kModuleIdClaudeProxy, kModuleIdSettings};
    DOCTEST_REQUIRE(store.save(userOff).isOk());
    const SettingsConfig reloaded = store.load();
    DOCTEST_CHECK_FALSE(moduleEnabled(reloaded, kModuleIdRelease));
    DOCTEST_CHECK_EQ(reloaded.enabledModules.size(), static_cast<size_t>(5));

    // ③ 旧配置里用户关掉的既有模块也不得被迁移连带重开
    DOCTEST_REQUIRE(writeRawConfig(
        "{\"enabledModules\": [\"home\", \"settings\"]}"));
    const SettingsConfig sparse = store.load();
    DOCTEST_CHECK(moduleEnabled(sparse, kModuleIdRelease));
    DOCTEST_CHECK_FALSE(moduleEnabled(sparse, kModuleIdTodo));
    DOCTEST_CHECK_FALSE(moduleEnabled(sparse, kModuleIdMarkdown));
    return true;
})

REGISTER_TEST(gating_config_roundtrip, "gating: enabledModules 保存-加载往返", {
    using namespace module::settings;
    // registerAppDb 不可省：SettingsStore 无参构造经 ::core::appDatabase() 取连接，
    // 未注册时 load() 直接失败，下面的断言会全部走回退分支而绿。
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
    SettingsConfig cfg;
    cfg.enabledModules = {kModuleIdMarkdown, kModuleIdHome, kModuleIdSettings};
    const SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK(store.save(cfg).isOk());
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdClaudeProxy));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_unknown_ids_filtered, "gating: 未知模块 ID 忽略 + 恒启用补齐", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "unknown");
    // 手改 config 写入未知 ID + 漏掉恒启用项 + 重复项
    DOCTEST_REQUIRE(writeRawConfig(currentVersionSeed(
        "\"enabledModules\": [\"home\", \"todo\", \"bogus\", \"todo\"]")));
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));  // 补齐
    DOCTEST_CHECK(!moduleEnabled(loaded, "bogus"));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_empty_array, "gating: 空数组 = 四工具模块全关（home/settings 补齐）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "empty");
    DOCTEST_REQUIRE(writeRawConfig(currentVersionSeed("\"enabledModules\": []")));
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdClaudeProxy));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdAgent));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_corrupt_falls_back, "gating: 字段损坏回退默认全启用", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "corrupt");
    // 数组里的非字符串项被跳过，但数组本身有效 → 列表按字面生效（只含 todo）
    DOCTEST_REQUIRE(writeRawConfig(currentVersionSeed(
        "\"enabledModules\": [\"todo\", 42, true]")));
    SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdMarkdown));
    // 整份内容非 JSON → 全默认（默认清单；agent 默认关）
    // 走原始行通道而非 writeRawConfig：seedConfigJsonRows 要求顶层是对象，
    // 遇到非对象返回 false，「值本身是坏的」这条负例过不去。
    const std::string kEnabledModulesRow =
        std::string(module::settings::SettingsStore::keyNamespace()) +
        "enabledModules";
    DOCTEST_REQUIRE(core::appconfig::eraseWithPrefixExcept(
                        *::core::appDatabase(),
                        module::settings::SettingsStore::keyNamespace(), {})
                        .isOk());
    DOCTEST_REQUIRE(test_support::writeRawConfigRow(
        ::core::appDatabase(), kEnabledModulesRow.c_str(), "{not json at all"));
    loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(6));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMarkdown));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

// 注册门控

REGISTER_TEST(gating_registration_respects_config, "gating: 关闭模块不注册进 Router（modules() 不含它）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "gating", /*registerAppDb=*/true);
    (void)0;
    const auto file = test_support::tempConfigFile(kCfgPrefix, "registration");
    SettingsConfig cfg;
    cfg.enabledModules = {kModuleIdHome, kModuleIdMarkdown, kModuleIdSettings};
    DOCTEST_CHECK(SettingsStore(::core::appDatabase()).save(cfg).isOk());

    // 复制 main.cpp getRouter() 的装配门控；那边改了这里不会自动跟
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule markdown(::core::PageId::Markdown, "markdown");
    StubModule todo(::core::PageId::Todo, "todo");
    StubModule claude(::core::PageId::ClaudeProxy, "claude");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);  // 恒注册
    if (moduleEnabled(loaded, kModuleIdMarkdown)) router.registerModule(&markdown);
    if (moduleEnabled(loaded, kModuleIdTodo)) router.registerModule(&todo);
    if (moduleEnabled(loaded, kModuleIdClaudeProxy)) router.registerModule(&claude);
    router.registerModule(&settings);  // 恒注册

    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(3));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Home));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Markdown));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Settings));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::Todo));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::ClaudeProxy));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

// Home 降级渲染

REGISTER_TEST(gating_home_all_modules_off, "gating: Home 三模块全关 → 卡片/按钮全隐藏 + 空态文案", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&settings);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_all_off");
    mod->compose(ui, screen);
    ui.end();

    // 5 张统计卡全隐
    DOCTEST_CHECK(ui.find("home.stat.tokens") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.notes") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.todo") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.overdue") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.api") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newnote") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    // 一个工具模块都没注册时的兜底文案
    const auto* empty = ui.find("home.stats.alloff");
    DOCTEST_REQUIRE(empty != nullptr);
    DOCTEST_CHECK(empty->text.find("所有工具模块已关闭") != std::string::npos);
    return true;
})

REGISTER_TEST(gating_home_partial_modules, "gating: Home 部分模块关闭 → 对应卡隐藏、其余保留", {
    // 只启用 markdown（todo/claude 关闭）
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule markdown(::core::PageId::Markdown, "markdown");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&markdown);
    router.registerModule(&settings);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_md_only");
    mod->compose(ui, screen);
    ui.end();

    // 卡片可见性按目标模块是否注册，跨模块不互相牵连
    DOCTEST_CHECK(ui.find("home.stat.notes") != nullptr);
    DOCTEST_CHECK(ui.find("home.act.newnote") != nullptr);
    DOCTEST_CHECK(ui.find("home.stat.tokens") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.api") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.todo") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.overdue") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
    // 部分启用不出空态文案
    DOCTEST_CHECK(ui.find("home.stats.alloff") == nullptr);
    return true;
})

REGISTER_TEST(gating_home_all_on_unchanged, "gating: 全注册时 Home 与现状等价（5 卡 + 3 按钮）", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule markdown(::core::PageId::Markdown, "markdown");
    StubModule todo(::core::PageId::Todo, "todo");
    StubModule claude(::core::PageId::ClaudeProxy, "claude");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&markdown);
    router.registerModule(&todo);
    router.registerModule(&claude);
    router.registerModule(&settings);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_all_on");
    mod->compose(ui, screen);
    ui.end();

    for (const char* id : {"home.stat.tokens", "home.stat.notes", "home.stat.todo",
                           "home.stat.overdue", "home.stat.api",
                           "home.act.newnote", "home.act.newtodo", "home.act.claude"}) {
        DOCTEST_CHECK(ui.find(id) != nullptr);
    }
    DOCTEST_CHECK(ui.find("home.stats.alloff") == nullptr);
    return true;
})

// G4 补缺（docs/superpowers/plans/2026-10-19-ui-test-gap-map.md）：home.actions 与
// 模块注册态的**联动**——既有用例只断按钮在场/缺席，这里钉三档收缩几何：
//   ① 全注册：三按钮等宽（UAT2 覆盖 W9 不等宽：权重全 1.0），行宽 = contentW；
//   ② 只剩 markdown：幸存按钮独占整行（宽 = ①行宽 = contentW，权重归一）；
//   ③ 全关：home.actions 整行元素不存在（不是空行占位）。
// contentW 两次 compose 间不变（同 screen 尺寸同代码路径），①②可互推。
REGISTER_TEST(gating_home_actions_row_collapse,
              "gating: Home 快捷按钮联动收缩（等宽）/ 全关整行隐藏", {
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    float rowW = 0.0f;  // ① 行宽（= contentW，跨档复算用）

    // ① 全注册：三按钮等宽（UAT2 覆盖 W9 不等宽：权重全 1.0）
    {
        ::core::Router router;
        StubModule home(::core::PageId::Home, "home");
        StubModule markdown(::core::PageId::Markdown, "markdown");
        StubModule todo(::core::PageId::Todo, "todo");
        StubModule claude(::core::PageId::ClaudeProxy, "claude");
        StubModule settings(::core::PageId::Settings, "settings");
        router.registerModule(&home);
        router.registerModule(&markdown);
        router.registerModule(&todo);
        router.registerModule(&claude);
        router.registerModule(&settings);
        test_support::ScopedAppRouter scoped(&router);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Ui ui;
        ui.begin("act_all3");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);

        const auto* row = ui.find("home.actions");
        DOCTEST_REQUIRE(row != nullptr);
        const auto* note = ui.find("home.act.newnote");
        const auto* td = ui.find("home.act.newtodo");
        const auto* cl = ui.find("home.act.claude");
        DOCTEST_REQUIRE(note != nullptr);
        DOCTEST_REQUIRE(td != nullptr);
        DOCTEST_REQUIRE(cl != nullptr);
        rowW = row->frame.width;  // = contentW
        DOCTEST_CHECK(rowW > 0.0f);
        // UAT2 覆盖 W9：三按钮等宽（权重全 1.0），newnote == newtodo == claude
        DOCTEST_CHECK(std::fabs(td->frame.width - note->frame.width) < 0.5f);
        DOCTEST_CHECK(std::fabs(cl->frame.width - note->frame.width) < 0.5f);
        // 行宽 = 三按钮宽 + 2×16 gap
        DOCTEST_CHECK(std::fabs(row->frame.width -
                                (note->frame.width + td->frame.width +
                                 cl->frame.width + 32.0f)) < 1.0f);
    }

    // ② 只剩 markdown：newnote 独占整行，兄弟按钮消失
    {
        ::core::Router router;
        StubModule home(::core::PageId::Home, "home");
        StubModule markdown(::core::PageId::Markdown, "markdown");
        StubModule settings(::core::PageId::Settings, "settings");
        router.registerModule(&home);
        router.registerModule(&markdown);
        router.registerModule(&settings);
        test_support::ScopedAppRouter scoped(&router);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Ui ui;
        ui.begin("act_md_only");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);

        const auto* row = ui.find("home.actions");
        DOCTEST_REQUIRE(row != nullptr);
        const auto* note = ui.find("home.act.newnote");
        DOCTEST_REQUIRE(note != nullptr);
        DOCTEST_REQUIRE_MESSAGE(
            std::fabs(note->frame.width - rowW) < 1.0f,
            "关掉兄弟模块后幸存按钮未独占整行: " +
                std::to_string(note->frame.width) + " vs 期望 " +
                std::to_string(rowW));
        DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    }

    // ③ 全关：整行隐藏
    {
        ::core::Router router;
        StubModule home(::core::PageId::Home, "home");
        StubModule settings(::core::PageId::Settings, "settings");
        router.registerModule(&home);
        router.registerModule(&settings);
        test_support::ScopedAppRouter scoped(&router);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Ui ui;
        ui.begin("act_none");
        mod->compose(ui, screen);
        ui.end();

        DOCTEST_CHECK(ui.find("home.actions") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.newnote") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
        DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    }
    return true;
})
