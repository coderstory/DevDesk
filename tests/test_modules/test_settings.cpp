// tests/test_modules/test_settings.cpp
// P13 设置页单测：SettingsStore（config.json 读写）纯逻辑 + 模块 compose 无头冒烟
// 覆盖：默认值 / 保存-加载往返 / 损坏 JSON 回退 / 数值钳制 / 主题字段映射 /
//       设置页可渲染（DSL 元素树构建）/ 主题切换对下一次 compose 即时生效 /
//       日志保留下拉弹层不出窗体（openUp）/ 切换档位立即清理 / 文本垂直居中 /
//       窗口效果三档（磨砂 Phase D）：默认值/往返/非法值收敛、档位→alpha/
//       backdrop 映射、降级提示矩阵、applyWindowVisuals 接线（无头）
#include "doctest.h"
#include "settings_config.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "platform/keychain.h"

#include <eui/app.h>
#include <eui/detail/dsl_app_impl.h>
#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>

extern "C" ::core::Module* create_settings_module();

// 窗口效果（磨砂 Phase D）让 module_settings 引用 app:: 运行时 API；app API
// 的实现 TU（glfw_app_main）只编进 DevDesk 主程序，测试侧包含 dsl_app_impl.h
// 自带实现（与 EUI-NEO 自身单测相同的注入方式），并提供它要求的
// app::dslAppConfig。app::compose 不再需要桩——src/app_sidebar.cpp 的真实现
// 已直接编进测试二进制（tests/CMakeLists.txt，侧边栏折叠单测驱动同一份）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}.title("devdesk_test_settings");
    return config;
}
} // namespace app

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

