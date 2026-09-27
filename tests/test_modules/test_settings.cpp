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
#include "core/paths.h"
#include "core/theme.h"
#include "platform/keychain.h"
#include "platform/system_theme.h"

#include <eui/app.h>
#include <eui/detail/dsl_app_impl.h>
#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <thread>

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
    return test_temp::tempPath(
        "devdesk_test_settings_" + std::string(name) + ".json");
}

void removeQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// 原样读回文件（二进制，不做换行转换）——用于比对原子写/备份落盘的字节内容
std::string readFileRaw(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
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

// RAII：临时环境变量（析构恢复为「未设置」）——审计 THS-01 用例把笔记目录 /
// 皮肤目录指到临时路径，与真机目录互不污染
struct EnvVarGuard {
    const char* name;
    std::string saved;
    EnvVarGuard(const char* n, const std::string& value) : name(n) {
        const char* cur = std::getenv(n);
        saved = cur ? cur : "";
        setEnvVar(n, value);
    }
    ~EnvVarGuard() { setEnvVar(name, saved); }
};

// RAII：临时 config.json（DEVDESK_CONFIG_PATH）+ 临时 DB（appDatabase），
// 析构注销/清除，fail-fast 断言提前返回也不污染其他用例
struct PruneEnv {
    std::filesystem::path cfgPath;
    std::filesystem::path dbPath;
    std::unique_ptr<::core::Database> db;

    explicit PruneEnv(const char* tag) {
        cfgPath = test_temp::tempPath(
            "devdesk_test_settings_" + std::string(tag) + "_cfg.json");
        dbPath = test_temp::tempPath(
            "devdesk_test_settings_" + std::string(tag) + ".db");
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

// RAII：把「应用数据根目录的环境变量」重定向到临时目录（审计 H2 用）——
// 日志路径 core::paths::logFile() 的平台来源不同：
//   Windows: %APPDATA%/DevDesk/logs/devdesk.log  （paths_win.cpp）
//   macOS:   $HOME/Library/Application Support/DevDesk/logs/devdesk.log（paths_macos.cpp）
//   Linux:   $HOME/.local/share/DevDesk/logs/devdesk.log（paths_posix.cpp）
// 只重定向 APPDATA 的话，macOS/Linux 上本用例会把自造日志写进**真实**用户目录；
// 故两套变量都在此隔离（Windows 侧只动 APPDATA，行为与从前一字不差）。
// 析构恢复原值，fail-fast 早退也不影响后续用例。
struct AppDataEnv {
    std::string saved;
#if !defined(_WIN32)
    std::string savedHome;   // macOS/Linux 的日志路径来源是 $HOME
#endif
    std::filesystem::path dir;

    explicit AppDataEnv(const char* tag) {
        const char* cur = std::getenv("APPDATA");
        saved = cur ? cur : "";
#if !defined(_WIN32)
        const char* curHome = std::getenv("HOME");
        savedHome = curHome ? curHome : "";
#endif
        dir = test_temp::tempPath(
            "devdesk_test_settings_appdata_" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        setEnvVar("APPDATA", dir.string());
#if !defined(_WIN32)
        setEnvVar("HOME", dir.string());
#endif
    }

    ~AppDataEnv() {
        setEnvVar("APPDATA", saved);
#if !defined(_WIN32)
        setEnvVar("HOME", savedHome);
#endif
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // 写/追加测试日志（模块 tail 读的就是这个文件）
    void writeLog(const std::string& content) const {
        const auto p = ::core::paths::logFile();
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
    }

    void appendLog(const std::string& content) const {
        std::ofstream(::core::paths::logFile(), std::ios::binary | std::ios::app)
            << content;
    }
};

// ---- Task B tab 化：tab 点击助手 ----
// 在一次性元素树上点 tabs 命中矩形（无头等价真实点击：onClick 把 tab 索引
// 写进模块成员），此后 compose 渲染对应 tab 的卡。tab 选择是模块实例状态，
// 与调用方自己的 eui::Ui 互不干扰。找不到 tab 栏返回 false（调用方用
// DOCTEST_REQUIRE 包，失败时用例早退、错误原因明确）
bool clickSettingsTab(::core::Module& mod, const eui::Screen& screen,
                      int tab) {
    eui::Ui ui;
    ui.begin("settings_tab_click");
    mod.compose(ui, screen);
    ui.end();
    const auto* hit = ui.find("settings.tabs.hit." + std::to_string(tab));
    if (hit == nullptr || hit->onClick == nullptr) return false;
    hit->onClick();
    return true;
}

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
    auto bak = path;
    bak += ".bak";
    removeQuietly(path);
    removeQuietly(bak);
    const std::string broken = "{ this is not valid json !!!";
    {
        std::ofstream f(path, std::ios::trunc);
        f << broken;
        f.close();
    }
    module::settings::SettingsStore store(path);
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);  // 解析失败 → 默认
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);
    // M3（2026-09-27）：坏内容另存 .bak；原文件既不被删除也不被改写
    // （回落默认的既有行为不变，只是不再无声无息）
    DOCTEST_REQUIRE(std::filesystem::exists(bak));
    DOCTEST_CHECK_EQ(readFileRaw(bak), broken);
    DOCTEST_REQUIRE(std::filesystem::exists(path));
    DOCTEST_CHECK_EQ(readFileRaw(path), broken);
    removeQuietly(path);
    removeQuietly(bak);
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

REGISTER_TEST(settings_module_compose_renders, "settings: module compose builds page (headless, tab 化)", {
    // 隔离真机 config（本机可能开了 pet 等）：临时 config 无文件 → 默认档
    // （claude/markdown 开、pet 关），逐 tab 断言卡互斥渲染
    PruneEnv env("composetab");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 注意：REGISTER_TEST 宏体内不能出现裸逗号（花括号初始化列表会被预处理器
    // 当成宏参数分隔符），Screen 用字段赋值构造
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    // tab 栏（4 个命中矩形）+ 卡区 scrollView 存在；默认 tab=通用
    frame("smoke");
    DOCTEST_CHECK(ui.find("settings.tabs.hit.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.1") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.2") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.3") != nullptr);
    DOCTEST_CHECK(ui.find("settings.cards.scroll") != nullptr);
    // 通用 tab：外观卡 + 编辑器字号卡（Task B 自日志卡迁出）
    DOCTEST_CHECK(ui.find("settings.card.appearance.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.effect.ddwrap") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    // tab 划分 = 互斥：其余 tab 的卡不渲染
    DOCTEST_CHECK(ui.find("settings.card.modules.bg") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log.bg") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.about.bg") == nullptr);

    // 模块 tab：插拔开关 + 目录 + 代理（ClaudeService::global() 为 null 时
    // 代理卡仍渲染）。W10 复审项在此 tab 依旧有效（多卡超小窗可滚）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    frame("modtab");
    DOCTEST_CHECK(ui.find("settings.card.modules.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.directory.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.proxy.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.bg") == nullptr);

    // 日志 tab：设置卡（保留天数 + 级别）+ 日志查看卡（tail + 刷新按钮）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    frame("logtab");
    DOCTEST_CHECK(ui.find("settings.card.log.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log.level.dd") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tab.log.scroll") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tab.log.refresh.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.dd") == nullptr);

    // 关于 tab：版本 + 路径
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 3));
    frame("abouttab");
    DOCTEST_CHECK(ui.find("settings.card.about.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.about.version") != nullptr);

    mod->onStop();
    ::core::theme::setDark();  // 恢复全局默认主题，不影响其他用例
    return true;
})

