// tests/test_modules/test_settings.cpp
// P13 设置页单测：SettingsStore（config.json 读写）纯逻辑 + 模块 compose 无头冒烟
// 覆盖：默认值 / 保存-加载往返 / 损坏 JSON 回退 / 数值钳制 / 主题字段映射 /
//       设置页可渲染（DSL 元素树构建）/ 主题切换对下一次 compose 即时生效 /
//       日志保留下拉弹层不出窗体（openUp）/ 切换档位立即清理 / 文本垂直居中 /
//       窗口效果三档（磨砂 Phase D）：默认值/往返/非法值收敛、档位→alpha/
//       backdrop 映射、降级提示矩阵、applyWindowVisuals 接线（无头）
//       R6（spec §5.2）：settings_ui_logic 纯件等价（openUp 几何/档位表/最近邻/
//       字号缩放/保留档文案/下拉持有者语义）+ 卡片注册表（tab 归属/顺序/门控）
#include "doctest.h"
#include "test_support.h"
#include "settings_config.h"
#include "settings_ui_logic.h"

#include "claude_service.h"
#include "core/claude_proxy_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "module_pet/pet_window.h"  // PET-CLOSE：桌宠开关即时性断言读窗口可见意图
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

// 每个用例独立的临时配置文件（避免用例间串扰）—— 共享工厂（TIN-06b，
// 原三份 tempConfigFile 合流），调用点统一 test_support::tempConfigFile(
// kCfgPrefix, name)
constexpr const char* kCfgPrefix = "devdesk_test_settings_";

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

// ---- R0/D6：设置页的代理运行面经 core 只读服务定位器取 ----
// 设置页不再直呼 ClaudeService::global()（见源码护栏用例）；用例把自己 setGlobal
// 的 service 用**生产同一个适配器**（core::GlobalClaudeProxyService，转发到
// global()）暴露给定位器，生产 / 测试走同一条取用路径。RAII：作用域结束自动
// 注销（用例体内 fail-fast 提前 return 也不留悬垂注册）。
struct ScopedClaudeProxy {
    ::core::GlobalClaudeProxyService<module::claude::ClaudeService> adapter;
    ::core::ScopedService<::core::IClaudeProxyService> registration{&adapter};
};

// ==================== R0 护栏（源码级） ====================
// 需求（重构 spec R0 / 审计 D6/D7）两条：
//   D6 —— settings_module.cpp 不得再直连 ClaudeService 单例：零
//        `claude_service.h`、零 `module::claude`、**代码里**零 `ClaudeService`
//        （注释里的历史说明允许 → 逐行剥掉 `//` 之后的部分再查）；同时必须
//        真的经 core 只读服务定位器取（防「解耦 = 把功能删了」）。
//   D7 —— 代理端口常量单一来源 = core/proxy_port.h：字面量只在该头定义，
//        两个消费方（settings_config.h / claude_service.h）只从 core 取。
// 仓库根靠 cwd 上溯（**不用** __FILE__：ccache 会把绝对路径相对化，TIN-01
// 教训）；定位失败返回明确报告，不静默绿。

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

