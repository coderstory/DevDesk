// tests/test_modules/test_settings.cpp
// 设置模块单测：SettingsStore / app_config 表读写纯逻辑 + 模块 compose 无头冒烟。
// 配置落点是 app_config 表（不是文件）：顶层键 = 行键，pet 配置整份存一行。
// 无头 compose 用 dsl_app_impl.h 注入 app 运行时 API；app::compose 用 src/app_sidebar.cpp 真实现。
#include "doctest.h"
#include "test_support.h"
#include "app_tools.h"          // app.read_settings 工厂
#include "settings_config.h"
#include "settings_ui_logic.h"

#include "claude_service.h"
#include "core/claude_proxy_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/tool.h"          // ITool / ToolArgs
#include "platform/json.h"      // 读 app.read_settings 的 JSON 出参
#include "platform/keychain.h"
#include "platform/system_theme.h"

#include <eui/app.h>
#include <eui/detail/dsl_app_impl.h>
#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <memory>
#include <string>
#include <thread>

extern "C" ::core::Module* create_settings_module();

// app API 的实现 TU（glfw_app_main）只编进 DevDesk 主程序，测试侧靠包含
// dsl_app_impl.h 自带实现（与 EUI-NEO 自身单测相同的注入方式）。app::compose 用
// src/app_sidebar.cpp 的真实现，该 TU 已直接编进测试二进制。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}.title("devdesk_test_settings");
    return config;
}
} // namespace app

namespace {

// 每个用例独立的临时配置（避免用例间串扰）；调用点统一 test_support::tempConfigFile()
constexpr const char* kCfgPrefix = "devdesk_test_settings_";

void removeQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

    // 读配置字节走 test_support::readRawConfigRow；本 helper 供宠物皮肤 pet.json 夹具、
    // settings.json 接管用例按字节读文件用。

    // 数据根（APPDATA / HOME）隔离已收进共享头 test_support::AppDataEnv；
    // 以下两个是「模块 tail 读的就是这个文件」的写/追加助手，只读 ::core::paths::logFile()
void writeLogFile(const std::string& content) {
    const auto p = ::core::paths::logFile();
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
}

void appendLogFile(const std::string& content) {
    std::ofstream(::core::paths::logFile(), std::ios::binary | std::ios::app)
        << content;
}

// tab 点击助手：在一次性元素树上点 tabs 命中矩形（无头等价真实点击——onClick 把 tab
// 索引写进模块成员），此后 compose 渲染对应 tab 的卡。找不到 tab 栏返回 false，调用方用
// DOCTEST_REQUIRE 包。tab 选择是模块实例状态，与调用方自己的 eui::Ui 互不干扰。
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

// 设置页的代理运行面经 core 只读服务定位器取。用例把自己 setGlobal 的 service 用
// **生产同一个适配器**（core::GlobalClaudeProxyService，转发到 global()）暴露给定位器，
// 生产 / 测试走同一条取用路径。RAII：作用域结束自动注销，fail-fast 提前 return 也不留
// 悬垂注册。
struct ScopedClaudeProxy {
    ::core::GlobalClaudeProxyService<module::claude::ClaudeService> adapter;
    ::core::ScopedService<::core::IClaudeProxyService> registration{&adapter};
};

// 源码级护栏两条：
//   - settings_module.cpp 不得直连 ClaudeService 单例：零 claude_service.h、零
//     module::claude、**代码里**零 ClaudeService（剥掉 `//` 之后再查）；同时必须真的
//     经 core 只读服务定位器取——防「解耦 = 把功能删了」。
//   - 代理端口常量单一来源 = core/proxy_port.h：字面量只在该头定义，两个消费方
//     （settings_config.h / claude_service.h）只从 core 取。
// 仓库根靠 cwd 上溯，**不用** __FILE__（ccache 会把绝对路径相对化）。定位失败返回
// 明确报告，不静默绿。

std::filesystem::path findRepoRoot() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec);
    if (ec) return {};
    for (int up =        if (std::filesystem::exists(dir / "src" / "core" / "proxy_port.h", ec) &&
            std::filesystem::exists(dir / "tests" / "CMakeLists.txt", ec)) {
            return dir;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;  // 已到根
        dir = parent;
    }
    return {};
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// 逐行扫针，命中拼成「相对路径:行号: 行内容」；stripLineComment=true 时先剥掉行内
// `//` 之后的部分
std::vector<std::string> scanFileForTokens(
    const std::filesystem::path& root, const std::string& relPath,
    const std::vector<std::string>& needles, bool stripLineComment) {
    std::vector<std::string> hits;
    std::ifstream in(root / relPath);
    if (!in) {
        hits.push_back(relPath + ":        return hits;
    }
    std::string line;
    int lineNo =    while (std::getline(in, line)) {
        ++lineNo;
        std::string code = line;
        if (stripLineComment) {
            const auto pos = code.find("//");
            if (pos != std::string::npos) code.erase(pos);
        }
        for (const std::string& needle : needles) {
            if (code.find(needle) != std::string::npos) {
                hits.push_back(relPath + ":" + std::to_string(lineNo) + ": " +
                               line);
                break;
            }
        }
    }
    return hits;
}

// 空串 = 通过；非空 = 失败报告（含逐条命中）
std::string r    const std::filesystem::path root = findRepoRoot();
    if (root.empty()) {
        std::error_code ec;
        return "护栏无法定位仓库根（cwd=" +
               std::filesystem::current_path(ec).string() +
               "）：请从仓库根或 build/ 下运行 devdesk_tests —— 护栏靠 cwd 上溯"
               "定位（不能用 __FILE__：ccache 会把绝对路径相对化）";
    }
    const std::string rel = "src/module_settings/settings_module.cpp";

    std::string report;
    //    for (const std::string& hit : scanFileForTokens(
             root, rel, {"claude_service.h", "module::claude"}, false)) {
        report += "设置页又直接引用了兄弟模块 claude：" + hit + "\n";
    }
    //    for (const std::string& hit :
         scanFileForTokens(root, rel, {"ClaudeService"}, true)) {
        report += "设置页代码里又直呼了 ClaudeService：" + hit + "\n";
    }
    //    const std::string src = readWholeFile(root / rel);
    if (src.find("core/service_locator.h") == std::string::npos) {
        report += "设置页没有 include core/service_locator.h —— 代理运行面应由"
                  " core 只读服务定位器取（R    }
    if (src.find("::core::IClaudeProxyService") == std::string::npos) {
        report += "设置页没有用 core::IClaudeProxyService —— 定位器通道被摘掉了？"
                  "（解耦不是删功能）\n";
    }
    //    //    各自注册、覆盖不到这条生产接线）
    const std::string mainSrc = readWholeFile(root / "src/main.cpp");
    if (mainSrc.find("ServiceLocator::set<::core::IClaudeProxyService>") ==
        std::string::npos) {
        report += "src/main.cpp 没有把代理运行面注册进 core 服务定位器"
                  "（ServiceLocator::set<::core::IClaudeProxyService> 缺失）——"
                  "生产装配段漏注册，设置页会静默降级\n";
    }
    return report;
}

