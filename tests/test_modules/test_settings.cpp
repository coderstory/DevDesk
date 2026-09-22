// tests/test_modules/test_settings.cpp
// P13 设置页单测：SettingsStore（config.json 读写）纯逻辑 + 模块 compose 无头冒烟
// 覆盖：默认值 / 保存-加载往返 / 损坏 JSON 回退 / 数值钳制 / 主题字段映射 /
//       设置页可渲染（DSL 元素树构建）/ 主题切换对下一次 compose 即时生效 /
//       日志保留下拉弹层不出窗体（openUp）/ 切换档位立即清理 / 文本垂直居中
#include "doctest.h"
#include "settings_config.h"

#include "core/database.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <ctime>
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

// 环境变量设置（Windows 用 _putenv_s，空串清除；与 test_markdown 同款）
void setEnvVar(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

// RAII：临时 config.json（DEVDESK_CONFIG_PATH）+ 临时 DB（appDatabase），
// 析构注销/清除，fail-fast 断言提前返回也不污染其他用例
struct PruneEnv {
    std::filesystem::path cfgPath;
    std::filesystem::path dbPath;
    std::unique_ptr<::core::Database> db;

    explicit PruneEnv(const char* tag) {
        cfgPath = std::filesystem::temp_directory_path() /
                  ("devdesk_test_settings_" + std::string(tag) + "_cfg.json");
        dbPath = std::filesystem::temp_directory_path() /
                 ("devdesk_test_settings_" + std::string(tag) + ".db");
        removeQuietly(cfgPath);
        removeQuietly(dbPath);
        auto r = ::core::Database::open(dbPath);
        if (r.isFail()) return;
        db = std::make_unique<::core::Database>(std::move(r.unsafeValue()));
        if (db->migrate().isFail()) {
            db.reset();
            return;
        }
        ::core::registerAppDatabase(&db->db());
        setEnvVar("DEVDESK_CONFIG_PATH", cfgPath.string());
    }

    ~PruneEnv() {
        setEnvVar("DEVDESK_CONFIG_PATH", "");
        ::core::registerAppDatabase(nullptr);
        removeQuietly(cfgPath);
        removeQuietly(dbPath);
    }
};

} // namespace

REGISTER_TEST(settings_defaults_on_missing_file, "settings: missing file -> defaults", {
    const auto path = tempConfigFile("missing");
    removeQuietly(path);
    module::settings::SettingsStore store(path);
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);                        // 默认深色（方案 B Dark）
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);  // W9 默认（em 语义修复后）
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
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);  // 解析失败 → 默认
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

