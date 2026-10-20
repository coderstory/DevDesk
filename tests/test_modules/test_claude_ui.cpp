// tests/test_modules/test_claude_ui.cpp
// Claude 模块 UI 接线无头冒烟（compose 模式）：无服务时不崩、有服务时渲染
// profile 卡片 / ACTIVE 徽标 / 详情 / 操作按钮。
// 跨文件前提：详情面板读的是后台快照（H2），保存 / 外部写库后最多晚 1 帧可见——
// 断言可见性一律走有界轮询 + onPageEnter，不能只 compose 一帧就断言。
// 真实鼠标点击留给人工点验。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "claude_ui_logic.h"
#include "core/app_config.h"
#include "core/database.h"
#include "platform/json.h"
#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "core/time.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "platform/terminal_launch.h"
#include "platform/folder_picker.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <eui/dsl.h>
#include <components/input.h>   // InputState：读回编辑框文本（格式化当场改写的判据）

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

extern "C" ::core::Module* create_claude_module();
// 调用记录后台查询的测试缝（claude_module.cpp 文件末尾同名导出）：累计派发
// 次数（派发点即增，同帧可读）/ 最近一次实际执行查询的线程 id 哈希
//（worker 写，0 = 未跑过）。用于把「渲染帧只发意图」写成计数断言
extern "C" int claude_calls_query_dispatches_for_test();
extern "C" unsigned long long claude_calls_query_last_thread_for_test();

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeModule";

// 临时 db 文件名前缀（test_support::TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_claude_ui_";

// RAII：临时库 + 注册全局 ClaudeService（析构时解除注册，防悬垂污染后续用例）
// TestDb 基座在共享头，这里只剩 ClaudeService glue。
struct TestEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    TestEnv() : test_support::TestDb(kDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(db->db(),
                                                              kTestKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~TestEnv() {
        module::claude::ClaudeService::setGlobal(nullptr);
    }
};

// 清某个 profile 的测试密钥
using test_support::cleanupProfileKey;

// 动态空闲端口：借 platform::http::Server 的 bind 0 支持（http_server.cpp：
// port==0 → bind_to_any_port）向系统要一个空闲端口随即释放，交还调用方独占——
// 多 worktree / 残留进程并发跑测试时不撞写死的端口号。失败返回 0（调用方 REQUIRE 拦）。
int pickFreePort() {
    platform::http::Server probe("127.0.0.1", 0);
    if (!probe.start()) {
        return 0;
    }
    const int port = probe.actualPort();
    probe.stop();
    return port;
}

// 「导入当前配置」测试的 settings.json 路径（不碰真实 ~/.claude——
// settings_sync 的 Paths 注入模式）
constexpr const char* kImportTempDirPrefix = "devdesk_test_import_cur_";

// sidecar 是 app_config 表的一行（键 kSidecarConfigKey），故要一并传入库句柄
module::claude::settings_sync::Paths importPaths(
    const test_support::TempDir& d, core::data::SqliteDb& db) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.db = &db;
    return p;
}

// 浮点几何相等（px 级判等，0.01 容差）——表单双列几何断言用
bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }

} // namespace

// 0. 纯函数 formatCallTime
    // 锚定本地正午避免跨零点抖动
REGISTER_TEST(claude_format_call_time, "claude: formatCallTime 今日/昨天/更早三态", {
    const std::time_t now = std::time(nullptr);
    std::tm nowTm{};
    core::time::localTime(now, &nowTm);
    nowTm.tm_hour = 12;
    nowTm.tm_min = 0;
    nowTm.tm_sec = 0;
    const std::time_t noon = std::mktime(&nowTm);

    DOCTEST_CHECK_EQ(module::claude::formatCallTime(0, noon),
                     std::string("--:--"));
    // 今日 → HH:MM
    DOCTEST_CHECK_EQ(module::claude::formatCallTime(noon, noon),
                     std::string("12:00"));
    DOCTEST_CHECK_EQ(module::claude::formatCallTime(noon - 90 * 60, noon),
                     std::string("10:30"));
    // 昨天 → 昨天 HH:MM
    DOCTEST_CHECK_EQ(module::claude::formatCallTime(noon - 86400, noon),
                     std::string("昨天 12:00"));
    DOCTEST_CHECK_EQ(module::claude::formatCallTime(noon - 86400 - 3600, noon),
                     std::string("昨天 11:00"));
    // 更早 → MM-DD
    const std::string older =
        module::claude::formatCallTime(noon - 3 * 86400, noon);
    DOCTEST_CHECK_EQ(older.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(older[2], '-');
    return true;
})

// 1. 无服务（global 为 null）时 compose 不崩

REGISTER_TEST(claude_module_compose_null_service,
              "claude_module: compose 无服务不崩（headless）", {
    module::claude::ClaudeService::setGlobal(nullptr);   // 干净起点
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    ui.begin("claude_null");
    mod->compose(ui, screen);
    ui.end();

    // 关键操作按钮即使无数据也应渲染
    DOCTEST_CHECK(ui.find("claude.root") != nullptr);
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.enable") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.run") != nullptr);   // 第 4 键（profile-run）
    // 删除 / 建副本 / 复制配置三键不在动作行（入口只剩列表右键菜单，业务逻辑同源保留）
    DOCTEST_CHECK(ui.find("claude.act.del") == nullptr);
    DOCTEST_CHECK(ui.find("claude.act.dup") == nullptr);
    DOCTEST_CHECK(ui.find("claude.act.copyconfig") == nullptr);
    // 无服务时显示错误态（不静默吞错显示假空态）
    DOCTEST_CHECK(ui.find("claude.profiles.error") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.empty") == nullptr);

    // 活门控死页缓解：无服务时补「未装配」toast（回退成静默即红）
    {
        const auto* warnTitle = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(warnTitle != nullptr);
        DOCTEST_CHECK(warnTitle->text.find("未装配") != std::string::npos);
        const auto* warnMsg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(warnMsg != nullptr);
        DOCTEST_CHECK(warnMsg->text.find("重启") != std::string::npos);
    }

    // 无选中 + 无服务：动作行只剩 4 键——「未选中提示」通道由右键菜单的 warn 覆盖
    {
        DOCTEST_CHECK(ui.find("claude.act.del.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.act.dup.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.act.copyconfig.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.bg") != nullptr);
    }

    mod->onStop();
    return true;
})

// 4 键均分几何：末键右缘对齐分隔线
// actW = (w0 − actGap×3)/4。EUI 的 row spacing 只在相邻子项之间生效，n 项只有
// n−1 个间隙；多扣一个 gap 会让末键右缘短一整个 gap。

REGISTER_TEST(claude_module_actions_row_alignment,
              "claude_module: 4 键均分——末键（运行）右缘对齐分隔线（headless layout）", {
    module::claude::ClaudeService::setGlobal(nullptr);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("claude_actions_align");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* divider = ui.find("claude.act.divider");
    const auto* row = ui.find("claude.detail.actions");
    const auto* run = ui.find("claude.act.run.bg");
    DOCTEST_REQUIRE(divider != nullptr);
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(run != nullptr);
    const float dividerRight = divider->frame.x + divider->frame.width;
    const float runRight = run->frame.x + run->frame.width;
    DOCTEST_CHECK(runRight >= dividerRight - 0.5f);
    DOCTEST_CHECK(runRight <= dividerRight + 0.5f);
        // 单键宽 = (行宽 − 3×gap) / 4（gap=12）
    const float expect = (row->frame.width - 12.0f * 3.0f) / 4.0f;
    DOCTEST_CHECK(std::fabs(run->frame.width - expect) < 0.01f);
    DOCTEST_CHECK(std::fabs(ui.find("claude.act.enable.bg")->frame.width - expect) <
                  0.01f);

    mod->onStop();
    return true;
})

// UAT2 #10 C 批布局回归锁（L-5，UAT2 一审）：状态组（dot+text+help）从 title row
// 挪到 header 右侧 actions 区后——① 右对齐：status row 右缘 ≥ title row 右缘
//（状态组贴 header 右端，title row 只占左段 contentW−360）；② wrapContent
// 收缩：无服务态「已停止」≈39px，回退成旧 200px 固定宽盒 → 超 120 红。
// headless 出 frame 须 ui.layout()，写法仿上一条。
REGISTER_TEST(claude_module_header_status_geometry,
              "claude_module: header 状态组右对齐 + wrapContent 收缩（headless layout）", {
    module::claude::ClaudeService::setGlobal(nullptr);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("claude_status_geometry");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    const auto* statusRow = ui.find("claude.status.row");
    const auto* titleRow = ui.find("claude.title.row");
    const auto* statusText = ui.find("claude.status.text");
    DOCTEST_REQUIRE(statusRow != nullptr);
    DOCTEST_REQUIRE(titleRow != nullptr);
    DOCTEST_REQUIRE(statusText != nullptr);
    // ① 右对齐：状态组右缘不早于 title row 右缘
    const float statusRight = statusRow->frame.x + statusRow->frame.width;
    const float titleRight = titleRow->frame.x + titleRow->frame.width;
    DOCTEST_CHECK(statusRight >= titleRight);
    // ② wrapContent 收缩：上界 120 容「运行中 :port」动态变体（本用例为已停止态）
    DOCTEST_CHECK(statusText->frame.width <= 120.0f);

    mod->onStop();
    return true;
})

// 2. 有真实服务 + 数据时 compose 渲染卡片/徽标/详情

REGISTER_TEST(claude_module_compose_with_data,
              "claude_module: compose 有数据渲染卡片+按钮（headless）", {
    TestEnv env;
    auto a = env.svc->createProfile("官方", "https://api.anthropic.com",
                                    "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    auto b = env.svc->createProfile("中转", "https://relay.example.com",
                                    "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("claude_data");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("claude.profile.0.name") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.1.name") != nullptr);
    // 激活卡片的 ACTIVE 徽标（profile 0 = 激活的「官方」）
    DOCTEST_CHECK(ui.find("claude.profile.0.active.text") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.1.active.text") == nullptr);

    const auto* nameV = ui.find("claude.field.name.value");
    DOCTEST_REQUIRE(nameV != nullptr);
    DOCTEST_CHECK_EQ(nameV->text, std::string("官方"));
    const auto* urlV = ui.find("claude.field.url.value");
    DOCTEST_REQUIRE(urlV != nullptr);
    DOCTEST_CHECK_EQ(urlV->text, std::string("https://api.anthropic.com"));

    DOCTEST_CHECK(ui.find("claude.act.enable") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.run") != nullptr);   // 第 4 键（profile-run）
    // 删除 / 建副本 / 复制配置三键不在动作行（入口只剩列表右键菜单）
    DOCTEST_CHECK(ui.find("claude.act.del") == nullptr);
    DOCTEST_CHECK(ui.find("claude.act.dup") == nullptr);
    DOCTEST_CHECK(ui.find("claude.act.copyconfig") == nullptr);
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// 3. 测试连通不阻塞 UI 线程（后台线程 + 结果回 compose 收割）
// UI 线程同步调 testConnection（timeout 15s）会在慢上游时冻结整个界面。
// 点击立即返回 + toast「测试中」，上游返回后由 compose 每帧收割结果。
REGISTER_TEST(claude_module_test_connection_async,
              "claude_module: 测试连通后台执行不冻结 UI（headless）", {
    TestEnv env;
        // mock 上游挂 500ms 再回 200：点击必须立即返回（不等上游）
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("POST", "/v1/messages",
                   [](const platform::http::RequestCtx&,
                      platform::http::ResponseWriter& w) {
                       std::this_thread::sleep_for(
                           std::chrono::milliseconds(500));
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    const std::string baseUrl =
        "http://127.0.0.1:" + std::to_string(upstream.actualPort());
    auto a = env.svc->createProfile("异步测试", baseUrl,
                                    "sk-ant-async-8888", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_async");
        mod->compose(ui, screen);
        ui.end();
    };

    {
        eui::Ui ui;
        compose(ui);
        const auto* testBtn = ui.find("claude.act.test.bg");
        DOCTEST_REQUIRE(testBtn != nullptr);
        DOCTEST_REQUIRE(testBtn->onClick != nullptr);
        testBtn->onClick();
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("测试中") != std::string::npos);
    }

        // 轮询 compose（每帧收割）直到上游响应被收割成结果 toast
    bool gotResult = false;
    for (int i = 0; i < 250 && !gotResult; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        if (title != nullptr &&
            title->text.find("连通成功") != std::string::npos) {
            gotResult = true;
        }
    }
    DOCTEST_CHECK(gotResult);

    mod->onStop();
    upstream.stop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// 3b. 前置校验：无激活 profile 点「启动代理」
REGISTER_TEST(claude_module_start_proxy_requires_active_profile,
              "claude_module: 无激活 profile 点「启动代理」→ 后台校验挡下（不起代理 + toast 给原因）", {
    TestEnv env;                       // 空库 = 事故现场；svc 已 setGlobal
    // 绝不绑 8888（本机开发会话的推理链路走 127.0.0.1:8888）。也不能写 0——
    // setConfiguredProxyPort 把越界值（含 0）钳回 kDefaultProxyPort = 8888，
    // 等于把红线焊死。取系统空闲端口。
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port > 0);
    env.svc->setConfiguredProxyPort(port);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_pv");
        mod->compose(ui, screen);
        ui.end();
    };

    {
        eui::Ui ui;
        compose(ui);
        // components::button 的**可点节点**是 `<id>.bg`（根 stack 只作容器，
        const auto* btn = ui.find("claude.btn.proxy.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
    }

        // 轮询 compose（每帧收割）直到失败结果 toast（空库路径零网络，通常 1-2 帧）
    bool gotResult = false;
    for (int i = 0; i < 250 && !gotResult; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        const auto* msg = ui.find("claude.toast.message");
        if (title != nullptr && msg != nullptr &&
            title->text.find("启动代理失败") != std::string::npos &&
            msg->text.find("前置校验未通过") != std::string::npos) {
            gotResult = true;
        }
    }
    DOCTEST_CHECK(gotResult);
    DOCTEST_CHECK_FALSE(env.svc->isProxyRunning());
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), 0);
    mod->onStop();
    return true;
})

// 3c. startProxy 只在 UI 收割帧执行
// 判别式：上游 1500ms 后才回 200；点击后在**不 compose** 的情况下静置 2000ms——
//   · startProxy 在 harvestTestResult 里 → 线程早已跑完但没人收割 → 仍不 bind；
//   · startProxy 在线程里 → 上游一回包线程立刻 bind → 循环里的 CHECK_FALSE 红。
REGISTER_TEST(claude_module_start_proxy_binds_only_at_harvest,
              "claude_module: startProxy 只在 UI 收割帧执行（校验在飞时不 bind，A1）", {
    TestEnv env;
    // 绝不绑 8888。也不能写 0——会被 setConfiguredProxyPort 钳回 8888，
    // 而本用例会真的走到 bind。取系统空闲端口（harvest 帧按此端口 bind）
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port > 0);
    env.svc->setConfiguredProxyPort(port);
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("POST", "/v1/messages",
                   [](const platform::http::RequestCtx&,
                      platform::http::ResponseWriter& w) {
                       // 1500ms ≫ 一帧：探测在飞窗口足够长，「startProxy 又回
                       // 线程」的变异必然落在下面的静置窗内 → 必红（延迟降到
                       // 500ms 时静置窗与延迟 1:1，边界时序会漏咬）
                       std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    auto a = env.svc->createProfile(
        "慢校验", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-pv-harvest", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_pv_harvest");
        mod->compose(ui, screen);
        ui.end();
    };

    {   // 点「启动代理」→ 立即返回（不冻结 UI 由 3b 钉；这里钉「不立刻 bind」）
        eui::Ui ui;
        compose(ui);
        // components::button 的**可点节点**是 `<id>.bg`（根 stack 只作容器，
        const auto* btn = ui.find("claude.btn.proxy.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
    }

    // 点击立即返回的可见面：此刻 toast 是 pending 态「正在校验」，结果要等收割帧。
    // pending 态钉在本用例（上游挂 1500ms）而非 3b——3b 的空库路径线程 ~µs 即回，
    // 下一帧 compose 已收割掉，那条断言与收割竞速、非确定。
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("正在校验") != std::string::npos);
        DOCTEST_CHECK_FALSE(env.svc->isProxyRunning());   // 仍在飞 = 未 bind
    }

    // 校验在飞 + 已回包但**尚无 compose 帧**的整个窗口：一律不 bind
    //（100×20ms ≈ 2000ms > 上游 1500ms 延迟，留 500ms 余量；这段刻意不 compose）
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        DOCTEST_CHECK_FALSE(env.svc->isProxyRunning());   // A1：绑定必须晚于收割帧
    }
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), 0);

    // 一帧 compose = 一次收割 → 这时才 bind
    bool started = false;
    for (int i = 0; i < 50 && !started; ++i) {
        eui::Ui ui;
        compose(ui);
        started = env.svc->isProxyRunning();
        if (!started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    DOCTEST_CHECK(started);
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), port);  // bind 在配置的空闲端口上（非 8888）
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("代理已启动") != std::string::npos);
    }
    env.svc->stopProxy();  // 用完还原
    mod->onStop();
    upstream.stop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

    // 4. Profile 表单 tabs：槽位/env 填写 → 保存 → DB 往返 → 重开回显
// 输入经 input.hit 的 onTextInput 驱动，等价于真实键盘输入路径（insertAtCursor
// + onChange 回模块状态）
REGISTER_TEST(claude_module_form_tabs_roundtrip,
              "claude_module: 表单 tabs 槽位/env 填写保存回显（headless）", {
    TestEnv env;
    auto a = env.svc->createProfile("槽位测试", "https://a.example.com",
                                    "sk-ant-slots-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_form_rt");
        mod->compose(ui, screen);
        ui.end();
    };
    // CHECK 宏内含 return false，不能进 void lambda —— helper 返回 bool，由测试体的
    // DOCTEST_CHECK 收敛（失败即 fail-fast 本用例）
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    // 无激活 profile 时详情面板需要 selectedId 才有编辑目标
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.form.tabs.hit.1") != nullptr);
        const auto* nameText = ui.find("claude.dialog.form.name.text");
        DOCTEST_REQUIRE(nameText != nullptr);
        DOCTEST_CHECK_EQ(nameText->text, std::string("槽位测试"));
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.1"));
    }
    // 模型映射页：槽位输入；默认槽位空 → name placeholder 是「自动派生」
    {
        eui::Ui ui;
        compose(ui);
        for (int i = 0; i < 6; ++i) {
            DOCTEST_CHECK(ui.find("claude.form.slot." + std::to_string(i) +
                                  ".model.hit") != nullptr);
        }
        const auto* ph = ui.find("claude.form.slot.0.name.text");
        DOCTEST_REQUIRE(ph != nullptr);
        DOCTEST_CHECK_EQ(ph->text, std::string("（自动派生）"));
        type(ui, "claude.form.slot.0.model.hit", "fangzhou/glm-5.3[1M]");
        type(ui, "claude.form.slot.1.model.hit", "m-sonnet");
        type(ui, "claude.form.slot.1.name.hit", "sonnet显示名");
        type(ui, "claude.form.slot.5.model.hit", "m-sub");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.env.add.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        type(ui, "claude.form.env.row.0.key.hit", "DISABLE_AUTOUPDATER");
        type(ui, "claude.form.env.row.0.value.hit", "1");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    // 详情面板模型映射 pane
    {
        {
            eui::Ui ui;
            compose(ui);
            DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.1"));  // 切到模型 tab
        }
        // 保存是异步刷新：详情 pane 读的是后台快照——有界轮询等新槽位值进**渲染树**。
        // 只 compose 两帧就读旧快照 = 快照竞态 flaky。
        bool slotUpdated = false;
        for (int i = 0; i < 300 && !slotUpdated; ++i) {
            mod->onPageEnter();   // 保持脏意图存活（在途旧快照收割会清脏 → 重新置脏）
            eui::Ui ui;
            compose(ui);
            const auto* s0 = ui.find("claude.slot.val.0");
            slotUpdated = s0 != nullptr &&
                          s0->text.find("fangzhou/glm-5.3") !=
                              std::string::npos;
            if (!slotUpdated) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        DOCTEST_CHECK(slotUpdated);   // 新 models_config 已进渲染快照
        eui::Ui ui;
        compose(ui);
        const auto* slot0 = ui.find("claude.slot.val.0");  // 默认槽位
        DOCTEST_REQUIRE(slot0 != nullptr);
        // 显示层剥 [1M]：1M 状态已由复选框表达，名字里不重复带标记。值列文本 =
        // deriveDisplayName(存储值)。用**精确相等**断言——find 前缀命中会被未剥的
        // "…[1M]" 假绿混过。
        DOCTEST_CHECK_EQ(slot0->text, std::string("fangzhou/glm-5.3"));
        // 列表卡片 model 行同口径剥除（同一 deriveDisplayName 通道）
        const auto* cardModel = ui.find("claude.profile.0.model");
        DOCTEST_REQUIRE(cardModel != nullptr);
        DOCTEST_CHECK_EQ(cardModel->text,
                         std::string("model: fangzhou/glm-5.3"));
    }
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        const auto& p = got.value();
        auto mc = module::claude::parseModelsConfig(p.modelsConfigJson);
        DOCTEST_REQUIRE(mc.isOk());
        DOCTEST_CHECK_EQ(mc.value().def.model,
                         std::string("fangzhou/glm-5.3[1M]"));
        DOCTEST_CHECK_EQ(mc.value().sonnet.model, std::string("m-sonnet"));
        DOCTEST_CHECK_EQ(mc.value().sonnet.name, std::string("sonnet显示名"));
        DOCTEST_CHECK_EQ(mc.value().subagent.model, std::string("m-sub"));
        DOCTEST_CHECK(mc.value().opus.model.empty());
        DOCTEST_CHECK_EQ(p.model, std::string("fangzhou/glm-5.3[1M]"));
        auto ee = module::claude::parseExtraEnv(p.extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_REQUIRE_EQ(ee.value().size(), 1u);
        DOCTEST_CHECK_EQ(ee.value().at("DISABLE_AUTOUPDATER"), std::string("1"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.1"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* m0 = ui.find("claude.form.slot.0.model.text");
        DOCTEST_REQUIRE(m0 != nullptr);
        DOCTEST_CHECK_EQ(m0->text, std::string("fangzhou/glm-5.3[1M]"));
        const auto* s1 = ui.find("claude.form.slot.1.name.text");
        DOCTEST_REQUIRE(s1 != nullptr);
        DOCTEST_CHECK_EQ(s1->text, std::string("sonnet显示名"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* k = ui.find("claude.form.env.row.0.key.text");
        DOCTEST_REQUIRE(k != nullptr);
        DOCTEST_CHECK_EQ(k->text, std::string("DISABLE_AUTOUPDATER"));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 5. cc-switch JSON 粘贴导入：解析预览 → 应用到表单 → 保存
REGISTER_TEST(claude_module_import_flow,
              "claude_module: cc-switch 粘贴导入解析应用（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_import");
        mod->compose(ui, screen);
        ui.end();
    };
    // CHECK 宏内含 return false，不能进 void lambda —— helper 返回 bool，由测试体的
    // DOCTEST_CHECK 收敛（失败即 fail-fast 本用例）
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };
    std::string tpl = R"({"env":{)";
    tpl += R"("ANTHROPIC_BASE_URL":"https://gw.example.com/a",)";
    tpl += R"("ANTHROPIC_AUTH_TOKEN":"sk-import-tok",)";
    tpl += R"("ANTHROPIC_MODEL":"m-default[1M]",)";
    tpl += R"("ANTHROPIC_DEFAULT_SONNET_MODEL":"m-sonnet[1M]",)";
    tpl += R"("AUTO_COMPACT_ENABLED":"0",)";
    tpl += R"("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE":"75",)";
    tpl += R"("DISABLE_AUTOUPDATER":"1")";
    tpl += "}}";

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        type(ui, "claude.dialog.form.name.hit", "导入测试");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        type(ui, "claude.form.import.text.hit", tpl);
        DOCTEST_CHECK(click(ui, "claude.form.import.parse.bg"));
    }
    // 解析预览：槽位 2/<行数> + 别名 1 + 未识别 1；别名提示行存在
    {
        eui::Ui ui;
        compose(ui);
        const auto* preview = ui.find("claude.form.import.preview");
        DOCTEST_REQUIRE(preview != nullptr);
        // 分母从槽位表派生：表加一行 → UI 预览分母自动跟随，故不钉死具体行数
        const std::string slotTag =
            "槽位 2/" + std::to_string(module::claude::modelSlotTable().size());
        DOCTEST_CHECK(preview->text.find(slotTag) != std::string::npos);
        DOCTEST_CHECK(preview->text.find("别名映射 1") != std::string::npos);
        const auto* note0 = ui.find("claude.form.import.note.0");
        DOCTEST_REQUIRE(note0 != nullptr);
        DOCTEST_CHECK(note0->text.find("AUTO_COMPACT_ENABLED") !=
                      std::string::npos);
        DOCTEST_CHECK(ui.find("claude.form.import.apply.bg") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.form.import.apply.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.0"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* url = ui.find("claude.dialog.form.url.text");
        DOCTEST_REQUIRE(url != nullptr);
        DOCTEST_CHECK_EQ(url->text, std::string("https://gw.example.com/a"));
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
        const auto& p = list.value()[0];
        DOCTEST_CHECK_EQ(p.name, std::string("导入测试"));
        DOCTEST_CHECK_EQ(p.baseUrl, std::string("https://gw.example.com/a"));
        DOCTEST_CHECK(!p.apiKeyHint.empty());
        auto mc = module::claude::parseModelsConfig(p.modelsConfigJson);
        DOCTEST_REQUIRE(mc.isOk());
        DOCTEST_CHECK_EQ(mc.value().def.model, std::string("m-default[1M]"));
        DOCTEST_CHECK_EQ(mc.value().sonnet.model, std::string("m-sonnet[1M]"));
        auto ee = module::claude::parseExtraEnv(p.extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
    // AUTO_COMPACT_ENABLED=0 → DISABLE_AUTO_COMPACT=1（值取反）
        DOCTEST_CHECK_EQ(ee.value().at("DISABLE_AUTO_COMPACT"),
                         std::string("1"));
        DOCTEST_CHECK_EQ(ee.value().at("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE"),
                         std::string("75"));
        DOCTEST_CHECK_EQ(ee.value().at("DISABLE_AUTOUPDATER"),
                         std::string("1"));
        DOCTEST_CHECK_EQ(p.model, std::string("m-default[1M]"));
        cleanupProfileKey(kTestKeychainService, p.id);
    }

    mod->onStop();
    return true;
})

// 6. 「导入当前配置」按钮存在 + 读 settings.json → 弹窗预填全链
// 点按钮 = 等价用户手动「粘贴导入」但全自动：临时 settings.json（Paths 注入）
// → 弹窗打开 → 基础页名称建议/BASE_URL/Token、模型映射页槽位、高级 env 页
// env 行 + 预览标记 → 保存 → DB 往返。
REGISTER_TEST(claude_module_import_current_full_chain,
              "claude_module: 导入当前配置读settings.json预填表单（headless）", {
    TestEnv env;
    test_support::TempDir d("full", kImportTempDirPrefix);
    const auto paths = importPaths(d, env.handle());
    env.svc->setSettingsSyncPaths(paths);
    env.svc->setSettingsTakeOverEnabled(true);
    // 整段 settings.json 形状（含非 env 顶层字段，验证取 env 子对象）
    d.write(d.file("settings.json"), R"({
        "permissions": {"allow": ["Bash"]},
        "env": {
            "ANTHROPIC_BASE_URL": "https://gw.example.com/v1",
            "ANTHROPIC_AUTH_TOKEN": "sk-imp-current-0001",
            "ANTHROPIC_MODEL": "m-cur-default",
            "ANTHROPIC_DEFAULT_SONNET_MODEL": "m-cur-sonnet",
            "DISABLE_AUTOUPDATER": "1"
        }
    })");

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_imp_cur");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing:") + id + ">"
                               : node->text;
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.profiles.import") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.profiles.import.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* backdrop = ui.find("claude.dialog.form.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(!backdrop->disabled);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.name.text"),
                         std::string("导入 gw.example.com"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.url.text"),
                         std::string("https://gw.example.com/v1"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.key.text"),
                         std::string("sk-imp-current-0001"));
        DOCTEST_CHECK(ui.find("claude.dialog.form.importwarn") == nullptr);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.1"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.slot.0.model.text"),
                         std::string("m-cur-default"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.slot.1.model.text"),
                         std::string("m-cur-sonnet"));
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.env.row.0.key.text"),
                         std::string("DISABLE_AUTOUPDATER"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.env.row.0.value.text"),
                         std::string("1"));
        const auto* preview = ui.find("claude.form.import.preview");
        DOCTEST_REQUIRE(preview != nullptr);
        const std::string slotTag =
            "槽位 2/" + std::to_string(module::claude::modelSlotTable().size());
        DOCTEST_CHECK(preview->text.find(slotTag) != std::string::npos);
        DOCTEST_CHECK(preview->text.find("env 1 项") != std::string::npos);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
        const auto& p = list.value()[0];
        DOCTEST_CHECK_EQ(p.name, std::string("导入 gw.example.com"));
        DOCTEST_CHECK_EQ(p.baseUrl, std::string("https://gw.example.com/v1"));
        DOCTEST_CHECK(!p.apiKeyHint.empty());
        auto mc = module::claude::parseModelsConfig(p.modelsConfigJson);
        DOCTEST_REQUIRE(mc.isOk());
        DOCTEST_CHECK_EQ(mc.value().def.model, std::string("m-cur-default"));
        DOCTEST_CHECK_EQ(mc.value().sonnet.model, std::string("m-cur-sonnet"));
        auto ee = module::claude::parseExtraEnv(p.extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("DISABLE_AUTOUPDATER"), std::string("1"));
        DOCTEST_CHECK_EQ(p.model, std::string("m-cur-default"));
        cleanupProfileKey(kTestKeychainService, p.id);
    }

    mod->onStop();
    return true;
})

// 6b.「导入当前配置」必须读入高级配置
// parseImportedEnv 按其职责只取 env 子对象，doApplyImport 也只回填基础/槽位/env
// 三域——非 env 顶层属性没有任何导入路径读它。startImportCurrentConfig 抽取
// 非 env 顶层属性写入 formExtraSettingsJson_（与源一致，pretty 规范形）。
REGISTER_TEST(claude_module_import_current_reads_advanced_json,
              "claude_module: 导入当前配置读入高级配置（非 env 顶层属性→高级设置 JSON，headless）", {
    TestEnv env;
    test_support::TempDir d("adv", kImportTempDirPrefix);
    const auto paths = importPaths(d, env.handle());
    env.svc->setSettingsSyncPaths(paths);
    env.svc->setSettingsTakeOverEnabled(true);
    // 整段 settings.json：env（基础/槽位域）+ 三个非 env 顶层属性（高级配置域）
    d.write(d.file("settings.json"), R"({
        "permissions": {"allow": ["Bash"]},
        "statusLine": {"type": "command", "command": "~/sl.sh"},
        "model": "m-adv",
        "env": {
            "ANTHROPIC_BASE_URL": "https://adv.example.com",
            "ANTHROPIC_AUTH_TOKEN": "sk-imp-adv-0001"
        }
    })");

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_imp_adv");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    // 多行输入框全文（逐行 .text.N 拼接）
    auto joinedText = [](eui::Ui& ui,
                         const std::string& prefix) -> std::string {
        std::string out;
        for (size_t i = 0;; ++i) {
            const auto* n = ui.find(prefix + std::to_string(i));
            if (n == nullptr) break;
            if (!out.empty()) out += "\n";
            out += n->text;
        }
        return out;
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profiles.import.bg"));
    }
    // 高级配置页：缓冲 = 源文件非 env 顶层属性（env 键不外泄——它属 env 域）。
    // 多行 pretty 会超出表单小框的可视行数（输入框只渲染视口内的行节点），故经
    // 「全屏编辑」读**全量**缓冲。
    std::string advText;
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        advText = joinedText(ui, "claude.jsonfs.editor.text.");
        DOCTEST_CHECK(advText.find("permissions") != std::string::npos);
        DOCTEST_CHECK(advText.find("statusLine") != std::string::npos);
        DOCTEST_CHECK(advText.find("m-adv") != std::string::npos);
        DOCTEST_CHECK(advText.find("\"env\"") == std::string::npos);
    // 逐键与源一致（解析后比对，格式无关）
        auto parsed = platform::json::parse(advText);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_REQUIRE(parsed.value().isObject());
        auto obj = parsed.value().asObject();
        DOCTEST_CHECK(obj.has("permissions"));
        DOCTEST_CHECK(obj.has("statusLine"));
        DOCTEST_CHECK(obj.has("model"));
        DOCTEST_CHECK(!obj.has("env"));
        DOCTEST_CHECK_EQ(obj.get("model").asString(), std::string("m-adv"));
        DOCTEST_CHECK_EQ(obj.get("statusLine").asObject().get("command").asString(),
                         std::string("~/sl.sh"));
    // 「放弃修改」回落到基线 = 导入预填值
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);
        DOCTEST_CHECK(ui.find("claude.jsonfs.editor.text.0") == nullptr);
    // 表单小框仍在、首行 = pretty 的 "{"（缓冲未被清空——放弃 = 回导入基线）
        const auto* first = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(first != nullptr);
        DOCTEST_CHECK_EQ(first->text, std::string("{"));
    }
    // 保存 → 落库 → 详情「高级配置」pane 逐键显示同一组键
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
        // 轮询必须等**渲染快照**（等写库而非快照 = 快照竞态 flaky）
    bool saved = false;
    bool cardReady = false;  // 快照收割完成：卡片已进渲染树（下一步 click 依赖）
    int64_t pid = 0;
    for (int i = 0; i < 300 && !(saved && cardReady); ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui);
        if (!cardReady) {
            cardReady = ui.find("claude.profile.0.bg") != nullptr;
        }
        auto list = env.svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.name.find("adv.example.com") != std::string::npos) {
                    pid = p.id;
                    saved = p.extraSettingsJson.find("statusLine") !=
                            std::string::npos;
                }
            }
        }
        if (!(saved && cardReady)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(saved);  // 高级配置真落库（不只是 UI 显示）
    DOCTEST_CHECK(cardReady);  // 快照收割完成（卡片进渲染树，可点）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
    // 详情「高级配置」pane：逐键来源行（键名节点）
        std::vector<std::string> keys;
        for (size_t i = 0;; ++i) {
            const auto* n =
                ui.find("claude.advjson.row.key." + std::to_string(i));
            if (n == nullptr) break;
            keys.push_back(n->text);
        }
        DOCTEST_CHECK(std::find(keys.begin(), keys.end(), "model") != keys.end());
        DOCTEST_CHECK(std::find(keys.begin(), keys.end(), "permissions") !=
                      keys.end());
        DOCTEST_CHECK(std::find(keys.begin(), keys.end(), "statusLine") !=
                      keys.end());
        DOCTEST_CHECK(std::find(keys.begin(), keys.end(), "env") == keys.end());
    }

    if (pid != 0) cleanupProfileKey(kTestKeychainService, pid);
    mod->onStop();
    return true;
})

    // 7.「导入当前配置」文件缺失 → toast 提示，不弹窗
