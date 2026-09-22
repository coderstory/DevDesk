// tests/test_modules/test_claude_ui.cpp
// Claude 模块 UI 接线无头冒烟（照 T11 settings 的 compose 模式）：
//   1. 无 ClaudeService（global 为 null）时 compose 不崩、关键元素渲染
//   2. 有真实服务 + 数据时 compose 渲染 profile 卡片 / ACTIVE 徽标 / 详情 /
//      全部操作按钮（新建/启用/测试连通/编辑/删除/代理开关）
// 接线证据 = 代码路径 + 本无头 compose 测试 + 启动冒烟；真实鼠标点击留给用户人工点验
#include "doctest.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "platform/http_server.h"
#include "platform/keychain.h"

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
