// tests/test_modules/test_settings.cpp
// P13 设置页单测：SettingsStore（config.json 读写）纯逻辑 + 模块 compose 无头冒烟
// 覆盖：默认值 / 保存-加载往返 / 损坏 JSON 回退 / 数值钳制 / 主题字段映射 /
//       设置页可渲染（DSL 元素树构建）/ 主题切换对下一次 compose 即时生效
#include "doctest.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern "C" ::core::Module* create_settings_module();

namespace {

// 每个用例独立的临时配置文件（避免用例间串扰）
std::filesystem::path tempConfigFile(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("devdesk_test_settings_" + std::string(name) + ".json");
}

void removeQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

} // namespace

REGISTER_TEST(settings_defaults_on_missing_file, "settings: missing file -> defaults", {
    const auto path = tempConfigFile("missing");
    removeQuietly(path);
    module::settings::SettingsStore store(path);
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);                        // 默认深色（方案 B Dark）
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);
    return true;
})

REGISTER_TEST(settings_save_load_roundtrip, "settings: save/load roundtrip", {
    const auto path = tempConfigFile("roundtrip");
    removeQuietly(path);
    module::settings::SettingsStore store(path);

    module::settings::SettingsConfig cfg;
    cfg.darkTheme = false;               // 浅色
    cfg.fontScalePercent = 115;          // 大
    cfg.logRetentionDays = 90;
    auto saved = store.save(cfg);
    DOCTEST_REQUIRE(saved.isOk());

    const auto loaded = store.load();
    DOCTEST_CHECK_FALSE(loaded.darkTheme);
    DOCTEST_CHECK_EQ(loaded.fontScalePercent, 115);
    DOCTEST_CHECK_EQ(loaded.logRetentionDays, 90);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_zero_days_permanent, "settings: 0 days = permanent retention", {
    const auto path = tempConfigFile("zero");
    removeQuietly(path);
    module::settings::SettingsStore store(path);

    module::settings::SettingsConfig cfg;
    cfg.logRetentionDays = 0;            // 0 = 永久保留（合法值，不得被钳成默认）
    auto saved = store.save(cfg);
    DOCTEST_REQUIRE(saved.isOk());

    const auto loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.logRetentionDays, 0);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_corrupted_json_fallback, "settings: corrupted json -> defaults", {
    const auto path = tempConfigFile("corrupt");
    removeQuietly(path);
    {
        std::ofstream f(path, std::ios::trunc);
        f << "{ this is not valid json !!!";
        f.close();
    }
    module::settings::SettingsStore store(path);
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_partial_file_uses_defaults, "settings: partial file -> per-field defaults", {
    const auto path = tempConfigFile("partial");
    removeQuietly(path);
    {
        // 只有 theme 字段；fontScale / logRetention 缺失 → 各自默认值
        std::ofstream f(path, std::ios::trunc);
        f << "{\"theme\": \"light\"}";
        f.close();
    }
    module::settings::SettingsStore store(path);
    const auto cfg = store.load();
    DOCTEST_CHECK_FALSE(cfg.darkTheme);            // theme=light 生效
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);   // 缺失 → 默认
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);    // 缺失 → 默认
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_theme_value_mapping, "settings: theme string mapping", {
    const auto path = tempConfigFile("theme");
    removeQuietly(path);
    {
        std::ofstream f(path, std::ios::trunc);
        f << "{\"theme\": \"dark\"}";
        f.close();
    }
    module::settings::SettingsStore store(path);
    DOCTEST_CHECK(store.load().darkTheme);

    {
        std::ofstream f(path, std::ios::trunc);
        f << "{\"theme\": \"light\"}";
        f.close();
    }
    DOCTEST_CHECK_FALSE(store.load().darkTheme);

    {
        // 未知字符串 → 安全回退深色（默认值）
        std::ofstream f(path, std::ios::trunc);
        f << "{\"theme\": \"whatever\"}";
        f.close();
    }
    DOCTEST_CHECK(store.load().darkTheme);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_clamp_out_of_range, "settings: out-of-range values clamped", {
    // sanitized 直接钳制（load/save 内部都会调用）
    module::settings::SettingsConfig lo;
    lo.fontScalePercent = 10;              // 低于 85
    lo.logRetentionDays = -5;              // 负数
    const auto loS = module::settings::SettingsStore::sanitized(lo);
    DOCTEST_CHECK_EQ(loS.fontScalePercent, module::settings::kMinFontScalePercent);
    DOCTEST_CHECK_EQ(loS.logRetentionDays, 0);

    module::settings::SettingsConfig hi;
    hi.fontScalePercent = 400;             // 高于 130
    hi.logRetentionDays = 99999;           // 高于 3650
    const auto hiS = module::settings::SettingsStore::sanitized(hi);
    DOCTEST_CHECK_EQ(hiS.fontScalePercent, module::settings::kMaxFontScalePercent);
    DOCTEST_CHECK_EQ(hiS.logRetentionDays, module::settings::kMaxLogRetentionDays);
    return true;
})

REGISTER_TEST(settings_save_clamps_before_write, "settings: save clamps, load returns clamped", {
    const auto path = tempConfigFile("clampwrite");
    removeQuietly(path);
    module::settings::SettingsStore store(path);

    module::settings::SettingsConfig cfg;
    cfg.fontScalePercent = 999;            // save 时应钳到 130
    auto saved = store.save(cfg);
    DOCTEST_REQUIRE(saved.isOk());

    const auto loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.fontScalePercent, module::settings::kMaxFontScalePercent);
    removeQuietly(path);
    return true;
})

// ---- 模块 compose 无头冒烟（DSL 元素树构建，无需窗口/GL） ----

REGISTER_TEST(settings_module_compose_renders, "settings: module compose builds page (headless)", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读取真实 config.json（只读；无文件则默认值）
    // 注意：REGISTER_TEST 宏体内不能出现裸逗号（花括号初始化列表会被预处理器
    // 当成宏参数分隔符），Screen 用字段赋值构造
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("smoke");
    mod->compose(ui, screen);
    ui.end();

    // 关键元素存在 = 设置页可渲染（背景 + 4 张设置卡）
    DOCTEST_CHECK(ui.find("settings.pagebg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.directory.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.proxy.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log.bg") != nullptr);
    // ClaudeService::global() 为 null（测试环境）时代理卡仍应渲染
    DOCTEST_CHECK(ui.find("settings.card.proxy.token.value") != nullptr);

    mod->onStop();
    ::core::theme::setDark();  // 恢复全局默认主题，不影响其他用例
    return true;
})

REGISTER_TEST(settings_theme_switch_reflects, "settings: theme switch reflects on recompose", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;

    // 深色主题 compose 一次，取页面背景色（Element 在下一次 begin 时销毁，先拷值）
    ::core::theme::setDark();
    ui.begin("dark");
    mod->compose(ui, screen);
    ui.end();
    const auto* darkBg = ui.find("settings.pagebg");
    DOCTEST_REQUIRE(darkBg != nullptr);
    const float darkR = darkBg->color.r;

    // 切浅色再 compose：背景色应随之变化（与运行时"每帧读 current()"同一机制）
    ::core::theme::setLight();
    ui.begin("light");
    mod->compose(ui, screen);
    ui.end();
    const auto* lightBg = ui.find("settings.pagebg");
    DOCTEST_REQUIRE(lightBg != nullptr);
    const float lightR = lightBg->color.r;

    DOCTEST_CHECK(darkR != lightR);
    ::core::theme::setDark();  // 恢复
    return true;
})