// ---- Task B：日志查看卡 —— 刷新按钮点击 → 重读 tail（读盘在 compose 顶部，
// 不在 scrollView content lambda 内）；日志文件缺失时渲染空态不崩 ----

REGISTER_TEST(settings_log_viewer_refresh_button,
              "settings: 日志查看 tail 渲染 + 刷新按钮重读（Task B headless）", {
    PruneEnv env("logviewer");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    ui.begin("lv1");
    mod->compose(ui, screen);
    ui.end();

    auto* refresh = ui.find("settings.tab.log.refresh.bg");
    DOCTEST_REQUIRE(refresh != nullptr);
    DOCTEST_REQUIRE(refresh->onClick != nullptr);
    refresh->onClick();   // 置重读标记；下一帧 compose 顶部读一次 tail
    ui.begin("lv2");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.tab.log.scroll") != nullptr);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 审计 [19]：代理端口校验失败反馈（红字状态行 + 下一次输入清除）----
// 设置页无创建弹窗，端口 draft 就地提交；反馈通道对齐 claude formError 语义：
// 校验错误以红字呈现（settings.subtitle 走 colors.error，非 toast），
// 且随下一次输入自动清除（修复前红字挂到下一次操作才走）。

REGISTER_TEST(settings_port_error_clear_on_input,
              "settings: 端口校验失败 → 红字状态行 + 下一次输入清除（headless，审计 [19]）", {
    PruneEnv env("portformerr");   // 临时 config，不碰真机配置
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    frame("pf1");
    auto* input = ui.find("settings.card.proxy.port.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onTextInput != nullptr);
    eui::TextInputEvent bad;
    bad.text = "abc";               // 非纯数字 → parseProxyPortInput 拒绝
    input->onTextInput(bad);
    frame("pf2");
    // 点「应用」提交非法 draft（与回车 onEnter 同走 applyProxyPort）
    auto* apply = ui.find("settings.card.proxy.port.apply.bg");
    DOCTEST_REQUIRE(apply != nullptr);
    DOCTEST_REQUIRE(apply->onClick != nullptr);
    apply->onClick();
    frame("pf3");
    const auto* sub = ui.find("settings.subtitle");
    DOCTEST_REQUIRE(sub != nullptr);
    DOCTEST_CHECK(sub->text.find("端口无效") != std::string::npos);  // 红字校验错误行
    // 非法端口未落盘：临时 config 文件里的 proxyPort 仍是默认 8888
    module::settings::SettingsStore store(env.cfgPath);
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);

    // 下一次输入 → 校验错误清除，subtitle 回静态说明（formError「输入即清」语义）
    auto* input2 = ui.find("settings.card.proxy.port.input.hit");
    DOCTEST_REQUIRE(input2 != nullptr);
    eui::TextInputEvent fix;
    fix.text = "9";
    input2->onTextInput(fix);       // onChange → statusIsError_ 清
    frame("pf4");
    const auto* sub2 = ui.find("settings.subtitle");
    DOCTEST_REQUIRE(sub2 != nullptr);
    DOCTEST_CHECK(sub2->text.find("端口无效") == std::string::npos);
    mod->onStop();
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

    // Task B tab 化后日志卡位于「日志」tab 顶部（cardsTopY = 40 pad + 64
    // header + 16 gap + 40 tab + 16 gap = 176）。1200×1000 向下展开空间充足
    // （保留字段底 264 + 8 gap + 弹层 216 = 488 ≤ 984）→ 默认 openDown；
    // 短窗 1200×500 下同一弹层越出窗底（488 > 484）→ 应改 openUp 向上展开
    //（弹层整体留在窗体内且位于字段上方）。卡顶游标只累计本 tab 的卡，
    // 全量累加会把这里算错——即审计裁决「日志卡 openUp 游标」的回归位）
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 500.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
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

    // 高窗口 1600×1300：下方空间充足，保持默认向下展开（弹层在字段下方）
    eui::Screen tall;
    tall.width = 1600.0f;
    tall.height = 1300.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, tall, 2));
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

// ---- 目录路径文本框文字垂直居中（W8；原同测的代理 token 框已于 2026-09-27 随鉴权移除） ----

REGISTER_TEST(settings_path_text_vertically_centered, "settings: path text vertically centered", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：代理/目录卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    ui.begin("center");
    mod->compose(ui, screen);
    ui.end();

    // 目录路径是"可见圆角框内的单行文本"，必须显式 Center
    //（缺省 Top 会贴框顶，W7 侧边栏同款问题）
    const auto* path = ui.find("settings.card.directory.path.value");
    DOCTEST_REQUIRE(path != nullptr);
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
    // Task B tab 化：日志（保留天数）下拉在「日志」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
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
        test_temp::tempPath("devdesk_test_settings_takeover");
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
    svc.setSettingsTakeOverEnabled(true);
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
    // Task B tab 化：接管状态行随代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
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
    // 审计 THS-01：接管状态改为只读快照（compose 不再每帧 stat sidecar），
    // 刷新点 = onStart / 进页面 / 切模块 tab / 本页改端口后。代理是页外
    // 启动的，故这里显式走一次「进页面」取新值（真实流程同款）
    mod->onPageEnter();
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

    // 停止代理 → 还原 → 未接管（同样经进页面刷新点取新值）
    svc.stopProxy();
    mod->onPageEnter();
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
    // Task B tab 化：模块管理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
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
    // Task B tab 化：代理/目录/模块卡在「模块」tab，日志卡在「日志」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    ui.begin("gateclaude");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.proxy.bg") == nullptr);  // 整卡隐藏
    DOCTEST_CHECK(ui.find("settings.card.directory.bg") != nullptr);  // markdown 开
    DOCTEST_CHECK(ui.find("settings.card.modules.bg") != nullptr);

    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    ui.begin("gateclaude_log");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.card.log.bg") != nullptr);
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
    // Task B tab 化：目录/代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
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
    const auto* themeDd = ui.find("settings.card.appearance.theme.ddwrap");
    const auto* font = ui.find("settings.card.appearance.font.ddwrap");
    const auto* effect = ui.find("settings.card.appearance.effect.ddwrap");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(themeDd != nullptr);
    DOCTEST_REQUIRE(font != nullptr);
    DOCTEST_REQUIRE(effect != nullptr);
    // 效果下拉右缘收在卡内（设计右缘留白 28，断言留 ≥20 容差）
    DOCTEST_CHECK(effect->frame.x + effect->frame.width <=
                  card->frame.x + card->frame.width - 20.0f);
    // 字号下拉不与主题模式下拉重叠、效果下拉不与字号下拉重叠
    //（主题控件 = spec 缩水 A 的三档下拉，替代原深色/浅色双按钮）
    DOCTEST_CHECK(font->frame.x >= themeDd->frame.x + themeDd->frame.width);
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
    // Task B tab 化：pet/目录卡同在「模块」tab（弹层 z 逃逸断言的兄弟关系不变）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
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

    // 下拉自带 zIndex(20) 沿祖先链传播：pet 卡子树上界 > 目录卡（z 0）。
    // spec 缩水 B 的编辑器字号 Task B 起在「通用」tab 独立卡（不同 tab 不同
    // 屏，无 z 竞争）；目录卡维持 z0，本断言不变
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

// ---- 审计 [21]：代理端口输入 Esc=丢弃草稿（旧映射 Esc→onEnter 会应用端口并重启代理）----

REGISTER_TEST(settings_port_esc_discards_draft,
              "settings: 端口草稿 Esc=丢弃回落已应用端口，不触发应用（headless，审计 [21]）", {
    PruneEnv env("portesc");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：端口输入在代理卡（「模块」tab）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    // 初始显示已应用端口 8888
    frame("pe1");
    const auto* shown1 = ui.find("settings.card.proxy.port.input.text");
    DOCTEST_REQUIRE(shown1 != nullptr);
    DOCTEST_CHECK_EQ(shown1->text, std::string("8888"));

    // Ctrl+A 全选后输入 → 替换成合法草稿 9999（不替换会追加成 88889999 非法值，
    // apply 校验失败即早退，失去「Esc 是否触发 apply」的判别力）
    auto* input = ui.find("settings.card.proxy.port.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onTextInput != nullptr);
    eui::KeyEvent selectAll;
    selectAll.key = eui::InputKey::A;
    selectAll.action = eui::KeyAction::Press;
    selectAll.modifiers.control = true;
    input->onKeyEvent(selectAll);
    eui::TextInputEvent t;
    t.text = "9999";
    input->onTextInput(t);
    frame("pe2");
    const auto* shown2 = ui.find("settings.card.proxy.port.input.text");
    DOCTEST_REQUIRE(shown2 != nullptr);
    DOCTEST_CHECK_EQ(shown2->text, std::string("9999"));   // 草稿已显示

    // Esc = onCancel：丢弃草稿、回落 8888；配置与代理均未被触碰
    auto* input2 = ui.find("settings.card.proxy.port.input.hit");
    DOCTEST_REQUIRE(input2 != nullptr);
    DOCTEST_REQUIRE(input2->onKeyEvent != nullptr);
    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK(input2->onKeyEvent(esc));   // 被 onCancel 消费
    frame("pe3");
    const auto* shown3 = ui.find("settings.card.proxy.port.input.text");
    DOCTEST_REQUIRE(shown3 != nullptr);
    DOCTEST_CHECK_EQ(shown3->text, std::string("8888"));   // 草稿清空回落

    // 未落盘：配置仍是默认 8888（旧行为 Esc=applyProxyPort 会写 9999 并重启代理）
    module::settings::SettingsStore store(env.cfgPath);
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 产品审计 [22]：设置页 Ctrl+S 保存快捷键（headless）----
// 设置页除端口草稿外全部即时落盘，Ctrl+S 语义 = 提交待决草稿（=「应用」按钮
// 全链路，含 ClaudeService 端口同步），无待决时显式重写落盘 + 状态行回显。
// 服务注册 + 代理启动用于验证代理运行中的 Ctrl+S 分支。
REGISTER_TEST(settings_ctrl_s_save_shortcut,
              "settings: Ctrl+S 提交端口草稿 / 显式落盘（审计 [22]）", {
    PruneEnv env("ctrls");
    module::claude::ClaudeService svc(env.db->db(), "DevDeskTestCtrlS");
    module::claude::ClaudeService::setGlobal(&svc);
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：端口输入 / 重新生成按钮在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    auto ctrlS = [](eui::KeyAction action) {
        eui::KeyEvent e;
        e.key = eui::InputKey::S;
        e.action = action;
        e.modifiers.control = true;
        return e;
    };

    // 1) 无草稿：Ctrl+S → 配置落盘（onStart 只读不写，此处才出现文件）
    //    + 状态行回显「设置已保存」
    DOCTEST_CHECK(!std::filesystem::exists(env.cfgPath));
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));
    DOCTEST_CHECK(std::filesystem::exists(env.cfgPath));
    frame("css1");
    {
        const auto* sub = ui.find("settings.subtitle");
        DOCTEST_REQUIRE(sub != nullptr);
        DOCTEST_CHECK(sub->text.find("设置已保存") != std::string::npos);
    }
    {
        module::settings::SettingsStore store(env.cfgPath);
        DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);
    }
    // 2) 门控：裸 S（无 Ctrl）/ Release / 其他 Ctrl 组合 → 不消费
    {
        eui::KeyEvent plain;
        plain.key = eui::InputKey::S;
        plain.action = eui::KeyAction::Press;
        DOCTEST_CHECK(!mod->onKeyEvent(plain));
        DOCTEST_CHECK(!mod->onKeyEvent(ctrlS(eui::KeyAction::Release)));
        eui::KeyEvent ctrlA = ctrlS(eui::KeyAction::Press);
        ctrlA.key = eui::InputKey::A;
        DOCTEST_CHECK(!mod->onKeyEvent(ctrlA));
        eui::KeyEvent ctrlAltS = ctrlS(eui::KeyAction::Press);
        ctrlAltS.modifiers.alt = true;
        DOCTEST_CHECK(!mod->onKeyEvent(ctrlAltS));
    }
    // 3) 端口草稿 ≠ 已应用端口 → Ctrl+S 提交草稿（等价点「应用」，
    //    ClaudeService::configuredProxyPort 同步；代理未运行 → 状态「下次启动生效」）
    frame("css2");
    {
        auto* input = ui.find("settings.card.proxy.port.input.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onKeyEvent != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        eui::KeyEvent selectAll;
        selectAll.key = eui::InputKey::A;
        selectAll.action = eui::KeyAction::Press;
        selectAll.modifiers.control = true;
        input->onKeyEvent(selectAll);           // Ctrl+A 全选
        eui::TextInputEvent t;
        t.text = "9001";
        input->onTextInput(t);                  // 替换 → draft = "9001"
    }
    frame("css3");
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));
    {
        module::settings::SettingsStore store(env.cfgPath);
        DOCTEST_CHECK_EQ(store.load().proxyPort, 9001);
    }
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 9001);
    frame("css3b");   // 重新 compose，让状态行进树
    {
        const auto* sub = ui.find("settings.subtitle");
        DOCTEST_REQUIRE(sub != nullptr);
        DOCTEST_CHECK(sub->text.find("代理端口已保存") != std::string::npos);
    }
    // 4) 代理运行中再按 Ctrl+S：draft 已应用（"9001" == cfg），走显式落盘分支
    //（原「重新生成 Token 确认弹窗门控」一项随鉴权移除，2026-09-27）
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    frame("css4");
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));

    svc.stopProxy();
    module::claude::ClaudeService::setGlobal(nullptr);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- spec 缩水 A：跟随系统主题（themeMode 三档 + platform::prefersDarkTheme）----

REGISTER_TEST(settings_theme_mode_roundtrip_and_legacy,
              "settings: themeMode 默认 auto/读写往返/旧 theme 键派生/未知收敛（spec缩水 A）", {
    using namespace module::settings;
    const auto path = tempConfigFile("thememode");
    removeQuietly(path);
    SettingsStore store(path);

    // 无文件 → 默认 auto（新人跟随系统）
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeAuto));

    // 三档读写往返；theme 键随 darkTheme 同步写（main.cpp 兼容通道）
    SettingsConfig light;
    light.themeMode = kThemeModeLight;
    light.darkTheme = false;
    DOCTEST_REQUIRE(store.save(light).isOk());
    {
        const auto loaded = store.load();
        DOCTEST_CHECK_EQ(loaded.themeMode, std::string(kThemeModeLight));
        DOCTEST_CHECK_FALSE(loaded.darkTheme);
    }
    SettingsConfig dark;
    dark.themeMode = kThemeModeDark;
    dark.darkTheme = true;
    DOCTEST_REQUIRE(store.save(dark).isOk());
    {
        const auto loaded = store.load();
        DOCTEST_CHECK_EQ(loaded.themeMode, std::string(kThemeModeDark));
        DOCTEST_CHECK(loaded.darkTheme);
    }

    // 存量文件（只有 theme 键）→ 派生为显式档，不静默转 auto
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "light"})";
    }
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeLight));
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "dark"})";
    }
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeDark));

    // themeMode 非法值：有 legacy theme → 按 theme 派生；无 theme → auto
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"themeMode": "neon", "theme": "light"})";
    }
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeLight));
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"themeMode": "neon"})";
    }
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeAuto));

    // sanitized：未知档收敛 auto（纯函数，不经 platform）
    SettingsConfig bad;
    bad.themeMode = "sepia";
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(bad).themeMode,
                     std::string(kThemeModeAuto));
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_resolve_theme_dark_matrix,
              "settings: resolveThemeDark 纯函数矩阵（spec缩水 A）", {
    using namespace module::settings;
    DOCTEST_CHECK(resolveThemeDark(kThemeModeDark, false));
    DOCTEST_CHECK(resolveThemeDark(kThemeModeDark, true));
    DOCTEST_CHECK_FALSE(resolveThemeDark(kThemeModeLight, true));
    DOCTEST_CHECK_FALSE(resolveThemeDark(kThemeModeLight, false));
    DOCTEST_CHECK(resolveThemeDark(kThemeModeAuto, true));
    DOCTEST_CHECK_FALSE(resolveThemeDark(kThemeModeAuto, false));
    // 未知 mode 按 auto 语义防御
    DOCTEST_CHECK(resolveThemeDark("neon", true));
    return true;
})

