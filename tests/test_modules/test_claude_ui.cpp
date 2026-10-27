// tests/test_modules/test_claude_ui.cpp
// Claude 模块 UI 接线无头冒烟（compose 模式）：无服务时不崩、有服务时渲染
// profile 卡片 / ACTIVE 徽标 / 详情 / 操作按钮。
// 跨文件前提：详情面板读的是后台快照（H2），保存 / 外部写库后最多晚 1 帧可见——
// 断言可见性一律走有界轮询 + onPageEnter，不能只 compose 一帧就断言。
// 真实鼠标点击留给人工点验。
#include "doctest.h"
#include "test_dialog_frame_helper.h"  // dialog_frame：弹窗「元素在框内」共享断言
#include "test_support.h"

#include "claude_service.h"
#include "claude_ui_logic.h"
#include "core/app_config.h"
#include "core/database.h"
#include "platform/json.h"
#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/component_theme.h"   // componentTokensWeChat：Danger 按钮底色的实际令牌源
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
extern "C" void claude_test_bypass_switch_confirm(::core::Module*);

// 测试模块工厂：默认关掉切换确认弹窗（全量回归直走选中；对话框测试用 mkmod(false)）
static std::unique_ptr<::core::Module> mkmod(bool bypassSwitch = true) {
    auto m = std::unique_ptr<::core::Module>(create_claude_module());
    if (bypassSwitch) claude_test_bypass_switch_confirm(m.get());
    return m;
}

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

// 接管/同步测试的 settings.json 路径（不碰真实 ~/.claude——
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

REGISTER_TEST(claude_module_compose_null_service,
              "claude_module: compose 无服务不崩（headless）", {
    module::claude::ClaudeService::setGlobal(nullptr);   // 干净起点
    auto mod = mkmod();
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    ui.begin("claude_null");
    mod->compose(ui, screen);
    ui.end();

    // 关键操作按钮即使无数据也应渲染
    DOCTEST_CHECK(ui.find("claude.root") != nullptr);
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.run") != nullptr);
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

    // 无选中 + 无服务：动作行只剩 3 键——「未选中提示」通道由右键菜单的 warn 覆盖
    {
        DOCTEST_CHECK(ui.find("claude.act.del.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.act.dup.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.act.copyconfig.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.act.run.bg") != nullptr);
    }

    mod->onStop();
    return true;
})

// 3 键均分几何：末键右缘对齐分隔线
// actW = (w0 − actGap×2)/3。EUI 的 row spacing 只在相邻子项之间生效，n 项只有
// n−1 个间隙；多扣一个 gap 会让末键右缘短一整个 gap。

REGISTER_TEST(claude_module_actions_row_alignment,
              "claude_module: 3 键均分——末键（运行）右缘对齐分隔线（headless layout）", {
    module::claude::ClaudeService::setGlobal(nullptr);
    auto mod = mkmod();
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
        // 单键宽 = (行宽 − 2×gap) / 3（gap=8）
    const float expect = (row->frame.width - 8.0f * 2.0f) / 3.0f;
    DOCTEST_CHECK(std::fabs(run->frame.width - expect) < 0.01f);
    DOCTEST_CHECK(std::fabs(ui.find("claude.act.test.bg")->frame.width - expect) <
                  0.01f);

    mod->onStop();
    return true;
})

// 三页签最小布局：每个 tab 切过去内容可达；动作行钉面板底但不越界
REGISTER_TEST(claude_detail_three_tabs_layout,
              "claude_module: 详情三页签内容可达 + 动作行不越面板底（headless layout）", {
    TestEnv env;
    auto a = env.svc->createProfile("三页", "https://three.example.com",
                                    "sk-ant-three-0001", "claude-sonnet-4-5",
                                    /*activateIfNoneActive=*/true);
    DOCTEST_REQUIRE(a.isOk());
    auto mod = mkmod();
    eui::Screen screen{1600.0f, 1000.0f};
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_detail3");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };

    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(ui.find("claude.detail.tab.0.hit") != nullptr);
        DOCTEST_REQUIRE(ui.find("claude.detail.tab.1.hit") != nullptr);
        DOCTEST_REQUIRE(ui.find("claude.detail.tab.2.hit") != nullptr);
        DOCTEST_CHECK(ui.find("claude.detail.tab.3.hit") == nullptr);
        DOCTEST_CHECK(ui.find("claude.field.name.value") != nullptr);
        const auto* tab1 = ui.find("claude.detail.tab.1.hit");
        DOCTEST_REQUIRE(tab1 != nullptr);
        DOCTEST_REQUIRE(tab1->onClick != nullptr);
        tab1->onClick();
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.slot.name.0") != nullptr);
        const auto* tab2 = ui.find("claude.detail.tab.2.hit");
        DOCTEST_REQUIRE(tab2 != nullptr);
        DOCTEST_REQUIRE(tab2->onClick != nullptr);
        tab2->onClick();
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.env.row.bg.0") != nullptr);
        const auto* row = ui.find("claude.detail.actions");
        const auto* panel = ui.find("claude.detail.overlay");
        DOCTEST_REQUIRE(row != nullptr);
        DOCTEST_REQUIRE(panel != nullptr);
        const float panelBottom = panel->frame.y + panel->frame.height;
        DOCTEST_CHECK(row->frame.y >= panel->frame.y);
        DOCTEST_CHECK(row->frame.y + row->frame.height <= panelBottom + 0.5f);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    return true;
})

