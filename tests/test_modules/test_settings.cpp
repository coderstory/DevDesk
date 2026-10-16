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
#include "core/event_bus.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/tool.h"          // ITool / ToolArgs
#include "github_sync_config.h" // 自动推送开关接线用例（配置夹具）
#include "github_sync_exit_hook.h" // 退出补推用例（经 free 函数驱动）
#include "platform/github_api.h" // 推送分支定向用例：静态默认发送器注入面
#include "platform/credential_protection.h" // Token 落库形态断言（storesPlaintext）
#include "platform/json.h"      // 读 app.read_settings 的 JSON 出参
#include "platform/system_theme.h"
#include "quota_home_bridge.h"  // isQuotaFeatureEnabled / setQuotaFeatureEnabled

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

    // 读配置字节走 test_support::readRawConfigRow；供 settings.json 接管用例按字节读文件用。

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
    for (int up = 0; up < 8; ++up) {
        if (std::filesystem::exists(dir / "src" / "core" / "proxy_port.h", ec) &&
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
        hits.push_back(relPath + ":0: 读不到该文件（护栏无法取证）");
        return hits;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
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
std::string r0CouplingGuardReport() {
    const std::filesystem::path root = findRepoRoot();
    if (root.empty()) {
        std::error_code ec;
        return "护栏无法定位仓库根（cwd=" +
               std::filesystem::current_path(ec).string() +
               "）：请从仓库根或 build/ 下运行 devdesk_tests —— 护栏靠 cwd 上溯"
               "定位（不能用 __FILE__：ccache 会把绝对路径相对化）";
    }
    const std::string rel = "src/module_settings/settings_module.cpp";

    std::string report;
    // 1) 硬针：跨模块 include 与限定名，注释里也不许出现
    for (const std::string& hit : scanFileForTokens(
             root, rel, {"claude_service.h", "module::claude"}, false)) {
        report += "设置页又直接引用了兄弟模块 claude：" + hit + "\n";
    }
    // 2) 软针：类型名本身——按行剥注释后再查
    for (const std::string& hit :
         scanFileForTokens(root, rel, {"ClaudeService"}, true)) {
        report += "设置页代码里又直呼了 ClaudeService：" + hit + "\n";
    }
    // 3) 正向：必须真的经 core 只读服务定位器取
    const std::string src = readWholeFile(root / rel);
    if (src.find("core/service_locator.h") == std::string::npos) {
        report += "设置页没有 include core/service_locator.h —— 代理运行面应由"
                  " core 只读服务定位器取（R0/D6）\n";
    }
    if (src.find("::core::IClaudeProxyService") == std::string::npos) {
        report += "设置页没有用 core::IClaudeProxyService —— 定位器通道被摘掉了？"
                  "（解耦不是删功能）\n";
    }
    // 4) 提供方：装配段必须真的注册（否则设置页永久静默降级成「无服务」，而用例侧
    //    各自注册、覆盖不到这条生产接线）
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
    for (const char* def : {"kDefaultProxyPort = 8888", "kMinProxyPort = 1",
                            "kMaxProxyPort = 65535"}) {
        if (coreSrc.find(def) == std::string::npos) {
            report += coreHeader + " 缺少定义 '" + def + "'\n";
        }
    }
    // 消费方：不得自带字面量，必须从 core 取
    for (const std::string& rel : consumers) {
        for (const std::string& hit :
             scanFileForTokens(root, rel,
                               {"kDefaultProxyPort = 8888", "kMinProxyPort = 1",
                                "kMaxProxyPort = 65535"},
                               false)) {
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
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg1", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "missing");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());
    const auto cfg = store.load();
    DOCTEST_CHECK(cfg.darkTheme);                        // 默认深色（方案 B Dark）
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);  // W9 默认（em 语义修复后）
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);
    return true;
})

REGISTER_TEST(settings_proxy_autostart_default_off, "settings: claudeProxyAutoStart 默认 false + 读写往返", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg2", true);
    // 默认不启动代理
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
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "light", "logRetentionDays": 30})"));
    }
    DOCTEST_CHECK_FALSE(store.load().claudeProxyAutoStart);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_quota_switch_default_off, "settings quota_switch: quotaEnabled 默认 false + 读写往返", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "quota");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    // 默认 false
    DOCTEST_CHECK_FALSE(store.load().quotaEnabled);

    // 开 → 持久化 → 读回 true
    module::settings::SettingsConfig cfg;
    cfg.quotaEnabled = true;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_CHECK(store.load().quotaEnabled);

    // 存量配置无该键 → 保持默认 false（兜底）
    {
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(),
            R"({"theme": "light", "logRetentionDays": 7})"));
    }
    DOCTEST_CHECK_FALSE(store.load().quotaEnabled);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_quota_switch_bridge_flag, "settings quota_switch: bridge 标记默认 false + 设置后即时同步", {
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg");
    const auto path = test_support::tempConfigFile(kCfgPrefix, "bridgeflag");
    removeQuietly(path);

    // 默认 false
    DOCTEST_CHECK_FALSE(devdesk::quota_bridge::isQuotaFeatureEnabled());

    // 开启 bridge
    devdesk::quota_bridge::setQuotaFeatureEnabled(true);
    DOCTEST_CHECK(devdesk::quota_bridge::isQuotaFeatureEnabled());

    // 关闭 bridge
    devdesk::quota_bridge::setQuotaFeatureEnabled(false);
    DOCTEST_CHECK_FALSE(devdesk::quota_bridge::isQuotaFeatureEnabled());

    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_quota_switch_data_preserved, "settings quota_switch: 关闭后数据保留（库里的 quota 行不删）", {
    // 验证语义：setQuotaFeatureEnabled(false) 不触发任何数据库写操作。
    // 本用例只测 setQuotaFeatureEnabled(false) 把标记切回 false 即可——数据保留由架构保证
    //（bridge 只管进程级 atomic bool，不碰数据库；quota 表由 QuotaStore 独立维护）。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg");
    const auto path = test_support::tempConfigFile(kCfgPrefix, "datakeep");
    removeQuietly(path);

    // 先设为 true
    devdesk::quota_bridge::setQuotaFeatureEnabled(true);
    DOCTEST_CHECK(devdesk::quota_bridge::isQuotaFeatureEnabled());

    // 关闭：标记切回 false，数据库不受影响（bridge 无 write 路径）
    devdesk::quota_bridge::setQuotaFeatureEnabled(false);
    DOCTEST_CHECK_FALSE(devdesk::quota_bridge::isQuotaFeatureEnabled());

    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_proxy_port_roundtrip_clamp, "settings: proxyPort 默认/读写往返/非法回落/输入解析", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg3", true);
    using namespace module::settings;
    const auto path = test_support::tempConfigFile(kCfgPrefix, "proxyport");
    removeQuietly(path);
    SettingsStore store(::core::appDatabase());

    auto withPort = [](int port) {
        SettingsConfig c;
        c.proxyPort = port;
        return c;
    };

    // 默认 8888
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

    // 存量配置无 claudeProxyPort 键 → 默认 8888
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "dark", "claudeProxyAutoStart": true})"));
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
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg4", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

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
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg5", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "zero");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

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
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg6", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "corrupt");
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
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);  // 解析失败 → 默认
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);
        // 脏内容没有任何**文件**载体。「原始内容可查」由两件事兑现：① 坏行原样留在
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
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg7", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "partial");
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
    DOCTEST_CHECK_EQ(cfg.fontScalePercent, 100);   // 缺失 → 默认
    DOCTEST_CHECK_EQ(cfg.logRetentionDays, 30);    // 缺失 → 默认
    removeQuietly(path);
    return true;
})

    // fontScalePercent / fontScalePercentV2 的存量值（85/115/130）一律忽略、回落
    // 默认 100；V3 键下的值按字面生效
REGISTER_TEST(settings_legacy_fontscale_migration, "settings: legacy font scale keys reset to 100", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg8", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "legacy");
    removeQuietly(path);
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\": \"dark\", \"fontScalePercent\": 85}"));
    }
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().fontScalePercent, 100);  // 最老键 → 重置

    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"fontScalePercentV2\": 130}"));
    }
    DOCTEST_CHECK_EQ(store.load().fontScalePercent, 100);  // V2 键 → 重置

    {
        // V3 键按字面生效（V2 同时存在也被忽略）
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"fontScalePercent\": 85, \"fontScalePercentV2\": 130, \"fontScalePercentV3\": 115}"));
    }
    DOCTEST_CHECK_EQ(store.load().fontScalePercent, 115);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_theme_value_mapping, "settings: theme string mapping", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg9", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "theme");
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
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg10", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "clampwrite");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    module::settings::SettingsConfig cfg;
    cfg.fontScalePercent = 999;            // save 时应钳到 130
    auto saved = store.save(cfg);
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
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };

    frame("smoke");
    DOCTEST_CHECK(ui.find("settings.tabs.hit.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.1") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.2") != nullptr);
    DOCTEST_CHECK(ui.find("settings.cards.scroll") != nullptr);
    // 通用 tab：外观卡 + 编辑器字号卡
    DOCTEST_CHECK(ui.find("settings.card.appearance") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.effect.ddwrap") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.about") == nullptr);

    // 模块 tab：插拔开关 + 目录 + 代理（服务为空时代理卡仍渲染）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    frame("modtab");
    DOCTEST_CHECK(ui.find("settings.card.modules") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.directory") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.proxy") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance") == nullptr);

    // 日志 tab：设置卡（保留天数 + 级别）+ 日志查看卡（tail + 刷新按钮）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    frame("logtab");
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
    // 非法端口未落盘：proxyPort 仍是默认 8888
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);

    // 下一次输入 → 校验错误清除，subtitle 回静态说明
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

    // 页面底色载体是主窗口 clearColor（随 applyWindowVisuals 按主题重设），不是
    // compose 自铺的 settings.pagebg——自铺不透明底会把帧缓冲 alpha 恒置 1，半透 /
    // 磨砂档下主体看不见效果。
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

    // 日志卡位于「日志」tab 顶部（cardsTopY = 40 pad + 64 header + 16 gap + 40 tab
    // + 16 gap = 176；保留字段底 = 176 + 48 + 36 = 260，+ 8 gap + 弹层 216 = 484）。
    // 1600×1300 下方空间充足 → 默认 openDown；短窗下 484 > 高 − 16 → 改 openUp。
    // 卡顶游标只累计本 tab 的卡——全量累加会把这里算错。
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 470.0f;
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

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& sq = env.db->db();
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, api_key_check, "
        "model, created_at, updated_at) VALUES "
        "('p1', 'http://x', 'sk-***', 'chk', 'm', " +
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
    DOCTEST_CHECK_EQ(row.value().getInt64("n").value(),
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
    ScopedClaudeProxy proxyForSettings;  // R0/D6：设置页经定位器取代理运行面
    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.db = &db->db();
    paths.overlayFiles.push_back(overlay);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

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
    // DOCTEST_CHECK 宏内含 return false，不能进返回 string 的 lambda——故用 out 参数收敛
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
    ScopedClaudeProxy proxyForSettings;  // R0/D6：设置页经定位器取代理运行面
    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.db = &db->db();
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);

    auto pr = svc.createProfile("警告未接管", "https://up.example.com",
                                "sk-warn-unmanaged-0001", "");
    DOCTEST_REQUIRE(pr.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(pr.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));  // 模块 tab

        // 起代理：settings.json 是坏 JSON → 接管失败（写 sidecar 之前中止）
    {
        std::ofstream out(paths.settingsFile);
        out << "{ not valid json !!!";
    }
    auto started = svc.startProxy("127.0.0.1", 0);
    DOCTEST_REQUIRE(started.isOk());
    DOCTEST_REQUIRE(!svc.lastSyncWarnings().empty());
    DOCTEST_CHECK(!svc.isClaudeCodeManaged());   // 未接管
    DOCTEST_CHECK_FALSE(test_support::configRowExists(&db->db(), module::claude::settings_sync::kSidecarConfigKey));  // 失败在写盘之前

    mod->onPageEnter();  // 刷新接管状态快照
    eui::Ui ui;
    ui.begin("warn_unmanaged");
    mod->compose(ui, screen);
    ui.end();
    const auto* warn = ui.find("settings.card.proxy.takeover.warn.0");
    DOCTEST_REQUIRE(warn != nullptr);
    DOCTEST_CHECK(warn->text.find("接管失败") != std::string::npos);
        // 状态行仍如实写「未接管」（managed 只决定文案与色）
    const auto* value = ui.find("settings.card.proxy.takeover.value");
    DOCTEST_REQUIRE(value != nullptr);
    DOCTEST_CHECK(value->text.find("未接管") != std::string::npos);

    svc.stopProxy();
    mod->onStop();
    module::claude::ClaudeService::setGlobal(nullptr);
    fs::remove_all(dir, ec);
    return true;
})

// 窗口效果三档

