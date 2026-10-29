// tests/test_modules/test_mcp_settings.cpp
// MCP 设置面三组：配置（解析/默认/回落/过滤/18 键往返）、app::mcp 桥（桩/注入/复位）、
// 设置卡（模块卡开关 / 门控显隐 / 状态三态 / 反馈四态 / 命令端口来源 / 三档几何）。
// 防的事故：开关被当未知 ID 清掉、端口脏数据落库、未装配时设置页崩溃、
// 测试间桩残留串扰、卡片挤坏现有布局、错误状态不可见。
#include "doctest.h"
#include "test_support.h"
#include "settings_config.h"
#include "settings_ui_logic.h"
#include "mcp_service_bridge.h"
#include "module_toggle_bridge.h"

#include "core/app_config.h"
#include "core/database.h"
#include "core/theme.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_settings_module();

// app::dslAppConfig()（dsl_app.h 声明）的实现 TU 全二进制唯一：dsl_app_impl.h
// 只由 test_settings.cpp 包含；本 TU 再包含/再定义即链接期 multiple definition。

namespace {

constexpr const char* kCfgPrefix = "devdesk_test_mcp_settings_";

// tab 点击助手（与 test_settings.cpp 同款：onClick 把 tab 索引写进模块成员）。
bool clickSettingsTab(::core::Module& mod, const eui::Screen& screen, int tab) {
    eui::Ui ui;
    ui.begin("mcp_settings_tab_click");
    mod.compose(ui, screen);
    ui.end();
    const auto* hit = ui.find("settings.tabs." + std::to_string(tab) + ".hit");
    if (hit == nullptr || hit->onClick == nullptr) return false;
    hit->onClick();
    return true;
}

std::unique_ptr<::core::Module> makeStartedSettingsModule() {
    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();
    return mod;
}

// 组一帧 compose 并返回 ui（调用方随后 find）。
void composeFrame(::core::Module& mod, eui::Ui& ui, const eui::Screen& screen,
                  const char* page) {
    ui.begin(page);
    mod.compose(ui, screen);
    ui.end();
}

// ── 桥 recording 桩 ──
struct McpBridgeRecording {
    int startCalls = 0;
    int stopCalls = 0;
    int applyCalls = 0;
    int statusCalls = 0;
    int lastStartPort = -1;
    int lastApplyPort = -1;
    bool startResult = true;    // 注入的成功/失败形态
    bool running = false;
    int livePort = 0;
    std::string liveError;
};
McpBridgeRecording g_mcpBridge;

core::Result<void> mcpRecStart(int port) {
    g_mcpBridge.startCalls++;
    g_mcpBridge.lastStartPort = port;
    if (g_mcpBridge.startResult) return core::Result<void>::ok();
    return core::Result<void>::fail(core::ErrorCode::Internal, "注入的启动失败");
}
void mcpRecStop() { g_mcpBridge.stopCalls++; }
core::Result<void> mcpRecApplyPort(int port) {
    g_mcpBridge.applyCalls++;
    g_mcpBridge.lastApplyPort = port;
    if (g_mcpBridge.startResult) return core::Result<void>::ok();
    return core::Result<void>::fail(core::ErrorCode::Internal, "注入的重启失败");
}
app::mcp::LiveStatus mcpRecStatus() {
    g_mcpBridge.statusCalls++;
    app::mcp::LiveStatus st;
    st.running = g_mcpBridge.running;
    st.port = g_mcpBridge.livePort;
    st.lastError = g_mcpBridge.liveError;
    return st;
}

// RAII 复位：fail-fast 早退也回厂（出厂态 = 四槽全空 = 默认桩）。
struct McpBridgeGuard {
    McpBridgeGuard() { g_mcpBridge = McpBridgeRecording{}; }
    ~McpBridgeGuard() { app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr); }
    McpBridgeGuard(const McpBridgeGuard&) = delete;
    McpBridgeGuard& operator=(const McpBridgeGuard&) = delete;
};