REGISTER_TEST(claude_module_import_current_missing_file,
              "claude_module: 导入当前配置文件缺失toast不弹窗（headless）", {
    TestEnv env;
    test_support::TempDir d("miss", kImportTempDirPrefix);
    const auto paths = importPaths(d, env.handle());   // settings.json 不创建
    env.svc->setSettingsSyncPaths(paths);
    env.svc->setSettingsTakeOverEnabled(true);

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_imp_miss");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profiles.import.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("导入失败"));
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("未找到") != std::string::npos);
        DOCTEST_CHECK(msg->text.find("settings.json") != std::string::npos);
        // 弹窗未开、表单未预填（input 空值时渲染 placeholder，非空才是预填）
        const auto* backdrop = ui.find("claude.dialog.form.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(backdrop->disabled);
        const auto* name = ui.find("claude.dialog.form.name.text");
        DOCTEST_REQUIRE(name != nullptr);
        DOCTEST_CHECK_EQ(name->text, std::string("例如：官方 / 中转 A"));
    }

    // 补充分支：settings.json 存在但没有 env 块 → 明确提示（而非共享解析器
    d.write(d.file("settings.json"), R"({"permissions": {"allow": ["Bash"]}})");
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profiles.import.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("导入失败"));
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("env 块") != std::string::npos);
        const auto* backdrop = ui.find("claude.dialog.form.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(backdrop->disabled);
    }

    mod->onStop();
    return true;
})

// 8.「导入当前配置」接管态 → 弹窗内警告 + 仍预填（不阻断）
// sidecar 存在 = 接管态：导入的是代理托管值而非原始上游 → 警告行展示，
// 弹窗照开照预填，由用户决定。
REGISTER_TEST(claude_module_import_current_takeover_warn,
              "claude_module: 导入当前配置接管态警告不阻断（headless）", {
    TestEnv env;
    test_support::TempDir d("takeover", kImportTempDirPrefix);
    const auto paths = importPaths(d, env.handle());
    env.svc->setSettingsSyncPaths(paths);
    env.svc->setSettingsTakeOverEnabled(true);
    // 接管态：settings.json 是代理托管值 + sidecar 残留
    d.write(d.file("settings.json"), R"({
        "env": {
            "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
            "ANTHROPIC_AUTH_TOKEN": "devdesk-proxy-token",
            "MY_KEY": "user"
        }
    })");
    // sidecar 是 app_config 表的 claude.settings_snapshot 行（整份一行）
    DOCTEST_REQUIRE(test_support::writeRawConfigRow(
        &env.handle(), module::claude::settings_sync::kSidecarConfigKey,
        R"({"env": {"ANTHROPIC_BASE_URL": "https://orig.example.com"}})"));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_imp_takeover");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing:") + id + ">"
                               : node->text;
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profiles.import.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* backdrop = ui.find("claude.dialog.form.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(!backdrop->disabled);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.url.text"),
                         std::string("http://127.0.0.1:8888"));
        const auto* warn = ui.find("claude.dialog.form.importwarn");
        DOCTEST_REQUIRE(warn != nullptr);
        DOCTEST_CHECK(warn->text.find("接管态") != std::string::npos);
        DOCTEST_CHECK(warn->text.find("代理配置") != std::string::npos);
        DOCTEST_CHECK(warn->text.find("停止代理") != std::string::npos);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.env.row.0.key.text"),
                         std::string("MY_KEY"));
    }

    mod->onStop();
    return true;
})

// 9. 调用记录弹窗：搜索 / 状态码过滤 / 翻页
// 25 条日志 → 弹窗每页 20 条 + 搜索框（path/method 模糊）+ 状态码下拉 +
// 上下页；过滤条件跨开关保留
REGISTER_TEST(claude_module_calls_dialog,
              "claude_module: 调用记录弹窗搜索/状态码过滤/翻页（headless）", {
    TestEnv env;
    auto p = env.svc->createProfile("日志弹窗", "https://l.example.com",
                                    "sk-ant-logdlg-01", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    const std::time_t base = std::time(nullptr);

    // 20×200 messages + 3×429 complete + 2×超时（status 0 带 error）
    // DOCTEST 宏在 lambda 内 return false → 显式 bool 返回，调用点传播
    auto addRow = [&](const std::string& m, const std::string& path, int st,
                      const std::string& err, std::time_t ts) -> bool {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = pid;
        e.method = m;
        e.path = path;
        e.statusCode = st;
        e.error = err;
        e.latencyMs = 120;
        e.startedAt = ts;
        DOCTEST_REQUIRE(env.svc->logCall(e).isOk());
        return true;
    };
    for (int i = 0; i < 20; ++i) {
        DOCTEST_REQUIRE(addRow("POST", "/v1/messages", 200, "", base + i));
    }
    for (int i = 0; i < 3; ++i) {
        DOCTEST_REQUIRE(addRow("POST", "/v1/complete", 429, "", base + 100 + i));
    }
    for (int i = 0; i < 2; ++i) {
        DOCTEST_REQUIRE(addRow("GET", "/v1/hold", 0, "connection timeout",
                               base + 200 + i));
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_calls_rt");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& txt) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = txt;
        node->onTextInput(ev);
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing>") : node->text;
    };
    // 取数已后台化（开弹窗 / 改过滤 / 翻页后结果下一帧起才可见），断言前先有界
    // 轮询——每轮新起一帧 compose（帧内收割后台结果），不靠固定睡眠
    auto waitFrame = [&](auto&& pred) -> bool {
        for (int i = 0; i < 400; ++i) {
            eui::Ui ui;
            compose(ui);
            if (pred(ui)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    auto waitCount = [&](const std::string& want) -> bool {
        return waitFrame([&](eui::Ui& ui) {
            return textOf(ui, "claude.dialog.calls.count") == want;
        });
    };
    auto waitRows = [&](int n) -> bool {   // 当页恰有 n 行
        return waitFrame([&](eui::Ui& ui) {
            return (n == 0 || ui.find("claude.dialog.calls.row." +
                                       std::to_string(n - 1) + ".wrap") != nullptr) &&
                   ui.find("claude.dialog.calls.row." + std::to_string(n) +
                           ".wrap") == nullptr;
        });
    };

    // 1. 点「查看全部 →」开弹窗。调用记录是详情面板第 5 页签（idx 4）：先切 tab
    // 该帧，再点「查看全部」
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.4"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.calls.viewall.hit"));
    }
    // 2. 弹窗数据到齐（等后台结果）：搜索框 / 状态下拉 / scrollView / 首页 20 行
    DOCTEST_REQUIRE(waitCount("25 条匹配"));
    DOCTEST_REQUIRE(waitRows(20));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.search.hit") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.status.ddwrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.scroll") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.0.wrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.19.wrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.20.wrap") == nullptr);
        DOCTEST_CHECK(textOf(ui, "claude.dialog.calls.row.0").find("/v1/hold") !=
                      std::string::npos);
        DOCTEST_CHECK(textOf(ui, "claude.dialog.calls.row.0").find("connection timeout") !=
                      std::string::npos);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("25 条匹配"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.page"),
                         std::string("第 1 / 2 页"));
        // disabled/onClick 都挂在按钮的 .bg rect 上
        DOCTEST_CHECK(ui.find("claude.dialog.calls.prev.bg")->disabled);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.next.bg"));
    }
    DOCTEST_REQUIRE(waitRows(5));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.4.wrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.5.wrap") == nullptr);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.page"),
                         std::string("第 2 / 2 页"));
        DOCTEST_CHECK(textOf(ui, "claude.dialog.calls.row.0").find("/v1/messages") !=
                      std::string::npos);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.next.bg")->disabled);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.prev.bg"));
    }
    DOCTEST_REQUIRE(waitRows(20));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.page"),
                         std::string("第 1 / 2 页"));
    }
    // 5. 搜索框过滤（path 子串）：3 条命中，单页。按键 → 防抖窗口满后一次后台
    // 查询 → 等结果再断言
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.calls.search.hit", "complete"));
    }
    DOCTEST_REQUIRE(waitCount("3 条匹配"));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("3 条匹配"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.page"),
                         std::string("第 1 / 1 页"));
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.2.wrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.3.wrap") == nullptr);
        DOCTEST_CHECK(textOf(ui, "claude.dialog.calls.row.0").find("429") !=
                      std::string::npos);
    }
    // 6. 状态码下拉：4xx ∧ 搜索 complete → 3；切 2xx ∧ 搜索 → 0（空态）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.status.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.status.dd.item.3") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.status.dd.item.3"));  // 4xx
    }
        // complete 全是 429 → 4xx 命中不变（下拉立即重查：等一轮后台结果落地）
    DOCTEST_REQUIRE(waitFrame([&](eui::Ui& ui) {
        return ui.find("claude.dialog.calls.row.2.wrap") != nullptr &&
               ui.find("claude.dialog.calls.row.3.wrap") == nullptr;
    }));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("3 条匹配"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.status.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.status.dd.item.1"));  // 2xx
    }
    DOCTEST_REQUIRE(waitCount("0 条匹配"));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("0 条匹配"));
        DOCTEST_CHECK(ui.find("claude.dialog.calls.empty") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.scroll") == nullptr);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.status.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.status.dd.item.0"));
    }
    DOCTEST_REQUIRE(waitCount("3 条匹配"));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("3 条匹配"));
    }
    // 8. 关闭再打开：过滤条件保留（会话内记忆），页码回第一页。dialog 关闭只是收起，
    // content 节点树仍在，不以此断言开关。
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.close.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.4"));  // tab 保持 4（幂等）
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.calls.viewall.hit"));
    }
    DOCTEST_REQUIRE(waitFrame([&](eui::Ui& ui) {
        return textOf(ui, "claude.dialog.calls.count") == std::string("3 条匹配") &&
               ui.find("claude.dialog.calls.row.2.wrap") != nullptr;
    }));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("3 条匹配"));
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.2.wrap") != nullptr);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 9b. 调用记录弹窗取数后台化 + 搜索防抖（渲染路径零 DB）
// 开弹窗 / 搜索框逐键在回调里同步查库（queryCalls 分页行 + countCalls 总数
// 两条 SQL），与代理 worker 持同一把 SQLite 锁时会卡帧。渲染路径不做 IO/DB 是
// 硬约束——本用例把它锁成可回归的行为：
//   1. 弹窗关着 compose 多帧零查询；
//   2. 开弹窗那一帧只发意图：数据未到（加载态）、恰好派发一次；查询由 worker
//      线程跑完才回填（渲染线程同步查库会在线程 id 上现形）；
//   3. 空闲帧不重复派发；数据由后台结果回填（下一帧起可见）；
//   4. 搜索框同一防抖窗口内 3 次按键只派发一次查询，停手后按最终文本出结果。
// 查询次数走测试缝计数（不靠墙钟 / 不靠固定睡眠）
REGISTER_TEST(claude_cpx06_calls_query_async_debounce,
              "claude_module: 调用记录取数后台化 + 搜索防抖单次派发", {
    TestEnv env;
    auto p = env.svc->createProfile("异步弹窗", "https://async.example.com",
                                    "sk-ant-cpx06-01", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    const std::time_t base = std::time(nullptr);
    auto addRow = [&](const std::string& m, const std::string& path, int st,
                      const std::string& err, std::time_t ts) -> bool {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = pid;
        e.method = m;
        e.path = path;
        e.statusCode = st;
        e.error = err;
        e.latencyMs = 5;
        e.startedAt = ts;
        DOCTEST_REQUIRE(env.svc->logCall(e).isOk());
        return true;
    };
    // 2×messages + 1×complete + 1×hold（共 4 条；搜 "com" 只命中 complete）
    DOCTEST_REQUIRE(addRow("POST", "/v1/messages", 200, "", base + 1));
    DOCTEST_REQUIRE(addRow("POST", "/v1/messages", 200, "", base + 2));
    DOCTEST_REQUIRE(addRow("POST", "/v1/complete", 429, "", base + 3));
    DOCTEST_REQUIRE(addRow("GET", "/v1/hold", 0, "timeout", base + 4));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_cpx06");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& txt) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = txt;
        node->onTextInput(ev);
        return true;
    };
    auto countText = [](eui::Ui& ui) -> std::string {
        const auto* n = ui.find("claude.dialog.calls.count");
        return n == nullptr ? std::string("<missing>") : n->text;
    };

    const int q0 = claude_calls_query_dispatches_for_test();
    const unsigned long long selfThread = static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // ① 弹窗关着：compose 多帧不查库（不预查、不按帧轮询）
    for (int i = 0; i < 5; ++i) {
        eui::Ui ui;
        compose(ui);
    }
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q0);

    {   // 开弹窗帧：调用记录在详情面板第 5 页签里（idx 4），先切 tab（同帧不派发）
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(click(ui, "claude.detail.tabs.hit.4"));
    }
    // ② 开弹窗那一帧：只发意图（查询在后台）——数据未到 + 恰好派发一次。同步查库
    // 会让本帧就把行画出来，且派发计数按每次点击 +1 而非 +1/帧。
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(click(ui, "claude.calls.viewall.hit"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q0 + 1);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.loading") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.0.wrap") == nullptr);
    }
    // ③ 后台结果回填（下一帧起可见，有界等待）——到这一步查询确实已在非渲染
    //    线程跑完（线程 id 是 worker 写的，同步查库会让它等于渲染线程）
    {
        bool loaded = false;
        for (int i = 0; i < 200 && !loaded; ++i) {
            eui::Ui ui;
            compose(ui);
            loaded = ui.find("claude.dialog.calls.row.0.wrap") != nullptr;
            if (!loaded) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        DOCTEST_CHECK(loaded);
    }
    DOCTEST_CHECK(claude_calls_query_last_thread_for_test() != selfThread);
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q0 + 1);
    // ④ 空闲帧零派发（一次置脏 = 一次查询，不按帧重查）
    for (int i = 0; i < 5; ++i) {
        eui::Ui ui;
        compose(ui);
    }
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q0 + 1);

    // ⑤ 搜索防抖：同一窗口内逐键 3 次 → 只派发一次查询（关键计数断言）
    const int q1 = claude_calls_query_dispatches_for_test();
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(type(ui, "claude.dialog.calls.search.hit", "c"));
        DOCTEST_REQUIRE(type(ui, "claude.dialog.calls.search.hit", "o"));
        DOCTEST_REQUIRE(type(ui, "claude.dialog.calls.search.hit", "m"));
    }
    {  // 防抖窗口内的一帧：未派发，快照沿用上次值（不闪空 / 不逐键查）
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q1);
        DOCTEST_CHECK_EQ(countText(ui), std::string("4 条匹配"));
    }
    {  // 停手满一个窗口 → 一次查询按最终文本 "com" 出结果
        bool searched = false;
        for (int i = 0; i < 300 && !searched; ++i) {
            eui::Ui ui;
            compose(ui);
            searched = countText(ui) == std::string("1 条匹配");
            if (!searched) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        DOCTEST_CHECK(searched);
    }
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q1 + 1);
    for (int i = 0; i < 5; ++i) {
        eui::Ui ui;
        compose(ui);
    }
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q1 + 1);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 10. Profile 列表 scrollView
// 卡片固定 y=56+i*108 平铺时，8 个 profile 的第 7 张起画出左栏、叠底部按钮，
// 最小窗高 720 下第 4 张即溢出。整列表进 scrollView，超界可滚、全可达。
REGISTER_TEST(claude_profiles_scrollview,
              "claude_module: Profile 列表超长可滚动不压按钮（headless）", {
    TestEnv env;
    std::vector<int64_t> ids;
    for (int i = 0; i < 8; ++i) {
        auto p = env.svc->createProfile("站" + std::to_string(i),
                                        "https://p" + std::to_string(i) + ".example.com",
                                        "sk-ant-scroll-0" + std::to_string(i), "m");
        DOCTEST_REQUIRE(p.isOk());
        ids.push_back(p.value().id);
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    auto composeAt = [&](eui::Ui& ui, float h) {
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = h;
        ui.begin("claude_scroll");
        mod->compose(ui, screen);
        ui.end();
    };

    // 默认高度 1000：scrollView 存在，8 张卡全在滚动内容区，底部按钮照常
    {
        eui::Ui ui;
        composeAt(ui, 1000.0f);
        DOCTEST_CHECK(ui.find("claude.profiles.scroll") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.0.wrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.7.wrap") != nullptr);
        const auto* name7 = ui.find("claude.profile.7.name");
        DOCTEST_REQUIRE(name7 != nullptr);
        DOCTEST_CHECK_EQ(name7->text, std::string("站7"));
        DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profiles.import") != nullptr);
    }
    // 最小高度 720：末卡滚到底可达且可点选。视口让出共享区块高（~76px）后，
    // 7×108=756 的末卡起点在视口外——virtualList 只渲染可见行 + overscan，
    // 先滚到底（钳到 maxOffset）再断言树内 + 可点选。
    {
        eui::Ui ui;
        composeAt(ui, 720.0f);
        DOCTEST_CHECK(ui.find("claude.profiles.scroll") != nullptr);
        const auto* root = ui.find("claude.profiles.scroll");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_REQUIRE(root->onScrollOffsetChanged != nullptr);
        root->onScrollOffsetChanged(2000.0f);
        composeAt(ui, 720.0f);
        const auto* node = ui.find("claude.profile.7.bg");
        DOCTEST_REQUIRE(node != nullptr);
        DOCTEST_REQUIRE(node->onClick != nullptr);
        node->onClick();
    }
    {
        eui::Ui ui;
        composeAt(ui, 720.0f);
        const auto* nameV = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameV != nullptr);
        DOCTEST_CHECK_EQ(nameV->text, std::string("站7"));
    }

    mod->onStop();
    for (size_t i = 0; i < ids.size(); ++i) cleanupProfileKey(kTestKeychainService, ids[i]);
    return true;
})

// 共享配置区块必须整体位于 virtualList 视口 clip 之外
// 视口若按 scrollH 全高、区块只压自己的绝对 Y，后建区块会叠在带 clip 的
// 视口底带上 → ≥5 profile 时末 1-2 张卡被盖、滚到底显示不全、点击误开片段编辑。
// 视口高度 = listH（让出区块高 + 12px），区块钉在 clip 之外。
REGISTER_TEST(claude_shared_block_outside_viewport,
              "claude_module: 共享配置区块钉在 virtualList 视口之外（审计修复）", {
    TestEnv env;
    auto f1 = env.svc->createSharedConfig("片段A", R"({"MY_A":"1"})");
    DOCTEST_REQUIRE(f1.isOk());
    std::vector<int64_t> ids;
    for (int i = 0; i < 8; ++i) {
        auto p = env.svc->createProfile(
            "站" + std::to_string(i),
            "https://p" + std::to_string(i) + ".example.com",
            "sk-ant-vp-000" + std::to_string(i), "m");
        DOCTEST_REQUIRE(p.isOk());
        ids.push_back(p.value().id);
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    auto composeAt = [&](eui::Ui& ui, float h) {
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = h;
        ui.begin("claude_vp");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);  // 元素 frame 需显式布局
    };

    // 三种高度（默认 / 最小窗 / 超短窗）：区块顶 ≥ 视口底 12px，且区块落在带 clip
    // 的视口之外——不盖末卡、不误点片段；720 以上窗口区块底 == 底部按钮行上沿 − 12
    //（**钉底**，新建/导入同排同 y）。
    // 500 属极端矮窗（低于最小窗）：区块仍钉底、不向下溢出按钮区（该档下区块会被
    // 压扁到自然高以下，LOG_ERROR 记录），本用例只锁锚点关系。
    for (float h : {1000.0f, 720.0f, 500.0f}) {
        eui::Ui ui;
        composeAt(ui, h);
        const auto* scroll = ui.find("claude.profiles.scroll");
        const auto* block = ui.find("claude.shared.block");
        DOCTEST_REQUIRE(scroll != nullptr);
        DOCTEST_REQUIRE(block != nullptr);
        const float viewportBottom = scroll->frame.y + scroll->frame.height;
        DOCTEST_CHECK(block->frame.y - viewportBottom >= 11.5f);
        if (h >= 720.0f) {
            const auto* btn = ui.find("claude.profiles.new");
            DOCTEST_REQUIRE(btn != nullptr);
            DOCTEST_CHECK(block->frame.y + block->frame.height <=
                          btn->frame.y + 0.5f);
        }
    }

    mod->onStop();
    for (size_t i = 0; i < ids.size(); ++i) cleanupProfileKey(kTestKeychainService, ids[i]);
    return true;
})

REGISTER_TEST(claude_module_form_esc_cancels,
              "claude_module: 表单基础页/模型页输入框 Esc=取消不落库（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto profileCount = [&](void) -> size_t {
        auto r = env.svc->listProfiles();
        return r.isOk() ? r.value().size() : 0;
    };
    DOCTEST_REQUIRE_EQ(profileCount(), 0u);

    // CHECK 宏内含 return false，不能进 void lambda —— helper 返回 bool，由测试体的
    // DOCTEST_CHECK 收敛（本文件既有约定）
    auto openForm = [&](void) -> bool {
        eui::Ui ui;
        ui.begin("fe_open");
        mod->compose(ui, screen);
        ui.end();
        auto* btn = ui.find("claude.profiles.new.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_REQUIRE(btn->onClick != nullptr);
        btn->onClick();
        return true;
    };
    auto typeInto = [&](const char* page, const std::string& id,
                        const std::string& text) -> bool {
        eui::Ui ui;
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
        auto* hit = ui.find(id + ".hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        core::TextInputEvent ev;
        ev.text = text;
        hit->onTextInput(ev);
        return true;
    };
    auto pressEsc = [&](const char* page, const std::string& id) -> bool {
        eui::Ui ui;
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
        auto* hit = ui.find(id + ".hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(hit->onKeyEvent(esc));  // onCancel 消费
        eui::Ui ui2;  // 新帧确认弹窗关闭（每帧新建 Ui，旧指针失效）
        ui2.begin(page);
        mod->compose(ui2, screen);
        ui2.end();
        const auto* panel = ui2.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
        return true;
    };

    // 1) 基础页：填名称/URL → 名称框 Esc → 关表单，profile 不落库
    DOCTEST_CHECK(openForm());
    DOCTEST_CHECK(typeInto("fe_b1", "claude.dialog.form.name", "Esc取消表单"));
    DOCTEST_CHECK(typeInto("fe_b2", "claude.dialog.form.url", "https://esc.example.com"));
    DOCTEST_CHECK(pressEsc("fe_b3", "claude.dialog.form.name"));
    DOCTEST_CHECK_EQ(profileCount(), 0u);

    // 2) 模型页：重开 → 切 tab1 → 槽位输入框 Esc → 同样关表单不落库
    DOCTEST_CHECK(openForm());
    {
        eui::Ui ui;
        ui.begin("fe_m1");
        mod->compose(ui, screen);
        ui.end();
        auto* tab = ui.find("claude.form.tabs.hit.1");
        DOCTEST_REQUIRE(tab != nullptr);
        DOCTEST_REQUIRE(tab->onClick != nullptr);
        tab->onClick();
    }
    DOCTEST_CHECK(typeInto("fe_m2", "claude.form.slot.0.model", "esc/model"));
    DOCTEST_CHECK(pressEsc("fe_m3", "claude.form.slot.0.model"));
    DOCTEST_CHECK_EQ(profileCount(), 0u);

    mod->onStop();
    return true;
})

// Claude 页级快捷键 Ctrl+N / Ctrl+S / Ctrl+Enter（headless）
// 通道 = Module::onKeyEvent（模拟 app 级 onKeyEvent → Router 派发到当前页模块）。
// 端口动态取空闲端口避让冲突。
REGISTER_TEST(claude_module_keyboard_shortcuts,
              "claude: Ctrl+N 新建 / Ctrl+S 保存表单 / Ctrl+Enter 启停代理", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    auto compose = [&](eui::Ui& ui, const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    // 快捷键修饰位按平台取（见 setShortcutModifier）
    auto ctrlPress = [](eui::InputKey k) {
        eui::KeyEvent e;
        e.key = k;
        e.action = eui::KeyAction::Press;
        test_support::setShortcutModifier(e.modifiers);
        return e;
    };
    auto type = [](eui::Ui& ui, const std::string& nodeId,
                   const std::string& text) {
        const auto* n = ui.find(nodeId);
        if (n == nullptr || n->onTextInput == nullptr) {
            return;
        }
        core::TextInputEvent ev;
        ev.text = text;
        n->onTextInput(ev);
    };

    // 1) Ctrl+N 打开新建表单（dialog 子节点常驻构建树，开合用 panel opacity 判定）
    DOCTEST_CHECK(mod->onKeyEvent(ctrlPress(eui::InputKey::N)));
    {
        eui::Ui ui;
        compose(ui, "ks1");
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 1.0f);
        DOCTEST_CHECK(ui.find("claude.dialog.form.name.hit") != nullptr);
        type(ui, "claude.dialog.form.name.hit", "快捷键建档");
    }
    // 2) 表单开着再按 Ctrl+N → 不消费（不得重置草稿）；Ctrl+Enter → 不消费
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlPress(eui::InputKey::N)));
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlPress(eui::InputKey::Enter)));
    DOCTEST_CHECK(!env.svc->isProxyRunning());
    {
        eui::Ui ui;
        compose(ui, "ks2");
        const auto* name = ui.find("claude.dialog.form.name.text");
        DOCTEST_REQUIRE(name != nullptr);
        DOCTEST_CHECK_EQ(name->text, std::string("快捷键建档"));
        type(ui, "claude.dialog.form.url.hit", "https://kbd.example.com");
        type(ui, "claude.dialog.form.key.hit", "sk-ant-kbd-00001");
    }
    // 3) Ctrl+S = 点「保存/创建」按钮：建档落库、表单关闭
    DOCTEST_CHECK(mod->onKeyEvent(ctrlPress(eui::InputKey::S)));
    auto listed = env.svc->listProfiles();
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_CHECK_EQ(listed.value().size(), 1u);
    DOCTEST_CHECK_EQ(listed.value()[0].name, std::string("快捷键建档"));
    const int64_t pid = listed.value()[0].id;
    {
        eui::Ui ui;
        compose(ui, "ks3");
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);   // 提交成功后表单关闭
    }
    // 4) 表单外 Ctrl+S（无保存对象）、未注册组合、裸键、Alt 修饰、Release 全不消费，
    //    且不得开表单。
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlPress(eui::InputKey::S)));
    DOCTEST_CHECK(!mod->onKeyEvent(ctrlPress(eui::InputKey::X)));
    {
        eui::KeyEvent plain;
        plain.key = eui::InputKey::N;
        plain.action = eui::KeyAction::Press;
        DOCTEST_CHECK(!mod->onKeyEvent(plain));
        eui::KeyEvent withAlt = ctrlPress(eui::InputKey::N);
        withAlt.modifiers.alt = true;
        DOCTEST_CHECK(!mod->onKeyEvent(withAlt));
        eui::KeyEvent release = ctrlPress(eui::InputKey::N);
        release.action = eui::KeyAction::Release;
        DOCTEST_CHECK(!mod->onKeyEvent(release));
    }
    {
        eui::Ui ui;
        compose(ui, "ks4");
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);   // 上述不消费按键不得重开表单
    }

    // 5) Ctrl+Enter 启停：无弹窗开着 → 按一次启动、再按一次停止。
    // 前置校验与手动按钮同入口：起代理前先过「激活 profile + 连通测试」，
    // 且**联网校验段在后台线程**、bind 在 compose 收割帧——故用例先备好
    // 「可达上游 + 激活 profile」，再把「按一次键」改成「按键 + 收割帧轮询」。
    // 断言面不变：启动后监听在配置端口。
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("POST", "/v1/messages",
                   [](const platform::http::RequestCtx&,
                      platform::http::ResponseWriter& w) {
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    // 上面的表单建档 profile 是首建自动激活，但 baseUrl 指向真实网络域名——
    // 换一套指向 mock 上游的 profile 并激活
    auto mockProf = env.svc->createProfile(
        "校验通过", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-kbd-ok", "m");
    DOCTEST_REQUIRE(mockProf.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(mockProf.value().id).isOk());

    int port = 0;
    bool started = false;
    for (int attempt = 0; attempt < 3 && !started; ++attempt) {
        port = pickFreePort();
        DOCTEST_REQUIRE(port > 0);
        env.svc->setConfiguredProxyPort(port);
        DOCTEST_CHECK(mod->onKeyEvent(ctrlPress(eui::InputKey::Enter)));
        started = env.svc->isProxyRunning();   // 同步返回时必为 false（校验在飞）
        // 收割帧轮询：bind 发生在 harvestTestResult（UI 线程、join 之后）
        for (int i = 0; i < 50 && !started; ++i) {
            eui::Ui ui;
            compose(ui, "ks5");
            started = env.svc->isProxyRunning();
            if (!started) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
    DOCTEST_CHECK(started);
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), port);
    // 停止是同步路径（isProxyRunning 早退分支），不经过线程 / 收割
    DOCTEST_CHECK(mod->onKeyEvent(ctrlPress(eui::InputKey::Enter)));
    DOCTEST_CHECK(!env.svc->isProxyRunning());
    upstream.stop();
    cleanupProfileKey(kTestKeychainService, mockProf.value().id);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 11. compose 数据缓存等价
// compose 每帧同步跑 listProfiles/recentCalls/getActiveProfile 三条查询；
// 缓存命中帧零查询。本用例锁等价契约：
//   1. 首帧（脏）与次帧（缓存命中）渲染结果逐字段一致
//   2. 代理 worker 的 logCall（不经过模块、不标脏）经 callLogVersion 驱动
//      recent 重查 —— 「新调用下一帧可见」不变
//   3. 外部直写 DB 绕过模块：缓存帧不可见，onPageEnter 后可见
//   4. 模块写操作（启用）标脏：ACTIVE 徽标与详情即时更新
REGISTER_TEST(claude_module_compose_cache_equivalence,
              "claude_module: compose 缓存等价（脏标记/callLogVersion/onPageEnter）（headless）", {
    TestEnv env;
    auto a = env.svc->createProfile("A缓存", "https://a.example.com",
                                    "sk-ant-cache-aaaa", "m");
    DOCTEST_REQUIRE(a.isOk());
    auto b = env.svc->createProfile("B缓存", "https://b.example.com",
                                    "sk-ant-cache-bbbb", "m");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(b.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("cache");
        mod->compose(ui, screen);
        ui.end();
    };
    // CHECK 宏内含 return false，helper 返回 bool（本文件既有约定）
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing>") : node->text;
    };
    // DB 查询在后台线程 → 「变更可见」最多晚 1 帧（本帧发刷新意图、下帧收割）。
    // 断言可见性改有界轮询等——契约不变（脏标记 / callLogVersion / onPageEnter 各自
    // 仍驱动重查）
    auto composeUntil = [&](auto&& pred) -> bool {
        for (int i = 0; i < 200; ++i) {
            eui::Ui ui;
            compose(ui);
            if (pred(ui)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    // 1) 首帧（脏标记全查）记录渲染，次帧（缓存命中，零 SQLite）必须一致
    std::string name0;
    std::string name1;
    std::string detail;
    {
        eui::Ui ui;
        compose(ui);
        name0 = textOf(ui, "claude.profile.0.name");
        name1 = textOf(ui, "claude.profile.1.name");
        detail = textOf(ui, "claude.field.name.value");
        DOCTEST_CHECK_EQ(name0, std::string("B缓存"));
        DOCTEST_CHECK_EQ(name1, std::string("A缓存"));
        DOCTEST_CHECK_EQ(detail, std::string("B缓存"));
        DOCTEST_CHECK(ui.find("claude.profile.0.active.text") != nullptr);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.profile.0.name"), name0);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.profile.1.name"), name1);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.field.name.value"), detail);
    }
    // 调用记录是详情面板页签（idx 4）：切过去断空态
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.4"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.calls.empty") != nullptr);
    }

    // 2) 代理 worker 落库（logCall 直写，不经过模块）
    {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = a.value().id;
        e.method = "POST";
        e.path = "/v1/cache-probe";
        e.statusCode = 200;
        e.latencyMs = 42;
        e.startedAt = std::time(nullptr);
        DOCTEST_REQUIRE(env.svc->logCall(e).isOk());
        DOCTEST_REQUIRE(composeUntil([](eui::Ui& ui) {
            return ui.find("claude.calls.empty") == nullptr;
        }));
        {
            eui::Ui ui;
            compose(ui);
            DOCTEST_CHECK(textOf(ui, "claude.calls.p.0").find("/v1/cache-probe") !=
                          std::string::npos);
        }
    }

    // 3) 外部直写绕过模块：缓存帧不可见，onPageEnter 后可见
    auto c = env.svc->createProfile("C缓存", "https://c.example.com",
                                    "sk-ant-cache-cccc", "m");
    DOCTEST_REQUIRE(c.isOk());
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.profile.2.name") == nullptr);
    }
    mod->onPageEnter();
    // 脏标记 → 本帧发刷新意图、下帧收割：有界轮询等到 C 可见
    DOCTEST_REQUIRE(composeUntil([](eui::Ui& ui) {
        return ui.find("claude.profile.2.name") != nullptr;
    }));
    {
        eui::Ui ui;
        compose(ui);
        const auto* n2 = ui.find("claude.profile.2.name");   // 序：[B, A, C]
        DOCTEST_REQUIRE(n2 != nullptr);
        DOCTEST_CHECK_EQ(n2->text, std::string("C缓存"));
    }

    // 4) 模块写操作（选中 A → 启用）标脏
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.enable.bg"));
    }
    // 启用写操作标脏 → 后台重查、下帧收割：有界轮询等 A 排到 index 0
    DOCTEST_REQUIRE(composeUntil([](eui::Ui& ui) {
        const auto* n0 = ui.find("claude.profile.0.name");
        return n0 != nullptr && n0->text == std::string("A缓存");
    }));
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.profile.0.name"),
                         std::string("A缓存"));
        DOCTEST_CHECK(ui.find("claude.profile.0.active.text") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.active.text") == nullptr);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    cleanupProfileKey(kTestKeychainService, c.value().id);
    return true;
})