REGISTER_TEST(settings_window_effect_defaults_and_roundtrip, "settings: windowEffect 默认 off + 三档读写往返", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg11", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "wineffect");
    removeQuietly(path);
    module::settings::SettingsStore store(::core::appDatabase());

    // 默认 off；存量配置无该键同样回落 off
    DOCTEST_CHECK_EQ(store.load().windowEffect, std::string(module::settings::kWindowEffectOff));
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "dark", "logRetentionDays": 30})"));
    }
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

    // 两处 alpha 各自独立：clearColor（内容区纱）off=1.0 / translucent=0.60 /
    // frosted=0.15 / 未知→1.0；窗口背景色（标题栏底）1.0 / 0.90 / 0.65 / 未知→1.0。
    // 两处不可取同一个值。
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectOff) == 1.0f);
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectTranslucent) == 0.60f);
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectFrosted) == 0.15f);
    DOCTEST_CHECK(ms::windowEffectAlpha("bogus") == 1.0f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectOff) == 1.0f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectTranslucent) == 0.90f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) == 0.65f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha("bogus") == 1.0f);

    // 半透档目标带 **236–244**（黑壁纸），两端是实机标定值不是随手取的。
    // 0.60/0.90 → 250×(1−0.10×0.40) = 240.0 落在带中央。这条同时锁常量与目标：
    // 任一 alpha 往 0.70/0.95 走 → 246.25 出上界；回到 0.45/0.90 → 236.25 出下界。
    const float translucentOverBlack =
        250.0f * ms::windowEffectThemeOpacity(ms::kWindowEffectTranslucent);
    DOCTEST_CHECK(translucentOverBlack >= 236.0f);
    DOCTEST_CHECK(translucentOverBlack <= 244.0f);
    DOCTEST_CHECK(translucentOverBlack < 250.0f);
    // 关闭档：实色，黑壁纸下恒 250
    DOCTEST_CHECK(250.0f * ms::windowEffectThemeOpacity(ms::kWindowEffectOff) == 250.0f);
    // 半透档标题栏 vs 内容区差 ≤ 20（黑壁纸：标题栏 = 250×titleAlpha）
    const float translucentBar = 250.0f * ms::windowEffectTitleBarAlpha(ms::kWindowEffectTranslucent);
    DOCTEST_CHECK(std::abs(translucentOverBlack - translucentBar) <= 20.0f);

    // 磨砂档判据（材质输出 M = 192 @ titleAlpha 0.65）：内容区 = M + contentAlpha×(250−M)，
    // 标题栏 = M → 差值 ≤ 20。差值式在 M 恒定时只惩罚内容区纱，故另加「标题栏 alpha 必须
    // 落在能把材质抬到浅灰的区间」一条。
    constexpr float kFrostedMaterialOut = 192.0f;  // 实机实测（黑壁纸，titleAlpha=0.65）
    const float frostedVeil = ms::windowEffectAlpha(ms::kWindowEffectFrosted);
    DOCTEST_CHECK(frostedVeil * (250.0f - kFrostedMaterialOut) <= 20.0f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) >= 0.6f);
    // 磨砂必须比半透更透：两处 alpha 都要更小
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

    // 半透档：clearColor alpha=0.60 + backdrop 请求 None（显式关）
    ms::applyWindowVisuals(ms::kWindowEffectTranslucent);
    DOCTEST_CHECK(app::currentClearColor().a == 0.60f);
    DOCTEST_CHECK(app::currentWindowEffect() == WE::None);
    // 标题栏底**独立** alpha（显式 customColor，覆盖 EUI-NEO 按 clearColor 推导的做法）
    {
        const auto tb = app::currentTitleBarAppearance();
        DOCTEST_CHECK(tb.customColor);
        DOCTEST_CHECK(tb.colorAlpha == 0.90f);
    }

    // 磨砂档：alpha=0.15 + Acrylic 请求 + 标题栏 0.65
    ms::applyWindowVisuals(ms::kWindowEffectFrosted);
    DOCTEST_CHECK(app::currentClearColor().a == 0.15f);
    DOCTEST_CHECK(app::currentWindowEffect() == WE::Acrylic);
    {
        const auto tb = app::currentTitleBarAppearance();
        DOCTEST_CHECK(tb.customColor);
        DOCTEST_CHECK(tb.colorAlpha == 0.65f);
    }

    // 关档：alpha=1（视觉等价实色，透明 hint 仍开）——收尾恢复，不污染后续用例
    ms::applyWindowVisuals(ms::kWindowEffectOff);
    DOCTEST_CHECK(app::currentClearColor().a == 1.0f);
    DOCTEST_CHECK(app::currentWindowEffect() == WE::None);
    {
        const auto tb = app::currentTitleBarAppearance();
        DOCTEST_CHECK(tb.customColor);
        DOCTEST_CHECK(tb.colorAlpha == 1.0f);
    }
    return true;
})


REGISTER_TEST(settings_modules_card_renders, "settings: 模块管理卡渲染（5 行模块开关）", {
    test_support::TestEnv env(kCfgPrefix, "modcard", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "modcard_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读 TestEnv + ScopedConfigPath 指向的临时 config（无文件 → 默认全启用）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：模块管理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    ui.begin("modcard");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.modules") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.1") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.2") != nullptr);
    // 桌宠已摘除，开关序: sw.0=markdown/sw.1=todo/sw.2=claude/sw.3=agent
    DOCTEST_CHECK(ui.find("settings.card.modules.sw.3") != nullptr);
    const auto* agentName = ui.find("settings.card.modules.name.3");
    DOCTEST_REQUIRE(agentName != nullptr);
    DOCTEST_CHECK(agentName->text == "智能体");
    // agent 行开关默认**不**勾选（默认清单不含 kModuleIdAgent）。track 色 =
    // checked ? on : off——默认开的 markdown 行（sw.0）与 agent 行（sw.3）
    // 同主题下色必不同，抓「回退成默认勾选」的回归（Color 无 operator==，
    // 逐分量比）
    const auto* mdTrack = ui.find("settings.card.modules.sw.0.track");
    const auto* agentTrack = ui.find("settings.card.modules.sw.3.track");
    DOCTEST_REQUIRE(mdTrack != nullptr);
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
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    // Task B tab 化：代理/目录/模块卡在「模块」tab，日志卡在「日志」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    ui.begin("gateclaude");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("settings.card.proxy") == nullptr);  // 整卡隐藏
    DOCTEST_CHECK(ui.find("settings.card.directory") != nullptr);  // markdown 开
    DOCTEST_CHECK(ui.find("settings.card.modules") != nullptr);

    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    ui.begin("gateclaude_log");
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
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // Task B tab 化：目录/代理卡在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
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
              "settings: 效果下拉 1200 最小窗收进外观卡内", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 1200 最小窗 = 页面宽 940（去掉 260 侧边栏）→ 外观卡内宽 860；效果下拉右缘须收在卡内
    eui::Screen screen;
    screen.width = 940.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
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
    // 效果下拉右缘收在卡内（设计右缘留白 28，断言留 ≥20 容差）
    DOCTEST_CHECK(effect->frame.x + effect->frame.width <=
                  card->frame.x + card->frame.width - 20.0f);
    // 字号下拉不与主题模式下拉重叠、效果下拉不与字号下拉重叠
    DOCTEST_CHECK(font->frame.x >= themeDd->frame.x + themeDd->frame.width);
    DOCTEST_CHECK(effect->frame.x >= font->frame.x + font->frame.width);

    // 宽窗 1600：保持既有位置（字号 380 / 效果 680 相对卡原点）
    eui::Screen wide;
    wide.width = 1340.0f;
    wide.height = 1000.0f;
    ui.begin("wide");
    mod->compose(ui, wide);
    ui.end();
    ui.layout(wide.width, wide.height);
    const auto* wideFont = ui.find("settings.card.appearance.font.ddwrap");
    const auto* wideEffect = ui.find("settings.card.appearance.effect.ddwrap");
    const auto* wideCard = ui.find("settings.card.appearance");
    DOCTEST_REQUIRE(wideFont != nullptr);
    DOCTEST_REQUIRE(wideEffect != nullptr);
    DOCTEST_REQUIRE(wideCard != nullptr);
    DOCTEST_CHECK_EQ(wideFont->frame.x - wideCard->frame.x, 380.0f);
    DOCTEST_CHECK_EQ(wideEffect->frame.x - wideCard->frame.x, 680.0f);

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
    for (size_t i = 0; i < parent->orderedChildren.size(); ++i) {
        if (parent->orderedChildren[i] == child) return static_cast<int>(i);
    }
    return -1;
}
} // namespace

// 日志卡两行同列
//   ① 两个下拉同列：field.frame.x 相等、同宽同高；
//   ② 标签同列 + 标签→下拉间距一致；
//   ③ 标签单行：框高 ≤ 单行行高、文本无换行符、估算文本宽 ≤ 标签框宽
//     （「日志保留」4 字 × 12px = 48 ≤ 84，恒单行）。
REGISTER_TEST(settings_log_rows_same_column,
              "settings: 日志 tab 两下拉同列 + 标签单行（错位修复）", {
    test_support::TestEnv env(kCfgPrefix, "logcols", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "logcols_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));  // 日志 tab
    eui::Ui ui;
    ui.begin("logcols");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* field1 = ui.find("settings.card.log.dd.field");        // 日志保留
    const auto* field2 = ui.find("settings.card.log.level.dd.field");  // 日志级别
    const auto* label1 = ui.find("settings.card.log.label");
    const auto* label2 = ui.find("settings.card.log.level.label");
    const auto* card = ui.find("settings.card.log");
    DOCTEST_REQUIRE(field1 != nullptr);
    DOCTEST_REQUIRE(field2 != nullptr);
    DOCTEST_REQUIRE(label1 != nullptr);  
    DOCTEST_REQUIRE(label2 != nullptr);
    DOCTEST_REQUIRE(card != nullptr);

    // ① 两下拉同列（左边缘 / 宽 / 高）
    DOCTEST_CHECK_MESSAGE(std::abs(field1->frame.x - field2->frame.x) < 0.5f,
                          "两下拉 frame.x " + std::to_string(field1->frame.x) +
                              " vs " + std::to_string(field2->frame.x));
    DOCTEST_CHECK(std::abs(field1->frame.width - field2->frame.width) < 0.5f);
    DOCTEST_CHECK(std::abs(field1->frame.height - field2->frame.height) < 0.5f);

    // ② 标签同列 + 标签→下拉间距一致 + 标签与控件垂直居中对齐（基线口径）
    DOCTEST_CHECK(std::abs(label1->frame.x - label2->frame.x) < 0.5f);
    DOCTEST_CHECK(std::abs(label1->frame.width - label2->frame.width) < 0.5f);
    const float gap1 = field1->frame.x - (label1->frame.x + label1->frame.width);
    const float gap2 = field2->frame.x - (label2->frame.x + label2->frame.width);
    DOCTEST_CHECK_MESSAGE(std::abs(gap1 - gap2) < 0.5f,
                          "标签→下拉间距 " + std::to_string(gap1) + " vs " +
                              std::to_string(gap2));
    DOCTEST_CHECK(gap1 > 0.0f);
    DOCTEST_CHECK(std::abs((label1->frame.y + label1->frame.height * 0.5f) -
                           (field1->frame.y + field1->frame.height * 0.5f)) <
                  0.5f);
    DOCTEST_CHECK(std::abs((label2->frame.y + label2->frame.height * 0.5f) -
                           (field2->frame.y + field2->frame.height * 0.5f)) <
                  0.5f);

    // ③ 标签单行（框高 / 无换行 / 文本放得进框）
    for (const core::dsl::Element* label : {label1, label2}) {
        DOCTEST_CHECK_MESSAGE(label->frame.height <= 20.5f,
                              "标签框高 " + std::to_string(label->frame.height));
        DOCTEST_CHECK_MESSAGE(label->text.find('\n') == std::string::npos,
                              "标签文案含换行：" + label->text);
        const float textW =
            module::settings::estimatedTextWidth(label->text, label->fontSize);
        DOCTEST_CHECK_MESSAGE(textW <= label->frame.width + 0.5f,
                              "标签文本宽 " + std::to_string(textW) + " > 框宽 " +
                                  std::to_string(label->frame.width));
    }
    DOCTEST_CHECK(std::string(label1->text) == "日志保留");
    DOCTEST_CHECK(std::string(label2->text) == "日志级别");

    // 不与标题重叠、两行自上而下不叠，说明列在控件列右侧，且内容底沿仍留 ≥16 下边距
    DOCTEST_CHECK(field1->frame.y > card->frame.y + 38.0f);
    DOCTEST_CHECK(field2->frame.y >= field1->frame.y + field1->frame.height);
    const auto* hint = ui.find("settings.card.log.hint");
    DOCTEST_REQUIRE(hint != nullptr);
    DOCTEST_CHECK(hint->frame.x >= field1->frame.x + field1->frame.width + 8.0f);
    DOCTEST_CHECK(field2->frame.y + field2->frame.height + 16.0f <=
                  card->frame.y + card->frame.height + 0.5f);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 行盒 / 行距 / 卡高随生效字号派生（roundFs 口径见 settings_ui_logic.h）
// 纯件例与 compose 例分工：
//   - 纯件例 = proxyCardHeightFor 在 100% 档与旧字面公式逐值相等（零回归锚）+
//     130% 档等于派生式字面期望；
//   - compose 例 = 三 tab 每卡行盒矩形两两不重叠（跨卡叠印同池判定；x 相离的同行
//     「标签|控件|说明」列不算重叠）+ 行盒 ⊆ 卡 frame + 派生钉值。
// 缩放守卫：fontScale 是进程级全局状态，用 RAII 还原（fail-fast REQUIRE 提前 return
// 时靠析构，尾部手动还原会漏）。
REGISTER_TEST(settings_fs_derive_pure,
              "settings: 行盒派生纯件 proxyCardHeightFor（100% 锚 + 130% 档）", {
    namespace ms = module::settings;
    struct FontScaleGuard {
        float prev;
        explicit FontScaleGuard(float s)
            : prev(::core::theme::fontScale()) {
            ::core::theme::setFontScale(s);
        }
        ~FontScaleGuard() { ::core::theme::setFontScale(prev); }
    };
    {
        FontScaleGuard g{1.0f};
    // 零回归锚：与字面公式（118+16n / 116，+16 下边距）逐值相等
        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(0), 132.0f);
        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(2), 166.0f);
        DOCTEST_CHECK_EQ(ms::roundFs(16.0f), 16.0f);
        DOCTEST_CHECK_EQ(ms::roundFs(18.0f), 18.0f);
    }
    {
        FontScaleGuard g{1.3f};
        // round(基准×1.3)：16→21 / 18→23 / 2→3 / 16（下边距）→21
        DOCTEST_CHECK_EQ(ms::roundFs(16.0f), 21.0f);
        DOCTEST_CHECK_EQ(ms::roundFs(18.0f), 23.0f);
        // 无警告 98+23+21=142；2 条警告 (98+23+3)+2×21+21=187
        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(0), 142.0f);
        DOCTEST_CHECK_EQ(ms::proxyCardHeightFor(2), 187.0f);
    }
    return true;
})

