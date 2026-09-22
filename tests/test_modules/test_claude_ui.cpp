// tests/test_modules/test_claude_ui.cpp
// Claude 模块 UI 接线无头冒烟（照 T11 settings 的 compose 模式）：
//   1. 无 ClaudeService（global 为 null）时 compose 不崩、关键元素渲染
//   2. 有真实服务 + 数据时 compose 渲染 profile 卡片 / ACTIVE 徽标 / 详情 /
//      全部操作按钮（新建/启用/测试连通/编辑/删除/代理开关）
// 接线证据 = 代码路径 + 本无头 compose 测试 + 启动冒烟；真实鼠标点击留给用户人工点验
#include "doctest.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/input/input_types.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/time.h"
#include "platform/http_server.h"
#include "platform/keychain.h"
#include "provider_config.h"

#include <eui/dsl.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" ::core::Module* create_claude_module();

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeModule";

std::filesystem::path tempDbPath() {
    static int counter = 0;
    ++counter;
    return std::filesystem::temp_directory_path() /
           ("devdesk_test_claude_ui_" + std::to_string(counter) + ".db");
}

// RAII：临时库 + 注册全局 ClaudeService（析构时一定解除注册，防悬垂污染后续用例）
struct TestEnv {
    std::filesystem::path path;
    core::Database db;
    std::unique_ptr<module::claude::ClaudeService> svc;

    TestEnv() {
        path = tempDbPath();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " + r.errorMessage());
        }
        db = std::move(r.unsafeValue());
        auto m = db.migrate();
        if (m.isFail()) {
            throw std::runtime_error("migrate failed: " + m.errorMessage());
        }
        svc = std::make_unique<module::claude::ClaudeService>(db.db(),
                                                              kTestKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~TestEnv() {
        module::claude::ClaudeService::setGlobal(nullptr);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
};

void cleanupKey(int64_t profileId) {
    platform::keychain::deleteSecret(
        kTestKeychainService, "claude:" + std::to_string(profileId));
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
    // 注意：REGISTER_TEST 宏体内不能出现裸逗号，Screen 用字段赋值构造
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("claude_null");
    mod->compose(ui, screen);
    ui.end();

    // 页面骨架 + 关键操作按钮存在（即使无数据也应渲染，按钮带防误触提示）
    DOCTEST_CHECK(ui.find("claude.root") != nullptr);
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);
    DOCTEST_CHECK(ui.find("claude.btn.new") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.new") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.enable") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.test") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.edit") != nullptr);
    DOCTEST_CHECK(ui.find("claude.act.del") != nullptr);
    // 无服务时显示错误态（W7：不再静默吞错显示假空态）
    DOCTEST_CHECK(ui.find("claude.profiles.error") != nullptr);
    DOCTEST_CHECK(ui.find("claude.profiles.empty") == nullptr);

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
    DOCTEST_CHECK(ui.find("claude.btn.proxy") != nullptr);

    mod->onStop();
    cleanupKey(a.value().id);
    cleanupKey(b.value().id);
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
    cleanupKey(a.value().id);
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
    // 详情面板摘要（§6.2）：默认模型 + 已配 3/6
    {
        eui::Ui ui;
        compose(ui);
        const auto* summary = ui.find("claude.field.model.value");
        DOCTEST_REQUIRE(summary != nullptr);
        DOCTEST_CHECK(summary->text.find("fangzhou/glm-5.3[1M]") !=
                      std::string::npos);
        DOCTEST_CHECK(summary->text.find("3/6") != std::string::npos);
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
    cleanupKey(pid);
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
        DOCTEST_CHECK(click(ui, "claude.btn.new.bg"));
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
    // 解析预览：槽位 2/6 + 别名 1 + 未识别 1；别名提示行存在
    {
        eui::Ui ui;
        compose(ui);
        const auto* preview = ui.find("claude.form.import.preview");
        DOCTEST_REQUIRE(preview != nullptr);
        DOCTEST_CHECK(preview->text.find("槽位 2/6") != std::string::npos);
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
        cleanupKey(p.id);
    }

    mod->onStop();
    return true;
})
