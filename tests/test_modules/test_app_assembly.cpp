// tests/test_modules/test_app_assembly.cpp
// 应用装配层（专家审计 2026-09-27 M2）单测：devdesk_tests 原本不含 main.cpp，
// 装配段零护栏——而历史上两次启动顺序事故都落在这里（W3：模块 onStart 时
// ClaudeService::global() 仍为 null；W2：退出未还原 settings.json）。
// 这里用桩模块 + 临时目录无头驱动 app_assembly（真实装配函数，非复刻逻辑）：
//   1. 三模块全关 → 只注册 home/settings；被关模块连 onStart 都不被调到、不崩
//   2. 注册顺序 home→markdown→todo→claude→settings，且 preStart（ClaudeService
//      创建 + setGlobal）严格早于一切模块 onStart（W3 的结构性保证）
//   3. 退出接线：atexit 只挂一次（armExitHook 幂等）、handler 真的触发 stopAll
//   4. dispatchFileOpen / pageForTrayAction 既有语义（映射表 + 负载/降级路径）
//   5. captureOpenFileFromArgv 直测（argv 接缝：有路径 / flag / 幂等；F 残留 ②）
//   6. 空态兜底注入：装配后「当前页无模块」画出 app 层占位（CP-01 回归护栏）
// 注：门控**语义**（enabledModules 配置 / moduleEnabled 谓词）已由
// test_module_gating 覆盖，这里只测**接线**，不重复它的断言。
#include "doctest.h"
#include "test_support.h"

#include "app_assembly.h"
#include "app_sidebar.h"  // app::showAppNotice / appNotice*（D-1 toast 断言）
#include "claude_service.h"  // 惰性装配真服务桩（devdesk_tests 链 claude_proxy 库）
#include "module_toggle_bridge.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "platform/http_server.h"  // 前置校验用例的 mock 上游（200 /v1/models）

#include <eui/dsl.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Action = core::tray::TrayMenuAction;

// 桩模块：把生命周期调用记进共享账本（顺序 / 幂等断言用）
class StubModule : public ::core::Module {
public:
    StubModule(::core::PageId page, std::string name,
               std::vector<std::string>* log)
        : page_(page), name_(std::move(name)), log_(log) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui&, const eui::Screen&) override {}
    void onStart() override {
        if (log_) log_->push_back("start:" + name_);
    }
    void onStop() override {
        if (log_) log_->push_back("stop:" + name_);
    }

private:
    ::core::PageId page_;
    std::string name_;
    std::vector<std::string>* log_;
};

// 五个桩模块 + 共享账本（按装配的注册顺序构造，名字与门控键对齐）。
// R8-C2：装配入参从 ModuleSet 改为 GatedEntry 列表（门控已前置
// buildGatedEntries——本类只提供**已门控**的清单形态；真实表门控由下方
// kModuleRegistry 用例锁）
class StubModules {
public:
    // 全启用清单（5 项，= allOnConfig 下 buildGatedEntries 的真实输出形态）
    std::vector<app::assembly::GatedEntry> set() {
        return {
            {::core::PageId::Home, nullptr, &home_},
            {::core::PageId::Markdown, nullptr, &markdown_},
            {::core::PageId::Todo, nullptr, &todo_},
            {::core::PageId::ClaudeProxy, nullptr, &claude_},
            {::core::PageId::Settings, nullptr, &settings_},
        };
    }
    // 只 home + settings（= allToolsOffConfig 下的门控形态）
    std::vector<app::assembly::GatedEntry> homeSettingsOnly() {
        return {{::core::PageId::Home, nullptr, &home_},
                {::core::PageId::Settings, nullptr, &settings_}};
    }
    // home + markdown + settings（= markdownOnlyConfig 下的门控形态）
    std::vector<app::assembly::GatedEntry> markdownOnly() {
        return {{::core::PageId::Home, nullptr, &home_},
                {::core::PageId::Markdown, nullptr, &markdown_},
                {::core::PageId::Settings, nullptr, &settings_}};
    }
    std::vector<std::string>& log() { return log_; }
    bool logHas(const std::string& entry) const {
        return std::find(log_.begin(), log_.end(), entry) != log_.end();
    }

private:
    std::vector<std::string> log_;
    StubModule home_{::core::PageId::Home, "home", &log_};
    StubModule markdown_{::core::PageId::Markdown, "markdown", &log_};
    StubModule todo_{::core::PageId::Todo, "todo", &log_};
    StubModule claude_{::core::PageId::ClaudeProxy, "claude", &log_};
    StubModule settings_{::core::PageId::Settings, "settings", &log_};
};

// 全启用配置：**显式列全门控键**（final-review Fix 2，2026-10-01 起 agent
// 不在默认清单——spec §8 L4 默认关；all-on=7 契约按构造口径保住，断言不弱化）
module::settings::SettingsConfig allOnConfig() {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdClaudeProxy,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdAgent};
    return cfg;
}

// 只启用 home + settings（= 三个工具模块全关；moduleEnabled 对 home/settings
// 恒 true，但列表里写全更贴近真实 config.json 的形态）
module::settings::SettingsConfig allToolsOffConfig() {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules.clear();
    cfg.enabledModules.push_back(module::settings::kModuleIdHome);
    cfg.enabledModules.push_back(module::settings::kModuleIdSettings);
    return cfg;
}

// 只启用 home + markdown（dispatchFileOpen 的正常路径用）
module::settings::SettingsConfig markdownOnlyConfig() {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules.clear();
    cfg.enabledModules.push_back(module::settings::kModuleIdHome);
    cfg.enabledModules.push_back(module::settings::kModuleIdMarkdown);
    cfg.enabledModules.push_back(module::settings::kModuleIdSettings);
    return cfg;
}

// 全启用 + pet（SettingsConfig 默认 = 除 pet 外全启用；桌宠默认不在清单，
// maybeStartPet 的「开」态配置要显式补上）
module::settings::SettingsConfig petOnConfig() {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules.push_back(module::settings::kModuleIdPet);
    return cfg;
}

// 托盘动作 → 页面（无映射返回 false）
bool trayPage(Action action, ::core::PageId* out) {
    const auto page = app::assembly::pageForTrayAction(action);
    if (!page) return false;
    *out = *page;
    return true;
}

bool trayPageIs(Action action, ::core::PageId expected) {
    ::core::PageId got = ::core::PageId::Settings;
    return trayPage(action, &got) && got == expected;
}

bool trayPageIsNone(Action action) {
    return !app::assembly::pageForTrayAction(action).has_value();
}