REGISTER_TEST(settings_card_rows_scale_derived,
              "settings: 卡片行盒派生（130% 不重叠 + 派生钉值，RAII 守卫）", {
    test_support::TestEnv env(kCfgPrefix, "fsderive", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "fsderive_cfg"));
    // 写显式全启用配置，全部卡参与几何判定
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdClaudeProxy};
    DOCTEST_REQUIRE(
        module::settings::SettingsStore(::core::appDatabase()).save(cfg).isOk());
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();  // 读临时 config：全模块启用、fontScale=100

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 每卡参与判定的行元素（条件行——降级提示 / 警告行在 headless 默认档不渲染，
    // 不列入）。下拉字段包装（.ddwrap）随字号派生并入池。
    struct CardRows {
        int tab;
        const char* card;
        std::vector<const char*> rows;
    };
    const std::vector<CardRows> kCards = {
        {0, "settings.card.appearance",
         {"settings.card.appearance.title", "settings.card.appearance.desc",
          "settings.card.appearance.theme.label",
          "settings.card.appearance.font.label",
          "settings.card.appearance.effect.label"}},
        {0, "settings.card.editor",
         {"settings.card.editor.title", "settings.card.editor.hint",
          "settings.card.editor.ddwrap"}},
        {1, "settings.card.modules",
         {"settings.card.modules.title", "settings.card.modules.desc",
          "settings.card.modules.name.0", "settings.card.modules.rowdesc.0",
          "settings.card.modules.name.1", "settings.card.modules.rowdesc.1",
          "settings.card.modules.name.2", "settings.card.modules.rowdesc.2",
          "settings.card.modules.name.3", "settings.card.modules.rowdesc.3"}},
        
        {1, "settings.card.directory",
         {"settings.card.directory.title", "settings.card.directory.hint"}},
        {1, "settings.card.proxy",
         {"settings.card.proxy.title", "settings.card.proxy.desc",
          "settings.card.proxy.port.label", "settings.card.proxy.port.hint",
          "settings.card.proxy.takeover.label",
          "settings.card.proxy.takeover.value"}},
        {2, "settings.card.log",
         {"settings.card.log.title", "settings.card.log.label",
          "settings.card.log.hint", "settings.card.log.ddwrap",
          "settings.card.log.level.label", "settings.card.log.level.hint",
          "settings.card.log.level.ddwrap"}},
        {2, "settings.tab.log", {"settings.tab.log.title"}},
    };

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
            float x0, y0, x1, y1;
        };
        std::vector<Rect> rowRects;
        std::vector<Rect> cardRects;
        const auto nearEq = [](float a, float b) {
            return std::fabs(a - b) < 0.01f;
        };
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
                if (row->frame.y < card->frame.y - 0.5f ||
                    row->frame.y + row->frame.height > cardBottom + 0.5f) {
                    return std::string("行越卡: ") + rid + " 行底 " +
                           std::to_string(row->frame.y + row->frame.height) +
                           " 卡底 " + std::to_string(cardBottom);
                }
                rowRects.push_back({rid, row->frame.x, row->frame.y,
                                    row->frame.x + row->frame.width,
                                    row->frame.y + row->frame.height});
            }
        }
        for (size_t i = 0; i < rowRects.size(); ++i) {
            for (size_t j = i + 1; j < rowRects.size(); ++j) {
                const Rect& p = rowRects[i];
                const Rect& q = rowRects[j];
                if (p.x0 < q.x1 - 0.01f && q.x0 < p.x1 - 0.01f &&
                    p.y0 < q.y1 - 0.01f && q.y0 < p.y1 - 0.01f) {
                    return std::string("行盒叠印: ") + p.id + " × " + q.id;
                }
            }
        }
        for (size_t i = 0; i < cardRects.size(); ++i) {
            for (size_t j = i + 1; j < cardRects.size(); ++j) {
                const Rect& p = cardRects[i];
                const Rect& q = cardRects[j];
                if (p.x0 < q.x1 - 0.01f && q.x0 < p.x1 - 0.01f &&
                    p.y0 < q.y1 - 0.01f && q.y0 < p.y1 - 0.01f) {
                    return std::string("卡叠印: ") + p.id + " × " + q.id;
                }
            }
        }
    // 派生钉值（字面期望）。f(id) = frame 高，yOff = 两行 frame.y 差
        const auto heightOf = [&](const char* id) -> float {
            const auto* e = ui.find(id);
            return e != nullptr ? e->frame.height : -1.0f;
        };
        const auto yOf = [&](const char* id) -> float {
            const auto* e = ui.find(id);
            return e != nullptr ? e->frame.y : -1.0f;
        };
        const auto pin = [&](const char* what, float got, float want) {
            if (!nearEq(got, want)) {
                return std::string(what) + " = " + std::to_string(got) +
                       "（期望 " + std::to_string(want) + "）";
            }
            return std::string();
        };
        std::string err;
    // 三档期望值按生产公式 roundFs(基准) = round(基准 × scale) 手算
        const auto at = [&](float a85, float a100, float a130) {
            return scale < 0.9f ? a85 : (scale > 1.1f ? a130 : a100);
        };
        if (tab == 0) {
            err = pin("appearance.title 高", heightOf("settings.card.appearance.title"),
                      at(20.0f, 24.0f, 31.0f));
            if (err.empty())
                err = pin("appearance.desc 高",
                          heightOf("settings.card.appearance.desc"),
                          at(15.0f, 18.0f, 23.0f));
            if (err.empty())
                err = pin("appearance.theme.label 高",
                          heightOf("settings.card.appearance.theme.label"),
                          at(14.0f, 16.0f, 21.0f));
            if (err.empty())
                err = pin("editor.hint 高", heightOf("settings.card.editor.hint"),
                          at(15.0f, 18.0f, 23.0f));
            if (err.empty())
                err = pin("editor.ddwrap 行顶（editorFontDdOffsetY=38字面锚+roundFs(4)）",
                          yOf("settings.card.editor.ddwrap") -
                              yOf("settings.card.editor"),
                          at(41.0f, 42.0f, 43.0f));
            if (err.empty())
                err = pin("editor.ddwrap 高（logFieldHeight=roundFs(36)）",
                          heightOf("settings.card.editor.ddwrap"),
                          at(31.0f, 36.0f, 47.0f));
            if (err.empty())
                err = pin("appearance 卡高", heightOf("settings.card.appearance"),
                          at(211.0f, 248.0f, 322.0f));
        } else if (tab == 1) {
            err = pin("modules.name.0 高",
                      heightOf("settings.card.modules.name.0"),
                      at(15.0f, 18.0f, 23.0f));
            if (err.empty())
                err = pin("modules 行距",
                          yOf("settings.card.modules.name.1") -
                              yOf("settings.card.modules.name.0"),
                          at(48.0f, 56.0f, 73.0f));
            if (err.empty())
                err = pin("modules 行内偏移",
                          yOf("settings.card.modules.rowdesc.0") -
                              yOf("settings.card.modules.name.0"),
                          at(17.0f, 20.0f, 26.0f));
            if (err.empty())
                err = pin("modules 卡高", heightOf("settings.card.modules"),
                          at(292.0f, 344.0f, 447.0f));
            if (err.empty())
                err = pin("proxy.takeover.label 高",
                          heightOf("settings.card.proxy.takeover.label"),
                          at(15.0f, 18.0f, 23.0f));
            if (err.empty())
                err = pin("proxy 卡高（无警告）",
                          heightOf("settings.card.proxy"),
                          at(127.0f, 132.0f, 142.0f));
        } else {
    // 日志卡入派生池：85% 档字面字段行底沿 96+36=132 > 卡高 roundFs(148)=126 溢出——
    // 标签盒 / 字段高 / 行 y 随生效字号派生
            err = pin("log.label 高（logLabelHeight=roundFs(20)）",
                      heightOf("settings.card.log.label"),
                      at(17.0f, 20.0f, 26.0f));
            if (err.empty())
                err = pin("log.ddwrap 行顶（logDropdownOffsetY）",
                          yOf("settings.card.log.ddwrap") -
                              yOf("settings.card.log"),
                          at(41.0f, 48.0f, 62.5f));
            if (err.empty())
                err = pin("log.ddwrap 高（logFieldHeight=roundFs(36)）",
                          heightOf("settings.card.log.ddwrap"),
                          at(31.0f, 36.0f, 47.0f));
            if (err.empty())
                err = pin("log.level.ddwrap 行顶（logLevelOffsetY）",
                          yOf("settings.card.log.level.ddwrap") -
                              yOf("settings.card.log"),
                          at(81.0f, 96.0f, 124.5f));
            if (err.empty())
                err = pin("log 卡高", heightOf("settings.card.log"),
                          at(126.0f, 148.0f, 192.0f));
        }
        return err;
    };

    // 断言宏展开含 return，只能留在用例体——helper 只收集问题串，逐 tab 一条
    const auto runAllTabs = [&](float scale) -> std::vector<std::string> {
        std::vector<std::string> problems;
        for (int tab : {0, 1, 2}) {
            if (!clickSettingsTab(*mod, screen, tab)) {
                problems.push_back("tab 切换失败: " + std::to_string(tab));
                continue;
            }
            eui::Ui ui;
            ui.begin(scale < 0.9f ? "fsderive85"
                                  : (scale > 1.1f ? "fsderive130"
                                                  : "fsderive100"));
            mod->compose(ui, screen);
            ui.end();
            ui.layout(screen.width, screen.height);
            problems.push_back(tabProblem(ui, tab, scale));
        }
        return problems;
    };

    // 100% 档（零回归锚）
    {
        FontScaleGuard g{1.0f};
        for (const std::string& problem : runAllTabs(1.0f)) {
            DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
        }
    }
    // 130% 档：行盒随字号派生 → 两两不重叠
    {
        FontScaleGuard g{1.3f};
        for (const std::string& problem : runAllTabs(1.3f)) {
            DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
        }
    }
    // 85% 档（kFontPresets[0]，溢出档）：字面几何在本档溢出卡外（日志卡字段行 132 > 126）
    {
        FontScaleGuard g{0.85f};
        for (const std::string& problem : runAllTabs(0.85f)) {
            DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
        }
    }

    mod->onStop();
    return true;
})

// 文案宽度纯件（③ 判据的来源）
REGISTER_TEST(settings_label_fit_pure,
              "settings: 文案宽度估算/单行适配/皮肤下拉宽度（不溢出判据）", {
    namespace ms = module::settings;
    // 估算口径钉死：ASCII 0.62em / 全角 1.0em（UTF-8 多字节按字符计一次）
    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("abc", 10.0f) - 18.6f) < 1e-3f);
    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("永久保留", 12.0f) - 48.0f) <
                  1e-3f);
    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("ab中文", 10.0f) -
                           (2.0f * 0.62f + 2.0f) * 10.0f) < 1e-3f);
    DOCTEST_CHECK(std::abs(ms::estimatedTextWidth("", 16.0f)) < 1e-3f);

    // 放得下 → 原文；放不下 → 截断 + U+2026，且含省略号后仍不超宽
    const std::string kLong = "super-long-skin-name-here";
    DOCTEST_CHECK(ms::fitControlLabel("robot", 178.0f, 16.0f) == "robot");
    const std::string fit = ms::fitControlLabel(kLong, 178.0f, 16.0f);
    DOCTEST_CHECK(fit != kLong);
    DOCTEST_CHECK(fit.size() >= 4u);  // ≥1 字符 + '…'(3 字节)
    DOCTEST_CHECK(fit.compare(fit.size() - 3u, 3u, "\xE2\x80\xA6") == 0);
    DOCTEST_CHECK_MESSAGE(ms::estimatedTextWidth(fit, 16.0f) <= 178.0f + 1e-3f,
                          "截断后宽 " +
                              std::to_string(ms::estimatedTextWidth(fit, 16.0f)));
    DOCTEST_CHECK(kLong.compare(0, fit.size() - 3u,
                                fit.substr(0, fit.size() - 3u)) == 0);
    return true;
})