// 模块开关桥 recording 桩（模块卡 mcp 开关走 applyModuleEnabled → onModuleToggled）。
struct ToggleRecording {
    int calls = 0;
    std::string lastId;
    bool lastOn = false;
};
ToggleRecording g_mcpToggle;
void mcpToggleRecord(const std::string& id, bool on) {
    g_mcpToggle.calls++;
    g_mcpToggle.lastId = id;
    g_mcpToggle.lastOn = on;
}
struct McpToggleGuard {
    McpToggleGuard() { g_mcpToggle = ToggleRecording{}; }
    ~McpToggleGuard() { app::toggle::setToggleHandler(nullptr); }
    McpToggleGuard(const McpToggleGuard&) = delete;
    McpToggleGuard& operator=(const McpToggleGuard&) = delete;
};

} // namespace

// ────────────────── 组一：配置 ──────────────────

REGISTER_TEST(mcp_settings_parse_port_input_matrix,
              "mcp: parseMcpPortInput 边界/符号/空格/非数字只认范围内纯数字", {
    using namespace module::settings;
    int out = 0;
    DOCTEST_CHECK(parseMcpPortInput("1", out));
    DOCTEST_CHECK_EQ(out, 1);
    DOCTEST_CHECK(parseMcpPortInput("65535", out));
    DOCTEST_CHECK_EQ(out, 65535);
    DOCTEST_CHECK(parseMcpPortInput("8931", out));
    DOCTEST_CHECK_EQ(out, 8931);
    // 越界 / 0 / 符号 / 空白 / 非数字一律 false（与 parseProxyPortInput 同口径）
    DOCTEST_CHECK(!parseMcpPortInput("0", out));
    DOCTEST_CHECK(!parseMcpPortInput("65536", out));
    DOCTEST_CHECK(!parseMcpPortInput("-1", out));
    DOCTEST_CHECK(!parseMcpPortInput("+80", out));
    DOCTEST_CHECK(!parseMcpPortInput(" 8931", out));
    DOCTEST_CHECK(!parseMcpPortInput("8931 ", out));
    DOCTEST_CHECK(!parseMcpPortInput("88ab", out));
    DOCTEST_CHECK(!parseMcpPortInput("", out));
    DOCTEST_CHECK(!parseMcpPortInput("８９３１", out));  // 全角数字不是纯 ASCII 数字
    return true;
})

REGISTER_TEST(mcp_settings_defaults_port_8931_and_module_off,
              "mcp: 默认端口 8931（与 core 常量同源）；模块默认关闭", {
    using namespace module::settings;
    DOCTEST_CHECK_EQ(kDefaultMcpPort, static_cast<int>(::core::kDefaultMcpPort));
    DOCTEST_CHECK_EQ(kDefaultMcpPort, 8931);
    const SettingsConfig cfg;
    DOCTEST_CHECK_EQ(cfg.mcpPort, 8931);
    // 默认关闭：不在 defaultEnabledModules（agent/wallpaper 同款口径）
    const auto defaults = defaultEnabledModules();
    const bool hasMcp = std::find(defaults.begin(), defaults.end(),
                                  std::string(kModuleIdMcp)) != defaults.end();
    DOCTEST_CHECK(!hasMcp);
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdMcp));
    return true;
})

REGISTER_TEST(mcp_settings_sanitized_out_of_range_falls_back_default,
              "mcp: sanitized 越界/0/负回落 8931（不钳到 1/65535 边界）", {
    using namespace module::settings;
    for (const int dirty : {-1, 0, 65536, 70000, 1000000}) {
        SettingsConfig cfg;
        cfg.mcpPort = dirty;
        const SettingsConfig fixed = SettingsStore::sanitized(cfg);
        DOCTEST_CHECK_EQ(fixed.mcpPort, 8931);
    }
    // 合法边界值原样保留（回落 ≠ 钳制）
    SettingsConfig edge;
    edge.mcpPort = 65535;
    DOCTEST_CHECK_EQ(SettingsStore::sanitized(edge).mcpPort, 65535);
    return true;
})