// Esc 关最上层弹窗（删除确认无输入框）
// 表单弹窗的输入框聚焦时 Esc 走 input.onCancel（既有链路，天然让位）；
// 确认删除弹窗没有输入框 → 需要模块级兜底。
REGISTER_TEST(claude_ui_esc_closes_delete_confirm,
              "claude_ui: Esc 关删除确认弹窗（无输入框）", {
    TestEnv env;
    auto created = env.svc->createProfile("Esc目标", "https://esc.example.com",
                                          "sk-ant-esc-00001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t pid = created.value().id;

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    auto frame = [&](const char* page) {
        ui.begin(page);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& u, const std::string& id) -> bool {
        const auto* node = u.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto escPress = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::Escape;
        e.action = eui::KeyAction::Press;
        return e;
    };

    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    // 1) 选中 profile → 右键菜单「删除」→ 确认弹窗（无输入框）→ Esc = 取消，
    frame("esc1");
    DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    frame("esc2");
    {
        const auto* wrap = ui.find("claude.profile.0.wrap");
        DOCTEST_REQUIRE(wrap != nullptr);
        DOCTEST_REQUIRE(wrap->onContextMenu != nullptr);
        eui::PointerEvent pe;
        eui::Rect rect;
        pe.x = 400;
        pe.y = 300;
        wrap->onContextMenu(pe, rect);
    }
    frame("esc2b");
    DOCTEST_CHECK(click(ui, "claude.list.ctx.level.0.item.3"));
    frame("esc3");
    DOCTEST_REQUIRE(ui.find("claude.dialog.delete.panel") != nullptr);
    DOCTEST_CHECK_EQ(ui.find("claude.dialog.delete.panel")->opacity, 1.0f);
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame("esc4");
    DOCTEST_CHECK_EQ(ui.find("claude.dialog.delete.panel")->opacity, 0.0f);
    {
        auto listed = env.svc->listProfiles();
        DOCTEST_REQUIRE(listed.isOk());
        DOCTEST_CHECK_EQ(listed.value().size(), static_cast<size_t>(1));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// compose 只读快照 + DB 查询在后台线程
// refreshDataCache 若在 compose 里同步跑 listProfiles / getActiveProfile /
// recentCalls，会与代理 worker 的 logCall INSERT 争同一把 recursive_mutex
//（事务全程持锁，core/sqlite_db.cpp）→ 代理忙时整帧被卡。
// 渲染路径只发刷新意图 + 搬已就绪结果，查询在后台线程（std::async）。
REGISTER_TEST(claude_h2_compose_snapshot_async,
              "claude_module: compose 只读快照 DB 后台刷新（H2 tick 前旧值/后新值）", {
    TestEnv env;
    auto a = env.svc->createProfile("官方", "https://api.anthropic.com",
                                    "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 探针：跑一帧 compose，返回渲染出的全部 profile 卡名（列表序 =
    auto cardNames = [&]() -> std::vector<std::string> {
        eui::Ui ui;
        ui.begin("claude_h2_probe");
        mod->compose(ui, screen);
        ui.end();
        std::vector<std::string> out;
        for (size_t i = 0;; ++i) {
            const auto* n =
                ui.find("claude.profile." + std::to_string(i) + ".name");
            if (n == nullptr) break;
            out.push_back(n->text);
        }
        return out;
    };
    auto hasName = [](const std::vector<std::string>& names,
                      const char* want) {
        for (const auto& n : names) {
            if (n == want) return true;
        }
        return false;
    };

    // ① 首帧快照非空（= onStart 装载值）：异步首刷不允许闪空
    {
        const auto names = cardNames();
        DOCTEST_REQUIRE_EQ(names.size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(names[0], std::string("官方"));
    }

    // ② 外部新增一条（不经模块写路径）+ 模拟进页 → 置脏
    auto b = env.svc->createProfile("中转", "https://relay.example.com",
                                    "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(b.isOk());
    mod->onPageEnter();
    {
        // tick 前：本帧才发出刷新意图（查询本帧上路），快照恒为上次值。
        const auto names = cardNames();
        DOCTEST_CHECK(hasName(names, "官方"));  // 非空 + = 上次值
        DOCTEST_CHECK(!hasName(names, "中转"));  // 新值未到（确定，非时序）
    }

    // ③ tick 后：后台结果被收割 → 新值可见（有界轮询）
    bool updated = false;
    for (int i = 0; i < 200 && !updated; ++i) {
        updated = hasName(cardNames(), "中转");
        if (!updated) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(updated);
    DOCTEST_CHECK(hasName(cardNames(), "官方"));

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// 详情页「复制」→ 建副本、不激活、选中态不变
// 卡片列表走后台刷新 → 新卡片用有界轮询；toast / 选中态同帧即定。
REGISTER_TEST(claude_module_duplicate_profile_ui,
              "claude_module: 复制键建副本不激活且选中态不变（headless）", {
    TestEnv env;
    auto a = env.svc->createProfile("官方", "https://api.anthropic.com",
                                    "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());
    auto b = env.svc->createProfile("中转", "https://relay.example.com",
                                    "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_dup");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    // 详情动作行不再有「建副本」键（入口只剩右键菜单）。列表序 = is_active DESC,
    // name ASC → index 1 =「中转」，选中它
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.act.dup") == nullptr);
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    // 右键菜单「建副本」（item.1）→ 复制源 = 选中项
    auto dupViaCtxMenu = [&](const char* tag) -> bool {
        eui::Ui ui;
        compose(ui);
        const auto* nameV = ui.find("claude.field.name.value");
        if (nameV == nullptr) return false;
        DOCTEST_CHECK_EQ(nameV->text, std::string("中转"));
        const auto* wrap = ui.find("claude.profile.1.wrap");
        if (wrap == nullptr || wrap->onContextMenu == nullptr) return false;
        eui::PointerEvent pe;
        eui::Rect rect;
        pe.x = 400;
        pe.y = 300;
        wrap->onContextMenu(pe, rect);
        eui::Ui ui2;
        ui2.begin(tag);
        mod->compose(ui2, screen);
        ui2.end();
        const auto* item = ui2.find("claude.list.ctx.level.0.item.1");
        if (item == nullptr || item->onClick == nullptr) return false;
        item->onClick();
        return true;
    };
    DOCTEST_REQUIRE(dupViaCtxMenu("claude_dup_ctx"));
    {
        eui::Ui ui;
        compose(ui);
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("中转 副本") != std::string::npos);
        DOCTEST_CHECK(msg->text.find("未激活") != std::string::npos);
        const auto* nameV = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameV != nullptr);
        DOCTEST_CHECK_EQ(nameV->text, std::string("中转"));
    }
    int64_t copyId = 0;
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), static_cast<size_t>(3));
        for (const auto& p : list.value()) {
            if (p.name == "中转 副本") copyId = p.id;
        }
        DOCTEST_REQUIRE(copyId != 0);
        auto got = env.svc->getProfile(copyId);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().baseUrl,
                         std::string("https://relay.example.com"));
        DOCTEST_CHECK(!got.value().isActive);
        auto key = env.svc->loadApiKey(copyId);
        DOCTEST_REQUIRE(key.isOk());
        DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-bbbb-2222"));
        auto gotA = env.svc->getProfile(a.value().id);
        DOCTEST_REQUIRE(gotA.isOk());
        DOCTEST_CHECK(gotA.value().isActive);
    }
    // UI 层：新卡片出现（后台刷新 → 有界轮询），激活徽标仍在 index 0
    bool sawCopy = false;
    for (int i = 0; i < 250 && !sawCopy; ++i) {
        eui::Ui ui;
        compose(ui);
        const auto* n = ui.find("claude.profile.2.name");
        if (n != nullptr && n->text == "中转 副本") {
            sawCopy = true;
            DOCTEST_CHECK(ui.find("claude.profile.0.active.text") != nullptr);
            DOCTEST_CHECK(ui.find("claude.profile.2.active.text") == nullptr);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    DOCTEST_CHECK(sawCopy);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    cleanupProfileKey(kTestKeychainService, copyId);
    return true;
})

// 选中卡 → 点启用 → settings.json 的值收编进原激活 profile
// 反向同步链路的 UI 侧证据。
REGISTER_TEST(claude_module_ui_enable_captures_settings,
              "claude_module: 详情页启用触发反向同步（headless）", {
    TestEnv env;
    test_support::TempDir d("uienable", kImportTempDirPrefix);
    env.svc->setSettingsSyncPaths(importPaths(d, env.handle()));
    // Claude Code 侧（或 cc-switch）改过的真实上游值：非守卫键 → 会被收编
    d.write(d.file("settings.json"), R"({"env": {
        "ANTHROPIC_BASE_URL": "https://new.example.com/",
        "ANTHROPIC_AUTH_TOKEN": "sk-new-upstream-9876543210"
    }})");

    auto a = env.svc->createProfile("源A", "https://old.example.com",
                                    "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 首次激活：无激活 profile → 跳过（不读文件）
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());
    auto b = env.svc->createProfile("目标B", "https://b.example.com",
                                    "sk-b-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(b.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_ui_rev");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    // 选中「目标B」（列表序：激活的 A 在前 → index 1）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* nameV = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameV != nullptr);
        DOCTEST_CHECK_EQ(nameV->text, std::string("目标B"));
        DOCTEST_CHECK(click(ui, "claude.act.enable.bg"));
    }
    {
        auto gotA = env.svc->getProfile(a.value().id);
        DOCTEST_REQUIRE(gotA.isOk());
        DOCTEST_CHECK_EQ(gotA.value().baseUrl,
                         std::string("https://new.example.com"));
        auto keyA = env.svc->loadApiKey(a.value().id);
        DOCTEST_REQUIRE(keyA.isOk());
        DOCTEST_CHECK_EQ(keyA.value(),
                         std::string("sk-new-upstream-9876543210"));
        auto gotB = env.svc->getProfile(b.value().id);
        DOCTEST_REQUIRE(gotB.isOk());
        DOCTEST_CHECK(gotB.value().isActive);
        DOCTEST_CHECK_EQ(gotB.value().baseUrl, std::string("https://b.example.com"));
        DOCTEST_CHECK(!gotA.value().isActive);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// 接管失败 / 上层覆盖警告在 Claude 页详情可见
// 失败原因与覆盖警告并入 ClaudeService::lastSyncWarnings_。详情页只在「已接管」
// 分支渲染警告时，接管失败（无 sidecar → 未接管）用户只看到「未接管」而看不到原因。
// 空警告时不显示（不留空占位）。
REGISTER_TEST(claude_module_takeover_warn_visible,
              "claude_module: 接管失败警告在详情页可见（headless）", {
    TestEnv env;
    test_support::TempDir d("warnfail", kImportTempDirPrefix);
    env.svc->setSettingsSyncPaths(importPaths(d, env.handle()));
    env.svc->setSettingsTakeOverEnabled(true);
    // 坏 settings.json（非法 JSON）→ takeOver 解析失败中止，全程不写 sidecar
    d.write(d.file("settings.json"), "{ 这不是合法 JSON");

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_warn_fail");
        mod->compose(ui, screen);
        ui.end();
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.field.takeover.warn") == nullptr);
        DOCTEST_CHECK(ui.find("claude.field.takeover.warn.icon") == nullptr);
    }

    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    DOCTEST_REQUIRE(env.svc->startProxy("127.0.0.1", port).isOk());
    DOCTEST_CHECK(!env.svc->lastSyncWarnings().empty());
    DOCTEST_CHECK(!env.svc->isClaudeCodeManaged());

    // 无需额外刷新调用：下一次 compose 即见警告
    {
        eui::Ui ui;
        compose(ui);
        const auto* warn = ui.find("claude.field.takeover.warn");
        DOCTEST_REQUIRE(warn != nullptr);
        DOCTEST_CHECK(warn->text.find("接管失败") != std::string::npos);
        DOCTEST_CHECK(warn->text.find("解析失败") != std::string::npos);
        DOCTEST_CHECK(ui.find("claude.field.takeover.warn.icon") != nullptr);
        const auto* st = ui.find("claude.field.takeover.value");
        DOCTEST_REQUIRE(st != nullptr);
        DOCTEST_CHECK(st->text.find("未接管") != std::string::npos);
    }

    mod->onStop();
    return true;
})

// 上层覆盖警告（接管成功路径）：多条 → 摘要 + 条数（注入两层 overlay 文件）
REGISTER_TEST(claude_module_takeover_warn_summary,
              "claude_module: 上层覆盖警告条数+摘要（headless）", {
    TestEnv env;
    test_support::TempDir d("warnover", kImportTempDirPrefix);
    auto paths = importPaths(d, env.handle());
    // managed（企业策略）+ project 两层各含冲突 env 键 → 2 条覆盖警告
    paths.overlayFiles = {d.file("managed.json"), d.file("project.json")};
    env.svc->setSettingsSyncPaths(paths);
    env.svc->setSettingsTakeOverEnabled(true);
    d.write(d.file("settings.json"), R"({"env": {"MY_KEY": "user"}})");
    d.write(d.file("managed.json"),
            R"({"env": {"ANTHROPIC_BASE_URL": "https://corp.example.com"}})");
    d.write(d.file("project.json"), R"({"env": {"ANTHROPIC_MODEL": "m-override"}})");

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    DOCTEST_REQUIRE(env.svc->startProxy("127.0.0.1", port).isOk());
    DOCTEST_REQUIRE_EQ(env.svc->lastSyncWarnings().size(),
                       static_cast<size_t>(2));

    eui::Ui ui;
    ui.begin("claude_warn_over");
    mod->compose(ui, screen);
    ui.end();
    const auto* warn = ui.find("claude.field.takeover.warn");
    DOCTEST_REQUIRE(warn != nullptr);
    DOCTEST_CHECK(warn->text.find("覆盖") != std::string::npos);
    DOCTEST_CHECK(warn->text.find("共 2 条") != std::string::npos);

    mod->onStop();
    return true;
})

// 认证字段下拉（鉴权头三态 UI）

REGISTER_TEST(claude_ui_form_auth_field_dropdown_exists,
              "claude_module: 表单基础页认证字段下拉渲染（无服务不崩）", {
    module::claude::ClaudeService::setGlobal(nullptr);   // 干净起点
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    {
        eui::Ui ui;
        compose(ui, "authfield_ns1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "authfield_ns2");
        DOCTEST_CHECK(ui.find("claude.form.basic.authfield.label") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.basic.authfield.hint") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.basic.authfield.ddwrap") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.authfield.dd") != nullptr);
        const auto* label = ui.find("claude.dialog.form.authfield.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("双发（默认）"));
    }

    mod->onStop();
    return true;
})

// UAT3 弹窗双列批：基础页「名称 | 认证字段」同排双列的几何钉值。
// 防的事故：认证字段回退独占一行（x=28、y=314）→ 右列 x/同行 y 断言红；
// 名称/认证还原全宽 704 → 列宽 344 断言红；基础页弹高回 600 → 面板高断言红。
// 高度按 tab 取值是刻意设计：模型/env/高级配置三页既有内容深至 y≈480-516，
// 全局改 532 会把它们压进按钮带（472 起）——高级配置页仍 600 也一并钉住。
REGISTER_TEST(claude_ui_form_basic_two_column_geometry,
              "claude_module: 基础页「名称|认证」同排双列几何 + 弹高 532（其余 tab 仍 600）", {
    TestEnv env;
    auto a = env.svc->createProfile("双列几何", "https://geo.example.com",
                                    "sk-ant-geo-0001", "");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    {
        eui::Ui ui;
        compose(ui, "claude_geo1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));  // 新建表单，默认落基础页
    }
    {
        eui::Ui ui;
        compose(ui, "claude_geo2");
        ui.layout(screen.width, screen.height);
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        const float px = panel->frame.x;
        const float py = panel->frame.y;

        // 弹窗尺寸：760 宽 × 532 高（基础页双列紧凑高度）
        DOCTEST_CHECK(floatEq(panel->frame.width, 760.0f));
        DOCTEST_CHECK(floatEq(panel->frame.height, 532.0f));

        // 双列：左列 x=28 / 右列 x = 28+344+16 = 388；两标签同排 y=104
        const auto* nameLabel = ui.find("claude.form.basic.name.label");
        const auto* authLabel = ui.find("claude.form.basic.authfield.label");
        DOCTEST_REQUIRE(nameLabel != nullptr);
        DOCTEST_REQUIRE(authLabel != nullptr);
        DOCTEST_CHECK(floatEq(nameLabel->frame.x - px, 28.0f));
        DOCTEST_CHECK(floatEq(authLabel->frame.x - px, 388.0f));
        DOCTEST_CHECK(floatEq(authLabel->frame.y, nameLabel->frame.y));
        DOCTEST_CHECK(floatEq(nameLabel->frame.y - py, 104.0f));

        // 名称输入 / 认证下拉：各占列宽 344、同排 y=122
        const auto* nameInput = ui.find("claude.dialog.form.name");
        const auto* ddwrap = ui.find("claude.form.basic.authfield.ddwrap");
        DOCTEST_REQUIRE(nameInput != nullptr);
        DOCTEST_REQUIRE(ddwrap != nullptr);
        DOCTEST_CHECK(floatEq(nameInput->frame.x - px, 28.0f));
        DOCTEST_CHECK(floatEq(nameInput->frame.width, 344.0f));
        DOCTEST_CHECK(floatEq(ddwrap->frame.x - px, 388.0f));
        DOCTEST_CHECK(floatEq(ddwrap->frame.width, 344.0f));
        DOCTEST_CHECK(floatEq(ddwrap->frame.y, nameInput->frame.y));
        DOCTEST_CHECK(floatEq(nameInput->frame.y - py, 122.0f));

        // Base URL / API Key 值长保留全宽：704 = 760-56
        const auto* urlInput = ui.find("claude.dialog.form.url");
        const auto* keyInput = ui.find("claude.dialog.form.key");
        DOCTEST_REQUIRE(urlInput != nullptr);
        DOCTEST_REQUIRE(keyInput != nullptr);
        DOCTEST_CHECK(floatEq(urlInput->frame.width, 704.0f));
        DOCTEST_CHECK(floatEq(keyInput->frame.width, 704.0f));

        // 认证 hint 收在右列内：x = 388+76 = 464，右缘 ≤ 760-28
        const auto* authHint = ui.find("claude.form.basic.authfield.hint");
        DOCTEST_REQUIRE(authHint != nullptr);
        DOCTEST_CHECK(floatEq(authHint->frame.x - px, 464.0f));
        DOCTEST_CHECK(authHint->frame.x - px + authHint->frame.width <= 732.0f);
    }
    // 高级配置页（tab 3）内容深至 y≈496，须保持 600：全局改小即压按钮带
    {
        eui::Ui ui;
        compose(ui, "claude_geo3");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "claude_geo4");
        ui.layout(screen.width, screen.height);
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(floatEq(panel->frame.height, 600.0f));
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(claude_ui_auth_field_create_persists,
              "claude_module: 新建表单选「只发 x-api-key」保存后落库", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& txt) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = txt;
        node->onTextInput(ev);
        return true;
    };

    {
        eui::Ui ui;
        compose(ui, "authfield_c1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "authfield_c2");
        DOCTEST_CHECK(type(ui, "claude.dialog.form.name.hit", "认证字段建档"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.url.hit",
                           "https://af.example.com"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.key.hit", "sk-ant-af-00001"));
    }
    {
        eui::Ui ui;
        compose(ui, "authfield_c3");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.authfield.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui, "authfield_c4");
        DOCTEST_CHECK(ui.find("claude.dialog.form.authfield.dd.item.2") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.authfield.dd.item.2"));
    }
    {
        eui::Ui ui;
        compose(ui, "authfield_c5");
        const auto* label = ui.find("claude.dialog.form.authfield.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("只发 x-api-key"));
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
        DOCTEST_CHECK_EQ(list.value()[0].name, std::string("认证字段建档"));
        DOCTEST_CHECK_EQ(list.value()[0].authField, std::string("api_key"));
        auto act = env.svc->getActiveProfile();
        DOCTEST_REQUIRE(act.isOk());
        DOCTEST_CHECK_EQ(act.value().authField, std::string("api_key"));
        cleanupProfileKey(kTestKeychainService, list.value()[0].id);
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(claude_ui_auth_field_edit_shows_current,
              "claude_module: 编辑 api_key 的 profile 下拉回显当前值", {
    TestEnv env;
    auto a = env.svc->createProfile("回显", "https://af-edit.example.com",
                                    "sk-ant-afe-00001", "m",
                                    /*activateIfNoneActive=*/false,
                                    /*sharedConfigId=*/0, "api_key");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    // 选中卡片 → 点「编辑」
    {
        eui::Ui ui;
        compose(ui, "authfield_e1");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "authfield_e2");
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    // 回显 = index 2（只发 x-api-key），而非默认的「双发」
    {
        eui::Ui ui;
        compose(ui, "authfield_e3");
        const auto* label = ui.find("claude.dialog.form.authfield.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("只发 x-api-key"));
    }

    // 保存后仍是 api_key（编辑路径写 p.authField）
    {
        eui::Ui ui;
        compose(ui, "authfield_e4");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto g = env.svc->getProfile(a.value().id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().authField, std::string("api_key"));
        cleanupProfileKey(kTestKeychainService, a.value().id);
    }

    mod->onStop();
    return true;
})

// failover 备用下拉：编辑回显 + 选择保存落库 + 自引用排除

REGISTER_TEST(claude_ui_fallback_dropdown_edit_persists,
              "claude_module: failover 备用 Profile 下拉（回显/保存落库/自引用排除，headless）", {
    TestEnv env;
    // 备家先建；编辑主家时候选 = 全部排除自身 → 只剩备家 = item.1
    auto b = env.svc->createProfile("备家", "https://fb-b.example.com",
                                    "sk-ant-fbb-00001", "m");
    DOCTEST_REQUIRE(b.isOk());
    auto a = env.svc->createProfile("主家", "https://fb-a.example.com",
                                    "sk-ant-fba-00001", "m");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    {
        eui::Ui ui;
        compose(ui, "fb_e1");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fb_e2");
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    // failover 下拉在表单第 4 tab（高级配置，idx 3）——先切 tab 再操作下拉
    //（formTab_ 持久，后续帧免重切）
    {
        eui::Ui ui;
        compose(ui, "fb_tab3");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "fb_e3");
        const auto* label = ui.find("claude.form.basic.fallback.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("无（不启用 failover）"));
    }
    // 拉开 → 自引用排除：候选只有「备家」（item.0=无 / item.1=备家）
    {
        eui::Ui ui;
        compose(ui, "fb_e4");
        DOCTEST_CHECK(click(ui, "claude.form.basic.fallback.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui, "fb_e5");
        DOCTEST_CHECK(ui.find("claude.form.basic.fallback.dd.item.1") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.basic.fallback.dd.item.2") == nullptr);
        DOCTEST_CHECK(click(ui, "claude.form.basic.fallback.dd.item.1"));
    }
    {
        eui::Ui ui;
        compose(ui, "fb_e6");
        const auto* label = ui.find("claude.form.basic.fallback.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("备家"));
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto g = env.svc->getProfile(a.value().id);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().fallbackProfileId, b.value().id);
    }
    // 重开编辑：下拉回显「备家」（等后台刷新把新值带进缓存——打开表单的 profile 取自
    // 缓存快照，与真实应用一致）
    bool sawBackup = false;
    for (int i = 0; i < 300 && !sawBackup; ++i) {
        {
            eui::Ui ui;
            compose(ui, "fb_e7a");
            DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
        }
        {
            eui::Ui ui;
            compose(ui, "fb_e7b");
            DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
        }
        {   // 重开的表单回到 tab 0，failover 下拉在 tab 3
            eui::Ui ui;
            compose(ui, "fb_e7b2");
            DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
        }
        {
            eui::Ui ui;
            compose(ui, "fb_e7c");
            const auto* label = ui.find("claude.form.basic.fallback.dd.label");
            if (label != nullptr && label->text == "备家") {
                sawBackup = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
    DOCTEST_CHECK(sawBackup);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// 高级配置 tab：表单第 4 页方言下拉
// 覆盖新建 / 编辑接线 + 自动档 + 接线回归钉。
REGISTER_TEST(claude_ui_advanced_tab_dialect_persists,
              "claude_module: 高级配置页方言下拉（新建与编辑落库/自动档/不触碰保留，headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };
    // 按名字点卡片（不依赖中文名排序的列表序）
    auto clickCardNamed = [&](eui::Ui& ui, const char* nm) -> bool {
        for (int i = 0; i < 8; ++i) {
            const auto* name = ui.find("claude.profile." + std::to_string(i) +
                                       ".name");
            if (name != nullptr && name->text == nm) {
                return click(ui, "claude.profile." + std::to_string(i) + ".bg");
            }
        }
        return false;
    };
    // 等后台刷新把写库结果带进缓存快照
    auto waitFor = [&](auto&& pred) -> bool {
        for (int i = 0; i < 300; ++i) {
            // 每轮重发 onPageEnter：服务侧直写后的重查通道。若上一轮 post 的查询
            // 先于本批写库落库，其结果会把 dataDirty_ 清掉——200ms 节流窗口内的
            // 写库会被这次「旧结果收割」吞掉。重发脏标记保证下一窗口必有新 post；
            // 生产用户等价场景 = 切页重入。
            mod->onPageEnter();
            eui::Ui ui;
            compose(ui, "adv_poll");
            if (pred(ui)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    // ① 新建路径：新建表单 → tab 3 → 选「opencode zen」→ 基础页补名称/URL/Key
    //    → 创建 → dialectOverride == "opencode_zen"
    {
        eui::Ui ui;
        compose(ui, "adv_c1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_c2");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_c3");
        // 选中态逐帧归算：未覆盖 → 「自动（按站点推断）」（全角括号）
        const auto* label = ui.find("claude.form.advanced.dialect.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("自动（按站点推断）"));
        DOCTEST_CHECK(click(ui, "claude.form.advanced.dialect.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_c4");
        const auto* i1 = ui.find("claude.form.advanced.dialect.dd.item.label.1");
        const auto* i2 = ui.find("claude.form.advanced.dialect.dd.item.label.2");
        DOCTEST_REQUIRE(i1 != nullptr);
        DOCTEST_REQUIRE(i2 != nullptr);
        DOCTEST_CHECK_EQ(i1->text, std::string("anthropic"));
        DOCTEST_CHECK_EQ(i2->text, std::string("opencode zen"));
        DOCTEST_CHECK(click(ui, "claude.form.advanced.dialect.dd.item.2"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_c5");
        const auto* label = ui.find("claude.form.advanced.dialect.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("opencode zen"));
        // 几何断言：本页两下拉底沿 ≤ 弹窗底部按钮区上沿
        const auto* fbDd = ui.find("claude.form.basic.fallback.dd.field");
        const auto* confirm = ui.find("claude.dialog.form.confirm.bg");
        DOCTEST_REQUIRE(fbDd != nullptr);
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_CHECK(fbDd->frame.y + fbDd->frame.height <= confirm->frame.y);
    }
    {
        eui::Ui ui;
        compose(ui, "adv_c6");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.0"));   // 基础页补必填项
    }
    {
        eui::Ui ui;
        compose(ui, "adv_c7");
        DOCTEST_CHECK(type(ui, "claude.dialog.form.name.hit", "方言新建"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.url.hit", "https://dialect.example.com/"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.key.hit", "sk-ant-dialect-0001"));
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    DOCTEST_CHECK(waitFor([&](eui::Ui&) {
        auto list = env.svc->listProfiles();
        if (list.isFail()) return false;
        for (const auto& p : list.value()) {
            if (p.name == "方言新建") return p.dialectOverride == "opencode_zen";
        }
        return false;
    }));
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        int64_t createdId = 0;
        for (const auto& p : list.value()) {
            if (p.name == "方言新建") createdId = p.id;
        }
        DOCTEST_REQUIRE(createdId != 0);
        cleanupProfileKey(kTestKeychainService, createdId);
    }

    // ② 编辑路径：先服务层造一个 opencode_zen 覆盖的 profile → 编辑 → 下拉回显
    //    → 选「anthropic」保存 → anthropic_default；再改回「自动」保存 → 空串
    auto e = env.svc->createProfile("方言编辑", "https://edit.example.com",
                                    "sk-ant-edit-0001", "",
                                    /*activateIfNoneActive=*/false,
                                    /*sharedConfigId=*/0, /*authField=*/"both",
                                    /*dialectOverride=*/"opencode_zen");
    DOCTEST_REQUIRE(e.isOk());
    const int64_t editId = e.value().id;
    mod->onPageEnter();
    DOCTEST_CHECK(waitFor([&](eui::Ui& ui) {
        return clickCardNamed(ui, "方言编辑");
    }));
    {
        eui::Ui ui;
        compose(ui, "adv_e1");
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e2");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e3");
        const auto* label = ui.find("claude.form.advanced.dialect.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("opencode zen"));   // 回显
        DOCTEST_CHECK(click(ui, "claude.form.advanced.dialect.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e4");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.dialect.dd.item.1"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e5");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto g = env.svc->getProfile(editId);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().dialectOverride, std::string("anthropic_default"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e6");
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e7");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e8");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.dialect.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e9");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.dialect.dd.item.0"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_e10");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto g = env.svc->getProfile(editId);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().dialectOverride, std::string(""));
    }

    // ③ 接线回归钉：先服务层设覆盖，编辑弹窗里**不触碰**方言下拉直接保存 →
    //    覆盖值保留（钉住透传保留语义，防未来表单重构丢字段；现路径经 getProfile
    //    本就保留）。前置：等缓存快照带上新覆盖——详情 pane 文案与编辑预填同源自
    //    快照，pane 显示「opencode zen（显式）」即快照已新（否则表单会拿旧值保存）。
    {
        auto g = env.svc->getProfile(editId);
        DOCTEST_REQUIRE(g.isOk());
        g.value().dialectOverride = "opencode_zen";
        DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());
    }
    bool sawOverride = false;
    for (int i = 0; i < 300 && !sawOverride; ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui, "adv_k0");
        if (clickCardNamed(ui, "方言编辑")) {
            eui::Ui ui2;
            compose(ui2, "adv_k0b");
            if (click(ui2, "claude.detail.tabs.hit.3")) {
                eui::Ui ui3;
                compose(ui3, "adv_k0c");
                const auto* dv = ui3.find("claude.field.dialect.value");
                sawOverride = dv != nullptr &&
                              dv->text == "opencode zen（显式）";
            }
        }
        if (!sawOverride) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(sawOverride);
    {
        eui::Ui ui;
        compose(ui, "adv_k1");
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "adv_k2");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto g = env.svc->getProfile(editId);
        DOCTEST_REQUIRE(g.isOk());
        DOCTEST_CHECK_EQ(g.value().dialectOverride, std::string("opencode_zen"));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, editId);
    return true;
})

// 高级设置 JSON（headless）
// 反重构警告：落库 pretty 断言的表单输入**必须**用 compact / 未格式化文本——
// 输入本身是 pretty 时，「存原文」与「存 pretty」字节相同，断言恒绿。
REGISTER_TEST(claude_ui_advanced_json_pretty_persisted,
              "claude_module: 高级设置 JSON compact 输入落库为 pretty 文本（headless，变异④钉子）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    {
        eui::Ui ui;
        compose(ui, "json_c1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "json_c2");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "json_c3");
        // compact 单行输入（非 pretty 才能区分「存原文」与「存 pretty」）
        DOCTEST_CHECK(type(ui, "claude.form.advanced.json.hit",
                           R"({"model":"cmp-x","permissions":{"allow":["Bash"]}})"));
    }
    {
        eui::Ui ui;
        compose(ui, "json_c4");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.0"));
    }
    {
        eui::Ui ui;
        compose(ui, "json_c5");
        DOCTEST_CHECK(type(ui, "claude.dialog.form.name.hit", "JSON落库"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.url.hit", "https://json.example.com"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.key.hit", "sk-ant-json-0001"));
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    // 等后台刷新落库后断言：DB 中为 pretty 文本（含两空格缩进的键行）
    bool ok = false;
    int64_t createdId = 0;
    for (int i = 0; i < 300 && !ok; ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui, "json_poll");
        auto list = env.svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.name == "JSON落库") {
                    createdId = p.id;
                    ok = p.extraSettingsJson.find("\n  \"") != std::string::npos &&
                         p.extraSettingsJson.find("cmp-x") != std::string::npos;
                }
            }
        }
        if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(ok);
    DOCTEST_REQUIRE(createdId != 0);
    mod->onStop();
    cleanupProfileKey(kTestKeychainService, createdId);
    return true;
})

REGISTER_TEST(claude_ui_advanced_json_invalid_and_env_key_rejected,
              "claude_module: 高级设置 JSON 非法/含 env 键 → formError_ 红字不落库（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };
    // （lambda 返回 bool：DOCTEST_CHECK 的 fail-fast return 只能在 bool 函数里）
    auto openFormTab3FillBasic = [&](const char* tag) -> bool {
        {
            eui::Ui ui;
            compose(ui, tag);
            if (!click(ui, "claude.profiles.new.bg")) return false;
        }
        {
            eui::Ui ui;
            compose(ui, tag);
            if (!click(ui, "claude.form.tabs.hit.3")) return false;
        }
        return true;
    };
    auto fillBasicAndSave = [&](const char* tag) -> bool {
        {
            eui::Ui ui;
            compose(ui, tag);
            if (!click(ui, "claude.form.tabs.hit.0")) return false;
        }
        {
            eui::Ui ui;
            compose(ui, tag);
            if (!type(ui, "claude.dialog.form.name.hit", "JSON坏值")) return false;
            if (!type(ui, "claude.dialog.form.url.hit", "https://bad.example.com")) return false;
            if (!type(ui, "claude.dialog.form.key.hit", "sk-ant-bad-0001")) return false;
            if (!click(ui, "claude.dialog.form.confirm.bg")) return false;
        }
        return true;
    };

    // ① 非法 JSON：formError_ 非空、弹窗不关、不落库
    DOCTEST_CHECK(openFormTab3FillBasic("jsonb_1"));
    {
        eui::Ui ui;
        compose(ui, "jsonb_2");
        DOCTEST_CHECK(type(ui, "claude.form.advanced.json.hit", "{broken json"));
    }
    DOCTEST_CHECK(fillBasicAndSave("jsonb_3"));
    {
        eui::Ui ui;
        compose(ui, "jsonb_4");
        const auto* err = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(!err->text.empty());   // 红字在（弹窗不关 = form 仍开）
    }
    // 坏值不落库：库里没有名为「JSON坏值」的 profile（创建路径被校验挡下）
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        for (const auto& p : list.value()) {
            DOCTEST_CHECK(p.name != "JSON坏值");
        }
    }
    // ② 含 env 键：专属文案拒绝（D-5 fail-fast）
    {
        eui::Ui ui;
        compose(ui, "jsone_1");
        // 改写 JSON 框内容：先清空（全选替换不可行 → 取消重开表单再输）
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));
    }
    DOCTEST_CHECK(openFormTab3FillBasic("jsone_2"));
    {
        eui::Ui ui;
        compose(ui, "jsone_3");
        DOCTEST_CHECK(type(ui, "claude.form.advanced.json.hit",
                           R"({"env":{"ANTHROPIC_BASE_URL":"https://x"}})"));
    }
    DOCTEST_CHECK(fillBasicAndSave("jsone_4"));
    {
        eui::Ui ui;
        compose(ui, "jsone_5");
        const auto* err = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("不能包含 env 键") != std::string::npos);
    }

    // ③ 格式化按钮：compact 输入 → 失败分支行内红字（钉按钮接线）
    {
        eui::Ui ui;
        compose(ui, "jsonf_0");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));
    }
    DOCTEST_CHECK(openFormTab3FillBasic("jsonf_1"));
    {
        eui::Ui ui;
        compose(ui, "jsonf_2");
        DOCTEST_CHECK(type(ui, "claude.form.advanced.json.hit", "{not json"));
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.format.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "jsonf_3");
        const auto* err = ui.find("claude.form.advanced.json.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("解析失败") != std::string::npos);
    }
    // 成功分支：compact 输入 → 点「格式化」→ **编辑框文本当场**变成 pretty
    // 且键序重排为字典序。
    // 就会 pretty 化（claude_ui_form.cpp:322-327）——格式化按钮整段失效（no-op /
    // 只 trim / 解析失败提前 return）该断言照样绿。这里改断言格式化**当场**
    // 改写编辑框文本，并用乱序键（{"zz","aa"}）让「无操作」「只去空白」
    // 「保留原序」三类半吊子实现全部变红；键序契约见 formatAdvancedJson 注释
    // （parse→stringify 走 std::map = 字典序）。
    {
        eui::Ui ui;
        compose(ui, "jsonf_4");
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));
    }
    DOCTEST_CHECK(openFormTab3FillBasic("jsonf_5"));
    {
        eui::Ui ui;
        compose(ui, "jsonf_6");
        DOCTEST_CHECK(type(ui, "claude.form.advanced.json.hit",
                           R"({"zz":1,"aa":2})"));
    }
    // 格式化前：编辑框仍是用户敲进去的 compact 原文（判别力来源——若这一帧
    // 就已是 pretty，说明断言测的不是格式化按钮）
    {
        eui::Ui ui;
        compose(ui, "jsonf_6b");
        using InputState = components::input_detail::InputModel::InputState;
        DOCTEST_CHECK_EQ(
            ui.state<InputState>("claude.form.advanced.json").text,
            std::string(R"({"zz":1,"aa":2})"));
    }
    {
        eui::Ui ui;
        compose(ui, "jsonf_6c");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.format.bg"));
    }
    const std::string kFormatted =
        "{\n  \"aa\": 2,\n  \"zz\": 1\n}";
    {
        eui::Ui ui;
        compose(ui, "jsonf_6d");
        using InputState = components::input_detail::InputModel::InputState;
        DOCTEST_CHECK_EQ(ui.state<InputState>("claude.form.advanced.json").text,
                         kFormatted);
        // 成功分支不留行内红字（与失败分支的「解析失败」红字互斥）
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.error") == nullptr);
    }
    DOCTEST_CHECK(fillBasicAndSave("jsonf_7"));
    bool fmtOk = false;
    for (int i = 0; i < 300 && !fmtOk; ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui, "jsonf_poll");
        auto list = env.svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.name == "JSON坏值") {
                    // 落库值 == 格式化后的编辑框文本（逐字节）：保存链不再二次
                    // 改写形状，只把编辑框那份原文带进 DB
                    fmtOk = p.extraSettingsJson == kFormatted;
                }
            }
        }
        if (!fmtOk) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(fmtOk);
    // 清理本次创建的 profile
    {
        auto list = env.svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.name == "JSON坏值") {
                    cleanupProfileKey(kTestKeychainService, p.id);
                }
            }
        }
    }

    mod->onStop();
    return true;
})

// 高级设置 JSON 全屏编辑（headless）
// 用例 A：全屏态进入（覆盖层 = 整个窗口几何，且表单 dialog 面板整体落在覆盖层
// 内 = 「盖住表单弹窗」的数值断言）+ Esc 双通道退出回表单（输入框 onCancel
// 消费 / 模块级 closeTopmostDialog 序链先全屏后表单）+ 未应用文本留在缓冲
//（回表单可见）。z 语义：覆盖层 zIndex 1050 > 表单弹窗 1000——无头侧以
// 「面板 ⊆ 覆盖层几何」表达，z 排序本身由 EUI-NEO 兄弟 stable_sort 保证
REGISTER_TEST(claude_ui_json_fullscreen_enter_cover_and_esc,
              "claude_module: JSON 全屏编辑进入覆盖窗口几何 + Esc 退回表单文本保留（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);   // 元素 frame 需显式布局
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    // 进入：新建表单 → 高级配置页 → 点「全屏编辑」
    {
        eui::Ui ui;
        compose(ui, "fs_a1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_a2");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_a3");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    // 全屏态：覆盖层在场 + 覆盖整个窗口几何 + 表单面板整体落在覆盖层内
    {
        eui::Ui ui;
        compose(ui, "fs_a4");
        const auto* bg = ui.find("claude.jsonfs.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK_EQ(bg->frame.x, 0.0f);
        DOCTEST_CHECK_EQ(bg->frame.y, 0.0f);
        DOCTEST_CHECK_EQ(bg->frame.width, screen.width);
        DOCTEST_CHECK_EQ(bg->frame.height, screen.height);
        DOCTEST_CHECK(ui.find("claude.jsonfs.editor.hit") != nullptr);
        // 底部三键 = 格式化 / 保存 / 放弃修改（新节点 claude.jsonfs.discard）
        DOCTEST_CHECK(ui.find("claude.jsonfs.format.bg") != nullptr);
        const auto* saveBtn = ui.find("claude.jsonfs.apply.bg");
        DOCTEST_REQUIRE(saveBtn != nullptr);
        const auto* saveText = ui.find("claude.jsonfs.apply.text");
        DOCTEST_REQUIRE(saveText != nullptr);
        DOCTEST_CHECK_EQ(saveText->text, std::string("保存"));
        const auto* discardBtn = ui.find("claude.jsonfs.discard.bg");
        DOCTEST_REQUIRE(discardBtn != nullptr);
        const auto* discardText = ui.find("claude.jsonfs.discard.text");
        DOCTEST_REQUIRE(discardText != nullptr);
        DOCTEST_CHECK_EQ(discardText->text, std::string("放弃修改"));
        DOCTEST_CHECK(ui.find("claude.jsonfs.done.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.jsonfs.done.text") == nullptr);
        // 按钮行右缘不溢出覆盖层（三键同行：x=28 / 150 / 272，各 110 宽）
        DOCTEST_CHECK(discardBtn->frame.x + discardBtn->frame.width <=
                      screen.width - 27.0f);
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);   // 表单弹窗仍画（被盖在下面）
        DOCTEST_CHECK(panel->frame.x >= bg->frame.x - 0.5f);
        DOCTEST_CHECK(panel->frame.y >= bg->frame.y - 0.5f);
        DOCTEST_CHECK(panel->frame.x + panel->frame.width <=
                      bg->frame.x + bg->frame.width + 0.5f);
        DOCTEST_CHECK(panel->frame.y + panel->frame.height <=
                      bg->frame.y + bg->frame.height + 0.5f);
        // z 档位钉：覆盖层 1050 —— 高于表单弹窗根（dialog 默认 1000）、低于 toast
        //（1100，应用成功确认可见）。改成 900 必红：900 < 1000 时 dialog 的全屏
        // backdrop 反序先命中 = 全屏里一点就关表单，功能真坏。
        // 表单，功能真坏；这里以 z 值直接钉住该序）
        const auto* fsRoot = ui.find("claude.jsonfs.root");
        DOCTEST_REQUIRE(fsRoot != nullptr);
        DOCTEST_CHECK_EQ(fsRoot->zIndex, 1050);
        const auto* dlgRoot = ui.find("claude.dialog.form");
        DOCTEST_REQUIRE(dlgRoot != nullptr);
        DOCTEST_CHECK(fsRoot->zIndex > dlgRoot->zIndex);
        const auto* toastRoot = ui.find("claude.toast");   // 每帧无条件构建
        DOCTEST_REQUIRE(toastRoot != nullptr);
        DOCTEST_CHECK(fsRoot->zIndex < toastRoot->zIndex);
    }
    // 全屏框输入 → Esc（输入框 onCancel 消费）→ 回表单；Esc = 放弃修改
    // **Esc = 放弃修改**（原语义「保留文本回表单」已变）：编辑缓冲恢复为
    // 上次落库值 + 关闭全屏，表单仍开
    {
        eui::Ui ui;
        compose(ui, "fs_a5");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit",
                           R"({"keep":"me"})"));
        const auto* hit = ui.find("claude.jsonfs.editor.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(hit->onKeyEvent(esc));   // 输入框消费（放弃修改 + 退全屏）
    }
    {
        eui::Ui ui;
        compose(ui, "fs_a6");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);  // 全屏已退
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);   // 回到表单（弹窗还开）
        DOCTEST_CHECK_EQ(panel->opacity, 1.0f);
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.fs.bg") != nullptr);
        // 新建表单的落库基线 = 空 → 放弃后编辑缓冲回到空（多行空值渲染
        // 单节点 .text（占位符），不产生 .text.N 行节点）
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.text.0") == nullptr);
    }
    // 重进全屏 → 模块级 Esc（closeTopmostDialog）先退全屏（= 放弃修改）、
    // 再按一次才关表单
    {
        eui::Ui ui;
        compose(ui, "fs_a7");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_a8");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(mod->onKeyEvent(esc));   // 消费 = 放弃修改 + 退全屏
    }
    {
        eui::Ui ui;
        compose(ui, "fs_a9");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.panel") != nullptr);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(mod->onKeyEvent(esc));   // 再按 = 关表单（序链次序）
    }
    {
        eui::Ui ui;
        compose(ui, "fs_a10");
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);   // 表单已关
    }

    mod->onStop();
    return true;
})

// 用例 B：文本往返（表单框 ↔ 全屏框同一编辑缓冲，经 DB 真值验证）
// 输入一律 compact，pretty 落库才能区分。全屏 error 节点与表单
// claude.dialog.form.error 同读 formError_，文本相等。
REGISTER_TEST(claude_ui_json_fullscreen_buffer_roundtrip_and_error_gate,
              "claude_module: JSON 全屏编辑同一缓冲往返（双向落库）+ 应用校验红字同源（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);   // 元素 frame 需显式布局
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    // ① 方向 A（表单框 → 全屏框 → 保存落库）：新建表单→基础项先填全→高级配置页
    //    输 compact → 全屏内点「格式化」（若全屏是第二份缓冲则空串 no-op，
    //    全屏编辑器看不到 pretty 形 → 红）→「保存」（节点 claude.jsonfs.apply，
    {
        eui::Ui ui;
        compose(ui, "fs_b1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_b2");
        DOCTEST_CHECK(type(ui, "claude.dialog.form.name.hit", "FS往返A"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.url.hit", "https://fs-a.example.com"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.key.hit", "sk-ant-fs-a-0001"));
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_b3");
        DOCTEST_CHECK(type(ui, "claude.form.advanced.json.hit",
                           R"({"model":"fs-a","statusLine":"on"})"));
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_b4");
        // 全屏编辑器与表单同缓冲：进全屏即见表单框里输的 compact 原文
        const auto* l0 = ui.find("claude.jsonfs.editor.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text,
                         std::string(R"({"model":"fs-a","statusLine":"on"})"));
        DOCTEST_CHECK(click(ui, "claude.jsonfs.format.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_b5");
        // 格式化作用在同一缓冲：全屏首行变 pretty 的 "{"；底层表单框（被盖住）
        // 同步可见同一份 pretty 文本
        const auto* l0 = ui.find("claude.jsonfs.editor.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, std::string("{"));
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));   // 保存
    }
    bool okA = false;
    int64_t idA = 0;
    for (int i = 0; i < 300 && !okA; ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui, "fs_b_poll_a");
        auto list = env.svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.name == "FS往返A") {
                    idA = p.id;
                    okA = p.extraSettingsJson.find("\n  \"") != std::string::npos &&
                          p.extraSettingsJson.find("fs-a") != std::string::npos;
                }
            }
        }
        if (!okA) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(okA);   // pretty 落库 = 全屏「格式化」确实作用在同一缓冲

    // ② 方向 B（全屏框 → 表单保存落库）：新建表单→基础项填全→高级配置页→
    //    直接进全屏输入 compact →「保存」→ DB 含该键
    {
        eui::Ui ui;
        compose(ui, "fs_c1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_c2");
        DOCTEST_CHECK(type(ui, "claude.dialog.form.name.hit", "FS往返B"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.url.hit", "https://fs-b.example.com"));
        DOCTEST_CHECK(type(ui, "claude.dialog.form.key.hit", "sk-ant-fs-b-0001"));
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_c3");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_c4");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit",
                           R"({"model":"fs-b"})"));
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));   // 保存
    }
    bool okB = false;
    bool cardReady = false;   // 快照收割完成：卡片已进渲染树（fs_d1 的 click 依赖）
    int64_t idB = 0;
    for (int i = 0; i < 300 && !(okB && cardReady); ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui, "fs_b_poll_b");
        if (!cardReady) {
            cardReady = ui.find("claude.profile.0.bg") != nullptr;
        }
        auto list = env.svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.name == "FS往返B") {
                    idB = p.id;
                    okB = p.extraSettingsJson.find("fs-b") != std::string::npos;
                }
            }
        }
        if (!(okB && cardReady)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(okB);   // 全屏框的输入确实写进了表单保存的那份缓冲
    DOCTEST_CHECK(cardReady);   // 快照收割完成（卡片进渲染树，fs_d1 可点）

    // ③ 校验红字同源：编辑 ①的 profile → 全屏内改坏 JSON → 「保存」→ 全屏
    //    error 节点非空且不退全屏；「放弃修改」
    //    formError_，两处显示；放弃只清 JSON 行内红字与编辑缓冲，提交闸门
    //    文案保留）
    {
        eui::Ui ui;
        compose(ui, "fs_d1");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d2");
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d3");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d4");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d5");
        // 追加坏片段（缓冲已有 pretty JSON，前插后整体非法）
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d6");
        const auto* err = ui.find("claude.jsonfs.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(!err->text.empty());
        // 全屏红字 = 提交闸门 formError_（JSON 源，同文案双写）；表单侧同一帧
        // 也有这条红字（fs_d8 断言放弃后两处都消失）
        const auto* ferr = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(ferr != nullptr);
        DOCTEST_CHECK_EQ(ferr->text, err->text);
        // 应用失败不退全屏、表单不关（闸门拦截，同 submitForm 语义）
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.panel") != nullptr);
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d7");
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));   // 放弃修改
    }
    {
        eui::Ui ui;
        compose(ui, "fs_d8");
        // 这条 formError_ 是 **JSON 源**（保存时 validateExtraSettingsJson 失败写入，
        // 与 formAdvancedJsonError_ 同文案）→「放弃修改」必须把它一并清掉，否则
        // 留下一条描述**已被丢弃**文本的幽灵错误。
        DOCTEST_CHECK(ui.find("claude.dialog.form.error") == nullptr);
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.error") == nullptr);
        // 放弃 = 编辑缓冲回到落库值（坏片段没留下）
        const auto* adv = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(adv != nullptr);
        DOCTEST_CHECK(adv->text.find("{broken") == std::string::npos);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));   // 不落坏值
    }
    // 坏值未落库：全表扫描，任何 profile 的 extraSettingsJson 都不含 "{broken"
    //（编辑的是哪张卡片按列表序不定——按内容断言，不按 id 断言）
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        for (const auto& p : list.value()) {
            DOCTEST_CHECK(p.extraSettingsJson.find("{broken") ==
                          std::string::npos);
        }
    }

    // ④ 判别式：红字两来源可辨 + 优先级钉住——③ 的路径上 formError_ 与
    // formAdvancedJsonError_ 同文本（submitForm 两条一起写），读哪个都绿、非判别。
    // 这里构造两源文本不同的状态：
    //   路径① 先「格式化」坏 JSON → 仅 formAdvancedJsonError_ 非空
    //        （formError_ 空）→ 全屏 error 必须显示「解析失败…」（行内源可达）
    //   路径② 再「应用」触发基础项闸门 → formError_ = 名称/URL 文案，
    //        行内源仍是「解析失败…」→ 全屏 error 必须切到 formError_ 文案
    //        （提交闸门优先；读错源/优先级颠倒即红）
    {
        eui::Ui ui;
        compose(ui, "fs_e1");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_e2");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_e3");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_e4");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
        DOCTEST_CHECK(click(ui, "claude.jsonfs.format.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_e5");
        const auto* err = ui.find("claude.jsonfs.error");
        DOCTEST_REQUIRE(err != nullptr);
        // 路径①：formError_ 空 → 显示行内源（格式化失败文案）
        DOCTEST_CHECK(err->text.find("解析失败") != std::string::npos);
    }
    {
        eui::Ui ui;
        compose(ui, "fs_e6");
        // 基础项（名称/URL）为空 → submitForm 第一道闸门失败
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "fs_e7");
        const auto* err = ui.find("claude.jsonfs.error");
        DOCTEST_REQUIRE(err != nullptr);
        // 路径②：formError_ 非空 → 优先提交闸门文案；行内源此刻仍是「解析失败…」
        // ——两源文本不同 = 判别式成立
        DOCTEST_CHECK(err->text.find("名称和 Base URL") != std::string::npos);
        DOCTEST_CHECK(err->text.find("解析失败") == std::string::npos);
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") != nullptr);   // 不退全屏
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));  // 收尾，不落库
    }

    if (idA != 0) cleanupProfileKey(kTestKeychainService, idA);
    if (idB != 0) cleanupProfileKey(kTestKeychainService, idB);
    mod->onStop();
    return true;
})

// 全屏 JSON「放弃修改」语义（按钮 / 点遮罩 / Esc 三通道）
// 契约：
//   底部三键 = 格式化 / 保存（节点 claude.jsonfs.apply）/ 放弃修改（新节点
//   claude.jsonfs.discard）。
//   放弃 = 编辑缓冲恢复**上次落库值** + 关全屏（表单仍开，不报错）；
//   放弃 = 编辑缓冲恢复**上次落库值** + 关全屏（表单仍开，不报错）；
//   Esc / 点遮罩同「放弃修改」（原 Esc「保留文本回表单」语义已作废——防误丢）。
REGISTER_TEST(claude_ui_json_fullscreen_discard_semantics,
              "claude_module: JSON 全屏「放弃修改」= 缓冲恢复落库值（按钮/遮罩/Esc 三通道，headless）", {
    TestEnv env;
    // DB 原值用 compact 原文（读侧按原样预填，格式化按钮才规范化）——便于
    // 「恢复为落库值」用整串相等断言区分「恢复了」与「没动」
    const std::string savedJson = R"({"model":"keep-me","statusLine":"on"})";
    auto p = env.svc->createProfile("放弃源", "https://discard.example.com",
                                    "sk-ant-disc-0001", "m",
                                    /*activateIfNoneActive=*/true,
                                    /*sharedConfigId=*/0, "both", "",
                                    /*fallbackProfileId=*/0, savedJson);
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };
    // 全屏编辑器全文（多行 .text.N 逐行拼；空值走单节点 .text 占位符 → 空串）
    auto editorText = [](eui::Ui& ui) -> std::string {
        std::string out;
        bool any = false;
        for (size_t i = 0;; ++i) {
            const auto* n =
                ui.find("claude.jsonfs.editor.text." + std::to_string(i));
            if (n == nullptr) break;
            if (any) out += "\n";
            out += n->text;
            any = true;
        }
        return out;
    };
    // 返回 bool（DOCTEST_CHECK 展开含 `return false;`——显式 void 会撞
    // "void block should not return a value"；不写返回类型又会被推导成 bool
    // 而尾路径缺 return）。调用方用 DOCTEST_REQUIRE 包。
    auto openFullscreen = [&]() -> bool {
        {
            eui::Ui ui;
            compose(ui, "disc_sel");
            DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
        }
        {
            eui::Ui ui;
            compose(ui, "disc_edit");
            DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
        }
        {
            eui::Ui ui;
            compose(ui, "disc_tab3");
            DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
        }
        {
            eui::Ui ui;
            compose(ui, "disc_fs");
            DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
        }
        return true;
    };

    // 进全屏：编辑器 = 落库原文（int64 真值经 DB 预填，非第二份缓冲）
    DOCTEST_REQUIRE(openFullscreen());
    {
        eui::Ui ui;
        compose(ui, "disc_in");
        DOCTEST_REQUIRE(ui.find("claude.jsonfs.root") != nullptr);
        DOCTEST_CHECK_EQ(editorText(ui), savedJson);
    }

    // ① 按钮通道：插入坏片段 → 「放弃修改」→ 缓冲回落库值、全屏关、表单开、
    //    无红字（formAdvancedJsonError_ 一并清）
    {
        eui::Ui ui;
        compose(ui, "disc_btn_type");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_btn_pre");
        DOCTEST_CHECK(editorText(ui) != savedJson);   // 改动已进缓冲
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_btn_after");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);   // 全屏已关
        DOCTEST_CHECK(ui.find("claude.dialog.form.panel") != nullptr);  // 表单仍开
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.error") == nullptr);
        const auto* l0 = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, savedJson);   // 恢复为落库值
    }

    // ② 点遮罩通道：重进全屏 → 改坏 → 点覆盖层底（claude.jsonfs.bg）→ 同放弃
    DOCTEST_REQUIRE(openFullscreen());
    {
        eui::Ui ui;
        compose(ui, "disc_mask_type");
        DOCTEST_REQUIRE(ui.find("claude.jsonfs.root") != nullptr);
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_mask_pre");
        DOCTEST_CHECK(editorText(ui) != savedJson);
        DOCTEST_CHECK(click(ui, "claude.jsonfs.bg"));   // 遮罩点击 = 放弃修改
    }
    {
        eui::Ui ui;
        compose(ui, "disc_mask_after");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.panel") != nullptr);
        const auto* l0 = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, savedJson);
    }

    // ③ Esc 通道（模块级 closeTopmostDialog，输入框未聚焦可达）：同放弃 +
    //    只消费一层（第二次 Esc 才关表单）
    DOCTEST_REQUIRE(openFullscreen());
    {
        eui::Ui ui;
        compose(ui, "disc_esc_type");
        DOCTEST_REQUIRE(ui.find("claude.jsonfs.root") != nullptr);
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_esc_pre");
        DOCTEST_CHECK(editorText(ui) != savedJson);
        eui::KeyEvent esc;
        esc.key = eui::InputKey::Escape;
        esc.action = eui::KeyAction::Press;
        DOCTEST_CHECK(mod->onKeyEvent(esc));   // 消费 = 放弃修改 + 退全屏
    }
    {
        eui::Ui ui;
        compose(ui, "disc_esc_after");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.panel") != nullptr);
        const auto* l0 = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, savedJson);
    }

    // ④ 边界：当前文本 == 落库值（未改动）时放弃 = 等价于关闭，不报错
    DOCTEST_REQUIRE(openFullscreen());
    {
        eui::Ui ui;
        compose(ui, "disc_noop");
        DOCTEST_REQUIRE(ui.find("claude.jsonfs.root") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_noop_after");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.panel") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.error") == nullptr);
        const auto* l0 = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, savedJson);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));   // 收尾不落库
    }
    // ⑤ 幽灵错误：全屏内「保存」因非法 JSON 被提交闸门拦下 → formError_ +
    //    formAdvancedJsonError_ 同文案双写；此时放弃 → 缓冲已回滚，这条红字描述的
    //    是**已被丢弃**的内容 → 必须一并消失
    DOCTEST_REQUIRE(openFullscreen());
    {
        eui::Ui ui;
        compose(ui, "disc_err_type");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err_save");
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));   // 保存 → JSON 校验失败
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err_gate");
        // 闸门红字在：全屏 error 与表单 error 同文案（同一条 formError_）
        const auto* ferr = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(ferr != nullptr);
        DOCTEST_CHECK(ferr->text.find("高级设置 JSON") != std::string::npos);
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") != nullptr);   // 未退全屏
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));      // 放弃修改
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err_after");
        DOCTEST_CHECK(ui.find("claude.jsonfs.root") == nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.form.error") == nullptr);   // 无残留
        DOCTEST_CHECK(ui.find("claude.form.advanced.json.error") == nullptr);
        const auto* l0 = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, savedJson);
    }
    // ⑤b 同一条幽灵错误在「先改回合法、再放弃」路径上也要清：错误文案与当前
    //     缓冲无关——按**来源**清，不按「当前文本是否仍非法」重新判定
    {
        eui::Ui ui;
        compose(ui, "disc_err2_fs");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err2_type");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", "{broken"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err2_save");
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));   // 留下闸门红字
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err2_fix");
        // 全选（Ctrl/Cmd+A）后**同帧**输入合法 JSON → 缓冲整体改回合法。
        // 必须同帧：InputState（选区）挂在 Ui 实例上，跨帧新 Ui 不保证保留
        const auto* hit = ui.find("claude.jsonfs.editor.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        DOCTEST_REQUIRE(hit->onKeyEvent != nullptr);
        eui::KeyEvent selectAll;
        selectAll.key = eui::InputKey::A;
        selectAll.action = eui::KeyAction::Press;
        selectAll.modifiers.control = true;   // shortcut() = control（非 Apple）
        selectAll.modifiers.super = true;     // shortcut() = super（Apple）
        DOCTEST_CHECK(hit->onKeyEvent(selectAll));
        DOCTEST_REQUIRE(hit->onTextInput != nullptr);
        core::TextInputEvent ev;
        ev.text = R"({"ok":true})";
        hit->onTextInput(ev);
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err2_check_valid");
        const auto* l0 = ui.find("claude.jsonfs.editor.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, std::string(R"({"ok":true})"));   // 已改回合法
        // 闸门红字仍在（onChange 只清行内红字，提交闸门要等下一次提交/放弃）
        DOCTEST_CHECK(ui.find("claude.dialog.form.error") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_err2_after");
        DOCTEST_CHECK(ui.find("claude.dialog.form.error") == nullptr);
        const auto* l0 = ui.find("claude.form.advanced.json.text.0");
        DOCTEST_REQUIRE(l0 != nullptr);
        DOCTEST_CHECK_EQ(l0->text, savedJson);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));
    }
    // ⑥ 精准性：**非** JSON 源的 formError_ 不被放弃清掉。新建表单（名称/URL
    //    为空）→ 全屏内「保存」→ 基础项闸门先于 JSON 校验命中，写的是
    //    「名称和 Base URL 不能为空」（fromJson=false）→ 放弃后这条红字必须还在
    {
        eui::Ui ui;
        compose(ui, "disc_prec_new");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_prec_tab3");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_prec_fs");
        DOCTEST_CHECK(click(ui, "claude.form.advanced.json.fs.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_prec_type");
        DOCTEST_CHECK(type(ui, "claude.jsonfs.editor.hit", R"({"a":1})"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_prec_save");
        DOCTEST_CHECK(click(ui, "claude.jsonfs.apply.bg"));   // 基础项闸门
    }
    {
        eui::Ui ui;
        compose(ui, "disc_prec_gate");
        const auto* ferr = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(ferr != nullptr);
        DOCTEST_CHECK(ferr->text.find("名称和 Base URL") != std::string::npos);
        DOCTEST_CHECK(click(ui, "claude.jsonfs.discard.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "disc_prec_after");
        const auto* ferr = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(ferr != nullptr);   // 非 JSON 源 → 保留（仍成立）
        DOCTEST_CHECK(ferr->text.find("名称和 Base URL") != std::string::npos);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.cancel.bg"));   // 收尾不落库
    }

    // DB 未被放弃路径污染
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().extraSettingsJson, savedJson);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

static std::string s_claudeRunPickerDir;  // 假 picker 目标目录（函数指针缝，不能捕获）

// 运行第 7 键（直连形态）
// 本文件用真实 ClaudeService（setGlobal）：点「运行」前**必须**注入假 launch，
// 否则测试会真的启动 wt/powershell + claude
REGISTER_TEST(claude_module_run_button_headless,
              "claude_module: 「运行」按钮存在 + 无选中 toast + 假 launch 收到 --settings 参数", {
    TestEnv env;
    test_support::DataRootEnv root(kDbPrefix, "run");
    // doRun 先弹原生目录选择框：注入假 picker 防真弹窗阻塞套件。
    // RAII 保证断言失败提前退出也复位，stub 不泄漏进后续用例
    struct PickerStub {
        PickerStub(const std::string& d) {
            s_claudeRunPickerDir = d;
            platform::folderpicker::setPickerForTest(
                [](const std::string&, const std::string&) {
                    return core::Result<std::string>::ok(s_claudeRunPickerDir);
                });
        }
        ~PickerStub() { platform::folderpicker::setPickerForTest(nullptr); }
    } pickerStub(root.dir);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_run");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
        // DB 查询在后台线程 + 200ms 节流 → 「变更可见」最多晚一帧，有界轮询等可见
    // 有界轮询等可见（同本文件「compose 缓存等价」用例的 composeUntil）
    auto composeUntil = [&](auto&& pred) -> bool {
        for (int i = 0; i < 200; ++i) {
            eui::Ui ui;
            compose(ui);
            if (pred(ui)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    int launchCalls = 0;
    platform::terminal::LaunchSpec got;
    env.svc->setTerminalLauncher([&](const platform::terminal::LaunchSpec& s) {
        ++launchCalls;
        got = s;
        return core::Result<void>::ok();
    });
    // runProfile 直连形态不启代理：isProxyRunning 全程恒 false（零全局改动）

    // 第 7 键存在且可点（无 profile 时：无选中 → toast「请先选择 Profile」，不启动）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.act.run") != nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.bg") != nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.hint") != nullptr);   // 副提示（D-1）
        DOCTEST_CHECK(click(ui, "claude.act.run.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("请先选择 Profile") != std::string::npos);
        DOCTEST_CHECK_EQ(launchCalls, 0);          // 无选中不开会话
        DOCTEST_CHECK(!env.svc->isProxyRunning()); // 也没起代理
    }

    // 有 profile 且选中 → 点击后假 launch 被调用一次、参数含 --settings <路径>
    auto a = env.svc->createProfile("运行目标", "https://run.example.com",
                                    "sk-ant-run-ui-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(env.svc->setActiveProfile(a.value().id).isOk());
    // 外部直写 service 绕过模块的缓存帧 → 打脏（onPageEnter 语义），并轮询等
    // 列表可见：否则 profile 列表仍是空的、选不中卡片
    mod->onPageEnter();
    {
        DOCTEST_CHECK(composeUntil([&](eui::Ui& ui) {
            if (ui.find("claude.profile.0.bg") == nullptr) return false;
            return click(ui, "claude.profile.0.bg");   // 选中
        }));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.run.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(launchCalls, 1);
        DOCTEST_CHECK_EQ(got.title, std::string("运行目标 · Claude Code"));
        DOCTEST_CHECK_EQ(got.program, std::string("claude"));
        // D-1：argv 含 --settings + run-settings/ 下的路径（断言参数，不真起进程）
        DOCTEST_REQUIRE_EQ(got.args.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(got.args[0], std::string("--settings"));
        const std::filesystem::path settingsFile(got.args[1]);
        DOCTEST_CHECK_EQ(settingsFile.parent_path().string(),
                         (std::filesystem::path(root.dir) / "run-settings").string());
        DOCTEST_CHECK(std::filesystem::exists(settingsFile));   // 落盘成功
        DOCTEST_CHECK(!env.svc->isProxyRunning());              // 直连不启代理
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        // 直连语义提示（D-1：临时配置生命周期、零全局改动）
        DOCTEST_CHECK(msg->text.find("全局配置") != std::string::npos);
        std::error_code rmEc;
        std::filesystem::remove(settingsFile, rmEc);
    }
    cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// 共享配置片段 编辑器 UI

// 纯函数：片段按 id 查找
REGISTER_TEST(claude_ui_logic_shared_pure_functions,
              "claude_ui_logic: 片段查找（批 2 纯函数，frozenKeys 家族已拆）", {
    using module::claude::findSharedById;

    // findSharedById：命中 / 未命中 / 空表
    std::vector<module::claude::SharedConfig> list;
    module::claude::SharedConfig c1;
    c1.id = 7; c1.name = "片段A";
    list.push_back(c1);
    DOCTEST_REQUIRE(findSharedById(list, 7) != nullptr);
    DOCTEST_CHECK_EQ(findSharedById(list, 7)->name, std::string("片段A"));
    DOCTEST_CHECK(findSharedById(list, 8) == nullptr);
    DOCTEST_CHECK(findSharedById({}, 7) == nullptr);
    return true;
})

// 1. 左栏「共享配置」区块：标题 / 片段卡片（name + 预设副文案）/ 新建按钮；
//      点卡片开编辑弹窗（回显 name/env）----
REGISTER_TEST(claude_module_shared_block_and_card,
              "claude_module: 共享配置区块卡片+点卡片开编辑弹窗（headless）", {
    TestEnv env;
    auto f1 = env.svc->createSharedConfig("片段A", R"({"MY_A":"1"})");
    DOCTEST_REQUIRE(f1.isOk());
    auto f2 = env.svc->createSharedConfig("片段B", R"({"MY_B":"2","MY_C":"3"})");
    DOCTEST_REQUIRE(f2.isOk());
    // 至少一个 profile 才走「同滚动容器」主路径（区块在 scrollView 尾部；
    // 零 profile 的绝对坐标兜底路径不带 claude.shared.block 包装）
    auto p = env.svc->createProfile("占位", "https://p.example.com",
                                    "sk-ant-sblk-0001", "");
    DOCTEST_REQUIRE(p.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_block");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing:") + id + ">"
                               : node->text;
    };

    // 区块元素存在（滚动容器尾部 + 新建按钮）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.shared.block") != nullptr);
        DOCTEST_CHECK(ui.find("claude.shared.title") != nullptr);
        DOCTEST_CHECK(ui.find("claude.shared.card.0.name") != nullptr);
        DOCTEST_CHECK(ui.find("claude.shared.card.1.name") != nullptr);
        DOCTEST_CHECK(ui.find("claude.shared.new") != nullptr);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.shared.card.0.name"),
                         std::string("片段A"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.shared.card.1.name"),
                         std::string("片段B"));
        // 副文案：「N 个键」计数已随片段 env 域拆除失效，改「高级属性 JSON 预设」
        DOCTEST_CHECK(textOf(ui, "claude.shared.card.1.keys").find(
                          "高级属性 JSON 预设") != std::string::npos);
    }
    // 点片段B 卡片 → 编辑弹窗打开、name / 高级设置 JSON 回显（键值行已拆除；
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.card.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 1.0f);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.shared.name.text"),
                         std::string("片段B"));
        // 弹窗 JSON 化：键值行拆除，编辑框在场
        DOCTEST_CHECK(ui.find("claude.dialog.shared.json.hit") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.shared.json.format.bg") != nullptr);
    }
    // 编辑态底部有「删除」键；关掉（取消）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.dialog.shared.delete.bg") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.cancel.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, p.value().id);
    return true;
})

// 2. 新建片段弹窗 → 填名/JSON → 保存 → DB 往返
REGISTER_TEST(claude_module_shared_editor_create,
              "claude_module: 新建共享配置片段保存落库（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_create");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    // 点「+ 新建片段」→ 弹窗开（新建态无删除键）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 1.0f);
        // 新建态无「删除」键（.bg 是按钮点击节点；根 id「claude.dialog.shared
        // .delete」属于常驻构建的删除确认弹窗外框，恒存在、不能拿来判按钮）
        DOCTEST_CHECK(ui.find("claude.dialog.shared.delete.bg") == nullptr);
    }
    // 空名保存 → 红字（name 必填先行校验）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.confirm.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* err = ui.find("claude.dialog.shared.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("不能为空") != std::string::npos);
    }
    // 填名 + 高级设置 JSON（弹窗 JSON 化）→ 创建
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.name.hit", "网关env"));
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.json.hit",
                           R"({"MY_GW":"v1","gateway":{"timeout":30,"retry":true}})"));
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.confirm.bg"));
    }
    // 弹窗关 + 成功 toast
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("已创建"));
    }
    // DB 往返：name + 片段 JSON 落地（v11 起存 extra_settings_json 列）。
    // 断言经 platform::json 按对象核键值，含嵌套对象值——用 env 域 parseExtraEnv
    // 解析的话非字符串值即 fail，盖不住真实片段形态
    {
        auto list = env.svc->listSharedConfigs();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(list.value()[0].name, std::string("网关env"));
        auto doc = platform::json::parse(list.value()[0].extraSettingsJson);
        DOCTEST_REQUIRE(doc.isOk());
        DOCTEST_REQUIRE(doc.value().isObject());
        const auto obj = doc.value().asObject();
        DOCTEST_CHECK_EQ(obj.get("MY_GW").asString(), std::string("v1"));
        DOCTEST_REQUIRE(obj.get("gateway").isObject());   // 嵌套对象往返不失真
        const auto gw = obj.get("gateway").asObject();
        DOCTEST_CHECK(gw.get("timeout").isNumber());
        DOCTEST_CHECK_EQ(gw.get("timeout").asInt(), 30);
        DOCTEST_CHECK(gw.get("retry").isBool());
        DOCTEST_CHECK_EQ(gw.get("retry").asBool(), true);
    }

    mod->onStop();
    return true;
})

