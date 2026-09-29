// tests/test_modules/test_claude_ui.cpp
// Claude 模块 UI 接线无头冒烟（照 T11 settings 的 compose 模式）：
//   1. 无 ClaudeService（global 为 null）时 compose 不崩、关键元素渲染
//   2. 有真实服务 + 数据时 compose 渲染 profile 卡片 / ACTIVE 徽标 / 详情 /
//      全部操作按钮（新建/启用/测试连通/编辑/删除/代理开关）
// 接线证据 = 代码路径 + 本无头 compose 测试 + 启动冒烟；真实鼠标点击留给用户人工点验
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "claude_ui_logic.h"
#include "core/database.h"
#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "core/time.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "platform/terminal_launch.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <eui/dsl.h>

#include <chrono>
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
// 审计 CPX-06 测试缝（claude_module.cpp 文件末尾同名导出）：调用记录后台查询的
// 累计派发次数（派发点即增，同帧可读）/ 最近一次实际执行查询的线程 id 哈希
// （worker 写，0 = 未跑过）。用于把「弹窗打开那一帧只发意图」「搜索防抖把逐键
// 多次意图合并成一次派发」写成计数断言
extern "C" int claude_calls_query_dispatches_for_test();
extern "C" unsigned long long claude_calls_query_last_thread_for_test();

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeModule";

// 临时 db 文件名前缀（共享工厂 test_support::TestDb 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_claude_ui_";

// RAII：临时库（共享 test_support::TestDb）+ 注册全局 ClaudeService
//（析构时一定解除注册，防悬垂污染后续用例）
// 模块 glue，keep-local（TIN-06）：TestDb 基座已是共享头，剩 ClaudeService glue。
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

// 清某个 profile 的测试密钥（共享实现，TIN-06b）
using test_support::cleanupProfileKey;

// 动态空闲端口（审计 L2 / HANDOFF 排队 #9 测试脆弱点）：借 platform::http::Server
// 的 bind 0 支持（http_server.cpp：port==0 → bind_to_any_port）向系统要一个空闲
// 端口随即释放，交还调用方独占——多 worktree / 残留进程并发跑测试时不再撞写死的
// 端口号。失败返回 0（调用方 REQUIRE 拦）。
int pickFreePort() {
    platform::http::Server probe("127.0.0.1", 0);
    if (!probe.start()) {
        return 0;
    }
    const int port = probe.actualPort();
    probe.stop();
    return port;
}

// 「导入当前配置」测试的 settings.json / sidecar 路径（共享头 test_support::TempDir，
// 前缀沿用原样；绝不碰真实 ~/.claude——settings_sync 的 Paths 注入模式）
constexpr const char* kImportTempDirPrefix = "devdesk_test_import_cur_";

module::claude::settings_sync::Paths importPaths(const test_support::TempDir& d) {
    module::claude::settings_sync::Paths p;
    p.settingsFile = d.file("settings.json");
    p.sidecarFile = d.file("snapshot.json");
    return p;
}

} // namespace

// ---- 0. 纯函数 formatCallTime（W10 复审：调用时间列跨天显示） ----
// 锚定本地正午避免跨零点抖动（仿 test_home 的 noon 模式）
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
    // 更早 → MM-DD（5 字符，中间 '-'）
    const std::string older =
        module::claude::formatCallTime(noon - 3 * 86400, noon);
    DOCTEST_CHECK_EQ(older.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(older[2], '-');
    return true;
})

// ---- 1. 无服务（global 为 null）时 compose 不崩 ----

REGISTER_TEST(claude_module_compose_null_service,
              "claude_module: compose 无服务不崩（headless）", {
    module::claude::ClaudeService::setGlobal(nullptr);   // 保险：确保干净起点
    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    ui.begin("claude_null");
    mod->compose(ui, screen);
    ui.end();

    // 页面骨架 + 关键操作按钮存在（即使无数据也应渲染，按钮带防误触提示）
    DOCTEST_CHECK(ui.find("claude.root") != nullptr);
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.enable") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.del") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.dup") != nullptr);   // 第 5 键（M5）
    // 无服务时显示错误态（W7：不再静默吞错显示假空态）
    DOCTEST_CHECK(ui.find("claude.profiles.error") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.empty") == nullptr);

    // 无选中（shown.id == 0）+ 无服务：点「复制」不崩，toast 提示先选 Profile
    {
        const auto* dupBtn = ui.find("claude.act.dup.bg");
        DOCTEST_REQUIRE(dupBtn != nullptr);
        DOCTEST_REQUIRE(dupBtn->onClick != nullptr);
        dupBtn->onClick();
        eui::Ui ui2;
        ui2.begin("claude_null2");
        mod->compose(ui2, screen);
        ui2.end();
        const auto* msg = ui2.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("请先选择 Profile") != std::string::npos);
    }

    mod->onStop();
    return true;
})

// ---- 1b. 6 键均分几何：第 6 键右缘对齐分隔线（回归闸）----
// actW = (w0 - actGap*5)/6 —— EUI 的 row spacing 只在相邻子项之间生效，n 项只有
// n−1 个间隙。旧算式扣 6 个 gap 会让第 6 键右缘比分隔线短一整个 gap（12px）。
// 跑真实 layout 后比对计算 frame，无头也能锁死这个几何。

REGISTER_TEST(claude_module_actions_row_alignment,
              "claude_module: 6 键均分——第 6 键右缘对齐分隔线（headless layout）", {
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
    const auto* run6 = ui.find("claude.act.run.bg");   // 第 6 键（profile-run）
    DOCTEST_REQUIRE(divider != nullptr);
    DOCTEST_REQUIRE(run6 != nullptr);
    const float dividerRight = divider->frame.x + divider->frame.width;
    const float runRight = run6->frame.x + run6->frame.width;
    DOCTEST_CHECK(runRight >= dividerRight - 0.5f);
    DOCTEST_CHECK(runRight <= dividerRight + 0.5f);

    mod->onStop();
    return true;
})

// ---- 2. 有真实服务 + 数据时 compose 渲染卡片/徽标/详情 ----

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

    // 两张卡片
    DOCTEST_CHECK(ui.find("claude.profile.0.name") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.1.name") != nullptr);
    // 激活卡片的 ACTIVE 徽标（profile 0 = 激活的「官方」）
    DOCTEST_CHECK(ui.find("claude.profile.0.active.text") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profile.1.active.text") == nullptr);

    // 详情面板显示激活 profile 的真实值
    const auto* nameV = ui.find("claude.field.name.value");
    DOCTEST_REQUIRE(nameV != nullptr);
    DOCTEST_CHECK_EQ(nameV->text, std::string("官方"));
    const auto* urlV = ui.find("claude.field.url.value");
    DOCTEST_REQUIRE(urlV != nullptr);
    DOCTEST_CHECK_EQ(urlV->text, std::string("https://api.anthropic.com"));

    // 操作按钮 + 代理开关存在
    DOCTEST_CHECK(ui.find("claude.act.enable") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.del") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.dup") != nullptr);   // 第 5 键（M5）
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- 3. 测试连通不阻塞 UI 线程（后台线程 + 结果回 compose 收割） ----
// 回归：此前 UI 线程同步调 testConnection（timeout 15s），慢上游时
// 点「测试连通」整个界面冻结。现在点击立即返回 + toast「测试中」，
// 上游返回后由 compose 每帧收割结果弹 toast。
REGISTER_TEST(claude_module_test_connection_async,
              "claude_module: 测试连通后台执行不冻结 UI（headless）", {
    TestEnv env;
    // mock 上游挂 500ms 再回 200：点击必须立即返回（不等上游）
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("GET", "/v1/models",
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

    // 点「测试连通」→ 立即返回，toast 切到「测试中」
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

    // 轮询 compose（每帧收割）直到上游 500ms 响应被收割成结果 toast
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

// ---- 4. Profile 表单 tabs：槽位/env 填写 → 保存 → DB 往返 → 重开回显 ----
// （provider-config Phase D，§6.1；输入经 input.hit 的 onTextInput 驱动，
//   等价于真实键盘输入路径——insertAtCursor + onChange 回模块状态）
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
    // CHECK 宏内含 return false，不能进 void lambda —— helper 返回 bool，
    // 由测试体的 DOCTEST_CHECK 收敛（失败即 fail-fast 本用例）
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

    // 选中卡片（无激活 profile 时详情面板需要 selectedId 才有编辑目标）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    // 打开编辑弹窗：基础页默认，tabs 存在，名称回显
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.1"));   // → 模型映射页
    }
    // 模型映射页：6 行槽位输入；默认槽位空 → name placeholder 是「自动派生」
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));   // → 高级 env 页
    }
    // 高级 env 页：添加一行键值后保存
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
    // 详情面板模型映射 pane（2026-09-24 页签式）：切模型 tab，默认槽位含模型
    {
        {
            eui::Ui ui;
            compose(ui);
            DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.1"));  // 切到模型 tab（tabs 组件 hit 区）
        }
        eui::Ui ui;
        compose(ui);
        const auto* slot0 = ui.find("claude.slot.val.0");  // 默认槽位
        DOCTEST_REQUIRE(slot0 != nullptr);
        DOCTEST_CHECK(slot0->text.find("fangzhou/glm-5.3[1M]") !=
                      std::string::npos);
    }
    // DB 往返：models_config / extra_env / 旧 model 列同步
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
    // 重开编辑 → 槽位 / env 回显
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

