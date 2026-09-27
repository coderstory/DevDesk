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
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "core/service_locator.h"

#include <eui/dsl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
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

// 五个桩模块 + 共享账本（按装配的注册顺序构造，名字与门控键对齐）
class StubModules {
public:
    app::assembly::ModuleSet set() {
        return {&home_, &markdown_, &todo_, &claude_, &settings_};
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

// 全启用配置（SettingsConfig 默认值 = 五个模块全开）
module::settings::SettingsConfig allOnConfig() {
    return module::settings::SettingsConfig{};
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

// 全启用下的期望注册顺序（REGISTER_TEST 宏体内不能有裸逗号，清单放宏外）
const ::core::PageId kExpectedOrder[] = {
    ::core::PageId::Home,       ::core::PageId::Markdown,
    ::core::PageId::Todo,       ::core::PageId::ClaudeProxy,
    ::core::PageId::Settings,
};
const char* const kExpectedStartLog[] = {
    "start:home", "start:markdown", "start:todo", "start:claude",
    "start:settings",
};

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
        router, allToolsOffConfig(), mods.set(), app::assembly::PreStartHook{});

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

    app::assembly::assembleAndStart(router, allToolsOffConfig(), mods.set(),
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
    app::assembly::assembleAndStart(router, markdownOnlyConfig(), mods.set(),
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
                                    gatedMods.set(), app::assembly::PreStartHook{});
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
    // 全 nullptr 的 ModuleSet：装配后一个模块都没注册（registerModule 的既有
    // 容错分支）→ compose 落到「当前页无模块」。这是经装配后的 Router 唯一能
    // 走到兜底的形态（生产恒注册 home/settings，Home 未注册时 navigateTo 也
    // 只停在当前页）
    app::assembly::assembleAndStart(router, allOnConfig(),
                                    app::assembly::ModuleSet{},
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
            app::assembly::ModuleSet{nullptr, nullptr, nullptr, nullptr,
                                     &probeModule},
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
            app::assembly::ModuleSet{nullptr, nullptr, nullptr, nullptr,
                                     &probeModule},
            app::assembly::PreStartHook{});

        DOCTEST_REQUIRE_EQ(log.size(), static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(log[0], std::string("start:probe:missing"));
    }
    return true;
})