// 3. 绑定下拉（迁入高级配置页）
// 绑定保存落库、profile 自有同名键非破坏保留；env 页无绑定 UI、行恒为普通输入
REGISTER_TEST(claude_module_shared_bind_and_frozen_preview,
              "claude_module: 绑定下拉（高级配置页）选片段 + 保存落库 + 同名键保留（headless）", {
    TestEnv env;
    auto f = env.svc->createSharedConfig("片段A", R"({"MY_KEY":"shared-val"})");
    DOCTEST_REQUIRE(f.isOk());
    auto a = env.svc->createProfile("绑定测试", "https://s.example.com",
                                    "sk-ant-sbind-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    // 自有 env 带与片段同名的键（片段赢场景：行里保留，绑定/解绑不动物理数据）
    {
        module::claude::Profile p = a.value();
        p.extraEnvJson = R"({"MY_KEY":"own-val"})";
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_bind");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing>") : node->text;
    };

    // 选中卡片 → 编辑 → 高级配置页：初始「无（不绑定片段）」；env 页无绑定 UI
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* label = ui.find("claude.form.advanced.shared.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("无（不绑定片段）"));
        // env 页不再有绑定下拉（迁走后该节点不存在）
        eui::Ui ui2;
        compose(ui2);
        DOCTEST_CHECK(click(ui2, "claude.form.tabs.hit.2"));
        eui::Ui ui3;
        compose(ui3);
        DOCTEST_CHECK(ui3.find("claude.form.env.shared.dd.label") == nullptr);
        // 同名键行恒为普通输入（无冻结渲染）
        DOCTEST_CHECK(ui3.find("claude.form.env.row.0.key.hit") != nullptr);
        DOCTEST_CHECK(ui3.find("claude.form.env.row.0.frozen.key") == nullptr);
        DOCTEST_CHECK_EQ(textOf(ui3, "claude.form.env.row.0.key.text"),
                         std::string("MY_KEY"));
        eui::Ui ui4;
        compose(ui4);
        DOCTEST_CHECK(click(ui4, "claude.form.tabs.hit.3"));
    }
    // 高级配置页拉开绑定下拉 → 选「片段A」→ 提示行出现（片段赢语义明示）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.advanced.shared.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.advanced.shared.dd.item.1"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* label = ui.find("claude.form.advanced.shared.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("片段A"));
        const auto* hint = ui.find("claude.form.advanced.json.boundhint");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_CHECK(hint->text.find("片段A") != std::string::npos);
    }
    // 保存 → 绑定落库；自有同名键非破坏保留（片段赢由生成时叠加保证）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().sharedConfigId, f.value().id);
        auto ee = module::claude::parseExtraEnv(got.value().extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("MY_KEY"), std::string("own-val"));
    }
    // 重开编辑：绑定下拉回显片段A。先等后台刷新把绑定带进缓存（卡片「共享」
    // 角标 = 缓存里 sharedConfigId != 0 的可观测信号；与真实应用一致）
    bool sawBadge = false;
    for (int i = 0; i < 250 && !sawBadge; ++i) {
        eui::Ui ui;
        compose(ui);
        if (ui.find("claude.profile.0.shared.badge") != nullptr) {
            sawBadge = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(sawBadge);
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* label = ui.find("claude.form.advanced.shared.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("片段A"));
    }
    // 下拉切回「无」→ 保存 → 解绑落库（绑定态迁移的等价性：与迁移前同一
    // 「悬空归一 + 保存落库」口径）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.advanced.shared.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.advanced.shared.dd.item.0"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().sharedConfigId, 0);
        // 解绑回落：profile 自身值原样保留（片段值从不写进 profile 行）
        auto ee = module::claude::parseExtraEnv(got.value().extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_CHECK_EQ(ee.value().at("MY_KEY"), std::string("own-val"));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})


// 5. 详情行「共享配置：片段 A（高级属性预设）」+ 列表卡片「共享」角标
REGISTER_TEST(claude_module_shared_detail_row_and_badge,
              "claude_module: 详情共享配置行 + 列表「共享」角标（headless）", {
    TestEnv env;
    auto f = env.svc->createSharedConfig("片段A", R"({"MY_A":"1"})");
    DOCTEST_REQUIRE(f.isOk());
    auto bound = env.svc->createProfile("A绑定", "https://a.example.com",
                                        "sk-ant-sbdg-0001", "");
    DOCTEST_REQUIRE(bound.isOk());
    const int64_t bpid = bound.value().id;
    {
        module::claude::Profile p = bound.value();
        p.sharedConfigId = f.value().id;
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }
    auto unbound = env.svc->createProfile("B未绑", "https://b.example.com",
                                          "sk-ant-sbdg-0002", "");
    DOCTEST_REQUIRE(unbound.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_detail");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto textOf = [](eui::Ui& ui, const std::string& id) -> std::string {
        const auto* node = ui.find(id);
        return node == nullptr ? std::string("<missing>") : node->text;
    };

    // 列表序 = 名称升序 → [A绑定, B未绑]；角标只出现在绑定的那张卡
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.profile.0.shared.badge") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.shared.badge") == nullptr);
    }
    // 选中绑定的 → 详情行「共享配置：片段A（高级属性预设）」
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.field.shared.value"),
                         std::string("共享配置：片段A（高级属性预设）"));
    }
    // 选中未绑定的 → 「共享配置：未绑定」
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.field.shared.value"),
                         std::string("共享配置：未绑定"));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, bpid);
    cleanupProfileKey(kTestKeychainService, unbound.value().id);
    return true;
})

