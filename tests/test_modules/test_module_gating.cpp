// tests/test_modules/test_module_gating.cpp
// 模块插拔方案 A（设计 2026-09-22）单测：
//   1. enabledModules 配置：默认全启用 / 往返 / 未知 ID 过滤 / 恒启用项补齐 /
//      moduleEnabled 谓词（home/settings 恒 true）
//   2. 注册门控：按 moduleEnabled 注册（main.cpp getRouter 同款逻辑）后，
//      关闭模块不在 Router::modules()（零执行零菜单项的结构性保证）
//   3. Home 降级渲染：scoped appRouter 注入注册状态，无头 compose 断言
//      统计卡 / 快捷按钮 / 最近编辑按目标模块注册状态隐藏，全关出空态文案
#include "doctest.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/router.h"

#include <eui/dsl.h>

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

std::filesystem::path tempConfigFile(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("devdesk_test_gating_" + std::string(name) + ".json");
}

void writeRawConfig(const std::filesystem::path& file,
                    const std::string& json) {
    std::ofstream f(file, std::ios::trunc);
    f << json;
}

// scoped appRouter 注入（Home 降级渲染读注册状态）；离开作用域复位 null，
// 不影响后续无 Router 假设的用例（既有 home 测试 compose 时 appRouter()==null）
class ScopedAppRouter {
public:
    explicit ScopedAppRouter(::core::Router* router) {
        ::core::registerAppRouter(router);
    }
    ~ScopedAppRouter() { ::core::registerAppRouter(nullptr); }
};

// REGISTER_TEST 宏体内不能出现裸逗号（花括号初始化列表会被预处理器当成
// 宏参数分隔符），全量 ID 清单放宏外（test_settings.cpp 同款约定）
const char* const kAllModuleIds[] = {
    module::settings::kModuleIdHome,      module::settings::kModuleIdMarkdown,
    module::settings::kModuleIdTodo,      module::settings::kModuleIdClaudeProxy,
    module::settings::kModuleIdSettings,
};

// 全启用时 Home 应渲染的全部卡片/按钮元素 ID（gating_home_all_on_unchanged）
const char* const kHomeAllOnElementIds[] = {
    "home.stat.tokens", "home.stat.notes", "home.stat.todo",
    "home.stat.overdue", "home.stat.api", "home.recent",
    "home.act.newnote", "home.act.newtodo", "home.act.claude"};

} // namespace

// ============ 1. enabledModules 配置 ============

REGISTER_TEST(gating_config_defaults, "gating: enabledModules 缺省全启用 + 恒启用项", {
    using namespace module::settings;
    const SettingsConfig cfg;  // 默认构造 = 字段缺失的回退语义
    DOCTEST_CHECK_EQ(cfg.enabledModules.size(), static_cast<size_t>(5));
    for (const char* id : kAllModuleIds) {
        DOCTEST_CHECK(moduleEnabled(cfg, id));
    }
    // home/settings 从列表里删掉也恒启用（sanitized 强制补齐 + 谓词硬编码）
    SettingsConfig stripped = cfg;
    stripped.enabledModules.assign(1, kModuleIdMarkdown);
    const SettingsConfig fixed = SettingsStore::sanitized(stripped);
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdSettings));
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(fixed, kModuleIdTodo));
    // 未知 ID 恒 false（防手改 config 的兜底）
    DOCTEST_CHECK(!moduleEnabled(fixed, "bogus_module"));
    return true;
})

REGISTER_TEST(gating_config_roundtrip, "gating: enabledModules 保存-加载往返", {
    using namespace module::settings;
    const auto file = tempConfigFile("roundtrip");
    SettingsConfig cfg;
    cfg.enabledModules.assign(1, kModuleIdMarkdown);
    cfg.enabledModules.push_back(kModuleIdHome);
    cfg.enabledModules.push_back(kModuleIdSettings);
    const SettingsStore store(file);
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
    const auto file = tempConfigFile("unknown");
    // 手改 config 写入未知 ID + 漏掉恒启用项 + 重复项
    writeRawConfig(file,
                   "{\"enabledModules\": [\"home\", \"todo\", \"bogus\", "
                   "\"todo\"]}");
    const SettingsConfig loaded = SettingsStore(file).load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));  // 补齐
    DOCTEST_CHECK(!moduleEnabled(loaded, "bogus"));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_empty_array, "gating: 空数组 = 三工具模块全关（home/settings 补齐）", {
    using namespace module::settings;
    const auto file = tempConfigFile("empty");
    writeRawConfig(file, "{\"enabledModules\": []}");
    const SettingsConfig loaded = SettingsStore(file).load();
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdMarkdown));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdClaudeProxy));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdSettings));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