// 临时笔记目录（dispatchFileOpen 的 notesDir 注入点）
// —— 共享工厂 test_support::makeTempDir（TIN-06b，原 tempNotesDir 合流）
constexpr const char* kNotesDirPrefix = "devdesk_test_assembly_notes";

// ---- R0/D6：跨模块服务的装配契约（core 只读服务定位器） ----
// 需求（重构 spec R0 / 审计 D6）：跨模块服务由装配段在模块 onStart **之前**
// 注册进 core 只读服务定位器（生产 = main.cpp 的 preStart 注册 ClaudeService
// 适配器），模块 onStart 时即可解析；未注册时 requireService fail（模块显式
// 处理缺失，而不是拿到裸空指针）。这里用桩接口验证**接线**，不引任何
// module_* 头。
class IProbeService {
public:
    virtual ~IProbeService() = default;
    virtual int value() const = 0;
};

class ProbeService final : public IProbeService {
public:
    int value() const override { return 42; }
};

// onStart 里经定位器解析 IProbeService，把结果记进账本
class ProbeConsumerModule : public ::core::Module {
public:
    explicit ProbeConsumerModule(std::vector<std::string>* log) : log_(log) {}
    ::core::PageId pageId() const override { return ::core::PageId::Settings; }
    std::string displayName() const override { return "probe"; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui&, const eui::Screen&) override {}
    void onStart() override {
        auto resolved = ::core::ServiceLocator::requireService<IProbeService>();
        log_->push_back(resolved.isOk()
                            ? "start:probe:" +
                                  std::to_string(resolved.value()->value())
                            : std::string("start:probe:missing"));
    }

private:
    std::vector<std::string>* log_;
};

} // namespace

REGISTER_TEST(assembly_modules_all_off,
              "assembly: 三模块全关 → 只注册 home/settings（被关模块零 onStart）", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    StubModules mods;

    const auto order = app::assembly::assembleAndStart(
        router, allToolsOffConfig(), mods.homeSettingsOnly(),
        app::assembly::PreStartHook{});

    DOCTEST_REQUIRE_EQ(order.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(order[0] == ::core::PageId::Home);
    DOCTEST_CHECK(order[1] == ::core::PageId::Settings);
    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(2));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Home));
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Settings));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::Markdown));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::Todo));
    DOCTEST_CHECK(!router.isRegistered(::core::PageId::ClaudeProxy));
    // 零执行：被关模块连 onStart 都不该被调到（只登记 home/settings 的账）
    DOCTEST_CHECK(mods.logHas("start:home"));
    DOCTEST_CHECK(mods.logHas("start:settings"));
    DOCTEST_CHECK(!mods.logHas("start:markdown"));
    DOCTEST_CHECK(!mods.logHas("start:todo"));
    DOCTEST_CHECK(!mods.logHas("start:claude"));
    return true;
})

REGISTER_TEST(assembly_registration_order_prestart_first,
              "assembly: 注册顺序 home→markdown→todo→claude→settings 且 preStart 先于一切 onStart", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    StubModules mods;

    // preStart 记账 = W3 事故点：ClaudeService 创建 + setGlobal（生产为
    // main.cpp 的 prepareClaudeServiceBeforeModules）必须先于模块 onStart——
    // 模块 onStart 里经 ClaudeService::global() 取共享单例
    const auto order = app::assembly::assembleAndStart(
        router, allOnConfig(), mods.set(),
        [&mods] { mods.log().push_back("pre:claude-service"); });

    DOCTEST_REQUIRE_EQ(order.size(), static_cast<size_t>(5));
    // 全启用下的期望注册顺序
    const ::core::PageId kExpectedOrder[] = {
        ::core::PageId::Home,       ::core::PageId::Markdown,
        ::core::PageId::Todo,       ::core::PageId::ClaudeProxy,
        ::core::PageId::Settings,
    };
    const char* const kExpectedStartLog[] = {
        "start:home", "start:markdown", "start:todo", "start:claude",
        "start:settings",
    };
    for (size_t i = 0; i < 5; ++i) {
        DOCTEST_CHECK(order[i] == kExpectedOrder[i]);
    }
    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(5));
    // 账本：第一条是 preStart，其后依次是各模块 onStart（顺序 = 注册顺序）
    DOCTEST_REQUIRE_EQ(mods.log().size(), static_cast<size_t>(6));
    DOCTEST_CHECK_EQ(mods.log()[0], std::string("pre:claude-service"));
    for (size_t i = 0; i < 5; ++i) {
        DOCTEST_CHECK_EQ(mods.log()[i + 1], std::string(kExpectedStartLog[i]));
    }
    return true;
})

REGISTER_TEST(assembly_exit_hook_armed_idempotent,
              "assembly: 退出接线只挂一次（幂等）+ handler 触发各模块 onStop", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    StubModules mods;

    app::assembly::assembleAndStart(router, allToolsOffConfig(),
                                    mods.homeSettingsOnly(),
                                    app::assembly::PreStartHook{});

    // 装配即完成退出接线（armExitHook 在 startAll 之后、与原 getRouter 同位置）
    DOCTEST_CHECK(app::assembly::exitHookArmed());
    // 幂等：已挂过不再重复挂 atexit（重复挂 = 退出时 stopAll 跑两次）
    DOCTEST_CHECK_FALSE(app::assembly::armExitHook());
    DOCTEST_CHECK(app::assembly::exitHookArmed());

    // handler 主体：取 core::appRouter()（assembleAndStart 已注册本 Router）→ stopAll
    app::assembly::runExitHook();
    DOCTEST_CHECK(mods.logHas("stop:home"));
    DOCTEST_CHECK(mods.logHas("stop:settings"));
    DOCTEST_CHECK(!mods.logHas("stop:markdown"));  // 未注册模块不参与 stopAll
    return true;
})