// 6. 弹窗管理链：共享片段编辑弹窗入 anyDialogOpen 门控 + Esc 序链最顶层
REGISTER_TEST(claude_module_shared_esc_chain_and_gating,
              "claude_module: 片段弹窗 Esc 最顶层关闭 + Ctrl+N 门控（headless）", {
    TestEnv env;
    auto f = env.svc->createSharedConfig("片段A", R"({"MY_A":"1"})");
    DOCTEST_REQUIRE(f.isOk());
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_esc");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto escPress = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::Escape;
        e.action = eui::KeyAction::Press;
        return e;
    };
    // 快捷键修饰位按平台取（macOS = Cmd，其余 = Ctrl；见 setShortcutModifier）
    auto ctrlN = [] {
        eui::KeyEvent e;
        e.key = eui::InputKey::N;
        e.action = eui::KeyAction::Press;
        test_support::setShortcutModifier(e.modifiers);
        return e;
    };

    // 1) 开共享片段编辑弹窗（新建态）→ Ctrl+N 被门控不消费（不得重置表单），
    //    Esc 关闭它（序链 = 画在最上层、最先关）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 1.0f);
        DOCTEST_CHECK(!mod->onKeyEvent(ctrlN()));   // anyDialogOpen 门控
        DOCTEST_CHECK(mod->onKeyEvent(escPress()));  // Esc 关最顶层
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
        const auto* formPanel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(formPanel != nullptr);
        DOCTEST_CHECK_EQ(formPanel->opacity, 0.0f);   // 未误开 profile 表单
    }
    // 2) 删除确认弹窗（编辑态开「删除」）在最顶层：Esc 先关确认、编辑器还开着
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.card.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.delete.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* delPanel = ui.find("claude.dialog.shared.delete.panel");
        DOCTEST_REQUIRE(delPanel != nullptr);
        DOCTEST_CHECK_EQ(delPanel->opacity, 1.0f);
        DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* delPanel = ui.find("claude.dialog.shared.delete.panel");
        DOCTEST_REQUIRE(delPanel != nullptr);
        DOCTEST_CHECK_EQ(delPanel->opacity, 0.0f);   // 确认先关
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 1.0f);      // 编辑器还开着
        DOCTEST_CHECK(mod->onKeyEvent(escPress()));  // 再 Esc 关编辑器
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
        DOCTEST_CHECK(!mod->onKeyEvent(escPress()));  // 无弹窗不消费
    }

    mod->onStop();
    return true;
})

// 7. 删除片段 → 同事务解绑绑定 Profile → toast「N 个 Profile 已解绑」
REGISTER_TEST(claude_module_shared_delete_unbinds,
              "claude_module: 删除片段解绑 N 个 Profile（headless）", {
    TestEnv env;
    auto f = env.svc->createSharedConfig("片段A", R"({"MY_A":"1"})");
    DOCTEST_REQUIRE(f.isOk());
    const int64_t fid = f.value().id;
    std::vector<int64_t> pids;
    for (int i = 0; i < 2; ++i) {
        auto p = env.svc->createProfile("绑定" + std::to_string(i),
                                        "https://d.example.com",
                                        "sk-ant-sdel-000" + std::to_string(i), "");
        DOCTEST_REQUIRE(p.isOk());
        module::claude::Profile pp = p.value();
        pp.sharedConfigId = fid;
        DOCTEST_REQUIRE(env.svc->updateProfile(pp).isOk());
        pids.push_back(p.value().id);
    }
    auto free = env.svc->createProfile("未绑定", "https://f.example.com",
                                       "sk-ant-sdel-0009", "");
    DOCTEST_REQUIRE(free.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_del");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    // 开编辑 → 删除 → 确认 → toast 含「2 个 Profile 已解绑」
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.card.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.delete.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* delPanel = ui.find("claude.dialog.shared.delete.panel");
        DOCTEST_REQUIRE(delPanel != nullptr);
        DOCTEST_CHECK_EQ(delPanel->opacity, 1.0f);
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.delete.confirm.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("已删除"));
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("片段A") != std::string::npos);
        DOCTEST_CHECK(msg->text.find("2 个 Profile 已解绑") != std::string::npos);
        // 编辑弹窗随删除一并关闭（编辑目标已不存在）
        const auto* panel = ui.find("claude.dialog.shared.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK_EQ(panel->opacity, 0.0f);
    }
    // DB：片段删除 + 绑定全部同事务解绑；未绑定的不受影响
    {
        auto list = env.svc->listSharedConfigs();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE(list.value().empty());
        for (const int64_t pid : pids) {
            auto got = env.svc->getProfile(pid);
            DOCTEST_REQUIRE(got.isOk());
            DOCTEST_CHECK_EQ(got.value().sharedConfigId, 0);
        }
        auto got = env.svc->getProfile(free.value().id);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().sharedConfigId, 0);
    }

    mod->onStop();
    for (const int64_t pid : pids) cleanupProfileKey(kTestKeychainService, pid);
    cleanupProfileKey(kTestKeychainService, free.value().id);
    return true;
})

// R4（P-B1）：claude_ui_logic 抽出的 4 个纯函数等价断言
// 目的：证明「匿名 ns → claude_ui_logic.{h,cpp}」的搬迁**逐字等价**（spec
// 只在抽出的纯函数上补等价断言（不算新功能测试）。取值逐条钉死（边界 + 分支各一）。
REGISTER_TEST(claude_ui_logic_pure_functions,
              "claude_ui_logic: 纯函数等价断言（延时/UTF-8 截断/模型摘要/导入名）", {
    using module::claude::formatLatency;
    using module::claude::modelsSummary;
    using module::claude::suggestImportName;
    using module::claude::truncateUtf8;

    // formatLatency：<=0 → "-"；<1000 → "<n>ms"；否则 "%.1fs"
    DOCTEST_CHECK_EQ(formatLatency(0), std::string("-"));
    DOCTEST_CHECK_EQ(formatLatency(-5), std::string("-"));
    DOCTEST_CHECK_EQ(formatLatency(42), std::string("42ms"));
    DOCTEST_CHECK_EQ(formatLatency(999), std::string("999ms"));
    DOCTEST_CHECK_EQ(formatLatency(1000), std::string("1.0s"));
    DOCTEST_CHECK_EQ(formatLatency(1500), std::string("1.5s"));

    // truncateUtf8：不超限原样；超限回退到字符边界再补省略号
    DOCTEST_CHECK_EQ(truncateUtf8("abc", 3), std::string("abc"));
    DOCTEST_CHECK_EQ(truncateUtf8("abcdefghij", 5), std::string("abcde…"));
    // "αααα" = 8 字节；截 5 会落在第 3 个 α 的续字节上 → 回退到第 4 字节
    DOCTEST_CHECK_EQ(truncateUtf8("αααα", 5), std::string("αα…"));

    // modelsSummary：空配置 → "-"；model 列非空但无槽位配置 → 同样 "-"
    //（models_config 是槽位的唯一真相源，model 列不再参与读时回退）
    module::claude::Profile emptyProfile;
    DOCTEST_CHECK_EQ(modelsSummary(emptyProfile), std::string("-"));

    module::claude::Profile modelColumnOnly;
    modelColumnOnly.model = "claude-sonnet-4-5";
    DOCTEST_CHECK_EQ(modelsSummary(modelColumnOnly), std::string("-"));

    module::claude::ModelsConfig mc;
    mc.sonnet.model = "fangzhou/glm-5.3[1M]";
    auto mcJson = module::claude::serializeModelsConfig(mc);
    DOCTEST_REQUIRE(mcJson.isOk());
    module::claude::Profile slotOnly;
    slotOnly.modelsConfigJson = mcJson.value();
    DOCTEST_CHECK_EQ(modelsSummary(slotOnly),
                     std::string("（默认未配）· 槽位已配 1/6"));

    // 显示层剥 [1M]：默认槽位带标记 → 摘要显示剥除后的名字
    module::claude::ModelsConfig mcDef;
    mcDef.def.model = "m-def[1M]";
    auto defJson = module::claude::serializeModelsConfig(mcDef);
    DOCTEST_REQUIRE(defJson.isOk());
    module::claude::Profile defWithMarker;
    defWithMarker.modelsConfigJson = defJson.value();
    DOCTEST_CHECK_EQ(modelsSummary(defWithMarker),
                     std::string("默认 m-def · 槽位已配 1/6"));

    // suggestImportName：剥 scheme / 路径 / 端口；无 host → 「导入 <日期>」
    DOCTEST_CHECK_EQ(suggestImportName("https://api.example.com/v1"),
                     std::string("导入 api.example.com"));
    DOCTEST_CHECK_EQ(suggestImportName("https://api.example.com:8443/v1"),
                     std::string("导入 api.example.com"));
    DOCTEST_CHECK_EQ(suggestImportName("api.example.com"),
                     std::string("导入 api.example.com"));
    const std::string fallback = suggestImportName("");
    DOCTEST_CHECK_EQ(fallback.rfind("导入 ", 0), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(fallback.size(), static_cast<size_t>(17));   // 7 + YYYY-MM-DD
    return true;
})

// 28. 复制配置
// 纯函数形状：{"env":{...}} 顶层 / 尾斜杠剥离 / 三态凭据键 / 槽位 / extra_env /
// 片段冻结键赢 / 占位非明文 / 空占位无凭据键
REGISTER_TEST(claude_export_json_shape,
              "claude_module: buildProfileExportJson 形状（顶层 env/尾斜杠/三态键/片段赢/占位）", {
    module::claude::Profile p;
    p.id = 7;
    p.name = "导出";
    p.baseUrl = "https://exp.example.com/";   // 尾斜杠 → 导出时剥（写侧同口径）
    p.authField = "bearer";
    p.model = "m-expo";
    p.apiKeyHint = "sk-ant-***1234";
    p.modelsConfigJson = R"({"default":{"model":"m-expo","name":"我的模型"}})";
    p.extraEnvJson = R"({"MY_EXPORT_KEY":"v1"})";

    // 返回空 JsonObject = 解析失败（失败面由调用点的具体键断言接住变红；
    // 本框架 CHECK 宏含 return false，不能进非 bool 返回的 lambda）
    auto envOf = [](const std::string& json) -> platform::json::JsonObject {
        auto doc = platform::json::parse(json);
        if (!doc.isOk() || !doc.value().isObject()) return {};
        const auto envVal = doc.value().asObject().get("env");
        if (!envVal.isObject()) return {};
        return envVal.asObject();
    };

    // bearer：AUTH_TOKEN（占位，非明文）+ BASE_URL（尾斜杠已去）+ 槽位 + extra
    const std::string j1 = buildProfileExportJson(p, "", p.apiKeyHint);
    {
        auto doc = platform::json::parse(j1);
        DOCTEST_REQUIRE(doc.isOk());
        DOCTEST_CHECK(doc.value().isObject());
        // 顶层形状 = {"env": {...}}：恰好一键且名为 env（settings.json 形态）
        auto root = doc.value().asObject();
        DOCTEST_CHECK(root.get("env").isObject());
        const auto env = envOf(j1);
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_BASE_URL").asString(),
                         std::string("https://exp.example.com"));
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_AUTH_TOKEN").asString(),
                         std::string("sk-ant-***1234"));   // 占位，非明文
        DOCTEST_CHECK(!env.get("ANTHROPIC_API_KEY").isString());
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_MODEL").asString(),
                         std::string("m-expo"));
        DOCTEST_CHECK_EQ(env.get("MY_EXPORT_KEY").asString(), std::string("v1"));
    }
    // api_key / both 三态键名各断言
    {
        module::claude::Profile pk = p;
        pk.authField = "api_key";
        const auto env = envOf(buildProfileExportJson(pk, "", pk.apiKeyHint));
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_API_KEY").asString(),
                         std::string("sk-ant-***1234"));
        DOCTEST_CHECK(!env.get("ANTHROPIC_AUTH_TOKEN").isString());
    }
    {
        module::claude::Profile pb = p;
        pb.authField = "both";
        const auto env = envOf(buildProfileExportJson(pb, "", pb.apiKeyHint));
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_AUTH_TOKEN").asString(),
                         std::string("sk-ant-***1234"));
        DOCTEST_CHECK(!env.get("ANTHROPIC_API_KEY").isString());
    }
    // 空占位（Keychain 读不到）→ 无凭据键
    {
        const auto env = envOf(buildProfileExportJson(p, "", std::string()));
        DOCTEST_CHECK(!env.get("ANTHROPIC_AUTH_TOKEN").isString());
        DOCTEST_CHECK(!env.get("ANTHROPIC_API_KEY").isString());
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_BASE_URL").asString(),
                         std::string("https://exp.example.com"));
    }
    // 绑定片段：片段键并入**顶层**（非 env 域），同键片段赢；env 域不吃片段
    {
        const std::string jf = buildProfileExportJson(
            p, R"({"MY_TOP_KEY":"frozen","SHARED_ONLY":"sv"})", p.apiKeyHint);
        auto docF = platform::json::parse(jf);
        DOCTEST_REQUIRE(docF.isOk());
        auto rootF = docF.value().asObject();
        DOCTEST_CHECK_EQ(rootF.get("MY_TOP_KEY").asString(),
                         std::string("frozen"));
        DOCTEST_CHECK_EQ(rootF.get("SHARED_ONLY").asString(), std::string("sv"));
        const auto envF = envOf(jf);
        DOCTEST_CHECK(!envF.get("MY_TOP_KEY").isString());
        DOCTEST_CHECK(!envF.get("SHARED_ONLY").isString());
    }
    // 坏 extraEnvJson：读路径整体忽略（与运行功能同口径），导出仍成功
    {
        module::claude::Profile pj = p;
        pj.extraEnvJson = "{not-json";
        const std::string j = buildProfileExportJson(pj, "", pj.apiKeyHint);
        DOCTEST_CHECK(!j.empty());
        const auto env = envOf(j);
        DOCTEST_CHECK(!env.get("MY_EXPORT_KEY").isString());
        DOCTEST_CHECK_EQ(env.get("ANTHROPIC_BASE_URL").asString(),
                         std::string("https://exp.example.com"));
    }
    return true;
})