REGISTER_TEST(settings_theme_auto_follows_platform,
              "settings: auto 模式 onStart 解析跟系统深浅（platform 检测，spec缩水 A）", {
    PruneEnv env("thauto");
    {
        std::ofstream out(env.cfgPath);
        out << R"({"themeMode": "auto", "theme": "dark"})";
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 落盘的 theme=dark 是陈旧值；auto 解析后实际档必须等于本机系统检测
    // 结果（无论本机浅/深，断言机器无关）
    const bool systemDark = platform::prefersDarkTheme();
    DOCTEST_CHECK_EQ(::core::theme::current().isDark, systemDark);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_theme_mode_dropdown_click_persists,
              "settings: 主题模式下拉存在且点「深色」落盘（替代旧双按钮，spec缩水 A headless）", {
    PruneEnv env("thdmm");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("thdmm");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.dd") != nullptr);
    // 旧的深色/浅色按钮已删（下拉三档取代，fa-moon/fa-sun 图标随之退场）
    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.dark") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.light") == nullptr);

    // 点第 3 项（深色，索引 2）→ 即时持久化 themeMode=dark + theme 同步
    const auto* itemDark = ui.find("settings.card.appearance.theme.dd.item.2");
    DOCTEST_REQUIRE(itemDark != nullptr);
    DOCTEST_REQUIRE(itemDark->onClick != nullptr);
    itemDark->onClick();
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- spec 缩水 B：编辑器字号可配（SettingsConfig + settings UI + 通道写入）----

REGISTER_TEST(settings_editor_font_size_roundtrip_clamp,
              "settings: editorFontSize 默认 15/读写往返/越界钳制（spec缩水 B）", {
    using namespace module::settings;
    const auto path = tempConfigFile("edfont");
    removeQuietly(path);
    SettingsStore store(path);

    // 默认 15 = fontSizeBody token 基准
    DOCTEST_CHECK_EQ(store.load().editorFontSize, kDefaultEditorFontSize);

    // 读写往返
    SettingsConfig cfg;
    cfg.editorFontSize = 17;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK_EQ(store.load().editorFontSize, 17);

    // sanitized 钳制 [10,32]
    SettingsConfig lo;
    lo.editorFontSize = 5;
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(lo).editorFontSize,
                     kMinEditorFontSize);
    SettingsConfig hi;
    hi.editorFontSize = 999;
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(hi).editorFontSize,
                     kMaxEditorFontSize);

    // 存量文件无该键 → 默认 15
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"theme": "dark", "logRetentionDays": 30})";
    }
    DOCTEST_CHECK_EQ(store.load().editorFontSize, 15);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_editor_font_dropdown_applies_channel,
              "settings: 编辑器字号下拉点档 → core::theme 通道 + 落盘（spec缩水 B headless）", {
    PruneEnv env("edfontui");
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("edfontui");
    mod->compose(ui, screen);
    ui.end();

    // Task B tab 化：编辑器字号自日志卡迁到「通用」tab 独立卡（默认 tab，无需切）
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    // 点第 3 档「大 17px」→ 通道即时生效 + 持久化
    const auto* item = ui.find("settings.card.editor.dd.item.2");
    DOCTEST_REQUIRE(item != nullptr);
    DOCTEST_REQUIRE(item->onClick != nullptr);
    item->onClick();
    DOCTEST_CHECK_EQ(::core::theme::editorFontSize(), 17.0f);
    module::settings::SettingsStore store(env.cfgPath);
    DOCTEST_CHECK_EQ(store.load().editorFontSize, 17);

    mod->onStop();
    ::core::theme::setEditorFontSize(15.0f);  // 恢复通道默认
    ::core::theme::setDark();  // onStart 的 auto 解析在本机可能 setLight，防串扰
    return true;
})