REGISTER_TEST(assembly_dispatch_and_tray_mapping,
              "assembly: dispatchFileOpen / pageForTrayAction 既有语义", {
    test_support::ScopedAppRouter scoped;

    // ---- 1. 扩展名 → 目标页（纯映射，搬运前 main.cpp 匿名空间内同款）----
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.md") == ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.markdown") == ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.json") == ::core::PageId::ClaudeProxy);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.js") == ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.html") == ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.htm") == ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.txt") == ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.png") == ::core::PageId::Home);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("no_ext") == ::core::PageId::Home);
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("") == ::core::PageId::Home);
    // 大小写敏感（识别不了时回首页，搬运前后同款）
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.MD") == ::core::PageId::Home);

    // ---- 2. 托盘动作 → 页面（非导航动作无映射）----
    DOCTEST_CHECK(trayPageIs(Action::NavigateHome, ::core::PageId::Home));
    DOCTEST_CHECK(trayPageIs(Action::NavigateMarkdown, ::core::PageId::Markdown));
    DOCTEST_CHECK(trayPageIs(Action::NavigateTodo, ::core::PageId::Todo));
    DOCTEST_CHECK(trayPageIs(Action::NavigateClaude, ::core::PageId::ClaudeProxy));
    DOCTEST_CHECK(trayPageIsNone(Action::ShowMainWindow));
    DOCTEST_CHECK(trayPageIsNone(Action::Exit));
    DOCTEST_CHECK(trayPageIsNone(Action::None));

    // ---- 3. 页名（模块被关时 toast 文案用）----
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Home)), std::string("首页"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Markdown)), std::string("笔记"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Todo)), std::string("待办"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::ClaudeProxy)), std::string("Claude"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Settings)), std::string("设置"));

    // ---- 4. dispatchFileOpen：笔记目录内文件 → "open:<相对路径>" 负载 + 切页 ----
    const std::filesystem::path notesDir =
        test_support::makeTempDir(kNotesDirPrefix, "tray");
    std::error_code ec;
    std::filesystem::remove_all(notesDir, ec);
    std::filesystem::create_directories(notesDir, ec);
    DOCTEST_REQUIRE(std::filesystem::exists(notesDir, ec));
    {
        std::ofstream f(notesDir / "note.md", std::ios::trunc);
        f << "# 装配测试\n";
    }

    ::core::Router router;
    StubModules mods;
    app::assembly::assembleAndStart(router, markdownOnlyConfig(),
                                    mods.markdownOnly(),
                                    app::assembly::PreStartHook{});
    DOCTEST_REQUIRE(router.isRegistered(::core::PageId::Markdown));

    app::assembly::dispatchFileOpen(&router, (notesDir / "note.md").string(), notesDir);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Markdown);
    DOCTEST_CHECK_EQ(router.consumePendingPayload(::core::PageId::Markdown),
                     std::string("open:note.md"));

    // ---- 5. Markdown 被关（未注册）→ Router 回退 Home 且不投递负载 ----
    ::core::Router gatedRouter;
    StubModules gatedMods;
    app::assembly::assembleAndStart(gatedRouter, allToolsOffConfig(),
                                    gatedMods.homeSettingsOnly(),
                                    app::assembly::PreStartHook{});
    app::assembly::dispatchFileOpen(&gatedRouter, (notesDir / "note.md").string(), notesDir);
    DOCTEST_CHECK_EQ(gatedRouter.currentPage(), ::core::PageId::Home);  // 自带回退
    DOCTEST_CHECK_EQ(gatedRouter.consumePendingPayload(::core::PageId::Markdown),
                     std::string());  // 不往关掉的模块投递

    // ---- 6. .json → Claude 页（无负载切页，且不碰笔记目录）----
    ::core::Router jsonRouter;
    StubModules jsonMods;
    app::assembly::assembleAndStart(jsonRouter, allOnConfig(), jsonMods.set(),
                                    app::assembly::PreStartHook{});
    app::assembly::dispatchFileOpen(&jsonRouter,
                                    (notesDir / "不存在.json").string(), notesDir);
    DOCTEST_CHECK_EQ(jsonRouter.currentPage(), ::core::PageId::ClaudeProxy);
    DOCTEST_CHECK_EQ(jsonRouter.consumePendingPayload(::core::PageId::ClaudeProxy),
                     std::string());

    // ---- 7. 空路径 / 空 Router → 原地不动、不崩 ----
    app::assembly::dispatchFileOpen(&jsonRouter, "", notesDir);
    DOCTEST_CHECK_EQ(jsonRouter.currentPage(), ::core::PageId::ClaudeProxy);
    app::assembly::dispatchFileOpen(nullptr, (notesDir / "note.md").string(), notesDir);

    std::filesystem::remove_all(notesDir, ec);
    return true;
})

REGISTER_TEST(assembly_capture_open_file_from_argv,
              "assembly: argv 捕获直测 captureOpenFileFromArgv（有路径→pending+目标页 / flag→不写 / 幂等，F 残留 ②）", {
    using app::assembly::captureOpenFileFromArgv;
    using app::assembly::pendingOpenFilePath;
    using app::assembly::resetArgvCaptureForTest;

    // ---- 1. 无文件参数（只有 exe）→ 不写 pending ----
    resetArgvCaptureForTest();
    pendingOpenFilePath().clear();
    captureOpenFileFromArgv({"devdesk.exe"});
    DOCTEST_CHECK(pendingOpenFilePath().empty());

    // ---- 2. flag 参数（'-' 开头）→ 不写 pending ----
    resetArgvCaptureForTest();
    captureOpenFileFromArgv({"devdesk.exe", "--update"});
    DOCTEST_CHECK(pendingOpenFilePath().empty());

    // ---- 3. 有文件路径 → 写入 pending，且由它推出的目标页正确 ----
    resetArgvCaptureForTest();
    captureOpenFileFromArgv({"devdesk.exe", "note.md"});
    DOCTEST_CHECK_EQ(pendingOpenFilePath(), std::string("note.md"));
    DOCTEST_CHECK(app::assembly::dispatchPageForFile(pendingOpenFilePath()) ==
                  ::core::PageId::Markdown);

    // ---- 4. 幂等：重复调用只捕获一次（第二次的路径不覆盖已捕获值）----
    captureOpenFileFromArgv({"devdesk.exe", "other.md"});
    DOCTEST_CHECK_EQ(pendingOpenFilePath(), std::string("note.md"));

    // 收尾：不留进程级状态（pending 会被真实启动分发读取）
    resetArgvCaptureForTest();
    pendingOpenFilePath().clear();
    return true;
})

// 空态兜底渲染的注入接线（专家审计 2026-09-27 CP-01）：core 侧的 Router 不再
// 自行绘制「无页面」占位（spec §4.2.1：eui::Ui 仅可透传），文案与样式搬到装配
// 段注入。本用例证明装配后「当前页无模块」真的会画出 app 层的占位。