// 详情动作行 4 键（启用/测试连通/编辑/运行）；删除 / 建副本 / 复制配置三键
// **只**保留列表右键菜单入口（业务逻辑同源保留：requestDelete /
// requestDuplicate / requestCopyConfig）
REGISTER_TEST(claude_copyconfig_ctx_menu_after_row_slim,
              "claude_module: 详情动作行 4 键 + 复制配置仅右键菜单（文案/节点收口/点击派发）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_copycfg");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

        // 库空（shown 为 null）：三键节点一律不在动作行，
    // 4 键仍在（防误渲染提示）
    {
        eui::Ui ui;
        compose(ui);
        for (const char* k : {"claude.act.del", "claude.act.dup",
                              "claude.act.copyconfig"}) {
            DOCTEST_CHECK(ui.find(std::string(k) + ".bg") == nullptr);
        }
        DOCTEST_CHECK(ui.find("claude.act.enable.bg") != nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.bg") != nullptr);
    }

    auto a = env.svc->createProfile("导出源", "https://cfg.example.com",
                                    "sk-ant-cfgu-5678", "m");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    mod->onPageEnter();
    // 等卡片进缓存快照（服务侧造数 → onPageEnter 重查，同 27g 用例口径）
    bool cardReady = false;
    for (int i = 0; i < 300 && !cardReady; ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui);
        cardReady = ui.find("claude.profile.0.bg") != nullptr;
        if (!cardReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(cardReady);

    // 动作行只剩 4 键（顺序：启用/测试连通/编辑/运行），且窄行下
    {
        eui::Ui ui;
        compose(ui);
        for (const char* k : {"claude.act.enable", "claude.act.test",
                              "claude.act.edit", "claude.act.run"}) {
            DOCTEST_CHECK(ui.find(std::string(k) + ".bg") != nullptr);
        }
        for (const char* k : {"claude.act.del", "claude.act.dup",
                              "claude.act.copyconfig"}) {
            DOCTEST_CHECK(ui.find(std::string(k) + ".bg") == nullptr);
        }
    }
    // 右键菜单 → 「复制配置」（item.2）→ 真派发：toast「已复制配置」
    //（headless 下 setClipboardText 经 GLFW 未 init 早退 = 安全 no-op，不写真
    // 剪贴板；剪贴板真写以真机验收）
    {
        eui::Ui ui;
        compose(ui);
        const auto* wrap = ui.find("claude.profile.0.wrap");
        DOCTEST_REQUIRE(wrap != nullptr);
        DOCTEST_REQUIRE(wrap->onContextMenu != nullptr);
        eui::PointerEvent pe;
        eui::Rect rect;
        pe.x = 400;
        pe.y = 300;
        wrap->onContextMenu(pe, rect);
        eui::Ui ui2;
        compose(ui2);
        DOCTEST_CHECK(click(ui2, "claude.list.ctx.level.0.item.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* title = ui.find("claude.toast.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("已复制配置"));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})





// 组 B（macOS 布局统一）
//   P1 动作区钉面板底（五页签分隔线 y 恒定）+ 内容框显式边界
//   P2 「最近调用」去掉 10 行硬上限 / 3 行硬下限，行数纯按可用高推
//   P3 「环境变量」列全部生效 env + 每键来源
//   P4 共享配置区块钉面板底 + 「+ 新建片段」恒可见可点
// 尺寸取**真实 macOS 页面尺寸** 1262×801（窗口 1470×801 − 侧边栏 208），
// 这是「真实窗口尺寸下的区域比例」类问题在无头侧唯一能覆盖的方式。
// 统一原则：容器可用高只有一个来源（bodyH）；贴底元素锚容器底。


namespace {

// 合成一帧 + 显式布局（元素 frame 需 layout 后才有值；见
// claude_shared_block_outside_viewport 同款写法）
void composeAtSize(::core::Module* mod, eui::Ui& ui, const char* page,
                   float w, float h) {
    eui::Screen screen;
    screen.width = w;
    screen.height = h;
    ui.begin(page);
    mod->compose(ui, screen);
    ui.end();
    ui.layout(w, h);
}

// 直调元素 onClick（无头无命中测试；点击路径本身由 probe / 组件用例覆盖）
bool clickById(eui::Ui& ui, const std::string& id) {
    const auto* node = ui.find(id);
    if (node == nullptr || node->onClick == nullptr) {
        std::cerr << "missing clickable: " << id << std::endl;
        return false;
    }
    node->onClick();
    return true;
}

// 造 n 条调用记录（含 429 / 超时两态，让「另有 N 条」行与状态列都有内容）
bool seedCalls(module::claude::ClaudeService* svc, int64_t pid, int n) {
    const std::time_t base = std::time(nullptr);
    for (int i = 0; i < n; ++i) {
        module::claude::ClaudeService::CallLogEntry e;
        e.profileId = pid;
        e.method = (i % 5 == 0) ? "GET" : "POST";
        e.path = (i % 5 == 0) ? "/v1/models" : "/v1/messages";
        e.statusCode = (i % 7 == 0) ? 429 : 200;
        e.latencyMs = 100 + i;
        e.startedAt = base + i;
        DOCTEST_REQUIRE(svc->logCall(e).isOk());
    }
    return true;
}

}  // namespace

// 组 B P1：全部页签的动作区必须落在同一条水平线上
// 用户主诉（原话）：「环境变量 最近调用 2 个 tab 下方的区域高度不对」。
// 根因：actY = paneBottom + 20 跟着内容底跑，面板底恒为 bodyH → 数据少时动作区
// 浮在上部，页签间的分隔线互差最大 171px。修后 actY = max(paneBottom + 20,
// bodyH − 动作块预留)，内容框底由容器底推出 → 五页签五值恒等。
REGISTER_TEST(claude_detail_actions_anchor_four_tabs,
              "claude_module: 详情动作区五页签同一条水平线（macOS 真实尺寸 1262x801）", {
    TestEnv env;
    auto p = env.svc->createProfile("锚点", "https://anchor.example.com",
                                    "sk-ant-anchor-0001", "m-anchor");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    // 环境变量页签给 2 个额外键（各页签内容底不相同才有判别力）
    auto g = env.svc->getProfile(pid);
    DOCTEST_REQUIRE(g.isOk());
    g.value().extraEnvJson = R"({"MY_ANCHOR_A":"1","MY_ANCHOR_B":"2"})";
    DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());
    DOCTEST_REQUIRE(seedCalls(env.svc.get(), pid, 3));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();   // 首载同步装载快照（H2），下面第一帧就有 recent

    constexpr float kW = 1262.0f;   // 真实 macOS 页面宽（窗口 1470 − 侧边栏 208）
    constexpr float kH = 801.0f;    // 真实 macOS 页面高（窗口内容高 801）

    float actY[5] = {0, 0, 0, 0, 0};
    float dividerY[5] = {0, 0, 0, 0, 0};
    float panelBottom = 0.0f;
    for (int tab = 0; tab < 5; ++tab) {
        if (tab > 0) {
            eui::Ui ui;
            composeAtSize(mod.get(), ui, "claude_anchor", kW, kH);
            DOCTEST_REQUIRE(clickById(
                ui, "claude.detail.tabs.hit." + std::to_string(tab)));
        }
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_anchor", kW, kH);
        const auto* act = ui.find("claude.detail.actions");
        const auto* divider = ui.find("claude.act.divider");
        const auto* panel = ui.find("claude.detail");
        DOCTEST_REQUIRE(act != nullptr);
        DOCTEST_REQUIRE(divider != nullptr);
        DOCTEST_REQUIRE(panel != nullptr);
        actY[tab] = act->frame.y;
        dividerY[tab] = divider->frame.y;
        panelBottom = panel->frame.y + panel->frame.height;
        // 贴底元素不得越出容器底（不变量）
        DOCTEST_CHECK(act->frame.y + act->frame.height <= panelBottom + 0.5f);
    }
    // 五页签完全相等（这五值互不相等即红）
    for (int tab = 1; tab < 5; ++tab) {
        DOCTEST_CHECK_EQ(actY[tab], actY[0]);
        DOCTEST_CHECK_EQ(dividerY[tab], dividerY[0]);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 27h. 详情页「高级配置」pane
// tab 3 显示生效方言 / 备用名只读两行；tab 4 仍是最近调用；几何不越内容框
REGISTER_TEST(claude_detail_advanced_pane_readonly,
              "claude_module: 详情高级配置 pane（自动推断/显式覆盖/备用名/悬空/tab4 仍最近调用）", {
    TestEnv env;
    // 主家（显式覆盖 opencode_zen）+ 中家（未覆盖 → 自动按站点推断）+ 备家
    auto bak = env.svc->createProfile("备家", "https://fb.example.com",
                                      "sk-ant-advb-0001", "m");
    DOCTEST_REQUIRE(bak.isOk());
    auto mainP = env.svc->createProfile("主家", "https://zen.opencode.ai/v1",
                                        "sk-ant-advm-0001", "m",
                                        /*activateIfNoneActive=*/false,
                                        /*sharedConfigId=*/0, "both",
                                        /*dialectOverride=*/"opencode_zen",
                                        /*fallbackProfileId=*/bak.value().id);
    DOCTEST_REQUIRE(mainP.isOk());
    auto mid = env.svc->createProfile("中家", "https://api.example.com",
                                      "sk-ant-advd-0001", "m");
    DOCTEST_REQUIRE(mid.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    constexpr float kW = 1262.0f;
    constexpr float kH = 801.0f;

    // 按名字点卡片（同上，不依赖中文名排序的列表序）
    auto clickCardNamed = [&](eui::Ui& ui, const char* nm) -> bool {
        for (int i = 0; i < 8; ++i) {
            const auto* name = ui.find("claude.profile." + std::to_string(i) +
                                       ".name");
            if (name != nullptr && name->text == nm) {
                return clickById(ui, "claude.profile." + std::to_string(i) +
                                         ".bg");
            }
        }
        return false;
    };

    // 显式覆盖：zen host + 覆盖 opencode_zen → 「opencode zen（显式）」；
    // 备用名 = 快照解析的「备家」
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d1", kW, kH);
        DOCTEST_REQUIRE(clickCardNamed(ui, "主家"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d2", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d3", kW, kH);
        const auto* dialect = ui.find("claude.field.dialect.value");
        const auto* fallback = ui.find("claude.field.fallback.value");
        DOCTEST_REQUIRE(dialect != nullptr);
        DOCTEST_REQUIRE(fallback != nullptr);
        DOCTEST_CHECK_EQ(dialect->text, std::string("opencode zen（显式）"));
        DOCTEST_CHECK_EQ(fallback->text, std::string("备家"));
        // 几何：pane 内容底沿不越内容框（contentBottom 口径）
        const auto* box = ui.find("claude.detail.contentbox");
        DOCTEST_REQUIRE(box != nullptr);
        const float boxBottom = box->frame.y + box->frame.height;
        DOCTEST_CHECK(fallback->frame.y + fallback->frame.height <=
                      boxBottom + 0.5f);
    }
    // 未覆盖 → 「自动（按站点推断）→ <生效名>」；host 非 opencode → anthropic
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d4", kW, kH);
        DOCTEST_REQUIRE(clickCardNamed(ui, "中家"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d5", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d6", kW, kH);
        const auto* dialect = ui.find("claude.field.dialect.value");
        DOCTEST_REQUIRE(dialect != nullptr);
        DOCTEST_CHECK_EQ(dialect->text,
                         std::string("自动（按站点推断）→ anthropic"));
        const auto* fallback = ui.find("claude.field.fallback.value");
        DOCTEST_REQUIRE(fallback != nullptr);
        DOCTEST_CHECK_EQ(fallback->text, std::string("无"));
    }
    // 悬空备用（片段已删同款口径）：解析为「（已删除）」；等后台刷新带新值
    {
        auto g = env.svc->getProfile(mid.value().id);
        DOCTEST_REQUIRE(g.isOk());
        g.value().fallbackProfileId = 999999;   // 悬空 id
        DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());
    }
    mod->onPageEnter();
    bool sawDangling = false;
    for (int i = 0; i < 300 && !sawDangling; ++i) {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d7", kW, kH);
        if (clickCardNamed(ui, "中家")) {
            eui::Ui ui2;
            composeAtSize(mod.get(), ui2, "adv_d8", kW, kH);
            if (clickById(ui2, "claude.detail.tabs.hit.3")) {
                eui::Ui ui3;
                composeAtSize(mod.get(), ui3, "adv_d9", kW, kH);
                const auto* fallback = ui3.find("claude.field.fallback.value");
                if (fallback != nullptr && fallback->text == "（已删除）") {
                    sawDangling = true;
                }
            }
        }
        if (!sawDangling) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(sawDangling);

    // tab 4 仍是最近调用（高级配置插入后顺移，既有导航锚 +1 的正确性钉子）：
    // hit.4 帧有 calls 元素、无高级配置元素
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d10", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.4"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "adv_d11", kW, kH);
        DOCTEST_CHECK(ui.find("claude.calls.empty") != nullptr);
        DOCTEST_CHECK(ui.find("claude.field.dialect.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.field.fallback.bg") == nullptr);
        // 页签文字↔索引映射钉：把「高级配置」改插中间即红；只断 pane 元素不断
        const auto* lab3 = ui.find("claude.detail.tabs.label.3");
        const auto* lab4 = ui.find("claude.detail.tabs.label.4");
        DOCTEST_REQUIRE(lab3 != nullptr);
        DOCTEST_REQUIRE(lab4 != nullptr);
        DOCTEST_CHECK_EQ(lab3->text, std::string("高级配置"));
        DOCTEST_CHECK_EQ(lab4->text, std::string("最近调用"));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, mainP.value().id);
    cleanupProfileKey(kTestKeychainService, mid.value().id);
    cleanupProfileKey(kTestKeychainService, bak.value().id);
    return true;
})

// 4 键几何（macOS 真实页面 1262×801）：actW == (w0 − 3×12) / 4 且行右缘与容器
// 右缘对齐（等分无溢出）；末键 = 运行。放在 composeAtSize 声明之后
REGISTER_TEST(claude_copyconfig_seven_keys_geometry,
              "claude_module: 动作行 4 键 actW 等分几何（macOS 真实尺寸）", {
    TestEnv env;
    auto a = env.svc->createProfile("几何源", "https://geo.example.com",
                                    "sk-ant-geo-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    constexpr float kW = 1262.0f;
    constexpr float kH = 801.0f;
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_copycfg_geom", kW, kH);
    const auto* row = ui.find("claude.detail.actions");
    const auto* run = ui.find("claude.act.run.bg");
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(run != nullptr);
    const float w0 = row->frame.width;
    // 动作行 4 键，末键（运行）右缘对齐断言据此定宽
    const float expect = (w0 - 12.0f * 3.0f) / 4.0f;
    DOCTEST_CHECK(std::fabs(run->frame.width - expect) < 0.01f);
    // 行右缘对齐：末键右缘 == row 右缘（浮点 0.5px 容差）
    const float rowRight = row->frame.x + row->frame.width;
    const float runRight = run->frame.x + run->frame.width;
    DOCTEST_CHECK(std::fabs(rowRight - runRight) < 0.5f);
    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 组 B P2：行数纯按可用高推
REGISTER_TEST(claude_calls_rows_by_available_height,
              "claude_module: 最近调用行数按可用高推 + 截断有「另有 N 条」", {
    TestEnv env;
    auto p = env.svc->createProfile("行数", "https://rows.example.com",
                                    "sk-ant-rows-0001", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    DOCTEST_REQUIRE(seedCalls(env.svc.get(), pid, 30));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    constexpr float kW = 1262.0f;
    constexpr float kH = 801.0f;
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_rows", kW, kH);
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.4"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_rows", kW, kH);

    // 预算：内容框底 = bodyH − 85 − 20；行区 = 内容框底 − 表头底(24) −
    // 「查看全部」带(8+24)。本尺寸（bodyH=607 → 内容框底 502、表头底 136、
    // 带 32）→ floor(334/24) = 13 行（写死 clamp(...,3,10) 恒为 10 行）
    int drawn = 0;
    while (ui2.find("claude.calls.t." + std::to_string(drawn)) != nullptr) {
        ++drawn;
    }
    DOCTEST_CHECK_EQ(drawn, 13);
    DOCTEST_CHECK(ui2.find("claude.calls.t.13") == nullptr);   // 不画越界行

    // 截断必须有可见表达（与 env 页签同款口径）
    const auto* overflow = ui2.find("claude.calls.overflow");
    DOCTEST_REQUIRE(overflow != nullptr);
    DOCTEST_CHECK(overflow->text.find("另有 17 条") != std::string::npos);

    // 行区不越过内容框底：末行底 ≤ 内容框底（真实裁剪下沿）
    const auto* last = ui2.find("claude.calls.d.12");
    DOCTEST_REQUIRE(last != nullptr);
    const auto* viewall = ui2.find("claude.calls.viewall.hit");
    DOCTEST_REQUIRE(viewall != nullptr);
    DOCTEST_CHECK(last->frame.y + last->frame.height <=
                  viewall->frame.y + 0.5f);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 硬下限 3 会把 3 行画进动作区（越界）；这里是画 0 行 + 明确文案。
REGISTER_TEST(claude_calls_no_rows_when_too_short,
              "claude_module: 内容区不足一行时画 0 行 + 空态（含 F-02 叠印/越框锁）", {
    TestEnv env;
    auto p = env.svc->createProfile("矮窗", "https://short.example.com",
                                    "sk-ant-short-001", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    DOCTEST_REQUIRE(seedCalls(env.svc.get(), pid, 30));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    // 480 高 → bodyH = 480 − 80 − 64 − 16 − 34(引导条) = 286；
    // 内容框底 = 286 − 85 − 20 = 181；行区 = 181 − 136 − 32 = 13 < 24 → 0 行
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_short", 1000.0f, 480.0f);
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.4"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_short", 1000.0f, 480.0f);

    DOCTEST_CHECK(ui2.find("claude.calls.t.0") == nullptr);   // 不画越界行
    const auto* empty = ui2.find("claude.calls.empty");
    DOCTEST_REQUIRE(empty != nullptr);
    DOCTEST_CHECK(empty->text.find("不足") != std::string::npos);
    // 0 行档的两条硬约束
    // ① 不画「另有 N 条」—— 它与空态行同坐标会叠印
    DOCTEST_CHECK(ui2.find("claude.calls.overflow") == nullptr);
    // ② 空态行与「查看全部」带都不得越出内容框（真实裁剪下沿 = 内容框底）
    const auto* box = ui2.find("claude.detail.contentbox");
    DOCTEST_REQUIRE(box != nullptr);
    const float boxBottom = box->frame.y + box->frame.height;
    DOCTEST_CHECK(empty->frame.y + empty->frame.height <= boxBottom + 0.5f);
    if (const auto* va = ui2.find("claude.calls.viewall.hit")) {
        DOCTEST_CHECK(va->frame.y + va->frame.height <= boxBottom + 0.5f);
    }
        // 动作区仍在面板内（贴底锚定 + actYHi 夹取语义）
    const auto* act = ui2.find("claude.detail.actions");
    const auto* panel = ui2.find("claude.detail");
    DOCTEST_REQUIRE(act != nullptr);
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK(act->frame.y + act->frame.height <=
                  panel->frame.y + panel->frame.height + 0.5f);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 最近调用明细列「本地调用速率」
// output_tokens > 0 的调用有值；非 2xx / 0 token 行显示「—」（不是 0 —— 0 是
// 合法速率值，两者必须可区分）。口径单源 core/call_rate.h（与首页卡同一处）。
REGISTER_TEST(claude_calls_rate_column,
              "claude_module: 最近调用速率列（手算值 + 非 2xx/0 token 显示 —）", {
    TestEnv env;
    auto p = env.svc->createProfile("速率", "https://rate.example.com",
                                    "sk-ant-rate-0001", "m-rate");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    // 用量列由流末回填（logCall 不写 output_tokens），故种子直接落库；
    // started_at 递减 → 列表行序（started_at DESC）确定：
    //   行 0  200 / 2000ms / out 100 → 50.0
    //   行 1  200 / 1000ms / out   0 → 「—」
    //   行 2  500 /  500ms / out 900 → 「—」（非 2xx）
    //   行 3  200 / 4000ms / out 600 → 150.0（NULL 用量的老行见行 4）
    //   行 4  200 / 1000ms / NULL    → 「—」（未采集）
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& sq = env.db->db();
    const std::string pidStr = std::to_string(pid);
    DOCTEST_REQUIRE(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, status_code, "
        "latency_ms, output_tokens, request_bytes, response_bytes, error, "
        "started_at) VALUES "
        "(" + pidStr + ", 'POST', '/v1/messages', 200, 2000, 100, 10, 20, '', " +
        std::to_string(now - 100) + "), "
        "(" + pidStr + ", 'POST', '/v1/messages', 200, 1000, 0, 10, 20, '', " +
        std::to_string(now - 200) + "), "
        "(" + pidStr + ", 'POST', '/v1/messages', 500, 500, 900, 10, 20, "
        "'boom', " + std::to_string(now - 300) + "), "
        "(" + pidStr + ", 'POST', '/v1/messages', 200, 4000, 600, 10, 20, '', " +
        std::to_string(now - 400) + "), "
        "(" + pidStr + ", 'POST', '/v1/messages', 200, 1000, NULL, 10, 20, '', " +
        std::to_string(now - 500) + ")").isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    constexpr float kW = 1262.0f;
    constexpr float kH = 801.0f;
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_rate", kW, kH);
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.4"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_rate", kW, kH);

    const auto textOf = [](eui::Ui& u, const std::string& id) -> std::string {
        const auto* el = u.find(id);
        return el != nullptr ? el->text : std::string();
    };
    // 表头：新列在场，且四列 x 区间互不重叠（既有列没被挤掉/叠印）
    const auto* hPath = ui2.find("claude.calls.h.path");
    const auto* hStatus = ui2.find("claude.calls.h.status");
    const auto* hDur = ui2.find("claude.calls.h.dur");
    const auto* hRate = ui2.find("claude.calls.h.rate");
    DOCTEST_REQUIRE(hPath != nullptr);
    DOCTEST_REQUIRE(hStatus != nullptr);
    DOCTEST_REQUIRE(hDur != nullptr);
    DOCTEST_REQUIRE(hRate != nullptr);
    DOCTEST_CHECK_EQ(hRate->text, std::string("速率 (tok/s)"));
    DOCTEST_CHECK(hPath->frame.x + hPath->frame.width <= hStatus->frame.x + 0.5f);
    DOCTEST_CHECK(hStatus->frame.x + hStatus->frame.width <=
                  hDur->frame.x + 0.5f);
    DOCTEST_CHECK(hDur->frame.x + hDur->frame.width <= hRate->frame.x + 0.5f);
    DOCTEST_CHECK(hRate->frame.x + hRate->frame.width <=
                  hPath->frame.x + 1262.0f);   // 不越出面板右缘

    // 手算值对照（回退成「累计/实时速率口径」或把无速率写成 0，本组即红）
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.rate.0"), std::string("50.0"));
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.rate.1"), std::string("—"));
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.rate.2"), std::string("—"));
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.rate.3"), std::string("150.0"));
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.rate.4"), std::string("—"));
    // 既有列信息仍在（时间 / 方法·路径 / 状态 / 耗时）
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.p.0"),
                     std::string("POST /v1/messages"));
    DOCTEST_CHECK_EQ(textOf(ui2, "claude.calls.s.0"), std::string("200"));
    DOCTEST_CHECK(textOf(ui2, "claude.calls.d.0").empty() == false);
    DOCTEST_CHECK(textOf(ui2, "claude.calls.t.0").empty() == false);
    // 速率列与耗时列不同源（60ms 的耗时不会算成速率）
    DOCTEST_CHECK(textOf(ui2, "claude.calls.rate.0") !=
                  textOf(ui2, "claude.calls.d.0"));

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 两处文本垂直居中 + 「查看全部」改按钮
// 中心（EUI-NEO runtime_render.h renderText：y = frame.y + frame.height/2），
// 故「flag == Center」+「frame 中心 == 行框中心（±0.5）」两条合起来才是
// 「文本在行内垂直居中」的数值证据 —— 只写 position/size 的那一版（无 flag）
// 文本从行框顶沿起排，本用例即红。
REGISTER_TEST(claude_detail_vertical_center_and_viewall_button,
              "claude_module: 基础信息/模型映射文本垂直居中 + 查看全部是按钮", {
    TestEnv env;
    auto p = env.svc->createProfile("居中", "https://center.example.com",
                                    "sk-ant-center-01", "m-center");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    DOCTEST_REQUIRE(seedCalls(env.svc.get(), pid, 3));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    constexpr float kW = 1262.0f;
    constexpr float kH = 801.0f;
    const auto centerY = [](const core::dsl::Element& el) {
        return el.frame.y + el.frame.height * 0.5f;
    };
    const auto isCentered = [](const core::dsl::Element& el) {
        return el.verticalAlign == eui::VerticalAlign::Center;
    };

    // ⑤ 基础信息（tab 0 默认）：标签列与值列都相对各自行高居中
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_center_basic", kW, kH);
        // {文本 id, 行框 id（nullptr = 文本自身框即该行）}
        const std::pair<const char*, const char*> kFields[] = {
            {"claude.field.name.label", nullptr},
            {"claude.field.name.value", "claude.field.name.bg"},
            {"claude.field.url.label", nullptr},
            {"claude.field.url.value", "claude.field.url.bg"},
            {"claude.field.token.label", nullptr},
            {"claude.field.token.value", "claude.field.token.bg"},
            {"claude.field.takeover.value", nullptr},
            {"claude.field.shared.value", nullptr},
        };
        for (const auto& f : kFields) {
            const auto* txt = ui.find(f.first);
            DOCTEST_REQUIRE_MESSAGE(txt != nullptr,
                                    std::string("缺文本元素 ") + f.first);
            DOCTEST_CHECK_MESSAGE(isCentered(*txt),
                                  std::string(f.first) + " 未垂直居中（无 Center 对齐）");
            if (f.second != nullptr) {
                const auto* row = ui.find(f.second);
                DOCTEST_REQUIRE(row != nullptr);
                DOCTEST_CHECK_MESSAGE(
                    std::fabs(centerY(*txt) - centerY(*row)) <= 0.5f,
                    std::string(f.first) + " 文本中心 != 行框中心");
            } else {
                // 无独立行框的元素：行 = 自身框，行高必须等于行高设定
                //（14/16）—— 行框与行高不一致时 ink 中心必然偏
                DOCTEST_CHECK_EQ(txt->lineHeight, txt->frame.height);
            }
        }
        // 水平关系未被动过：标签在左（x = x0）、值列有左内缩 12
        const auto* nameLabel = ui.find("claude.field.name.label");
        const auto* nameValue = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameLabel != nullptr);
        DOCTEST_REQUIRE(nameValue != nullptr);
        DOCTEST_CHECK(nameValue->frame.x > nameLabel->frame.x);
    }

    // ④ 模型映射（tab 1）：每个槽位行的名称列/值列都相对 32 高行框居中
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_center_models", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.1"));
        eui::Ui ui2;
        composeAtSize(mod.get(), ui2, "claude_center_models", kW, kH);
        int rows = 0;
        for (int i = 0; i < 8; ++i) {   // 槽位固定 6 行，上界留余量
            const std::string idx = std::to_string(i);
            const auto* bg = ui2.find("claude.slot.bg." + idx);
            if (bg == nullptr) break;
            const auto* name = ui2.find("claude.slot.name." + idx);
            const auto* val = ui2.find("claude.slot.val." + idx);
            DOCTEST_REQUIRE(name != nullptr);
            DOCTEST_REQUIRE(val != nullptr);
            DOCTEST_CHECK_MESSAGE(isCentered(*name),
                                  "槽位名称列未垂直居中");
            DOCTEST_CHECK_MESSAGE(isCentered(*val), "槽位值列未垂直居中");
            DOCTEST_CHECK(std::fabs(centerY(*name) - centerY(*bg)) <= 0.5f);
            DOCTEST_CHECK(std::fabs(centerY(*val) - centerY(*bg)) <= 0.5f);
            ++rows;
        }
        DOCTEST_CHECK_EQ(rows, 6);
    }

    // ⑥ 最近调用（tab 3）：「查看全部 →」是按钮，点击开弹窗
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_center_calls", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.4"));
        eui::Ui ui2;
        composeAtSize(mod.get(), ui2, "claude_center_calls", kW, kH);
        const auto* hit = ui2.find("claude.calls.viewall.hit");
        DOCTEST_REQUIRE(hit != nullptr);
        // 按钮三要素：可点击接线 / hover·按下三态 / 圆角 + 描边
        //（退化成纯文本链接 = 无 onClick、无三态、无描边 → 本组红）
        DOCTEST_CHECK(hit->onClick != nullptr);
        DOCTEST_CHECK(hit->hasStateColors);
        DOCTEST_CHECK(hit->interactive);
        DOCTEST_CHECK(hit->radius > 0.0f);
        DOCTEST_CHECK(hit->border.width > 0.0f);
        const auto* label = ui2.find("claude.calls.viewall");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("查看全部 →"));
        DOCTEST_CHECK(isCentered(*label));
        DOCTEST_CHECK(std::fabs(centerY(*label) - centerY(*hit)) <= 0.5f);

        // 点击行为不变：开完整调用记录弹窗（有界等待，数据行是异步的）
        DOCTEST_REQUIRE(hit->onClick != nullptr);
        hit->onClick();
        bool opened = false;
        for (int i = 0; i < 40 && !opened; ++i) {
            eui::Ui ui3;
            composeAtSize(mod.get(), ui3, "claude_center_calls", kW, kH);
            opened = ui3.find("claude.dialog.calls") != nullptr;
            if (!opened) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        DOCTEST_CHECK(opened);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 调用记录弹窗分页控件垂直对齐
// 是文本没做垂直居中（ink 从行框顶沿起排）。判据 = 三者 frame 中心 y 相等
// （±0.5）+ 行框高相等（不靠给文本加 padding 蒙）。回退掉
// claude.dialog.calls.page 的 verticalAlign → 本用例红。
REGISTER_TEST(claude_dialog_pagination_vertical_align,
              "claude_module: 调用记录弹窗页码文本与翻页按钮同一水平中线", {
    TestEnv env;
    auto p = env.svc->createProfile("分页", "https://page.example.com",
                                    "sk-ant-page-0001", "m-page");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    DOCTEST_REQUIRE(seedCalls(env.svc.get(), pid, 30));  // >1 页

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    constexpr float kW = 1262.0f;
    constexpr float kH = 801.0f;
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_page_align", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.4"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_page_align", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.calls.viewall.hit"));
    }
    // 等后台查询落地（总匹配数到位 → 页数 = ceil(30/20) = 2；数据是
    // 异步的，每轮新起一帧收割，不靠固定睡眠）
    bool loaded = false;
    for (int i = 0; i < 400 && !loaded; ++i) {
        eui::Ui frame;
        composeAtSize(mod.get(), frame, "claude_page_align", kW, kH);
        const auto* t = frame.find("claude.dialog.calls.page");
        loaded = t != nullptr && t->text.find("2 页") != std::string::npos;
        if (!loaded) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_REQUIRE(loaded);
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_page_align", kW, kH);
    const auto* page = ui.find("claude.dialog.calls.page");
    const auto* prev = ui.find("claude.dialog.calls.prev");
    const auto* next = ui.find("claude.dialog.calls.next");
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_REQUIRE(prev != nullptr);
    DOCTEST_REQUIRE(next != nullptr);
    const auto centerY = [](const core::dsl::Element& el) {
        return el.frame.y + el.frame.height * 0.5f;
    };
    const auto fmt = [&](const char* name, const core::dsl::Element& el) {
        return std::string(name) + "{y=" + std::to_string(el.frame.y) + ",h=" +
               std::to_string(el.frame.height) + ",cy=" +
               std::to_string(centerY(el)) + "}";
    };
    // 同一水平中线（±0.5）
    DOCTEST_CHECK_MESSAGE(std::fabs(centerY(*page) - centerY(*prev)) <= 0.5f,
                          fmt("page", *page) + fmt("prev", *prev));
    DOCTEST_CHECK_MESSAGE(std::fabs(centerY(*page) - centerY(*next)) <= 0.5f,
                          fmt("page", *page) + fmt("next", *next));
    DOCTEST_CHECK(std::fabs(centerY(*prev) - centerY(*next)) <= 0.5f);
    // 行框/容器高一致（40 = 40 = 40，不是给文本加 padding 蒙出来的）
    DOCTEST_CHECK_EQ(page->frame.height, prev->frame.height);
    DOCTEST_CHECK_EQ(page->frame.height, next->frame.height);
    DOCTEST_CHECK_EQ(page->frame.height, 40.0f);
    DOCTEST_CHECK_EQ(page->lineHeight, page->frame.height);  // 行高口径一致
    // 文本自身垂直居中（唯一改动点；回退即红）
    DOCTEST_CHECK(page->verticalAlign == eui::VerticalAlign::Center);
    // 既有布局与状态不变：左→右顺序、页码在按钮右侧、禁用态（第 1 页 → 上一页禁）
    DOCTEST_CHECK(prev->frame.x < next->frame.x);
    DOCTEST_CHECK(next->frame.x < page->frame.x);
    const auto* prevBg = ui.find("claude.dialog.calls.prev.bg");
    const auto* nextBg = ui.find("claude.dialog.calls.next.bg");
    DOCTEST_REQUIRE(prevBg != nullptr);
    DOCTEST_REQUIRE(nextBg != nullptr);
    DOCTEST_CHECK(prevBg->disabled);
    DOCTEST_CHECK(!nextBg->disabled);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// Profile 卡 ACTIVE 胶囊徽标文字居中
// 文字 frame 中心 == 胶囊 frame 中心（x/y 各 ±0.5）+ 行高口径一致（文本
// lineHeight == 胶囊高，不靠 padding 蒙）。回退 active.text 的 verticalAlign
// → 本用例红；「共享」胶囊是同类项，一并钉住。
REGISTER_TEST(claude_profile_active_badge_centered,
              "claude_module: Profile 卡 ACTIVE / 共享 胶囊徽标文字居中", {
    TestEnv env;
    auto sc = env.svc->createSharedConfig("片段A", R"({"MY_K":"v"})");
    DOCTEST_REQUIRE(sc.isOk());
    auto p = env.svc->createProfile("徽标", "https://badge.example.com",
                                    "sk-ant-badge-001", "m-badge");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    auto g = env.svc->getProfile(pid);
    DOCTEST_REQUIRE(g.isOk());
    g.value().sharedConfigId = sc.value().id;   // 「共享」角标要绑定片段才画
    DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_badge", 1262.0f, 801.0f);

    // 判据 helper：返回空串 = 通过（本框架的断言宏展开含 return false，只能写在
    // 用例体里，helper 回传结论、由用例断）
    const auto pillProblem = [&](const std::string& textId,
                                 const std::string& bgId) -> std::string {
        const auto* txt = ui.find(textId);
        const auto* bg = ui.find(bgId);
        if (txt == nullptr) return "缺文本 " + textId;
        if (bg == nullptr) return "缺胶囊 " + bgId;
        const std::string box =
            textId + " text{cx=" +
            std::to_string(txt->frame.x + txt->frame.width * 0.5f) + ",cy=" +
            std::to_string(txt->frame.y + txt->frame.height * 0.5f) +
            ",h=" + std::to_string(txt->frame.height) + ",lh=" +
            std::to_string(txt->lineHeight) + "} bg{cx=" +
            std::to_string(bg->frame.x + bg->frame.width * 0.5f) + ",cy=" +
            std::to_string(bg->frame.y + bg->frame.height * 0.5f) +
            ",h=" + std::to_string(bg->frame.height) + "}";
        if (txt->verticalAlign != eui::VerticalAlign::Center) {
            return textId + " 未垂直居中（缺 Center 对齐）";
        }
        if (txt->horizontalAlign != eui::HorizontalAlign::Center) {
            return textId + " 未水平居中";
        }
        if (std::fabs((txt->frame.x + txt->frame.width * 0.5f) -
                      (bg->frame.x + bg->frame.width * 0.5f)) > 0.5f) {
            return textId + " 水平中心 != 胶囊中心 " + box;
        }
        if (std::fabs((txt->frame.y + txt->frame.height * 0.5f) -
                      (bg->frame.y + bg->frame.height * 0.5f)) > 0.5f) {
            return textId + " 垂直中心 != 胶囊中心 " + box;
        }
        // 行高口径一致（文本行高 == 胶囊高：不是靠 padding 顶下来的）
        if (std::fabs(txt->lineHeight - bg->frame.height) > 0.5f) {
            return textId + " 行高与胶囊高不一致 " + box;
        }
        return {};
    };
    const std::string activeBadge =
        pillProblem("claude.profile.0.active.text", "claude.profile.0.active.bg");
    DOCTEST_CHECK_MESSAGE(activeBadge.empty(), activeBadge);
    const std::string sharedBadge = pillProblem(
        "claude.profile.0.shared.badge.text", "claude.profile.0.shared.badge");
    DOCTEST_CHECK_MESSAGE(sharedBadge.empty(), sharedBadge);
    // 胶囊几何：§5 加宽后「当前选中」64×20 在卡片右上角（原 ACTIVE 48×20 放不下 4 全角字）
    const auto* abg = ui.find("claude.profile.0.active.bg");
    DOCTEST_REQUIRE(abg != nullptr);
    DOCTEST_CHECK_EQ(abg->frame.width, 64.0f);
    DOCTEST_CHECK_EQ(abg->frame.height, 20.0f);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 头像圆圈三处改
// 圆形头像 = 随机颜色 + 外层包边 + 垂直居中。
// 裁定：compose 每帧都跑，真随机会闪烁 → 「随机」= 按 profile id 稳定派生
//（哈希 → 9 色板）；包边 = 同色系加深/提亮一档的描边环；垂直居中用数值断言
// 钉住（圆心 y == 行 y + 行高/2，容差 0.5）。
REGISTER_TEST(claude_profile_avatar_color_ring_centered,
              "claude_module: Profile 头像圆圈稳定随机色 + 外层包边 + 垂直居中", {
    TestEnv env;
    for (int i = 0; i < 3; ++i) {
        auto p = env.svc->createProfile(
            "头像" + std::to_string(i),
            "https://av" + std::to_string(i) + ".example.com",
            "sk-ant-av-000" + std::to_string(i), "m");
        DOCTEST_REQUIRE(p.isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    const auto compose = [&](eui::Ui& ui) {
        composeAtSize(mod.get(), ui, "claude_avatar", 1262.0f, 801.0f);
    };
    const auto sameColor = [](const eui::Color& a, const eui::Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    eui::Ui ui1;
    compose(ui1);
    const auto* av = ui1.find("claude.profile.0.avatar");
    const auto* tx = ui1.find("claude.profile.0.avatar.text");
    const auto* row = ui1.find("claude.profile.0.wrap");
    DOCTEST_REQUIRE(av != nullptr);
    DOCTEST_REQUIRE(tx != nullptr);
    DOCTEST_REQUIRE(row != nullptr);

    // (a) 同一 profile 两次 compose 颜色一致（稳定派生，不闪烁）
    eui::Ui ui2;
    compose(ui2);
    const auto* av2 = ui2.find("claude.profile.0.avatar");
    DOCTEST_REQUIRE(av2 != nullptr);
    DOCTEST_CHECK_MESSAGE(sameColor(av->color, av2->color),
                          "同一 profile 两次 compose 头像色不一致（每帧随机闪烁）");

    // (a) 非占位底色：既不是 surfaceElevated 占位，也不是纯白
    const core::theme::Tokens& t = core::theme::current();
    const bool isPlaceholder = sameColor(av->color, t.colors.surfaceElevated);
    const bool isWhite = av->color.r >= 0.999f && av->color.g >= 0.999f &&
                         av->color.b >= 0.999f;
    DOCTEST_CHECK_MESSAGE(!isPlaceholder,
                          "头像圆圈仍是占位底色 surfaceElevated（未改随机色）");
    DOCTEST_CHECK_MESSAGE(!isWhite, "头像圆圈仍是纯白底");

    // (b) 不同 profile 颜色不同（三个样本至少两色；id 确定 → 结果确定）
    std::vector<eui::Color> fills;
    for (int i = 0; i < 3; ++i) {
        const auto* avi = ui1.find("claude.profile." + std::to_string(i) +
                                   ".avatar");
        DOCTEST_REQUIRE(avi != nullptr);
        fills.push_back(avi->color);
    }
    const bool anyDistinct =
        !sameColor(fills[0], fills[1]) || !sameColor(fills[1], fills[2]) ||
        !sameColor(fills[0], fills[2]);
    DOCTEST_CHECK_MESSAGE(anyDistinct,
                          "三个不同 profile 的头像色全相同（派生未生效）");

    // (c) 外层包边存在（描边环宽度 ≥ 1）
    DOCTEST_CHECK_MESSAGE(av->border.width >= 1.0f,
                          "头像圆圈缺外层包边（border 宽度 < 1）");
    // (c) 环色是派生的同色系色，不是回退到中性 borderDefault（只断宽度时，
    const bool ringIsBorderDefault =
        sameColor(av->border.color, t.colors.borderDefault);
    DOCTEST_CHECK_MESSAGE(!ringIsBorderDefault,
                          "头像包边色回退为中性 borderDefault（未用派生环色）");

    // (d) 垂直居中：圆心 y == 行（96 高卡片）中心 y，容差 0.5；字形同框
    const float rowCy = row->frame.y + row->frame.height * 0.5f;
    const float circleCy = av->frame.y + av->frame.height * 0.5f;
    const float textCy = tx->frame.y + tx->frame.height * 0.5f;
    DOCTEST_CHECK_MESSAGE(std::fabs(circleCy - rowCy) <= 0.5f,
                          "头像圆圈未垂直居中：圆心 " +
                              std::to_string(circleCy) + " vs 行中心 " +
                              std::to_string(rowCy));
    DOCTEST_CHECK_MESSAGE(std::fabs(textCy - circleCy) <= 0.5f,
                          "头像字形框与圆圈不同心");

    // (e) 深浅主题派生分支：两主题各 compose 一帧，
    // borderDefault；结束恢复原主题（全局态还原，不污染后续用例）。
    // g_current 是单例被 setDark/setLight 原地改写 → token 只能**值拷贝**快照，
    // 引用会跨主题别名
    const std::string themeBefore = core::theme::currentThemeName();
    const auto avatarOf = [&](eui::Ui& ui) {
        return ui.find("claude.profile.0.avatar");   // 断言由调用方做（宏含 return false，不能进非 bool helper）
    };
    core::theme::setLight();
    const core::theme::Tokens tokensLight = core::theme::current();
    eui::Ui uiLight;
    compose(uiLight);
    const auto* lightAv = avatarOf(uiLight);
    DOCTEST_REQUIRE(lightAv != nullptr);
    const eui::Color lightFill = lightAv->color;
    const eui::Color lightRing = lightAv->border.color;
    core::theme::setDark();
    const core::theme::Tokens tokensDark = core::theme::current();
    eui::Ui uiDark;
    compose(uiDark);
    const auto* darkAv = avatarOf(uiDark);
    DOCTEST_REQUIRE(darkAv != nullptr);
    DOCTEST_CHECK_MESSAGE(!sameColor(darkAv->color, lightFill),
                          "深浅主题头像底色相同（isDark 派生分支未生效）");
    DOCTEST_CHECK_MESSAGE(!sameColor(darkAv->border.color, lightRing),
                          "深浅主题包边色相同（isDark 派生分支未生效）");
    DOCTEST_CHECK_MESSAGE(!sameColor(darkAv->color,
                                     tokensDark.colors.surfaceElevated) &&
                              !sameColor(lightAv->color,
                                         tokensLight.colors.surfaceElevated),
                          "头像回退占位底色（深或浅主题）");
    DOCTEST_CHECK_MESSAGE(!sameColor(darkAv->border.color,
                                     tokensDark.colors.borderDefault) &&
                              !sameColor(lightAv->border.color,
                                         tokensLight.colors.borderDefault),
                          "包边回退中性 borderDefault（深或浅主题）");
    if (themeBefore == "Dark") {
        core::theme::setDark();
    } else {
        core::theme::setLight();
    }

    mod->onStop();
    return true;
})

// 头像首字按 UTF-8 码点取
// std::string(1, p.name[0]) 只取首**字节**：CJK 首字符是 3 字节 UTF-8，截出
// 单字节非法序列 → 渲染 ?。判据：CJK 名渲染首字 == 名首字符完整 UTF-8；
// 英文名 == 首字符。
// 非法序列回退 ?。判据：CJK 名渲染首字 == 名首字符完整 UTF-8；英文名 == 首字符。
REGISTER_TEST(claude_profile_avatar_initial_utf8,
              "claude_module: Profile 头像首字按 UTF-8 码点取（CJK 不再渲染 ?）", {
    TestEnv env;
    auto cjk = env.svc->createProfile("智谱网关", "https://z.example.com",
                                      "sk-ant-u8-001", "m");
    DOCTEST_REQUIRE(cjk.isOk());
    auto lat = env.svc->createProfile("Alpha", "https://a.example.com",
                                      "sk-ant-u8-002", "m");
    DOCTEST_REQUIRE(lat.isOk());
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_u8", 1262.0f, 801.0f);

    // 列表按 name ASC 排序（"Alpha" < "智谱网关"），不能写死下标——经 service
    // 的同一排序把名字映到行号
    auto listed = env.svc->listProfiles();
    DOCTEST_REQUIRE(listed.isOk());
    const auto indexOfName = [&](const std::string& name) -> int {
        for (size_t i = 0; i < listed.value().size(); ++i) {
            if (listed.value()[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    const int cjkIdx = indexOfName("智谱网关");
    const int latIdx = indexOfName("Alpha");
    DOCTEST_REQUIRE(cjkIdx >= 0);
    DOCTEST_REQUIRE(latIdx >= 0);
    const auto* txCjk =
        ui.find("claude.profile." + std::to_string(cjkIdx) + ".avatar.text");
    const auto* txLat =
        ui.find("claude.profile." + std::to_string(latIdx) + ".avatar.text");
    DOCTEST_REQUIRE(txCjk != nullptr);
    DOCTEST_REQUIRE(txLat != nullptr);
    // "智" = 3 字节 UTF-8（E6 99 BA）；只留首字节 0xE5 → 断言红
    DOCTEST_CHECK_MESSAGE(txCjk->text == "智",
                          "CJK 名首字未按码点取：渲染 text 长度 " +
                              std::to_string(txCjk->text.size()) + "（应为 3）");
    DOCTEST_CHECK_MESSAGE(txLat->text == "A",
                          "拉丁名首字取错：" + txLat->text);

    mod->onStop();
    return true;
})

// Profile 卡头像首字：文本垂直居中根治
// claude_profile_active_badge_centered）：文字框与圆形底同 y 同高、
// lineHeight == 圆高，缺 VerticalAlign::Center 时 runtime 把 ink 从行框顶沿
// 起排（runtime_render.h:1068 取 y=frame.y、:1069 才切到 frame 中线），圆形里看着偏上。
// 与上面两条头像用例的分工：它们钉的是 frame 数值（圆心/首字码点），**frame
// 数值对不代表墨迹居中**——本用例钉渲染锚点，是那两条的唯一补集。
// 删 .verticalAlign(Center) → 本用例 verticalAlign 断言红
//（frame 数值不变、只有渲染锚点回 frame 顶，故上面两条不会红）。
REGISTER_TEST(claude_profile_avatar_centered,
              "claude_module: Profile 头像首字垂直居中（锚点 = 圆形底中心）", {
    TestEnv env;
    auto p = env.svc->createProfile("Zed", "https://avatar.example.com",
                                    "sk-ant-avatar-001", "m-avatar");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();
    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_avatar", 1262.0f, 801.0f);

    const auto* txt = ui.find("claude.profile.0.avatar.text");
    const auto* bg = ui.find("claude.profile.0.avatar");
    DOCTEST_REQUIRE(txt != nullptr);
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_CHECK_EQ(txt->text, std::string("Z"));   // 首字
    DOCTEST_CHECK_MESSAGE(txt->verticalAlign == eui::VerticalAlign::Center,
                          "头像首字未挂 VerticalAlign::Center"
                          "（ink 从行框顶沿起排 → 圆形内偏上）");
    DOCTEST_CHECK_MESSAGE(txt->horizontalAlign == eui::HorizontalAlign::Center,
                          "头像首字未水平居中");
    DOCTEST_CHECK_MESSAGE(
        std::fabs((txt->frame.y + txt->frame.height * 0.5f) -
                  (bg->frame.y + bg->frame.height * 0.5f)) <= 0.5f,
        "头像首字 frame 中心 y=" +
            std::to_string(txt->frame.y + txt->frame.height * 0.5f) +
            " != 圆形底中心 y=" +
            std::to_string(bg->frame.y + bg->frame.height * 0.5f));
    // 圆形底尺寸不变（40×40 圆，radius 20）
    DOCTEST_CHECK_EQ(bg->frame.width, 40.0f);
    DOCTEST_CHECK_EQ(bg->frame.height, 40.0f);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 长模型键在键列会截断显示
// 估宽 8px/字符 → 100% 档 keyMaxChars=27，"ANTHROPIC_DEFAULT_HAIKU_MODEL"(31)
// / "ANTHROPIC_DEFAULT_OPUS_MODEL"(29) 渲染成「前 26 字符 + …」，渲染文本
// ≠ 全键名，按全键名 count/find 断言会被截断掩蔽（泄漏了也恒真）。判据：
// 渲染文本等于某模型键全名，或剥掉截断省略号后是某模型键的**真前缀** → 泄漏。
// ANTHROPIC_BASE_URL / ANTHROPIC_AUTH_TOKEN / MY_* 等合法可见键既不是模型键、
// 也不是其前缀，不误报。返回空串 = 未泄漏，非空 = 泄漏的渲染文本。
static std::string leakedModelEnvKey(const std::string& rendered) {
    std::vector<std::string> modelKeys;
    for (const auto& meta : module::claude::modelSlotTable()) {
        modelKeys.push_back(meta.envKey);
        if (meta.nameKey != nullptr) modelKeys.push_back(meta.nameKey);
    }
    for (const std::string& k : modelKeys) {
        if (rendered == k) return rendered;   // 未截断全名
    }
    const std::string ell = "…";  // 生产截断符（U+2026，UTF-8 3 字节）
    if (rendered.size() < ell.size() ||
        rendered.compare(rendered.size() - ell.size(), ell.size(), ell) != 0) {
        return {};
    }
    const std::string base = rendered.substr(0, rendered.size() - ell.size());
    for (const std::string& k : modelKeys) {
        if (k.size() > base.size() && k.compare(0, base.size(), base) == 0) {
            return rendered;   // 截断形态
        }
    }
    return {};
}

// 组 B P3：环境变量页签列生效 env + 每键来源
// 该页只列 profile 自身的 extra_env 时，用户读到的是「只显示 1 条」。
// 模型映射域的键不在本页展示（「模型映射」页签已列）——托管写入的 ANTHROPIC_MODEL
// 与用户手写在 extra_env 的模型键都按键名过滤（不按来源）。
// 托管写入的 ANTHROPIC_MODEL 与用户手写在 extra_env 的
// ANTHROPIC_DEFAULT_OPUS_MODEL 都按键名过滤（不按来源）。
REGISTER_TEST(claude_env_tab_lists_effective_env_with_source,
              "claude_module: 环境变量页签列生效 env（模型键除外）+ 来源标注", {
    TestEnv env;
    auto sc = env.svc->createSharedConfig("片段Z", R"({"MY_SHARED":"s"})");
    DOCTEST_REQUIRE(sc.isOk());
    auto p = env.svc->createProfile("全量env", "https://env.example.com",
                                    "sk-ant-env-000001", "m-env-model");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    auto g = env.svc->getProfile(pid);
    DOCTEST_REQUIRE(g.isOk());
    // 手写的模型键（extra_env 途径）也必须被滤 —— 证明过滤按键名而非按来源
    g.value().extraEnvJson =
        R"({"MY_PROFILE":"p","ANTHROPIC_DEFAULT_OPUS_MODEL":"m-user-opus"})";
    g.value().sharedConfigId = sc.value().id;   // 片段带 1 个键
    DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_env", 1262.0f, 801.0f);
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.2"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_env", 1262.0f, 801.0f);

    // 逐行读「键 → 来源」，按键名断言（不依赖 map 序）
    std::map<std::string, std::string> srcOf;
    for (int i = 0; i < 40; ++i) {
        const auto* k = ui2.find("claude.env.key." + std::to_string(i));
        if (k == nullptr) break;
        const auto* s = ui2.find("claude.env.src." + std::to_string(i));
        DOCTEST_REQUIRE(s != nullptr);   // 每行都必须有来源标注
        srcOf[k->text] = s->text;
    }
    // 模型映射键（槽位派生的 envKey / *_NAME）不出现在本页 —— 判据按
    // E-AUD-1 截断感知口径（全名 + 截断形态都算泄漏），不按全键名 count
    //（长键渲染成「前缀 + …」，全名 count 会被截断掩蔽成恒真）：托管写入的
    // ANTHROPIC_MODEL（legacy model → def 槽位）与手写的 OPUS 键都被滤
    for (const auto& [renderedKey, srcText] : srcOf) {
        (void)srcText;
        const std::string leak = leakedModelEnvKey(renderedKey);
        DOCTEST_CHECK_MESSAGE(leak.empty(),
                              "模型键泄漏到 env 页签: " + renderedKey);
    }
    // 非模型键照常列：托管连接键、profile 自身 extra_env、所绑片段
    DOCTEST_REQUIRE(srcOf.count("ANTHROPIC_BASE_URL") == 1);
    DOCTEST_CHECK_EQ(srcOf["ANTHROPIC_BASE_URL"], std::string("托管键"));
    DOCTEST_CHECK_EQ(srcOf["ANTHROPIC_AUTH_TOKEN"], std::string("托管键"));
    DOCTEST_REQUIRE(srcOf.count("MY_PROFILE") == 1);
    DOCTEST_CHECK_EQ(srcOf["MY_PROFILE"], std::string("本 Profile"));
        //（片段不再携带 env 键——片段键不出现在
    DOCTEST_REQUIRE(srcOf.count("MY_SHARED") == 0);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 模型键不渲染 + 行几何不重叠（130% 缩放档）
// 全局字号缩放（130%）放大字号而行盒按 100% 写死 → 墨迹越盒叠印。断言：
//   ① 行卡 y 区间两两不重叠（行距 ≥ 行卡高）
//   ② 行内「键/值」带与「来源/说明」带 y 区间不重叠
//   ③ 渲染项里没有任何模型映射键（含 legacy model 派生的 ANTHROPIC_MODEL 与
//      用户手写的槽位键；键集与实现同源自 provider_config 槽位表）
// 缩放守卫：fontScale 是进程级全局状态，RAII 还原（fail-fast 框架 REQUIRE
// 会直接 return，尾部手动还原会漏）。
REGISTER_TEST(claude_env_tab_no_overlap_and_model_keys_hidden,
              "claude_module: env 页签行几何不重叠 + 模型键不渲染（含 130% 档）", {
    TestEnv env;
    auto p = env.svc->createProfile("几何env", "https://geo.example.com",
                                    "sk-ant-geo-00001", "m-geo-model");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    auto g = env.svc->getProfile(pid);
    DOCTEST_REQUIRE(g.isOk());
    g.value().extraEnvJson =
        R"({"MY_GEO":"v","ANTHROPIC_DEFAULT_HAIKU_MODEL":"m-user-h"})";
    DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    // 判据 helper：对一帧已 layout 的 Ui 逐行断言（返回空串 = 通过；本框架
    // 断言宏展开含 return false，只能写在用例体里）
    const auto paneProblem = [&](eui::Ui& ui) -> std::string {
        const core::dsl::Element* prevBg = nullptr;
        int rows = 0;
        for (int i = 0; i < 64; ++i) {
            const std::string n = std::to_string(i);
            const auto* k = ui.find("claude.env.key." + n);
            if (k == nullptr) break;
            const auto* val = ui.find("claude.env.val." + n);
            const auto* src = ui.find("claude.env.src." + n);
            const auto* hint = ui.find("claude.env.hint." + n);
            const auto* bg = ui.find("claude.env.row.bg." + n);
            if (val == nullptr || src == nullptr || hint == nullptr ||
                bg == nullptr) {
                return "行 " + n + " 缺 val/src/hint/bg 之一";
            }
            // ① 行卡 y 区间不重叠（与上一行比；行距 ≥ 行卡高 ⇒ 全列成立）
            if (prevBg != nullptr) {
                const float prevBottom =
                    prevBg->frame.y + prevBg->frame.height;
                if (prevBottom > bg->frame.y + 0.01f) {
                    return "行 " + n + " 行卡叠印（上一行底 " +
                           std::to_string(prevBottom) + " > 本行顶 " +
                           std::to_string(bg->frame.y) + "）";
                }
            }
            // ② 行带 1（键/值）与行带 2（来源/说明）y 区间不重叠
            const float band1Bottom =
                std::max(k->frame.y + k->frame.height,
                         val->frame.y + val->frame.height);
            const float band2Top = std::min(src->frame.y, hint->frame.y);
            if (band1Bottom > band2Top + 0.01f) {
                return "行 " + n + " 带叠印（带1底 " +
                       std::to_string(band1Bottom) + " > 带2顶 " +
                       std::to_string(band2Top) + "）";
            }
            // ③ 模型映射键不得渲染（E-AUD-1 截断感知口径：长键截断形态
            // 「前缀 + …」也算泄漏，不被显示截断掩蔽）
            const std::string leak = leakedModelEnvKey(k->text);
            if (!leak.empty()) {
                return "模型键泄漏到 env 页签: " + k->text;
            }
            prevBg = bg;
            ++rows;
        }
        if (rows == 0) return "env 页签一行都没画";
        return {};
    };

    // 100% 档（进程默认）：先点开「环境变量」页签再断言
    eui::Ui ui1;
    composeAtSize(mod.get(), ui1, "claude_env", 1262.0f, 801.0f);
    DOCTEST_REQUIRE(clickById(ui1, "claude.detail.tabs.hit.2"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_env", 1262.0f, 801.0f);
    const std::string p100 = paneProblem(ui2);
    DOCTEST_CHECK_MESSAGE(p100.empty(), p100);

    // 130% 档（用户机档位，真机根因复现）：fontScale 重建 token 后重新 compose，
    // 几何断言必须原样成立（行盒随字号派生，不写死 100% 像素）
    struct FontScaleGuard {
        float prev;
        explicit FontScaleGuard(float s)
            : prev(::core::theme::fontScale()) {
            ::core::theme::setFontScale(s);
        }
        ~FontScaleGuard() { ::core::theme::setFontScale(prev); }
    } guard{1.3f};
    eui::Ui ui3;
    composeAtSize(mod.get(), ui3, "claude_env", 1262.0f, 801.0f);
    const std::string p130 = paneProblem(ui3);
    DOCTEST_CHECK_MESSAGE(p130.empty(), p130);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 组 B P4：共享配置区块钉面板底 + 「+ 新建片段」恒可点
// 区块底必须锚定：listH 若按 clamp(scrollH − sharedH − 12, min(96, scrollH), scrollH)
// 算，96px 保底一生效（≈5 个片段起）区块就被推下去 → ≥6 个按钮被底部按钮压住
// 点不到、≥8 个被整页 clip。现在区块底锚定 = 底部按钮行上沿 − 12，高度取
// min(自然高, 不侵占列表保底视口)，卡片在区块内部滚。
REGISTER_TEST(claude_shared_block_bottom_anchored,
              "claude_module: 共享配置区块钉面板底 + 新建片段恒可点（8 片段）", {
    TestEnv env;
    std::vector<int64_t> scIds;
    for (int i = 0; i < 8; ++i) {
        auto f = env.svc->createSharedConfig("片段" + std::to_string(i), "");
        DOCTEST_REQUIRE(f.isOk());
        scIds.push_back(f.value().id);
    }
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        auto p = env.svc->createProfile(
            "站" + std::to_string(i),
            "https://s" + std::to_string(i) + ".example.com",
            "sk-ant-anc-000" + std::to_string(i), "m");
        DOCTEST_REQUIRE(p.isOk());
        ids.push_back(p.value().id);
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    // 尺寸矩阵：真实 macOS 尺寸 + 默认 + 最小窗 + 高窗，逐档断言同一条不变量
    for (float h : {801.0f, 1000.0f, 720.0f, 1200.0f}) {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_anchor", 1262.0f, h);
        const auto* block = ui.find("claude.shared.block");
        const auto* newBtn = ui.find("claude.shared.new");
        const auto* bottomBtn = ui.find("claude.profiles.new");
        const auto* panel = ui.find("claude.profiles");
        DOCTEST_REQUIRE(block != nullptr);
        DOCTEST_REQUIRE(newBtn != nullptr);
        DOCTEST_REQUIRE(bottomBtn != nullptr);
        DOCTEST_REQUIRE(panel != nullptr);
        const float blockBottom = block->frame.y + block->frame.height;
        const float btnBottom = newBtn->frame.y + newBtn->frame.height;
        // 「+ 新建片段」是区块的固定尾部 → 区块底 == 按钮底
        DOCTEST_CHECK(std::fabs(btnBottom - blockBottom) <= 0.5f);
        // 贴底锚点：区块底 = 底部按钮行上沿 − 12（不随片段数变）
        DOCTEST_CHECK(std::fabs(blockBottom - (bottomBtn->frame.y - 12.0f)) <= 0.5f);
        // 面板内（不越出容器底）
        DOCTEST_CHECK(blockBottom <= panel->frame.y + panel->frame.height + 0.5f);
        // 可点：按钮的命中层（.bg，button 组件契约）在，且按钮有实际高度
        const auto* newHit = ui.find("claude.shared.new.bg");
        DOCTEST_REQUIRE(newHit != nullptr);
        DOCTEST_REQUIRE(newHit->onClick != nullptr);
        DOCTEST_CHECK(btnBottom > newBtn->frame.y);
    }

    // 装不下时卡片仍可点开编辑（块内滚，不牺牲可达性）
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_anchor", 1262.0f, 801.0f);
        DOCTEST_CHECK(ui.find("claude.shared.card.0.bg") != nullptr);
        DOCTEST_REQUIRE(clickById(ui, "claude.shared.card.0.bg"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_anchor", 1262.0f, 801.0f);
        DOCTEST_CHECK(ui.find("claude.dialog.shared") != nullptr);  // 编辑弹窗已开
        //（弹窗保持开着不影响下面的块内滚动断言：断言直读元素，不走命中测试）
    }

    // 片段集变化 → 块内滚动偏移归零
    // 判别力：旧集与新集都是 8 张卡 → 内容高/maxOffset 不变，单靠渲染侧的 clamp
    // 藏不住旧偏移；只有真的复位才会回到 0
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_anchor", 1262.0f, 801.0f);
        const auto* sc = ui.find("claude.shared.scroll");
        DOCTEST_REQUIRE(sc != nullptr);
        DOCTEST_REQUIRE(sc->onScrollOffsetChanged != nullptr);
        DOCTEST_CHECK_EQ(sc->scrollOffset, 0.0f);        // 静止
        sc->onScrollOffsetChanged(120.0f);               // 模拟用户滚到底
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_anchor", 1262.0f, 801.0f);
        const auto* sc = ui.find("claude.shared.scroll");
        DOCTEST_REQUIRE(sc != nullptr);
        DOCTEST_CHECK(sc->scrollOffset > 60.0f);         // 偏移真的生效了
        DOCTEST_CHECK(sc->scrollMaxOffset > 60.0f);      // 且确有可滚余量
    }
    // 换一整套片段（8 个新名字）：签名变 → 偏移必须归零。
    // 注意：这里绕过 UI 直改 service，模块的快照是**后台刷新 + 200ms 节流**的
    //（直改 DB 不标脏）→ 必须 onPageEnter() 标脏 + 有界轮询等快照换过来，
        // 否则断言的还是旧快照（旧签名 → 不复位），会得到假红
    for (int64_t id : scIds) {
        DOCTEST_REQUIRE(env.svc->deleteSharedConfig(id).isOk());
    }
    for (int i = 0; i < 8; ++i) {
        auto f = env.svc->createSharedConfig("新片段" + std::to_string(i), "");
        DOCTEST_REQUIRE(f.isOk());
    }
    mod->onPageEnter();
    bool swapped = false;
    for (int i = 0; i < 40 && !swapped; ++i) {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_anchor", 1262.0f, 801.0f);
        const auto* name0 = ui.find("claude.shared.card.0.name");
        swapped = name0 != nullptr && name0->text == std::string("新片段0");
        if (!swapped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        } else {
            const auto* sc = ui.find("claude.shared.scroll");
            DOCTEST_REQUIRE(sc != nullptr);
            DOCTEST_CHECK(sc->scrollMaxOffset > 60.0f);  // 仍有可滚余量（非恒真）
            DOCTEST_CHECK_EQ(sc->scrollOffset, 0.0f);    // F-09：已归零
        }
    }
    DOCTEST_CHECK(swapped);   // 快照确实换过来了（否则上面那条断言没意义）

    mod->onStop();
    for (int64_t id : ids) cleanupProfileKey(kTestKeychainService, id);
    return true;
})

// 共享配置片段列表限高 2 条 + 块内滚
// 改前区块自然高随条目数涨（固定部分 + N×(卡 56+行距 8)，≤4 条不触
// sharedBlockMaxH 上界）→ 每加一个片段左列整列重排。现在条目区恒 2 整行
//（128 = 2×64）：
//   · 区块高恒 204（76 固定 + 128 条目框架），0/1/2/3 条全同 —— 空态 / 1 条按
//     框架留白不收缩（左列 Profile 列表视口 listH 由区块高推出，恒高 → 整列
//     不跳）
//   · 恰 2 条：内容高 == 视口 → scrollMaxOffset == 0，不出滚动条（用户口径
//     「超过才滚」）
//   · ≥3 条：scrollView 存在、可视区恰 2 条、第 3 条在视口外，滚一行（64）
//     后进入视口
// 数值 204/128/64 对应 claude_ui_list.cpp 匿名命名空间的
// kSharedFixedH/kSharedCardsH（该组常量 cpp 私有；此处写字面量 = 双向漂移
// 哨兵：源常量变了此处红，逼两边同步改）
REGISTER_TEST(claude_shared_list_cap_two_cards,
              "claude_module: 共享配置片段列表限高 2 条（超出滚动，空/1 条留白）", {
    for (int count = 0; count <= 3; ++count) {
        TestEnv env;
        auto p = env.svc->createProfile("占位", "https://cap.example.com",
                                        "sk-ant-cap-0001", "");
        DOCTEST_REQUIRE(p.isOk());
        const int64_t pid = p.value().id;
        for (int i = 0; i < count; ++i) {
            auto f = env.svc->createSharedConfig("片段" + std::to_string(i), "");
            DOCTEST_REQUIRE(f.isOk());
        }
        std::unique_ptr<::core::Module> mod(create_claude_module());
        mod->onStart();
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_shared_cap", 1600.0f, 1000.0f);

        const auto* block = ui.find("claude.shared.block");
        const auto* newBtn = ui.find("claude.profiles.new");
        DOCTEST_REQUIRE(block != nullptr);
        DOCTEST_REQUIRE(newBtn != nullptr);

    // ① 区块高恒 204，与条目数无关（改回随条目数涨
        DOCTEST_CHECK(std::fabs(block->frame.height - 204.0f) <= 0.5f);
        // ② 不溢出：区块底 ≤ 底部按钮行上沿（钉底锚点）
        DOCTEST_CHECK(block->frame.y + block->frame.height <=
                      newBtn->frame.y + 0.5f);

        if (count == 0) {
            // ③ 空态：提示行画在框架顶部，无条目 → 无 scrollView（留白不收缩）
            DOCTEST_CHECK(ui.find("claude.shared.empty") != nullptr);
            DOCTEST_CHECK(ui.find("claude.shared.scroll") == nullptr);
        } else {
            const auto* sc = ui.find("claude.shared.scroll");
            DOCTEST_REQUIRE(sc != nullptr);            // scrollView 存在
            // ④ 可视区恒 2 整行高
            DOCTEST_CHECK(std::fabs(sc->frame.height - 128.0f) <= 0.5f);
            if (count <= 2) {
                // ⑤ 1/2 条：无可滚余量（不出滚动条 —— 用户口径「超过才滚」）
                DOCTEST_CHECK_EQ(sc->scrollMaxOffset, 0.0f);
            }
            // 前 2 条完整可见（offset 0）
            const auto* c0 = ui.find("claude.shared.card.0.bg");
            DOCTEST_REQUIRE(c0 != nullptr);
            DOCTEST_CHECK(c0->frame.y >= sc->frame.y - 0.5f);
            DOCTEST_CHECK(c0->frame.y + c0->frame.height <=
                          sc->frame.y + sc->frame.height + 0.5f);
            if (count >= 2) {
                const auto* c1 = ui.find("claude.shared.card.1.bg");
                DOCTEST_REQUIRE(c1 != nullptr);
                DOCTEST_CHECK(c1->frame.y >= sc->frame.y - 0.5f);
                DOCTEST_CHECK(c1->frame.y + c1->frame.height <=
                              sc->frame.y + sc->frame.height + 0.5f);
            }
            if (count >= 3) {
                // ⑥ 第 3 条在视口外（要滚才可见）+ 确有可滚余量（非恒真）
                const auto* c2 = ui.find("claude.shared.card.2.bg");
                DOCTEST_REQUIRE(c2 != nullptr);
                DOCTEST_CHECK(c2->frame.y >=
                              sc->frame.y + sc->frame.height - 0.5f);
                DOCTEST_CHECK(sc->scrollMaxOffset > 0.0f);
                // ⑦ 滚一行（64）：偏移跨帧保持，且余量 ≥ 一整行（第 3 条能被
                //    滚进视口）。注：无头布局 frame 是内容坐标、不随 offset 平移
                //    （偏移在渲染期应用），故此处断状态 + 余量；「第 3 条进入
                //    视口」的渲染侧核对由 probe --dump 落图
                DOCTEST_REQUIRE(sc->onScrollOffsetChanged != nullptr);
                sc->onScrollOffsetChanged(64.0f);
                eui::Ui ui2;
                composeAtSize(mod.get(), ui2, "claude_shared_cap2",
                              1600.0f, 1000.0f);
                const auto* sc2 = ui2.find("claude.shared.scroll");
                DOCTEST_REQUIRE(sc2 != nullptr);
                DOCTEST_CHECK(std::fabs(sc2->scrollOffset - 64.0f) <= 0.5f);
                DOCTEST_CHECK(sc2->scrollMaxOffset >= 64.0f);
            }
        }
        mod->onStop();
        cleanupProfileKey(kTestKeychainService, pid);
    }
    return true;
})


// 29. 复制配置完整形态升级
// toast 副文案同步
REGISTER_TEST(claude_export_full_settings_shape,
              "claude_module: buildProfileExportJson 完整 settings.json 形态（env+高级属性/片段赢/pretty/toast）", {
    using module::claude::kCopyConfigToastSubtitle;
    module::claude::Profile p;
    p.id = 9;
    p.name = "全量导出";
    p.baseUrl = "https://full.example.com";
    p.authField = "both";
    p.model = "m-full";
    p.apiKeyHint = "sk-ant-***9876";
    p.extraEnvJson = R"({"MY_FULL_KEY":"v1"})";
    p.extraSettingsJson = R"({"model": "json-full", "statusLine": {"type": "command"}})";

    const std::string j = buildProfileExportJson(p, "", p.apiKeyHint);
    DOCTEST_REQUIRE(!j.empty());
    auto doc = platform::json::parse(j);
    DOCTEST_REQUIRE(doc.isOk());
    auto root = doc.value().asObject();
    // 顶层 = env + JSON 属性（完整 settings.json 形态）
    DOCTEST_CHECK(root.get("env").isObject());
    DOCTEST_CHECK_EQ(root.get("model").asString(), std::string("json-full"));
    DOCTEST_REQUIRE(root.get("statusLine").isObject());
    DOCTEST_CHECK_EQ(root.get("statusLine").asObject().get("type").asString(),
                     std::string("command"));
    const auto env = root.get("env").asObject();
    DOCTEST_CHECK_EQ(env.get("ANTHROPIC_AUTH_TOKEN").asString(),
                     std::string("sk-ant-***9876"));   // 占位非明文
    DOCTEST_CHECK_EQ(env.get("MY_FULL_KEY").asString(), std::string("v1"));
    // pretty 文本（可直接粘贴进 settings.json 的形态，含两空格缩进）
    DOCTEST_CHECK(j.find("\n  \"") != std::string::npos);

    // 片段叠加端到端：片段与 profile 各含独有键 + 同键 → 片段赢
    {
        const std::string jf = buildProfileExportJson(
            p, R"({"statusLine": "frag-s", "hooks": {"PreToolUse": []}})",
            p.apiKeyHint);
        auto docF = platform::json::parse(jf);
        DOCTEST_REQUIRE(docF.isOk());
        auto rootF = docF.value().asObject();
        DOCTEST_CHECK_EQ(rootF.get("statusLine").asString(), std::string("frag-s"));
        DOCTEST_CHECK(rootF.get("hooks").isObject());
        DOCTEST_CHECK_EQ(rootF.get("model").asString(), std::string("json-full"));
    }
    // 坏片段 JSON → 按 profile 自身 JSON 导出（读侧容错，不静默丢）
    {
        const std::string jb = buildProfileExportJson(p, "{broken", p.apiKeyHint);
        auto docB = platform::json::parse(jb);
        DOCTEST_REQUIRE(docB.isOk());
        DOCTEST_CHECK_EQ(docB.value().asObject().get("statusLine")
                             .asObject().get("type").asString(),
                         std::string("command"));
    }
    // toast 副文案同步：完整形态措辞 + 占位声明
    DOCTEST_CHECK(std::string(kCopyConfigToastSubtitle)
                      .find("完整 settings.json") != std::string::npos);
    DOCTEST_CHECK(std::string(kCopyConfigToastSubtitle)
                      .find("env + 高级属性") != std::string::npos);
    DOCTEST_CHECK(std::string(kCopyConfigToastSubtitle)
                      .find("不含明文") != std::string::npos);
    return true;
})

// 高级配置 pane 键来源标注 + contentBottom 真消费护栏
REGISTER_TEST(claude_detail_advanced_key_sources,
              "claude_module: 详情高级配置 pane 键来源（片段/本 Profile/model 说明）+ 不越内容框", {
    TestEnv env;
    // 片段与 profile JSON 各含独有键 + 同键（片段赢）
    auto f = env.svc->createSharedConfig("片段K", R"({"statusLine": "frag-s", "hooks": {"a": 1}})");
    DOCTEST_REQUIRE(f.isOk());
    auto a = env.svc->createProfile("键来源", "https://ks.example.com",
                                    "sk-ant-ksrc-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    {
        module::claude::Profile p = a.value();
        p.extraSettingsJson =
            R"({"statusLine": "profile-s", "permissions": {"p": 1}})";
        p.sharedConfigId = f.value().id;
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }
    // 坏 JSON profile：解析失败提示态必须可达——compose 经 warningsOut
    // 感知「配置存在但被合并按空」，pane 显示失败行而非「未配置」）
    auto bad = env.svc->createProfile("坏JSON源", "https://ksb.example.com",
                                      "sk-ant-ksrc-0002", "");
    DOCTEST_REQUIRE(bad.isOk());
    {
        module::claude::Profile p = bad.value();
        p.extraSettingsJson = "{broken json";
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }
    // model 说明仅对**非空字符串** model 显示（抑制只在该形态真
    // settings.json，说明不适用
    auto emptyModel = env.svc->createProfile("空串model源",
                                             "https://kse.example.com",
                                             "sk-ant-ksrc-0003", "");
    DOCTEST_REQUIRE(emptyModel.isOk());
    {
        module::claude::Profile p = emptyModel.value();
        p.extraSettingsJson = R"({"model":""})";
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }
    auto nonStrModel = env.svc->createProfile("非串model源",
                                              "https://ksn.example.com",
                                              "sk-ant-ksrc-0004", "");
    DOCTEST_REQUIRE(nonStrModel.isOk());
    {
        module::claude::Profile p = nonStrModel.value();
        p.extraSettingsJson = R"({"model":123})";
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_advjson_pane");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);   // frame 断言必须真 layout
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    // 等缓存快照带上 JSON 与绑定（绑定卡片角标 = 缓存信号；列表含两个 profile，
    // 按名定位「键来源」的卡片序——不依赖 collation 排序）
    bool sawBadge = false;
    int boundIdx = -1;
    for (int i = 0; i < 250 && !sawBadge; ++i) {
        eui::Ui ui;
        compose(ui);
        for (int k = 0; k < 8; ++k) {
            const auto* name = ui.find("claude.profile." + std::to_string(k) +
                                       ".name");
            if (name != nullptr && name->text == "键来源" &&
                ui.find("claude.profile." + std::to_string(k) +
                        ".shared.badge") != nullptr) {
                sawBadge = true;
                boundIdx = k;
                break;
            }
        }
        if (!sawBadge) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(sawBadge);
    DOCTEST_REQUIRE(boundIdx >= 0);
    // 选中卡片（详情 shown 优先用户选中——不选中时 shown 为 null，键清单不渲染）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile." + std::to_string(boundIdx) +
                                ".bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        // 生效 JSON 三键：statusLine（片段赢 → 片段）/ hooks（片段）/
        // permissions（本 Profile）
        const auto* k0 = ui.find("claude.advjson.row.key.0");
        const auto* s0 = ui.find("claude.advjson.row.src.0");
        DOCTEST_REQUIRE(k0 != nullptr);
        DOCTEST_REQUIRE(s0 != nullptr);
        DOCTEST_CHECK_EQ(k0->text, std::string("hooks"));      // 字典序
        DOCTEST_CHECK_EQ(s0->text, std::string("片段"));
        const auto* k2 = ui.find("claude.advjson.row.key.2");
        const auto* s2 = ui.find("claude.advjson.row.src.2");
        DOCTEST_REQUIRE(k2 != nullptr);
        DOCTEST_REQUIRE(s2 != nullptr);
        DOCTEST_CHECK_EQ(k2->text, std::string("statusLine"));
        DOCTEST_CHECK_EQ(s2->text, std::string("片段"));
        const auto* k1 = ui.find("claude.advjson.row.key.1");
        const auto* s1 = ui.find("claude.advjson.row.src.1");
        DOCTEST_REQUIRE(k1 != nullptr);
        DOCTEST_REQUIRE(s1 != nullptr);
        DOCTEST_CHECK_EQ(k1->text, std::string("permissions"));
        DOCTEST_CHECK_EQ(s1->text, std::string("本 Profile"));
        // model 覆盖说明未在场（JSON 无 model 键）
        DOCTEST_CHECK(ui.find("claude.advjson.note") == nullptr);
        // contentBottom 真消费：行底 ≤ 内容框底（裁决⑤护栏的正常档形态）
        const auto* box = ui.find("claude.detail.contentbox");
        DOCTEST_REQUIRE(box != nullptr);
        const float boxBottom = box->frame.y + box->frame.height;
        for (int i = 0; i < 3; ++i) {
            const auto* row = ui.find("claude.advjson.row.bg." + std::to_string(i));
            DOCTEST_REQUIRE(row != nullptr);
            DOCTEST_CHECK(row->frame.y + row->frame.height <= boxBottom + 0.5f);
        }
    }

    // 切到坏 JSON profile → 解析失败提示态（单行）
    {
        eui::Ui ui;
        compose(ui);
        bool clicked = false;
        for (int i = 0; i < 8 && !clicked; ++i) {
            const auto* name = ui.find("claude.profile." + std::to_string(i) +
                                       ".name");
            if (name != nullptr && name->text == "坏JSON源") {
                clicked = click(ui, "claude.profile." + std::to_string(i) +
                                        ".bg");
            }
        }
        DOCTEST_CHECK(clicked);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* status = ui.find("claude.advjson.status");
        DOCTEST_REQUIRE(status != nullptr);
        DOCTEST_CHECK(status->text.find("JSON 解析失败") != std::string::npos);
        DOCTEST_CHECK(ui.find("claude.advjson.label") == nullptr);
    }

    // 空串 / 非字符串 model：key 行在场但「ANTHROPIC_MODEL 不
    for (const std::string target : {"空串model源", "非串model源"}) {
        {
            eui::Ui ui;
            compose(ui);
            bool clicked = false;
            for (int i = 0; i < 8 && !clicked; ++i) {
                const auto* name = ui.find("claude.profile." +
                                           std::to_string(i) + ".name");
                if (name != nullptr && name->text == target) {
                    clicked = click(ui, "claude.profile." + std::to_string(i) +
                                            ".bg");
                }
            }
            DOCTEST_CHECK(clicked);
        }
        {
            eui::Ui ui;
            compose(ui);
            DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
        }
        {
            eui::Ui ui;
            compose(ui);
            // model 行在场（证明 pane 渲染到该 profile 且键被解析出）
            const auto* k0 = ui.find("claude.advjson.row.key.0");
            DOCTEST_REQUIRE(k0 != nullptr);
            DOCTEST_CHECK_EQ(k0->text, std::string("model"));
        // 说明不显示
            DOCTEST_CHECK(ui.find("claude.advjson.note") == nullptr);
        }
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    cleanupProfileKey(kTestKeychainService, bad.value().id);
    cleanupProfileKey(kTestKeychainService, emptyModel.value().id);
    cleanupProfileKey(kTestKeychainService, nonStrModel.value().id);
    return true;
})

REGISTER_TEST(claude_detail_advanced_key_overflow_within_box,
              "claude_module: 详情高级配置 pane 键清单溢出——行数按 contentBottom 推、不越内容框（变异⑥护栏）", {
    TestEnv env;
    auto a = env.svc->createProfile("多键", "https://ov.example.com",
                                    "sk-ant-ovr-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    {
        // 12 个顶层键（> 暂定上限 8）——短窗档下只画放得下的行 + 「另有 N 项」
        module::claude::Profile p = a.value();
        std::string json = "{";
        for (int i = 0; i < 12; ++i) {
            if (i) json += ",";
            json += "\"k" + std::to_string(i) + "\":\"v\"";
        }
        json += "}";
        p.extraSettingsJson = json;
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    // 最小窗档（1200×720）：详情内容框最矮——溢出行数预算的判别力来源
    screen.width = 1200.0f;
    screen.height = 720.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_advjson_overflow");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);   // frame 断言必须真 layout
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    // 先选中卡片（详情 shown 优先用户选中；键清单仅在 shown 非空时渲染）
    bool sawCard = false;
    for (int i = 0; i < 250 && !sawCard; ++i) {
        eui::Ui ui;
        compose(ui);
        sawCard = click(ui, "claude.profile.0.bg");
        if (!sawCard) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(sawCard);
    // 切到详情「高级配置」页签（键清单只在 tab 3 渲染）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
    }
    bool sawKeys = false;
    for (int i = 0; i < 250 && !sawKeys; ++i) {
        eui::Ui ui;
        compose(ui);
        sawKeys = ui.find("claude.advjson.label") != nullptr;
        if (!sawKeys) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(sawKeys);
    {
        eui::Ui ui;
        compose(ui);
        const auto* box = ui.find("claude.detail.contentbox");
        DOCTEST_REQUIRE(box != nullptr);
        const float boxBottom = box->frame.y + box->frame.height;
        // 行数 < 12（预算生效）；画出的行 + 溢出提示全部 ≤ 内容框底。
        //（逐行 frame 断言别删——绿态应过：行数预算 while 收缩保证 rows+tail ≤
        int drawn = 0;
        for (int i = 0; i < 12; ++i) {
            const auto* row =
                ui.find("claude.advjson.row.bg." + std::to_string(i));
            if (row == nullptr) break;
            ++drawn;
            DOCTEST_CHECK(row->frame.y + row->frame.height <= boxBottom + 0.5f);
        }
        DOCTEST_CHECK(drawn < 12);
        DOCTEST_CHECK(drawn > 0);
        const auto* ovf = ui.find("claude.advjson.overflow");
        DOCTEST_REQUIRE(ovf != nullptr);
        DOCTEST_CHECK(ovf->text.find("另有") != std::string::npos);
        // 溢出提示行本身也不越内容框
        DOCTEST_CHECK(ovf->frame.y <= boxBottom);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 高级配置 pane 字号/行盒随生效字号派生（85% 档钉值）
// 全局字号 85% 档下字号缩小但行盒不动 → 盒内大片空白 + 行距相对过宽，内容易越
// 内容框与下一块叠印。修法同 env pane：字号 = 基准×fs，盒/纵向间距 = round(基准×fs)。
// 期望值**手算写死**，不复刻实现表达式：
//   100% 档：14 / 18 / 28 / 10 / 28 / 34 / 18 / 18，字号 11 / 12（字面常量逐值）
//   85%  档：12 / 15 / 24 /  9 / 24 / 29 / 15 / 15，字号 9.35 / 10.2
//            （round(14×0.85)=12、round(18×0.85)=15、round(28×0.85)=24、
//              round(10×0.85)=round(8.5)=9、行距 24+round(6×0.85)=24+5=29、
//              round(18×0.85)=15；字号不取整）
//            （round(14×0.85)=12、round(18×0.85)=15、round(28×0.85)=24、
//              round(10×0.85)=round(8.5)=9、行距 24+round(6×0.85)=24+5=29、
//              round(18×0.85)=15；字号不取整）
REGISTER_TEST(claude_advjson_pane_geometry_follows_font_scale,
              "claude_module: 高级配置 pane 字号行盒随生效字号派生（100% 逐值 + 85% 钉值）", {
    TestEnv env;
    auto p = env.svc->createProfile("派生档", "https://fs.example.com",
                                    "sk-ant-fsc-0001", "m-fs");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    {
        // 12 个顶层键（> 暂定上限 8 → 必走「另有 N 项」溢出行）+ 非空字符串
        // model 键（→ 说明行 note 在场）
        module::claude::Profile pr = p.value();
        std::string json = R"({"model":"m-user")";
        for (int i = 0; i < 12; ++i) {
            json += ",\"k" + std::to_string(i) + "\":\"v\"";
        }
        json += "}";
        pr.extraSettingsJson = json;
        DOCTEST_REQUIRE(env.svc->updateProfile(pr).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1262.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_advjson_scale");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);   // frame 断言必须真 layout
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    // 缩放守卫：fontScale 是进程级全局状态，RAII 还原
    struct FontScaleGuard {
        float prev;
        explicit FontScaleGuard(float s)
            : prev(::core::theme::fontScale()) {
            ::core::theme::setFontScale(s);
        }
        ~FontScaleGuard() { ::core::theme::setFontScale(prev); }
    };
    // 选中卡片 + 切到「高级配置」页签（等缓存就绪；键清单仅在 shown 非空时渲染）
    bool sawCard = false;
    for (int i = 0; i < 250 && !sawCard; ++i) {
        eui::Ui ui;
        compose(ui);
        sawCard = click(ui, "claude.profile.0.bg");
        if (!sawCard) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(sawCard);
    bool sawTab = false;
    bool sawPane = false;
    for (int i = 0; i < 250 && !sawPane; ++i) {
        eui::Ui ui;
        compose(ui);
        if (!sawTab) {
            sawTab = click(ui, "claude.detail.tabs.hit.3");
        }
        sawPane = ui.find("claude.field.dialect.label") != nullptr;
        if (!sawPane) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_CHECK(sawTab);
    DOCTEST_CHECK(sawPane);

    // 判据 helper：逐项核对本档期望派生值（返回空串 = 通过）。期望值由调用方
    // 按档位**手算传入**（不复刻实现里的 rfs 表达式）
    struct Geom {
        float labelH, labelGap, fieldH, fieldGap;
        float keyRowH, keyPitch, noteH, overflowH;
        float labelFs, keyFs, valueFs;
    };
    const auto geomProblem = [&](eui::Ui& ui, const Geom& g,
                                 int* drawnOut) -> std::string {
        const auto* dL = ui.find("claude.field.dialect.label");
        const auto* dB = ui.find("claude.field.dialect.bg");
        const auto* dV = ui.find("claude.field.dialect.value");
        const auto* fL = ui.find("claude.field.fallback.label");
        const auto* fB = ui.find("claude.field.fallback.bg");
        const auto* aL = ui.find("claude.advjson.label");
        if (dL == nullptr || dB == nullptr || dV == nullptr || fL == nullptr ||
            fB == nullptr || aL == nullptr) {
            return "高级配置 pane 固定行元素缺失";
        }
        const auto near = [](float a, float b) {
            return std::fabs(a - b) <= 0.01f;
        };
        // 标签行盒（行高 = 盒高，单行不折行）
        if (!near(dL->lineHeight, g.labelH) || !near(dL->frame.height, g.labelH)) {
            return "方言标签行盒 " + std::to_string(dL->frame.height) +
                   " != " + std::to_string(g.labelH);
        }
        if (!near(fL->frame.height, g.labelH) || !near(aL->frame.height, g.labelH)) {
            return "备用/高级属性标签行盒 != " + std::to_string(g.labelH);
        }
        // 标签底 → 值盒顶
        if (!near(dB->frame.y - dL->frame.y, g.labelGap)) {
            return "方言标签底→盒顶 " + std::to_string(dB->frame.y - dL->frame.y) +
                   " != " + std::to_string(g.labelGap);
        }
        // 值盒高（两行同款）
        if (!near(dB->frame.height, g.fieldH) ||
            !near(fB->frame.height, g.fieldH) ||
            !near(dV->frame.height, g.fieldH)) {
            return "方言/备用值盒高 != " + std::to_string(g.fieldH);
        }
        // 值盒底 → 下一标签顶
        if (!near(fL->frame.y - dB->frame.y, g.fieldH + g.fieldGap)) {
            return "方言盒底→备用标签顶 " +
                   std::to_string(fL->frame.y - dB->frame.y) + " != " +
                   std::to_string(g.fieldH + g.fieldGap);
        }
        // 标签字号（小字 11×fs，值走 token 13×fs）
        if (!near(dL->fontSize, g.labelFs) || !near(fL->fontSize, g.labelFs) ||
            !near(aL->fontSize, g.labelFs)) {
            return "标签字号 != " + std::to_string(g.labelFs);
        }
        if (!near(dV->fontSize, g.valueFs)) {
            return "方言值字号 != " + std::to_string(g.valueFs);
        }
        // 键行卡：卡高 / 行距 / 字号 / 行高
        int drawn = 0;
        const ::core::dsl::Element* prevBg = nullptr;
        for (int i = 0; i < 12; ++i) {
            const std::string n = std::to_string(i);
            const auto* bg = ui.find("claude.advjson.row.bg." + n);
            if (bg == nullptr) break;
            const auto* k = ui.find("claude.advjson.row.key." + n);
            const auto* s = ui.find("claude.advjson.row.src." + n);
            if (k == nullptr || s == nullptr) return "行 " + n + " 缺 key/src";
            if (!near(bg->frame.height, g.keyRowH) ||
                !near(k->frame.height, g.keyRowH) ||
                !near(s->lineHeight, g.keyRowH)) {
                return "键行卡高/行高 != " + std::to_string(g.keyRowH);
            }
            if (!near(k->fontSize, g.keyFs)) {
                return "键名字号 " + std::to_string(k->fontSize) + " != " +
                       std::to_string(g.keyFs);
            }
            if (!near(s->fontSize, g.labelFs)) {
                return "来源字号 != " + std::to_string(g.labelFs);
            }
            if (prevBg != nullptr &&
                !near(bg->frame.y - prevBg->frame.y, g.keyPitch)) {
                return "键行距 " + std::to_string(bg->frame.y - prevBg->frame.y) +
                       " != " + std::to_string(g.keyPitch);
            }
            prevBg = bg;
            ++drawn;
        }
        if (drawn < 2) return "键行画得太少（行距断言无从判别）: " +
                            std::to_string(drawn);
        *drawnOut = drawn;
        // 溢出行 + 说明行（12 键 > 上限 8，note 由 model 键触发，两者必在场）
        const auto* ovf = ui.find("claude.advjson.overflow");
        const auto* note = ui.find("claude.advjson.note");
        if (ovf == nullptr || note == nullptr) return "溢出行/说明行缺场";
        if (!near(ovf->frame.height, g.overflowH) ||
            !near(ovf->lineHeight, g.overflowH)) {
            return "溢出行高 != " + std::to_string(g.overflowH);
        }
        if (!near(note->frame.height, g.noteH) ||
            !near(note->lineHeight, g.noteH)) {
            return "说明行高 != " + std::to_string(g.noteH);
        }
        // 溢出/说明两行紧跟键清单（行数预算自然跟随派生行距）
        const auto* box = ui.find("claude.detail.contentbox");
        if (box == nullptr) return "内容框缺场";
        const float boxBottom = box->frame.y + box->frame.height;
        if (ovf->frame.y < prevBg->frame.y + prevBg->frame.height - 0.01f) {
            return "溢出行压住最后一行键卡";
        }
        if (note->frame.y < ovf->frame.y + ovf->frame.height - 0.01f) {
            return "说明行压住溢出行";
        }
        if (note->frame.y + note->frame.height > boxBottom + 0.5f) {
            return "说明行越内容框底";
        }
        return {};
    };

    int drawn100 = 0;
    int drawn85 = 0;
    // 100% 档：全部与字面常量逐值相等（零回归锚）
    {
        eui::Ui ui;
        compose(ui);
        const std::string msg = geomProblem(ui, Geom{
            /*labelH*/14.0f, /*labelGap*/18.0f, /*fieldH*/28.0f, /*fieldGap*/10.0f,
            /*keyRowH*/28.0f, /*keyPitch*/34.0f, /*noteH*/18.0f, /*overflowH*/18.0f,
            /*labelFs*/11.0f, /*keyFs*/12.0f, /*valueFs*/13.0f}, &drawn100);
        DOCTEST_CHECK_MESSAGE(msg.empty(), msg);
    }
    // 85% 档（kFontPresets[0]，本次修的真机风险档）：钉值见上注释
    {
        FontScaleGuard guard{0.85f};
        eui::Ui ui;
        compose(ui);
        const std::string msg = geomProblem(ui, Geom{
            /*labelH*/12.0f, /*labelGap*/15.0f, /*fieldH*/24.0f, /*fieldGap*/9.0f,
            /*keyRowH*/24.0f, /*keyPitch*/29.0f, /*noteH*/15.0f, /*overflowH*/15.0f,
            /*labelFs*/9.35f, /*keyFs*/10.2f, /*valueFs*/11.05f}, &drawn85);
        DOCTEST_CHECK_MESSAGE(msg.empty(), msg);
    }
    // 行数预算随派生行距自然跟随：85% 档行距 29 < 100% 档 34，同一可用高下画得
    // 出的行数**只多不少**（不多 = 预算没把行白扔；不少 = 派生行距真被消费）。
    // 12 键 > 暂定上限 8，两档都撞 8 行上限时该判据退化为 8 ≥ 8（仍成立）——
    // 短窗档（行数由 availH 而非上限决定）才有判别力。
    DOCTEST_CHECK(drawn85 >= drawn100);
    {
        // 短窗档：行数预算由 contentBottom 推（不撞 8 行上限），85% 档（行距
        // 29）应比 100% 档（行距 34）**多画行**（实测 4 → 6）—— 派生行距真被
        // 行数预算消费的判别力：行距若被写死回 34，本判据变红
        auto drawnAt = [&](float h, int* out) -> bool {
            eui::Screen s2;
            s2.width = 1262.0f;
            s2.height = h;
            eui::Ui ui;
            ui.begin("claude_advjson_scale_short");
            mod->compose(ui, s2);
            ui.end();
            ui.layout(s2.width, s2.height);
            *out = 0;
            for (int i = 0; i < 12; ++i) {
                if (ui.find("claude.advjson.row.bg." + std::to_string(i)) ==
                    nullptr) {
                    break;
                }
                ++*out;
            }
            return true;
        };
        int short100 = 0;
        int short85 = 0;
        DOCTEST_REQUIRE(drawnAt(720.0f, &short100));
        DOCTEST_CHECK(short100 > 0);
        {
            FontScaleGuard guard{0.85f};
            DOCTEST_REQUIRE(drawnAt(720.0f, &short85));
        }
        DOCTEST_CHECK(short85 > short100);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 片段弹窗 JSON 化：JSON 保存/重开回读 +
// env 键拒绝 + 格式化
REGISTER_TEST(claude_shared_dialog_json_roundtrip,
              "claude_module: 片段弹窗 JSON 保存落库 pretty + 重开回读 + env 键拒绝（headless）", {
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_json");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) return false;
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) return false;
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    // 新建片段：compact JSON → 创建 → DB 落库为 pretty
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.name.hit", "JSON片段"));
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.json.hit",
                           R"({"model":"frag-m","permissions":{"p":1}})"));
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.confirm.bg"));
    }
    // 保存是异步刷新（H2）：写库同步、快照收割异步——轮询必须等**渲染快照**
        //（DB 第 1 轮即真 = 等的是写库而非快照，Windows CI 快照竞态 flaky
    bool saved = false;
    bool cardReady = false;   // 快照收割完成：片段卡已进渲染树（下一步 click 依赖）
    for (int i = 0; i < 300 && !(saved && cardReady); ++i) {
        mod->onPageEnter();
        eui::Ui ui;
        compose(ui);
        if (!cardReady) {
            cardReady = ui.find("claude.shared.card.0.bg") != nullptr;
        }
        auto list = env.svc->listSharedConfigs();
        if (list.isOk()) {
            for (const auto& c : list.value()) {
                if (c.name == "JSON片段") {
                    saved = c.extraSettingsJson.find("\n  \"") != std::string::npos;
                }
            }
        }
        if (!(saved && cardReady)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(saved);
    DOCTEST_CHECK(cardReady);   // 快照收割完成（片段卡进渲染树，可点）

    // 重开编辑回读：JSON 框预填 DB 原文（= 上一步落库的 pretty 文本，
    // 逐字节相等才算「保存 → 重开回读」这条链真的走通）
    {
        eui::Ui ui;
        compose(ui);
        // 卡片 0 即「JSON片段」（列表按 name 排序，单片段场景）
        DOCTEST_CHECK(click(ui, "claude.shared.card.0.bg"));
    }
    {
        using InputState = components::input_detail::InputModel::InputState;
        eui::Ui ui;
        compose(ui);
        // 编辑弹窗真的开着（backdrop 未 disabled = open_ 为真；isEdit 分支
        // 才画的删除键同时在，说明走的是 openSharedEdit 而非 openSharedCreate）
        const auto* backdrop = ui.find("claude.dialog.shared.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(!backdrop->disabled);
        DOCTEST_CHECK(ui.find("claude.dialog.shared.delete.bg") != nullptr);
        // 回读：编辑框文本 == DB 落库原文
        std::string dbJson;
        auto list = env.svc->listSharedConfigs();
        DOCTEST_REQUIRE(list.isOk());
        for (const auto& c : list.value()) {
            if (c.name == "JSON片段") dbJson = c.extraSettingsJson;
        }
        DOCTEST_REQUIRE(!dbJson.empty());
        DOCTEST_CHECK_EQ(ui.state<InputState>("claude.dialog.shared.json").text,
                         dbJson);
    }
    // 改名保存 → 更新生效（openSharedEdit 预填 + submitSharedForm update 分支）
    {
        using InputState = components::input_detail::InputModel::InputState;
        eui::Ui ui;
        compose(ui);
        const auto* nameHit = ui.find("claude.dialog.shared.name.hit");
        DOCTEST_REQUIRE(nameHit != nullptr);
        // 全选后重打（预填文本非空，onTextInput 只在光标处插入——不选区就会
        // 把新名插到旧名中间，测不到「改名」）
        auto& st = ui.state<InputState>("claude.dialog.shared.name");
        st.cursor = 0;
        st.selectionStart = 0;
        st.selectionEnd = static_cast<int>(st.text.size());
        core::TextInputEvent ev;
        ev.text = "JSON片段改名";
        nameHit->onTextInput(ev);
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.confirm.bg"));
    }
    {
        bool renamed = false;
        for (int i = 0; i < 300 && !renamed; ++i) {
            mod->onPageEnter();
            eui::Ui ui;
            compose(ui);
            auto list = env.svc->listSharedConfigs();
            if (list.isOk()) {
                for (const auto& c : list.value()) {
                    if (c.name == "JSON片段改名") {
                        renamed = true;
                        // 改名不该动 JSON（update 分支带的是预填原文）
                        DOCTEST_CHECK(c.extraSettingsJson.find("frag-m") !=
                                      std::string::npos);
                    }
                }
            }
            if (!renamed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        DOCTEST_CHECK(renamed);
    }
    {
        eui::Ui ui;
        compose(ui);
        // 弹窗保存后已关（backdrop disabled = open_ 已清；update 失败会留在
        // 开着 + error_ 红字）
        const auto* backdrop = ui.find("claude.dialog.shared.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(backdrop->disabled);
        DOCTEST_CHECK(ui.find("claude.dialog.shared.error") == nullptr);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.shared.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.name.hit", "坏片段"));
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.json.hit",
                           R"({"env":{"A":"1"}})"));
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.confirm.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* err = ui.find("claude.dialog.shared.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("不能包含 env 键") != std::string::npos);
        // 弹窗不关、不落库
        auto list = env.svc->listSharedConfigs();
        DOCTEST_REQUIRE(list.isOk());
        for (const auto& c : list.value()) {
            DOCTEST_CHECK(c.name != "坏片段");
        }
    }

    mod->onStop();
    return true;
})

// N. onStart/onStop 重入幂等（活门控 Task 4）：双 onStart / 双 onStop 不崩

REGISTER_TEST(claude_module_onstart_reentry_idempotent,
              "claude_module: onStart→onStop→onStart 重入 + 双 onStart/双 onStop no-op", {
    TestEnv env;
    DOCTEST_REQUIRE(module::claude::ClaudeService::global() != nullptr);
    std::unique_ptr<::core::Module> mod(create_claude_module());

    // 观察面（onStart 无注册表副作用，取服务状态一致性最低契约）：
    // 硬性需求 1 —— onStart 不自动起代理（main 按设置门控 / 用户手动启动）
    mod->onStart();
    DOCTEST_CHECK(!module::claude::ClaudeService::global()->isProxyRunning());
    mod->onStart();   // 双 onStart：no-op（不重跑同步首刷 + 孤儿 Keychain 清理）
    DOCTEST_CHECK(!module::claude::ClaudeService::global()->isProxyRunning());
    mod->onStop();
    DOCTEST_CHECK(!module::claude::ClaudeService::global()->isProxyRunning());
    mod->onStop();    // 双 onStop：no-op（代理未运行不重复 stop）
    mod->onStart();   // off→on 循环
    DOCTEST_CHECK(!module::claude::ClaudeService::global()->isProxyRunning());
    mod->onStop();
    DOCTEST_CHECK(!module::claude::ClaudeService::global()->isProxyRunning());
    return true;
})

// O. 模型映射「1M」开关（详情页）
// 语义照 cc-switch useModelState.ts：勾态 = **槽位支持位 && 串尾（大小写不敏感）
// 有 [1M]**（双重条件，不是只看串）；不支持槽位 / 空 base 的行开关置灰且点了
// 不落库。落库仍存「基础名 + 可选 [1M]」一个字符串（零 schema 迁移）。
REGISTER_TEST(claude_ui_model_one_m_switch,
              "claude_module: 模型映射 1M 开关（回显/落库/禁用行不生效）", {
    TestEnv env;
    auto a = env.svc->createProfile("1M开关", "https://onem.example.com",
                                    "sk-ant-onem-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    // 造数（直接写库，绕开 UI）：覆盖四种状态
    //   def     支持 + 无标记        → 开关可用、未勾
    //   sonnet  支持 + 串尾 [1m]     → **回显勾上**（大小写不敏感）
    //   opus    支持 + 无标记
    //   haiku   不支持（唯一）+ [1M] → 禁用 + 不勾（双重条件第二项不成立）
    //   fable   支持 + 空 base       → 禁用（空 base 不许勾）
    //   subagent 支持 + 空 base      → 禁用（空 base 不许勾）
    module::claude::ModelsConfig mc;
    mc.def.model = "m-def";
    mc.sonnet.model = "m-sonnet[1m]";
    mc.opus.model = "m-opus";
    mc.haiku.model = "m-haiku[1M]";
    auto seeded = module::claude::serializeModelsConfig(mc);
    DOCTEST_REQUIRE(seeded.isOk());
    auto cur = env.svc->getProfile(pid);
    DOCTEST_REQUIRE(cur.isOk());
    cur.value().modelsConfigJson = seeded.value();
    DOCTEST_REQUIRE(env.svc->updateProfile(cur.value()).isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_onem");
        mod->compose(ui, screen);
        ui.end();
        // 几何断言要 frame 有效：headless 的 ui.end() 不跑 layout，需显式补
        //（claude_module_actions_row_alignment 同款）
        ui.layout(screen.width, screen.height);
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };

    // 选中该 profile（详情面板 shown 取选中，其次激活）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    // 切到「模型映射」页签，核对每行开关存在 + 勾态回显
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.1"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto& table = module::claude::modelSlotTable();
        for (size_t i = 0; i < table.size(); ++i) {
            DOCTEST_CHECK(ui.find("claude.slot.onem." + std::to_string(i) +
                                  ".hit") != nullptr);
        }
        const auto* trackDef = ui.find("claude.slot.onem.0.track");
        const auto* knobDef = ui.find("claude.slot.onem.0.knob");
        const auto* trackSonnet = ui.find("claude.slot.onem.1.track");
        const auto* knobSonnet = ui.find("claude.slot.onem.1.knob");
        const auto* trackHaiku = ui.find("claude.slot.onem.3.track");
        const auto* knobHaiku = ui.find("claude.slot.onem.3.knob");
        DOCTEST_REQUIRE(trackDef != nullptr);
        DOCTEST_REQUIRE(knobDef != nullptr);
        DOCTEST_REQUIRE(trackSonnet != nullptr);
        DOCTEST_REQUIRE(knobSonnet != nullptr);
        DOCTEST_REQUIRE(trackHaiku != nullptr);
        DOCTEST_REQUIRE(knobHaiku != nullptr);
        // 勾态判据 = 滑块相对轨道的偏移（勾上滑块移到轨道右端）；未勾也有
        // margin 偏移（= max(spacing.micro, 轨道高×0.125)）→ 用**相对比较**
        // （勾上行偏移 − 未勾行偏移 ≈ 轨道可走行程 ≥ 5px），不写绝对阈值
        const float defOff = knobDef->frame.x - trackDef->frame.x;
        const float sonOff = knobSonnet->frame.x - trackSonnet->frame.x;
        const float haiOff = knobHaiku->frame.x - trackHaiku->frame.x;
        DOCTEST_CHECK(sonOff > defOff + 5.0f);
        // haiku：串尾确有 [1M] 但槽位不支持 → 双重条件 → 不勾（与未勾行同态）
        DOCTEST_CHECK(std::fabs(haiOff - defOff) < 1.0f);
        // 禁用态可辨识：不支持行轨道色 != 支持但未勾行（灰）
        const bool sameColor =
            trackHaiku->color.r == trackDef->color.r &&
            trackHaiku->color.g == trackDef->color.g &&
            trackHaiku->color.b == trackDef->color.b &&
            trackHaiku->color.a == trackDef->color.a;
        DOCTEST_CHECK_FALSE(sameColor);
        // 显示层剥标记：1M 由复选框表达，名字不重复带
        // [1m] 照剥；haiku 槽位不支持但串尾 [1M] 同样剥（剥除是无条件显示
        // 语义，勾态才看双重条件）。存储值原样（下方落库断言仍带标记）
        const auto* valSonnet = ui.find("claude.slot.val.1");
        const auto* valHaiku = ui.find("claude.slot.val.3");
        DOCTEST_REQUIRE(valSonnet != nullptr);
        DOCTEST_REQUIRE(valHaiku != nullptr);
        DOCTEST_CHECK_EQ(valSonnet->text, std::string("m-sonnet"));
        DOCTEST_CHECK_EQ(valHaiku->text, std::string("m-haiku"));
    }
    // 禁用行点了不落库：haiku(3) 不支持 / fable(4) 空 base / subagent(5) 空 base
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.slot.onem.3.hit"));
        DOCTEST_CHECK(click(ui, "claude.slot.onem.4.hit"));
        DOCTEST_CHECK(click(ui, "claude.slot.onem.5.hit"));
    }
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        auto gotMc = module::claude::parseModelsConfig(got.value().modelsConfigJson);
        DOCTEST_REQUIRE(gotMc.isOk());
        DOCTEST_CHECK_EQ(gotMc.value().haiku.model, std::string("m-haiku[1M]"));
        DOCTEST_CHECK_EQ(gotMc.value().fable.model, std::string(""));
        DOCTEST_CHECK_EQ(gotMc.value().subagent.model, std::string(""));
        DOCTEST_CHECK_EQ(gotMc.value().def.model, std::string("m-def"));
    }
    // 可用行：def 点一下 → 加标记落库；sonnet 点一下 → 去标记落库
    //（两下都在同一帧的 UI 快照上：勾态取自当帧构建时的 checked，互不串扰）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.slot.onem.0.hit"));
        DOCTEST_CHECK(click(ui, "claude.slot.onem.1.hit"));
    }
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        auto gotMc = module::claude::parseModelsConfig(got.value().modelsConfigJson);
        DOCTEST_REQUIRE(gotMc.isOk());
        DOCTEST_CHECK_EQ(gotMc.value().def.model, std::string("m-def[1M]"));
        DOCTEST_CHECK_EQ(gotMc.value().sonnet.model, std::string("m-sonnet"));
        // 旧 model 列与 def 槽位同步（与表单保存同口径）
        DOCTEST_CHECK_EQ(got.value().model, std::string("m-def[1M]"));
        // 保存前归一是全表口径：不支持槽位的遗留标记顺带剥掉（自愈到不变式）
        DOCTEST_CHECK_EQ(gotMc.value().haiku.model, std::string("m-haiku"));
    }
    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// P. 表单保存前归一（不支持槽位强制剥离 + 大小写规范化）
REGISTER_TEST(claude_ui_form_one_m_normalize_on_save,
              "claude_module: 表单保存 1M 归一（不支持剥离 + 大小写规范化）", {
    TestEnv env;
    auto a = env.svc->createProfile("1M表单", "https://form1m.example.com",
                                    "sk-ant-1m-form-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_onem_form");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto type = [](eui::Ui& ui, const std::string& id,
                   const std::string& text) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onTextInput == nullptr) {
            std::cerr << "missing input: " << id << std::endl;
            return false;
        }
        core::TextInputEvent ev;
        ev.text = text;
        node->onTextInput(ev);
        return true;
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.edit.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.1"));   // 模型映射页
    }
    // 手输：def 小写标记（支持→规范化）/ haiku 大写标记（不支持→剥离）/
    //       opus 无标记（原样）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.form.slot.0.model.hit", "m-form-def[1m]"));
        DOCTEST_CHECK(type(ui, "claude.form.slot.2.model.hit", "m-form-opus"));
        DOCTEST_CHECK(type(ui, "claude.form.slot.3.model.hit", "m-form-haiku[1M]"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        auto gotMc = module::claude::parseModelsConfig(got.value().modelsConfigJson);
        DOCTEST_REQUIRE(gotMc.isOk());
        // 支持槽位：保留且**规范成大写 [1M]**
        DOCTEST_CHECK_EQ(gotMc.value().def.model, std::string("m-form-def[1M]"));
        // 不支持槽位：手输的标记被**强制剥离**
        DOCTEST_CHECK_EQ(gotMc.value().haiku.model, std::string("m-form-haiku"));
        DOCTEST_CHECK_EQ(gotMc.value().opus.model, std::string("m-form-opus"));
        // 旧 model 列 = def 槽位（含规范化后的标记）
        DOCTEST_CHECK_EQ(got.value().model, std::string("m-form-def[1M]"));
    }
    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 旧库（有 keychain_account 列 + 待回填行）启动时的钥匙串回填**只允许尝试一次**：
// ad-hoc 签名下钥匙串授权每次重装都会失效（macOS 会弹「请输入钥匙串密码」），
// 若每次启动都重试，用户永远清不掉这个框（2026-10-18 实证：id=4 是激活 profile，
// 密钥只在钥匙串里）。判据：①首启确实读取 ②一次性标记落库 ③二次启动零额外读取。
REGISTER_TEST(claude_keychain_backfill_one_shot,
              "claude_module: 旧凭据回填只尝试一次（二次启动零钥匙串触达）", {
    TestEnv env;
    // 还原旧库形态：新 DDL 已无该列，手工补回 + 一行「密文列空、指向钥匙串」的 profile
    DOCTEST_REQUIRE(env.handle().exec(
        "ALTER TABLE claude_profiles ADD COLUMN keychain_account TEXT").isOk());
    DOCTEST_REQUIRE(env.handle().exec(
        "INSERT INTO claude_profiles "
        "(name, base_url, api_key_hint, api_key_check, model, created_at, updated_at, "
        "keychain_account) VALUES ('legacy', 'https://example.invalid', 'hint', '', "
        "'m', 0, 0, 'claude:legacy')").isOk());

    // 假钥匙串：只记账，恒答「没有这条」（真实现会让测试机弹授权框）
    static int loads = 0;
    loads = 0;
    platform::keychain::Backend fake = platform::keychain::defaultBackend();
    fake.loadSecret = [](const std::string&, const std::string&)
        -> core::Result<std::string> {
        ++loads;
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                               "Keychain item not found");
    };
    const platform::keychain::Backend* prev =
        platform::keychain::setBackendForTest(&fake);

    {
        std::unique_ptr<::core::Module> mod(create_claude_module());
        mod->onStart();
        mod->onStop();
    }
    DOCTEST_CHECK(loads >= 1);   // ①首启确实试过（派生命名空间 + 旧名回退）
    const int afterFirst = loads;
    auto done = core::appconfig::get(env.handle(), "claude.keychain_backfill_done");
    DOCTEST_CHECK(done.isOk());
    DOCTEST_CHECK_EQ(done.isOk() ? done.value() : std::string(), std::string("1"));

    {
        std::unique_ptr<::core::Module> mod(create_claude_module());
        mod->onStart();
        mod->onStop();
    }
    DOCTEST_CHECK_EQ(loads, afterFirst);   // ③二次启动零钥匙串触达

    platform::keychain::setBackendForTest(prev);
    return true;
})

// UAT4 布局自适应（用户真机反馈：布局应随窗宽伸缩）
// 左栏 leftW = clamp(contentW×0.39, 320, 460)，替代固定 400。防两件事故：
//   ① 最小窗 1000（contentW=712）下右栏只剩 296，详情面板放不下；
//   ② 右栏宽不随窗宽变（默认窗起宽窗大量空白）。
// 0.39 = 400/1024：默认窗 1312 下与旧固定 400 几何等价（零回归锚）。
// 四档窗宽复核右栏 ≥350（审计口径）；档间随窗宽单调伸缩。
REGISTER_TEST(claude_module_left_pane_adaptive_width,
              "claude_module: 左栏 clamp(0.39,320,460) 自适应 + 四档右栏 ≥350（headless）", {
    module::claude::ClaudeService::setGlobal(nullptr);
    std::unique_ptr<::core::Module> mod(create_claude_module());
    // 模块可见屏宽 = 窗宽 − 侧栏 208（core::windowToCompose 口径，test_router 同源）
    const struct { float winW; float expLeftW; } kCases[] = {
        {1000.0f, 320.0f},    // contentW 712 → 277.68 触下限 320；右栏 376
        {1200.0f, 355.68f},   // contentW 912 → 912×0.39；右栏 540.32
        {1312.0f, 399.36f},   // contentW 1024 → ≈400（旧固定值）；右栏 608.64
        {1600.0f, 460.0f},    // contentW 1312 → 511.68 触上限 460；右栏 836
    };
    for (const auto& c : kCases) {
        eui::Screen screen{c.winW - 208.0f, 1000.0f};
        eui::Ui ui;
        ui.begin("claude_leftw");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);

        const auto* left = ui.find("claude.profiles");
        const auto* right = ui.find("claude.detail");
        DOCTEST_REQUIRE(left != nullptr);
        DOCTEST_REQUIRE(right != nullptr);
        DOCTEST_CHECK_MESSAGE(
            floatEq(left->frame.width, c.expLeftW),
            "窗 " + std::to_string(c.winW) + " 左栏宽 " +
                std::to_string(left->frame.width) + " 期望 " +
                std::to_string(c.expLeftW));
        const float contentW = screen.width - 80.0f;   // pad 40×2
        const float expRightW = contentW - c.expLeftW - 16.0f;   // gap 16
        DOCTEST_CHECK(floatEq(right->frame.width, expRightW));
        DOCTEST_CHECK_MESSAGE(right->frame.width >= 350.0f,
                              "窗 " + std::to_string(c.winW) + " 右栏宽 " +
                                  std::to_string(right->frame.width) +
                                  " < 350（审计下限）");
    }
    mod->onStop();
    return true;
})