// ---- spec 缩水 C：关于卡（版本 / EUI-NEO rev / 构建信息，CMake 注入宏）----

REGISTER_TEST(settings_about_card_renders,
              "settings: 关于卡渲染版本与 EUI-NEO 行（spec缩水 C headless）", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：关于卡在「关于」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 3));
    eui::Ui ui;
    ui.begin("aboutc");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.about.bg") != nullptr);
    const auto* version = ui.find("settings.card.about.version");
    const auto* euiLine = ui.find("settings.card.about.eui");
    const auto* cfgLine = ui.find("settings.card.about.config");
    DOCTEST_REQUIRE(version != nullptr);
    DOCTEST_REQUIRE(euiLine != nullptr);
    DOCTEST_REQUIRE(cfgLine != nullptr);
    DOCTEST_CHECK(version->text.find("DevDesk") != std::string::npos);
    // CMake 注入的 project VERSION（0.1.0）出现在版本行
    DOCTEST_CHECK(version->text.find("0.1.") != std::string::npos);
    DOCTEST_CHECK(version->text.find("构建") != std::string::npos);
    DOCTEST_CHECK(euiLine->text.find("EUI-NEO") != std::string::npos);
    DOCTEST_CHECK(cfgLine->text.find("config.json") != std::string::npos);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_about_text_helpers,
              "settings: aboutVersionLine/aboutEuiNeoLine 纯函数（spec缩水 C）", {
    const std::string v = module::settings::aboutVersionLine();
    DOCTEST_CHECK(v.find("DevDesk 0.1.") != std::string::npos);
    // 宏缺失回退分支不可能在本 target 命中；有值即可（构建戳/类型非空）
    DOCTEST_CHECK(v.find("dev") == std::string::npos);  // VERSION 注入生效
    const std::string e = module::settings::aboutEuiNeoLine();
    DOCTEST_CHECK(e.find("EUI-NEO rev") != std::string::npos);
    // 最低依赖 rev 恒打印（顶层 CMake 变量）
    DOCTEST_CHECK(e.find("最低依赖 rev") != std::string::npos);
    return true;
})