REGISTER_TEST(settings_appearance_dropdown_popup_above_following_cards,
              "settings: appearance 卡下拉弹层盖过后续兄弟卡（不被编辑器字号卡截断）", {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("apddesc");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* popup =
        ui.find("settings.card.appearance.theme.dd.popup");
    const auto* appearanceCard = ui.find("settings.card.appearance");
    const auto* editorCard = ui.find("settings.card.editor");
    DOCTEST_REQUIRE(popup != nullptr);
    DOCTEST_REQUIRE(appearanceCard != nullptr);
    DOCTEST_REQUIRE(editorCard != nullptr);

    DOCTEST_CHECK(popup->frame.y + popup->frame.height >
                  appearanceCard->frame.y + appearanceCard->frame.height);

    // 绘制序 = orderedChildren 顺序（后者在上）。弹层深嵌外观卡子树、与字号卡没有
    // 共同直接父——按「整卡抬层」语义（subtreeMaxZIndex 让含弹层的卡片整卡作为一个
    // 绘制单元与兄弟卡竞争），断言两卡在滚动内容列里的兄弟序：外观卡必须排在字号卡之后
    const core::dsl::Element* parent = nullptr;
    for (const auto& root : ui.roots()) {
        parent = commonParentOf(root.get(), appearanceCard, editorCard);
        if (parent != nullptr) break;
    }
    DOCTEST_REQUIRE(parent != nullptr);
    DOCTEST_CHECK(orderedIndexOf(parent, appearanceCard) >
                  orderedIndexOf(parent, editorCard));

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 按钮在「位置重置」左侧同排（间距 8）、文案 / 点击回调齐备。
// 卡高保持 120（内容底沿 104 不变 → contentKey 不变，不触发滚动高度重测）
REGISTER_TEST(settings_port_esc_discards_draft,
              "settings: 端口草稿 Esc=丢弃回落已应用端口，不触发应用（headless）", {
    test_support::TestEnv env(kCfgPrefix, "portesc", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "portesc_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // 端口输入在代理卡（「模块」tab）
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

    // Ctrl+A 全选后输入 → 替换成合法草稿 9999（不替换会追加成非法值，apply 校验
    // 失败即早退，失去「Esc 是否触发 apply」的判别力）
    auto* input = ui.find("settings.card.proxy.port.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onTextInput != nullptr);
    eui::KeyEvent selectAll;
    selectAll.key = eui::InputKey::A;
    selectAll.action = eui::KeyAction::Press;
    test_support::setShortcutModifier(selectAll.modifiers);
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

    // 未落盘：配置仍是默认 8888
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().proxyPort, 8888);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 设置页 Ctrl+S 保存快捷键（headless）
// 设置页除端口草稿外全部即时落盘，Ctrl+S 语义 = 提交待决草稿（=「应用」按钮全链路，
// 含 ClaudeService 端口同步），无待决时显式重写落盘 + 状态行回显。
// 服务注册 + 代理启动用于验证代理运行中的 Ctrl+S 分支。
REGISTER_TEST(settings_ctrl_s_save_shortcut,
              "settings: Ctrl+S 提交端口草稿 / 显式落盘", {
    test_support::TestEnv env(kCfgPrefix, "ctrls", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "ctrls_cfg"));
    module::claude::ClaudeService svc(env.db->db(), "DevDeskTestCtrlS");
    module::claude::ClaudeService::setGlobal(&svc);
    ScopedClaudeProxy proxyForSettings;  // 设置页经定位器取代理运行面
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
        // 端口输入 / 重新生成按钮在「模块」tab
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
        // Ctrl+S（修饰位按平台取，见 setShortcutModifier）
    auto ctrlS = [](eui::KeyAction action) {
        eui::KeyEvent e;
        e.key = eui::InputKey::S;
        e.action = action;
        test_support::setShortcutModifier(e.modifiers);
        return e;
    };

    // 1) 无草稿：Ctrl+S → 配置落库（onStart 只读不写，此处才出现行）
    //    + 状态行回显「设置已保存」
        // 「落盘了没有」看 app_config 的那一行在不在
    DOCTEST_CHECK(test_support::readRawConfigRow(
                      ::core::appDatabase(), "config.json:claudeProxyPort").empty());
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));
    DOCTEST_CHECK(!test_support::readRawConfigRow(
                      ::core::appDatabase(), "config.json:claudeProxyPort").empty());
    frame("css1");
    {
        const auto* sub = ui.find("settings.subtitle");
        DOCTEST_REQUIRE(sub != nullptr);
        DOCTEST_CHECK(sub->text.find("设置已保存") != std::string::npos);
    }
    {
        module::settings::SettingsStore store(::core::appDatabase());
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
    // 3) 端口草稿 ≠ 已应用端口 → Ctrl+S 提交草稿（ClaudeService::configuredProxyPort
    //    同步；代理未运行 → 状态文案见 settings_module.cpp 的 applyProxyPort）
    frame("css2");
    {
        auto* input = ui.find("settings.card.proxy.port.input.hit");
        DOCTEST_REQUIRE(input != nullptr);
        DOCTEST_REQUIRE(input->onKeyEvent != nullptr);
        DOCTEST_REQUIRE(input->onTextInput != nullptr);
        eui::KeyEvent selectAll;
        selectAll.key = eui::InputKey::A;
        selectAll.action = eui::KeyAction::Press;
        test_support::setShortcutModifier(selectAll.modifiers);
        input->onKeyEvent(selectAll);           // Ctrl+A 全选
        eui::TextInputEvent t;
        t.text = "9001";
        input->onTextInput(t);                  // 替换 → draft = "9001"
    }
    frame("css3");
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));
    {
        module::settings::SettingsStore store(::core::appDatabase());
        DOCTEST_CHECK_EQ(store.load().proxyPort, 9001);
    }
    DOCTEST_CHECK_EQ(svc.configuredProxyPort(), 9001);
    frame("css3b");   // 重新 compose，让状态行进树
    {
        const auto* sub = ui.find("settings.subtitle");
        DOCTEST_REQUIRE(sub != nullptr);
        DOCTEST_CHECK(sub->text.find("代理端口已保存") != std::string::npos);
    }
    // 4) 代理运行中再按 Ctrl+S：draft 已应用，走显式落盘分支
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

// 跟随系统主题（themeMode 三档 + platform::prefersDarkTheme）

REGISTER_TEST(settings_theme_mode_roundtrip_and_legacy,
              "settings: themeMode 默认 auto/读写往返/非法值回落（spec缩水 A）", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg12", true);
    using namespace module::settings;
    const auto path = test_support::tempConfigFile(kCfgPrefix, "thememode");
    removeQuietly(path);
    SettingsStore store(::core::appDatabase());

    // 无配置 → 默认 auto
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeAuto));

    // 三档读写往返；theme 键随 darkTheme 同步写（main.cpp 启动通道）
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

    // themeMode 键缺失 → auto（只认 themeMode 键，不从 theme 键派生）
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "light"})"));
    }
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeAuto));

        // themeMode 非法值 → auto
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"themeMode": "neon", "theme": "light"})"));
    }
    DOCTEST_CHECK_EQ(store.load().themeMode, std::string(kThemeModeAuto));
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"themeMode": "neon"})"));
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
    test_support::TestEnv env(kCfgPrefix, "thauto", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "thauto_cfg"));
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"themeMode": "auto", "theme": "dark"})"));
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 落盘的 theme=dark 是陈旧值；auto 解析后实际档必须等于本机系统检测结果（机器无关）
    const bool systemDark = platform::prefersDarkTheme();
    DOCTEST_CHECK_EQ(::core::theme::current().isDark, systemDark);
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_theme_mode_dropdown_click_persists,
              "settings: 主题模式下拉存在且点「深色」落盘（替代旧双按钮，spec缩水 A headless）", {
    test_support::TestEnv env(kCfgPrefix, "thdmm", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "thdmm_cfg"));
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
        // 点第 3 项（深色，索引 2）
    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.dark") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.theme.light") == nullptr);

        // → 即时持久化 themeMode=dark + theme 同步
    const auto* itemDark = ui.find("settings.card.appearance.theme.dd.item.2");
    DOCTEST_REQUIRE(itemDark != nullptr);
    DOCTEST_REQUIRE(itemDark->onClick != nullptr);
    itemDark->onClick();
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 编辑器字号可配（SettingsConfig + settings UI + 通道写入）

REGISTER_TEST(settings_editor_font_size_roundtrip_clamp,
              "settings: editorFontSize 默认 15/读写往返/越界钳制（spec缩水 B）", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg13", true);
    using namespace module::settings;
    const auto path = test_support::tempConfigFile(kCfgPrefix, "edfont");
    removeQuietly(path);
    SettingsStore store(::core::appDatabase());

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
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"theme": "dark", "logRetentionDays": 30})"));
    }
    DOCTEST_CHECK_EQ(store.load().editorFontSize, 15);
    removeQuietly(path);
    return true;
})

REGISTER_TEST(settings_editor_font_dropdown_applies_channel,
              "settings: 编辑器字号下拉点档 → core::theme 通道 + 落盘（spec缩水 B headless）", {
    test_support::TestEnv env(kCfgPrefix, "edfontui", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "edfontui_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("edfontui");
    mod->compose(ui, screen);
    ui.end();

    // 编辑器字号自日志卡迁到「通用」tab 独立卡（默认 tab，无需切）
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    // 点第 3 档「大 17px」→ 通道即时生效 + 持久化
    const auto* item = ui.find("settings.card.editor.dd.item.2");
    DOCTEST_REQUIRE(item != nullptr);
    DOCTEST_REQUIRE(item->onClick != nullptr);
    item->onClick();
    DOCTEST_CHECK_EQ(::core::theme::editorFontSize(), 17.0f);
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().editorFontSize, 17);

    mod->onStop();
    ::core::theme::setEditorFontSize(15.0f);  // 恢复通道默认
    ::core::theme::setDark();  // onStart 的 auto 解析可能 setLight，防串扰
    return true;
})

// 关于文案纯函数（版本 / EUI-NEO rev / 构建信息，CMake 注入宏）
// 设置页关于卡已拆成独立模块 module_about，settings_config.cpp 的 about* 函数为死代码。
// 纯函数测试保留，防宏注入回归。

// 版本断言与真值同源的前提：本 TU 必须拿到与 devdesk_module_settings **同一**
// PROJECT_VERSION 注入的 DEVDESK_VERSION 宏（tests/CMakeLists.txt 的
// target_compile_definitions）。缺了就是编译期硬失败，不让断言退化成假绿。
#ifndef DEVDESK_VERSION
#error "devdesk_tests 缺 DEVDESK_VERSION 宏注入（见 tests/CMakeLists.txt）"
#endif

REGISTER_TEST(settings_about_text_helpers,
              "settings: aboutVersionLine/aboutEuiNeoLine 纯函数（spec缩水 C）", {
    // 期望版本段取自 DEVDESK_VERSION 宏——写死版本号会在真值涨版后恒红，
    // 且与 settings_config.cpp 构造 aboutVersionLine() 的宏不同源。
    const std::string expectPrefix = std::string("DevDesk ") + DEVDESK_VERSION;
    const std::string v = module::settings::aboutVersionLine();
    DOCTEST_CHECK_MESSAGE(v.rfind(expectPrefix, 0) == 0,
                          "actual=[" + v + "] expected_prefix=[" + expectPrefix + "]");
    // 格式不变式（不依赖具体版本值）：
    // "DevDesk <版本> · <构建类型> · 构建 <日期> <时刻>"
    DOCTEST_CHECK(v.rfind(expectPrefix + " · ", 0) == 0);
    DOCTEST_CHECK(v.find(" · 构建 ") != std::string::npos);
        // 前缀比对已直接证明注入生效
    const std::string e = module::settings::aboutEuiNeoLine();
    DOCTEST_CHECK(e.find("EUI-NEO rev") != std::string::npos);
    // 最低依赖 rev 恒打印（顶层 CMake 变量）
    DOCTEST_CHECK(e.find("最低依赖 rev") != std::string::npos);
    return true;
})

// 日志级别可配（SettingsConfig + core::log::setLevel 应用）

REGISTER_TEST(settings_log_level_roundtrip_matrix,
              "settings: logLevel 默认 INFO/读写往返/未知收敛/parseLogLevel 矩阵（spec缩水 D）", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg14", true);
    using namespace module::settings;
    const auto path = test_support::tempConfigFile(kCfgPrefix, "loglevel");
    removeQuietly(path);
    SettingsStore store(::core::appDatabase());

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
    test_support::TestEnv env(kCfgPrefix, "loglvl", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "loglvl_cfg"));
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"logLevel": "WARN"})"));
    }
    const core::log::Level prev = core::log::level();
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    // 持久化 WARN 档在 onStart 即应用
    DOCTEST_CHECK(core::log::level() == core::log::Level::Warn);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    // 日志级别下拉在「日志」tab
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
    module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK_EQ(store.load().logLevel,
                     std::string(module::settings::kLogLevelDebug));

    mod->onStop();
    core::log::setLevel(prev);  // 恢复（测试进程默认 Info）
    ::core::theme::setDark();   // onStart 的 auto 解析可能 setLight，防串扰
    return true;
})