REGISTER_TEST(mcp_settings_unknown_module_id_mcp_is_known,
              "mcp: sanitized 过滤未知 ID 但保留 mcp（防开关被当脏数据清掉）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "knownids", true);
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(), SettingsStore::keyNamespace(),
        std::string("{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
                    "\"claude_proxy\", \"settings\", \"mcp\", \"bogus\"]}")));
    const SettingsConfig loaded = SettingsStore(::core::appDatabase()).load();
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdMcp));   // mcp 是合法 ID：保留
    DOCTEST_CHECK(!moduleEnabled(loaded, "bogus"));       // 真 unknown 仍被过滤
    DOCTEST_CHECK(moduleEnabled(loaded, kModuleIdHome));  // 恒启用补齐不回退
    return true;
})

REGISTER_TEST(mcp_settings_save_load_roundtrip_19_keys,
              "mcp: mcpPort save/load 往返；顶层键 17→19（新键落库不吞旧键）", {
    using namespace module::settings;
    test_support::TestEnv env(kCfgPrefix, "roundtrip", true);
    SettingsConfig cfg;
    cfg.mcpPort = 9800;
    const SettingsStore store(::core::appDatabase());
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.mcpPort, 9800);
    // 行数口径：save 是 21 个顶层键一事务（settings_config.cpp save 的 rows 表）。
    // 演进：mcpPort 加入时 17→18，release 模块补 moduleDefaultsVersion → 19，
    // 09d8aa62 +releaseReminderPopup → 20，48ddb708 +fullscreenPopup → 21。
    // 本断言钉「save 写全部顶层键、eraseWithPrefixExcept keep 全部键
    // （新键落库不吞旧键）」的口径。
    auto rows = core::appconfig::listWithPrefix(*::core::appDatabase(),
                                                SettingsStore::keyNamespace());
    DOCTEST_REQUIRE(rows.isOk());
    DOCTEST_CHECK_EQ(rows.value().size(), static_cast<size_t>(21));
    // 存量库缺 mcpPort 键（旧版本升级）→ 读出默认 8931，不报错
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(), SettingsStore::keyNamespace(),
        "{\"theme\": \"dark\"}"));
    const SettingsConfig legacy = store.load();
    DOCTEST_CHECK_EQ(legacy.mcpPort, 8931);
    return true;
})

// ────────────────── 组二：app::mcp 桥 ──────────────────

REGISTER_TEST(mcp_bridge_default_stub_fails_closed,
              "mcp: 默认桩 start/applyPort fail「MCP 服务未装配」；status 全空；stop 不崩", {
    app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr);  // 出厂态显式化
    auto startR = app::mcp::start(8931);
    DOCTEST_CHECK(startR.isFail());
    DOCTEST_CHECK(startR.errorMessage().find("MCP 服务未装配") !=
                  std::string::npos);
    auto applyR = app::mcp::applyPort(9000);
    DOCTEST_CHECK(applyR.isFail());
    DOCTEST_CHECK(applyR.errorMessage().find("MCP 服务未装配") !=
                  std::string::npos);
    const app::mcp::LiveStatus st = app::mcp::status();
    DOCTEST_CHECK(!st.running);
    DOCTEST_CHECK_EQ(st.port, 0);
    DOCTEST_CHECK(st.lastError.empty());
    app::mcp::stop();  // no-op：不崩即通过
    return true;
})