// ---- 5. cc-switch JSON 粘贴导入：解析预览 → 应用到表单 → 保存 ----
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
    // CHECK 宏内含 return false，不能进 void lambda —— helper 返回 bool，
    // 由测试体的 DOCTEST_CHECK 收敛（失败即 fail-fast 本用例）
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
    // 用户模板（BASE_URL/AUTH_TOKEN/模型键/别名键/非官方键）
    std::string tpl = R"({"env":{)";
    tpl += R"("ANTHROPIC_BASE_URL":"https://gw.example.com/a",)";
    tpl += R"("ANTHROPIC_AUTH_TOKEN":"sk-import-tok",)";
    tpl += R"("ANTHROPIC_MODEL":"m-default[1M]",)";
    tpl += R"("ANTHROPIC_DEFAULT_SONNET_MODEL":"m-sonnet[1M]",)";
    tpl += R"("AUTO_COMPACT_ENABLED":"0",)";
    tpl += R"("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE":"75",)";
    tpl += R"("DISABLE_AUTOUPDATER":"1")";
    tpl += "}}";

    // 新建表单 → 填名称 → 高级 env 页粘贴导入
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
    // 解析预览：槽位 2/<槽位表行数> + 别名 1 + 未识别 1；别名提示行存在
    {
        eui::Ui ui;
        compose(ui);
        const auto* preview = ui.find("claude.form.import.preview");
        DOCTEST_REQUIRE(preview != nullptr);
        // 分母从槽位表派生（CPX-08）：表加一行 → UI 预览分母自动跟随，
        // 此处不再钉死 6（否则新增槽位就让本断言红）
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
    // 应用后基础页回填 BASE_URL，确认创建（API Key 来自导入的 AUTH_TOKEN）
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
    // DB 断言：base_url / 槽位 / env 别名归一 + 透传 / key 入库
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
        const auto& p = list.value()[0];
        DOCTEST_CHECK_EQ(p.name, std::string("导入测试"));
        DOCTEST_CHECK_EQ(p.baseUrl, std::string("https://gw.example.com/a"));
        DOCTEST_CHECK(!p.apiKeyHint.empty());   // 导入 token 已入 Keychain
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

// ---- 6. 「导入当前配置」按钮存在 + 读 settings.json → 弹窗预填全链 ----
// 点按钮 = 等价用户手动「粘贴导入」但全自动：临时 settings.json（Paths 注入）
// → 弹窗打开 → 基础页名称建议/BASE_URL/Token、模型映射页槽位、高级 env 页
// env 行 + 预览标记 → 保存 → DB 往返。
REGISTER_TEST(claude_module_import_current_full_chain,
              "claude_module: 导入当前配置读settings.json预填表单（headless）", {
    TestEnv env;
    test_support::TempDir d("full", kImportTempDirPrefix);
    const auto paths = importPaths(d);
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

    // 按钮存在（profile 列表底部，与「+ 新建 Profile」并列）→ 点击
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.profiles.import") != nullptr);
        DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.profiles.import.bg"));
    }
    // 弹窗已开 + 基础页预填（名称建议取 BASE_URL host）
    {
        eui::Ui ui;
        compose(ui);
        const auto* backdrop = ui.find("claude.dialog.form.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(!backdrop->disabled);   // 弹窗开着
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.name.text"),
                         std::string("导入 gw.example.com"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.url.text"),
                         std::string("https://gw.example.com/v1"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.key.text"),
                         std::string("sk-imp-current-0001"));
        // 非接管态（无 sidecar）→ 无警告行
        DOCTEST_CHECK(ui.find("claude.dialog.form.importwarn") == nullptr);
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.1"));
    }
    // 模型映射页：default / sonnet 槽位预填
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.slot.0.model.text"),
                         std::string("m-cur-default"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.slot.1.model.text"),
                         std::string("m-cur-sonnet"));
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    // 高级 env 页：env 行预填 + 预览标记保留（识别槽位 2/<槽位表行数> · env 1 项）
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
    // 保存 → DB 往返（名称/Base URL/槽位/env 全量落地）
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
        const auto& p = list.value()[0];
        DOCTEST_CHECK_EQ(p.name, std::string("导入 gw.example.com"));
        DOCTEST_CHECK_EQ(p.baseUrl, std::string("https://gw.example.com/v1"));
        DOCTEST_CHECK(!p.apiKeyHint.empty());   // AUTH_TOKEN 已入 Keychain
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

// ---- 7. 「导入当前配置」文件缺失 → toast 提示，不弹窗 ----
REGISTER_TEST(claude_module_import_current_missing_file,
              "claude_module: 导入当前配置文件缺失toast不弹窗（headless）", {
    TestEnv env;
    test_support::TempDir d("miss", kImportTempDirPrefix);
    const auto paths = importPaths(d);   // settings.json 不创建
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
    // 把整段当裸 env 对象报「键名非法」的误报）
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

// ---- 8. 「导入当前配置」接管态 → 弹窗内警告 + 仍预填（不阻断）----
// sidecar 快照存在 = 接管态：导入的是代理托管值（BASE_URL=127.0.0.1:8888）
// 而非原始上游 → 警告行展示，弹窗照开照预填，由用户决定。
REGISTER_TEST(claude_module_import_current_takeover_warn,
              "claude_module: 导入当前配置接管态警告不阻断（headless）", {
    TestEnv env;
    test_support::TempDir d("takeover", kImportTempDirPrefix);
    const auto paths = importPaths(d);
    env.svc->setSettingsSyncPaths(paths);
    env.svc->setSettingsTakeOverEnabled(true);
    // 模拟接管态：settings.json 是代理托管值 + sidecar 快照残留
    d.write(d.file("settings.json"), R"({
        "env": {
            "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
            "ANTHROPIC_AUTH_TOKEN": "devdesk-proxy-token",
            "MY_KEY": "user"
        }
    })");
    d.write(d.file("snapshot.json"),
            R"({"env": {"ANTHROPIC_BASE_URL": "https://orig.example.com"}})");

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
        // 不阻断：弹窗照开、照预填（导入的就是接管态值）
        const auto* backdrop = ui.find("claude.dialog.form.backdrop");
        DOCTEST_REQUIRE(backdrop != nullptr);
        DOCTEST_CHECK(!backdrop->disabled);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.form.url.text"),
                         std::string("http://127.0.0.1:8888"));
        // 接管态警告行：提示导入的是代理配置而非原始上游 + 建议先停代理
        const auto* warn = ui.find("claude.dialog.form.importwarn");
        DOCTEST_REQUIRE(warn != nullptr);
        DOCTEST_CHECK(warn->text.find("接管态") != std::string::npos);
        DOCTEST_CHECK(warn->text.find("代理配置") != std::string::npos);
        DOCTEST_CHECK(warn->text.find("停止代理") != std::string::npos);
        // MY_KEY 透传进 env 行（未识别键原样保留）
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

// ---- 9. 调用记录弹窗：搜索 / 状态码过滤 / 翻页（产品审计 2026-09-23 [11]） ----
// spec §8.3 第 6 条「完整日志面板（支持过滤、搜索）」：25 条日志 → 弹窗每页
// 20 条 + 搜索框（path/method 模糊）+ 状态码下拉 + 上下页；过滤条件跨开关保留
REGISTER_TEST(claude_module_calls_dialog,
              "claude_module: 调用记录弹窗搜索/状态码过滤/翻页（headless）", {
    TestEnv env;
    auto p = env.svc->createProfile("日志弹窗", "https://l.example.com",
                                    "sk-ant-logdlg-01", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    const std::time_t base = std::time(nullptr);

    // 20×200 messages + 3×429 complete + 2×超时（status 0 带 error）
    // 注：DOCTEST 宏在 lambda 内 return false → 显式 bool 返回，调用点传播
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
    // 审计 CPX-06：取数已后台化（开弹窗 / 改过滤 / 翻页后结果下一帧起才可见），
    // 断言前先有界轮询——每轮新起一帧 compose（帧内收割后台结果），不靠固定睡眠
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
    auto waitRows = [&](int n) -> bool {   // 当页恰有 n 行（新页到位的判据）
        return waitFrame([&](eui::Ui& ui) {
            return (n == 0 || ui.find("claude.dialog.calls.row." +
                                       std::to_string(n - 1) + ".wrap") != nullptr) &&
                   ui.find("claude.dialog.calls.row." + std::to_string(n) +
                           ".wrap") == nullptr;
        });
    };

    // 1. 点「查看全部 →」开弹窗（CPX-06：渲染路径不再同步查库，数据由后台取。
    //    2026-09-28 调用记录已抽成详情面板第 4 页签：先切 tab 该帧，再点
    //    「查看全部」开弹窗）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.calls.viewall.hit"));
    }
    // 2. 弹窗数据到齐（等后台结果）：搜索框 / 状态下拉 / scrollView / 首页 20 行
    //    / 页码 / 总数 / 超时行错误摘要
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
        // 超时行（status 0）行尾附错误摘要
        DOCTEST_CHECK(textOf(ui, "claude.dialog.calls.row.0").find("/v1/hold") !=
                      std::string::npos);
        DOCTEST_CHECK(textOf(ui, "claude.dialog.calls.row.0").find("connection timeout") !=
                      std::string::npos);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("25 条匹配"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.page"),
                         std::string("第 1 / 2 页"));
        // disabled/onClick 都挂在按钮的 .bg rect 上（button build 结构）
        DOCTEST_CHECK(ui.find("claude.dialog.calls.prev.bg")->disabled);
    }
    // 3. 下一页：剩 5 行，下一页置灰（翻页后等新页到位）
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
    // 4. 上一页回第一页
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
    // 5. 搜索框过滤（path 子串）：3 条命中，单页
    //（CPX-06：按键 → 防抖窗口满后一次后台查询 → 等结果再断言）
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
    // 7. 切回全部状态 → 恢复 3 条
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
    // 8. 关闭再打开：过滤条件保留（会话内记忆），页码回第一页
    //（dialog 关闭只是收起，content 节点树仍在，不以此断言开关）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.close.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));  // tab 保持 3（幂等）
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

// ---- 审计 CPX-06：调用记录弹窗取数后台化 + 搜索防抖（渲染路径零 DB）----
// 回归：原实现开弹窗 / 搜索框逐键都在回调里同步查库（queryCalls 分页行 +
// countCalls 总数两条 SQL），代理 worker 持同一把 SQLite 锁时开弹窗/打字会卡帧。
// H2 立的「渲染路径不做 IO/DB」硬约束此前只落了 compose 的 profiles/recent/
// active 三处，弹窗侧全靠注释维持——本用例把它锁成可回归的行为：
//   1. 弹窗关着 compose 多帧零查询；
//   2. 开弹窗那一帧只发意图：数据未到（加载态）、恰好派发一次；查询由 worker
//      线程跑完才回填（渲染线程同步查库会在线程 id 上现形）；
//   3. 空闲帧不重复派发；数据由后台结果回填（下一帧起可见）；
//   4. 搜索框同一防抖窗口内 3 次按键只派发一次查询，停手后按最终文本出结果。
// 查询次数走 claude_module.cpp 的测试缝计数（不靠墙钟 / 不靠固定睡眠）
REGISTER_TEST(claude_cpx06_calls_query_async_debounce,
              "claude_module: 调用记录取数后台化 + 搜索防抖单次派发（CPX-06）", {
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

    {   // 开弹窗帧：调用记录在详情面板第 4 页签里，先切 tab（同帧不派发查询）
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(click(ui, "claude.detail.tabs.hit.3"));
    }
    // ② 开弹窗那一帧：只发意图（查询在后台）——数据未到 + 恰好派发一次
    //   （同步查库会让本帧就把行画出来，且派发计数按每次点击 +1 而非 +1/帧）
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
    {   // 防抖窗口内的一帧：未派发，快照沿用上次值（不闪空/不逐键查）
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q1);
        DOCTEST_CHECK_EQ(countText(ui), std::string("4 条匹配"));
    }
    {   // 停手满一个窗口 → 一次查询按最终文本 "com" 出结果
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
    for (int i = 0; i < 5; ++i) {   // 收敛后仍零重查
        eui::Ui ui;
        compose(ui);
    }
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q1 + 1);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 10. Profile 列表 scrollView（产品审计 2026-09-23 [26]） ----
// 此前卡片固定 y=56+i*108 平铺：8 个 profile 时第 7 张起画出左栏、叠底部按钮，
// 最小窗口高度 720 下第 4 张即溢出。现在整列表进 scrollView，超界可滚、全可达
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
    // 最小高度 720：末卡滚到底可达且可点选（详情面板跟随第 8 个 profile）。
    // 审计修复后视口让出共享区块高（~76px）→ 720 高下 7×108=756 的末卡起点
    // 在视口外：virtualList 只渲染可见行 + overscan，先滚到底（钳到 maxOffset）
    // 再断言树内 + 可点选
    {
        eui::Ui ui;
        composeAt(ui, 720.0f);
        DOCTEST_CHECK(ui.find("claude.profiles.scroll") != nullptr);
        const auto* root = ui.find("claude.profiles.scroll");
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_REQUIRE(root->onScrollOffsetChanged != nullptr);
        root->onScrollOffsetChanged(2000.0f);  // 超界值 → 内部钳到 maxOffset
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

// ---- 跨模块剩余审计修：共享配置区块必须整体位于 virtualList 视口 clip 之外 ----
// 修复前视口按 scrollH 全高、区块只压自己的绝对 Y，后建区块叠在带 clip 的
// 视口底带上 → ≥5 profile 时末 1-2 张卡被盖、滚到底显示不全、点击误开片段
// 编辑。现在视口高度 = listH（让出区块高 + 12px），区块钉在 clip 之外。
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

    // 三种高度（默认 / 最小窗 / 超短窗）：区块顶 ≥ 视口底 12px（间距），
    // 区块落在带 clip 的视口之外——不盖末卡、不误点片段；
    // 720 以上窗口区块底 == 底部按钮行上沿 − 12（**钉底**，新建/导入同排同 y）。
    // 500 属极端矮窗（低于最小窗），P4 起区块仍钉底、不再向下溢出按钮区
    //（审计 F-10a：本注释此前写「可向下溢出按钮区」，是 clamp 让位时代的旧口径）；
    // 该档下区块会被压扁到自然高以下（LOG_ERROR 记录），本用例只锁锚点关系
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
              "claude_module: 表单基础页/模型页输入框 Esc=取消不落库（headless，审计 [21]）", {
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

    // CHECK 宏内含 return false，不能进 void lambda —— helper 返回 bool，
    // 由测试体的 DOCTEST_CHECK 收敛（本文件既有约定）
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
        DOCTEST_CHECK(hit->onKeyEvent(esc));   // onCancel 消费（旧行为：Esc 无响应）
        eui::Ui ui2;   // 新帧确认弹窗关闭（每帧新建 Ui，旧指针失效）
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

// ---- 产品审计 [22]：Claude 页级快捷键 Ctrl+N / Ctrl+S / Ctrl+Enter（headless）----
// 通道 = Module::onKeyEvent（模拟 app 级 onKeyEvent → Router 派发到当前页
// 模块，与 todo_ui_keyboard_move 同款直调）。端口动态取空闲端口避让冲突。
REGISTER_TEST(claude_module_keyboard_shortcuts,
              "claude: Ctrl+N 新建 / Ctrl+S 保存表单 / Ctrl+Enter 启停代理（审计 [22]）", {
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
    // 快捷键修饰位按平台取（macOS = Cmd，其余 = Ctrl；见 setShortcutModifier）
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
    // 2) 表单开着再按 Ctrl+N → 不消费（不得重置填写中的草稿）；
    //    Ctrl+Enter 表单开着 → 不消费（不得打字中途启停代理）
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
    // 4) 表单外 Ctrl+S（无保存对象）、未注册组合、裸键、Alt 修饰、Release
    //    全部不消费，且不得开表单
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

    // 5) Ctrl+Enter 启停：无弹窗开着 → 按一次启动（端口 = configuredProxyPort）、
    //    再按一次停止。（原「token 文件写进重定向的临时 APPDATA」一项随代理
    //    鉴权移除，2026-09-27——启动代理不再向用户目录写任何凭据文件）
    //    端口动态取（原写死 28888：多实例/残留进程会撞端口）——取系统空闲端口，
    //    万一在探测与 bind 之间被抢则换端口重试 ≤3 次；断言语义不变
    //    （启动后监听在配置的端口上）
    int port = 0;
    bool started = false;
    for (int attempt = 0; attempt < 3 && !started; ++attempt) {
        port = pickFreePort();
        DOCTEST_REQUIRE(port > 0);
        env.svc->setConfiguredProxyPort(port);
        DOCTEST_CHECK(mod->onKeyEvent(ctrlPress(eui::InputKey::Enter)));
        started = env.svc->isProxyRunning();
    }
    DOCTEST_CHECK(started);
    DOCTEST_CHECK_EQ(env.svc->proxyPort(), port);
    DOCTEST_CHECK(mod->onKeyEvent(ctrlPress(eui::InputKey::Enter)));
    DOCTEST_CHECK(!env.svc->isProxyRunning());

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 11. 审计 perf [13]：compose 数据缓存等价 ----
// 优化前 compose 每帧同步跑 listProfiles/recentCalls(3)/getActiveProfile 三条
// 查询；优化后缓存命中帧零查询。本用例锁等价契约：
//   1. 首帧（脏）与次帧（缓存命中）渲染结果逐字段一致
//   2. 代理 worker 的 logCall（不经过模块、不标脏）经 callLogVersion 驱动
//      recent 重查 —— 「新调用下一帧可见」不变
//   3. 外部直写 DB 绕过模块：缓存帧不可见（新语义，TodoModule 同款），
//      onPageEnter 后可见
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
    // B 激活 → 列表序（is_active DESC, name ASC）= [B, A]
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
    // 审计 H2：DB 查询挪到后台线程 → 「变更可见」最多晚 1 帧（本帧发刷新
    // 意图、下帧收割）。以下三处原为「一次 compose 即断言」，改为有界轮询
    // 等可见——契约不变（脏标记 / callLogVersion / onPageEnter 各自仍驱动重查）
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
    // 调用记录已抽成详情面板第 4 页签（2026-09-28）：切过去断空态
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.calls.empty") != nullptr);
    }

    // 2) 代理 worker 落库（logCall 直写，不经过模块）→ callLogVersion 驱动重查
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

    // 3) 外部直写绕过模块：缓存帧不可见（新语义），onPageEnter 后可见
    auto c = env.svc->createProfile("C缓存", "https://c.example.com",
                                    "sk-ant-cache-cccc", "m");
    DOCTEST_REQUIRE(c.isOk());
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.profile.2.name") == nullptr);
    }
    mod->onPageEnter();
    // 脏标记 → 本帧发刷新意图、下帧收割（H2 异步）：有界轮询等到 C 可见
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

    // 4) 模块写操作（选中 A → 启用）标脏：ACTIVE 徽标移到 A，详情跟随
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));   // 选中「A缓存」
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.act.enable.bg"));  // doSetActive(A)
    }
    // 启用写操作标脏 → 后台重查、下帧收割（H2 异步）：有界轮询等 A 排到 index 0
    DOCTEST_REQUIRE(composeUntil([](eui::Ui& ui) {
        const auto* n0 = ui.find("claude.profile.0.name");
        return n0 != nullptr && n0->text == std::string("A缓存");
    }));
    {
        eui::Ui ui;
        compose(ui);
        // A 激活后排到 index 0，B 不再是 ACTIVE
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

// ---- 审计 M5：Esc 关最上层弹窗（删除确认无输入框）----
// 表单弹窗的输入框聚焦时 Esc 走 input.onCancel（既有链路，天然让位）；
// 确认删除弹窗没有输入框、Esc 修复前无任何响应 → 模块级兜底补上。
REGISTER_TEST(claude_ui_esc_closes_delete_confirm,
              "claude_ui: Esc 关删除确认弹窗（无输入框，审计 M5）", {
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

    // 0) 无弹窗：不消费（兜底不越权吃键）
    DOCTEST_CHECK(!mod->onKeyEvent(escPress()));

    // 1) 选中 profile → 「删除」→ 确认弹窗（无输入框）→ Esc = 取消，profile 不删
    frame("esc1");
    DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));   // 详情面板需要选中项
    frame("esc2");
    DOCTEST_CHECK(click(ui, "claude.act.del.bg"));
    frame("esc3");
    DOCTEST_REQUIRE(ui.find("claude.dialog.delete.panel") != nullptr);
    DOCTEST_CHECK_EQ(ui.find("claude.dialog.delete.panel")->opacity, 1.0f);
    DOCTEST_CHECK(mod->onKeyEvent(escPress()));
    frame("esc4");
    DOCTEST_CHECK_EQ(ui.find("claude.dialog.delete.panel")->opacity, 0.0f);
    {
        auto listed = env.svc->listProfiles();
        DOCTEST_REQUIRE(listed.isOk());
        DOCTEST_CHECK_EQ(listed.value().size(), static_cast<size_t>(1));  // 未删
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 审计 H2：compose 只读快照 + DB 查询在后台线程 ----
// 回归：此前 refreshDataCache 在 compose 里同步跑 listProfiles /
// getActiveProfile / recentCalls，与代理 worker 的 logCall INSERT 争同一把
// recursive_mutex（事务全程持锁，core/sqlite_db.cpp）→ 代理忙时整帧被卡。
// 现在渲染路径只发刷新意图 + 搬已就绪结果，查询在后台线程（std::async）。
REGISTER_TEST(claude_h2_compose_snapshot_async,
              "claude_module: compose 只读快照 DB 后台刷新（H2 tick 前旧值/后新值）", {
    TestEnv env;
    auto a = env.svc->createProfile("官方", "https://api.anthropic.com",
                                    "sk-ant-aaaa-1111", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(a.isOk());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();   // 首次装载在启动期同步做（非渲染路径）
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 探针：跑一帧 compose，返回渲染出的全部 profile 卡名（列表序 =
    // is_active DESC, name ASC → 断言按名字集合，不绑 index）
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

    // ② 外部新增一条（不经模块写路径）+ 模拟进页 → 置脏（= 发刷新意图）
    auto b = env.svc->createProfile("中转", "https://relay.example.com",
                                    "sk-ant-bbbb-2222", "claude-opus-4-5");
    DOCTEST_REQUIRE(b.isOk());
    mod->onPageEnter();
    {
        // tick 前：本帧才发出刷新意图（查询本帧上路），快照恒为上次值。
        // 同一次 compose 内断言 → 不依赖时序
        const auto names = cardNames();
        DOCTEST_CHECK(hasName(names, "官方"));    // 非空 + = 上次值
        DOCTEST_CHECK(!hasName(names, "中转"));   // 新值未到（确定，非时序）
    }

    // ③ tick 后：后台结果被收割 → 新值可见（有界轮询，不依赖固定睡眠）
    bool updated = false;
    for (int i = 0; i < 200 && !updated; ++i) {
        updated = hasName(cardNames(), "中转");
        if (!updated) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    DOCTEST_CHECK(updated);
    DOCTEST_CHECK(hasName(cardNames(), "官方"));   // 旧卡不丢

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, a.value().id);
    cleanupProfileKey(kTestKeychainService, b.value().id);
    return true;
})

// ---- M5（复制 UI）：详情页第 5 键「复制」→ 建副本、不激活、选中态不变 ----
// 设计 §4.1/§4.4；服务层 duplicateProfile 由 M4 落地，这里只验 UI 接线。
// 卡片列表走后台刷新（H2）→ 新卡片用有界轮询；toast / 选中态同帧即定。
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

    // 第 5 键存在且可点（.bg 点击节点，与同区其他键同款）；列表序 =
    // is_active DESC, name ASC → index 1 =「中转」，选中它
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.act.dup") != nullptr);
        DOCTEST_CHECK(ui.find("claude.act.dup.bg") != nullptr);
        DOCTEST_CHECK(click(ui, "claude.profile.1.bg"));
    }
    // 详情面板跟随选中项 → 复制源就是它（shown）
    {
        eui::Ui ui;
        compose(ui);
        const auto* nameV = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameV != nullptr);
        DOCTEST_CHECK_EQ(nameV->text, std::string("中转"));
        DOCTEST_CHECK(click(ui, "claude.act.dup.bg"));
    }
    // 成功 toast（同帧可见）：副本名 + 明示未激活
    {
        eui::Ui ui;
        compose(ui);
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        DOCTEST_CHECK(msg->text.find("中转 副本") != std::string::npos);
        DOCTEST_CHECK(msg->text.find("未激活") != std::string::npos);
        // 选中态未变：详情仍显示「中转」，未跳去副本
        const auto* nameV = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameV != nullptr);
        DOCTEST_CHECK_EQ(nameV->text, std::string("中转"));
    }
    // DB 层对账：3 条；副本 key 同源、is_active 未变、源行未动
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
        DOCTEST_CHECK(!got.value().isActive);   // 副本不激活（S-4）
        auto key = env.svc->loadApiKey(copyId);
        DOCTEST_REQUIRE(key.isOk());
        DOCTEST_CHECK_EQ(key.value(), std::string("sk-ant-bbbb-2222"));
        auto gotA = env.svc->getProfile(a.value().id);
        DOCTEST_REQUIRE(gotA.isOk());
        DOCTEST_CHECK(gotA.value().isActive);   // 原激活未被副本顶掉
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

// ---- M5 验收（spec §5 无头 UI 行）：选中卡 → 点启用 → settings.json 的值
// 收编进原激活 profile（M3 反向同步链路的 UI 侧证据）----
REGISTER_TEST(claude_module_ui_enable_captures_settings,
              "claude_module: 详情页启用触发反向同步（headless）", {
    TestEnv env;
    test_support::TempDir d("uienable", kImportTempDirPrefix);
    env.svc->setSettingsSyncPaths(importPaths(d));
    // Claude Code 侧（或 cc-switch）改过的真实上游值：非守卫键 → 会被收编
    d.write(d.file("settings.json"), R"({"env": {
        "ANTHROPIC_BASE_URL": "https://new.example.com/",
        "ANTHROPIC_AUTH_TOKEN": "sk-new-upstream-9876543210"
    }})");

    auto a = env.svc->createProfile("源A", "https://old.example.com",
                                    "sk-old-key-0123456789abcdef", "");
    DOCTEST_REQUIRE(a.isOk());
    // 首次激活：无激活 profile → G0 跳过（不读文件）
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
    // 点启用 → setActiveProfile(B)：先反向收编 A，再切激活
    {
        eui::Ui ui;
        compose(ui);
        const auto* nameV = ui.find("claude.field.name.value");
        DOCTEST_REQUIRE(nameV != nullptr);
        DOCTEST_CHECK_EQ(nameV->text, std::string("目标B"));
        DOCTEST_CHECK(click(ui, "claude.act.enable.bg"));
    }
    // 断言：settings.json 的值已进原 profile（A）
    {
        auto gotA = env.svc->getProfile(a.value().id);
        DOCTEST_REQUIRE(gotA.isOk());
        DOCTEST_CHECK_EQ(gotA.value().baseUrl,
                         std::string("https://new.example.com"));
        auto keyA = env.svc->loadApiKey(a.value().id);
        DOCTEST_REQUIRE(keyA.isOk());
        DOCTEST_CHECK_EQ(keyA.value(),
                         std::string("sk-new-upstream-9876543210"));
        // B 正常激活且未被误写；A 不再激活
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

// ---- 审计 CPX-02（UI 半边）：接管失败/上层覆盖警告在 Claude 页详情可见 ----
// 服务侧已把失败原因与覆盖警告并入 ClaudeService::lastSyncWarnings_；此前详情页
// 只在「已接管」分支渲染警告，接管失败（无 sidecar → 未接管）时用户只看到
// 「未接管」而看不到原因。空警告时不显示（不留空占位）。
REGISTER_TEST(claude_module_takeover_warn_visible,
              "claude_module: 接管失败警告在详情页可见（headless）", {
    TestEnv env;
    test_support::TempDir d("warnfail", kImportTempDirPrefix);
    env.svc->setSettingsSyncPaths(importPaths(d));
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

    // 代理未启动、从未接管 → 无警告：警告元素不存在（空时不显示）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.field.takeover.warn") == nullptr);
        DOCTEST_CHECK(ui.find("claude.field.takeover.warn.icon") == nullptr);
    }

    // 起代理 → 接管失败（不阻断起代理），原因并入 lastSyncWarnings_
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    DOCTEST_REQUIRE(env.svc->startProxy("127.0.0.1", port).isOk());
    DOCTEST_CHECK(!env.svc->lastSyncWarnings().empty());
    DOCTEST_CHECK(!env.svc->isClaudeCodeManaged());   // 失败 → 未接管

    // 无需额外刷新调用：下一次 compose（进页面/启停代理后的首帧）即见警告，
    // 文案含失败原因；状态行仍如实显示「未接管」
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

// 上层覆盖警告（接管成功路径）：多条 → 摘要 + 条数（本地注入两层 overlay 文件）
REGISTER_TEST(claude_module_takeover_warn_summary,
              "claude_module: 上层覆盖警告条数+摘要（headless）", {
    TestEnv env;
    test_support::TempDir d("warnover", kImportTempDirPrefix);
    auto paths = importPaths(d);
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

// ---- 认证字段下拉（鉴权头三态 UI；2026-09-27 profile-auth-field 设计 §6）----

REGISTER_TEST(claude_ui_form_auth_field_dropdown_exists,
              "claude_module: 表单基础页认证字段下拉渲染（无服务不崩）", {
    module::claude::ClaudeService::setGlobal(nullptr);   // 保险：确保干净起点
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
        // 默认选中第 0 项（双发）
        const auto* label = ui.find("claude.dialog.form.authfield.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("双发（默认）"));
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

    // 新建表单 → 填名称 / URL / Key
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
    // 下拉：打开 → 选 index 2（只发 x-api-key）
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
    // 选中回显 → 保存
    {
        eui::Ui ui;
        compose(ui, "authfield_c5");
        const auto* label = ui.find("claude.dialog.form.authfield.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("只发 x-api-key"));
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    // DB 断言：新建路径把表单选择传进了 createProfile（落库 = api_key）
    {
        auto list = env.svc->listProfiles();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), 1u);
        DOCTEST_CHECK_EQ(list.value()[0].name, std::string("认证字段建档"));
        DOCTEST_CHECK_EQ(list.value()[0].authField, std::string("api_key"));
        // 首建自动激活 → 代理转发主路径（getActiveProfile）也必须读到
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

// ---- 27f. failover 备用下拉（批 3，设计 §3.4/§3.7）：编辑回显 + 选择保存落库
//      + 自引用排除（编辑态列表里没有自己）----

REGISTER_TEST(claude_ui_fallback_dropdown_edit_persists,
              "claude_module: failover 备用 Profile 下拉（回显/保存落库/自引用排除，headless）", {
    TestEnv env;
    // 备家先建（名字「备家」<「主家」也随 is_active DESC 排后；编辑主家时
    // 候选 = 全部排除自身 → 只剩备家 = item.1）
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

    // 选中主家 → 编辑（基础页默认）：初始「无（不启用 failover）」
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
    {
        eui::Ui ui;
        compose(ui, "fb_e3");
        const auto* label = ui.find("claude.form.basic.fallback.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("无（不启用 failover）"));
    }
    // 拉开 → 自引用排除：候选只有「备家」（item.0=无 / item.1=备家；
    // 「主家」不出现 = item.2 不存在）
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
    // 选中回显 → 保存 → 落库（编辑路径写 p.fallbackProfileId）
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
    // 重开编辑：下拉回显「备家」（等后台刷新把新值带进缓存——打开表单的
    // profile 取自缓存快照，与真实应用一致）
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

// ---- 运行第 6 键（2026-09-28 profile-run 设计 §6/§8；D-1 直连形态）----
// ⚠️ 本文件用真实 ClaudeService（setGlobal）：点「运行」前**必须**注入假 launch，
//    否则测试会真的启动 wt/powershell + claude（spec §6 第 4 项）
REGISTER_TEST(claude_module_run_button_headless,
              "claude_module: 「运行」按钮存在 + 无选中 toast + 假 launch 收到 --settings 参数", {
    TestEnv env;
    test_support::DataRootEnv root(kDbPrefix, "run");
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
    // 审计 H2：DB 查询在后台线程 + 200ms 节流 → 「变更可见」最多晚一帧，
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

    // 第 6 键存在且可点（无 profile 时：无选中 → toast「请先选择 Profile」，不启动）
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

// ============ 批 2（设计 §2.10）：共享配置片段 编辑器 UI ============

// ---- 纯函数：片段 K 键集（别名归一）/ 键名归一 / 按 id 查找 / 键集判定 ----
REGISTER_TEST(claude_ui_logic_shared_pure_functions,
              "claude_ui_logic: 片段冻结键集/键名归一/查找（批 2 纯函数）", {
    using module::claude::findSharedById;
    using module::claude::keyIn;
    using module::claude::normalizedEnvKey;
    using module::claude::sharedFrozenKeys;

    // 空/非法 JSON → 空集（读路径容错）
    module::claude::SharedConfig empty;
    DOCTEST_CHECK(sharedFrozenKeys(empty).empty());
    module::claude::SharedConfig bad;
    bad.extraEnvJson = "{ 不是 JSON";
    DOCTEST_CHECK(sharedFrozenKeys(bad).empty());

    // 别名归一：AUTO_COMPACT_ENABLED → DISABLE_AUTO_COMPACT（与写侧同口径）
    module::claude::SharedConfig sc;
    sc.extraEnvJson = R"({"MY_A":"1","AUTO_COMPACT_ENABLED":"0"})";
    const auto keys = sharedFrozenKeys(sc);
    DOCTEST_REQUIRE_EQ(keys.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(keyIn(keys, "MY_A"));
    DOCTEST_CHECK(keyIn(keys, "DISABLE_AUTO_COMPACT"));
    DOCTEST_CHECK(!keyIn(keys, "AUTO_COMPACT_ENABLED"));

    // 键名归一：别名命中 → 官方名；未命中 → 原名；空串 → 空串
    DOCTEST_CHECK_EQ(normalizedEnvKey("AUTO_COMPACT_ENABLED"),
                     std::string("DISABLE_AUTO_COMPACT"));
    DOCTEST_CHECK_EQ(normalizedEnvKey("MY_X"), std::string("MY_X"));
    DOCTEST_CHECK_EQ(normalizedEnvKey(""), std::string(""));

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

// ---- 1. 左栏「共享配置」区块：标题 / 片段卡片（name + N 个键）/ 新建按钮；
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
        // 键数 = K 键集大小（片段B 2 键）
        DOCTEST_CHECK(textOf(ui, "claude.shared.card.1.keys").find("2 个键") !=
                      std::string::npos);
    }
    // 点片段B 卡片 → 编辑弹窗打开、name/env 回显（parseExtraEnv 出 map 按
    // 键名排序：MY_B 在前）
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
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.shared.env.row.0.key.text"),
                         std::string("MY_B"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.shared.env.row.1.key.text"),
                         std::string("MY_C"));
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

// ---- 2. 新建片段弹窗 → 填名/键值 → 保存 → DB 往返 ----
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
    // 填名 + 键值行 → 创建
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.name.hit", "网关env"));
        DOCTEST_CHECK(click(ui, "claude.dialog.shared.env.add.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.env.row.0.key.hit",
                           "MY_GW"));
        DOCTEST_CHECK(type(ui, "claude.dialog.shared.env.row.0.value.hit",
                           "v1"));
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
    // DB 往返：name + extra_env 落地
    {
        auto list = env.svc->listSharedConfigs();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE_EQ(list.value().size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(list.value()[0].name, std::string("网关env"));
        auto ee = module::claude::parseExtraEnv(list.value()[0].extraEnvJson);
        DOCTEST_REQUIRE(ee.isOk());
        DOCTEST_REQUIRE_EQ(ee.value().size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(ee.value().at("MY_GW"), std::string("v1"));
    }

    mod->onStop();
    return true;
})

// ---- 3. 绑定下拉 + 片段赢预览：K 键行置灰只读 + chip「来自片段 X」；
//      保存后 sharedConfigId 落库、profile 自有同名键非破坏保留 ----
REGISTER_TEST(claude_module_shared_bind_and_frozen_preview,
              "claude_module: 绑定下拉选片段 + 冻结行灰字chip + 保存落库（headless）", {
    TestEnv env;
    auto f = env.svc->createSharedConfig("片段A", R"({"MY_KEY":"shared-val"})");
    DOCTEST_REQUIRE(f.isOk());
    auto a = env.svc->createProfile("绑定测试", "https://s.example.com",
                                    "sk-ant-sbind-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    // 自有 env 带与片段同名的键（片段赢场景：行里保留、冻结显示）
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

    // 选中卡片 → 编辑 → 高级 env 页：初始「无（不绑定片段）」，行是普通输入
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* label = ui.find("claude.form.env.shared.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("无（不绑定片段）"));
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.key.hit") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.frozen.key") == nullptr);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.env.row.0.key.text"),
                         std::string("MY_KEY"));
    }
    // 拉开 → 选「片段A」→ 冻结预览：K 键行 → 灰字 + chip，输入控件消失
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.env.shared.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.env.shared.dd.item.1"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* label = ui.find("claude.form.env.shared.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("片段A"));
        // 冻结行：灰字 key / chip 标来源；同 id 的输入控件不再存在
        const auto* fk = ui.find("claude.form.env.row.0.frozen.key");
        DOCTEST_REQUIRE(fk != nullptr);
        DOCTEST_CHECK_EQ(fk->text, std::string("MY_KEY"));
        const auto* chip = ui.find("claude.form.env.row.0.frozen.chip");
        DOCTEST_REQUIRE(chip != nullptr);
        DOCTEST_CHECK(chip->text.find("片段A") != std::string::npos);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.key.hit") == nullptr);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.value.hit") == nullptr);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.del") == nullptr);
    }
    // 保存 → 绑定落库；自有同名键非破坏保留（行还在，片段赢由服务层保证）
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
    // 重开编辑：绑定下拉回显片段A，冻结行仍在。先等后台刷新把绑定带进
    // 缓存（卡片「共享」角标 = 缓存里 sharedConfigId != 0 的可观测信号；
    // 与真实应用一致——用户重开编辑前角标已可见），否则缓存里还是旧绑定
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
        DOCTEST_CHECK(click(ui, "claude.form.tabs.hit.2"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* label = ui.find("claude.form.env.shared.dd.label");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("片段A"));
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.frozen.key") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.key.hit") == nullptr);
    }
    // 下拉切回「无」→ 冻结行解冻回普通输入（解绑预览）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.env.shared.dd.field"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.form.env.shared.dd.item.0"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.key.hit") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.frozen.key") == nullptr);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 4. 新增同键拦截：绑定片段后手动加 K 键行 → 保存被拦 + 红字提示 ----
REGISTER_TEST(claude_module_shared_new_key_blocked,
              "claude_module: 新增片段冻结键保存被拦（headless）", {
    TestEnv env;
    auto f = env.svc->createSharedConfig("片段X", R"({"MY_KEY":"shared"})");
    DOCTEST_REQUIRE(f.isOk());
    auto a = env.svc->createProfile("拦截测试", "https://i.example.com",
                                    "sk-ant-iblk-0001", "");
    DOCTEST_REQUIRE(a.isOk());
    const int64_t pid = a.value().id;
    {
        module::claude::Profile p = a.value();
        p.sharedConfigId = f.value().id;   // 服务层已绑定
        DOCTEST_REQUIRE(env.svc->updateProfile(p).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_shared_blocked");
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

    // 编辑 → 高级 env 页（profile 无自有 env，空表）→ 添加一行并敲 K 键名
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
        // 敲 K 键名 → 该行驶进冻结渲染（灰字+chip，输入控件消失 = 立即可见）
        DOCTEST_CHECK(type(ui, "claude.form.env.row.0.key.hit", "MY_KEY"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.frozen.key") != nullptr);
        DOCTEST_CHECK(ui.find("claude.form.env.row.0.key.hit") == nullptr);
        // 保存 → 红字拦截，不落库
        DOCTEST_CHECK(click(ui, "claude.dialog.form.confirm.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        const auto* err = ui.find("claude.dialog.form.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("MY_KEY") != std::string::npos);
        DOCTEST_CHECK(err->text.find("片段X") != std::string::npos);
        DOCTEST_CHECK(err->text.find("共享配置") != std::string::npos);
    }
    // DB 未变：extra_env 仍空、绑定未动（表单仍开着）
    {
        auto got = env.svc->getProfile(pid);
        DOCTEST_REQUIRE(got.isOk());
        DOCTEST_CHECK_EQ(got.value().sharedConfigId, f.value().id);
        DOCTEST_CHECK(got.value().extraEnvJson.empty());
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 5. 详情行「共享配置：片段 A（N 个键）」+ 列表卡片「共享」角标 ----
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
    // 选中绑定的 → 详情行「共享配置：片段A（1 个键）」
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.profile.0.bg"));
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.field.shared.value"),
                         std::string("共享配置：片段A（1 个键）"));
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

// ---- 6. 弹窗管理链：共享片段编辑弹窗入 anyDialogOpen 门控 + Esc 序链最顶层 ----
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

// ---- 7. 删除片段 → 同事务解绑绑定 Profile → toast「N 个 Profile 已解绑」----
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

// ---- R4（P-B1）：claude_ui_logic 抽出的 4 个纯函数等价断言 ----
// 目的：证明「匿名 ns → claude_ui_logic.{h,cpp}」的搬迁**逐字等价**（spec
// 2026-09-27-refactor-incremental N9：只在抽出的纯函数上补等价断言，不算
// 新功能测试）。取值按搬迁前实现逐条钉死（边界 + 分支各一）。
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

    // modelsSummary：空配置 → "-"；仅旧 model 列 → 默认 + 1/6；
    // 仅槽位 → 「默认未配」+ 1/6（分母取自槽位表，不再硬编码 /6）
    module::claude::Profile emptyProfile;
    DOCTEST_CHECK_EQ(modelsSummary(emptyProfile), std::string("-"));

    module::claude::Profile legacyOnly;
    legacyOnly.model = "claude-sonnet-4-5";
    DOCTEST_CHECK_EQ(modelsSummary(legacyOnly),
                     std::string("默认 claude-sonnet-4-5 · 槽位已配 1/6"));

    module::claude::ModelsConfig mc;
    mc.sonnet.model = "fangzhou/glm-5.3[1M]";
    auto mcJson = module::claude::serializeModelsConfig(mc);
    DOCTEST_REQUIRE(mcJson.isOk());
    module::claude::Profile slotOnly;
    slotOnly.modelsConfigJson = mcJson.value();
    DOCTEST_CHECK_EQ(modelsSummary(slotOnly),
                     std::string("（默认未配）· 槽位已配 1/6"));

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

// ============================================================================
// 组 B（macOS 布局统一方案 §1 组 B / §5 组 B verify 口径，2026-09-28）
//   P1 动作区钉面板底（四页签分隔线 y 恒定）+ 内容框显式边界
//   P2 「最近调用」去掉 10 行硬上限 / 3 行硬下限，行数纯按可用高推
//   P3 「环境变量」列全部生效 env + 每键来源
//   P4 共享配置区块钉面板底 + 「+ 新建片段」恒可见可点
// 尺寸取**真实 macOS 页面尺寸** 1262×801（窗口 1470×801 − 侧边栏 208），
// 这是「真实窗口尺寸下的区域比例」类问题在无头侧唯一能覆盖的方式。
// 统一原则：容器可用高只有一个来源（bodyH）；贴底元素锚容器底。
// ============================================================================
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

// ---- 组 B P1：四个页签的动作区必须落在同一条水平线上 ----
// 用户主诉（原话）：「环境变量 最近调用 2 个 tab 下方的区域高度不对」。
// 根因：actY = paneBottom + 20 跟着内容底跑，面板底恒为 bodyH → 数据少时动作区
// 浮在上部，四个页签的分隔线互差最大 171px。修后 actY = max(paneBottom + 20,
// bodyH − 动作块预留)，内容框底由容器底推出 → 四值恒等。
REGISTER_TEST(claude_detail_actions_anchor_four_tabs,
              "claude_module: 详情动作区四页签同一条水平线（macOS 真实尺寸 1262x801）", {
    TestEnv env;
    auto p = env.svc->createProfile("锚点", "https://anchor.example.com",
                                    "sk-ant-anchor-0001", "m-anchor");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    DOCTEST_REQUIRE(env.svc->setActiveProfile(pid).isOk());
    // 环境变量页签给 2 个额外键（四页签内容底各不相同才有判别力）
    auto g = env.svc->getProfile(pid);
    DOCTEST_REQUIRE(g.isOk());
    g.value().extraEnvJson = R"({"MY_ANCHOR_A":"1","MY_ANCHOR_B":"2"})";
    DOCTEST_REQUIRE(env.svc->updateProfile(g.value()).isOk());
    DOCTEST_REQUIRE(seedCalls(env.svc.get(), pid, 3));

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();   // 首载同步装载快照（H2），下面第一帧就有 recent

    constexpr float kW = 1262.0f;   // 真实 macOS 页面宽（窗口 1470 − 侧边栏 208）
    constexpr float kH = 801.0f;    // 真实 macOS 页面高（窗口内容高 801）

    float actY[4] = {0, 0, 0, 0};
    float dividerY[4] = {0, 0, 0, 0};
    float panelBottom = 0.0f;
    for (int tab = 0; tab < 4; ++tab) {
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
        // 贴底元素不得越出容器底（方案 §4 不变量）
        DOCTEST_CHECK(act->frame.y + act->frame.height <= panelBottom + 0.5f);
    }
    // 四页签完全相等（旧实现下这四值互不相等 → 本用例即用户主诉的红灯）
    for (int tab = 1; tab < 4; ++tab) {
        DOCTEST_CHECK_EQ(actY[tab], actY[0]);
        DOCTEST_CHECK_EQ(dividerY[tab], dividerY[0]);
    }

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 组 B P2：行数纯按可用高推（旧实现锁死 10 行且截断无提示）----
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
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_rows", kW, kH);

    // 预算：内容框底 = bodyH − 85 − 20；行区 = 内容框底 − 表头底(24) −
    // 「查看全部」带(8+24)。本尺寸（bodyH=607 → 内容框底 502、表头底 136、
    // 带 32）→ floor(334/24) = 13 行（旧实现恒为 clamp(...,3,10) = 10 行）
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

// ---- 组 B P2 极端档：可用高不足一行 → 只画空态，不画越界行 ----
// 旧实现的硬下限 3 会把 3 行画进动作区（越界）；现在画 0 行 + 明确文案。
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
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
    eui::Ui ui2;
    composeAtSize(mod.get(), ui2, "claude_short", 1000.0f, 480.0f);

    DOCTEST_CHECK(ui2.find("claude.calls.t.0") == nullptr);   // 不画越界行
    const auto* empty = ui2.find("claude.calls.empty");
    DOCTEST_REQUIRE(empty != nullptr);
    DOCTEST_CHECK(empty->text.find("不足") != std::string::npos);
    // ---- 审计 F-02：0 行档的两条硬约束（此前这条用例断不到）----
    // ① 不再画「另有 N 条」—— 它与空态行同坐标会叠印（旧实现就是这么错的）
    DOCTEST_CHECK(ui2.find("claude.calls.overflow") == nullptr);
    // ② 空态行与「查看全部」带都不得越出内容框（真实裁剪下沿 = 内容框底）
    const auto* box = ui2.find("claude.detail.contentbox");
    DOCTEST_REQUIRE(box != nullptr);
    const float boxBottom = box->frame.y + box->frame.height;
    DOCTEST_CHECK(empty->frame.y + empty->frame.height <= boxBottom + 0.5f);
    if (const auto* va = ui2.find("claude.calls.viewall.hit")) {
        DOCTEST_CHECK(va->frame.y + va->frame.height <= boxBottom + 0.5f);
    }
    // 动作区仍在面板内（贴底锚定 + F-03 的 actYHi 夹取语义）
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

// ---- 真机反馈 ②（2026-09-29）：最近调用明细列「本地调用速率」 ----
// 手算值对照：速率 = output_tokens / (latency_ms/1000)，只有 2xx 且
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
    DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
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

// ---- 真机反馈 ④⑤⑥（2026-09-29）：两处文本垂直居中 + 「查看全部」改按钮 ----
// 居中判据：runtime 只在 verticalAlign == Center 时把 ink 中心对齐到 frame
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

    // ---- ⑤ 基础信息（tab 0 默认）：标签列与值列都相对各自行高居中 ----
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

    // ---- ④ 模型映射（tab 1）：每个槽位行的名称列/值列都相对 32 高行框居中 ----
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

    // ---- ⑥ 最近调用（tab 3）：「查看全部 →」是按钮，点击开弹窗 ----
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_center_calls", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
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

// ---- 真机反馈 ⑦（2026-09-29）：调用记录弹窗分页控件垂直对齐 ----
// 页码文本与左右翻页按钮的行框本来就是同一档（同 y = h−60、同 40 高），差的
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
        DOCTEST_REQUIRE(clickById(ui, "claude.detail.tabs.hit.3"));
    }
    {
        eui::Ui ui;
        composeAtSize(mod.get(), ui, "claude_page_align", kW, kH);
        DOCTEST_REQUIRE(clickById(ui, "claude.calls.viewall.hit"));
    }
    // 等后台查询落地（总匹配数到位 → 页数 = ceil(30/20) = 2；CPX-06 起数据是
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

// ---- 真机反馈 ⑧（2026-09-29）：Profile 卡 ACTIVE 胶囊徽标文字居中 ----
// 文字框与胶囊同 y 同高，缺的只是垂直居中（ink 默认从行框顶沿起排）。判据 =
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
    // 胶囊几何/位置不变（ACTIVE 48×20 在卡片右上角）
    const auto* abg = ui.find("claude.profile.0.active.bg");
    DOCTEST_REQUIRE(abg != nullptr);
    DOCTEST_CHECK_EQ(abg->frame.width, 48.0f);
    DOCTEST_CHECK_EQ(abg->frame.height, 20.0f);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// E-AUD-1（2026-09-30 审计）：长模型键在键列会截断显示 —— 键列宽 220px、
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

// ---- 组 B P3：环境变量页签列生效 env + 每键来源（2026-09-30 E1 起模型键除外）----
// 用户读到「只显示 1 条」的真相：该页此前只列 profile 自身的 extra_env。
// E1（2026-09-30）：模型映射域的键不再在本页展示（「模型映射」页签已列）——
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
    DOCTEST_REQUIRE(srcOf.count("MY_SHARED") == 1);
    DOCTEST_CHECK(srcOf["MY_SHARED"].find("片段") == 0);

    mod->onStop();
    cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// ---- 2026-09-30 E1/E2 几何断言：模型键不渲染 + 行几何不重叠（130% 缩放档）----
// 用户真机根因：全局字号缩放（用户机 130%）放大字号而行盒按 100% 写死 → 墨迹
// 越盒叠印；旧实现值框还跨整卡垂直居中，与行带 2 的来源/说明文本叠印。断言：
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

// ---- 组 B P4：共享配置区块钉面板底 + 「+ 新建片段」恒可点 ----
// 用户原话：「新建片段的按钮原本底部对齐的，现在没了」。
// 旧实现 listH = clamp(scrollH − sharedH − 12, min(96, scrollH), scrollH)：
// 96px 保底一生效（≈5 个片段起）区块就被推下去 → ≥6 个按钮被底部按钮压住点不到、
// ≥8 个被整页 clip。现在区块底锚定 = 底部按钮行上沿 − 12，高度取
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

    // ---- 审计 F-09：片段集变化 → 块内滚动偏移归零 ----
    // 判别力设计（避免恒真断言）：旧集与新集都是 8 张卡 → 内容高/maxOffset 不变，
    // 单靠渲染侧的 clamp 藏不住旧偏移；只有真的复位才会回到 0
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