std::string proxyPortSingleSourceReport() {
    const std::filesystem::path root = findRepoRoot();
    if (root.empty()) {
        std::error_code ec;
        return "护栏无法定位仓库根（cwd=" +
               std::filesystem::current_path(ec).string() + "）";
    }
    const std::string coreHeader = "src/core/proxy_port.h";
    const std::vector<std::string> consumers = {
        "src/module_settings/settings_config.h",
        "src/module_claude_proxy/claude_service.h",
    };

    std::string report;
    // 唯一来源头：三个字面量必须在这里定义
    const std::string coreSrc = readWholeFile(root / coreHeader);
    if (coreSrc.empty()) {
        return "读不到 " + coreHeader + "（端口常量唯一来源头缺失）";
    }
    for (const char* def : {"kDefaultProxyPort =                            "kMaxProxyPort =        if (coreSrc.find(def) == std::string::npos) {
            report += coreHeader + " 缺少定义 '" + def + "'\n";
        }
    }
    // 消费方：不得自带字面量，必须从 core 取
    for (const std::string& rel : consumers) {
        for (const std::string& hit :
             scanFileForTokens(root, rel,
                               {"kDefaultProxyPort =                                "kMaxProxyPort =                               false)) {
            report += rel + " 又自带端口字面量定义（应取 core/proxy_port.h）：" +
                      hit + "\n";
        }
        const std::string src = readWholeFile(root / rel);
        for (const char* ref : {"::core::kDefaultProxyPort",
                                "::core::kMinProxyPort",
                                "::core::kMaxProxyPort"}) {
            if (src.find(ref) == std::string::npos) {
                report += rel + " 没有引用 " + ref +
                          "（端口常量未从唯一来源取）\n";
            }
        }
    }
    return report;
}

} // namespace

REGISTER_TEST(settings_defaults_on_missing_file, "settings: missing file -> defaults", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "missing");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);                        // 默认深色（方案 B Dark）
    DOCTEST_CHECK_EQ(cfg.fontScalePercent,    DOCTEST_CHECK_EQ(cfg.logRetentionDays,    return true;
})

REGISTER_TEST(settings_proxy_autostart_default_off, "settings: claudeProxyAutoStart 默认 false + 读写往返", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    // 默认不启动代理
    const auto path = test_support::tempConfigFile(kCfgPrefix, "autostart");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    DOCTEST_CHECK_FALSE(store.load().claudeProxyAutoStart);

    // 打开后持久化 + 读回 true
    module::settings::SettingsConfig cfg;
    cfg.claudeProxyAutoStart = true;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK(store.load().claudeProxyAutoStart);

    // 存量配置无该键 → 保持默认 false
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "light", "logRetentionDays":    }
    DOCTEST_CHECK_FALSE(store.load().claudeProxyAutoStart);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_proxy_port_roundtrip_clamp, "settings: proxyPort 默认/读写往返/非法回落/输入解析", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    using namespace module::settings;
    const auto path = test_support::tempConfigFile(kCfgPrefix, "proxyport");
    removeQuietly(path);
    SettingsStore store(::core::appDatabase());

    auto withPort = [](int port) {
        SettingsConfig c;
        c.proxyPort = port;
        return c;
    };

    // 默认    DOCTEST_CHECK_EQ(store.load().proxyPort,
    // 读写往返
    DOCTEST_REQUIRE(store.save(withPort(    DOCTEST_CHECK_EQ(store.load().proxyPort,
    // 非法值回落默认（    DOCTEST_REQUIRE(store.save(withPort(    DOCTEST_CHECK_EQ(store.load().proxyPort,    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(-    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(    DOCTEST_CHECK_EQ(SettingsStore::sanitized(withPort(
    // 存量配置无 claudeProxyPort 键 → 默认    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "dark", "claudeProxyAutoStart": true})"));
    }
    DOCTEST_CHECK_EQ(store.load().proxyPort,
    // 输入框解析（纯数字 + 范围）
    int out =    DOCTEST_CHECK(parseProxyPortInput("    DOCTEST_CHECK(parseProxyPortInput("    DOCTEST_CHECK(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("", out));
    DOCTEST_CHECK_FALSE(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("    DOCTEST_CHECK_FALSE(parseProxyPortInput("-    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_save_load_roundtrip, "settings: save/load roundtrip", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    module::settings::SettingsConfig cfg;
    cfg.darkTheme = false;               // 浅色
    cfg.fontScalePercent =    cfg.logRetentionDays =    auto saved = store.save(cfg);
    DOCTEST_REQUIRE(saved.isOk());

    const auto loaded = store.load();
    DOCTEST_CHECK_FALSE(loaded.darkTheme);
    DOCTEST_CHECK_EQ(loaded.fontScalePercent,    DOCTEST_CHECK_EQ(loaded.logRetentionDays,    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_zero_days_permanent, "settings:    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "zero");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    module::settings::SettingsConfig cfg;
    cfg.logRetentionDays =    auto saved = store.save(cfg);
    DOCTEST_REQUIRE(saved.isOk());

    const auto loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.logRetentionDays,    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_corrupted_json_fallback, "settings: corrupted json -> defaults", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "corrupt");
    auto bak = path;
    bak += ".bak";
    removeQuietly(path);
    removeQuietly(bak);
    const std::string broken = "{ this is not valid json !!!";
    // 坏内容塞进一个**已知的顶层键**——只有这样 load 才会真的去解析它
    DOCTEST_REQUIRE(test_support::writeRawConfigRow(
        ::core::appDatabase(), "config.json:theme", broken));
    module::settings::SettingsStore store(::core::appDatabase());
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);
    DOCTEST_CHECK_EQ(cfg.fontScalePercent,    DOCTEST_CHECK_EQ(cfg.logRetentionDays,        // 脏内容没有任何**文件**载体。「原始内容可查」由两件事兑现：① 坏行原样留在
        // app_config 里；② loadChecked 对「读到的值不是合法 JSON」如实报错。
    DOCTEST_CHECK_EQ(
        test_support::readRawConfigRow(::core::appDatabase(), "config.json:theme"),
        broken);
    auto checked = store.loadChecked();
    DOCTEST_REQUIRE(checked.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(checked.errorCode()),
                     static_cast<int>(core::ErrorCode::JsonParseError));
    removeQuietly(path);
    removeQuietly(bak);
    return true;
})

REGISTER_TEST(settings_partial_file_uses_defaults, "settings: partial file -> per-field defaults", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "partial");
    removeQuietly(path);
    {
        // 只有 theme 字段；fontScale / logRetention 缺失 → 各自默认值
        // 按顶层键写进 app_config 表各行
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\": \"light\"}"));
    }
    module::settings::SettingsStore store(::core::appDatabase());
    const auto cfg = store.load();
    DOCTEST_CHECK_FALSE(cfg.darkTheme);            // theme=light 生效
    DOCTEST_CHECK_EQ(cfg.fontScalePercent,    DOCTEST_CHECK_EQ(cfg.logRetentionDays,    removeQuietly(path);
    return true;
})

    // fontScalePercent / fontScalePercentV    // 默认REGISTER_TEST(settings_legacy_fontscale_migration, "settings: legacy font scale keys reset to    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "legacy");
    removeQuietly(path);
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\": \"dark\", \"fontScalePercent\":    }
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().fontScalePercent,
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"fontScalePercentV    }
    DOCTEST_CHECK_EQ(store.load().fontScalePercent,
    {
        // V        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"fontScalePercent\":    }
    DOCTEST_CHECK_EQ(store.load().fontScalePercent,    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_theme_value_mapping, "settings: theme string mapping", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "theme");
    removeQuietly(path);
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\": \"dark\"}"));
    }
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK(store.load().darkTheme);

    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\": \"light\"}"));
    }
    DOCTEST_CHECK_FALSE(store.load().darkTheme);

    {
        // 未知字符串 → 安全回退深色（默认值）
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\": \"whatever\"}"));
    }
    DOCTEST_CHECK(store.load().darkTheme);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_clamp_out_of_range, "settings: out-of-range values clamped", {
    // sanitized 直接钳制（load/save 内部都会调用）
    module::settings::SettingsConfig lo;
    lo.fontScalePercent =    lo.logRetentionDays = -    const auto loS = module::settings::SettingsStore::sanitized(lo);
    DOCTEST_CHECK_EQ(loS.fontScalePercent, module::settings::kMinFontScalePercent);
    DOCTEST_CHECK_EQ(loS.logRetentionDays,
    module::settings::SettingsConfig hi;
    hi.fontScalePercent =    hi.logRetentionDays =    const auto hiS = module::settings::SettingsStore::sanitized(hi);
    DOCTEST_CHECK_EQ(hiS.fontScalePercent, module::settings::kMaxFontScalePercent);
    DOCTEST_CHECK_EQ(hiS.logRetentionDays, module::settings::kMaxLogRetentionDays);
    return true;
})