REGISTER_TEST(mcp_bridge_injected_handlers_route_and_reset,
              "mcp: 注入 recording 桩后四槽路由计数正确；复位回默认桩", {
    McpBridgeGuard guard;
    app::mcp::setHandlers(&mcpRecStart, &mcpRecStop, &mcpRecApplyPort,
                          &mcpRecStatus);
    DOCTEST_REQUIRE(app::mcp::start(8931).isOk());
    DOCTEST_CHECK_EQ(g_mcpBridge.startCalls, 1);
    DOCTEST_CHECK_EQ(g_mcpBridge.lastStartPort, 8931);
    g_mcpBridge.startResult = false;
    auto failed = app::mcp::start(1);
    DOCTEST_CHECK(failed.isFail());
    DOCTEST_CHECK(failed.errorMessage().find("注入的启动失败") != std::string::npos);
    g_mcpBridge.startResult = true;
    DOCTEST_REQUIRE(app::mcp::applyPort(9000).isOk());
    DOCTEST_CHECK_EQ(g_mcpBridge.lastApplyPort, 9000);
    g_mcpBridge.startResult = false;
    auto applyFailed = app::mcp::applyPort(1);
    DOCTEST_CHECK(applyFailed.isFail());
    DOCTEST_CHECK(applyFailed.errorMessage().find("注入的重启失败") !=
                  std::string::npos);
    g_mcpBridge.startResult = true;
    app::mcp::stop();
    DOCTEST_CHECK_EQ(g_mcpBridge.stopCalls, 1);
    g_mcpBridge.running = true;
    g_mcpBridge.livePort = 8931;
    const app::mcp::LiveStatus st = app::mcp::status();
    DOCTEST_CHECK(st.running);
    DOCTEST_CHECK_EQ(st.port, 8931);
    DOCTEST_CHECK_EQ(g_mcpBridge.statusCalls, 1);
    // 复位：四槽全空 = 回默认桩（防桩残留串扰到后续用例）
    app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr);
    DOCTEST_CHECK(app::mcp::start(1).isFail());
    DOCTEST_CHECK_EQ(g_mcpBridge.startCalls, 2);  // 桩不再路由，计数冻结
    return true;
})

// ────────────────── 组三：设置卡 ──────────────────

REGISTER_TEST(mcp_card_modules_card_toggle_off_by_default_and_hit_wiring,
              "mcp: 模块卡标题右 MCP 开关默认关、.hit 可点；点击即落库 + 走既有开关桥", {
    test_support::TestEnv env(kCfgPrefix, "toggle", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "toggle_cfg"));
    McpToggleGuard toggleGuard;
    app::toggle::setToggleHandler(&mcpToggleRecord);
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    std::unique_ptr<::core::Module> mod = makeStartedSettingsModule();
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 1));  // 模块 tab
    eui::Ui ui;
    composeFrame(*mod, ui, screen, "mcp_toggle_1");
    ui.layout(screen.width, screen.height);  // frame 断言必须真 layout
    const auto* sw = ui.find("settings.card.modules.mcp");
    DOCTEST_REQUIRE(sw != nullptr);
    const auto* hit = ui.find("settings.card.modules.mcp.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_REQUIRE(hit->onClick != nullptr);
    // 标题盒收窄让位 150 开关盒（w-56-150），与开关盒相接不叠印
    const auto* title = ui.find("settings.card.modules.title");
    const auto* card = ui.find("settings.card.modules");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_CHECK(std::fabs(title->frame.width -
                            (card->frame.width - 56.0f - 150.0f)) < 0.5f);
    // 点击 → mcp 进 enabledModules 落库 + 既有开关桥收到 ("mcp", true)
    hit->onClick();
    composeFrame(*mod, ui, screen, "mcp_toggle_2");
    const module::settings::SettingsStore store(::core::appDatabase());
    DOCTEST_CHECK(module::settings::moduleEnabled(store.load(),
                                                  module::settings::kModuleIdMcp));
    DOCTEST_CHECK_EQ(g_mcpToggle.calls, 1);
    DOCTEST_CHECK_EQ(g_mcpToggle.lastId, std::string("mcp"));
    DOCTEST_CHECK(g_mcpToggle.lastOn);
    mod->onStop();
    return true;
})