REGISTER_TEST(settings_proxy_autostart_default_off, "settings: claudeProxyAutoStart 默认 false + 读写往返", {
    // 硬性需求 1（provider-config Phase B）：默认不启动代理
    const auto path = tempConfigFile("autostart");
    removeQuietly(path);
    module::settings::SettingsStore store(path);

    // 默认 false
    DOCTEST_CHECK_FALSE(store.load().claudeProxyAutoStart);

    // 打开后持久化 + 读回 true
    module::settings::SettingsConfig cfg;
    cfg.claudeProxyAutoStart = true;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK(store.load().claudeProxyAutoStart);

    // 存量 config.json（无该键）→ 保持默认 false
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "light", "logRetentionDays": 30})";
    }
    DOCTEST_CHECK_FALSE(store.load().claudeProxyAutoStart);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_proxy_port_roundtrip_clamp, "settings: proxyPort 默认/读写往返/非法回落/输入解析", {
    using namespace module::settings;
    const auto path = tempConfigFile("proxyport");
    removeQuietly(path);
    SettingsStore store(path);

    auto withPort = [](int port) {
        SettingsConfig c;
        c.proxyPort = port;
        return c;
    };

    // 默认 8888（spec §8.5 承诺默认端口，审计 2026-09-23 [14]）
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);

    // 读写往返
    DOCTEST_REQUIRE(store.save(withPort(9000)).isOk());
    DOCTEST_CHECK_EQ(store.load().proxyPort, 9000);

    // 非法值回落默认（0 / 负数 / 越界）；边界合法值保留
    DOCTEST_REQUIRE(store.save(withPort(0)).isOk());
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(-1)).proxyPort, 8888);
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(70000)).proxyPort, 8888);
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(1)).proxyPort, 1);
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(65535)).proxyPort, 65535);

    // 存量 config.json 无 claudeProxyPort 键 → 默认 8888
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "dark", "claudeProxyAutoStart": true})";
    }
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);

    // 输入框解析（纯数字 + 范围）
    int out = 0;
    DOCTEST_CHECK(parseProxyPortInput("8888", out) && out == 8888);
    DOCTEST_CHECK(parseProxyPortInput("1", out) && out == 1);
    DOCTEST_CHECK(parseProxyPortInput("65535", out) && out == 65535);
    DOCTEST_CHECK_FALSE(parseProxyPortInput("", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput("0", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput("65536", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput("123456", out));   // 6 位封顶拒绝
    DOCTEST_CHECK_FALSE(parseProxyPortInput("88ab", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput(" 88", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput("8.8", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput("-1", out));
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_proxy_token_mask, "settings: maskProxyToken 默认脱敏策略（审计 [16]）", {
    using module::settings::maskProxyToken;

    // 常规 32-hex token：前 4 + *** + 后 4（provider-config §7 示例形态）
    const std::string token = "a3f8b2c1d4e5f60718293a4b5c6d7e8f";
    DOCTEST_CHECK_EQ(maskProxyToken(token), std::string("a3f8***7e8f"));
    // 掩码绝不等于原文、不含中段
    DOCTEST_CHECK(maskProxyToken(token) != token);
    DOCTEST_CHECK(maskProxyToken(token).find("18293") == std::string::npos);

    // 恰 8 位走前后缀分支
    DOCTEST_CHECK_EQ(maskProxyToken("abcdefgh"), std::string("abcd***efgh"));
    // 短于 8 位全掩且固定形态（不泄露真实长度、不输出真实字符）
    DOCTEST_CHECK_EQ(maskProxyToken("abcdefg"), std::string("****"));
    DOCTEST_CHECK_EQ(maskProxyToken("a"), std::string("****"));
    // 空串原样（调用方以「代理未运行」分支为主，防御式定义）
    DOCTEST_CHECK(maskProxyToken("").empty());

    // 「显形不持久化」结构性保证：token 与显形态都不进 SettingsConfig——
    // 任意 save 后 config.json 文本不含 token / revealed 键
    const auto path = tempConfigFile("tokenmask");
    removeQuietly(path);
    module::settings::SettingsStore store(path);
    module::settings::SettingsConfig cfg;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        DOCTEST_CHECK_FALSE(text.empty());
        DOCTEST_CHECK(text.find("token") == std::string::npos);
        DOCTEST_CHECK(text.find("revealed") == std::string::npos);
    }
    removeQuietly(path);
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

    // 关键元素存在 = 设置页可渲染（4 张设置卡；页面底色载体是主窗口
    // clearColor 不再自铺——settings.pagebg 已删，见下方回归守卫）
    DOCTEST_CHECK(ui.find("settings.card.appearance.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.effect.ddwrap") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.directory.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.proxy.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log.bg") != nullptr);
    // ClaudeService::global() 为 null（测试环境）时代理卡仍应渲染
    DOCTEST_CHECK(ui.find("settings.card.proxy.token.value") != nullptr);
    // W10 复审项：卡片区套 scrollView（4 卡总高 > 小窗口可视高，可滚动）
    DOCTEST_CHECK(ui.find("settings.cards.scroll") != nullptr);

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

    // 页面底色载体从 settings.pagebg（已删——自铺不透明全屏底会把帧缓冲
    // alpha 恒置 1，半透/磨砂档下主体看不见效果，用户实测根因）改为主窗口
    // clearColor：applyWindowVisuals 随主题重设，无头验证颜色跟随
    ::core::theme::setDark();
    module::settings::applyWindowVisuals(module::settings::kWindowEffectOff);
    const float darkClearR = app::currentClearColor().r;

    // 切浅色再应用：底色应随之变化（运行时切主题走 theme 回调同一函数）
    ::core::theme::setLight();
    module::settings::applyWindowVisuals(module::settings::kWindowEffectOff);
    const float lightClearR = app::currentClearColor().r;
    DOCTEST_CHECK(darkClearR != lightClearR);

    // 回归守卫：compose 不得再出现全屏自铺底（加回 = 主体效果全灭）
    ui.begin("light");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.pagebg") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.bg") != nullptr);

    ::core::theme::setDark();  // 恢复
    return true;
})

// ---- 日志保留下拉弹层不被窗体裁剪（W8：小窗口下自动向上展开） ----

REGISTER_TEST(settings_log_dropdown_popup_stays_in_window, "settings: log dropdown popup stays in window (openUp)", {
    // 隔离：不隔离会读到真机 config.json（如本机启用了 pet 卡），卡片变多
    // 把日志卡推到更低位，openUp 也装不下——测试必须与机器本地配置无关
    PruneEnv env("openup");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    // 模块插拔后设置页 5 卡（外观/模块/目录/代理/日志），日志卡在小窗里被
    // 滚出视口——取「日志卡贴近底部、字段可见」的最小窗口 1200×1000：
    // 向下展开的弹层会超出窗体，应改向上展开（弹层整体留在窗体内且位于
    // 字段上方）
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 1000.0f;
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

    // 大窗口 1600×1300：下方空间充足，保持默认向下展开（弹层在字段下方）
    eui::Screen tall;
    tall.width = 1600.0f;
    tall.height = 1300.0f;
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

// ---- Claude Code 接管状态行（provider-config Phase D，§6.3）----
// 数据源 ClaudeService（临时接管路径注入，绝不碰真实 ~/.claude）：
// 启动代理 → takeOver →「已接管 <路径>」+ 上层覆盖警告行；停止 →「未接管」
REGISTER_TEST(settings_takeover_status_row,
              "settings: Claude Code 接管状态行 + 覆盖警告（headless）", {
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / "devdesk_test_settings_takeover";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // 伪造 project 层覆盖文件（含 ANTHROPIC_BASE_URL → 检测应命中）
    const fs::path overlay = dir / "project_settings.json";
    {
        std::ofstream out(overlay);
        out << R"({"env":{"ANTHROPIC_BASE_URL":"https://evil.example.com"}})";
    }
    const fs::path cfgPath = dir / "config.json";

    auto r = ::core::Database::open(dir / "db.sqlite");
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<::core::Database>(std::move(r.unsafeValue()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    module::claude::ClaudeService svc(db->db(), "DevDeskTestTakeover");
    module::claude::ClaudeService::setGlobal(&svc);
    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.sidecarFile = dir / "snapshot.json";
    paths.overlayFiles.push_back(overlay);
    svc.setSettingsSyncPaths(paths);
    setEnvVar("DEVDESK_CONFIG_PATH", cfgPath.string());

    auto pr = svc.createProfile("接管", "https://up.example.com",
                                "sk-takeover-0001", "");
    DOCTEST_REQUIRE(pr.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pr.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // CHECK 宏内含 return false，不能进返回 string 的 lambda —— out 参数收敛
    auto composeTakeoverText = [&](std::string* out) -> bool {
        eui::Ui ui;
        ui.begin("takeover_status");
        mod->compose(ui, screen);
        ui.end();
        const auto* v = ui.find("settings.card.proxy.takeover.value");
        if (v == nullptr) return false;
        *out = v->text;
        return true;
    };

    // 启动前：未接管
    {
        std::string text;
        DOCTEST_CHECK(composeTakeoverText(&text));
        DOCTEST_CHECK(text.find("未接管") != std::string::npos);
    }

    // 启动代理 → takeOver → 已接管 + 路径 + 覆盖警告行
    auto start = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(start.isOk());
    {
        std::string text;
        DOCTEST_CHECK(composeTakeoverText(&text));
        DOCTEST_CHECK(text.find("已接管") != std::string::npos);
        DOCTEST_CHECK(text.find("settings.json") != std::string::npos);
        eui::Ui ui;
        ui.begin("takeover_warn");
        mod->compose(ui, screen);
        ui.end();
        const auto* warn = ui.find("settings.card.proxy.takeover.warn.0");
        DOCTEST_REQUIRE(warn != nullptr);   // overlay 含同名 env 键 → 命中
        DOCTEST_CHECK(warn->text.find("ANTHROPIC_BASE_URL") !=
                      std::string::npos);
    }

    // 停止代理 → 还原 → 未接管
    svc.stopProxy();
    {
        std::string text;
        DOCTEST_CHECK(composeTakeoverText(&text));
        DOCTEST_CHECK(text.find("未接管") != std::string::npos);
    }

    mod->onStop();
    module::claude::ClaudeService::setGlobal(nullptr);
    setEnvVar("DEVDESK_CONFIG_PATH", "");
    platform::keychain::deleteSecret(
        "DevDeskTestTakeover", "claude:" + std::to_string(pr.value().id));
    fs::remove_all(dir, ec);
    return true;
})

// ---- 窗口效果三档（磨砂 Phase D）----

// REGISTER_TEST 宏体内不能出现裸逗号，档位数组放宏外
namespace {
bool windowEffectRoundtripOne(module::settings::SettingsStore& store,
                              const char* mode) {
    module::settings::SettingsConfig cfg;
    cfg.windowEffect = mode;
    if (store.save(cfg).isFail()) return false;
    return store.load().windowEffect == mode;
}
} // namespace

REGISTER_TEST(settings_window_effect_defaults_and_roundtrip, "settings: windowEffect 默认 off + 三档读写往返", {
    const auto path = tempConfigFile("wineffect");
    removeQuietly(path);
    module::settings::SettingsStore store(path);

    // 默认 off；存量 config.json（无该键）同样回落 off
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "dark", "logRetentionDays": 30})";
    }
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));

    // 三档逐一保存-读回
    DOCTEST_CHECK(windowEffectRoundtripOne(store, module::settings::kWindowEffectOff));
    DOCTEST_CHECK(windowEffectRoundtripOne(store, module::settings::kWindowEffectTranslucent));
    DOCTEST_CHECK(windowEffectRoundtripOne(store, module::settings::kWindowEffectFrosted));

    // 非法值：save 前 sanitized 收敛为 off；手写进文件同样收敛
    module::settings::SettingsConfig bad;
    bad.windowEffect = "glass";
    DOCTEST_CHECK_EQ(module::settings::SettingsStore::sanitized(bad).windowEffect,
                     std::string(module::settings::kWindowEffectOff));
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "dark", "windowEffect": "vibrancy"})";
    }
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_window_effect_param_mapping, "settings: 档位→alpha/backdrop 请求映射（纯函数）", {
    namespace ms = module::settings;
    using WE = core::platform::WindowEffect;

    // clearColor alpha：off=1.0 / translucent=0.85 / frosted=0.55 / 未知→1.0
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectOff) == 1.0f);
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectTranslucent) == 0.85f);
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectFrosted) == 0.55f);
    DOCTEST_CHECK(ms::windowEffectAlpha("bogus") == 1.0f);

    // backdrop 请求：off/translucent 显式关（None），frosted→Acrylic，未知→None
    DOCTEST_CHECK(ms::windowEffectRequest(ms::kWindowEffectOff) == WE::None);
    DOCTEST_CHECK(ms::windowEffectRequest(ms::kWindowEffectTranslucent) == WE::None);
    DOCTEST_CHECK(ms::windowEffectRequest(ms::kWindowEffectFrosted) == WE::Acrylic);
    DOCTEST_CHECK(ms::windowEffectRequest("bogus") == WE::None);
    return true;
})