REGISTER_TEST(assembly_injects_empty_page_fallback,
              "assembly: 装配时注入空态兜底（无模块可显示 → 「无页面」由 app 层画）", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    // 空 GatedEntry 列表：装配后一个模块都没注册（registerModule 的既有
    // 容错分支）→ compose 落到「当前页无模块」。这是经装配后的 Router 唯一能
    // 走到兜底的形态（生产恒注册 home/settings，Home 未注册时 navigateTo 也
    // 只停在当前页）
    app::assembly::assembleAndStart(router, allOnConfig(),
                                    std::vector<app::assembly::GatedEntry>{},
                                    app::assembly::PreStartHook{});

    eui::Screen screen;
    screen.width = 800.0f;
    screen.height = 600.0f;
    eui::Ui ui;
    ui.begin("a1");
    router.compose(ui, screen);
    ui.end();
    // 占位真被画出来：id/文案逐字搬自 core 旧内联实现（"empty" / 「无页面」）
    DOCTEST_CHECK(ui.find("empty") != nullptr);
    return true;
})

// R0/D6 装配契约：preStart 注册的服务 → 模块 onStart 可解析；未注册 → 显式缺失

REGISTER_TEST(assembly_service_locator_registration_precedes_onstart,
              "assembly: R0/D6 —— preStart 注册的服务在模块 onStart 可解析（未注册则显式缺失）", {
    // 场景 1：preStart 里注册（生产 main.cpp 的适配器注册点同位置——严格早于
    // 模块 onStart）→ 模块 onStart 解析到同一实例
    {
        test_support::ScopedAppRouter scoped;
        ::core::Router router;
        std::vector<std::string> log;
        ProbeService probe;
        ProbeConsumerModule probeModule(&log);
        // RAII 记账：作用域结束把槽位还原成「未注册」（用例 fail-fast 提前
        // return 也不会把栈上 probe 的地址留在进程级槽里）
        ::core::ScopedService<IProbeService> restore{nullptr};

        app::assembly::assembleAndStart(
            router, markdownOnlyConfig(),
            std::vector<app::assembly::GatedEntry>{
                {::core::PageId::Settings, nullptr, &probeModule}},
            [&probe] { ::core::ServiceLocator::set<IProbeService>(&probe); });

        DOCTEST_REQUIRE_EQ(log.size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(log[0], std::string("start:probe:42"));
    }
    // 场景 2：preStart 不注册 → 模块 onStart 拿到 fail（显式缺失，不是裸空指针）
    {
        test_support::ScopedAppRouter scoped;
        ::core::Router router;
        std::vector<std::string> log;
        ProbeConsumerModule probeModule(&log);

        app::assembly::assembleAndStart(
            router, markdownOnlyConfig(),
            std::vector<app::assembly::GatedEntry>{
                {::core::PageId::Settings, nullptr, &probeModule}},
            app::assembly::PreStartHook{});

        DOCTEST_REQUIRE_EQ(log.size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(log[0], std::string("start:probe:missing"));
    }
    return true;
})

// ---- R8-C2：真实 kModuleRegistry 表（门控键列填错 / 表序漂移此处必挂）----

REGISTER_TEST(assembly_registry_table_shape,
              "assembly: 真实 kModuleRegistry —— 7 行 / 行序==PageId 枚举序 / home+settings+about gatingKey==nullptr / 另四行==kModuleId*", {
    using ::app::registry::kModuleRegistry;
    constexpr size_t kRegistrySize =
        sizeof(kModuleRegistry) / sizeof(kModuleRegistry[0]);
    const ::core::PageId kEnumOrder[] = {
        ::core::PageId::Home,       ::core::PageId::Markdown,
        ::core::PageId::Todo,       ::core::PageId::ClaudeProxy,
        ::core::PageId::Settings,   ::core::PageId::Agent,
        ::core::PageId::About,
    };
    DOCTEST_CHECK_EQ(kRegistrySize, static_cast<size_t>(7));
    for (size_t i = 0; i < kRegistrySize; ++i) {
        // 表序 == PageId 枚举序（重排 = 侧边栏序静默变化，core_pageid_order
        // 契约的装配侧镜像）
        DOCTEST_CHECK(kModuleRegistry[i].page == kEnumOrder[i]);
    }
    // T3 口径：home/settings/about 恒注册（gatingKey==nullptr）；四个工具模块
    // 各自带对应门控键
    DOCTEST_CHECK(kModuleRegistry[0].gatingKey == nullptr);
    DOCTEST_CHECK_EQ(std::string(kModuleRegistry[1].gatingKey),
                     std::string(module::settings::kModuleIdMarkdown));
    DOCTEST_CHECK_EQ(std::string(kModuleRegistry[2].gatingKey),
                     std::string(module::settings::kModuleIdTodo));
    DOCTEST_CHECK_EQ(std::string(kModuleRegistry[3].gatingKey),
                     std::string(module::settings::kModuleIdClaudeProxy));
    DOCTEST_CHECK(kModuleRegistry[4].gatingKey == nullptr);
    DOCTEST_CHECK_EQ(std::string(kModuleRegistry[5].gatingKey),
                     std::string(module::settings::kModuleIdAgent));
    DOCTEST_CHECK(kModuleRegistry[6].gatingKey == nullptr);
    return true;
})

REGISTER_TEST(assembly_build_gated_entries_real_table,
              "assembly: buildGatedEntries 真实表 —— all-off 恰 3 条（home/settings/about 恒注册）/ all-on 7 条按表序", {
    constexpr size_t kRegistrySize =
        sizeof(::app::registry::kModuleRegistry) /
        sizeof(::app::registry::kModuleRegistry[0]);
    // 全关（四个工具模块）→ 恒注册行必含，输出恰 3 条（语义同
    // assembly_modules_all_off 的 :165 恒注册断言，但锁的是**真实表**——
    // gatingKey 列填错此处必挂）
    const auto gated = app::assembly::buildGatedEntries(
        allToolsOffConfig(), ::app::registry::kModuleRegistry,
        kRegistrySize);
    DOCTEST_REQUIRE_EQ(gated.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(gated[0].page == ::core::PageId::Home);
    DOCTEST_CHECK(gated[1].page == ::core::PageId::Settings);
    DOCTEST_CHECK(gated[2].page == ::core::PageId::About);
    // 实例列恒为 nullptr——由调用方按 page 从自有 static 持有填入
    DOCTEST_CHECK(gated[0].module == nullptr);
    DOCTEST_CHECK(gated[1].module == nullptr);
    DOCTEST_CHECK(gated[2].module == nullptr);
    // 全开 → 7 条且序 == 表序
    const auto allGated = app::assembly::buildGatedEntries(
        allOnConfig(), ::app::registry::kModuleRegistry, kRegistrySize);
    DOCTEST_REQUIRE_EQ(allGated.size(), static_cast<size_t>(7));
    const ::core::PageId kEnumOrder[] = {
        ::core::PageId::Home,       ::core::PageId::Markdown,
        ::core::PageId::Todo,       ::core::PageId::ClaudeProxy,
        ::core::PageId::Settings,   ::core::PageId::Agent,
        ::core::PageId::About,
    };
    for (size_t i = 0; i < allGated.size(); ++i) {
        DOCTEST_CHECK(allGated[i].page == kEnumOrder[i]);
    }
    return true;
})

// 表短名/图标列 ↔ 模块虚函数双副本一致性（R8 T4/T5：静态列不可省——tray
// 关闭态必须无活实例可查；一致性用例是双副本唯一缝合点，仿
// test_module_icons 先例构造真实模块取纯 getter，无 DB）
REGISTER_TEST(assembly_registry_matches_module_getters,
              "assembly: 表 shortName/iconGlyph/page 与真实模块 getter 一致（防双副本 drift）", {
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> mod(e.factory());
        DOCTEST_REQUIRE(mod != nullptr);
        DOCTEST_CHECK(e.page == mod->pageId());
        DOCTEST_CHECK_EQ(std::string(e.shortName), mod->shortName());
        DOCTEST_CHECK_EQ(std::string(e.iconGlyph), mod->iconGlyph());
    }
    return true;
})

// ---- R8-C2 PAB-03：托盘委托判定（Navigate* 委托成功切页 / 其余不导航）----

REGISTER_TEST(assembly_dispatch_tray_navigation,
              "assembly: dispatchTrayNavigation —— Navigate* 委托成功切页返回 true / ShowMainWindow·Exit·None 返回 false 不导航", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    StubModules mods;
    app::assembly::assembleAndStart(router, allOnConfig(), mods.set(),
                                    app::assembly::PreStartHook{});
    DOCTEST_REQUIRE_EQ(router.modules().size(), static_cast<size_t>(5));

    // Navigate*：委托 navigateFromTray——目标页已注册 → 直接切页 + true
    DOCTEST_CHECK(app::assembly::dispatchTrayNavigation(
        Action::NavigateMarkdown, router));
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Markdown);
    DOCTEST_CHECK(app::assembly::dispatchTrayNavigation(
        Action::NavigateTodo, router));
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Todo);
    DOCTEST_CHECK(app::assembly::dispatchTrayNavigation(
        Action::NavigateClaude, router));
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::ClaudeProxy);
    DOCTEST_CHECK(app::assembly::dispatchTrayNavigation(
        Action::NavigateHome, router));
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Home);

    // 非导航动作：返回 false 且不动当前页（由调用方处理显示/退出）
    const ::core::PageId before = router.currentPage();
    DOCTEST_CHECK_FALSE(app::assembly::dispatchTrayNavigation(
        Action::ShowMainWindow, router));
    DOCTEST_CHECK_FALSE(
        app::assembly::dispatchTrayNavigation(Action::Exit, router));
    DOCTEST_CHECK_FALSE(
        app::assembly::dispatchTrayNavigation(Action::None, router));
    DOCTEST_CHECK_EQ(router.currentPage(), before);
    return true;
})