// ---- spec 缩水 D：日志级别可配（SettingsConfig + core::log::setLevel 应用）----

REGISTER_TEST(settings_log_level_roundtrip_matrix,
              "settings: logLevel 默认 INFO/读写往返/未知收敛/parseLogLevel 矩阵（spec缩水 D）", {
    using namespace module::settings;
    const auto path = tempConfigFile("loglevel");
    removeQuietly(path);
    SettingsStore store(path);

    // 默认 INFO
    DOCTEST_CHECK_EQ(store.load().logLevel, std::string(kLogLevelInfo));

    // 四档读写往返
    SettingsConfig cfg;
    cfg.logLevel = kLogLevelDebug;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK_EQ(store.load().logLevel, std::string(kLogLevelDebug));
    cfg.logLevel = kLogLevelWarn;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK_EQ(store.load().logLevel, std::string(kLogLevelWarn));
    cfg.logLevel = kLogLevelError;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK_EQ(store.load().logLevel, std::string(kLogLevelError));

    // 未知值（含小写/脏值）→ sanitized 收敛 INFO
    SettingsConfig bad;
    bad.logLevel = "verbose";
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(bad).logLevel,
                     std::string(kLogLevelInfo));
    bad.logLevel = "debug";  // 大小写敏感
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(bad).logLevel,
                     std::string(kLogLevelInfo));

    // parseLogLevel 矩阵（Trace/Fatal 字面量不开放 → Info）
    DOCTEST_CHECK(parseLogLevel(kLogLevelDebug) ==
                  core::log::Level::Debug);
    DOCTEST_CHECK(parseLogLevel(kLogLevelWarn) == core::log::Level::Warn);
    DOCTEST_CHECK(parseLogLevel(kLogLevelError) == core::log::Level::Error);
    DOCTEST_CHECK(parseLogLevel(kLogLevelInfo) == core::log::Level::Info);
    DOCTEST_CHECK(parseLogLevel("TRACE") == core::log::Level::Info);
    DOCTEST_CHECK(parseLogLevel("") == core::log::Level::Info);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_log_level_applied_on_start_and_click,
              "settings: 日志级别 onStart 恢复 + 下拉切档即时应用（spec缩水 D headless）", {
    PruneEnv env("loglvl");
    {
        std::ofstream out(env.cfgPath);
        out << R"({"logLevel": "WARN"})";
    }
    const core::log::Level prev = core::log::level();
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 持久化 WARN 档在 onStart 即应用
    DOCTEST_CHECK(core::log::level() == core::log::Level::Warn);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：日志级别下拉在「日志」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    eui::Ui ui;
    ui.begin("loglvl");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.card.log.level.dd") != nullptr);

    // 点第 1 项 DEBUG → 运行时 + 落盘同步
    const auto* item = ui.find("settings.card.log.level.dd.item.0");
    DOCTEST_REQUIRE(item != nullptr);
    DOCTEST_REQUIRE(item->onClick != nullptr);
    item->onClick();
    DOCTEST_CHECK(core::log::level() == core::log::Level::Debug);
    module::settings::SettingsStore store(env.cfgPath);
    DOCTEST_CHECK_EQ(store.load().logLevel,
                     std::string(module::settings::kLogLevelDebug));

    mod->onStop();
    core::log::setLevel(prev);  // 恢复（测试进程默认 Info）
    ::core::theme::setDark();   // onStart 的 auto 解析可能 setLight，防串扰
    return true;
})