REGISTER_TEST(settings_window_effect_degraded_hint, "settings: 降级提示文案矩阵（纯函数）", {
    namespace ms = module::settings;
    using WE = core::platform::WindowEffect;

    // off：任何 active 都视觉等价（alpha=1），永不提示
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectOff, WE::None).empty());
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectOff, WE::Transparent).empty());
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectOff, WE::Acrylic).empty());

    // translucent：拿不到 Transparent 才提示回退不透明
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectTranslucent, WE::Transparent).empty());
    const std::string t1 = ms::windowEffectDegradedHint(ms::kWindowEffectTranslucent, WE::None);
    DOCTEST_CHECK(t1.find("回退为不透明") != std::string::npos);

    // frosted：拿不到 Acrylic 时按实际值提示（Transparent→回退半透，None→回退不透明）
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectFrosted, WE::Acrylic).empty());
    const std::string f1 = ms::windowEffectDegradedHint(ms::kWindowEffectFrosted, WE::Transparent);
    DOCTEST_CHECK(f1.find("回退为半透") != std::string::npos);
    const std::string f2 = ms::windowEffectDegradedHint(ms::kWindowEffectFrosted, WE::None);
    DOCTEST_CHECK(f2.find("回退为不透明") != std::string::npos);
    return true;
})