// ---- 活门控文案口径（Task 5 fix 轮 [2]）：托盘进未启用页的 toast = 即时生效口径 ----
// navigateFromTray 落到未注册页 → 回首页 + toast。文案从「重启后生效」改为
// 「即时启用」指引（与活门控一致）；断言「即时」在文、「重启」不在——
// 文案回退即红。只注册 Home（markdown 未装配）构造未启用场景。
REGISTER_TEST(assembly_navigate_from_tray_unregistered_toast_copy,
              "assembly: navigateFromTray 未启用页 → 回首页 + toast 即时生效口径（无重启字样）", {
    test_support::ScopedAppRouter scoped;
    std::vector<std::string> log;
    StubModule home(::core::PageId::Home, "首页", &log);
    ::core::Router router;
    router.registerModule(&home);
    app::clearAppNotice();  // 进程级 toast 回厂（本用例只认自己触发的通知）

    // markdown 未注册 → 托盘进 markdown 页 = 未启用场景（回首页 + toast）
    app::assembly::navigateFromTray(&router, ::core::PageId::Markdown);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Home);
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("模块已关闭"));
    const std::string msg = app::appNoticeMessage();
    DOCTEST_CHECK(msg.find("笔记") != std::string::npos);  // shortName 单源
    DOCTEST_CHECK(msg.find("设置 → 模块开关") != std::string::npos);  // 入口指引保留
    DOCTEST_CHECK(msg.find("即时") != std::string::npos);  // 新口径在文
    DOCTEST_CHECK(msg.find("重启") == std::string::npos);  // 旧字样回退即红

    app::clearAppNotice();  // 收尾回厂，不留给后续用例
    return true;
})

// ---- R8-C2 PAB-04：maybeStartPet 门控两态 ----

REGISTER_TEST(assembly_maybe_start_pet_gated,
              "assembly: maybeStartPet —— 开则 loadPetConfig+startFn 收透传 config / 关则双不调", {
    module::pet::PetConfig expected;
    expected.skinName = "neo_plus";
    expected.x = 42;
    expected.y = -7;
    int loadCalls = 0;
    int startCalls = 0;
    module::pet::PetConfig received;

    // 开（enabledModules 含 "pet"）：loadPetConfig 返回值原样透传 startFn
    app::assembly::maybeStartPet(
        petOnConfig(),
        [&] {
            ++loadCalls;
            return expected;
        },
        [&](const module::pet::PetConfig& cfg) {
            ++startCalls;
            received = cfg;
        });
    DOCTEST_CHECK_EQ(loadCalls, 1);
    DOCTEST_CHECK_EQ(startCalls, 1);
    DOCTEST_CHECK_EQ(received.skinName, expected.skinName);
    DOCTEST_CHECK_EQ(received.x, expected.x);
    DOCTEST_CHECK_EQ(received.y, expected.y);

    // 关（默认配置不含 pet）：startFn **与** loadPetConfig 都不被调——短
    // 路径证门控谓词同源（同一 moduleEnabled(kModuleIdPet)、同一 cfg 参数）
    loadCalls = 0;
    startCalls = 0;
    app::assembly::maybeStartPet(
        allOnConfig(),
        [&] {
            ++loadCalls;
            return expected;
        },
        [&](const module::pet::PetConfig&) { ++startCalls; });
    DOCTEST_CHECK_EQ(loadCalls, 0);
    DOCTEST_CHECK_EQ(startCalls, 0);
    return true;
})