// 日志级别「切档 → 立即生效 → 重启后仍生效」三段可观察
// 机制层（setLevel + persist / onStart 恢复 / 四档 ↔ core::log::Level 映射）由上一条
// 用例钉着。缺口在可见面：级别下拉与「运行日志」卡同屏，而 tail 快照只在「切 tab /
// 点刷新」时置脏重读，切档后同屏日志内容一动不动。本用例把三段钉死：
//   (a) 切档后后续 LOG_DEBUG **真进日志文件**（过滤对写入生效，非仅内存字段）；
//   (b) 不切 tab、不点刷新，「运行日志」卡自己重读到新行；
//   (c) 新实例 onStart 恢复持久化档。
REGISTER_TEST(settings_log_level_effective_visible_and_persistent,
              "settings: 日志级别切档→文件真过滤 + 运行日志卡自动重读 + 重启恢复（batch-C C-5）", {
    test_support::TestEnv env(kCfgPrefix, "lvlvis", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "lvlvis_cfg"));
    test_support::AppDataEnv appdata("lvlvis");   // 日志文件隔离到临时 HOME
    {
        // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), R"({"logLevel": "INFO"})"));
    }
    writeLogFile("C5-SEED\n");   // 冷启动即有一次装载（tailLoaded_ 走 onStart 路径）

    const core::log::Level prev = core::log::level();
    core::log::addFileOutput(::core::paths::logFile());   // 与生产 main 同款：落文件
    core::log::setLevel(core::log::Level::Info);

    auto readLogFile = []() -> std::string {
        std::ifstream f(::core::paths::logFile(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 前置：INFO 档下 DEBUG 行被过滤（写进文件的是空）
    LOG_INFO("C5-INFO-BEFORE");
    LOG_DEBUG("C5-DEBUG-BEFORE");
    {
        const std::string log = readLogFile();
        DOCTEST_CHECK(log.find("C5-INFO-BEFORE") != std::string::npos);
        DOCTEST_CHECK(log.find("C5-DEBUG-BEFORE") == std::string::npos);
    }

    // 进日志 tab（级别下拉 + 运行日志卡同屏）
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
    // 日志查看卡渲染出的行（"" 结束；同 settings_h2_log_tail_snapshot 口径）
    auto tailLines = [&]() -> std::vector<std::string> {
        eui::Ui ui;
        ui.begin("settings_c5_probe");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
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
    DOCTEST_CHECK(hasLine(tailLines(), "C5-SEED"));   // 基准快照已装载

    // 切 DEBUG（点下拉第 0 项）
    {
        eui::Ui ui;
        ui.begin("settings_c5_click");
        mod->compose(ui, screen);
        ui.end();
        const auto* item = ui.find("settings.card.log.level.dd.item.0");
        DOCTEST_REQUIRE(item != nullptr);
        DOCTEST_REQUIRE(item->onClick != nullptr);
        item->onClick();
    }
    DOCTEST_CHECK(core::log::level() == core::log::Level::Debug);   // 立即生效

    // (a) 过滤对「写入」生效：切档后的 DEBUG 行真落进日志文件
    LOG_DEBUG("C5-DEBUG-AFTER");
    {
        const std::string log = readLogFile();
        DOCTEST_CHECK(log.find("C5-DEBUG-AFTER") != std::string::npos);
        DOCTEST_CHECK(log.find("C5-DEBUG-BEFORE") == std::string::npos);
    }

    // (b) 可见面：不切 tab、不点刷新 → 运行日志卡自己重读到新行
    //（tail 读盘异步：置脏后隔帧收割，有界轮询等可见）
    bool visible = false;
    for (int i = 0; i < 40 && !visible; ++i) {
        visible = hasLine(tailLines(), "C5-DEBUG-AFTER");
        if (!visible) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(visible);

    // 切档后的可见回显两处：① 下拉选中态（field.label 换成新档文案）；
    // ② 页头状态行「日志级别：DEBUG（即时生效）」
    {
        eui::Ui ui;
        ui.begin("settings_c5_selected");
        mod->compose(ui, screen);
        ui.end();
        const auto* label = ui.find("settings.card.log.level.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("DEBUG（详尽）"));
        const auto* sub = ui.find("settings.subtitle");
        DOCTEST_REQUIRE(sub != nullptr);
        DOCTEST_CHECK_EQ(sub->text, std::string("日志级别：DEBUG（即时生效）"));
    }

    // 配置落盘 + (c) 重启（新实例 onStart）恢复持久化档
    {
        module::settings::SettingsStore store(::core::appDatabase());
        DOCTEST_CHECK_EQ(store.load().logLevel,
                         std::string(module::settings::kLogLevelDebug));
    }
    mod->onStop();
    std::unique_ptr<::core::Module> mod2(create_settings_module());
    mod2->onStart();
    DOCTEST_CHECK(core::log::level() == core::log::Level::Debug);
    LOG_DEBUG("C5-DEBUG-RESTART");
    DOCTEST_CHECK(readLogFile().find("C5-DEBUG-RESTART") != std::string::npos);
    mod2->onStop();

    core::log::setLevel(prev);  // 恢复（测试进程默认 Info）
    ::core::theme::setDark();   // onStart 的 auto 解析可能 setLight，防串扰
    return true;
})

// 日志 tail 读盘在后台线程，compose 只读快照
// compose 顶部同步读 256KB 日志文件会与代理 worker 的 DB 写抢同一把锁。切 tab /
// 点刷新只置脏标记 → 发一次读意图（std::async 后台线程），结果下一帧收割；
// 快照恒非空沿用上次值（不闪空、不丢行）。
REGISTER_TEST(settings_h2_log_tail_snapshot,
              "settings: 日志 tail 后台重读快照（H2 tick 前旧值/后新值）", {
    test_support::TestEnv env(kCfgPrefix, "h2tail", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "h2tail_cfg"));
    test_support::AppDataEnv appdata("h2tail");   // 日志文件隔离到临时 APPDATA
    writeLogFile("H2-TAIL-ONE\n");

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
    appendLogFile("H2-TAIL-TWO\n");

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
// 外部状态只读快照（compose 不做文件系统查询）
// 接管状态（sidecar stat）、笔记目录存在性（fs::exists）、桌宠皮肤清单
// （discoverSkins 遍历 + 每皮肤 pet.json stat）读一次存成员，compose 只读快照，
// 刷新点 = onStart / onPageEnter / 切模块 tab / 打开目录后。本用例验：
// ① 刷新前快照非空且等于上次值（不闪空、不闪变）；② 刷新入口后取新值；
// ③ 目录消失 / 出现的边界下不崩、不闪空。
REGISTER_TEST(settings_ths01_fs_snapshot,
              "settings: 外部状态快照——刷新前旧值 / 刷新后新值（THS-01）", {
    test_support::TestEnv env(kCfgPrefix, "ths01", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "ths01_cfg"));
    // 模块插拔门控：目录卡要渲染
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdSettings};
    DOCTEST_REQUIRE(module::settings::SettingsStore(::core::appDatabase()).save(cfg).isOk());

    const auto notesDir = test_temp::tempPath("devdesk_ths01_notes");
    std::error_code ec;
    std::filesystem::remove_all(notesDir, ec);
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", notesDir.string());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();   // 首次装载在启动期（非渲染路径）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));   // 模块 tab

    // 探针：跑一帧 compose，返回目录卡路径文本
    auto probe = [&]() {
        eui::Ui ui;
        ui.begin("settings_ths01_probe");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);  // scrollView 内容在 layout 期展开
        const auto* path = ui.find("settings.card.directory.path.value");
        return path ? path->text : std::string();
    };
    auto notCreated = [](const std::string& t) {
        return t.find("（尚未创建）") != std::string::npos;
    };

    // ① onStart 快照：笔记目录不存在（标「尚未创建」）
    const auto initial = probe();
    DOCTEST_CHECK(!initial.empty());        // 非空（不闪空）
    DOCTEST_CHECK(notCreated(initial));

    // ② 盘上出现目录，但未到刷新点 → 快照沿用上次值（不闪变）
    std::filesystem::create_directories(notesDir, ec);
    DOCTEST_REQUIRE(!ec);
    const auto stale = probe();
    DOCTEST_CHECK(notCreated(stale));                  // = 上次值

    // ③ 刷新入口（进页面）→ 快照更新为新值，文本仍非空
    mod->onPageEnter();
    const auto refreshed = probe();
    DOCTEST_CHECK(!refreshed.empty());
    DOCTEST_CHECK_FALSE(notCreated(refreshed));        // 新值

    // ④ 边界：目录消失 → 刷新前仍显示上次值（不闪空），刷新后回到未创建态
    std::filesystem::remove_all(notesDir, ec);
    DOCTEST_CHECK_FALSE(notCreated(probe()));          // 上次值仍在
    mod->onPageEnter();
    const auto gone = probe();
    DOCTEST_CHECK(!gone.empty());
    DOCTEST_CHECK(notCreated(gone));

    mod->onStop();
    std::filesystem::remove_all(notesDir, ec);
    ::core::theme::setDark();
    return true;
})

// 下拉「谁打开着」的唯一持有者（5 个 bool → 1 个索引）
// ① 打开第二个下拉会自动关掉先前那个（同时最多一个展开）；
// ② Esc 关当前打开的下拉，全关时 Esc 不消费（不越权吃键）。

REGISTER_TEST(settings_dropdown_single_holder,
              "settings: 下拉唯一持有者——开第二个自动关第一个 + Esc 关当前项（审计 THS-05）", {
    test_support::TestEnv env(kCfgPrefix, "ddholder", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "ddholder_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    // 两个下拉同在「通用」tab 的外观卡（默认 tab，无需切换）
    const std::string themeDd = "settings.card.appearance.theme.dd";
    const std::string fontDd = "settings.card.appearance.font.dd";
    // 展开态 = 弹层 opacity（EUI-NEO dropdown 收起时 opacity 0，元素仍在树上）
    auto popupOpacity = [&](const std::string& dd) -> float {
        const auto* p = ui.find(dd + ".popup");
        return p != nullptr ? p->opacity : -1.0f;
    };
    auto clickField = [&](const std::string& dd) {
        auto* f = ui.find(dd + ".field");
        if (f == nullptr || f->onClick == nullptr) return false;
        f->onClick();
        return true;
    };
    auto escPress = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::Escape;
        e.action = eui::KeyAction::Press;
        return e;
    };

    frame("dd0");
    DOCTEST_CHECK_EQ(popupOpacity(themeDd), 0.0f);
    DOCTEST_CHECK_EQ(popupOpacity(fontDd), 0.0f);
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    // 1) 开主题下拉 → 只有它展开
    DOCTEST_REQUIRE(clickField(themeDd));
    frame("dd1");
    DOCTEST_CHECK_EQ(popupOpacity(themeDd), 1.0f);
    DOCTEST_CHECK_EQ(popupOpacity(fontDd), 0.0f);

    // 2) 再开字号下拉 → 主题自动关（唯一持有者：同时最多一个展开）
    DOCTEST_REQUIRE(clickField(fontDd));
    frame("dd2");
    DOCTEST_CHECK_EQ(popupOpacity(fontDd), 1.0f);
    DOCTEST_CHECK_EQ(popupOpacity(themeDd), 0.0f);

    // 3) Esc 关当前项；再按一次（已全关）不消费
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame("dd3");
    DOCTEST_CHECK_EQ(popupOpacity(fontDd), 0.0f);
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// 代理端口 / 解耦护栏用例

REGISTER_TEST(settings_no_direct_claude_service_coupling,
              "settings: R0/D6 —— 设置页零 ClaudeService 直连（改经 core 只读服务定位器）", {
                  const std::string report = r0CouplingGuardReport();
                  DOCTEST_CHECK_MESSAGE(report.empty(), report);
                  return true;
              })

REGISTER_TEST(settings_proxy_port_single_source,
              "settings: R0/D7 —— 代理端口常量单一来源 core/proxy_port.h", {
    // 1) 取值钉死（两侧仍须 8888 / 1 / 65535）
                  DOCTEST_CHECK_EQ(::core::kDefaultProxyPort, 8888);
                  DOCTEST_CHECK_EQ(::core::kMinProxyPort, 1);
                  DOCTEST_CHECK_EQ(::core::kMaxProxyPort, 65535);
    // 2) 两个消费方都跟随 core（改值只有一个落点）
                  DOCTEST_CHECK_EQ(module::settings::kDefaultProxyPort,
                                   ::core::kDefaultProxyPort);
                  DOCTEST_CHECK_EQ(module::settings::kMinProxyPort,
                                   ::core::kMinProxyPort);
                  DOCTEST_CHECK_EQ(module::settings::kMaxProxyPort,
                                   ::core::kMaxProxyPort);
                  DOCTEST_CHECK_EQ(module::claude::ClaudeService::kDefaultProxyPort,
                                   ::core::kDefaultProxyPort);
                  DOCTEST_CHECK_EQ(module::claude::ClaudeService::kMinProxyPort,
                                   ::core::kMinProxyPort);
                  DOCTEST_CHECK_EQ(module::claude::ClaudeService::kMaxProxyPort,
                                   ::core::kMaxProxyPort);
    // 3) 源码级：字面量只许在 core/proxy_port.h 定义
                  const std::string report = proxyPortSingleSourceReport();
                  DOCTEST_CHECK_MESSAGE(report.empty(), report);
                  return true;
              })

REGISTER_TEST(settings_service_locator_contract,
              "settings: R0/D6 —— core 只读服务定位器契约（未注册 fail / 注册可解析 / RAII 还原）", {
                  using ::core::IClaudeProxyService;
                  using ::core::ServiceLocator;

                  // 未注册 → fail（不返回裸空指针）；注册 → 可解析；RAII 结束 → 复原
                  DOCTEST_CHECK(
                      ServiceLocator::requireService<IClaudeProxyService>().isFail());
                  ::core::GlobalClaudeProxyService<module::claude::ClaudeService>
                      adapter;
                  {
                      ::core::ScopedService<IClaudeProxyService> registration(
                          &adapter);
                      auto resolved =
                          ServiceLocator::requireService<IClaudeProxyService>();
                      DOCTEST_REQUIRE(resolved.isOk());
                      DOCTEST_CHECK(resolved.value() == &adapter);
                  }
                  DOCTEST_CHECK(
                      ServiceLocator::requireService<IClaudeProxyService>().isFail());

    // 适配器「空单例」语义（未 setGlobal / claude 模块关闭）
                  DOCTEST_CHECK_FALSE(adapter.isProxyRunning());
                  DOCTEST_CHECK_EQ(adapter.proxyPort(), 0);
                  DOCTEST_CHECK_EQ(adapter.lastSyncWarnings().size(),
                                   static_cast<size_t>(0));
                  DOCTEST_CHECK_EQ(adapter.settingsFilePath(), std::string());
                  DOCTEST_CHECK_FALSE(adapter.isClaudeCodeManaged());
                  DOCTEST_CHECK(adapter.startProxy("127.0.0.1", 0).isFail());
                  adapter.setConfiguredProxyPort(9000);  // no-op，不崩
                  adapter.stopProxy();                   // no-op，不崩
                  return true;
              })

// settings_module 拆分 + 卡片表驱动
// 拆分要求零行为变化。以下两条用例是「拆出的纯件 / 注册表」与拆分前逐字等价的护栏：
// 纯件断言逐点复刻拆分前的内联式；注册表断言 tab 归属、绘制顺序与模块门控
//（拆分前是 compose 内 switch 的分支）。

REGISTER_TEST(settings_ui_logic_equivalence,
              "settings: R6 纯件等价——openUp 几何/档位表/最近邻/字号缩放/保留档/下拉持有者", {
    namespace ms = module::settings;
    // 1) openUp 几何：与拆分前三处内联式逐点比对（右侧字面量 = 拆分前取值）
    auto legacyOverflows = [](float cardTopY, float fieldOffsetY,
                              float fieldHeight, int itemCount,
                              float screenH) {
        const float fieldBottomY = cardTopY + fieldOffsetY + fieldHeight;
        const float popupH = static_cast<float>(itemCount) * 34.0f + 12.0f;
        return fieldBottomY + 8.0f + popupH > screenH - 16.0f;
    };
    int gridHits = 0;
    for (float top : {0.0f, 100.0f, 176.0f, 400.0f, 984.0f}) {
        for (float off : {ms::logDropdownOffsetY(), ms::editorFontDdOffsetY(),
                          ms::logLevelOffsetY()}) {
    // 三处下拉高单一来源 logFieldHeight()，fh 轴收敛为 1 档
            for (float fh : {ms::logFieldHeight()}) {
                for (int n : {ms::kRetentionPresetCount,
                              ms::kEditorFontPresetCount,
                              ms::kLogLevelPresetCount}) {
                    for (float sh : {484.0f, 500.0f, 1000.0f, 1300.0f}) {
                        DOCTEST_CHECK_EQ(
                            ms::dropdownPopupOverflows(top, off, fh, n, sh),
                            legacyOverflows(top, off, fh, n, sh));
                        ++gridHits;
                    }
                }
            }
        }
    }
    DOCTEST_CHECK_EQ(gridHits, 5 * 3 * 1 * 3 * 4);

    // 2) 弹层几何 / 卡片高度常量取值（拆分前后同源，值不得漂）
    DOCTEST_CHECK_EQ(ms::kDropdownItemHeight, 34.0f);
    DOCTEST_CHECK_EQ(ms::kDropdownPopupPadding, 12.0f);
    DOCTEST_CHECK_EQ(ms::kDropdownPopupGap, 8.0f);
    DOCTEST_CHECK_EQ(ms::kWindowBottomMargin, 16.0f);
    DOCTEST_CHECK_EQ(ms::kSettingsTabHeight, 40.0f);
    DOCTEST_CHECK_EQ(ms::kAppearanceCardHeight, 248.0f);
    DOCTEST_CHECK_EQ(ms::kEditorFontCardHeight, 108.0f);
    DOCTEST_CHECK_EQ(ms::kModulesCardHeight, 344.0f);  // 4 行开关（agent 加入）
    // 目录卡加「数据根忽略规则（.gitignore）」路径行（新行 y=126 高 34，盒底 160 + 16 下边距）
    DOCTEST_CHECK_EQ(ms::kDirectoryCardHeight, 176.0f);
    DOCTEST_CHECK_EQ(ms::kLogSettingsCardHeight, 148.0f);

    // 3) 档位表（值 + 标签，逐项钉死）
    DOCTEST_CHECK_EQ(ms::kFontPresetCount, 4);
    DOCTEST_CHECK_EQ(ms::kFontPresets[0], 85);
    DOCTEST_CHECK_EQ(ms::kFontPresets[3], 130);
    DOCTEST_CHECK(std::string(ms::kFontPresetLabels[2]) == "大 115%");
    DOCTEST_CHECK_EQ(ms::kRetentionPresetCount, 6);
    DOCTEST_CHECK_EQ(ms::kRetentionPresets[0], 0);
    DOCTEST_CHECK_EQ(ms::kRetentionPresets[5], 365);
    DOCTEST_CHECK_EQ(ms::kWindowEffectPresetCount, 3);
    DOCTEST_CHECK(std::string(ms::kWindowEffectPresets[2]) ==
                  ms::kWindowEffectFrosted);
    DOCTEST_CHECK(std::string(ms::kWindowEffectPresetLabels[0]) == "关闭（默认）");
    DOCTEST_CHECK_EQ(ms::kThemeModePresetCount, 3);
    DOCTEST_CHECK(std::string(ms::kThemeModePresets[0]) == ms::kThemeModeAuto);
    DOCTEST_CHECK(std::string(ms::kThemeModePresetLabels[2]) == "深色");
    DOCTEST_CHECK_EQ(ms::kEditorFontPresetCount, 4);
    DOCTEST_CHECK_EQ(ms::kEditorFontPresets[1], 15);   // 默认 = fontSizeBody 基准
    DOCTEST_CHECK_EQ(ms::kLogLevelPresetCount, 4);
    DOCTEST_CHECK(std::string(ms::kLogLevelPresets[0]) == ms::kLogLevelDebug);
    DOCTEST_CHECK(std::string(ms::kLogLevelPresetLabels[1]) == "INFO（默认）");

    // 4) nearestIndex：命中 / 最近 / 越界回落
    DOCTEST_CHECK_EQ(ms::nearestIndex(100, ms::kFontPresets, ms::kFontPresetCount), 1);
    DOCTEST_CHECK_EQ(ms::nearestIndex(86, ms::kFontPresets, ms::kFontPresetCount), 0);
    DOCTEST_CHECK_EQ(ms::nearestIndex(0, ms::kFontPresets, ms::kFontPresetCount), 0);
    DOCTEST_CHECK_EQ(ms::nearestIndex(999, ms::kFontPresets, ms::kFontPresetCount), 3);
    DOCTEST_CHECK_EQ(ms::nearestIndex(90, ms::kRetentionPresets, ms::kRetentionPresetCount), 3);  // 恰好命中 90
    DOCTEST_CHECK_EQ(ms::nearestIndex(91, ms::kRetentionPresets, ms::kRetentionPresetCount), 3);  // 90 更近（1 vs 89）
    DOCTEST_CHECK_EQ(ms::nearestIndex(20, ms::kRetentionPresets, ms::kRetentionPresetCount), 2);  // 30 更近（10 vs 13）
    DOCTEST_CHECK_EQ(ms::nearestIndex(15, ms::kEditorFontPresets, ms::kEditorFontPresetCount), 1);

    // 5) fs：非 token 字号跟随全局缩放（同一来源 core::theme::fontScale）
    const float savedScale = ::core::theme::fontScale();
    ::core::theme::setFontScale(1.0f);
    DOCTEST_CHECK_EQ(ms::fs(12.0f), 12.0f);
    ::core::theme::setFontScale(1.5f);
    DOCTEST_CHECK_EQ(ms::fs(12.0f), 18.0f);
    ::core::theme::setFontScale(savedScale);

    // 6) retentionLabel：0 = 永久保留
    DOCTEST_CHECK(ms::retentionLabel(0) == "永久保留");
    DOCTEST_CHECK(ms::retentionLabel(30) == "30 天");
    DOCTEST_CHECK(ms::retentionLabel(365) == "365 天");

    // 7) 下拉唯一持有者：开 = 独占；关 = 只关自己（迟到回调不误关）
    DOCTEST_CHECK(ms::isDropdownOpen(1, ms::Dropdown::Font));
    DOCTEST_CHECK_FALSE(ms::isDropdownOpen(-1, ms::Dropdown::Font));
    DOCTEST_CHECK_EQ(ms::applyDropdownOpen(-1, ms::Dropdown::Theme, true),
                     static_cast<int>(ms::Dropdown::Theme));
    DOCTEST_CHECK_EQ(ms::applyDropdownOpen(static_cast<int>(ms::Dropdown::Theme),
                                           ms::Dropdown::Font, true),
                     static_cast<int>(ms::Dropdown::Font));   // 开第二个关第一个
    DOCTEST_CHECK_EQ(ms::applyDropdownOpen(static_cast<int>(ms::Dropdown::Font),
                                           ms::Dropdown::Font, false), -1);
    DOCTEST_CHECK_EQ(ms::applyDropdownOpen(static_cast<int>(ms::Dropdown::Font),
                                           ms::Dropdown::Theme, false),
                     static_cast<int>(ms::Dropdown::Font));   // 迟到回调不误关
    return true;
})

REGISTER_TEST(settings_card_registry_order,
              "settings: R6 卡片注册表——tab 归属 / 绘制顺序 / 模块门控（headless）", {
    // 卡根 id 序列 = scrollView 内容列的直接子元素（创建序 = 绘制序）
    auto cardIdsOf = [](::core::Module& mod, const eui::Screen& screen) {
        eui::Ui ui;
        ui.begin("registry");
        mod.compose(ui, screen);
        ui.end();
        std::vector<std::string> ids;
        const auto* content = ui.find("settings.cards.scroll.content");
        if (content == nullptr) return ids;
        for (const auto& child : content->children) {
            const auto pos = child->id.find("settings.");
            ids.push_back(pos == std::string::npos ? child->id
                                                   : child->id.substr(pos));
        }
        return ids;
    };
    // 断言宏展开含 return false，只许写在用例体（返回 bool）里；helper 只回传值
    auto join = [](const std::vector<std::string>& ids) {
        std::string s;
        for (const auto& id : ids) s += id + " | ";
        return s;
    };

    eui::Screen screen{1600.0f, 1000.0f};

    // ① 全模块开启：三 tab 各自的卡序列（含受门控的卡）
    {
        test_support::TestEnv env(kCfgPrefix, "registryall", true);
        test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "registryall_cfg"));
        {
                // 按顶层键写进 app_config 表各行
            DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
                ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\":\"dark\",\"enabledModules\":[\"home\"," "\"markdown\",\"todo\",\"claude_proxy\",\"settings\"]}"));
        }
        std::unique_ptr<::core::Module> mod(create_settings_module());
        mod->onStart();
        const std::vector<std::string> wantGeneral = {
            "settings.card.appearance",
            "settings.card.editor"};
        const auto t0 = cardIdsOf(*mod, screen);
        DOCTEST_CHECK_MESSAGE(t0 == wantGeneral, "通用 tab 实测：" + join(t0));
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
        const std::vector<std::string> wantModulesAll = {
            "settings.card.modules",
            "settings.card.quota",
            "settings.card.directory",
            "settings.card.proxy"};
        const auto t1 = cardIdsOf(*mod, screen);
        DOCTEST_CHECK_MESSAGE(t1 == wantModulesAll, "模块 tab（全开） 实测：" + join(t1));
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 2));
        const std::vector<std::string> wantLog = {
            "settings.card.log",
            "settings.tab.log"};
        const auto t2 = cardIdsOf(*mod, screen);
        DOCTEST_CHECK_MESSAGE(t2 == wantLog, "日志 tab 实测：" + join(t2));
        mod->onStop();
    }
    // ② pet / markdown / claude 关：模块 tab 只剩插拔卡（其余整卡隐藏）
    {
        test_support::TestEnv env(kCfgPrefix, "registrygated", true);
        test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "registrygated_cfg"));
        {
                // 按顶层键写进 app_config 表各行
            DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
                ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\":\"dark\",\"enabledModules\":[\"home\"," "\"settings\"]}"));
        }
        std::unique_ptr<::core::Module> mod(create_settings_module());
        mod->onStart();
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
        const std::vector<std::string> wantModulesGated = {
            "settings.card.modules",
            "settings.card.quota"};
        const auto g1 = cardIdsOf(*mod, screen);
        DOCTEST_CHECK_MESSAGE(g1 == wantModulesGated, "模块 tab（门控全关） 实测：" + join(g1));
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 0));
        const std::vector<std::string> wantGeneralGated = {
            "settings.card.appearance",
            "settings.card.editor"};
        const auto g0 = cardIdsOf(*mod, screen);
        DOCTEST_CHECK_MESSAGE(g0 == wantGeneralGated, "通用 tab（门控不影响） 实测：" + join(g0));
        mod->onStop();
    }
    ::core::theme::setDark();
    return true;
})