REGISTER_TEST(settings_window_effect_apply_visuals, "settings: applyWindowVisuals 接线（无头：app 运行时覆盖）", {
    namespace ms = module::settings;
    using WE = core::platform::WindowEffect;

    // 半透档：clearColor alpha=0.85 + backdrop 请求 None（显式关）
    ms::applyWindowVisuals(ms::kWindowEffectTranslucent);
    DOCTEST_CHECK(app::currentClearColor().a == 0.85f);
    DOCTEST_CHECK(app::currentWindowEffect() == WE::None);

    // 磨砂档：alpha=0.55 + Acrylic 请求
    ms::applyWindowVisuals(ms::kWindowEffectFrosted);
    DOCTEST_CHECK(app::currentClearColor().a == 0.55f);
    DOCTEST_CHECK(app::currentWindowEffect() == WE::Acrylic);

    // 关档：alpha=1（视觉等价实色，透明 hint 仍开）——收尾恢复，不污染后续用例
    ms::applyWindowVisuals(ms::kWindowEffectOff);
    DOCTEST_CHECK(app::currentClearColor().a == 1.0f);
    DOCTEST_CHECK(app::currentWindowEffect() == WE::None);
    return true;
})

// ============ 模块插拔（方案 A）：模块管理卡 + 卡片门控 ============