// ---- 活门控（live-toggle）dispatchModuleToggle：计数桩 + 查找缝 ----
// ModuleLookup 是裸函数指针（无捕获约束）→ 桩与计数走文件级静态、自由函数
// 直接引用（形态同 test_module_toggle_bridge 的 record 桩）。用例开头回厂
// 计数与 toast 状态（进程级，fail-fast 提前 return 也不污染后续用例）。
namespace {

int g_agentStarts = 0;
int g_agentStops = 0;
int g_homeStarts = 0;
int g_homeStops = 0;
// 时序账本（2026-10-01 惰性装配计划 T1，Review Focus #2 钉子）：seam 调用与
// 桩模块 onStart 各取自增序号，断言 seam 序号 < onStart 序号
int g_seqCounter = 0;

class ToggleCountModule : public ::core::Module {
public:
    ToggleCountModule(::core::PageId page, std::string name, int* starts,
                      int* stops, int* seq = nullptr)
        : page_(page), name_(std::move(name)), starts_(starts), stops_(stops),
          seq_(seq) {}
    // 观测 D-1 时序（T12 契约）：onStop 里登记「此刻本模块是否仍注册」。
    // off 路径要求 onStop 先于 unregisterModule——顺序颠倒（先注销再停）时
    // isRegistered 已为 false，断言 stopWhileRegistered == stops 必红
    void observeRouter(const ::core::Router* router) { router_ = router; }
    int stopWhileRegistered() const { return stopWhileRegistered_; }
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui&, const eui::Screen&) override {}
    void onStart() override {
        ++*starts_;
        if (seq_ != nullptr) *seq_ = ++g_seqCounter;
    }
    void onStop() override {
        ++*stops_;
        if (router_ != nullptr && router_->isRegistered(page_)) {
            ++stopWhileRegistered_;
        }
    }

private:
    ::core::PageId page_;
    std::string name_;
    int* starts_;
    int* stops_;
    int* seq_ = nullptr;  // 非空时记录 onStart 的全局时序号（惰性装配时序断言）
    const ::core::Router* router_ = nullptr;
    int stopWhileRegistered_ = 0;
};

ToggleCountModule g_toggleAgent{::core::PageId::Agent, "agent",
                                &g_agentStarts, &g_agentStops};
ToggleCountModule g_toggleHome{::core::PageId::Home, "home", &g_homeStarts,
                               &g_homeStops};

int g_claudeStarts = 0;
int g_claudeStops = 0;
int g_claudeStartSeq = 0;
ToggleCountModule g_toggleClaude{::core::PageId::ClaudeProxy, "claude",
                                 &g_claudeStarts, &g_claudeStops,
                                 &g_claudeStartSeq};

// Agent / ClaudeProxy 映桩对象、其余 nullptr（生产 = main.cpp getRouter 的
// moduleForPage 语义）
::core::Module* toggleLookupStub(::core::PageId page) {
    if (page == ::core::PageId::Agent) return &g_toggleAgent;
    if (page == ::core::PageId::ClaudeProxy) return &g_toggleClaude;
    return nullptr;
}

} // namespace

REGISTER_TEST(assembly_dispatch_module_toggle_lifecycle,
              "assembly: dispatchModuleToggle on/off 驱动注册与生命周期（off 当前页重定向 Home + toast + 恒开/未知 no-op）", {
    g_agentStarts = 0;
    g_agentStops = 0;
    g_homeStarts = 0;
    g_homeStops = 0;
    app::clearAppNotice();  // 进程级 toast 回厂（本用例只认自己触发的通知）

    ::core::Router router;
    // 生产形态：Home 恒注册（导航兜底与 D-1 重定向 guard 都依赖它）
    router.registerModule(&g_toggleHome);
    g_toggleAgent.observeRouter(&router);  // onStop 时序观测（D-1）

    // on：注册 + onStart
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Agent));
    DOCTEST_CHECK_EQ(g_agentStarts, 1);
    DOCTEST_CHECK_EQ(g_agentStops, 0);

    // off：onStop 先行 + 注销（当前页 Home ≠ Agent → 不重定向、无 toast）
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, false,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_FALSE(router.isRegistered(::core::PageId::Agent));
    DOCTEST_CHECK_EQ(g_agentStops, 1);
    DOCTEST_CHECK_EQ(g_toggleAgent.stopWhileRegistered(), 1);
    DOCTEST_CHECK_FALSE(app::appNoticeVisible());

    // 再开 → 把 Agent 设为当前页 → 再关 → D-1 重定向 Home + toast
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_agentStarts, 2);
    router.navigateTo(::core::PageId::Agent);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Agent);
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, false,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_agentStops, 2);
    // 两次 onStop 都发生在「本模块仍注册」时 = onStop 先于 unregisterModule
    //（D-1/T12 时序：运行线程在 onStop 内 join 后页才消失；顺序颠倒必红）
    DOCTEST_CHECK_EQ(g_toggleAgent.stopWhileRegistered(), 2);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Home);
    // toast 文案：shortName 取自注册表静态列（不依赖活实例），锁 D-1 反馈
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("模块已关闭"));
    DOCTEST_CHECK_EQ(app::appNoticeMessage(),
                     std::string("智能体 已关闭，已返回首页"));

    // 恒开模块（home 行 gatingKey==nullptr）→ no-op：计数不动、Home 不动
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdHome, true,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_homeStarts, 0);
    DOCTEST_CHECK_EQ(g_homeStops, 0);
    DOCTEST_CHECK_EQ(g_agentStarts, 2);
    DOCTEST_CHECK(router.isRegistered(::core::PageId::Home));

    // 未知 moduleId → no-op（WARN 分支）
    app::assembly::dispatchModuleToggle("no_such_module", true, router,
                                        &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_agentStarts, 2);
    DOCTEST_CHECK_EQ(g_agentStops, 2);

    app::clearAppNotice();  // 收尾回厂，不留给后续用例
    return true;
})