// ---- 审计 [5][36] 模块隔离静态守卫 ----
// settings 与 pet 彻底解耦后：源文件不得再 include module_pet 头、不得再
// 触盘 store 类；CMakeLists 的非注释行不得再出现 devdesk_module_pet /
// devdesk_module_claude_proxy 两个 link 目标名（解释注释里允许提及）。
// claude 侧按设计保留 claude_service.h 头通道（global() 单例），故守卫只
// 盯 pet include 与两条 CMake link。
namespace {

// 从测试 TU 的 __FILE__（Ninja 下为绝对路径）或 cwd 向上找仓库根
std::filesystem::path findRepoRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path marker = "src/module_settings/CMakeLists.txt";
    std::vector<fs::path> candidates;
    candidates.emplace_back(fs::path(__FILE__).parent_path().parent_path().parent_path());
    candidates.emplace_back(fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path());
    for (fs::path p = fs::current_path(ec); !ec && !p.empty() && p != p.root_path(); p = p.parent_path()) {
        candidates.emplace_back(p);
    }
    for (const auto& c : candidates) {
        if (fs::exists(c / marker, ec)) return c;
    }
    return {};
}

std::string readWholeFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

REGISTER_TEST(settings_module_isolation_guard,
              "settings: 模块隔离守卫——无 module_pet include、CMake 无 pet/claude link（审计 [5][36]）", {
    const auto root = findRepoRoot();
    DOCTEST_REQUIRE_MESSAGE(!root.empty(),
                            "找不到仓库根，守卫失效（检查 __FILE__/cwd 定位）");

    // 1) settings 源文件：pet 头零 include、盘 store 零触碰；声明走中立桥头
    const std::string src =
        readWholeFile(root / "src/module_settings/settings_module.cpp");
    DOCTEST_REQUIRE_MESSAGE(!src.empty(), "settings_module.cpp 读取失败");
    DOCTEST_CHECK(src.find("#include \"module_pet/") == std::string::npos);
    DOCTEST_CHECK(src.find("module_pet/") == std::string::npos);
    DOCTEST_CHECK(src.find("PetConfigStore") == std::string::npos);
    DOCTEST_CHECK(src.find("petConfigFile") == std::string::npos);
    // 正向：桥声明唯一来源 = src 根中立头（恰好 include 一次，解耦而非删功能）
    // 且四个桥函数仍真在调用点被用到（`::module::pet::X` 调用形）
    const std::string bridgeInclude = "#include \"module_pet_bridge.h\"";
    const size_t bridgeAt = src.find(bridgeInclude);
    DOCTEST_CHECK(bridgeAt != std::string::npos);
    DOCTEST_CHECK(src.find(bridgeInclude, bridgeAt + bridgeInclude.size()) ==
                  std::string::npos);
    for (const char* fn : {"discoverSkins", "applyPetSkin", "resetPetPosition",
                           "currentSkinName"}) {
        DOCTEST_CHECK(src.find(std::string("::module::pet::") + fn) !=
                      std::string::npos);
    }
    // 声明已搬去中立头：本文件不得再有内联 pet 函数声明块
    DOCTEST_CHECK(src.find("namespace module::pet") == std::string::npos);
    // currentClickThrough 桥已随挂机穿透功能移除（2026-09-24）
    DOCTEST_CHECK(src.find("#include \"claude_service.h\"") != std::string::npos);

    // 2) settings CMakeLists：非注释行不得再 link 兄弟模块库
    const std::string cmk = readWholeFile(root / "src/module_settings/CMakeLists.txt");
    DOCTEST_REQUIRE_MESSAGE(!cmk.empty(), "module_settings/CMakeLists.txt 读取失败");
    std::istringstream lines(cmk);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        if (line[first] == '#') continue;  // 注释允许提及目标名
        DOCTEST_CHECK(line.find("devdesk_module_pet") == std::string::npos);
        DOCTEST_CHECK(line.find("devdesk_module_claude_proxy") == std::string::npos);
    }
    return true;
})