REGISTER_TEST(mcp_card_gated_hidden_when_module_off,
              "mcp: 模块关闭时通用 tab 整卡不建；开启后卡出现（门控语义）", {
    test_support::TestEnv env(kCfgPrefix, "gate", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "gate_cfg"));
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    {
        std::unique_ptr<::core::Module> mod = makeStartedSettingsModule();
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 0));  // 通用 tab
        eui::Ui ui;
        composeFrame(*mod, ui, screen, "mcp_gate_off");
        DOCTEST_CHECK(ui.find("settings.card.mcp") == nullptr);
        mod->onStop();
    }
    // 播种 mcp 开启 → 重新进模块，卡必须在
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(),
        module::settings::SettingsStore::keyNamespace(),
        std::string("{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
                    "\"claude_proxy\", \"settings\", \"mcp\"]}")));
    {
        std::unique_ptr<::core::Module> mod = makeStartedSettingsModule();
        DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 0));
        eui::Ui ui;
        composeFrame(*mod, ui, screen, "mcp_gate_on");
        DOCTEST_CHECK(ui.find("settings.card.mcp") != nullptr);
        DOCTEST_CHECK(ui.find("settings.card.mcp.port.input") != nullptr);
        DOCTEST_CHECK(ui.find("settings.card.mcp.port.apply") != nullptr);
        DOCTEST_CHECK(ui.find("settings.card.mcp.status") != nullptr);
        DOCTEST_CHECK(ui.find("settings.card.mcp.command") != nullptr);
        mod->onStop();
    }
    return true;
})

REGISTER_TEST(mcp_card_status_row_three_states,
              "mcp: 状态行三态文案（已停止/运行中 127.0.0.1:<端口>/启动失败:<原因>）", {
    test_support::TestEnv env(kCfgPrefix, "status", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "status_cfg"));
    McpBridgeGuard guard;
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(),
        module::settings::SettingsStore::keyNamespace(),
        std::string("{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
                    "\"claude_proxy\", \"settings\", \"mcp\"]}")));
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    std::unique_ptr<::core::Module> mod = makeStartedSettingsModule();
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 0));
    eui::Ui ui;
    // ① 出厂桩：未运行 → 已停止
    composeFrame(*mod, ui, screen, "mcp_status_stopped");
    const auto* stopped = ui.find("settings.card.mcp.status");
    DOCTEST_REQUIRE(stopped != nullptr);
    DOCTEST_CHECK(stopped->text.find("已停止") != std::string::npos);
    // ② 运行中：快照端口进文案
    g_mcpBridge.running = true;
    g_mcpBridge.livePort = 8800;
    app::mcp::setHandlers(nullptr, nullptr, nullptr, &mcpRecStatus);
    composeFrame(*mod, ui, screen, "mcp_status_running");
    const auto* running = ui.find("settings.card.mcp.status");
    DOCTEST_REQUIRE(running != nullptr);
    DOCTEST_CHECK(running->text.find("运行中 127.0.0.1:8800") != std::string::npos);
    // ③ 启动失败：lastError 透出
    g_mcpBridge.running = false;
    g_mcpBridge.liveError = "端口 8800 被占用或无法监听";
    composeFrame(*mod, ui, screen, "mcp_status_failed");
    const auto* failed = ui.find("settings.card.mcp.status");
    DOCTEST_REQUIRE(failed != nullptr);
    DOCTEST_CHECK(failed->text.find("启动失败：端口 8800 被占用或无法监听") !=
                  std::string::npos);
    mod->onStop();
    return true;
})