// 逐行扫针，命中拼成「相对路径:行号: 行内容」；stripLineComment=true 时先剥掉
// 行内 `//` 之后的部分（注释豁免——注释里点名历史机制是有意保留的说明）
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
               "定位（不能用 __FILE__：ccache 会把绝对路径相对化，TIN-01 教训）";
    }
    const std::string rel = "src/module_settings/settings_module.cpp";

    std::string report;
    // 1) 硬针：跨模块 include 与限定名，注释里也不许出现（历史说明不点名它们）
    for (const std::string& hit : scanFileForTokens(
             root, rel, {"claude_service.h", "module::claude"}, false)) {
        report += "设置页又直接引用了兄弟模块 claude：" + hit + "\n";
    }
    // 2) 软针：类型名本身——按行剥注释后再查（代码里出现即违规）
    for (const std::string& hit :
         scanFileForTokens(root, rel, {"ClaudeService"}, true)) {
        report += "设置页代码里又直呼了 ClaudeService：" + hit + "\n";
    }
    // 3) 正向：必须真的经 core 只读服务定位器取（解耦不是删功能）
    const std::string src = readWholeFile(root / rel);
    if (src.find("core/service_locator.h") == std::string::npos) {
        report += "设置页没有 include core/service_locator.h —— 代理运行面应由"
                  " core 只读服务定位器取（R0/D6）\n";
    }
    if (src.find("::core::IClaudeProxyService") == std::string::npos) {
        report += "设置页没有用 core::IClaudeProxyService —— 定位器通道被摘掉了？"
                  "（解耦不是删功能）\n";
    }
    // 4) 提供方：装配段必须真的注册（否则设置页会永久静默降级成「无服务」，
    //    而用例侧各自注册、覆盖不到这条生产接线）
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "missing");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "autostart");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "proxyport");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "roundtrip");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "zero");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "corrupt");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "partial");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "legacy");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "theme");
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "clampwrite");
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

    // tab 栏（3 个命中矩形）+ 卡区 scrollView 存在；默认 tab=通用
    frame("smoke");
    DOCTEST_CHECK(ui.find("settings.tabs.hit.0") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.1") != nullptr);
    DOCTEST_CHECK(ui.find("settings.tabs.hit.2") != nullptr);
    DOCTEST_CHECK(ui.find("settings.cards.scroll") != nullptr);
    // 通用 tab：外观卡 + 编辑器字号卡（Task B 自日志卡迁出）
    DOCTEST_CHECK(ui.find("settings.card.appearance") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.appearance.effect.ddwrap") != nullptr);
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    // tab 划分 = 互斥：其余 tab 的卡不渲染
    DOCTEST_CHECK(ui.find("settings.card.modules") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.log") == nullptr);
    DOCTEST_CHECK(ui.find("settings.card.about") == nullptr);

    // 模块 tab：插拔开关 + 目录 + 代理（ClaudeService::global() 为 null 时
    // 代理卡仍渲染）。W10 复审项在此 tab 依旧有效（多卡超小窗可滚）
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

// ---- Task B：日志查看卡 —— 刷新按钮点击 → 重读 tail（读盘在 compose 顶部，
// 不在 scrollView content lambda 内）；日志文件缺失时渲染空态不崩 ----

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

// ---- 审计 [19]：代理端口校验失败反馈（红字状态行 + 下一次输入清除）----
// 设置页无创建弹窗，端口 draft 就地提交；反馈通道对齐 claude formError 语义：
// 校验错误以红字呈现（settings.subtitle 走 colors.error，非 toast），
// 且随下一次输入自动清除（修复前红字挂到下一次操作才走）。

REGISTER_TEST(settings_port_error_clear_on_input,
              "settings: 端口校验失败 → 红字状态行 + 下一次输入清除（headless，审计 [19]）", {
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
    // 非法端口未落盘：临时 config 文件里的 proxyPort 仍是默认 8888
    module::settings::SettingsStore store(cfgEnv.cfgPath);
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
    DOCTEST_CHECK(ui.find("settings.card.appearance") != nullptr);

    ::core::theme::setDark();  // 恢复
    return true;
})

// ---- 日志保留下拉弹层不被窗体裁剪（W8：小窗口下自动向上展开） ----

REGISTER_TEST(settings_log_dropdown_popup_stays_in_window, "settings: log dropdown popup stays in window (openUp)", {
    // 隔离：不隔离会读到真机 config.json（如本机启用了 pet 卡），卡片变多
    // 把日志卡推到更低位，openUp 也装不下——测试必须与机器本地配置无关
    test_support::TestEnv env(kCfgPrefix, "openup", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "openup_cfg"));
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    // Task B tab 化后日志卡位于「日志」tab 顶部（cardsTopY = 40 pad + 64
    // header + 16 gap + 40 tab + 16 gap = 176）。1600×1300 向下展开空间充足
    // （保留字段底 = 176 + 48 + 36 = 260，+ 8 gap + 弹层 216 = 484 ≤ 1284）
    // → 默认 openDown；短窗下同一弹层越出窗底（484 > 高 − 16）→ 应改 openUp
    // 向上展开（弹层整体留在窗体内且位于字段上方）。卡顶游标只累计本 tab 的卡，
    // 全量累加会把这里算错——即审计裁决「日志卡 openUp 游标」的回归位）
    // 2026-09-29：日志卡两行改同列口径后字段高 40→36（底边 88→84），越界阈值
    // 随之右移——短窗高度 500 → 470 才复现同一场景（484 > 454）
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
    test_support::TestEnv env(kCfgPrefix, "prune", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "prune_cfg"));
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
    auto db = std::make_unique<::core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    module::claude::ClaudeService svc(db->db(), "DevDeskTestTakeover");
    module::claude::ClaudeService::setGlobal(&svc);
    ScopedClaudeProxy proxyForSettings;  // R0/D6：设置页经定位器取代理运行面
    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.sidecarFile = dir / "snapshot.json";
    paths.overlayFiles.push_back(overlay);
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    test_support::setEnvVar("DEVDESK_CONFIG_PATH", cfgPath.string());

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
    test_support::setEnvVar("DEVDESK_CONFIG_PATH", "");
    platform::keychain::deleteSecret(
        "DevDeskTestTakeover", "claude:" + std::to_string(pr.value().id));
    fs::remove_all(dir, ec);
    return true;
})

// ---- 审计 CPX-02 残（波 1 L7）：未接管 + 警告非空时警告行仍要渲染 ----
// 接管失败（settings.json 坏 JSON → takeOver 在写 sidecar 之前中止）⇒
// isClaudeCodeManaged()=false（sidecar 不存在），但失败原因已进
// lastSyncWarnings。旧渲染门是 managed && svc → 该行永不显示，而卡高仍按
// 警告行数预留（渲染门与高度算源不一致）。本用例锁「行画出来 + 状态行仍
// 如实写未接管」
REGISTER_TEST(settings_takeover_warn_unmanaged,
              "settings: 未接管 + 警告非空 → 警告行仍渲染（CPX-02）", {
    namespace fs = std::filesystem;
    const fs::path dir =
        test_temp::tempPath("devdesk_test_settings_warn_unmanaged");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path cfgPath = dir / "config.json";

    auto r = ::core::Database::open(dir / "db.sqlite");
    DOCTEST_REQUIRE(r.isOk());
    auto db = std::make_unique<::core::Database>(std::move(r.value()));
    DOCTEST_REQUIRE(db->migrate().isOk());
    module::claude::ClaudeService svc(db->db(), "DevDeskTestWarnUnmanaged");
    module::claude::ClaudeService::setGlobal(&svc);
    ScopedClaudeProxy proxyForSettings;  // R0/D6：设置页经定位器取代理运行面
    module::claude::settings_sync::Paths paths;
    paths.settingsFile = dir / "settings.json";
    paths.sidecarFile = dir / "snapshot.json";
    svc.setSettingsSyncPaths(paths);
    svc.setSettingsTakeOverEnabled(true);
    test_support::setEnvVar("DEVDESK_CONFIG_PATH", cfgPath.string());

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
    DOCTEST_CHECK_FALSE(fs::exists(paths.sidecarFile));  // 失败在写盘之前

    mod->onPageEnter();  // 刷新接管状态快照（真实流程同款刷新点）
    eui::Ui ui;
    ui.begin("warn_unmanaged");
    mod->compose(ui, screen);
    ui.end();
    const auto* warn = ui.find("settings.card.proxy.takeover.warn.0");
    DOCTEST_REQUIRE(warn != nullptr);
    DOCTEST_CHECK(warn->text.find("接管失败") != std::string::npos);
    // 状态行不受影响：仍如实写「未接管」（managed 只留给文案/色）
    const auto* value = ui.find("settings.card.proxy.takeover.value");
    DOCTEST_REQUIRE(value != nullptr);
    DOCTEST_CHECK(value->text.find("未接管") != std::string::npos);

    svc.stopProxy();
    mod->onStop();
    module::claude::ClaudeService::setGlobal(nullptr);
    test_support::setEnvVar("DEVDESK_CONFIG_PATH", "");
    platform::keychain::deleteSecret(
        "DevDeskTestWarnUnmanaged", "claude:" + std::to_string(pr.value().id));
    fs::remove_all(dir, ec);
    return true;
})

// ---- 窗口效果三档（磨砂 Phase D）----

REGISTER_TEST(settings_window_effect_defaults_and_roundtrip, "settings: windowEffect 默认 off + 三档读写往返", {
    const auto path = test_support::tempConfigFile(kCfgPrefix, "wineffect");
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
    for (const char* mode : {module::settings::kWindowEffectOff,
                             module::settings::kWindowEffectTranslucent,
                             module::settings::kWindowEffectFrosted}) {
        module::settings::SettingsConfig cfg;
        cfg.windowEffect = mode;
        DOCTEST_CHECK(store.save(cfg).isOk() &&
                      store.load().windowEffect == std::string(mode));
    }

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

    // 两处 alpha 各自独立（2026-09-29 二轮黑壁纸标定）：
    //   clearColor（内容区纱）：off=1.0 / translucent=0.60 / frosted=0.15 / 未知→1.0
    //   窗口背景色（标题栏底）：off=1.0 / translucent=0.90 / frosted=0.65 / 未知→1.0
    // 回退任一常量本用例即红（旧值：两处同为 0.94 / 同为 0.30，即 EUI-NEO 按 clearColor
    // 推导标题栏底的年代——那正是 P1「半透等同关闭」与 P2「标题栏与内容区差 105」的成因）
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectOff) == 1.0f);
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectTranslucent) == 0.60f);
    DOCTEST_CHECK(ms::windowEffectAlpha(ms::kWindowEffectFrosted) == 0.15f);
    DOCTEST_CHECK(ms::windowEffectAlpha("bogus") == 1.0f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectOff) == 1.0f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectTranslucent) == 0.90f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) == 0.65f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha("bogus") == 1.0f);

    // 半透档目标带（用户第三轮反馈「半透的透明度几乎没了」后重标）：**240 上下一档**。
    // 带的两端都有来历，不是随手取的：
    //   下界 236 —— 用户第 4 次反馈「看不清内容」时实测 236.3，低于此即重蹈覆辙；
    //   上界 244 —— 第 5 次反馈「几乎没了」时实测 246.25，高于此即与关闭档无差。
    // 故 0.60/0.90 → 250×(1−0.10×0.40) = 240.0，落在带中央。
    // **这条同时锁常量与目标**：回退任一 alpha（如 0.60/0.90 → 0.70/0.95）→ 246.25 出上界变红；
    // 回退到首轮 0.45/0.90 → 236.25 出下界变红；回退到「两层同 0.94」→ 249.1 变红。
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

    // 磨砂档判据（同一判据链，材质输出 M 用实机标定值 192@titleAlpha=0.65）：
    //   内容区 = M + contentAlpha×(250−M)，标题栏 = M → 差值 = contentAlpha×(250−M) ≤ 20
    // 旧值 0.30 与「标题栏没有材质」两件事都会被这两条拆开判：差值式在 M 恒定时只惩罚
    // 内容区纱，故另加「标题栏 alpha 必须落在能把材质抬到浅灰的区间」一条。
    constexpr float kFrostedMaterialOut = 192.0f;  // 实机实测（黑壁纸，titleAlpha=0.65）
    const float frostedVeil = ms::windowEffectAlpha(ms::kWindowEffectFrosted);
    DOCTEST_CHECK(frostedVeil * (250.0f - kFrostedMaterialOut) <= 20.0f);
    DOCTEST_CHECK(ms::windowEffectTitleBarAlpha(ms::kWindowEffectFrosted) >= 0.6f);
    // 磨砂必须比半透更透（用户原话「半透……明显比磨砂档实」）：两处 alpha 都要更小
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
    // 标题栏底**独立** alpha（显式 customColor 覆盖 EUI-NEO 的按 clearColor 推导）
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

