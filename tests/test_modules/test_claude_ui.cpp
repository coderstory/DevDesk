// tests/test_modules/test_claude_ui.cpp
// Claude 模块 UI 接线无头冒烟（照 T11 settings 的 compose 模式）：
//   1. 无 ClaudeService（global 为 null）时 compose 不崩、关键元素渲染
//   2. 有真实服务 + 数据时 compose 渲染 profile 卡片 / ACTIVE 徽标 / 详情 /
//      全部操作按钮（新建/启用/测试连通/编辑/删除/代理开关）
// 接线证据 = 代码路径 + 本无头 compose 测试 + 启动冒烟；真实鼠标点击留给用户人工点验
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/time.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "platform/terminal_launch.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <eui/dsl.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

    // 1. 点「查看全部 →」开弹窗（CPX-06：渲染路径不再同步查库，数据由后台取）
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
    // 最小高度 720：末卡仍在树里且可点选（详情面板跟随第 8 个 profile）
    {
        eui::Ui ui;
        composeAt(ui, 720.0f);
        DOCTEST_CHECK(ui.find("claude.profiles.scroll") != nullptr);
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

// ---- 审计 [21]：Profile 表单 Esc=取消（旧行为：输入框没绑 onEnter，Esc 完全无效）----

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
    auto ctrlPress = [](eui::InputKey k) {
        eui::KeyEvent e;
        e.key = k;
        e.action = eui::KeyAction::Press;
        e.modifiers.control = true;
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
        DOCTEST_CHECK(ui.find("claude.calls.empty") != nullptr);
    }
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.profile.0.name"), name0);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.profile.1.name"), name1);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.field.name.value"), detail);
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

// ---- 运行第 6 键（2026-09-28 profile-run 设计 §6/§8）----
// ⚠️ 本文件用真实 ClaudeService（setGlobal）：点「运行」前**必须**注入假 launch，
//    否则测试会真的启动 wt/powershell + claude（spec §6 第 4 项）
REGISTER_TEST(claude_module_run_button_headless,
              "claude_module: 「运行」按钮存在 + 无选中 toast + 假 launch 被调用一次", {
    TestEnv env;
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
    // runProfile 会真起代理：端口用动态空闲口（写死端口在多 worktree 下会撞）
    const int port = pickFreePort();
    DOCTEST_REQUIRE(port != 0);
    env.svc->setConfiguredProxyPort(port);

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

    // 有 profile 且选中 → 点击后假 launch 被调用一次、参数正确、代理已起
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
        DOCTEST_CHECK(env.svc->isProxyRunning());          // 顺带把代理拉起来了
        const auto* msg = ui.find("claude.toast.message");
        DOCTEST_REQUIRE(msg != nullptr);
        // D-9 的提示文案（会话不受 DevDesk 管理）
        DOCTEST_CHECK(msg->text.find("不受 DevDesk 管理") != std::string::npos);
    }
    cleanupProfileKey(kTestKeychainService, a.value().id);
    env.svc->stopProxy();
    return true;
})
