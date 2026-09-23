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
#include "settings_sync.h"

#include <eui/dsl.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
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

// RAII 临时目录（「导入当前配置」测试：临时 settings.json / sidecar 注入，
// 绝不碰真实 ~/.claude——settings_sync 的 Paths 注入模式）
struct ImportTempDir {
    std::filesystem::path path;

    explicit ImportTempDir(const std::string& tag) {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("devdesk_test_import_cur_" + tag + "_" +
                std::to_string(++counter));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~ImportTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path file(const char* name) const { return path / name; }
    void write(const char* name, const std::string& content) const {
        std::ofstream(file(name), std::ios::binary | std::ios::trunc) << content;
    }
    module::claude::settings_sync::Paths paths() const {
        module::claude::settings_sync::Paths p;
        p.settingsFile = file("settings.json");
        p.sidecarFile = file("snapshot.json");
        return p;
    }
};

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

// ---- 6. 「导入当前配置」按钮存在 + 读 settings.json → 弹窗预填全链 ----
// 点按钮 = 等价用户手动「粘贴导入」但全自动：临时 settings.json（Paths 注入）
// → 弹窗打开 → 基础页名称建议/BASE_URL/Token、模型映射页槽位、高级 env 页
// env 行 + 预览标记 → 保存 → DB 往返。
REGISTER_TEST(claude_module_import_current_full_chain,
              "claude_module: 导入当前配置读settings.json预填表单（headless）", {
    TestEnv env;
    ImportTempDir d("full");
    const auto paths = d.paths();
    env.svc->setSettingsSyncPaths(paths);
    // 整段 settings.json 形状（含非 env 顶层字段，验证取 env 子对象）
    d.write("settings.json", R"({
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
    // 高级 env 页：env 行预填 + 预览标记保留（识别槽位 2/6 · env 1 项）
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.env.row.0.key.text"),
                         std::string("DISABLE_AUTOUPDATER"));
        DOCTEST_CHECK_EQ(textOf(ui, "claude.form.env.row.0.value.text"),
                         std::string("1"));
        const auto* preview = ui.find("claude.form.import.preview");
        DOCTEST_REQUIRE(preview != nullptr);
        DOCTEST_CHECK(preview->text.find("槽位 2/6") != std::string::npos);
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
        cleanupKey(p.id);
    }

    mod->onStop();
    return true;
})

// ---- 7. 「导入当前配置」文件缺失 → toast 提示，不弹窗 ----
REGISTER_TEST(claude_module_import_current_missing_file,
              "claude_module: 导入当前配置文件缺失toast不弹窗（headless）", {
    TestEnv env;
    ImportTempDir d("miss");
    const auto paths = d.paths();   // settings.json 不创建
    env.svc->setSettingsSyncPaths(paths);

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
    d.write("settings.json", R"({"permissions": {"allow": ["Bash"]}})");
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
    ImportTempDir d("takeover");
    const auto paths = d.paths();
    env.svc->setSettingsSyncPaths(paths);
    // 模拟接管态：settings.json 是代理托管值 + sidecar 快照残留
    d.write("settings.json", R"({
        "env": {
            "ANTHROPIC_BASE_URL": "http://127.0.0.1:8888",
            "ANTHROPIC_AUTH_TOKEN": "devdesk-proxy-token",
            "MY_KEY": "user"
        }
    })");
    d.write("snapshot.json",
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

    // 1. 点「查看全部 →」开弹窗
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.calls.viewall.hit"));
    }
    // 2. 弹窗首帧：搜索框 / 状态下拉 / scrollView / 首页 20 行 / 页码 / 总数
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
    // 3. 下一页：剩 5 行，下一页置灰
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(click(ui, "claude.dialog.calls.next.bg"));
    }
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
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.page"),
                         std::string("第 1 / 2 页"));
    }
    // 5. 搜索框过滤（path 子串）：3 条命中，单页
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(type(ui, "claude.dialog.calls.search.hit", "complete"));
    }
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
    {
        eui::Ui ui;
        compose(ui);
        // complete 全是 429 → 4xx 命中不变
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
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK_EQ(textOf(ui, "claude.dialog.calls.count"),
                         std::string("3 条匹配"));
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.2.wrap") != nullptr);
    }

    mod->onStop();
    cleanupKey(pid);
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
    for (size_t i = 0; i < ids.size(); ++i) cleanupKey(ids[i]);
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
        auto* btn = ui.find("claude.btn.new.bg");
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