// ============ 模块插拔（方案 A）：模块管理卡 + 卡片门控 ============

REGISTER_TEST(settings_modules_card_renders, "settings: 模块管理卡渲染（3 个工具模块开关）", {
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
    // 描述文案含重启提示（设计 §5.4）
    const auto* desc = ui.find("settings.card.modules.desc");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_CHECK(desc->text.find("重启") != std::string::npos);
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
    DOCTEST_REQUIRE(module::settings::SettingsStore(cfgEnv.cfgPath).save(cfg).isOk());

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
    DOCTEST_REQUIRE(module::settings::SettingsStore(cfgEnv.cfgPath).save(cfg).isOk());

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
    test_support::TestEnv env(kCfgPrefix, "petz", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "petz_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
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

// ---- 2026-09-29 用户真机反馈「设置-日志：日志和日志级别这 2 个条件 错位了啊
//      日志显示 2 行」----
// 判据全部数值化（改前逐条必红）：
//   ① 两个下拉同列：field.frame.x 相等（改前 28 vs 124，差 96px）、同宽同高；
//   ② 标签同列 + 标签→下拉间距一致（改前行 1 根本没有标签列，本用例
//      REQUIRE("settings.card.log.label") 即回归位）；
//   ③ 标签单行：标签框高 ≤ 单行行高、文本无换行符、估算文本宽 ≤ 标签框宽
//      （「日志保留」4 字 × 12px = 48 ≤ 84，折行根本装不下 → 恒单行）。
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
    DOCTEST_REQUIRE(label1 != nullptr);  // 改前缺失 → 本行即回归位
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

    // 卡内不与标题重叠、两行自上而下不叠，说明列在控件列右侧，
    // 且内容底沿仍留 ≥16 下边距（卡高 148 不变的前提）
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

// ---- 文案宽度纯件（③ 判据的来源）：估算 / 单行适配 / 皮肤下拉宽度 ----
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

    // 皮肤下拉宽度：下限 240（同其余控件）/ 按最长名放宽 / 上限 420
    DOCTEST_CHECK(std::abs(ms::petSkinFieldWidth({}, 16.0f) - 240.0f) < 1e-3f);
    DOCTEST_CHECK(std::abs(ms::petSkinFieldWidth({"robot"}, 16.0f) - 240.0f) <
                  1e-3f);
    DOCTEST_CHECK(std::abs(ms::petSkinFieldWidth({kLong}, 16.0f) -
                           (25.0f * 0.62f * 16.0f + 62.0f)) < 1e-3f);
    DOCTEST_CHECK(std::abs(ms::petSkinFieldWidth({std::string(80, 'x')}, 16.0f) -
                           420.0f) < 1e-3f);
    return true;
})

// ---- 2026-09-29 用户真机反馈「桌宠设置的下拉框不应该换行」----
// EUI-NEO 的 text 默认 wrap=false（core/dsl.h:149）：超宽皮肤名**不折行**，
// 而是画出控件右边缘（真机复现：25 字符名在 240 宽下拉里文本压过 chevron）。
// 判据（改回固定 240 宽 / 去掉截断都会红）：
//   ① 字段宽度 ≥ 最长皮肤名估算宽 + 字段 chrome（且 ≤ 420 上限）；
//   ② 字段文案与每个选项的估算宽 ≤ 各自可用宽（放不下 = 溢出）；
//   ③ 文案无换行符；短名原样显示、清单项数不减（截断只碰显示串，不改取值）。
REGISTER_TEST(settings_pet_skin_dropdown_label_fits,
              "settings: 桌宠皮肤下拉单行不溢出（按最长名放宽 + 超长截断）", {
    test_support::TestEnv env(kCfgPrefix, "petskinfit", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "petskinfit_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":["
               "\"home\",\"markdown\",\"todo\",\"claude_proxy\",\"settings\","
               "\"pet\"]}";
    }
    test_support::TempDir petsDir("pet_skins_fit");
    const std::string kLong = "super-long-skin-name-here";  // 25 字符
    for (const std::string& s : {std::string("robot"), kLong}) {
        std::filesystem::create_directories(petsDir.path / s);
        std::ofstream(petsDir.path / s / "pet.json") << "{}";
    }
    test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", petsDir.path.string());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));  // 模块 tab

    // 探针：compose 一帧后把下拉几何 / 文案拷出来（Ui 是局部的，指针不外带）
    struct Snapshot {
        bool ok = false;
        float width = 0.0f;
        std::string label;
        float labelFont = 0.0f;
        std::vector<std::string> items;
        float itemFont = 0.0f;
    };
    auto snapshot = [&]() {
        Snapshot snap;
        eui::Ui ui;
        ui.begin("petskinfit");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const auto* field = ui.find("settings.card.pet.skin.dd.field");
        const auto* label = ui.find("settings.card.pet.skin.dd.label");
        if (field == nullptr || label == nullptr) return snap;
        snap.ok = true;
        snap.width = field->frame.width;
        snap.label = label->text;
        snap.labelFont = label->fontSize;
        for (size_t i = 0;; ++i) {
            const auto* item = ui.find(
                "settings.card.pet.skin.dd.item.label." + std::to_string(i));
            if (item == nullptr) break;
            snap.items.push_back(item->text);
            snap.itemFont = item->fontSize;
        }
        return snap;
    };
    namespace ms = module::settings;
    const float kFieldFont = 16.0f;   // components/theme.h typography.body
    const float kChrome = 62.0f;      // content(12) + micro(2) + overlay(48)
    const float kItemChrome = 36.0f;  // small(6)×2 + panel(24)

    // ① 按最长皮肤名放宽（固定 240 = 改前状态 → 本行红）
    const float longEst = ms::estimatedTextWidth(kLong, kFieldFont);
    DOCTEST_CHECK(std::abs(longEst - 248.0f) < 0.5f);  // 25 × 0.62 × 16
    const Snapshot wide = snapshot();
    DOCTEST_REQUIRE(wide.ok);
    DOCTEST_CHECK_MESSAGE(wide.width >= longEst + kChrome - 0.5f,
                          "皮肤下拉宽 " + std::to_string(wide.width) + "（应 ≥ " +
                              std::to_string(longEst + kChrome) + "）");
    DOCTEST_CHECK(wide.width <= 420.5f);

    // ② 字段文案 + 每个选项都放得进各自可用宽（溢出 = 压 chevron 的根因）
    DOCTEST_CHECK(wide.label.find('\n') == std::string::npos);
    DOCTEST_CHECK_MESSAGE(
        ms::estimatedTextWidth(wide.label, wide.labelFont) <=
            wide.width - kChrome + 0.5f,
        "字段文案 " + wide.label + " 估算宽 " +
            std::to_string(ms::estimatedTextWidth(wide.label, wide.labelFont)));
    for (const std::string& t : wide.items) {
        DOCTEST_CHECK(t.find('\n') == std::string::npos);
        DOCTEST_CHECK_MESSAGE(
            ms::estimatedTextWidth(t, wide.itemFont) <=
                wide.width - kItemChrome + 0.5f,
            "选项 " + t + " 溢出（控件宽 " + std::to_string(wide.width) + "）");
    }
    // ③ 短名原样显示（截断只碰显示串）+ 清单项数不减
    DOCTEST_CHECK_MESSAGE(
        std::find(wide.items.begin(), wide.items.end(), std::string("robot")) !=
            wide.items.end(),
        "短皮肤名应原样显示");
    DOCTEST_CHECK_EQ(wide.items.size(), 2u);
    DOCTEST_CHECK(wide.label == "robot");  // 选中项 = 默认皮肤

    // ④ 超长名（估算宽破上限）→ 控件封顶 420、文案截断（'…'）且仍放得下
    const std::string kHuge =
        "skin-with-a-very-long-name-that-exceeds-the-cap-limit-for-sure";
    std::filesystem::create_directories(petsDir.path / kHuge);
    std::ofstream(petsDir.path / kHuge / "pet.json") << "{}";
    {
        // 当前皮肤 = 超长名（pet_config.json 是 currentSkinName 的真值来源）
        std::ofstream out(petsDir.path / "pet_config.json");
        out << "{\"skinName\":\"" << kHuge << "\",\"x\":-1,\"y\":-1}";
    }
    mod->onPageEnter();  // 皮肤清单 / 当前皮肤的刷新点（THS-01）
    const Snapshot huge = snapshot();
    DOCTEST_REQUIRE(huge.ok);
    DOCTEST_CHECK(std::abs(huge.width - 420.0f) < 0.5f);  // 上限封顶
    DOCTEST_CHECK_EQ(huge.items.size(), 3u);              // 清单没丢项
    DOCTEST_CHECK(huge.label.size() >= 4u);
    DOCTEST_CHECK_MESSAGE(
        huge.label.compare(huge.label.size() - 3u, 3u, "\xE2\x80\xA6") == 0,
        "超长皮肤名应截断显示：" + huge.label);
    DOCTEST_CHECK_MESSAGE(
        ms::estimatedTextWidth(huge.label, huge.labelFont) <=
            huge.width - kChrome + 0.5f,
        "截断后仍溢出：" +
            std::to_string(ms::estimatedTextWidth(huge.label, huge.labelFont)));
    // 截断串 = 原名前缀（只改显示，取值仍是原下标 → onChange 语义不变）
    DOCTEST_CHECK(kHuge.compare(0, huge.label.size() - 3u,
                                huge.label.substr(0, huge.label.size() - 3u)) ==
                  0);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 2026-09-29 用户真机反馈（第二次，上一轮只改了宽度）
//      「设置-模块-桌宠 下拉框不应该另起一行 这个没修」----
// 模块 tab 的桌宠卡（drawPetCard）此前**没有标签列**：皮肤下拉在卡内 (28,64)
// 独占一行，读起来就是「下拉另起一行」。修复 = 补「皮肤」标签列（28/72）+
// 控件列（104/40），标签左、控件右、同一基线。判据全数值化，改前逐条必红：
//   ① 标签 settings.card.pet.skin.label 存在（改前无此元素 → REQUIRE 即红）；
//   ② 标签框中心 y == 下拉字段框中心 y（±0.5，同一基线）；
//   ③ 下拉字段框顶 y ≤ 标签框底（证明控件没被折到标签下面 = 没另起一行）；
//   ④ 标签在控件左侧、水平不重叠（标签右缘 ≤ 控件左缘）。
REGISTER_TEST(settings_pet_skin_row_inline,
              "settings: 桌宠卡皮肤下拉与标签同行（不另起一行）", {
    test_support::TestEnv env(kCfgPrefix, "petrow", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "petrow_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":[\"home\",\"markdown\","
               "\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));  // 模块 tab
    eui::Ui ui;
    ui.begin("petrow");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* card = ui.find("settings.card.pet");
    const auto* label = ui.find("settings.card.pet.skin.label");
    const auto* field = ui.find("settings.card.pet.skin.dd.field");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(label != nullptr);  // 改前无标签列 → 本行即回归位
    DOCTEST_REQUIRE(field != nullptr);
    DOCTEST_CHECK(std::string(label->text) == "皮肤");

    // ② 同一基线：两框中心 y 相等（标签框高 20、控件高 40，靠 y 偏置对齐）
    const float labelCy = label->frame.y + label->frame.height * 0.5f;
    const float fieldCy = field->frame.y + field->frame.height * 0.5f;
    DOCTEST_CHECK_MESSAGE(std::abs(labelCy - fieldCy) < 0.5f,
                          "标签中心 y " + std::to_string(labelCy) +
                              " vs 下拉中心 y " + std::to_string(fieldCy));

    // ③ 没另起一行：控件顶不超过标签底（改回「标签在上、控件在下一行」→ 红）
    DOCTEST_CHECK_MESSAGE(field->frame.y <= label->frame.y + label->frame.height,
                          "下拉 frame.y " + std::to_string(field->frame.y) +
                              " > 标签底 " +
                              std::to_string(label->frame.y + label->frame.height) +
                              "（折到下一行了）");

    // ④ 标签左、控件右，水平不重叠
    DOCTEST_CHECK(label->frame.x < field->frame.x);
    DOCTEST_CHECK(label->frame.x + label->frame.width <= field->frame.x);
    // 同排且行仍在卡内（卡高 168、内容底沿 152——2026-09-30 加显示大小行
    // 后由 120/104 抬高，审计 F4 注释对齐）
    DOCTEST_CHECK(field->frame.y + field->frame.height <=
                  card->frame.y + card->frame.height + 0.5f);
    DOCTEST_CHECK(label->frame.y > card->frame.y + 38.0f);  // 不与标题/说明重叠

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 2026-09-29 用户真机反馈「设置-模块-桌宠取消勾选，桌宠没消失」----
// 口径（PET-CLOSE）：模块开关的**注册门控**仍要重启（main 装配期），但桌宠
// 是唯一有独立可见载体的模块 → 取消勾选立即关窗、重新勾选立即重开（本会话
// 装配过时）。判据（去掉 settings 侧的桥调用逐条必红）：
//   ① 点开关命中矩形（settings.card.modules.sw.3.hit，勾选态）→ pet 侧
//      「窗口可见意图」由 true 翻 false（= 取消勾选即关窗，不是等重启）；
//   ② enabledModules 持久化不受影响：盘上 config.json 去掉 pet（插拔语义不变）；
//   ③ 卡片门控即时预览不变：pet 卡随开关消失；
//   ④ 重新勾选 → 意图回 true **且真的重新入队一条 pet 开窗请求**（窗口能再出现）。
REGISTER_TEST(settings_pet_module_toggle_closes_window,
              "settings: 取消勾选桌宠立即关窗 / 重新勾选立即重现（PET-CLOSE）", {
    test_support::TestEnv env(kCfgPrefix, "petclose", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "petclose_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":[\"home\",\"markdown\","
               "\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
    }
    // 皮肤夹具：DEVDESK_PETS_DIR 指临时目录并写入 shipped 形态皮肤（帧只需
    // PNG 头合法，无头不解码）。不造夹具时测试环境 exeDir/build/tests 没有
    // 随包资产 → ensureDefaultSkin 失败 → startPetWindow 根本不开窗
    test_support::TempDir petsDir("pet_close_pets");
    test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", petsDir.path.string());
    static const char* kSkinJson =
        "{\"name\":\"robot\",\"frameSize\":[128,128],\"frameRate\":4,"
        "\"states\":{\"idle\":[\"f0.png\",\"f1.png\"],\"poked\":[\"p0.png\"]}}";
    test_support::writeShippedSkinFixture(petsDir.path, "robot", kSkinJson);

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));  // 模块 tab

    // 前置：本会话已装配桌宠（= 启动时 enabledModules 含 pet 的会话状态）
    (void)app::consumeWindowRequests();
    module::pet::startPetWindow(module::pet::PetWindowConfig{});
    {
        // 装配必须真的入队开窗请求（皮肤夹具失效时在此先红，不把「关了窗」
        // 的假绿带到下面）
        const std::vector<app::DslWindowRequest> initial =
            app::consumeWindowRequests();
        DOCTEST_REQUIRE_MESSAGE(initial.size() == 1u,
                                "装配未入队开窗请求（皮肤夹具不可用？实测 " +
                                    std::to_string(initial.size()) + " 条）");
        DOCTEST_REQUIRE_EQ(initial[0].pageId, "pet");
    }
    DOCTEST_REQUIRE(module::pet::petWindowRequested());  // 装配 = 想显示

    eui::Ui ui;
    auto composeModules = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    composeModules("petclose1");
    // 模块卡第 4 行 = 桌宠（kToggleable 序：markdown/todo/claude/pet）；
    // 命中矩形 onClick 按 compose 期勾选态取反 → 本次点击 = 取消勾选
    const auto* offHit = ui.find("settings.card.modules.sw.3.hit");
    DOCTEST_REQUIRE(offHit != nullptr);
    DOCTEST_REQUIRE(offHit->onClick != nullptr);
    DOCTEST_REQUIRE(ui.find("settings.card.pet") != nullptr);  // 取消前卡在
    offHit->onClick();

    // ① 取消勾选 → 立即关窗（改前这里恒 true：真机现象「桌宠没消失」）
    DOCTEST_CHECK_MESSAGE(
        !module::pet::petWindowRequested(),
        "取消勾选后桌宠仍处于「应显示」状态（窗口没被关）");
    // ② 持久化：盘上 enabledModules 去掉 pet
    {
        std::ifstream in(cfgEnv.cfgPath);
        std::ostringstream buf;
        buf << in.rdbuf();
        DOCTEST_CHECK_MESSAGE(buf.str().find("\"pet\"") == std::string::npos,
                              "config.json 仍含 pet：" + buf.str());
    }
    // ③ 卡片门控即时预览不变（关掉的模块整卡隐藏）
    composeModules("petclose2");
    DOCTEST_CHECK(ui.find("settings.card.pet") == nullptr);
    const auto* onHit = ui.find("settings.card.modules.sw.3.hit");
    DOCTEST_REQUIRE(onHit != nullptr);  // 开关行恒在（不受门控影响）
    onHit->onClick();                   // 本次点击 = 重新勾选

    // ④ 重新勾选 → 立即重现：意图回 true + 真的重新入队一条 pet 开窗请求
    DOCTEST_CHECK_MESSAGE(module::pet::petWindowRequested(),
                          "重新勾选后桌宠没有回到「应显示」状态");
    {
        const std::vector<app::DslWindowRequest> reqs =
            app::consumeWindowRequests();
        DOCTEST_CHECK_MESSAGE(reqs.size() == 1u,
                              "重新勾选应重新入队 1 条开窗请求，实测 " +
                                  std::to_string(reqs.size()));
        if (reqs.size() == 1u) {
            DOCTEST_CHECK_EQ(reqs[0].pageId, "pet");
        }
    }
    {
        std::ifstream in(cfgEnv.cfgPath);
        std::ostringstream buf;
        buf << in.rdbuf();
        DOCTEST_CHECK(buf.str().find("\"pet\"") != std::string::npos);
    }

    // 收尾：不留残留可见意图 / 待消费开窗请求（进程级 pet 运行时是共享状态）
    module::pet::closePetWindow();
    (void)app::consumeWindowRequests();
    mod->onStop();
    ::core::theme::setDark();
    return true;
})

REGISTER_TEST(settings_pet_skin_dropdown_openup,
              "settings: 桌宠皮肤下拉 openUp——小窗 3 皮肤弹层留在窗内", {
    test_support::TestEnv env(kCfgPrefix, "petopenup", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "petopenup_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":["
               "\"home\",\"markdown\",\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
    }
    // 隔离皮肤清单：DEVDESK_PETS_DIR 指向临时目录，造 3 个皮肤（discoverSkins
    // 只验 pet.json 存在；TestEnv 已隔离 APPDATA，默认 0 皮肤 → 占位单项）
    test_support::TempDir petsDir("pet_skins");
    const char* skins[] = {"robot", "cat", "tatsu"};
    for (const char* s : skins) {
        std::filesystem::create_directories(petsDir.path / s);
        std::ofstream out(petsDir.path / s / "pet.json");
        out << "{}";
    }
    test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", petsDir.path.string());

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    // 小窗 1200×720（最小窗）：pet 卡为模块 tab 第 2 卡（卡顶 ≈ 480），
    // 字段底 584 + 8 gap + 弹层 114 = 706 > 704（屏高-16）→ openUp 向上，
    // 弹层整体留在窗体内且位于字段上方（修复前被 scrollView clip 裁掉）
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 720.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
    eui::Ui ui;
    ui.begin("petopenup_small");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* field = ui.find("settings.card.pet.skin.dd.field");
    const auto* popup = ui.find("settings.card.pet.skin.dd.popup");
    DOCTEST_REQUIRE(field != nullptr);
    DOCTEST_REQUIRE(popup != nullptr);
    DOCTEST_CHECK(popup->frame.y >= 0.0f);
    DOCTEST_CHECK(popup->frame.y + popup->frame.height <= screen.height);
    DOCTEST_CHECK(popup->frame.y + popup->frame.height <= field->frame.y);

    // 高窗 1600×1000：字段底 584 + 8 + 114 = 706 ≤ 984 → 保持默认向下展开
    eui::Screen tall;
    tall.width = 1600.0f;
    tall.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, tall, 1));
    ui.begin("petopenup_tall");
    mod->compose(ui, tall);
    ui.end();
    ui.layout(tall.width, tall.height);
    const auto* tallField = ui.find("settings.card.pet.skin.dd.field");
    const auto* tallPopup = ui.find("settings.card.pet.skin.dd.popup");
    DOCTEST_REQUIRE(tallField != nullptr);
    DOCTEST_REQUIRE(tallPopup != nullptr);
    DOCTEST_CHECK(tallPopup->frame.y >=
                  tallField->frame.y + tallField->frame.height);
    DOCTEST_CHECK(tallPopup->frame.y + tallPopup->frame.height <= tall.height);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 外观卡下拉弹层 vs 后续兄弟卡（回归 1379cc1）：外观卡(z30) 与紧随其下的
// 编辑器字号卡(z30) 同为 z30 → 稳定排序保持 DOM 序 → 后绘的编辑器字号卡盖住
// 外观卡向下铺开的弹层（「展开被下方内容截断」）。EUI-NEO 子树 zIndex 上界
// 逃逸（0485b47）只解决「高 z 盖低 z」，等值 z 兄弟按 DOM 后绘是框架语义——
// 修复 = 外观卡抬到 40（表内最前带下拉卡拿最高 z，见 drawAppearanceCard）。----
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

    // 弹层底边越出外观卡下沿 → 视觉上落入紧随其后的编辑器字号卡区域
    DOCTEST_CHECK(popup->frame.y + popup->frame.height >
                  appearanceCard->frame.y + appearanceCard->frame.height);

    // 绘制序 = orderedChildren 顺序（后者 = 上层）。弹层深嵌外观卡子树、
    // 与编辑器字号卡没有共同直接父——按「整卡抬层」语义（subtreeMaxZIndex
    // 让含弹层卡片整卡作为一个绘制单元与兄弟卡竞争），断言两卡在滚动内容列
    // 里的兄弟序：外观卡必须排在编辑器字号卡之后绘制，否则弹层下半被该卡
    // 盖住（等值 z 时稳定排序保持 DOM 序 → 1379cc1 起后者盖前者）
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

// ---- PAB-13（波 1 L7）：桌宠卡「显示桌宠」按钮 ----
// 无头渲染断言：按钮在「位置重置」左侧同排（间距 8）、文案/点击回调齐备。
// 卡高保持 120（内容底沿 104 不变 → contentKey 不变，不触发滚动高度重测）
REGISTER_TEST(settings_pet_show_button_renders,
              "settings: 桌宠卡「显示桌宠」按钮渲染（PAB-13）", {
    test_support::TestEnv env(kCfgPrefix, "petshow", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "petshow_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":["
               "\"home\",\"markdown\",\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));  // pet 卡在「模块」tab
    eui::Ui ui;
    ui.begin("petshow");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);  // scrollView 内容在 layout 期展开

    const auto* show = ui.find("settings.card.pet.show");
    const auto* reset = ui.find("settings.card.pet.reset");
    const auto* label = ui.find("settings.card.pet.show.text");
    const auto* hit = ui.find("settings.card.pet.show.bg");
    DOCTEST_REQUIRE(show != nullptr);
    DOCTEST_REQUIRE(reset != nullptr);
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("显示桌宠"));
    DOCTEST_CHECK(hit->onClick != nullptr);
    // 同排 y=64，居「位置重置」左侧：160 宽按钮 + 8 间隙 = 168 的 x 差
    DOCTEST_CHECK(show->hasX);
    DOCTEST_CHECK(show->hasY);
    DOCTEST_CHECK_EQ(show->y, 64.0f);
    DOCTEST_CHECK_EQ(reset->y, 64.0f);
    DOCTEST_CHECK_EQ(reset->x - show->x, 168.0f);

    mod->onStop();
    ::core::theme::setDark();
    return true;
})

// ---- 审计 [21]：代理端口输入 Esc=丢弃草稿（旧映射 Esc→onEnter 会应用端口并重启代理）----

REGISTER_TEST(settings_port_esc_discards_draft,
              "settings: 端口草稿 Esc=丢弃回落已应用端口，不触发应用（headless，审计 [21]）", {
    test_support::TestEnv env(kCfgPrefix, "portesc", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "portesc_cfg"));
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

    // 未落盘：配置仍是默认 8888（旧行为 Esc=applyProxyPort 会写 9999 并重启代理）
    module::settings::SettingsStore store(cfgEnv.cfgPath);
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
    test_support::TestEnv env(kCfgPrefix, "ctrls", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "ctrls_cfg"));
    module::claude::ClaudeService svc(env.db->db(), "DevDeskTestCtrlS");
    module::claude::ClaudeService::setGlobal(&svc);
    ScopedClaudeProxy proxyForSettings;  // R0/D6：设置页经定位器取代理运行面
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
    // Ctrl+S（修饰位按平台取：macOS = Cmd，其余 = Ctrl；见 setShortcutModifier）
    auto ctrlS = [](eui::KeyAction action) {
        eui::KeyEvent e;
        e.key = eui::InputKey::S;
        e.action = action;
        test_support::setShortcutModifier(e.modifiers);
        return e;
    };

    // 1) 无草稿：Ctrl+S → 配置落盘（onStart 只读不写，此处才出现文件）
    //    + 状态行回显「设置已保存」
    DOCTEST_CHECK(!std::filesystem::exists(cfgEnv.cfgPath));
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));
    DOCTEST_CHECK(std::filesystem::exists(cfgEnv.cfgPath));
    frame("css1");
    {
        const auto* sub = ui.find("settings.subtitle");
        DOCTEST_REQUIRE(sub != nullptr);
        DOCTEST_CHECK(sub->text.find("设置已保存") != std::string::npos);
    }
    {
        module::settings::SettingsStore store(cfgEnv.cfgPath);
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
        test_support::setShortcutModifier(selectAll.modifiers);
        input->onKeyEvent(selectAll);           // Ctrl+A 全选
        eui::TextInputEvent t;
        t.text = "9001";
        input->onTextInput(t);                  // 替换 → draft = "9001"
    }
    frame("css3");
    DOCTEST_CHECK(mod->onKeyEvent(ctrlS(eui::KeyAction::Press)));
    {
        module::settings::SettingsStore store(cfgEnv.cfgPath);
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "thememode");
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
    test_support::TestEnv env(kCfgPrefix, "thauto", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "thauto_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "edfont");
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

    // Task B tab 化：编辑器字号自日志卡迁到「通用」tab 独立卡（默认 tab，无需切）
    DOCTEST_CHECK(ui.find("settings.card.editor.dd") != nullptr);
    // 点第 3 档「大 17px」→ 通道即时生效 + 持久化
    const auto* item = ui.find("settings.card.editor.dd.item.2");
    DOCTEST_REQUIRE(item != nullptr);
    DOCTEST_REQUIRE(item->onClick != nullptr);
    item->onClick();
    DOCTEST_CHECK_EQ(::core::theme::editorFontSize(), 17.0f);
    module::settings::SettingsStore store(cfgEnv.cfgPath);
    DOCTEST_CHECK_EQ(store.load().editorFontSize, 17);

    mod->onStop();
    ::core::theme::setEditorFontSize(15.0f);  // 恢复通道默认
    ::core::theme::setDark();  // onStart 的 auto 解析在本机可能 setLight，防串扰
    return true;
})

// ---- spec 缩水 C：关于文案纯函数（版本 / EUI-NEO rev / 构建信息，CMake 注入
// 宏）。设置页关于卡已于 2026-09-28 拆成独立模块 module_about（关于 tab 删
// 除，卡渲染测试随之删除）；settings_config.cpp 的 about* 函数暂留死代码
//（D-02 明确不属该需求范围），以下纯函数测试保留防宏注入回归 ----

// 版本断言与真值同源的前提：本 TU 必须拿到与 devdesk_module_settings **同一**
// PROJECT_VERSION 注入的 DEVDESK_VERSION 宏（tests/CMakeLists.txt 的
// target_compile_definitions）。缺了就是编译期硬失败，不让下方断言悄悄退化成
// 「只查应用名」的假绿。
#ifndef DEVDESK_VERSION
#error "devdesk_tests 缺 DEVDESK_VERSION 宏注入（见 tests/CMakeLists.txt）"
#endif

REGISTER_TEST(settings_about_text_helpers,
              "settings: aboutVersionLine/aboutEuiNeoLine 纯函数（spec缩水 C）", {
    // 2026-09-29 修硬编码版本断言：原写法 DOCTEST_CHECK(v.find("DevDesk 0.1.")
    // != npos) 把版本号写死成写用例当时的 VERSION，真值涨到 0.7.x 后恒红
    // （且无法再反映真值）。现在期望版本段取自 DEVDESK_VERSION 宏——与
    // settings_config.cpp 构造 aboutVersionLine() 用的是同一个宏、同一个
    // PROJECT_VERSION 来源，VERSION 文件变则两边同步变。
    const std::string expectPrefix = std::string("DevDesk ") + DEVDESK_VERSION;
    const std::string v = module::settings::aboutVersionLine();
    DOCTEST_CHECK_MESSAGE(v.rfind(expectPrefix, 0) == 0,
                          "actual=[" + v + "] expected_prefix=[" + expectPrefix + "]");
    // 格式不变式（不依赖具体版本值）：
    // "DevDesk <版本> · <构建类型> · 构建 <日期> <时刻>"
    DOCTEST_CHECK(v.rfind(expectPrefix + " · ", 0) == 0);
    DOCTEST_CHECK(v.find(" · 构建 ") != std::string::npos);
    // 原 v.find("dev") == npos 只是「宏注入生效」的启发式，且会被 "0.8.0-dev"
    // 这类合法版本误伤；前缀比对已直接证明注入生效（回退分支产出
    // "DevDesk dev · …" 即红），故删。
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
    const auto path = test_support::tempConfigFile(kCfgPrefix, "loglevel");
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
    test_support::TestEnv env(kCfgPrefix, "loglvl", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "loglvl_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
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
    module::settings::SettingsStore store(cfgEnv.cfgPath);
    DOCTEST_CHECK_EQ(store.load().logLevel,
                     std::string(module::settings::kLogLevelDebug));

    mod->onStop();
    core::log::setLevel(prev);  // 恢复（测试进程默认 Info）
    ::core::theme::setDark();   // onStart 的 auto 解析可能 setLight，防串扰
    return true;
})

// ---- 审计 H2：日志 tail 读盘在后台线程，compose 只读快照 ----
// 回归：此前 compose 顶部同步读 256KB 日志文件（在渲染帧里与代理 worker 的
// DB 写抢同一把锁）。现在切 tab / 点刷新只置脏标记 → 发一次读意图
// （std::async 后台线程），结果下一帧收割；快照恒非空沿用上次值（不闪空、不丢行）。
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
    test_support::TestEnv env(kCfgPrefix, "ths01", true);
    test_support::ScopedConfigPath cfgEnv(test_support::tempConfigFile(kCfgPrefix, "ths01_cfg"));
    // 模块插拔门控：目录卡 + 桌宠卡都要渲染（pet 不在默认启用集合里）
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdPet};
    DOCTEST_REQUIRE(module::settings::SettingsStore(cfgEnv.cfgPath).save(cfg).isOk());

    const auto notesDir = test_temp::tempPath("devdesk_ths01_notes");
    const auto petsDir = test_temp::tempPath("devdesk_ths01_pets");
    std::error_code ec;
    std::filesystem::remove_all(notesDir, ec);
    std::filesystem::remove_all(petsDir, ec);
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", notesDir.string());
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", petsDir.string());

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

// ---- 审计 THS-05：下拉「谁打开着」的唯一持有者（7 个 bool → 1 个索引）----
// 断言：① 打开第二个下拉会自动关掉先前那个（同时最多一个展开）；
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

    // 0) 初始全关；无下拉时 Esc 不消费
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

// ==================== R0 / 审计 D6・D7 护栏用例 ====================

REGISTER_TEST(settings_no_direct_claude_service_coupling,
              "settings: R0/D6 —— 设置页零 ClaudeService 直连（改经 core 只读服务定位器）", {
                  const std::string report = r0CouplingGuardReport();
                  DOCTEST_CHECK_MESSAGE(report.empty(), report);
                  return true;
              })

REGISTER_TEST(settings_proxy_port_single_source,
              "settings: R0/D7 —— 代理端口常量单一来源 core/proxy_port.h", {
                  // 1) 取值钉死（R0 是唯一改链接期契约的重构：两侧仍须 8888/1/65535）
                  DOCTEST_CHECK_EQ(::core::kDefaultProxyPort, 8888);
                  DOCTEST_CHECK_EQ(::core::kMinProxyPort, 1);
                  DOCTEST_CHECK_EQ(::core::kMaxProxyPort, 65535);
                  // 2) 两个消费方都跟随 core（改值只有一个落点，不会单边漂移）
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
                  // 3) 源码级：字面量只许在 core/proxy_port.h 定义（防再次各自硬编码）
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

                  // 适配器「空单例」语义（未 setGlobal / claude 模块关闭）：与解耦前
                  // `auto* svc = ClaudeService::global(); if (svc) {...}` 的降级逐字一致
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

// ==================== R6：settings_module 拆分 + 卡片表驱动 ====================
// spec 2026-09-27-refactor-incremental-design §5.2 要求 R6 零行为变化。以下两
// 条用例是「拆出的纯件 / 注册表」与拆分前逐字等价的护栏（N9 允许新增纯函数
// 等价断言）：纯件断言逐点复刻拆分前的内联式；注册表断言 tab 归属、绘制顺序
// 与模块门控（拆分前是 compose 内 switch 的 9 个分支）。

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
        for (float off : {ms::kLogDropdownOffsetY, ms::kEditorFontDdOffsetY,
                          ms::kLogLevelOffsetY}) {
            for (float fh : {ms::kLogDropdownHeight, ms::kLogEditorFontHeight,
                             ms::kLogLevelHeight}) {
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
    DOCTEST_CHECK_EQ(gridHits, 5 * 3 * 3 * 3 * 4);

    // 2) 弹层几何 / 卡片高度常量取值（拆分前后同源，值不得漂）
    DOCTEST_CHECK_EQ(ms::kDropdownItemHeight, 34.0f);
    DOCTEST_CHECK_EQ(ms::kDropdownPopupPadding, 12.0f);
    DOCTEST_CHECK_EQ(ms::kDropdownPopupGap, 8.0f);
    DOCTEST_CHECK_EQ(ms::kWindowBottomMargin, 16.0f);
    DOCTEST_CHECK_EQ(ms::kSettingsTabHeight, 40.0f);
    DOCTEST_CHECK_EQ(ms::kAppearanceCardHeight, 172.0f);
    DOCTEST_CHECK_EQ(ms::kEditorFontCardHeight, 108.0f);
    DOCTEST_CHECK_EQ(ms::kModulesCardHeight, 288.0f);
    DOCTEST_CHECK_EQ(ms::kPetCardHeight, 168.0f);  // 两行控件（皮肤+显示大小）
    DOCTEST_CHECK_EQ(ms::kDirectoryCardHeight, 116.0f);
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

    // 4) nearestIndex：命中 / 最近 / 越界回落（拆分前同实现）
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

    // 7) 下拉唯一持有者（THS-05）：开 = 独占；关 = 只关自己（迟到回调不误关）
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
            std::ofstream out(cfgEnv.cfgPath);
            out << "{\"theme\":\"dark\",\"enabledModules\":[\"home\","
                   "\"markdown\",\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
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
            "settings.card.pet",
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
            std::ofstream out(cfgEnv.cfgPath);
            out << "{\"theme\":\"dark\",\"enabledModules\":[\"home\","
                   "\"settings\"]}";
        }
        std::unique_ptr<::core::Module> mod(create_settings_module());
        mod->onStart();
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));
        const std::vector<std::string> wantModulesGated = {
            "settings.card.modules"};
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

// ---- 综合审查（2026-09-28）→ 2026-09-28 删 9 卡外层 cardFrame ----
// 用户原话：「设置页面最外层的卡片没删除」+「目前所有的菜单详情页面都在外层
// 加了卡片？底色是白色？不需要啊」。根因：settings 9 张卡每张调 ui_kit::cardFrame
//（surface 填充 + 1px 边框 + panelShadow 阴影 + radiusLarge 圆角），白色卡框叠在
// M3-4 主体渐变底（settings.body.bg）之上 = 用户看到的「最外层卡片」。
// 修复 = 删全部 9 处 cardFrame 调用（settings_ui_cards.cpp），不再有 <id>.bg 矩形 /
// 边框 / 阴影 / 圆角。旧「阴影分层」回归断言随卡框废弃，改反断言：bg 矩形必须不存在
//（cardFrame 若被加回会重新出现）、卡 stack 必须仍在。
// 回归断言（两档主题 × 9 张可见卡）：<stack>.bg 不存在、卡 stack 存在（内容未丢）。
//
// 同日第二轮（用户真机反馈「设置页面最外层的卡片还没删除啊，让你删除好多次了」）：
// 上面说的「页面渐变底」本身也是用户眼中的「最外层卡片」→ 已删。本用例只看卡级
// <id>.bg，不受影响；页面级底板的反断言在 test_ui_modern.cpp
// uiModernSettingsBodyGradientRemoved（settings.body.gradient / .bg 必须都不存在）。
REGISTER_TEST(settings_cards_no_outer_cardframe,
              "settings: 8 卡无外层 cardFrame——内容直接铺页面底（两档主题）", {
    test_support::TestEnv env(kCfgPrefix, "noframe", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "noframe_cfg"));
    {
        std::ofstream out(cfgEnv.cfgPath);
        out << "{\"theme\":\"dark\",\"enabledModules\":["
               "\"home\",\"markdown\",\"todo\",\"claude_proxy\",\"settings\",\"pet\"]}";
    }
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 卡 root stack id（内容锚：证卡内容仍在渲染）+ cardFrame 若加回会出现的 bg id
    //（bg 是 cardFrame 在 (0,0) 铺的矩形，id = <stack>.bg）。顺序 = 注册表序
    const int kTabOf[] = {0, 0, 1, 1, 1, 1, 2, 2};
    const char* const kStackIds[] = {
        "settings.card.appearance",
        "settings.card.editor",
        "settings.card.modules",
        "settings.card.pet",
        "settings.card.directory",
        "settings.card.proxy",
        "settings.card.log",
        "settings.tab.log",
    };
    const char* const kBgIds[] = {
        "settings.card.appearance.bg",
        "settings.card.editor.bg",
        "settings.card.modules.bg",
        "settings.card.pet.bg",
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