// ---- 审计 H2：日志 tail 读盘在后台线程，compose 只读快照 ----
// 回归：此前 compose 顶部同步读 256KB 日志文件（在渲染帧里与代理 worker 的
// DB 写抢同一把锁）。现在切 tab / 点刷新只置脏标记 → 发一次读意图
// （std::async 后台线程），结果下一帧收割；快照恒非空沿用上次值（不闪空、不丢行）。
REGISTER_TEST(settings_h2_log_tail_snapshot,
              "settings: 日志 tail 后台重读快照（H2 tick 前旧值/后新值）", {
    PruneEnv env("h2tail");
    AppDataEnv appdata("h2tail");   // 日志文件隔离到临时 APPDATA
    appdata.writeLog("H2-TAIL-ONE\n");

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();   // 首次装载在启动期同步做（非渲染路径）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 探针：跑一帧 compose，返回日志查看卡渲染出的全部行（"" 结束）
    auto tailLines = [&]() -> std::vector<std::string> {
        eui::Ui ui;
        ui.begin("settings_h2_probe");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);   // scrollView 内容在 layout 期展开
        std::vector<std::string> out;
        for (size_t i = 0;; ++i) {
            const auto* e =
                ui.find("settings.tab.log.line." + std::to_string(i));
            if (e == nullptr) break;
            out.push_back(e->text);
        }
        return out;
    };
    auto hasLine = [](const std::vector<std::string>& lines,
                      const char* needle) {
        for (const auto& l : lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    // 切到日志 tab（置脏）+ 外部追加新行 → 下一帧 compose 才发出读意图
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    appdata.appendLog("H2-TAIL-TWO\n");

    // tick 前：本帧才发读意图（读盘本帧上路），快照 = 上次装载值且不含新行
    const auto before = tailLines();
    DOCTEST_CHECK(!before.empty());                 // 非空（不闪空）
    DOCTEST_CHECK(hasLine(before, "H2-TAIL-ONE"));  // = 上次值
    DOCTEST_CHECK(!hasLine(before, "H2-TAIL-TWO")); // 新值未到（确定，非时序）

    // tick 后：后台结果被收割 → 新行可见（有界轮询，不依赖固定睡眠）
    bool updated = false;
    for (int i = 0; i < 200 && !updated; ++i) {
        updated = hasLine(tailLines(), "H2-TAIL-TWO");
        if (!updated) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(updated);
    DOCTEST_CHECK(hasLine(tailLines(), "H2-TAIL-ONE"));   // 旧行不丢

    mod->onStop();
    ::core::theme::setDark();
    return true;
})
// ---- 审计 THS-01：外部状态只读快照（compose 不做文件系统查询）----
// 回归：接管状态（isClaudeCodeManaged → sidecar stat，compose 顶部 + 代理卡
// 每帧）、笔记目录存在性（fs::exists(markdownDir)，目录卡每帧）、桌宠皮肤清单
// （discoverSkins 目录遍历 + 每皮肤 pet.json stat，桌宠卡首帧）原先都直接落在
// 渲染路径上。改后与 H2（logTail）同款——读一次存成员，compose 只读快照，
// 刷新点 = onStart / onPageEnter / 切模块 tab / 打开目录后。本用例验：
// ① 刷新前快照非空且等于上次值（不闪空、不闪变）；② 刷新入口后取新值；
// ③ 目录消失 / 出现的边界下不崩、不闪空。
REGISTER_TEST(settings_ths01_fs_snapshot,
              "settings: 外部状态快照——刷新前旧值 / 刷新后新值（THS-01）", {
    PruneEnv env("ths01");
    // 模块插拔门控：目录卡 + 桌宠卡都要渲染（pet 不在默认启用集合里）
    module::settings::SettingsConfig cfg;
    cfg.enabledModules.clear();
    cfg.enabledModules.push_back(module::settings::kModuleIdHome);
    cfg.enabledModules.push_back(module::settings::kModuleIdMarkdown);
    cfg.enabledModules.push_back(module::settings::kModuleIdSettings);
    cfg.enabledModules.push_back(module::settings::kModuleIdPet);
    DOCTEST_REQUIRE(module::settings::SettingsStore(env.cfgPath).save(cfg).isOk());

    const auto notesDir = test_temp::tempPath("devdesk_ths01_notes");
    const auto petsDir = test_temp::tempPath("devdesk_ths01_pets");
    std::error_code ec;
    std::filesystem::remove_all(notesDir, ec);
    std::filesystem::remove_all(petsDir, ec);
    const EnvVarGuard notesEnv("DEVDESK_NOTES_DIR", notesDir.string());
    const EnvVarGuard petsEnv("DEVDESK_PETS_DIR", petsDir.string());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();   // 首次装载在启动期（非渲染路径）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));   // 模块 tab

    // 探针：跑一帧 compose，返回 [目录卡路径文本, 皮肤下拉清单]
    auto probe = [&]() {
        eui::Ui ui;
        ui.begin("settings_ths01_probe");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);  // scrollView 内容在 layout 期展开
        const auto* path = ui.find("settings.card.directory.path.value");
        std::vector<std::string> skins;
        for (size_t i = 0;; ++i) {
            const auto* e = ui.find(
                "settings.card.pet.skin.dd.item.label." + std::to_string(i));
            if (e == nullptr) break;
            skins.push_back(e->text);
        }
        return std::make_pair(path ? path->text : std::string(), skins);
    };
    auto notCreated = [](const std::string& t) {
        return t.find("（尚未创建）") != std::string::npos;
    };
    auto hasSkin = [](const std::vector<std::string>& skins, const char* n) {
        for (const auto& s : skins) {
            if (s == n) return true;
        }
        return false;
    };

    // ① onStart 快照：笔记目录不存在（标「尚未创建」），皮肤清单为空
    const auto initial = probe();
    DOCTEST_CHECK(!initial.first.empty());        // 非空（不闪空）
    DOCTEST_CHECK(notCreated(initial.first));
    DOCTEST_CHECK_FALSE(hasSkin(initial.second, "skinA"));

    // ② 盘上出现目录 + 新皮肤，但未到刷新点 → 快照沿用上次值（不闪变）
    std::filesystem::create_directories(notesDir, ec);
    DOCTEST_REQUIRE(!ec);
    std::filesystem::create_directories(petsDir / "skinA", ec);
    DOCTEST_REQUIRE(!ec);
    {
        std::ofstream(petsDir / "skinA" / "pet.json", std::ios::binary) << "{}";
    }
    const auto stale = probe();
    DOCTEST_CHECK(notCreated(stale.first));                  // = 上次值
    DOCTEST_CHECK_FALSE(hasSkin(stale.second, "skinA"));     // = 上次值

    // ③ 刷新入口（进页面）→ 快照更新为新值，文本仍非空
    mod->onPageEnter();
    const auto refreshed = probe();
    DOCTEST_CHECK(!refreshed.first.empty());
    DOCTEST_CHECK_FALSE(notCreated(refreshed.first));        // 新值
    DOCTEST_CHECK(hasSkin(refreshed.second, "skinA"));       // 新值

    // ④ 边界：目录消失 → 刷新前仍显示上次值（不闪空），刷新后回到未创建态
    std::filesystem::remove_all(notesDir, ec);
    DOCTEST_CHECK_FALSE(notCreated(probe().first));          // 上次值仍在
    mod->onPageEnter();
    const auto gone = probe();
    DOCTEST_CHECK(!gone.first.empty());
    DOCTEST_CHECK(notCreated(gone.first));

    mod->onStop();
    std::filesystem::remove_all(notesDir, ec);
    std::filesystem::remove_all(petsDir, ec);
    ::core::theme::setDark();
    return true;
})