// 反断言：设置页不得出现 cardFrame 的 bg 矩形
// cardFrame = surface 填充 + 1px 边框 + panelShadow 阴影 + radiusLarge 圆角；白色卡框叠在
// 主体渐变底上就是用户说的「最外层卡片」。反断言：<stack>.bg 不存在（cardFrame 若加回会
// 重新出现）、卡 stack 仍在（内容未丢）。
// 页面渐变底本身也已删，页面级反断言在 test_ui_modern.cpp
// uiModernSettingsBodyGradientRemoved（settings.body.gradient / .bg 必须都不存在）。
REGISTER_TEST(settings_cards_no_outer_cardframe,
              "settings: 8 卡无外层 cardFrame——内容直接铺页面底（两档主题）", {
    test_support::TestEnv env(kCfgPrefix, "noframe", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "noframe_cfg"));
    {
            // 按顶层键写进 app_config 表各行
        DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
            ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(), "{\"theme\":\"dark\",\"enabledModules\":[" "\"home\",\"markdown\",\"todo\",\"claude_proxy\",\"settings\"]}"));
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 卡 root stack id（内容锚）+ cardFrame 若加回会出现的 bg id（cardFrame 在 (0,0)
    // 铺的矩形，id = <stack>.bg）。顺序 = 注册表序
    const int kTabOf[] = {0, 0, 1, 1, 1, 1, 2, 2};
    const char* const kStackIds[] = {
        "settings.card.appearance",
        "settings.card.editor",
        "settings.card.modules",
        "settings.card.quota",
        "settings.card.directory",
        "settings.card.proxy",
        "settings.card.log",
        "settings.tab.log",
    };
    const char* const kBgIds[] = {
        "settings.card.appearance.bg",
        "settings.card.editor.bg",
        "settings.card.modules.bg",
        "settings.card.quota.bg",
        "settings.card.directory.bg",
        "settings.card.proxy.bg",
        "settings.card.log.bg",
        "settings.tab.log.bg",
    };
    for (int pass = 0; pass < 2; ++pass) {
        const std::string tag = pass == 0 ? "dark" : "light";
        if (pass == 0) {
            ::core::theme::setDark();
        } else {
            ::core::theme::setLight();
        }
        int lastTab = -1;
        for (size_t i = 0; i < 8; ++i) {
            if (kTabOf[i] != lastTab) {
                lastTab = kTabOf[i];
                DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, lastTab));
            }
            eui::Ui ui;
            ui.begin("noframe_" + tag);
            mod->compose(ui, screen);
            ui.end();
            ui.layout(screen.width, screen.height);
            DOCTEST_CHECK_MESSAGE(ui.find(kBgIds[i]) == nullptr,
                                  tag + " 卡 " + kBgIds[i] +
                                      " 不应再有外层卡框（cardFrame 已删）");
            DOCTEST_REQUIRE_MESSAGE(ui.find(kStackIds[i]) != nullptr,
                                    tag + " 卡 stack " + kStackIds[i] +
                                        " 应存在（内容未丢）");
        }
    }
    mod->onStop();
    ::core::theme::setDark();
    return true;
})


// AppTools（app.read_settings）
// 取数面 = SettingsStore.load()（自持实例，不摸模块成员——agent 工作线程调用天然
// 安全）。钉三件事：读回持久化值、输出经 sanitized 钳制且只暴露字段白名单（不泄配置里的
// 敏感键）、磁盘异常不静默。

REGISTER_TEST(app_read_settings_tool_roundtrip,
              "app_tools: app.read_settings 读回持久化配置（只读非破坏）", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg15", true);
    const auto path = test_support::tempConfigFile(kCfgPrefix, "appread");
    module::settings::SettingsStore store(::core::appDatabase());
    module::settings::SettingsConfig cfg;                    // 默认值 + 改两处
    cfg.fontScalePercent = 110;
    cfg.proxyPort = 9999;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    auto tool = module::settings::makeAppReadSettingsTool(::core::appDatabase());
    DOCTEST_CHECK_EQ(tool->name(), std::string("app.read_settings"));
    DOCTEST_CHECK_FALSE(tool->isDestructive());
    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("fontScalePercent").asInt(), 110);
    DOCTEST_CHECK_EQ(obj.get("proxyPort").asInt(), 9999);
    DOCTEST_CHECK(obj.get("themeMode").isString());
    return true;
})

    // 安全面：输出只含白名单字段——不 dump 整份配置，配置里将来若混入敏感键（此处以
    // apiToken 模拟）也不得外泄；越界值以 sanitized 后的「能生效的值」返回。
    // 盘上直写 raw JSON = 绕开 save 的 sanitized（模拟手改）