// UAT2 #10 C 批布局回归锁（L-5，UAT2 一审）：状态组（dot+text+help）从 title row
// 挪到 header 右侧 actions 区后——① 右对齐：status row 右缘 ≥ title row 右缘
//（状态组贴 header 右端，title row 只占左段 contentW−360）；② wrapContent
// 收缩：无服务态「已停止」≈39px，回退成旧 200px 固定宽盒 → 超 120 红；
// ③ 页头只剩标题行锁（UAT6 副题横扫）：副题 14 行已删，标题行 24 独占页头块。
// headless 出 frame 须 ui.layout()，写法仿上一条。
REGISTER_TEST(claude_module_header_status_geometry,
              "claude_module: header 状态组右对齐 + wrapContent 收缩（headless layout）", {
    module::claude::ClaudeService::setGlobal(nullptr);
    auto mod = mkmod();
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
    // ③ 页头只剩标题行锁：副题 14 行已删（UAT6 副题横扫），标题行 24 独占页头块；
    // 副题元素不得回潮。
    DOCTEST_CHECK(floatEq(titleRow->frame.height, 24.0f));
    DOCTEST_CHECK(ui.find("claude.subtitle") == nullptr);

    mod->onStop();
    return true;
})

// UAT6 问号钮重设计（控件一致性横扫）：帮助图标从死 text 占位（无 hover 无
// tooltip，注释自认「占位待绑定」）改为标准小图标钮——24px 正圆、icon 居中、
// hover 三态、flat（ButtonStyle 默认阴影已根修为无），长文案经 EUI tooltip 弹回。
REGISTER_TEST(claude_module_help_button_spec,
              "claude_module: 帮助钮规格 = 24px 正圆/hover 三态/flat/tooltip 绑定", {
    module::claude::ClaudeService::setGlobal(nullptr);   // 无服务态：页头恒渲染且为已停止
    auto mod = mkmod();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("claude_help_btn");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    // ① 结构：stack + .bg + .icon 三节点（旧实现是裸 text 节点占位，.icon 子
    //    节点存在即新版结构；旧 id 现被 stack 占用）
    const auto* help = ui.find("claude.title.help");
    const auto* bg = ui.find("claude.title.help.bg");
    const auto* icon = ui.find("claude.title.help.icon");
    DOCTEST_REQUIRE(help != nullptr);
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_REQUIRE(icon != nullptr);
    // ② 几何：24px 正圆（radius 12 = 直径一半，md.help.icon 同款 24px 档）；icon 双向居中
    DOCTEST_CHECK(floatEq(bg->frame.width, 24.0f));
    DOCTEST_CHECK(floatEq(bg->frame.height, 24.0f));
    DOCTEST_CHECK(floatEq(bg->radius, 12.0f));
    DOCTEST_CHECK(icon->horizontalAlign == eui::HorizontalAlign::Center);
    DOCTEST_CHECK(icon->verticalAlign == eui::VerticalAlign::Center);
    // ③ hover 三态接线（states）+ 0xF059 问号 glyph
    DOCTEST_CHECK(bg->hasStateColors);
    DOCTEST_CHECK(icon->text == eui::utf8(0xF059));
    // ④ flat（UAT6 阴影统一）：帮助钮与同排「启动代理」主按钮一律无阴影
    DOCTEST_CHECK(!bg->shadow.enabled);
    const auto* proxyBg = ui.find("claude.btn.proxy.bg");
    DOCTEST_REQUIRE(proxyBg != nullptr);
    DOCTEST_CHECK(!proxyBg->shadow.enabled);
    // ⑤ tooltip 绑定：节点在 + source 指向帮助钮 bg + 已停态一句话摘要
    //   （UAT6 副题横扫收起的文案在 hover 弹回，防丢文案回归）
    const auto* tip = ui.find("claude.title.help.tooltip");
    DOCTEST_REQUIRE(tip != nullptr);
    DOCTEST_CHECK(tip->hoverOpacitySourceId.find("claude.title.help.bg") !=
                  std::string::npos);
    const auto* tipText = ui.find("claude.title.help.tooltip.text");
    DOCTEST_REQUIRE(tipText != nullptr);
    DOCTEST_CHECK(tipText->text == "管理多套 Claude API 配置");

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

    auto mod = mkmod();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("claude_data");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("claude.profile.0.name") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.1.name") != nullptr);

    const auto* nameV = ui.find("claude.field.name.value");
    DOCTEST_REQUIRE(nameV != nullptr);
    DOCTEST_CHECK_EQ(nameV->text, std::string("官方"));
    const auto* urlV = ui.find("claude.field.url.value");
    DOCTEST_REQUIRE(urlV != nullptr);
    DOCTEST_CHECK_EQ(urlV->text, std::string("https://api.anthropic.com"));

    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.run") != nullptr);
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

    auto mod = mkmod();
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
    auto mod = mkmod();
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

    auto mod = mkmod();
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

    auto mod = mkmod();
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
        DOCTEST_CHECK(ui.find("claude.form.tabs.1.hit") != nullptr);
        const auto* nameText = ui.find("claude.dialog.form.name.text");
        DOCTEST_REQUIRE(nameText != nullptr);
        DOCTEST_CHECK_EQ(nameText->text, std::string("槽位测试"));
        DOCTEST_CHECK(click(ui, "claude.form.tabs.1.hit"));
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.2.hit"));
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
            DOCTEST_CHECK(click(ui, "claude.detail.tab.1.hit"));  // 切到模型 tab
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.1.hit"));
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.2.hit"));
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

    auto mod = mkmod();
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
    }
    // 最小高度 720：末卡滚到底可达且可点选。视口让出底部按钮区后，
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

REGISTER_TEST(claude_module_form_esc_cancels,
              "claude_module: 表单基础页/模型页输入框 Esc=取消不落库（headless）", {
    TestEnv env;
    auto mod = mkmod();
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
        auto* tab = ui.find("claude.form.tabs.1.hit");
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
    auto mod = mkmod();
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

REGISTER_TEST(claude_ui_esc_closes_delete_confirm,
              "claude_ui: Esc 关删除确认弹窗（无输入框）", {
    TestEnv env;
    auto created = env.svc->createProfile("Esc目标", "https://esc.example.com",
                                          "sk-ant-esc-00001", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t pid = created.value().id;

    auto mod = mkmod();
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
// refreshDataCache 若在 compose 里同步跑 listProfiles / getActiveProfile，
// 会与代理 worker 的 DB 写争同一把 recursive_mutex
//（事务全程持锁，core/sqlite_db.cpp）→ 代理忙时整帧被卡。
// 渲染路径只发刷新意图 + 搬已就绪结果，查询在后台线程（std::async）。
REGISTER_TEST(claude_h2_compose_snapshot_async,
              "claude_module: compose 只读快照 DB 后台刷新（H2 tick 前旧值/后新值）", {
    TestEnv env;
    auto a = env.svc->createProfile("官方", "https://api.anthropic.com",
                                    "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());

    auto mod = mkmod();
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

    auto mod = mkmod();
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

REGISTER_TEST(claude_module_takeover_warn_visible,
              "claude_module: 接管失败警告在详情页可见（headless）", {
    TestEnv env;
    test_support::TempDir d("warnfail", kImportTempDirPrefix);
    env.svc->setSettingsSyncPaths(importPaths(d, env.handle()));
    env.svc->setSettingsTakeOverEnabled(true);
    // 坏 settings.json（非法 JSON）→ takeOver 解析失败中止，全程不写 sidecar
    d.write(d.file("settings.json"), "{ 这不是合法 JSON");

    auto mod = mkmod();
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

    auto mod = mkmod();
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
    auto mod = mkmod();
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

// UAT5 弹窗单列回退批：基础页「名称」「认证字段」改回全宽纵排（UAT3 双列被用户
// 推翻）+ 四 tab 弹高统一 600（cols2 的 532 偏离致切 tab 跳变）。
// 防的事故：名称/认证回退同排双列（右列 x=388）→ 纵排 y 断言红；任一 tab 弹高
// 回 532 → 恒高断言红；切 tab 往返高度变化 → 恒定断言红。
REGISTER_TEST(claude_ui_form_basic_single_column_geometry,
              "claude_module: 基础页名称/认证全宽纵排 + 四 tab 弹高恒 600", {
    TestEnv env;
    auto a = env.svc->createProfile("单列几何", "https://geo.example.com",
                                    "sk-ant-geo-0001", "");
    DOCTEST_REQUIRE(a.isOk());

    auto mod = mkmod();
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

        // 弹窗尺寸：760 宽 × 540 高（UAT6：最小窗 1000×640 下 540 档不溢出）
        DOCTEST_CHECK(floatEq(panel->frame.width, 760.0f));
        DOCTEST_CHECK(floatEq(panel->frame.height, 540.0f));

        // 单列纵排：名称行在认证行上方（y=104 < 314）；两标签同 x=28
        const auto* nameLabel = ui.find("claude.form.basic.name.label");
        const auto* authLabel = ui.find("claude.form.basic.authfield.label");
        DOCTEST_REQUIRE(nameLabel != nullptr);
        DOCTEST_REQUIRE(authLabel != nullptr);
        DOCTEST_CHECK(floatEq(nameLabel->frame.x - px, 28.0f));
        DOCTEST_CHECK(floatEq(nameLabel->frame.y - py, 104.0f));
        DOCTEST_CHECK(floatEq(authLabel->frame.x - px, 28.0f));
        DOCTEST_CHECK(floatEq(authLabel->frame.y - py, 314.0f));
        DOCTEST_CHECK(authLabel->frame.y > nameLabel->frame.y);

        // 名称输入 / 认证下拉：全宽 704（= 760-56），右缘 == 内容右缘
        const auto* nameInput = ui.find("claude.dialog.form.name");
        const auto* ddwrap = ui.find("claude.form.basic.authfield.ddwrap");
        DOCTEST_REQUIRE(nameInput != nullptr);
        DOCTEST_REQUIRE(ddwrap != nullptr);
        DOCTEST_CHECK(floatEq(nameInput->frame.x - px, 28.0f));
        DOCTEST_CHECK(floatEq(nameInput->frame.width, 704.0f));
        DOCTEST_CHECK(floatEq(ddwrap->frame.x - px, 28.0f));
        DOCTEST_CHECK(floatEq(ddwrap->frame.width, 704.0f));
        DOCTEST_CHECK(floatEq(nameInput->frame.y - py, 122.0f));
        DOCTEST_CHECK(floatEq(ddwrap->frame.y - py, 332.0f));
        DOCTEST_CHECK(nameInput->frame.x + nameInput->frame.width <=
                      px + 760.0f - 28.0f + 0.5f);
        DOCTEST_CHECK(ddwrap->frame.x + ddwrap->frame.width <=
                      px + 760.0f - 28.0f + 0.5f);

        // Base URL / API Key 全宽不动：704
        const auto* urlInput = ui.find("claude.dialog.form.url");
        const auto* keyInput = ui.find("claude.dialog.form.key");
        DOCTEST_REQUIRE(urlInput != nullptr);
        DOCTEST_REQUIRE(keyInput != nullptr);
        DOCTEST_CHECK(floatEq(urlInput->frame.width, 704.0f));
        DOCTEST_CHECK(floatEq(keyInput->frame.width, 704.0f));

        // 认证 hint 随标签行（x=104 起、全宽余量）
        const auto* authHint = ui.find("claude.form.basic.authfield.hint");
        DOCTEST_REQUIRE(authHint != nullptr);
        DOCTEST_CHECK(floatEq(authHint->frame.x - px, 104.0f));
        DOCTEST_CHECK(authHint->frame.x - px + authHint->frame.width <= 732.0f);
    }
    // 三个 tab 弹高统一：逐 tab 切过去断高度（按页取值会红）
    for (int tab = 1; tab <= 2; ++tab) {
        {
            eui::Ui ui;
            compose(ui, "claude_geo_tab");
            const std::string hit =
                std::string("claude.form.tabs.") + std::to_string(tab) + ".hit";
            DOCTEST_CHECK(click(ui, hit));
        }
        {
            eui::Ui ui;
            compose(ui, "claude_geo_h");
            ui.layout(screen.width, screen.height);
            const auto* panel = ui.find("claude.dialog.form.panel");
            DOCTEST_REQUIRE(panel != nullptr);
            DOCTEST_CHECK(floatEq(panel->frame.height, 540.0f));
        }
    }
    // 切回基础页（tab 0）往返：高度恒定（用户口径「切 tab 不跳变」）
    {
        eui::Ui ui;
        compose(ui, "claude_geo_back");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.0.hit"));
    }
    {
        eui::Ui ui;
        compose(ui, "claude_geo_back_h");
        ui.layout(screen.width, screen.height);
        const auto* panel = ui.find("claude.dialog.form.panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(floatEq(panel->frame.height, 540.0f));
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(claude_ui_auth_field_create_persists,
              "claude_module: 新建表单选「只发 x-api-key」保存后落库", {
    TestEnv env;
    auto mod = mkmod();
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
                                    /*activateIfNoneActive=*/false, "api_key");
    DOCTEST_REQUIRE(a.isOk());

    auto mod = mkmod();
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

static std::string s_claudeRunPickerDir;  // 假 picker 目标目录（函数指针缝，不能捕获）

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
    auto mod = mkmod();
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

REGISTER_TEST(claude_ui_logic_pure_functions,
              "claude_ui_logic: 纯函数等价断言（模型摘要）", {
    using module::claude::modelsSummary;

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
    return true;
})

REGISTER_TEST(claude_copyconfig_ctx_menu_after_row_slim,
              "claude_module: 详情动作行 3 键 + 复制配置仅右键菜单（文案/节点收口/点击派发）", {
    TestEnv env;
    auto mod = mkmod();
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
    // 3 键仍在（防误渲染提示）
    {
        eui::Ui ui;
        compose(ui);
        for (const char* k : {"claude.act.del", "claude.act.dup",
                              "claude.act.copyconfig"}) {
            DOCTEST_CHECK(ui.find(std::string(k) + ".bg") == nullptr);
        }
        DOCTEST_CHECK(ui.find("claude.act.test.bg") != nullptr);
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

    // 动作行只剩 3 键（顺序：测试连通/编辑/运行），且窄行下
    {
        eui::Ui ui;
        compose(ui);
        for (const char* k : {"claude.act.test",
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
//   P1 动作区钉面板底（三页签分隔线 y 恒定）+ 内容框显式边界
//   P2 「环境变量」列全部生效 env + 每键来源
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

// click 是 clickById 的别名（222 处旧调用，clickById 是 2026-09-29 规范命名；新调用用 clickById）
bool click(eui::Ui& ui, const std::string& id) { return clickById(ui, id); }

}  // namespace

REGISTER_TEST(claude_copyconfig_seven_keys_geometry,
              "claude_module: 动作行 3 键 actW 等分几何（macOS 真实尺寸）", {
    TestEnv env;
    auto a = env.svc->createProfile("几何源", "https://geo.example.com",
                                    "sk-ant-geo-0001", "m");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    auto mod = mkmod();
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
    // 动作行 3 键，末键（运行）右缘对齐断言据此定宽
    const float expect = (w0 - 8.0f * 2.0f) / 3.0f;
    DOCTEST_CHECK(std::fabs(run->frame.width - expect) < 0.01f);
    // 行右缘对齐：末键右缘 == row 右缘（浮点 0.5px 容差）
    const float rowRight = row->frame.x + row->frame.width;
    const float runRight = run->frame.x + run->frame.width;
    DOCTEST_CHECK(std::fabs(rowRight - runRight) < 0.5f);
    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

REGISTER_TEST(claude_switch_bottom_buttons_dual_tier,
              "claude_module: H17 切换器底部按钮双档可见性（1000x640 / 1600x1000）", {
    TestEnv env;
    auto p = env.svc->createProfile("底钮", "https://btn.example.com",
                                    "sk-ant-btn-0001", "m-btn");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());

    auto mod = mkmod();
    mod->onStart();

    // 两档逐档断言（不可放入 lambda——DOCTEST 宏内 return false 与 void lambda 冲突）
    // 640 档（最小窗内容高度档）
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "h17_640", 1000.0f, 640.0f);
        const auto* profilesPanel = ui.find("claude.profiles");
        const auto* detailPanel = ui.find("claude.detail");
        DOCTEST_REQUIRE(profilesPanel != nullptr);
        DOCTEST_REQUIRE(detailPanel != nullptr);
        const float leftBottom =
            profilesPanel->frame.y + profilesPanel->frame.height;
        const float rightBottom =
            detailPanel->frame.y + detailPanel->frame.height;
        // 左列新建钮完整可见
        for (const char* id : {"claude.profiles.new"}) {
            const auto* btn = ui.find(id);
            DOCTEST_REQUIRE(btn != nullptr);
            DOCTEST_CHECK_MESSAGE(
                btn->frame.y + btn->frame.height <= leftBottom + 0.5f,
                std::string("640 ") + id + " 越出左列面板底: btn bottom=" +
                    std::to_string(btn->frame.y + btn->frame.height) +
                    " panel bottom=" + std::to_string(leftBottom));
        }
        // 右列动作区三钮 + 副提示 + 分隔线完整可见
        for (const char* id : {"claude.act.test", "claude.act.edit",
                               "claude.act.run"}) {
            const auto* btn = ui.find(id);
            DOCTEST_REQUIRE(btn != nullptr);
            DOCTEST_CHECK_MESSAGE(
                btn->frame.y + btn->frame.height <= rightBottom + 0.5f,
                std::string("640 ") + id + " 越出右列面板底: btn bottom=" +
                    std::to_string(btn->frame.y + btn->frame.height) +
                    " panel bottom=" + std::to_string(rightBottom));
        }
        const auto* hint = ui.find("claude.act.run.hint");
        const auto* divider = ui.find("claude.act.divider");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_REQUIRE(divider != nullptr);
        DOCTEST_CHECK(hint->frame.y + hint->frame.height <=
                      rightBottom + 0.5f);
        DOCTEST_CHECK(divider->frame.y + divider->frame.height <=
                      rightBottom + 0.5f);
        // H17 面板底不得越出页面区（越出即被 app.page 裁掉；修复前实证红：
        // ①claude.root padding 实参序错位 ②帮助钮 tooltip 排入列布局占 56px）
        DOCTEST_CHECK_MESSAGE(leftBottom <= 640.0f + 0.5f,
                              "640 档左列面板底越出页面区: " +
                                  std::to_string(leftBottom));
        DOCTEST_CHECK_MESSAGE(rightBottom <= 640.0f + 0.5f,
                              "640 档右列面板底越出页面区: " +
                                  std::to_string(rightBottom));
    }

    // 1000 档（大窗，等比放大后不得越界）
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "h17_1000", 1600.0f, 1000.0f);
        const auto* profilesPanel = ui.find("claude.profiles");
        const auto* detailPanel = ui.find("claude.detail");
        DOCTEST_REQUIRE(profilesPanel != nullptr);
        DOCTEST_REQUIRE(detailPanel != nullptr);
        const float leftBottom =
            profilesPanel->frame.y + profilesPanel->frame.height;
        const float rightBottom =
            detailPanel->frame.y + detailPanel->frame.height;
        for (const char* id : {"claude.profiles.new"}) {
            const auto* btn = ui.find(id);
            DOCTEST_REQUIRE(btn != nullptr);
            DOCTEST_CHECK_MESSAGE(
                btn->frame.y + btn->frame.height <= leftBottom + 0.5f,
                std::string("1000 ") + id + " 越出左列面板底: btn bottom=" +
                    std::to_string(btn->frame.y + btn->frame.height) +
                    " panel bottom=" + std::to_string(leftBottom));
        }
        for (const char* id : {"claude.act.test", "claude.act.edit",
                               "claude.act.run"}) {
            const auto* btn = ui.find(id);
            DOCTEST_REQUIRE(btn != nullptr);
            DOCTEST_CHECK_MESSAGE(
                btn->frame.y + btn->frame.height <= rightBottom + 0.5f,
                std::string("1000 ") + id + " 越出右列面板底: btn bottom=" +
                    std::to_string(btn->frame.y + btn->frame.height) +
                    " panel bottom=" + std::to_string(rightBottom));
        }
        const auto* hint = ui.find("claude.act.run.hint");
        const auto* divider = ui.find("claude.act.divider");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_REQUIRE(divider != nullptr);
        DOCTEST_CHECK(hint->frame.y + hint->frame.height <=
                      rightBottom + 0.5f);
        DOCTEST_CHECK(divider->frame.y + divider->frame.height <=
                      rightBottom + 0.5f);
        DOCTEST_CHECK_MESSAGE(leftBottom <= 1000.0f + 0.5f,
                              "1000 档左列面板底越出页面区: " +
                                  std::to_string(leftBottom));
        DOCTEST_CHECK_MESSAGE(rightBottom <= 1000.0f + 0.5f,
                              "1000 档右列面板底越出页面区: " +
                                  std::to_string(rightBottom));
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

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

    auto mod = mkmod();
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
    auto mod = mkmod();
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

    auto mod = mkmod();
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
    DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());

    auto mod = mkmod();
    mod->onStart();

    eui::Ui ui;
    composeAtSize(mod.get(), ui, "claude_env", 1262.0f, 801.0f);
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tab.2.hit"));
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
    // 非模型键照常列：托管连接键、profile 自身 extra_env
    DOCTEST_REQUIRE(srcOf.count("ANTHROPIC_BASE_URL") == 1);
    DOCTEST_CHECK_EQ(srcOf["ANTHROPIC_BASE_URL"], std::string("托管键"));
    DOCTEST_CHECK_EQ(srcOf["ANTHROPIC_AUTH_TOKEN"], std::string("托管键"));
    DOCTEST_REQUIRE(srcOf.count("MY_PROFILE") == 1);
    DOCTEST_CHECK_EQ(srcOf["MY_PROFILE"], std::string("本 Profile"));

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

    auto mod = mkmod();
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
    DOCTEST_REQUIRE(clickById(ui1, "claude.detail.tab.2.hit"));
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

REGISTER_TEST(claude_module_onstart_reentry_idempotent,
              "claude_module: onStart→onStop→onStart 重入 + 双 onStart/双 onStop no-op", {
    TestEnv env;
    DOCTEST_REQUIRE(module::claude::ClaudeService::global() != nullptr);
    auto mod = mkmod();

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

    auto mod = mkmod();
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
        DOCTEST_CHECK(click(ui, "claude.detail.tab.1.hit"));
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
        //（UAT5 米黄收尾）禁用 knob 换 WeChat 白（surface，与启用态白 knob 同源），
        // 不再走暖橙 surfaceElevated；「禁用」语义仍由 textDisabled 深灰轨道承担
        const auto& wc = core::theme::current().colors;
        DOCTEST_CHECK(knobHaiku->color.r == wc.surface.r &&
                      knobHaiku->color.g == wc.surface.g &&
                      knobHaiku->color.b == wc.surface.b);
        DOCTEST_CHECK(!(knobHaiku->color.r == wc.surfaceElevated.r &&
                        knobHaiku->color.g == wc.surfaceElevated.g &&
                        knobHaiku->color.b == wc.surfaceElevated.b));
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

    auto mod = mkmod();
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.1.hit"));   // 模型映射页
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
        auto mod = mkmod();
        mod->onStart();
        mod->onStop();
    }
    DOCTEST_CHECK(loads >= 1);   // ①首启确实试过（派生命名空间 + 旧名回退）
    const int afterFirst = loads;
    auto done = core::appconfig::get(env.handle(), "claude.keychain_backfill_done");
    DOCTEST_CHECK(done.isOk());
    DOCTEST_CHECK_EQ(done.isOk() ? done.value() : std::string(), std::string("1"));

    {
        auto mod = mkmod();
        mod->onStart();
        mod->onStop();
    }
    DOCTEST_CHECK_EQ(loads, afterFirst);   // ③二次启动零钥匙串触达

    platform::keychain::setBackendForTest(prev);
    return true;
})

// UAT4 布局自适应（用户真机反馈：布局应随窗宽伸缩）
// 左栏固定 240px（S3.4），替代旧 clamp(0.39,320,460)。防两件事故：
//   ① 最小窗 1000（contentW=712）下右栏只剩 296，详情面板放不下；
//   ② 右栏宽不随窗宽变（默认窗起宽窗大量空白）。
// 0.39 = 400/1024：默认窗 1312 下与旧固定 400 几何等价（零回归锚）。
// 四档窗宽复核右栏 ≥350（审计口径）；档间随窗宽单调伸缩。
REGISTER_TEST(claude_module_left_pane_adaptive_width,
              "claude_module: 左栏 clamp(0.39,320,460) 自适应 + 四档右栏 ≥350（headless）", {
    module::claude::ClaudeService::setGlobal(nullptr);
    auto mod = mkmod();
    // 左栏固定 240px（S3.4），右栏 flex 自适应；模块可见屏宽 = 窗宽 − 侧栏 208
    const struct { float winW; float expLeftW; } kCases[] = {
        {1000.0f, 240.0f},
        {1200.0f, 240.0f},
        {1312.0f, 240.0f},
        {1600.0f, 240.0f},
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
        const float contentW = screen.width - 64.0f;   // padSide 32×2
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

// ── UAT5 批：Profile 卡选中「绿圈白勾」/ URL 省略号 / 表单与详情米黄清零 / 未激活徽章删除 ──

// W3-B4：选中态 3px 左 accent 竖线（替代绿圈白勾）；未选卡不画 accent，
// 点另一卡 accent 跟随移动；选中底色浅灰 primarySoftBg 保留。
REGISTER_TEST(claude_profile_selection_check_badge,
              "claude_module: 选中卡 3px accent 左竖线（未选不画/跟随切换/旧绿圈已删）", {
    TestEnv env;
    auto a = env.svc->createProfile("勾圈A", "https://a.example.com",
                                    "sk-ant-chk-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    auto b = env.svc->createProfile("勾圈B", "https://b.example.com",
                                    "sk-ant-chk-0002", "");
    DOCTEST_REQUIRE(b.isOk());

    auto mod = mkmod();
    eui::Screen screen{1600.0f, 1000.0f};
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
    auto sameColor = [](const eui::Color& x, const eui::Color& y) {
        return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
               std::fabs(x.b - y.b) < 0.01f;
    };
    const auto& t = core::theme::current();

    // 初帧：无显式选中 → 两卡都不画 accent；旧 sel.accent 已删
    {
        eui::Ui ui;
        compose(ui, "chk_init");
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") == nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.accent") == nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.0.sel.accent") == nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.sel.accent") == nullptr);
    }
    // 点卡 0 → accent 落卡 0（3px 左竖线 + 浅灰选中底）；卡 1 无 accent
    {
        eui::Ui ui;
        compose(ui, "chk_click0");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "chk_on0");
        ui.layout(screen.width, screen.height);  // frame 需显式布局
        const auto* c0 = ui.find("claude.profile.0.accent");
        const auto* bg0 = ui.find("claude.profile.0.bg");
        DOCTEST_REQUIRE(c0 != nullptr);
        DOCTEST_REQUIRE(bg0 != nullptr);
        DOCTEST_CHECK(sameColor(c0->color, t.colors.primary));
        DOCTEST_CHECK(sameColor(bg0->color, t.colors.primarySoftBg));
        // 竖线左缘 3px 全卡高
        DOCTEST_CHECK(floatEq(c0->frame.width, 3.0f));
        DOCTEST_CHECK(ui.find("claude.profile.1.accent") == nullptr);
    }
    // 点卡 1 → accent 跟随移动（卡 0 失 accent、卡 1 得 accent）
    {
        eui::Ui ui;
        compose(ui, "chk_click1");
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "chk_on1");
        const auto* c1 = ui.find("claude.profile.1.accent");
        DOCTEST_REQUIRE(c1 != nullptr);
        DOCTEST_CHECK(sameColor(c1->color, t.colors.primary));
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") == nullptr);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// UAT6 切换 Profile 二次确认弹窗：点卡弹窗 → 取消 = 不变 → 确认 = 切换；点已选卡无弹窗
REGISTER_TEST(claude_profile_switch_confirm_dialog,
              "claude_module: 切换 Profile 二次确认弹窗", {
    TestEnv env;
    auto a = env.svc->createProfile("A", "https://a.example.com", "sk-a", "");
    DOCTEST_REQUIRE(a.isOk());
    auto b = env.svc->createProfile("B", "https://b.example.com", "sk-b", "");
    DOCTEST_REQUIRE(b.isOk());
    const int64_t idA = a.value().id;
    const int64_t idB = b.value().id;

    auto mod = mkmod(false);  // 不 bypass——真走弹窗路径
    mod->onStart();
    eui::Screen screen{1600.0f, 1000.0f};
    auto compose = [&](eui::Ui& ui, const char* tag) {
        ui.begin(tag);
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    // 初帧：无选中 → 两卡都无 accent
    {
        eui::Ui ui;
        compose(ui, "dlg_init");
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") == nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.accent") == nullptr);
    }
    // 点卡 A → 弹窗出现；卡 A accent 未出现（确认前不切）
    {
        eui::Ui ui;
        compose(ui, "dlg_clickA");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_open");
        DOCTEST_CHECK(ui.find("claude.dialog.switch") != nullptr);   // 对话框节点常驻（组件无条件创建），开/关由 open 态控
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") == nullptr);  // 确认前不渲染 accent
    }
    // 点取消 → 弹窗关闭；卡 A accent 仍未出现（选择未变）
    {
        eui::Ui ui;
        compose(ui, "dlg_cancel");
        DOCTEST_CHECK(click(ui, "claude.dialog.switch.cancel.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_closed");
        DOCTEST_CHECK(ui.find("claude.dialog.switch") != nullptr);   // 节点常驻，取消后 open 态为 false
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") == nullptr);
    }
    // 再点卡 A + 确认 → accent 落到卡 A
    {
        eui::Ui ui;
        compose(ui, "dlg_clickA2");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_confirmA");
        DOCTEST_CHECK(click(ui, "claude.dialog.switch.confirm.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_confirmed");
        DOCTEST_CHECK(ui.find("claude.dialog.switch") != nullptr);   // 节点常驻，确认后 open 态回 false
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.accent") == nullptr);
    }
    // 点已选卡 A（selectedId_==idA）→ 无弹窗、accent 不变
    {
        eui::Ui ui;
        compose(ui, "dlg_sameA");
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_noop");
        DOCTEST_CHECK(ui.find("claude.dialog.switch") != nullptr);   // 节点常驻，点已选卡不弹窗
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") != nullptr);
    }
    // 点卡 B + 确认 → accent 移到卡 B
    {
        eui::Ui ui;
        compose(ui, "dlg_clickB");
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_confirmB");
        DOCTEST_CHECK(click(ui, "claude.dialog.switch.confirm.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dlg_onB");
        DOCTEST_CHECK(ui.find("claude.profile.0.accent") == nullptr);
        DOCTEST_CHECK(ui.find("claude.profile.1.accent") != nullptr);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, idA);
    cleanupProfileKey(kTestKeychainService, idB);
    return true;
})

// UAT5 米黄清理：表单 env tab 的输入面/按钮全量换 WeChat 令牌——
// 输入底 == surface、工具钮 == hover、主动作 == primary；
// 反向断言锁死暖橙 surfaceElevated/legacy primary。
REGISTER_TEST(claude_form_wechat_colors,
              "claude_module: 表单/全屏编辑输入面与按钮 WeChat 令牌锁死（反暖橙）", {
    TestEnv env;
    auto mod = mkmod();
    eui::Screen screen{1600.0f, 1000.0f};
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
    // 按值取色（缺席节点回透明色，配合 REQUIRE 在场断言使用）
    auto colorOf = [](eui::Ui& ui, const std::string& id) -> eui::Color {
        const auto* node = ui.find(id);
        return node == nullptr ? eui::Color{0.0f, 0.0f, 0.0f, 0.0f}
                               : node->color;
    };
    auto sameColor = [](const eui::Color& x, const eui::Color& y) {
        return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
               std::fabs(x.b - y.b) < 0.01f;
    };
    const auto& t = core::theme::current();
    // Danger 变体的底色来自传给 .theme() 的组件令牌（componentTokensWeChat），
    // 该映射未覆写 error/success/warning/info 槽——暗色下组件 error 仍是
    // ThemeColorTokens 默认 #EF4444，与 t.colors.error（暗色 #F87171）非同值。
    const auto wcTokens = core::theme::componentTokensWeChat(t);
    const eui::Color warmSurface = t.colors.surfaceElevated;

    // 开新建表单 → env 页加一行变量
    {
        eui::Ui ui;
        compose(ui, "wc_open");
        DOCTEST_CHECK(click(ui, "claude.profiles.new.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "wc_tab2");
        DOCTEST_CHECK(click(ui, "claude.form.tabs.2.hit"));
    }
    {
        eui::Ui ui;
        compose(ui, "wc_env_addrow");
        DOCTEST_CHECK(click(ui, "claude.form.env.add.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "wc_env");
        DOCTEST_REQUIRE(ui.find("claude.form.env.row.0.key.hit") != nullptr);
        // env 页（W4 批2）：键/值输入底 == 白；删除钮 = Danger Ghost 浅红底（error 20%）
        // 添加钮 = Outline 白底绿边
        DOCTEST_CHECK(sameColor(colorOf(ui, "claude.form.env.row.0.key.hit"),
                                t.colors.surface));
        DOCTEST_CHECK(sameColor(colorOf(ui, "claude.form.env.row.0.value.hit"),
                                t.colors.surface));
        DOCTEST_CHECK(sameColor(colorOf(ui, "claude.form.env.row.0.del.bg"),
                                wcTokens.error));   // S2.1 Danger 默认底 = 组件 error 20%（RGB 同组件 error）
        DOCTEST_CHECK(sameColor(colorOf(ui, "claude.form.env.add.bg"),
                                t.colors.surface)); // S2.1 Outline 默认底 = surface
        // 反向：输入面/工具钮不得命中暖橙
        DOCTEST_CHECK(!sameColor(colorOf(ui, "claude.form.env.row.0.key.hit"),
                                 warmSurface));
        DOCTEST_CHECK(!sameColor(colorOf(ui, "claude.form.env.add.bg"),
                                 warmSurface));
    }

    mod->onStop();
    return true;
})

// UAT5 米黄清理（详情页三 pane）+「未激活」徽章删除锁：环境变量行底全量
// WeChat 化；非激活 Profile 的详情不再挂「未激活」徽章（选中语义已由列表
// 绿圈白勾承担）。
REGISTER_TEST(claude_detail_wechat_and_no_inactive_badge,
              "claude_module: 详情 env 面 WeChat 化 + 未激活徽章已删", {
    TestEnv env;
    auto a = env.svc->createProfile("激活站", "https://act.example.com",
                                    "sk-ant-dt-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    // 非激活第二站：详情选中它时旧实现会挂「未激活」徽章
    module::claude::Profile b = env.svc
        ->createProfile("未激活站", "https://idle.example.com",
                        "sk-ant-dt-0002", "")
        .value();
    b.extraEnvJson = R"({"MY_DT_KEY":"v1"})";
    DOCTEST_REQUIRE(env.svc->updateProfile(b).isOk());

    auto mod = mkmod();
    eui::Screen screen{1600.0f, 1000.0f};
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
    auto sameColor = [](const eui::Color& x, const eui::Color& y) {
        return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
               std::fabs(x.b - y.b) < 0.01f;
    };
    const auto& t = core::theme::current();

    // 选中非激活站（卡 1）→ 详情跟随展示它
    {
        eui::Ui ui;
        compose(ui, "dt_sel");
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    {
        eui::Ui ui;
        compose(ui, "dt_basic");
        // 「未激活」徽章已删：节点不在（名称值在场证明详情真的画了）
        DOCTEST_CHECK(ui.find("claude.field.name.inactive") == nullptr);
        DOCTEST_CHECK(ui.find("claude.field.name.inactive.bg") == nullptr);
        DOCTEST_CHECK(ui.find("claude.field.name.value") != nullptr);
    }
    // 环境变量 pane：行底 == surface（原暖橙 surfaceElevated 行）
    {
        eui::Ui ui;
        compose(ui, "dt_tab2");
        DOCTEST_CHECK(click(ui, "claude.detail.tab.2.hit"));
    }
    {
        eui::Ui ui;
        compose(ui, "dt_env");
        const auto* rowBg = ui.find("claude.env.row.bg.0");
        DOCTEST_REQUIRE(rowBg != nullptr);
        DOCTEST_CHECK(sameColor(rowBg->color, t.colors.surface));
        DOCTEST_CHECK(!sameColor(rowBg->color, t.colors.surfaceElevated));
    }
    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.id);
    return true;
})

REGISTER_TEST(claude_ui_dialog_frame_containment,
              "claude_ui: P4/P8/P5 弹窗全元素在框内 + 标题 y=16/字号 20（防回潮）", {
    TestEnv env;
    auto mod = mkmod();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    int n = 0;
    auto frame = [&]() {
        ui.begin("claude_df" + std::to_string(n++));
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    auto click = [&](const char* id) -> bool {
        auto* b = ui.find(id);
        DOCTEST_REQUIRE(b != nullptr);
        DOCTEST_REQUIRE(b->onClick != nullptr);
        b->onClick();
        frame();
        return true;
    };
    auto check = [&](const char* panelId, const char* titleId, const char* tag) -> bool {
        std::string v = dialog_frame::allInside(ui, panelId);
        DOCTEST_CHECK_MESSAGE(v.empty(), std::string(tag) + " 全元素在框内: " + v);
        v = dialog_frame::titleAnchor(ui, panelId, titleId);
        DOCTEST_CHECK_MESSAGE(v.empty(), std::string(tag) + " 标题锚: " + v);
            return true;
    };

    // P5 前置：一条可删 Profile
    auto prof = env.svc->createProfile("框内档案", "https://frame.example.com",
                                       "sk-ant-frame-00000001", "m");
    DOCTEST_REQUIRE(prof.isOk());

    // P4 新建 Profile 表单（基础 tab）
    frame();
    DOCTEST_CHECK(click("claude.profiles.new.bg"));
    DOCTEST_CHECK(check("claude.dialog.form.panel", "claude.dialog.form.title", "P4 表单"));

    // P8 环境变量页（同 panel，切 tab 后内容面不同）
    DOCTEST_CHECK(click("claude.form.tabs.2.hit"));
    DOCTEST_CHECK(check("claude.dialog.form.panel", "claude.dialog.form.title", "P8 编辑 env"));
    click("claude.dialog.form.backdrop");   // 关表单

    // P5 删除确认（P4 族）：选中行 → 右键菜单「删除」
    DOCTEST_CHECK(click("claude.profile.0.bg"));
    {
        auto* wrap = ui.find("claude.profile.0.wrap");
        DOCTEST_REQUIRE(wrap != nullptr);
        DOCTEST_REQUIRE(wrap->onContextMenu != nullptr);
        eui::PointerEvent pe;
        eui::Rect rect;
        wrap->onContextMenu(pe, rect);
        frame();
    }
    DOCTEST_CHECK(click("claude.list.ctx.level.0.item.3"));
    DOCTEST_CHECK(check("claude.dialog.delete.panel", "claude.dialog.delete.title", "P5 删除确认"));
    click("claude.dialog.delete.backdrop");   // 取消不删

    mod->onStop();
    return true;
})

// 右键「设为当前」触发反向同步（enable 按钮已删，保留切换链路断言）
REGISTER_TEST(claude_module_ui_set_active_captures_settings,
              "claude_module: 右键设为当前触发反向同步（headless）", {
    TestEnv env;
    test_support::TempDir d("uisetactive", kImportTempDirPrefix);
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

    auto mod = mkmod();
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
        auto* wrap = ui.find("claude.profile.1.wrap");
        DOCTEST_REQUIRE(wrap != nullptr);
        DOCTEST_REQUIRE(wrap->onContextMenu != nullptr);
        eui::PointerEvent pe;
        eui::Rect rect;
        wrap->onContextMenu(pe, rect);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.list.ctx.level.0.item.4"));
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