// W9 一次性重置：EUI-NEO 0.7.0 修字号 em 语义后文字放大约 33%，
// 旧键 fontScalePercent / fontScalePercentV2 的存量值（85/115/130）都是
// 缩水语义下选的 → 一律忽略、回落默认 100；V3 键下的值按字面生效
REGISTER_TEST(settings_legacy_fontscale_migration, "settings: legacy font scale keys reset to 100", {
    const auto path = tempConfigFile("legacy");
    removeQuietly(path);
    {
        std::ofstream f(path, std::ios::trunc);
        f << "{\"theme\": \"dark\", \"fontScalePercent\": 85}";
        f.close();
    }
    module::settings::SettingsStore store(path);
    DOCTEST_CHECK_EQ(store.load().fontScalePercent, 100);  // 最老键 → 重置

    {
        std::ofstream f(path, std::ios::trunc);
        f << "{\"fontScalePercentV2\": 130}";
        f.close();
    }
    DOCTEST_CHECK_EQ(store.load().fontScalePercent, 100);  // V2 键 → 重置

    {
        // V3 键按字面生效（旧键同时存在也被忽略）
        std::ofstream f(path, std::ios::trunc);
        f << "{\"fontScalePercent\": 85, \"fontScalePercentV2\": 130, \"fontScalePercentV3\": 115}";
        f.close();
    }
    DOCTEST_CHECK_EQ(store.load().fontScalePercent, 115);
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

// ---- 日志保留下拉弹层不被窗体裁剪（W8：小窗口下自动向上展开） ----

REGISTER_TEST(settings_log_dropdown_popup_stays_in_window, "settings: log dropdown popup stays in window (openUp)", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    // 最小窗口 1200×720：日志卡贴近底部，向下展开的弹层会超出窗体，
    // 应改向上展开（弹层整体留在窗体内且位于字段上方）
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 720.0f;
    eui::Ui ui;
    ui.begin("small");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* field = ui.find("settings.card.log.dd.field");
    const auto* popup = ui.find("settings.card.log.dd.popup");
    DOCTEST_REQUIRE(field != nullptr);
    DOCTEST_REQUIRE(popup != nullptr);
    DOCTEST_CHECK(popup->frame.y >= 0.0f);
    DOCTEST_CHECK(popup->frame.y + popup->frame.height <= screen.height);
    DOCTEST_CHECK(popup->frame.y + popup->frame.height <= field->frame.y);

    // 大窗口 1600×1000：下方空间充足，保持默认向下展开（弹层在字段下方）
    eui::Screen tall;
    tall.width = 1600.0f;
    tall.height = 1000.0f;
    ui.begin("tall");
    mod->compose(ui, tall);
    ui.end();
    ui.layout(tall.width, tall.height);
    const auto* tallField = ui.find("settings.card.log.dd.field");
    const auto* tallPopup = ui.find("settings.card.log.dd.popup");
    DOCTEST_REQUIRE(tallField != nullptr);
    DOCTEST_REQUIRE(tallPopup != nullptr);
    DOCTEST_CHECK(tallPopup->frame.y >=
                  tallField->frame.y + tallField->frame.height);
    DOCTEST_CHECK(tallPopup->frame.y + tallPopup->frame.height <=
                  tall.height);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 代理 token 文本框文字垂直居中（W8） ----

REGISTER_TEST(settings_token_text_vertically_centered, "settings: token text vertically centered", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("center");
    mod->compose(ui, screen);
    ui.end();

    // token 值与目录路径都是"可见圆角框内的单行文本"，必须显式 Center
    //（缺省 Top 会贴框顶，W7 侧边栏同款问题）
    const auto* token = ui.find("settings.card.proxy.token.value");
    const auto* path = ui.find("settings.card.directory.path.value");
    DOCTEST_REQUIRE(token != nullptr);
    DOCTEST_REQUIRE(path != nullptr);
    DOCTEST_CHECK(token->verticalAlign == eui::VerticalAlign::Center);
    DOCTEST_CHECK(path->verticalAlign == eui::VerticalAlign::Center);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 切换保留档位立即执行一次清理（W8：此前仅启动时清理） ----

REGISTER_TEST(settings_retention_change_prunes_immediately, "settings: retention change prunes immediately", {
    // 隔离：临时 config.json（DEVDESK_CONFIG_PATH）+ 临时 DB（appDatabase）
    PruneEnv env("prune");
    DOCTEST_REQUIRE(env.db != nullptr);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& sq = env.db->db();
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, created_at, updated_at) VALUES "
        "('p1', 'http://x', 'sk-***', 'chk', 'acct', 'm', " +
        std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());
    // 40 天前 ×2 + 5 天前 ×1：切到「30 天」应立即删掉前两条
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) VALUES "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 40LL * 86400) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 35LL * 86400) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 5LL * 86400) + ")"
    ).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("prune");
    mod->compose(ui, screen);
    ui.end();

    // 点下拉第 3 项（「30 天」）——直接调用元素的 onClick（无头等价点击）
    const auto* item = ui.find("settings.card.log.dd.item.2");
    DOCTEST_REQUIRE(item != nullptr);
    DOCTEST_REQUIRE(item->onClick != nullptr);
    item->onClick();

    // DB 立即只剩 5 天前那条
    auto prep = sq.prepare("SELECT COUNT(*) AS n FROM claude_call_logs");
    DOCTEST_REQUIRE(prep.isOk());
    DOCTEST_CHECK(prep.value().step().isOk());
    auto row = prep.value().currentRow();
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.unsafeValue().getInt64("n").unsafeValue(),
                     static_cast<int64_t>(1));

    // 状态反馈「已清理 2 条」渲染在副标题
    ui.begin("prune_status");
    mod->compose(ui, screen);
    ui.end();
    const auto* subtitle = ui.find("settings.subtitle");
    DOCTEST_REQUIRE(subtitle != nullptr);
    DOCTEST_CHECK(subtitle->text.find("已清理 2 条") != std::string::npos);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})