REGISTER_TEST(app_read_settings_tool_sanitizes_and_whitelists_fields,
              "app_tools: app.read_settings 输出钳制 + 字段白名单（不泄敏感键）", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg16", true);
    const auto path =
        test_support::tempConfigFile(kCfgPrefix, "appread_sanitize");
    removeQuietly(path);
        // 盘上键名（不是工具出参键名）：直写必须按盘上契约，否则测的是「键没读到」。
        // 未知键（apiToken）照样写进去——那正是白名单要挡的东西。
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(), module::settings::SettingsStore::keyNamespace(),
        std::string("{\n") +
            "\"fontScalePercentV3\": 9999,\n" +
            "\"logRetentionDays\": -5,\n" +
            "\"claudeProxyPort\": 70000,\n" + "\"themeMode\": \"neon\",\n" +
            "\"windowEffect\": \"liquid-glass\",\n" +
            "\"apiToken\": \"sk-should-never-leak\",\n" +
            "\"enabledModules\": [\"markdown\", \"not-a-module\"]\n" + "}\n"));

    auto tool = module::settings::makeAppReadSettingsTool(::core::appDatabase());
    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();

    DOCTEST_CHECK_EQ(obj.get("fontScalePercent").asInt(),
                     static_cast<int64_t>(
                         module::settings::kMaxFontScalePercent));
    DOCTEST_CHECK_EQ(obj.get("logRetentionDays").asInt(), 0);
    DOCTEST_CHECK_EQ(obj.get("proxyPort").asInt(),
                     static_cast<int64_t>(module::settings::kDefaultProxyPort));
    DOCTEST_CHECK_EQ(obj.get("themeMode").asString(),
                     std::string(module::settings::kThemeModeAuto));
    DOCTEST_CHECK_EQ(obj.get("windowEffect").asString(),
                     std::string(module::settings::kWindowEffectOff));

    // 白名单：恰好 9 个键；敏感 / 未知键一个都不出现
    DOCTEST_CHECK_EQ(obj.keys().size(), 9u);
    DOCTEST_CHECK_FALSE(obj.has("apiToken"));
    DOCTEST_CHECK(r.value().contentJson.find("sk-should-never-leak") ==
                  std::string::npos);

    // 模块开关数组：未知 ID 被过滤 + 恒启用项补齐（sanitized 口径）
    auto mods = obj.get("enabledModules");
    DOCTEST_REQUIRE(mods.isArray());
    DOCTEST_CHECK_EQ(mods.size(), 3);
    DOCTEST_CHECK_EQ(mods[0].asString(), std::string("markdown"));
    DOCTEST_CHECK(r.value().contentJson.find("not-a-module") ==
                  std::string::npos);
    return true;
})

    // 磁盘异常：配置存在但不可读 → isError 回喂。load() 对同类异常静默回落默认值
    //（settings_config.cpp 只 WARN）——工具若照抄，agent 会把「读不到」当成「用户就是
    // 这么配的」= 静默假答案。
REGISTER_TEST(app_read_settings_tool_reports_unreadable_config,
              "app_tools: app.read_settings 配置不可读 → isError（不静默回落）", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg17", true);
        // 「读不到配置」= app_config 表不存在。用一个**只 open、没 migrate** 的连接造出
        // 这个形态（表真的不存在，不是模拟），并另外钉住 db 为 nullptr 的那一支。
    test_support::InMemoryDb noSchema;
    auto noSchemaTool = module::settings::makeAppReadSettingsTool(&noSchema.handle());
    auto noSchemaResult = noSchemaTool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(noSchemaResult.isOk());   // 工具级错误走 isError，不是 Result fail
    DOCTEST_CHECK(noSchemaResult.value().isError);
    DOCTEST_CHECK(platform::json::parse(noSchemaResult.value().contentJson)
                      .value()
                      .asObject()
                      .get("error")
                      .isString());

    auto nullDbTool = module::settings::makeAppReadSettingsTool(nullptr);
    auto nullDbResult = nullDbTool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(nullDbResult.isOk());
    DOCTEST_CHECK(nullDbResult.value().isError);

    // 对照支：库正常且表在 → 读得到，**不**是错误（否则前两条断言就成了
    // 「工具对任何输入都报错」的恒真）
    auto tool = module::settings::makeAppReadSettingsTool(::core::appDatabase());
    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    return true;
})

    // 非破坏性：无配置（首次启动路径）→ 默认值 + isError=false，且**不落盘**
REGISTER_TEST(app_read_settings_tool_missing_file_is_defaults_without_write,
              "app_tools: app.read_settings 无配置文件 → 默认值且不落盘", {
    // 配置落 app_config 表，生产代码经 core::appDatabase() 取连接；未注册则 save 一律失败。
    test_support::TestEnv cfgDbEnv(kCfgPrefix, "cfg18", true);
    const auto path =
        test_support::tempConfigFile(kCfgPrefix, "appread_missing");
    removeQuietly(path);
    DOCTEST_REQUIRE(!std::filesystem::exists(path));

    auto tool = module::settings::makeAppReadSettingsTool(::core::appDatabase());
    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK_EQ(parsed.value().asObject().get("fontScalePercent").asInt(),
                     100);

    std::error_code ec;
    DOCTEST_CHECK_FALSE(std::filesystem::exists(path, ec));
    return true;
})

// Token 持久化落 github_sync_state（本机专属表，不进 dump）。存的是 credential::protect
// 后形态，读回必须还原明文；未存过 = fail(NotFound)（hasToken 判据依赖 fail，不是空串）。
REGISTER_TEST(settings_github_token_db_roundtrip,
              "settings: GitHub Sync Token 落库往返 + 未存过 NotFound + 删除", {
    test_support::TestEnv env(kCfgPrefix, "gstoken", true);
    module::settings::GithubSyncStore store(::core::appDatabase());

    auto none = store.loadToken();
    DOCTEST_CHECK(none.isFail());

    const std::string tok = "ghp_roundtrip_token_0123456789abcdef";
    DOCTEST_REQUIRE(store.saveToken(tok).isOk());
    auto back = store.loadToken();
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK_EQ(back.value(), tok);

    // 行里确实是 protect 后形态：明文平台 = 原文；加密平台不得出现明文
    {
        auto* db = ::core::appDatabase();
        std::lock_guard<std::recursive_mutex> lk(db->mutex());
        auto prep = db->prepare("SELECT value FROM github_sync_state WHERE key = 'token'");
        DOCTEST_REQUIRE(prep.isOk());
        auto& st = prep.value();
        auto step = st.step();
        DOCTEST_REQUIRE(step.isOk());
        DOCTEST_REQUIRE(step.value());
        auto row = st.currentRow();
        DOCTEST_REQUIRE(row.isOk());
        auto stored = row.value().getString("value");
        DOCTEST_REQUIRE(stored.isOk());
        if (!platform::credential::storesPlaintext()) {
            DOCTEST_CHECK(stored.value().find(tok) == std::string::npos);
        }
    }

    // 同键覆盖写：upsert 不报错、读回是新值
    DOCTEST_REQUIRE(store.saveToken("ghp_second_token").isOk());
    auto again = store.loadToken();
    DOCTEST_REQUIRE(again.isOk());
    DOCTEST_CHECK_EQ(again.value(), std::string("ghp_second_token"));

    // 删除后回到「未存过」态
    DOCTEST_REQUIRE(store.deleteToken().isOk());
    DOCTEST_CHECK(store.loadToken().isFail());
    return true;
})

// 「自动推送」开关接线：关掉后**自动**路径不得发起推送，手动按钮不受开关限制。
// 观察口：仓库 URL 留空时，自动闸一放行就落在本地错误分支（setGithubSyncError
// 「未配置仓库 URL」）→ 卡片红字行出现即证明「自动推送真被发起」；红字不出现即
// 证明闸拦住了。该分支不读 keychain、不起线程、不出网，故可作无头判据。
REGISTER_TEST(settings_github_sync_autopush_switch_gates_auto_path,
              "settings: 关「自动推送」→ dirty 不自动推、手动「立即同步」照常发起", {
    test_support::TestEnv env(kCfgPrefix, "gsautopush", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "gsautopush_cfg"));
    eui::Screen screen{1600.0f, 1000.0f};
    const char* kErrRow = "settings.card.github_sync.error";

    // 一次「置 dirty → 帧内跑自动闸 → 看卡片有没有红字」的完整往返
    auto autoPathFiresWhenDirty = [&](bool autoPush) {
        module::settings::GithubSyncConfig cfg;
        cfg.enabled = true;
        cfg.autoPush = autoPush;   // repoUrl 留空
        module::settings::GithubSyncStore(::core::appDatabase()).save(cfg);

        std::unique_ptr<::core::Module> mod(create_settings_module());
        mod->onStart();
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 3));
        ::core::event::bus().emit(::core::event::kMdSaved);   // 笔记写入 → 置 dirty

        eui::Ui ui;
        ui.begin("gsautopush");
        mod->compose(ui, screen);   // 帧内跑自动闸（dirty 已置位）
        ui.end();
        const bool fired = ui.find(kErrRow) != nullptr;

        if (!fired) {
            // 闸拦住时才验「手动仍可」：手动按钮不经自动闸 → 红字必须出现
            const auto* btn = ui.find("settings.card.github_sync.btn.push.bg");
            DOCTEST_REQUIRE(btn != nullptr);
            DOCTEST_REQUIRE(btn->onClick != nullptr);
            btn->onClick();
            eui::Ui ui2;
            ui2.begin("gsautopush_manual");
            mod->compose(ui2, screen);
            ui2.end();
            DOCTEST_CHECK(ui2.find(kErrRow) != nullptr);
        }
        mod->onStop();
        return fired;
    };

    // 关掉开关：dirty 也不自动推；开着（对照）：同一 dirty 立刻发起
    const bool off = autoPathFiresWhenDirty(false);
    const bool on = autoPathFiresWhenDirty(true);
    DOCTEST_CHECK_MESSAGE(!off, "关掉「自动推送」后自动路径仍发起了推送");
    DOCTEST_CHECK_MESSAGE(on, "开着「自动推送」时自动路径未发起（判据恒假）");
    return true;
})

// dirty 触发源：笔记写入/删除 + 待办增删改，每个写路径事件都必须把 GitHub Sync 置
// dirty（只订一部分 = 那类改动永远不自动同步）。逐个事件名验证，并带无关事件对照
// ——否则断言退化成「任何事件都置位」。观察口同 autopush 用例：无仓库 URL 时自动闸
// 一放行就报「未配置仓库 URL」，卡片红字行即「dirty 已置位」的可见证据。
REGISTER_TEST(settings_github_sync_dirty_from_write_events,
              "settings: 笔记/待办各写路径事件 → GitHub Sync 置 dirty（带无关事件对照）", {
    test_support::TestEnv env(kCfgPrefix, "gsdirty", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "gsdirty_cfg"));
    eui::Screen screen{1600.0f, 1000.0f};
    const char* kErrRow = "settings.card.github_sync.error";

    // dirty 是闩（置位后不再变、日志也只记首次），故每个事件名用独立模块实例
    auto dirtyAfter = [&](const char* ev) {
        module::settings::GithubSyncConfig cfg;
        cfg.enabled = true;
        cfg.autoPush = true;   // 自动闸开着：dirty 一置位就发起推送 → 红字可见
        module::settings::GithubSyncStore(::core::appDatabase()).save(cfg);

        std::unique_ptr<::core::Module> mod(create_settings_module());
        mod->onStart();
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 3));
        ::core::event::bus().emit(ev);

        eui::Ui ui;
        ui.begin("gsdirty");
        mod->compose(ui, screen);
        ui.end();
        const bool dirty = ui.find(kErrRow) != nullptr;
        mod->onStop();
        return dirty;
    };

    DOCTEST_CHECK(dirtyAfter(::core::event::kMdSaved));
    DOCTEST_CHECK(dirtyAfter(::core::event::kMdCreated));
    DOCTEST_CHECK(dirtyAfter(::core::event::kMdRenamed));
    DOCTEST_CHECK(dirtyAfter(::core::event::kMdDeleted));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoCreated));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoUpdated));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoMoved));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoCompleted));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoUncompleted));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoDeleted));
    DOCTEST_CHECK(dirtyAfter(::core::event::kTodoColumnChanged));
    // 对照：本模块没订阅的事件名 → 不置 dirty
    DOCTEST_CHECK(!dirtyAfter(::core::event::kDbImported));
    return true;
})

// 关掉「自动推送」时的退出：不推（尊重用户选择），但必须留下「有未同步改动」信号——
// 否则退出后既不推也没黄字，用户无从知道改动还没上传。观察口两条：卡片无「未配置仓库
// URL」红字（= 没发起推送；repoUrl 留空，一旦走推送路径就会本地报错变红）+ 提示键落库。
REGISTER_TEST(settings_github_sync_exit_push_keeps_unsynced_notice_when_autopush_off,
              "settings: 关自动推送时退出不推但落「有未同步改动」提示（带无改动对照）", {
    test_support::TestEnv env(kCfgPrefix, "gsexitnotice", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "gsexitnotice_cfg"));
    eui::Screen screen{1600.0f, 1000.0f};
    const char* kErrRow = "settings.card.github_sync.error";

    // 断言宏一律 return false（自制 doctest 的 fail-fast 形态），故 lambda 里不嵌断言，
    // 把「前置是否成立」一并回传，由调用方判——否则前置失败会退化成恒假断言（假绿）。
    struct Probe {
        bool tabOk = false;    // 卡片确实在渲染（否则 pushed 恒假）
        bool pushed = false;   // 红字 = 真发起了推送
        bool noticed = false;  // 「有未同步改动」提示已落库
    };
    auto runExitAndProbe = [&](bool dirty) -> Probe {
        module::settings::GithubSyncConfig cfg;
        cfg.enabled = true;
        cfg.autoPush = false;   // repoUrl 留空：走推送路径会立刻本地报错 → 红字可见
        module::settings::GithubSyncStore store(::core::appDatabase());
        store.save(cfg);
        store.clearUnsyncedNotice();

        std::unique_ptr<::core::Module> mod(create_settings_module());
        mod->onStart();
        const bool tabOk = clickSettingsTab(*mod, screen, 3);
        if (dirty) ::core::event::bus().emit(::core::event::kMdSaved);

        ::module::settings::runGithubSyncExitPush();   // 生产退出路径（atexit 调的就是它）

        eui::Ui ui;
        ui.begin("gsexitnotice");
        mod->compose(ui, screen);
        ui.end();
        Probe out;
        out.tabOk = tabOk;
        out.pushed = ui.find(kErrRow) != nullptr;
        const auto notice = store.hasUnsyncedNotice();
        out.noticed = notice.isOk() && notice.value();
        mod->onStop();
        return out;
    };

    const Probe withDirty = runExitAndProbe(true);
    DOCTEST_CHECK(withDirty.tabOk);
    DOCTEST_CHECK(!withDirty.pushed);    // 关掉开关 → 退出不发起推送
    DOCTEST_CHECK(withDirty.noticed);    // 但「有未同步改动」必须留下
    const Probe noDirty = runExitAndProbe(false);
    DOCTEST_CHECK(noDirty.tabOk);
    DOCTEST_CHECK(!noDirty.pushed);
    DOCTEST_CHECK(!noDirty.noticed);     // 对照：没改动 → 不留信号（证明上面那条来自 dirty）
    return true;
})