REGISTER_TEST(settings_save_clamps_before_write, "settings: save clamps, load returns clamped", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "clampwrite");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    module::settings::SettingsConfig cfg;
    cfg.fontScalePercent =    auto saved = store.save(cfg);
    DOCTEST_REQUIRE(saved.isOk());

    const auto loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.fontScalePercent, module::settings::kMaxFontScalePercent);
    removeQuietly(path);
    return true;
})

// 模块 compose 无头冒烟（DSL 元素树构建，无需窗口 / GL）

REGISTER_TEST(settings_module_compose_renders, "settings: module compose builds page (headless, tab 化)", {
    // 隔离真机 config（本机可能开了 pet 等）：临时 config 无文件 → 默认档
    // （claude/markdown 开、pet 关），逐 tab 断言卡互斥渲染
    test_support::TestEnv env(kCfgPrefix, "composetab", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "composetab_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen{    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    frame("smoke");
    DOCTEST_CHECK(ui.find("settings.tabs.hit.    DOCTEST_CHECK(ui.find("settings.tabs.hit.    DOCTEST_CHECK(ui.find("settings.tabs.hit.    DOCTEST_CHECK(ui.find("settings.cards.scroll") != nullptr);
    // 通用 tab：外观卡 + 编辑器字号卡
    DOCTEST_CHECK(ui.find("settings.card.appearance") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.effect.ddwrap") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.about") == nullptr);

    // 模块 tab：插拔开关 + 目录 + 代理（服务为空时代理卡仍渲染）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    frame("modtab");
    DOCTEST_CHECK(ui.find("settings.card.modules") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.    DOCTEST_CHECK(ui.find("settings.card.directory") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.proxy") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance") == nullptr);

    // 日志 tab：设置卡（保留天数 + 级别）+ 日志查看卡（tail + 刷新按钮）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    frame("logtab");
    DOCTEST_CHECK(ui.find("settings.card.log") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log.level.dd") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tab.log.scroll") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tab.log.refresh.bg") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.dd") == nullptr);

    mod->onStop();
    ::core::theme::setDark();  // 恢复全局默认主题，不影响其他用例
    return true;
})

// 日志查看卡：刷新按钮 → 重读 tail
// 读盘在 compose 顶部，不在 scrollView content lambda 内；日志文件缺失时渲染空态不崩

REGISTER_TEST(settings_log_viewer_refresh_button,
              "settings: 日志查看 tail 渲染 + 刷新按钮重读（Task B headless）", {
    test_support::TestEnv env(kCfgPrefix, "logviewer", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "logviewer_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    eui::Ui ui;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    ui.begin("lv    mod->compose(ui, screen);
    ui.end();

    auto* refresh = ui.find("settings.tab.log.refresh.bg");
    DOCTEST_REQUIRE(refresh != nullptr);
    DOCTEST_REQUIRE(refresh->onClick != nullptr);
    refresh->onClick();   // 置重读标记；下一帧 compose 顶部读一次 tail
    ui.begin("lv    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.tab.log.scroll") != nullptr);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 代理端口校验失败反馈（红字状态行 + 下一次输入清除）：设置页无创建弹窗，端口 draft
// 就地提交；反馈通道对齐 claude formError 语义——校验错误以红字呈现
//（settings.subtitle 走 colors.error，非 toast），且随下一次输入自动清除。

REGISTER_TEST(settings_port_error_clear_on_input,
              "settings: 端口校验失败 → 红字状态行 + 下一次输入清除（headless）", {
    test_support::TestEnv env(kCfgPrefix, "portformerr", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "portformerr_cfg"));   // 临时 config，不碰真机配置
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    // Task B tab 化：代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    frame("pf    auto* input = ui.find("settings.card.proxy.port.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onTextInput != nullptr);
    eui::TextInputEvent bad;
    bad.text = "abc";               // 非纯数字 → parseProxyPortInput 拒绝
    input->onTextInput(bad);
    frame("pf    // 点「应用」提交非法 draft（与回车 onEnter 同走 applyProxyPort）
    auto* apply = ui.find("settings.card.proxy.port.apply.bg");
    DOCTEST_REQUIRE(apply != nullptr);
    DOCTEST_REQUIRE(apply->onClick != nullptr);
    apply->onClick();
    frame("pf    const auto* sub = ui.find("settings.subtitle");
    DOCTEST_REQUIRE(sub != nullptr);
    DOCTEST_CHECK(sub->text.find("端口无效") != std::string::npos);  // 红字校验错误行
    // 非法端口未落盘：proxyPort 仍是默认    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().proxyPort,
    // 下一次输入 → 校验错误清除，subtitle 回静态说明
    auto* input    DOCTEST_REQUIRE(input    eui::TextInputEvent fix;
    fix.text = "    input    frame("pf    const auto* sub    DOCTEST_REQUIRE(sub    DOCTEST_CHECK(sub    mod->onStop();
    return true;
})

REGISTER_TEST(settings_theme_switch_reflects, "settings: theme switch reflects on recompose", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    eui::Screen screen;
    screen.width =    screen.height =    eui::Ui ui;

    // 页面底色载体是主窗口 clearColor（随 applyWindowVisuals 按主题重设），不是
    // compose 自铺的 settings.pagebg——自铺不透明底会把帧缓冲 alpha 恒置    // 磨砂档下主体看不见效果。
    ::core::theme::setDark();
    module::settings::applyWindowVisuals(module::settings::kWindowEffectOff);
    const float darkClearR = app::currentClearColor().r;

    ::core::theme::setLight();
    module::settings::applyWindowVisuals(module::settings::kWindowEffectOff);
    const float lightClearR = app::currentClearColor().r;
    DOCTEST_CHECK(darkClearR != lightClearR);

    // 反断言：compose 不得出现全屏自铺底（加回即主体效果全灭）
    ui.begin("light");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.pagebg") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance") != nullptr);

    ::core::theme::setDark();  // 恢复
    return true;
})


REGISTER_TEST(settings_log_dropdown_popup_stays_in_window, "settings: log dropdown popup stays in window (openUp)", {
    // 必须隔离配置：不隔离会读到本机配置，卡片数变多、几何随机器漂移
    test_support::TestEnv env(kCfgPrefix, "openup", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "openup_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    // 日志卡位于「日志」tab 顶部（cardsTopY =    // +    //    // 卡顶游标只累计本 tab 的卡——全量累加会把这里算错。
    eui::Screen screen;
    screen.width =    screen.height =    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    ui.begin("small");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* field = ui.find("settings.card.log.dd.field");
    const auto* popup = ui.find("settings.card.log.dd.popup");
    DOCTEST_REQUIRE(field != nullptr);
    DOCTEST_REQUIRE(popup != nullptr);
    DOCTEST_CHECK(popup->frame.y >=    DOCTEST_CHECK(popup->frame.y + popup->frame.height <= screen.height);
    DOCTEST_CHECK(popup->frame.y + popup->frame.height <= field->frame.y);

    // 高窗口    eui::Screen tall;
    tall.width =    tall.height =    DOCTEST_REQUIRE(clickSettingsTab(*mod, tall,    ui.begin("tall");
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


REGISTER_TEST(settings_path_text_vertically_centered, "settings: path text vertically centered", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    // Task B tab 化：代理/目录卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    ui.begin("center");
    mod->compose(ui, screen);
    ui.end();

    // 目录路径是可见圆角框内的单行文本，必须显式 Center（缺省 Top 会贴框顶）
    const auto* path = ui.find("settings.card.directory.path.value");
    DOCTEST_REQUIRE(path != nullptr);
    DOCTEST_CHECK(path->verticalAlign == eui::VerticalAlign::Center);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 切换保留档位立即执行一次清理

REGISTER_TEST(settings_retention_change_prunes_immediately, "settings: retention change prunes immediately", {
    // 配置与调用日志同属 app_config 库，这里只留一个夹具——两个库会各自注册一份，
    // 页面清理的那份与本用例造日志用的那份不同，断言必红。
    test_support::TestEnv env(kCfgPrefix, "prune", true);
    DOCTEST_REQUIRE(env.db != nullptr);

    const int    auto& sq = env.db->db();
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, api_key_check, "
        "model, created_at, updated_at) VALUES "
        "('p        std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());
    //    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) VALUES "
        "(        "(        "(    ).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    // Task B tab 化：日志（保留天数）下拉在「日志」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    ui.begin("prune");
    mod->compose(ui, screen);
    ui.end();

    // 点下拉第    const auto* item = ui.find("settings.card.log.dd.item.    DOCTEST_REQUIRE(item != nullptr);
    DOCTEST_REQUIRE(item->onClick != nullptr);
    item->onClick();

    // DB 立即只剩    auto prep = sq.prepare("SELECT COUNT(*) AS n FROM claude_call_logs");
    DOCTEST_REQUIRE(prep.isOk());
    DOCTEST_CHECK(prep.value().step().isOk());
    auto row = prep.value().currentRow();
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().getInt                     static_cast<int
    // 状态反馈「已清理    ui.begin("prune_status");
    mod->compose(ui, screen);
    ui.end();
    const auto* subtitle = ui.find("settings.subtitle");
    DOCTEST_REQUIRE(subtitle != nullptr);
    DOCTEST_CHECK(subtitle->text.find("已清理
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// Claude Code 接管状态行
// 数据源 ClaudeService（临时接管路径注入，不碰真实 ~/.claude）：启动代理 → takeOver →
// 「已接管 <路径>」+ 上层覆盖警告行；停止 →「未接管」
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

    auto r = ::core::Database::open(dir / "db.sqlite");
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<::core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    module::claude::ClaudeService svc(db->db(), "DevDeskTestTakeover");
    module::claude::ClaudeService::setGlobal(&svc);
    ScopedClaudeProxy proxyForSettings;  // R    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.db = &db->db();
    paths.overlayFiles.push_back(overlay);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    auto pr = svc.createProfile("接管", "https://up.example.com",
                                "sk-takeover-    DOCTEST_REQUIRE(pr.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pr.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    // Task B tab 化：接管状态行随代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    // DOCTEST_CHECK 宏内含 return false，不能进返回 string 的 lambda——故用 out 参数收敛
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
    auto start = svc.startProxy("    DOCTEST_REQUIRE(start.isOk());
    // 接管状态是只读快照（compose 不每帧 stat sidecar）。刷新点 = onStart / 进页面 /
    // 切模块 tab / 本页改端口后；代理是页外启动的，故这里显式走一次「进页面」。
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
        const auto* warn = ui.find("settings.card.proxy.takeover.warn.        DOCTEST_REQUIRE(warn != nullptr);   // overlay 含同名 env 键 → 命中
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
    platform::keychain::deleteSecret(
        "DevDeskTestTakeover", "claude:" + std::to_string(pr.value().id));
    fs::remove_all(dir, ec);
    return true;
})

// 未接管 + 警告非空时警告行仍要渲染：接管失败（settings.json 坏 JSON → takeOver 在
// 写 sidecar 之前中止）⇒ isClaudeCodeManaged()=false，但失败原因已进 lastSyncWarnings。
// 渲染门与卡高算源若不一致，这行永不显示而卡高仍预留——本用例锁「行画出来 + 状态行
// 仍如实写未接管」。
REGISTER_TEST(settings_takeover_warn_unmanaged,
              "settings: 未接管 + 警告非空 → 警告行仍渲染", {
    namespace fs = std::filesystem;
    const fs::path dir =
        test_temp::tempPath("devdesk_test_settings_warn_unmanaged");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto r = ::core::Database::open(dir / "db.sqlite");
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<::core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    module::claude::ClaudeService svc(db->db(), "DevDeskTestWarnUnmanaged");
    module::claude::ClaudeService::setGlobal(&svc);
    ScopedClaudeProxy proxyForSettings;  // R    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.db = &db->db();
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    auto pr = svc.createProfile("警告未接管", "https://up.example.com",
                                "sk-warn-unmanaged-    DOCTEST_REQUIRE(pr.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pr.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,
        // 起代理：settings.json 是坏 JSON → 接管失败（写 sidecar 之前中止）
    {
        std::ofstream out(paths.settingsFile);
        out << "{ not valid json !!!";
    }
    auto started = svc.startProxy("    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_REQUIRE(!svc.lastSyncWarnings().empty());
    DOCTEST_CHECK(!svc.isClaudeCodeManaged());   // 未接管
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&db->db(), module::claude::settings_sync::kSidecarConfigKey));  // 失败在写盘之前

    mod->onPageEnter();  // 刷新接管状态快照
    eui::Ui ui;
    ui.begin("warn_unmanaged");
    mod->compose(ui, screen);
    ui.end();
    const auto* warn = ui.find("settings.card.proxy.takeover.warn.    DOCTEST_REQUIRE(warn != nullptr);
    DOCTEST_CHECK(warn->text.find("接管失败") != std::string::npos);
        // 状态行仍如实写「未接管」（managed 只决定文案与色）
    const auto* value = ui.find("settings.card.proxy.takeover.value");
    DOCTEST_REQUIRE(value != nullptr);
    DOCTEST_CHECK(value->text.find("未接管") != std::string::npos);

    svc.stopProxy();
    mod->onStop();
    module::claude::ClaudeService::setGlobal(nullptr);
    platform::keychain::deleteSecret(
        "DevDeskTestWarnUnmanaged", "claude:" + std::to_string(pr.value().id));
    fs::remove_all(dir, ec);
    return true;
})

// 窗口效果三档

REGISTER_TEST(settings_window_effect_defaults_and_roundtrip, "settings: windowEffect 默认 off + 三档读写往返", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg    const auto path = test_support::tempConfigFile(kCfgPrefix, "wineffect");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    // 默认 off；存量配置无该键同样回落 off
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "dark", "logRetentionDays":    }
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));

    // 三档逐一保存-读回
    for (const char* mode : {module::settings::kWindowEffectOff,
                             module::settings::kWindowEffectTranslucent,
                             module::settings::kWindowEffectFrosted}) {
        module::settings::SettingsConfig cfg;
        cfg.windowEffect = mode;
        DOCTEST_CHECK(store.save(cfg).isOk() &&
                      store.load().windowEffect == std::string(mode));
    }

    // 非法值：save 前 sanitized 收敛为 off；手写进表同样收敛
    module::settings::SettingsConfig bad;
    bad.windowEffect = "glass";
    DOCTEST_CHECK_EQ(module::settings::SettingsStore::sanitized(bad).windowEffect,
                     std::string(module::settings::kWindowEffectOff));
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "dark", "windowEffect": "vibrancy"})"));
    }
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_window_effect_param_mapping, "settings: 档位→alpha/backdrop 请求映射（纯函数）", {
    namespace ms = module::settings;
    using WE = core::platform::WindowEffect;

    // 两处 alpha 各自独立：clearColor（内容区纱）off=    // frosted=    // 两处不可取同一个值。
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectOff) ==    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectTranslucent) ==    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectFrosted) ==    DOCTEST_CHECK(ms::windowEffectAlpha("bogus") ==    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectOff) ==    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectTranslucent) ==    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) ==    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha("bogus") ==
    // 半透档目标带 **    //    // 任一 alpha 往    const float translucentOverBlack =
    DOCTEST_CHECK(translucentOverBlack >=    DOCTEST_CHECK(translucentOverBlack <=    DOCTEST_CHECK(translucentOverBlack <    // 关闭档：实色，黑壁纸下恒    DOCTEST_CHECK(    // 半透档标题栏 vs 内容区差 ≤    const float translucentBar =    DOCTEST_CHECK(std::abs(translucentOverBlack - translucentBar) <=
    // 磨砂档判据（材质输出 M =    // 标题栏 = M → 差值 ≤    // 落在能把材质抬到浅灰的区间」一条。
    constexpr float kFrostedMaterialOut =    const float frostedVeil = ms::windowEffectAlpha(ms::kWindowEffectFrosted);
    DOCTEST_CHECK(frostedVeil * (    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) >=    // 磨砂必须比半透更透：两处 alpha 都要更小
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectFrosted) <
                  ms::windowEffectAlpha(ms::kWindowEffectTranslucent));
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) <
                  ms::windowEffectTitleBarAlpha(ms::kWindowEffectTranslucent));

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

    // off：任何 active 都视觉等价（alpha=    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectOff, WE::None).empty());
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectOff, WE::Transparent).empty());
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectOff, WE::Acrylic).empty());

    // translucent：拿不到 Transparent 才提示回退不透明
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectTranslucent, WE::Transparent).empty());
    const std::string t    DOCTEST_CHECK(t
    // frosted：拿不到 Acrylic 时按实际值提示（Transparent→回退半透，None→回退不透明）
    DOCTEST_CHECK(ms::windowEffectDegradedHint(ms::kWindowEffectFrosted, WE::Acrylic).empty());
    const std::string f    DOCTEST_CHECK(f    const std::string f    DOCTEST_CHECK(f    return true;
})

REGISTER_TEST(settings_window_effect_apply_visuals, "settings: applyWindowVisuals 接线（无头：app 运行时覆盖）", {
    namespace ms = module::settings;
    using WE = core::platform::WindowEffect;

    // 半透档：clearColor alpha=    ms::applyWindowVisuals(ms::kWindowEffectTranslucent);
    DOCTEST_CHECK(app::currentClearColor().a ==    DOCTEST_CHECK(app::currentWindowEffect() == WE::None);
    // 标题栏底**独立** alpha（显式 customColor，覆盖 EUI-NEO 按 clearColor 推导的做法）
    {
        const auto tb = app::currentTitleBarAppearance();
        DOCTEST_CHECK(tb.customColor);
        DOCTEST_CHECK(tb.colorAlpha ==    }

    // 磨砂档：alpha=    ms::applyWindowVisuals(ms::kWindowEffectFrosted);
    DOCTEST_CHECK(app::currentClearColor().a ==    DOCTEST_CHECK(app::currentWindowEffect() == WE::Acrylic);
    {
        const auto tb = app::currentTitleBarAppearance();
        DOCTEST_CHECK(tb.customColor);
        DOCTEST_CHECK(tb.colorAlpha ==    }

    // 关档：alpha=    ms::applyWindowVisuals(ms::kWindowEffectOff);
    DOCTEST_CHECK(app::currentClearColor().a ==    DOCTEST_CHECK(app::currentWindowEffect() == WE::None);
    {
        const auto tb = app::currentTitleBarAppearance();
        DOCTEST_CHECK(tb.customColor);
        DOCTEST_CHECK(tb.colorAlpha ==    }
    return true;
})


REGISTER_TEST(settings_modules_card_renders, "settings: 模块管理卡渲染（    test_support::TestEnv env(kCfgPrefix, "modcard", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "modcard_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读 TestEnv + ScopedConfigPath 指向的临时 config（无文件 → 默认全启用）
    eui::Screen screen;
    screen.width =    screen.height =    // Task B tab 化：模块管理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    ui.begin("modcard");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.modules") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.    DOCTEST_CHECK(ui.find("settings.card.modules.sw.    DOCTEST_CHECK(ui.find("settings.card.modules.sw.    // 桌宠（sw.    DOCTEST_CHECK(ui.find("settings.card.modules.sw.    DOCTEST_CHECK(ui.find("settings.card.modules.sw.    const auto* agentName = ui.find("settings.card.modules.name.    DOCTEST_REQUIRE(agentName != nullptr);
    DOCTEST_CHECK(agentName->text == "智能体");
    // agent 行开关默认**不**勾选（默认清单不含 kModuleIdAgent）。track 色 =
    // checked ? on : off——默认开的 markdown 行（sw.    // 同主题下色必不同，抓「回退成默认勾选」的回归（Color 无 operator==，
    // 逐分量比）
    const auto* mdTrack = ui.find("settings.card.modules.sw.    const auto* agentTrack = ui.find("settings.card.modules.sw.    DOCTEST_REQUIRE(mdTrack != nullptr);
    DOCTEST_REQUIRE(agentTrack != nullptr);
    DOCTEST_CHECK(agentTrack->color.r != mdTrack->color.r ||
                  agentTrack->color.g != mdTrack->color.g ||
                  agentTrack->color.b != mdTrack->color.b);
    // 描述文案 = 即时生效口径（活门控）：断言「即时生效」在文、「重启」不在
    const auto* desc = ui.find("settings.card.modules.desc");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_CHECK(desc->text.find("即时生效") != std::string::npos);
    DOCTEST_CHECK(desc->text.find("重启") == std::string::npos);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_gated_proxy_card_hidden, "settings: claude 模块关闭 → 代理卡整卡隐藏", {
    test_support::TestEnv env(kCfgPrefix, "gateclaude", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "gateclaude_cfg"));
    // claude_proxy 关闭（markdown/todo 开）
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdSettings};
    DOCTEST_REQUIRE(module::settings::SettingsStore(::core::appDatabase()).save(cfg).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读临时 config：claude 关
    eui::Screen screen;
    screen.width =    screen.height =    eui::Ui ui;
    // Task B tab 化：代理/目录/模块卡在「模块」tab，日志卡在「日志」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    ui.begin("gateclaude");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.proxy") == nullptr);  // 整卡隐藏
    DOCTEST_CHECK(ui.find("settings.card.directory") != nullptr);  // markdown 开
    DOCTEST_CHECK(ui.find("settings.card.modules") != nullptr);

    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    ui.begin("gateclaude_log");
    mod->compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("settings.card.log") != nullptr);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_gated_directory_card_hidden, "settings: markdown 模块关闭 → 目录卡整卡隐藏", {
    test_support::TestEnv env(kCfgPrefix, "gatemd", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "gatemd_cfg"));
    // markdown 关闭（claude 开）
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdClaudeProxy,
                          module::settings::kModuleIdSettings};
    DOCTEST_REQUIRE(module::settings::SettingsStore(::core::appDatabase()).save(cfg).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width =    screen.height =    // Task B tab 化：目录/代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    ui.begin("gatemd");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.directory") == nullptr);  // 整卡隐藏
    DOCTEST_CHECK(ui.find("settings.card.proxy") != nullptr);  // claude 开
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 效果下拉不超外观卡

REGISTER_TEST(settings_effect_dropdown_fits_min_window,
              "settings: 效果下拉    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    //    eui::Screen screen;
    screen.width =    screen.height =    eui::Ui ui;
    ui.begin("minwin");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* card = ui.find("settings.card.appearance");
    const auto* themeDd = ui.find("settings.card.appearance.theme.ddwrap");
    const auto* font = ui.find("settings.card.appearance.font.ddwrap");
    const auto* effect = ui.find("settings.card.appearance.effect.ddwrap");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(themeDd != nullptr);
    DOCTEST_REQUIRE(font != nullptr);
    DOCTEST_REQUIRE(effect != nullptr);
    // 效果下拉右缘收在卡内（设计右缘留白    DOCTEST_CHECK(effect->frame.x + effect->frame.width <=
                  card->frame.x + card->frame.width -    // 字号下拉不与主题模式下拉重叠、效果下拉不与字号下拉重叠
    DOCTEST_CHECK(font->frame.x >= themeDd->frame.x + themeDd->frame.width);
    DOCTEST_CHECK(effect->frame.x >= font->frame.x + font->frame.width);

    // 宽窗    eui::Screen wide;
    wide.width =    wide.height =    ui.begin("wide");
    mod->compose(ui, wide);
    ui.end();
    ui.layout(wide.width, wide.height);
    const auto* wideFont = ui.find("settings.card.appearance.font.ddwrap");
    const auto* wideEffect = ui.find("settings.card.appearance.effect.ddwrap");
    const auto* wideCard = ui.find("settings.card.appearance");
    DOCTEST_REQUIRE(wideFont != nullptr);
    DOCTEST_REQUIRE(wideEffect != nullptr);
    DOCTEST_REQUIRE(wideCard != nullptr);
    DOCTEST_CHECK_EQ(wideFont->frame.x - wideCard->frame.x,    DOCTEST_CHECK_EQ(wideEffect->frame.x - wideCard->frame.x,
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

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
    for (size_t i =        if (parent->orderedChildren[i] == child) return static_cast<int>(i);
    }
    return -}
} // namespace

// 日志卡两行同列
//   ① 两个下拉同列：field.frame.x 相等、同宽同高；
//   ② 标签同列 + 标签→下拉间距一致；
//   ③ 标签单行：框高 ≤ 单行行高、文本无换行符、估算文本宽 ≤ 标签框宽
//     （「日志保留」REGISTER_TEST(settings_log_rows_same_column,
              "settings: 日志 tab 两下拉同列 + 标签单行（错位修复）", {
    test_support::TestEnv env(kCfgPrefix, "logcols", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "logcols_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    eui::Screen screen;
    screen.width =    screen.height =    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen,    eui::Ui ui;
    ui.begin("logcols");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* field    const auto* field    const auto* label    const auto* label    const auto* card = ui.find("settings.card.log");
    DOCTEST_REQUIRE(field    DOCTEST_REQUIRE(field    DOCTEST_REQUIRE(label    DOCTEST_REQUIRE(label    DOCTEST_REQUIRE(card != nullptr);

    // ① 两下拉同列（左边缘 / 宽 / 高）
    DOCTEST_CHECK_MESSAGE(std::abs(field                          "两下拉 frame.x " + std::to_string(field                              " vs " + std::to_string(field    DOCTEST_CHECK(std::abs(field    DOCTEST_CHECK(std::abs(field
    // ② 标签同列 + 标签→下拉间距一致 + 标签与控件垂直居中对齐（基线口径）
    DOCTEST_CHECK(std::abs(label    DOCTEST_CHECK(std::abs(label    const float gap    const float gap    DOCTEST_CHECK_MESSAGE(std::abs(gap                          "标签→下拉间距 " + std::to_string(gap                              std::to_string(gap    DOCTEST_CHECK(gap    DOCTEST_CHECK(std::abs((label                           (field    DOCTEST_CHECK(std::abs((label                           (field
    // ③ 标签单行（框高 / 无换行 / 文本放得进框）
    for (const core::dsl::Element* label : {label        DOCTEST_CHECK_MESSAGE(label->frame.height <=                              "标签框高 " + std::to_string(label->frame.height));
        DOCTEST_CHECK_MESSAGE(label->text.find('\n') == std::string::npos,
                              "标签文案含换行：" + label->text);
        const float textW =
            module::settings::estimatedTextWidth(label->text, label->fontSize);
        DOCTEST_CHECK_MESSAGE(textW <= label->frame.width +                              "标签文本宽 " + std::to_string(textW) + " > 框宽 " +
                                  std::to_string(label->frame.width));
    }
    DOCTEST_CHECK(std::string(label    DOCTEST_CHECK(std::string(label
    // 不与标题重叠、两行自上而下不叠，说明列在控件列右侧，且内容底沿仍留 ≥    DOCTEST_CHECK(field    DOCTEST_CHECK(field    const auto* hint = ui.find("settings.card.log.hint");
    DOCTEST_REQUIRE(hint != nullptr);
    DOCTEST_CHECK(hint->frame.x >= field    DOCTEST_CHECK(field                  card->frame.y + card->frame.height +
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 行盒 / 行距 / 卡高随生效字号派生（roundFs 口径见 settings_ui_logic.h）
// 纯件例与 compose 例分工：
//   - 纯件例 = proxyCardHeightFor 在////   - compose 例 = 三 tab 每卡行盒矩形两两不重叠（跨卡叠印同池判定；x 相离的同行
//     「标签|控件|说明」列不算重叠）+ 行盒 ⊆ 卡 frame + 派生钉值。
// 缩放守卫：fontScale 是进程级全局状态，用 RAII 还原（fail-fast REQUIRE 提前 return
// 时靠析构，尾部手动还原会漏）。
REGISTER_TEST(settings_fs_derive_pure,
              "settings: 行盒派生纯件 proxyCardHeightFor（    namespace ms = module::settings;
    struct FontScaleGuard {
        float prev;
        explicit FontScaleGuard(float s)
            : prev(::core::theme::fontScale()) {
            ::core::theme::setFontScale(s);
        }
        ~FontScaleGuard() { ::core::theme::setFontScale(prev); }
    };
    {
        FontScaleGuard g{    // 零回归锚：与字面公式（        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(        DOCTEST_CHECK_EQ(ms::roundFs(        DOCTEST_CHECK_EQ(ms::roundFs(    }
    {
        FontScaleGuard g{        // round(基准×        DOCTEST_CHECK_EQ(ms::roundFs(        DOCTEST_CHECK_EQ(ms::roundFs(        // 无警告        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(    }
    return true;
})

REGISTER_TEST(settings_card_rows_scale_derived,
              "settings: 卡片行盒派生（    test_support::TestEnv env(kCfgPrefix, "fsderive", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "fsderive_cfg"));
    // pet 已摘除（    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdClaudeProxy};
    DOCTEST_REQUIRE(
        module::settings::SettingsStore(::core::appDatabase()).save(cfg).isOk());
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读临时 config：全模块启用、fontScale=
    eui::Screen screen;
    screen.width =    screen.height =
    // 每卡参与判定的行元素（条件行——降级提示 / 警告行在 headless 默认档不渲染，
    // 不列入）。下拉字段包装（.ddwrap）随字号派生并入池。
    struct CardRows {
        int tab;
        const char* card;
        std::vector<const char*> rows;
    };
    const std::vector<CardRows> kCards = {
        {         {"settings.card.appearance.title", "settings.card.appearance.desc",
          "settings.card.appearance.theme.label",
          "settings.card.appearance.font.label",
          "settings.card.appearance.effect.label"}},
        {         {"settings.card.editor.title", "settings.card.editor.hint",
          "settings.card.editor.ddwrap"}},
        {         {"settings.card.modules.title", "settings.card.modules.desc",
          "settings.card.modules.name.          "settings.card.modules.name.          "settings.card.modules.name.          "settings.card.modules.name.        {         {"settings.card.pet.title", "settings.card.pet.desc",
          "settings.card.pet.skin.label", "settings.card.pet.skin.ddwrap",
          "settings.card.pet.scale.label", "settings.card.pet.scale.ddwrap"}},
        {         {"settings.card.directory.title", "settings.card.directory.hint"}},
        {         {"settings.card.proxy.title", "settings.card.proxy.desc",
          "settings.card.proxy.port.label", "settings.card.proxy.port.hint",
          "settings.card.proxy.takeover.label",
          "settings.card.proxy.takeover.value"}},
        {         {"settings.card.log.title", "settings.card.log.label",
          "settings.card.log.hint", "settings.card.log.ddwrap",
          "settings.card.log.level.label", "settings.card.log.level.hint",
          "settings.card.log.level.ddwrap"}},
        {    };

    struct FontScaleGuard {
        float prev;
        explicit FontScaleGuard(float s)
            : prev(::core::theme::fontScale()) {
            ::core::theme::setFontScale(s);
        }
        ~FontScaleGuard() { ::core::theme::setFontScale(prev); }
    };

    // 判定 helper（返回空串 = 通过；框架断言宏展开含 return false，只能写在用例体里）：
    //   ① 行盒矩形两两不重叠（同 tab 全行池化，跨卡叠印也在此被抓）
    //   ② 卡 frame 两两不重叠 + 行盒 ⊆ 所属卡 frame（卡高没跟派生即红）
    //   ③ 派生钉值：行盒高 / y 偏移 == 字面期望（at 按 scale 三档选值）
    const auto tabProblem = [&](eui::Ui& ui, int tab, float scale) {
        struct Rect {
            const char* id;
            float x        };
        std::vector<Rect> rowRects;
        std::vector<Rect> cardRects;
        const auto nearEq = [](float a, float b) {
            return std::fabs(a - b) <        };
        for (const CardRows& cr : kCards) {
            if (cr.tab != tab) continue;
            const auto* card = ui.find(cr.card);
            if (card == nullptr) {
                return std::string("缺卡 ") + cr.card;
            }
            const float cardBottom = card->frame.y + card->frame.height;
            cardRects.push_back({cr.card, card->frame.x, card->frame.y,
                                 card->frame.x + card->frame.width,
                                 cardBottom});
            for (const char* rid : cr.rows) {
                const auto* row = ui.find(rid);
                if (row == nullptr) {
                    return std::string("缺行 ") + rid;
                }
                if (row->frame.y < card->frame.y -                    row->frame.y + row->frame.height > cardBottom +                    return std::string("行越卡: ") + rid + " 行底 " +
                           std::to_string(row->frame.y + row->frame.height) +
                           " 卡底 " + std::to_string(cardBottom);
                }
                rowRects.push_back({rid, row->frame.x, row->frame.y,
                                    row->frame.x + row->frame.width,
                                    row->frame.y + row->frame.height});
            }
        }
        for (size_t i =            for (size_t j = i +                const Rect& p = rowRects[i];
                const Rect& q = rowRects[j];
                if (p.x                    p.y                    return std::string("行盒叠印: ") + p.id + " × " + q.id;
                }
            }
        }
        for (size_t i =            for (size_t j = i +                const Rect& p = cardRects[i];
                const Rect& q = cardRects[j];
                if (p.x                    p.y                    return std::string("卡叠印: ") + p.id + " × " + q.id;
                }
            }
        }
    // 派生钉值（字面期望）。f(id) = frame 高，yOff = 两行 frame.y 差
        const auto heightOf = [&](const char* id) -> float {
            const auto* e = ui.find(id);
            return e != nullptr ? e->frame.height : -        };
        const auto yOf = [&](const char* id) -> float {
            const auto* e = ui.find(id);
            return e != nullptr ? e->frame.y : -        };
        const auto pin = [&](const char* what, float got, float want) {
            if (!nearEq(got, want)) {
                return std::string(what) + " = " + std::to_string(got) +
                       "（期望 " + std::to_string(want) + "）";
            }
            return std::string();
        };
        std::string err;
    // 三档期望值按生产公式 roundFs(基准) = round(基准 × scale) 手算
        const auto at = [&](float a            return scale <        };
        if (tab ==            err = pin("appearance.title 高", heightOf("settings.card.appearance.title"),
                      at(            if (err.empty())
                err = pin("appearance.desc 高",
                          heightOf("settings.card.appearance.desc"),
                          at(            if (err.empty())
                err = pin("appearance.theme.label 高",
                          heightOf("settings.card.appearance.theme.label"),
                          at(            if (err.empty())
                err = pin("editor.hint 高", heightOf("settings.card.editor.hint"),
                          at(            if (err.empty())
                err = pin("editor.ddwrap 行顶（editorFontDdOffsetY=                          yOf("settings.card.editor.ddwrap") -
                              yOf("settings.card.editor"),
                          at(            if (err.empty())
                err = pin("editor.ddwrap 高（logFieldHeight=roundFs(                          heightOf("settings.card.editor.ddwrap"),
                          at(            if (err.empty())
                err = pin("appearance 卡高", heightOf("settings.card.appearance"),
                          at(        } else if (tab ==            err = pin("modules.name.                      heightOf("settings.card.modules.name.                      at(            if (err.empty())
                err = pin("modules 行距",
                          yOf("settings.card.modules.name.                              yOf("settings.card.modules.name.                          at(            if (err.empty())
                err = pin("modules 行内偏移",
                          yOf("settings.card.modules.rowdesc.                              yOf("settings.card.modules.name.                          at(            if (err.empty())
                err = pin("modules 卡高", heightOf("settings.card.modules"),
                          at(            if (err.empty())
                err = pin("pet.skin.ddwrap 行顶（skinTop=                          yOf("settings.card.pet.skin.ddwrap") -
                              yOf("settings.card.pet"),
                          at(            if (err.empty())
                err = pin("pet.skin.ddwrap 高（ddH=roundFs(                          heightOf("settings.card.pet.skin.ddwrap"),
                          at(            if (err.empty())
                err = pin("pet.scale.ddwrap 行顶（scaleTop=skinTop+ddH+roundFs(                          yOf("settings.card.pet.scale.ddwrap") -
                              yOf("settings.card.pet"),
                          at(            if (err.empty())
                err = pin("pet.scale.ddwrap 高（ddH=roundFs(                          heightOf("settings.card.pet.scale.ddwrap"),
                          at(            if (err.empty())
                err = pin("pet 卡高", heightOf("settings.card.pet"),
                          at(            if (err.empty())
                err = pin("proxy.takeover.label 高",
                          heightOf("settings.card.proxy.takeover.label"),
                          at(            if (err.empty())
                err = pin("proxy 卡高（无警告）",
                          heightOf("settings.card.proxy"),
                          at(        } else {
    // 日志卡入派生池：    // 标签盒 / 字段高 / 行 y 随生效字号派生
            err = pin("log.label 高（logLabelHeight=roundFs(                      heightOf("settings.card.log.label"),
                      at(            if (err.empty())
                err = pin("log.ddwrap 行顶（logDropdownOffsetY）",
                          yOf("settings.card.log.ddwrap") -
                              yOf("settings.card.log"),
                          at(            if (err.empty())
                err = pin("log.ddwrap 高（logFieldHeight=roundFs(                          heightOf("settings.card.log.ddwrap"),
                          at(            if (err.empty())
                err = pin("log.level.ddwrap 行顶（logLevelOffsetY）",
                          yOf("settings.card.log.level.ddwrap") -
                              yOf("settings.card.log"),
                          at(            if (err.empty())
                err = pin("log 卡高", heightOf("settings.card.log"),
                          at(        }
        return err;
    };

    // 断言宏展开含 return，只能留在用例体——helper 只收集问题串，逐 tab 一条
    const auto runAllTabs = [&](float scale) -> std::vector<std::string> {
        std::vector<std::string> problems;
        for (int tab : {            if (!clickSettingsTab(*mod, screen, tab)) {
                problems.push_back("tab 切换失败: " + std::to_string(tab));
                continue;
            }
            eui::Ui ui;
            ui.begin(scale <                                  : (scale >                                                  : "fsderive            mod->compose(ui, screen);
            ui.end();
            ui.layout(screen.width, screen.height);
            problems.push_back(tabProblem(ui, tab, scale));
        }
        return problems;
    };

    //    {
        FontScaleGuard g{        for (const std::string& problem : runAllTabs(            DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
        }
    }
    //    {
        FontScaleGuard g{        for (const std::string& problem : runAllTabs(            DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
        }
    }
    //    // 卡高 roundFs(    {
        FontScaleGuard g{        for (const std::string& problem : runAllTabs(            DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
        }
    }

    mod->onStop();
    return true;
})

// 文案宽度纯件（③ 判据的来源）
REGISTER_TEST(settings_label_fit_pure,
              "settings: 文案宽度估算/单行适配/皮肤下拉宽度（不溢出判据）", {
    namespace ms = module::settings;
    // 估算口径钉死：ASCII    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("abc",    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("永久保留",    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("ab中文",                           (    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("",
    // 放得下 → 原文；放不下 → 截断 + U+    const std::string kLong = "super-long-skin-name-here";
    DOCTEST_CHECK(ms::fitControlLabel("robot",    const std::string fit = ms::fitControlLabel(kLong,    DOCTEST_CHECK(fit != kLong);
    DOCTEST_CHECK(fit.size() >=    DOCTEST_CHECK(fit.compare(fit.size() -    DOCTEST_CHECK_MESSAGE(ms::estimatedTextWidth(fit,                          "截断后宽 " +
                              std::to_string(ms::estimatedTextWidth(fit,    DOCTEST_CHECK(kLong.compare(                                fit.substr(
                           (    return true;
})