REGISTER_TEST(mcp_card_port_feedback_four_states_and_persist_gate,
              "mcp: 反馈四态 + 非法端口不落库 + 输入变化清错 + 应用走桥 applyPort", {
    test_support::TestEnv env(kCfgPrefix, "feedback", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "feedback_cfg"));
    McpBridgeGuard guard;
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(),
        module::settings::SettingsStore::keyNamespace(),
        std::string("{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
                    "\"claude_proxy\", \"settings\", \"mcp\"]}")));
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    std::unique_ptr<::core::Module> mod = makeStartedSettingsModule();
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 0));
    eui::Ui ui;
    const module::settings::SettingsStore store(::core::appDatabase());
    // lambda 内不用 doctest 宏（宏展开含 return false，与返回类型冲突）；
    // 元素缺失时静默跳过注入，后续断言自然翻红。
    // onTextInput 是追加语义：每次注入先 Ctrl+A 全选再输入 = 替换成目标草稿
    //（与 test_settings 代理端口行同款驱动）。
    auto typeDraft = [&](const char* page, const std::string& text) {
        composeFrame(*mod, ui, screen, page);
        auto* input = ui.find("settings.card.mcp.port.input.hit");
        if (input == nullptr || input->onKeyEvent == nullptr ||
            input->onTextInput == nullptr) {
            return;
        }
        eui::KeyEvent selectAll;
        selectAll.key = eui::InputKey::A;
        selectAll.action = eui::KeyAction::Press;
        test_support::setShortcutModifier(selectAll.modifiers);
        input->onKeyEvent(selectAll);
        eui::TextInputEvent ev;
        ev.text = text;
        input->onTextInput(ev);
    };
    auto clickApply = [&](const char* page) {
        composeFrame(*mod, ui, screen, page);
        auto* apply = ui.find("settings.card.mcp.port.apply.bg");
        if (apply == nullptr || apply->onClick == nullptr) return;
        apply->onClick();
    };
    auto feedbackText = [&](const char* page) -> std::string {
        composeFrame(*mod, ui, screen, page);
        const auto* fb = ui.find("settings.card.mcp.port.feedback");
        return fb != nullptr ? fb->text : std::string("<feedback 元素缺失>");
    };
    // ① 空草稿点应用 → 红字「请输入端口（1-65535）」，不落库
    typeDraft("mcp_fb_t1", "");
    clickApply("mcp_fb_a1");
    DOCTEST_CHECK(feedbackText("mcp_fb_v1").find("请输入端口（1-65535）") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(store.load().mcpPort, 8931);
    // ② 非法草稿 → 红字原因，不落库
    typeDraft("mcp_fb_t2", "abc");
    clickApply("mcp_fb_a2");
    DOCTEST_CHECK(feedbackText("mcp_fb_v2").find("端口无效") != std::string::npos);
    DOCTEST_CHECK_EQ(store.load().mcpPort, 8931);
    // ③ 输入变化 → 清错回静态提示
    typeDraft("mcp_fb_t3", "9");
    DOCTEST_CHECK(feedbackText("mcp_fb_v3").find("改后回车或点「应用」") !=
                  std::string::npos);
    // ④ 合法且未变化（draft=当前端口 8931）→ 三级字「端口未变化」
    typeDraft("mcp_fb_t4", "8931");
    clickApply("mcp_fb_a4");
    DOCTEST_CHECK(feedbackText("mcp_fb_v4").find("端口未变化") != std::string::npos);
    DOCTEST_CHECK_EQ(store.load().mcpPort, 8931);
    // ⑤ 合法且变化 → 落库 + 反馈回静态提示 + 桥 applyPort 收到新端口
    app::mcp::setHandlers(&mcpRecStart, &mcpRecStop, &mcpRecApplyPort,
                          &mcpRecStatus);
    g_mcpBridge.running = true;  // 运行中改端口 = 桥内重启
    typeDraft("mcp_fb_t5", "9000");
    clickApply("mcp_fb_a5");
    DOCTEST_CHECK(feedbackText("mcp_fb_v5").find("改后回车或点「应用」") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(store.load().mcpPort, 9000);
    DOCTEST_CHECK_EQ(g_mcpBridge.applyCalls, 1);
    DOCTEST_CHECK_EQ(g_mcpBridge.lastApplyPort, 9000);
    // ⑥ Esc 丢草稿：回落已应用端口显示
    typeDraft("mcp_fb_t6", "7777");
    composeFrame(*mod, ui, screen, "mcp_fb_a6");
    auto* input = ui.find("settings.card.mcp.port.input.hit");
    DOCTEST_REQUIRE(input != nullptr);
    DOCTEST_REQUIRE(input->onKeyEvent != nullptr);
    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK(input->onKeyEvent(esc));  // 被 onCancel 消费
    composeFrame(*mod, ui, screen, "mcp_fb_v6");
    const auto* shown = ui.find("settings.card.mcp.port.input.text");
    DOCTEST_REQUIRE(shown != nullptr);
    DOCTEST_CHECK_EQ(shown->text, std::string("9000"));
    mod->onStop();
    return true;
})