REGISTER_TEST(gating_config_corrupt_falls_back, "gating: 字段损坏回退默认全启用", {
    using namespace module::settings;
    const auto file = tempConfigFile("corrupt");
    // 数组里的非字符串项被跳过，但数组本身有效 → 列表按字面生效（只含 todo）
    writeRawConfig(file, "{\"enabledModules\": [\"todo\", 42, true]}");
    SettingsConfig loaded = SettingsStore(file).load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdTodo));
    DOCTEST_CHECK(!moduleEnabled(loaded, kModuleIdMarkdown));
    // 整个文件非 JSON → 全默认（全启用）
    writeRawConfig(file, "{not json at all");
    loaded = SettingsStore(file).load();
    DOCTEST_CHECK_EQ(loaded.enabledModules.size(), static_cast<size_t>(5));
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMarkdown));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return true;
})

// ============ 2. 注册门控（main.cpp getRouter 同款逻辑） ============

REGISTER_TEST(gating_registration_respects_config, "gating: 关闭模块不注册进 Router（modules() 不含它）", {
    using namespace module::settings;
    const auto file = tempConfigFile("registration");
    SettingsConfig cfg;
    cfg.enabledModules.clear();
    cfg.enabledModules.push_back(kModuleIdHome);
    cfg.enabledModules.push_back(kModuleIdMarkdown);
    cfg.enabledModules.push_back(kModuleIdSettings);
    DOCTEST_CHECK(SettingsStore(file).save(cfg).isOk());

    // 以下复刻 main.cpp getRouter() 的装配门控
    const SettingsConfig loaded = SettingsStore(file).load();
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

// ============ 3. Home 降级渲染（注册状态感知） ============

REGISTER_TEST(gating_home_all_modules_off, "gating: Home 三模块全关 → 卡片/按钮全隐藏 + 空态文案", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&settings);
    ScopedAppRouter scoped(&router);

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
    // 最近编辑 + 3 个快捷按钮全隐
    DOCTEST_CHECK(ui.find("home.recent") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newnote") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    // 空态兜底文案（设计 §5.5）
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
    ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_md_only");
    mod->compose(ui, screen);
    ui.end();

    // markdown 的卡保留
    DOCTEST_CHECK(ui.find("home.stat.notes") != nullptr);
    DOCTEST_CHECK(ui.find("home.recent") != nullptr);
    DOCTEST_CHECK(ui.find("home.act.newnote") != nullptr);
    // claude 的卡隐藏
    DOCTEST_CHECK(ui.find("home.stat.tokens") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.api") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.claude") == nullptr);
    // todo 的卡隐藏
    DOCTEST_CHECK(ui.find("home.stat.todo") == nullptr);
    DOCTEST_CHECK(ui.find("home.stat.overdue") == nullptr);
    DOCTEST_CHECK(ui.find("home.act.newtodo") == nullptr);
    // 部分启用不出空态文案
    DOCTEST_CHECK(ui.find("home.stats.alloff") == nullptr);
    return true;
})

REGISTER_TEST(gating_home_all_on_unchanged, "gating: 全注册时 Home 与现状等价（5 卡 + 3 按钮 + 最近编辑）", {
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
    ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("home_gated_all_on");
    mod->compose(ui, screen);
    ui.end();

    for (const char* id : kHomeAllOnElementIds) {
        DOCTEST_CHECK(ui.find(id) != nullptr);
    }
    DOCTEST_CHECK(ui.find("home.stats.alloff") == nullptr);
    return true;
})