REGISTER_TEST(settings_log_tail_level_parse, "settings: log_tail_level 解析各行级别", {
    using module::settings::logLineLevel;
    using core::log::Level;

    // 四档标准行（levelToString 对 INFO/WARN 加尾空格 padding）
    DOCTEST_CHECK_EQ(logLineLevel("[2025-01-01 00:00:00.000] [DEBUG] x"), Level::Debug);
    DOCTEST_CHECK_EQ(logLineLevel("[2025-01-01 00:00:00.000] [INFO ] x"), Level::Info);
    DOCTEST_CHECK_EQ(logLineLevel("[2025-01-01 00:00:00.000] [WARN ] x"), Level::Warn);
    DOCTEST_CHECK_EQ(logLineLevel("[2025-01-01 00:00:00.000] [ERROR] x"), Level::Error);

    // TRACE / FATAL 虽不开放给设置页下拉，但日志文件中可能出现
    DOCTEST_CHECK_EQ(logLineLevel("[2025-01-01 00:00:00.000] [TRACE] x"), Level::Trace);
    DOCTEST_CHECK_EQ(logLineLevel("[2025-01-01 00:00:00.000] [FATAL] x"), Level::Fatal);

    // 解析失败 → nullopt
    DOCTEST_CHECK_FALSE(logLineLevel("garbage line").has_value());
    DOCTEST_CHECK_FALSE(logLineLevel("[stamp] no-second-bracket").has_value());
    DOCTEST_CHECK_FALSE(logLineLevel("[stamp] [XYZ] unknown level").has_value());
    DOCTEST_CHECK_FALSE(logLineLevel("").has_value());
    // 第一个方括号不是时间戳但格式上存在两个方括号 → 仍能解析
    DOCTEST_CHECK_EQ(logLineLevel("[a] [DEBUG] functional"), Level::Debug);
    return true;
})

REGISTER_TEST(settings_log_tail_level_filter, "settings: log_tail_level 按级别过滤", {
    using module::settings::filterLogTailByLevel;
    using core::log::Level;

    const std::vector<std::string> lines = {
        "[2025-01-01 00:00:00.000] [TRACE] t",
        "[2025-01-01 00:00:01.000] [DEBUG] d",
        "[2025-01-01 00:00:02.000] [INFO ] i",
        "[2025-01-01 00:00:03.000] [WARN ] w",
        "[2025-01-01 00:00:04.000] [ERROR] e",
        "[2025-01-01 00:00:05.000] [FATAL] f",
        "garbage line",
        "",
    };

    // DEBUG → 保留 DEBUG 及以上（TRACE 被过滤）
    auto rDebug = filterLogTailByLevel(lines, Level::Debug);
    DOCTEST_CHECK_EQ(rDebug.size(), 7u);  // TRACE 被滤，garbage+空行保留

    // INFO → 保留 INFO 及以上 + 无级别行
    auto rInfo = filterLogTailByLevel(lines, Level::Info);
    DOCTEST_CHECK_EQ(rInfo.size(), 6u);  // TRACE/DEBUG 被过滤

    // WARN → 保留 WARN 及以上 + 无级别行
    auto rWarn = filterLogTailByLevel(lines, Level::Warn);
    DOCTEST_CHECK_EQ(rWarn.size(), 5u);  // WARN/ERROR/FATAL + garbage + 空行

    // ERROR → 保留 ERROR 及以上 + 无级别行
    auto rError = filterLogTailByLevel(lines, Level::Error);
    DOCTEST_CHECK_EQ(rError.size(), 4u);  // ERROR/FATAL + garbage + 空行

    // FATAL → FATAL 本身 + garbage + 空行
    auto rFatal = filterLogTailByLevel(lines, Level::Fatal);
    DOCTEST_CHECK_EQ(rFatal.size(), 3u);  // FATAL(5) + garbage + 空行，其余均被滤

    // TRACE 永远保留全部
    auto rTrace = filterLogTailByLevel(lines, Level::Trace);
    DOCTEST_CHECK_EQ(rTrace.size(), 8u);
    return true;
})

REGISTER_TEST(settings_log_tail_level_filter_empty, "settings: log_tail_level 空输入与全滤空", {
    using module::settings::filterLogTailByLevel;
    using core::log::Level;

    // 空输入 → 空输出
    auto rEmpty = filterLogTailByLevel({}, Level::Info);
    DOCTEST_CHECK(rEmpty.empty());

    // 所有行级别都低于阈值 → 只保留无级别行；全有级别则全滤空
    const std::vector<std::string> allDebug = {"[x] [DEBUG] a", "[x] [DEBUG] b"};
    auto rErr = filterLogTailByLevel(allDebug, Level::Error);
    DOCTEST_CHECK(rErr.empty());

    // 纯 garbage 行 → 全部保留（fail-open）
    const std::vector<std::string> garbage = {"foo", "bar", ""};
    auto rG = filterLogTailByLevel(garbage, Level::Error);
    DOCTEST_CHECK_EQ(rG.size(), 3u);

    return true;
})

// 推送分支定向（审计 finding 4 回归钉）：PUT/DELETE 的 body 必须带配置分支
//（branch 字段），与拉取（?ref= / listTree）同一分支——不传 = Contents API 落
// 仓库默认分支，推拉分叉。离线：setDefaultSender 静态注入假发送器，模块真跑
// 推送路径（真按钮 → 后台线程 → onStop join 后断言录到的请求）。
// 前置 REQUIRE 全部在点击之前（线程未起，fail-fast 安全）；点击后只用非致命 CHECK。
REGISTER_TEST(settings_github_sync_push_writes_target_configured_branch,
              "github_sync: 推送 PUT/DELETE 全部定向配置分支（非默认分支）", {
    test_support::TestEnv env(kCfgPrefix, "gsbranch", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "gsbranch_cfg"));
    eui::Screen screen{1600.0f, 1000.0f};

    // 笔记夹具：本地一篇（New → Put）+ 登记有而本地无（LocalDeleted → Delete 传播）
    const auto notesDir = test_support::makeTempDir("devdesk_test_notes_", "gsbranch");
    test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", notesDir.string());
    {
        std::ofstream(notesDir / "note-a.md", std::ios::binary) << "本地新增";
    }

    module::settings::GithubSyncConfig cfg;
    cfg.enabled = true;
    cfg.repoUrl = "https://github.com/devdesk-test/sync-repo";
    cfg.branch = "sync-branch";   // 刻意 ≠ 默认分支 main：错落默认分支立刻显形
    cfg.autoPush = false;
    module::settings::GithubSyncStore store(::core::appDatabase());
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    DOCTEST_REQUIRE(store.setFirstSyncDone().isOk());   // 手动同步直走 Push 不跑 FirstSync
    // Delete 传播：登记 sha 与远端树一致（gonesha）→ LocalDeleted 且远端未变
    DOCTEST_REQUIRE(store.recordSyncedNote("gone.md", "gonesha", "0deadbeef").isOk());

    // 假发送器：按序回放脚本化响应；模块在后台线程里自建 api 实例，靠
    // setDefaultSender（静态）注入——实例级 setSender 够不着那个实例。
    struct ScriptedSender {
        std::mutex mu;
        std::vector<platform::http::Request> reqs;
        std::vector<platform::http::Response> plan;
        core::Result<platform::http::Response> operator()(
            const platform::http::Request& r) {
            platform::http::Response resp;
            {
                std::lock_guard<std::mutex> lk(mu);
                reqs.push_back(r);
                if (!plan.empty()) {
                    resp = reqs.size() <= plan.size() ? plan[reqs.size() - 1]
                                                      : plan.back();
                }
            }
            return core::Result<platform::http::Response>::ok(std::move(resp));
        }
    };
    ScriptedSender rec;
    // 顺序 = runGithubSyncPush 的请求序：getRepo → listTree（笔记判态与
    // database.sql 的冲突检测 sha 同源同一棵树，无独立 probe 请求；树清单必须
    // 带 sql 条目，否则 PUT 不带 sha、断言必红）→ PUT database.sql
    //（步④，先于笔记）→ DELETE gone.md → PUT note-a.md（步⑤，plans 按名字字典序）。
    rec.plan = {
        {200, R"({"full_name":"devdesk-test/sync-repo","default_branch":"main",)"
              R"("private":true,"permissions":{"push":true}})"},
        {200, R"({"sha":"t0","truncated":false,"tree":[)"
              R"({"path":"data/database.sql","sha":"sqlremotesha","type":"blob","size":3},)"
              R"({"path":"notes/gone.md","sha":"gonesha","type":"blob","size":5}]})"},
        {201, R"({"content":{"sha":"sqlnewsha"}})"},
        {200, R"({"commit":{"sha":"delsha"}})"},
        {201, R"({"content":{"sha":"notenewsha"}})"},
    };
    struct SenderGuard {
        SenderGuard(platform::github::GithubApi::Sender s) {
            platform::github::GithubApi::setDefaultSender(std::move(s));
        }
        ~SenderGuard() { platform::github::GithubApi::setDefaultSender(nullptr); }
        SenderGuard(const SenderGuard&) = delete;
        SenderGuard& operator=(const SenderGuard&) = delete;
    } senderGuard(
        [&rec](const platform::http::Request& r) { return rec(r); });

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 3));

    // Token 草稿经卡片真输入框注入（不碰 keychain）
    eui::Ui ui;
    ui.begin("gsbranch");
    mod->compose(ui, screen);
    ui.end();
    auto* tokenInput = ui.find("settings.card.github_sync.token.input.hit");
    DOCTEST_REQUIRE(tokenInput != nullptr);
    DOCTEST_REQUIRE(tokenInput->onTextInput != nullptr);
    eui::TextInputEvent tok;
    tok.text = "test-token";
    tokenInput->onTextInput(tok);

    auto* pushBtn = ui.find("settings.card.github_sync.btn.push.bg");
    DOCTEST_REQUIRE(pushBtn != nullptr);
    DOCTEST_REQUIRE(pushBtn->onClick != nullptr);

    pushBtn->onClick();   // 起后台推送线程
    // 轮询帧直到后台线程完成（compose 每帧 harvest → 按钮解禁），再 onStop join
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        eui::Ui w;
        w.begin("gsbranch_wait");
        mod->compose(w, screen);
        w.end();
        const auto* b = w.find("settings.card.github_sync.btn.push.bg");
        std::lock_guard<std::mutex> lk(rec.mu);
        if (b != nullptr && !b->disabled && rec.reqs.size() >= 4) break;
    }
    mod->onStop();        // join；此后无 REQUIRE，全部非致命 CHECK


    std::vector<platform::http::Request> reqs;
    {
        std::lock_guard<std::mutex> lk(rec.mu);
        reqs = rec.reqs;
    }
    DOCTEST_CHECK_MESSAGE(reqs.size() >= 5,
                          "请求数不足（推送未按脚本走完）: " +
                              std::to_string(reqs.size()));

    std::vector<const platform::http::Request*> puts;
    std::vector<const platform::http::Request*> dels;
    for (const auto& r : reqs) {
        if (r.method == "PUT") puts.push_back(&r);
        if (r.method == "DELETE") dels.push_back(&r);
    }
    DOCTEST_CHECK_MESSAGE(puts.size() == 2,
                          "PUT 数应为 2（database.sql + note-a.md），实际 " +
                              std::to_string(puts.size()));
    DOCTEST_CHECK_MESSAGE(dels.size() == 1,
                          "DELETE 数应为 1（gone.md），实际 " +
                              std::to_string(dels.size()));

    // 核心断言：每条写请求 body 都带 branch=sync-branch（缺 = 落默认分支）
    for (const auto* p : puts) {
        DOCTEST_CHECK_MESSAGE(
            p->body.find("\"branch\":\"sync-branch\"") != std::string::npos,
            "PUT " + p->url + " body 缺 branch=sync-branch: " + p->body);
    }
    for (const auto* d : dels) {
        DOCTEST_CHECK_MESSAGE(
            d->body.find("\"branch\":\"sync-branch\"") != std::string::npos,
            "DELETE " + d->url + " body 缺 branch=sync-branch: " + d->body);
    }
    // 写入对象核对：database.sql / notes/gone.md / notes/note-a.md
    bool sawSqlPut = false;
    bool sawNotePut = false;
    bool sawGoneDel = false;
    for (const auto* p : puts) {
        if (p->url.find("/contents/data/database.sql") != std::string::npos) {
            sawSqlPut = true;
            DOCTEST_CHECK_MESSAGE(
                p->body.find("\"sha\":\"sqlremotesha\"") != std::string::npos,
                "database.sql PUT 缺冲突检测 sha: " + p->body);
        }
        if (p->url.find("/contents/notes/note-a.md") != std::string::npos) {
            sawNotePut = true;
        }
    }
    if (!dels.empty()) {
        sawGoneDel = dels[0]->url.find("/contents/notes/gone.md") !=
                     std::string::npos;
    }
    DOCTEST_CHECK_MESSAGE(sawSqlPut, "未 PUT database.sql");
    DOCTEST_CHECK_MESSAGE(sawNotePut, "未 PUT notes/note-a.md");
    DOCTEST_CHECK_MESSAGE(sawGoneDel, "未 DELETE notes/gone.md");

    // 拉取侧基线：contents GET 的 ref 必须也是配置分支（防「两边都退回默认分支」假绿）
    for (const auto& r : reqs) {
        if (r.method == "GET" && r.url.find("/contents/") != std::string::npos) {
            DOCTEST_CHECK_MESSAGE(r.url.find("ref=sync-branch") !=
                                      std::string::npos,
                                  "contents GET ref 非配置分支: " + r.url);
        }
    }
    return true;
})