REGISTER_TEST(mcp_card_command_port_source_running_vs_configured,
              "mcp: 接入命令端口来源——运行中取实际端口，未运行取已配置端口", {
    test_support::TestEnv env(kCfgPrefix, "command", true);
    test_support::ScopedConfigPath cfgEnv(
        test_support::tempConfigFile(kCfgPrefix, "command_cfg"));
    McpBridgeGuard guard;
    DOCTEST_REQUIRE(test_support::seedConfigJsonRows(
        ::core::appDatabase(),
        module::settings::SettingsStore::keyNamespace(),
        std::string("{\"enabledModules\": [\"home\", \"markdown\", \"todo\", "
                    "\"claude_proxy\", \"settings\", \"mcp\"], "
                    "\"mcpPort\": 9800}")));
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    std::unique_ptr<::core::Module> mod = makeStartedSettingsModule();
    DOCTEST_REQUIRE(clickSettingsTab(*mod, screen, 0));
    eui::Ui ui;
    // 未运行：命令用已配置端口 9800
    composeFrame(*mod, ui, screen, "mcp_cmd_stopped");
    const auto* stopped = ui.find("settings.card.mcp.command");
    DOCTEST_REQUIRE(stopped != nullptr);
    DOCTEST_CHECK(stopped->text.find("claude mcp add --transport http devdesk") !=
                  std::string::npos);
    DOCTEST_CHECK(stopped->text.find("http://127.0.0.1:9800/mcp") !=
                  std::string::npos);
    // 运行中：命令用实际监听端口（可能与配置端口不同——port 0 / 占用回落场景）
    g_mcpBridge.running = true;
    g_mcpBridge.livePort = 8801;
    app::mcp::setHandlers(nullptr, nullptr, nullptr, &mcpRecStatus);
    composeFrame(*mod, ui, screen, "mcp_cmd_running");
    const auto* running = ui.find("settings.card.mcp.command");
    DOCTEST_REQUIRE(running != nullptr);
    DOCTEST_CHECK(running->text.find("http://127.0.0.1:8801/mcp") !=
                  std::string::npos);
    mod->onStop();
    return true;
})

REGISTER_TEST(mcp_card_geometry_derived_rows_three_scales,
              "mcp: 85/100/130% 三档行盒不重叠、下边距 ≥16；100% 锚定 68/128/160", {
    using namespace module::settings;
    // 卡高 = 行几何求和（不写死 160）；下边距下限 16：85% 档 roundFs(16)=14 必须
    // 被 std::max(16.0f, roundFs(16.0f)) 抬回 16
    for (const int percent : {85, 100, 130}) {
        ::core::theme::setFontScale(static_cast<float>(percent) / 100.0f);
        const float portY = mcpPortRowY();
        const float portH = logFieldHeight();
        const float statusY = portY + portH;
        const float statusH = mcpStatusRowHeight();
        const float cmdY = mcpCommandRowY();
        const float cmdH = mcpCommandRowHeight();
        const float cardH = mcpCardHeightFor();
        // 行盒不重叠（下一行 y ≥ 上一行底）
        DOCTEST_CHECK(statusY >= portY + portH - 0.01f);
        DOCTEST_CHECK(cmdY >= statusY + statusH - 0.01f);
        // 下边距 ≥16（卡内边距契约）
        DOCTEST_CHECK(cardH - (cmdY + cmdH) >= 16.0f);
        if (percent == 100) {
            DOCTEST_CHECK(std::fabs(portY - 68.0f) < 0.01f);
            DOCTEST_CHECK(std::fabs(cmdY - 128.0f) < 0.01f);
            DOCTEST_CHECK(std::fabs(cardH - 160.0f) < 0.01f);
        }
    }
    ::core::theme::setFontScale(1.0f);
    return true;
})