REGISTER_TEST(settings_modules_card_renders, "settings: 模块管理卡渲染（3 个工具模块开关）", {
    PruneEnv env("modcard");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读 PruneEnv 指向的临时 config（无文件 → 默认全启用）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("modcard");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.modules.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.1") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.2") != nullptr);
    // 描述文案含重启提示（设计 §5.4）
    const auto* desc = ui.find("settings.card.modules.desc");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_CHECK(desc->text.find("重启") != std::string::npos);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_gated_proxy_card_hidden, "settings: claude 模块关闭 → 代理卡整卡隐藏", {
    PruneEnv env("gateclaude");
    // claude_proxy 关闭（markdown/todo 开）
    module::settings::SettingsConfig cfg;
    cfg.enabledModules.clear();
    cfg.enabledModules.push_back(module::settings::kModuleIdHome);
    cfg.enabledModules.push_back(module::settings::kModuleIdMarkdown);
    cfg.enabledModules.push_back(module::settings::kModuleIdTodo);
    cfg.enabledModules.push_back(module::settings::kModuleIdSettings);
    DOCTEST_REQUIRE(module::settings::SettingsStore(env.cfgPath).save(cfg).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读临时 config：claude 关
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("gateclaude");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.proxy.bg") == nullptr);  // 整卡隐藏
    DOCTEST_CHECK(ui.find("settings.card.proxy.token.value") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.directory.bg") != nullptr);  // markdown 开
    DOCTEST_CHECK(ui.find("settings.card.log.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.bg") != nullptr);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_gated_directory_card_hidden, "settings: markdown 模块关闭 → 目录卡整卡隐藏", {
    PruneEnv env("gatemd");
    // markdown 关闭（claude 开）
    module::settings::SettingsConfig cfg;
    cfg.enabledModules.clear();
    cfg.enabledModules.push_back(module::settings::kModuleIdHome);
    cfg.enabledModules.push_back(module::settings::kModuleIdTodo);
    cfg.enabledModules.push_back(module::settings::kModuleIdClaudeProxy);
    cfg.enabledModules.push_back(module::settings::kModuleIdSettings);
    DOCTEST_REQUIRE(module::settings::SettingsStore(env.cfgPath).save(cfg).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("gatemd");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.directory.bg") == nullptr);  // 整卡隐藏
    DOCTEST_CHECK(ui.find("settings.card.proxy.bg") != nullptr);  // claude 开
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 效果下拉不超外观卡（终验复审：固定 x=680 在 1200 最小窗超卡 60px） ----

REGISTER_TEST(settings_effect_dropdown_fits_min_window,
              "settings: 效果下拉 1200 最小窗收进外观卡内", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 1200 最小窗 = 页面宽 940（去掉 260 侧边栏）→ 外观卡内宽 940-80=860，
    // 旧布局效果下拉 680+240=920 超出卡右缘 60px
    eui::Screen screen;
    screen.width = 940.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("minwin");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* card = ui.find("settings.card.appearance.bg");
    const auto* themeLight = ui.find("settings.card.appearance.theme.light.bg");
    const auto* font = ui.find("settings.card.appearance.font.ddwrap");
    const auto* effect = ui.find("settings.card.appearance.effect.ddwrap");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(themeLight != nullptr);
    DOCTEST_REQUIRE(font != nullptr);
    DOCTEST_REQUIRE(effect != nullptr);
    // 效果下拉右缘收在卡内（设计右缘留白 28，断言留 ≥20 容差）
    DOCTEST_CHECK(effect->frame.x + effect->frame.width <=
                  card->frame.x + card->frame.width - 20.0f);
    // 字号下拉不与主题按钮重叠、效果下拉不与字号下拉重叠
    DOCTEST_CHECK(font->frame.x >=
                  themeLight->frame.x + themeLight->frame.width);
    DOCTEST_CHECK(effect->frame.x >= font->frame.x + font->frame.width);

    // 宽窗 1600（页面 1340）：保持既有位置（字号 380 / 效果 680 相对卡原点）
    eui::Screen wide;
    wide.width = 1340.0f;
    wide.height = 1000.0f;
    ui.begin("wide");
    mod->compose(ui, wide);
    ui.end();
    ui.layout(wide.width, wide.height);
    const auto* wideFont = ui.find("settings.card.appearance.font.ddwrap");
    const auto* wideEffect = ui.find("settings.card.appearance.effect.ddwrap");
    const auto* wideCard = ui.find("settings.card.appearance.bg");
    DOCTEST_REQUIRE(wideFont != nullptr);
    DOCTEST_REQUIRE(wideEffect != nullptr);
    DOCTEST_REQUIRE(wideCard != nullptr);
    DOCTEST_CHECK_EQ(wideFont->frame.x - wideCard->frame.x, 380.0f);
    DOCTEST_CHECK_EQ(wideEffect->frame.x - wideCard->frame.x, 680.0f);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 桌宠皮肤下拉弹层 z 逃逸：scrollView 内的下拉弹层不得被后续兄弟卡片遮挡 ----
// 依赖 EUI-NEO 子树 zIndex 上界逃逸（core/dsl.h subtreeMaxZIndex）：下拉组件
// 自带 zIndex(20)，需把所在卡片整个抬到后绘兄弟之上。色值/层级无头断言。

namespace {
// 在 root 子树中找同时包含 a、b 两个直接子节点的元素（共同父）
const core::dsl::Element* commonParentOf(const core::dsl::Element* root,
                                         const core::dsl::Element* a,
                                         const core::dsl::Element* b) {
    if (root == nullptr) return nullptr;
    bool hasA = false;
    bool hasB = false;
    for (const auto& child : root->children) {
        if (child.get() == a) hasA = true;
        if (child.get() == b) hasB = true;
    }
    if (hasA && hasB) return root;
    for (const auto& child : root->children) {
        const core::dsl::Element* hit = commonParentOf(child.get(), a, b);
        if (hit != nullptr) return hit;
    }
    return nullptr;
}

int orderedIndexOf(const core::dsl::Element* parent, const core::dsl::Element* child) {
    for (size_t i = 0; i < parent->orderedChildren.size(); ++i) {
        if (parent->orderedChildren[i] == child) return static_cast<int>(i);
    }
    return -1;
}
} // namespace

REGISTER_TEST(settings_pet_dropdown_z_escape, "settings: pet skin dropdown popup escapes following cards", {
    // pet 卡开启（真机默认关），弹层展开时必须盖住下方目录卡
    PruneEnv env("petz");
    {
        std::ofstream out(env.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":["
               "\"home\",\"markdown\",\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("petz");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* dd = ui.find("settings.card.pet.skin.dd");
    const auto* petCard = ui.find("settings.card.pet");
    const auto* dirCard = ui.find("settings.card.directory");
    DOCTEST_REQUIRE(dd != nullptr);
    DOCTEST_REQUIRE(petCard != nullptr);
    DOCTEST_REQUIRE(dirCard != nullptr);
    // ⑥恢复路径：桌宠卡内「鼠标穿透」toggle 必须随卡片渲染
    DOCTEST_CHECK(ui.find("settings.card.pet.through.sw") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.pet.through.label") != nullptr);

    // 下拉自带 zIndex(20) 沿祖先链传播：pet 卡子树上界 > 目录卡（z 0）
    DOCTEST_CHECK(dd->subtreeMaxZIndex >= 20);
    DOCTEST_CHECK(petCard->subtreeMaxZIndex >= 20);
    DOCTEST_CHECK_EQ(dirCard->subtreeMaxZIndex, 0);

    // 两卡是 scrollView 内容栈里的兄弟：pet 卡（含弹层）排序必须在目录卡之后
    const core::dsl::Element* parent = nullptr;
    for (const auto& root : ui.roots()) {
        parent = commonParentOf(root.get(), petCard, dirCard);
        if (parent != nullptr) break;
    }
    DOCTEST_REQUIRE(parent != nullptr);
    DOCTEST_CHECK(orderedIndexOf(parent, petCard) > orderedIndexOf(parent, dirCard));

    mod->onStop();
    ::core::theme::setDark();
    return true;
})