// ---- 惰性装配（2026-10-01 计划 T1）：dispatch on 路径 → 第二桥 → 真服务 ----
// seam 注入**真服务 handler**（等价 main.cpp ensureClaudeServiceAssembledImpl
// 的逻辑：装配单例 → setConfiguredProxyPort → recover → autoStart 才起代理），
// 驱动真 ClaudeService（devdesk_tests 链 devdesk_module_claude_proxy）。唯一
// 刻意偏差：startProxy 用端口 0（OS 分配，test_claude_service 既有起代理用例
// 同款）——不用 configuredProxyPort()（配置口径默认 8888，与生产/其他
// worktree 并行测试撞端口）。headless 层钉的是 dispatch→seam 契约 + 真
// service 可观察幂等语义；「真 main.cpp getClaudeService 被触发」不在
// devdesk_tests 链接面（无 main.cpp），归 T3 app 级冒烟覆盖。
// 进程全局态纪律：ClaudeService::setGlobal / 桥 handler / toast 都是进程级
// ——RAII 守卫析构全还原（fail-fast 提前 return 也不泄漏）。
namespace {

std::unique_ptr<module::claude::ClaudeService> g_asmSvc;
test_support::TestDb* g_asmDb = nullptr;  // 用例持有库，桩只借用
int g_asmHandlerCalls = 0;
bool g_asmLastAutoStart = false;
int g_asmStartCalls = 0;  // 真正执行了 startProxy 的次数（幂等/校验挡下断言）
bool g_asmStartOk = true; // handler 是 void 自由函数不能内嵌断言 → 落账本
int g_asmSeamSeq = 0;     // seam 调用时序号（0 = 本用例未调）

// 等价 main.cpp assembleClaudeServiceNow 的构造段（从 handler 桩抽出，供用例
// 在 dispatch 之前预置「已装配 + 已有激活 profile」的生产形态）
void asmEnsureService() {
    if (g_asmSvc == nullptr) {
        // getClaudeService static 守卫语义：重复调用返回同指针（这里用
        // unique_ptr 空判等价表达），setGlobal 在创建时完成一次
        g_asmSvc = std::make_unique<module::claude::ClaudeService>(
            g_asmDb->handle(), "DevDeskTestClaudeService");
        module::claude::ClaudeService::setGlobal(g_asmSvc.get());
    }
}

// 等价 main.cpp：assembleClaudeServiceNow（getClaudeService → 端口自读注入 →
// recover）+ startClaudeProxy（前置校验 → 失败 notice/零副作用 → 通过才 bind；
// isProxyRunning 早退语义照搬）
void assemblyHandlerStub(bool autoStart) {
    ++g_asmHandlerCalls;
    g_asmLastAutoStart = autoStart;
    g_asmSeamSeq = ++g_seqCounter;
    asmEnsureService();
    // 端口自读：SettingsStore(::core::paths::configFile()).load()（与 main 的
    // assembleClaudeServiceNow 同源；测试配置经 DEVDESK_CONFIG_PATH 隔离）
    const module::settings::SettingsStore store(::core::paths::configFile());
    const auto cfg = store.load();
    g_asmSvc->setConfiguredProxyPort(cfg.proxyPort);
    g_asmSvc->recoverPendingSettingsRestore();
    if (!autoStart) return;
    if (g_asmSvc->isProxyRunning()) return;               // main 的早退语义
    auto v = g_asmSvc->validateProxyPreconditions();      // ← 新增：前置校验
    if (v.isFail()) {
        // 会话内路径（用户动作）→ 应用级 notice（镜像 main.cpp notifyFailure=true
        // 分支；真 main.cpp 不在本链接面，见上方说明）
        app::showAppNotice("代理未启动", v.errorMessage());
        return;                                           // 零副作用失败
    }
    auto r = g_asmSvc->startProxy("127.0.0.1", 0);        // 端口 0 = OS 分配
    ++g_asmStartCalls;
    g_asmStartOk = r.isOk();
}

// 全局态还原守卫：析构 = stopProxy + setGlobal(原值) + handler 复位 nullptr +
// clearAppNotice + 桩服务销毁（进程全局态全还原，含 fail-fast 早退路径）
struct LazyAssemblyGuard {
    LazyAssemblyGuard() = default;  // 声明 deleted 拷贝会抑制隐式默认构造，显式补回
    module::claude::ClaudeService* prevGlobal =
        module::claude::ClaudeService::global();
    ~LazyAssemblyGuard() {
        if (g_asmSvc && g_asmSvc->isProxyRunning()) {
            g_asmSvc->stopProxy();
        }
        module::claude::ClaudeService::setGlobal(prevGlobal);
        app::toggle::setClaudeAssemblyHandler(nullptr);
        g_asmSvc.reset();
        g_asmDb = nullptr;
        app::clearAppNotice();
    }
    LazyAssemblyGuard(const LazyAssemblyGuard&) = delete;
    LazyAssemblyGuard& operator=(const LazyAssemblyGuard&) = delete;
};

// 用例前置回厂（进程级账本清零；handler/服务由 LazyAssemblyGuard 兜底）
void resetLazyAssemblyLedger() {
    g_asmHandlerCalls = 0;
    g_asmLastAutoStart = false;
    g_asmStartCalls = 0;
    g_asmStartOk = true;
    g_asmSeamSeq = 0;
    g_claudeStarts = 0;
    g_claudeStops = 0;
    g_claudeStartSeq = 0;
    g_agentStarts = 0;
    g_agentStops = 0;
    g_seqCounter = 0;
    app::clearAppNotice();
}

// 配置隔离：claude 门控关（enabledModules 不含 claude）+ autoStart 按用例给。
// 返回 bool：DOCTEST_CHECK 宏含 `return false`，只能用在返回 bool 的函数里
//（doctest.h 断言语义 #3）；断言由调用方（用例体）做
bool writeLazyAssemblyConfig(const std::filesystem::path& cfgPath,
                             bool autoStart) {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdAgent};
    cfg.claudeProxyAutoStart = autoStart;
    return module::settings::SettingsStore(cfgPath).save(cfg).isOk();
}

} // namespace

REGISTER_TEST(assembly_dispatch_claude_lazy_assemble_autostart,
              "assembly: 会话内开 claude → dispatch on 先惰性装配真服务（global 非空+代理起+seam 序<onStart 序+重复 on 幂等+off/非 claude 不触缝）", {
    resetLazyAssemblyLedger();
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_lazy", "autostart"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(scoped.cfgPath, true));

    // 2026-10-01 新契约（本计划 T2 Step 1）：会话内 auto-start 前必须过前置
    // 校验（激活 profile + 连通测试）→ 用例必须先起 mock 上游 + 种激活 profile。
    // 旧版在**空 profile store** 上断言「代理已起」——那正是事故语义，已作废。
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("GET", "/v1/models",
                   [](const platform::http::RequestCtx&,
                      platform::http::ResponseWriter& w) {
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());

    test_support::TestDb asmDb("assembly_lazy", "autostart_db");
    g_asmDb = &asmDb;
    app::toggle::setClaudeAssemblyHandler(&assemblyHandlerStub);

    // 先装配服务并种激活 profile（等价生产：用户此前已在 Claude 页启用过一套）
    asmEnsureService();
    auto p = g_asmSvc->createProfile(
        "asm", "http://127.0.0.1:" + std::to_string(upstream.actualPort()),
        "sk-ant-asm-0001", "m");
    DOCTEST_REQUIRE(p.isOk());
    DOCTEST_REQUIRE(g_asmSvc->setActiveProfile(p.value().id).isOk());

    ::core::Router router;
    router.registerModule(&g_toggleHome);  // 生产形态：Home 恒注册

    // on：dispatch → seam（惰性装配 + auto-start 会话内恢复）
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        true, router, &toggleLookupStub);
    auto* svc = module::claude::ClaudeService::global();
    DOCTEST_CHECK(svc != nullptr);  // ① 装配完成（启动期关 → 会话内补装配）
    DOCTEST_CHECK_EQ(g_asmHandlerCalls, 1);
    DOCTEST_CHECK(g_asmLastAutoStart);
    DOCTEST_CHECK(g_asmStartOk);
    if (svc != nullptr) {
        DOCTEST_CHECK(svc->isProxyRunning());  // ① autoStart=true → 代理已起
        DOCTEST_CHECK(svc->proxyPort() > 0);
    }
    DOCTEST_CHECK(router.isRegistered(::core::PageId::ClaudeProxy));
    DOCTEST_CHECK_EQ(g_claudeStarts, 1);
    // ② 时序（Review Focus #2）：seam 调用严格先于桩模块 onStart
    DOCTEST_CHECK(g_asmSeamSeq > 0);
    DOCTEST_CHECK(g_claudeStartSeq > 0);
    DOCTEST_CHECK(g_asmSeamSeq < g_claudeStartSeq);
    const int firstPort = (svc != nullptr) ? svc->proxyPort() : 0;

    // ③ 再拨 on → 幂等：同 svc 指针（static 守卫语义）+ 代理仍跑 + 同端口 +
    //    startProxy 实际执行次数仍 == 1（无二次绑定副作用）
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        true, router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_asmHandlerCalls, 2);
    DOCTEST_CHECK(module::claude::ClaudeService::global() == svc);
    if (svc != nullptr) {
        DOCTEST_CHECK(svc->isProxyRunning());
        DOCTEST_CHECK_EQ(svc->proxyPort(), firstPort);
    }
    DOCTEST_CHECK_EQ(g_asmStartCalls, 1);
    DOCTEST_CHECK_EQ(g_claudeStarts, 2);

    // ④ off → 不触 seam（off 路径零改动：service 进程级存活，off 不拆服务）
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        false, router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_asmHandlerCalls, 2);
    DOCTEST_CHECK_EQ(g_claudeStops, 1);

    // ⑤ 非 claude id（agent）on → 不触缝
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_asmHandlerCalls, 2);
    DOCTEST_CHECK_EQ(g_agentStarts, 1);

    upstream.stop();
    test_support::cleanupProfileKey("DevDeskTestClaudeService", p.value().id);
    return true;
})

REGISTER_TEST(assembly_dispatch_claude_lazy_assemble_no_autostart,
              "assembly: 会话内开 claude（autoStart=false）→ 只装配不起代理（global 非空 + isProxyRunning false）", {
    resetLazyAssemblyLedger();
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_lazy", "noautostart"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(scoped.cfgPath, false));

    test_support::TestDb asmDb("assembly_lazy", "noautostart_db");
    g_asmDb = &asmDb;
    app::toggle::setClaudeAssemblyHandler(&assemblyHandlerStub);

    ::core::Router router;
    router.registerModule(&g_toggleHome);

    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        true, router, &toggleLookupStub);
    auto* svc = module::claude::ClaudeService::global();
    DOCTEST_CHECK(svc != nullptr);  // 已装配
    DOCTEST_CHECK_EQ(g_asmHandlerCalls, 1);
    DOCTEST_CHECK_FALSE(g_asmLastAutoStart);
    DOCTEST_CHECK_EQ(g_asmStartCalls, 0);
    if (svc != nullptr) {
        DOCTEST_CHECK_FALSE(svc->isProxyRunning());  // 代理未起（Review Focus #5）
        DOCTEST_CHECK_EQ(svc->proxyPort(), 0);
    }
    DOCTEST_CHECK_EQ(g_claudeStarts, 1);

    // off → 不触缝
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        false, router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_asmHandlerCalls, 1);
    DOCTEST_CHECK_EQ(g_claudeStops, 1);
    return true;
})

// ---- 新增（本计划核心回归 = 事故现场）：空 profile store + autoStart=true
// → 前置校验挡下：代理不起、不 bind、零接管，且用户可见面给原因
REGISTER_TEST(assembly_dispatch_claude_toggle_blocked_by_preconditions,
              "assembly: 会话内开 claude + autoStart，空 profile store → 前置校验挡下（代理不起/不接管）", {
    resetLazyAssemblyLedger();
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_pv", "noactive"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(scoped.cfgPath, true));   // autoStart=true

    test_support::TestDb asmDb("assembly_pv", "noactive_db");       // 空库 = 事故现场
    g_asmDb = &asmDb;
    app::toggle::setClaudeAssemblyHandler(&assemblyHandlerStub);

    ::core::Router router;
    router.registerModule(&g_toggleHome);

    app::assembly::dispatchModuleToggle(module::settings::kModuleIdClaudeProxy,
                                        true, router, &toggleLookupStub);

    auto* svc = module::claude::ClaudeService::global();
    DOCTEST_CHECK(svc != nullptr);            // 服务仍装配（模块要能进页，死页缓解不回归）
    DOCTEST_CHECK_EQ(g_asmStartCalls, 0);     // startProxy 一次都没跑（不 bind）
    if (svc != nullptr) {
        DOCTEST_CHECK_FALSE(svc->isProxyRunning());
        DOCTEST_CHECK_EQ(svc->proxyPort(), 0);
    }
    DOCTEST_CHECK(router.isRegistered(::core::PageId::ClaudeProxy));  // 模块本身照常开启
    DOCTEST_CHECK_EQ(g_claudeStarts, 1);      // onStart 照常
    // 用户可见面（桩镜像 main.cpp 的 showAppNotice 分支；真 main.cpp 路径归 T3 冒烟）
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK(app::appNoticeTitle() == std::string("代理未启动"));
    DOCTEST_CHECK(app::appNoticeMessage().find("前置校验未通过") != std::string::npos);
    return true;
})
