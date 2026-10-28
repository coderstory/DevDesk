// tests/test_modules/test_app_assembly.cpp
// 应用装配层单测：门控后的注册顺序与 onStart 时序、preStart 早于一切 onStart、退出接线、
// 文件/托盘分发、argv 接缝、空态占位注入、活门控 dispatchModuleToggle、claude 的会话内
// 惰性装配。用桩模块 + 临时目录无头驱动真实 app_assembly 函数（不是复刻逻辑）。
// 跨文件前提：
//   - 门控**语义**（enabledModules 配置 / moduleEnabled 谓词）由 test_module_gating 覆盖，
//     本文件只测接线，不重复它的断言。
//   - devdesk_tests 不链接 main.cpp。凡装配点写在 main.cpp 的（prepareClaudeServiceBeforeModules、
//     getRouter 的 moduleForPage、ensureClaudeServiceAssembledImpl），本文件以桩复刻其语义并
//     注明偏差；真 main.cpp 的行为不在本链接面。
//   - `module_*` 之间不可互相依赖：本文件对装配契约一律用本地桩接口验证，不引任何 module_* 头。
#include "doctest.h"
#include "test_support.h"

#include "app_assembly.h"
#include "app_sidebar.h"  // app::showAppNotice / appNotice*（toast 断言）
#include "claude_service.h"  // 惰性装配真服务桩（devdesk_tests 链 claude_proxy 库）
#include "module_toggle_bridge.h"
#include "settings_config.h"

#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "platform/http_server.h"  // 前置校验用例的 mock 上游（200 POST /v1/messages）
#include "platform/notification.h" // M1 接线用例（initialize spy）
#include "platform/wallpaper_capability.h"  // wallpaper 行空工厂的唯一合法条件

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
// 本类只提供**已门控**的清单形态；真实表的门控结果由 kModuleRegistry 与 buildGatedEntries
// 两个用例锁，不在这里复刻。
class StubModules {
public:
    // 全启用清单（6 项，= allOnConfig 下 buildGatedEntries 的真实输出形态；
    // agent/wallpaper 不在其中——真实表用例单独覆盖它们）
    std::vector<app::assembly::GatedEntry> set() {
        return {
            {::core::PageId::Home, nullptr, &home_},
            {::core::PageId::Markdown, nullptr, &markdown_},
            {::core::PageId::Todo, nullptr, &todo_},
            {::core::PageId::ClaudeProxy, nullptr, &claude_},
            {::core::PageId::Memory, nullptr, &memory_},
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
    StubModule memory_{::core::PageId::Memory, "memory", &log_};
    StubModule settings_{::core::PageId::Settings, "settings", &log_};
};

// 全启用配置：显式列全门控键（含默认清单里没有的 agent / wallpaper / mcp），
// 「all-on = 11 项」按这个构造口径保住
module::settings::SettingsConfig allOnConfig() {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdClaudeProxy,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdAgent,
                          module::settings::kModuleIdMemory,
                          module::settings::kModuleIdWallpaper,
                          module::settings::kModuleIdMcp};
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

// 临时笔记目录前缀（dispatchFileOpen 的 notesDir 注入点；目录本身由
// test_support::makeTempDir 建，该工厂先清残骸再建空目录）
constexpr const char* kNotesDirPrefix = "devdesk_test_assembly_notes";

// 跨模块服务的装配契约：core 只读服务定位器。跨模块服务由装配段在模块 onStart **之前**注册进去
//（生产 = main.cpp 的 preStart 注册 ClaudeService 适配器），模块 onStart 时即可解析；未注册时
// requireService fail（模块显式处理缺失，而不是拿到裸空指针）。
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
              "assembly: 注册顺序 home→markdown→todo→claude→memory→settings 且 preStart 先于一切 onStart", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    StubModules mods;

    // preStart 必须早于一切模块 onStart：生产在 main.cpp 的 prepareClaudeServiceBeforeModules
    // 里建 ClaudeService 并 setGlobal，模块 onStart 经 ClaudeService::global() 取共享单例。
    // preStart 晚一步，模块就拿到空单例。
    const auto order = app::assembly::assembleAndStart(
        router, allOnConfig(), mods.set(),
        [&mods] { mods.log().push_back("pre:claude-service"); });

    DOCTEST_REQUIRE_EQ(order.size(), static_cast<size_t>(6));
    // 全启用下的期望注册顺序
    const ::core::PageId kExpectedOrder[] = {
        ::core::PageId::Home,       ::core::PageId::Markdown,
        ::core::PageId::Todo,       ::core::PageId::ClaudeProxy,
        ::core::PageId::Memory,     ::core::PageId::Settings,
    };
    const char* const kExpectedStartLog[] = {
        "start:home", "start:markdown", "start:todo", "start:claude",
        "start:memory", "start:settings",
    };
    for (size_t i = 0; i < 6; ++i) {
        DOCTEST_CHECK(order[i] == kExpectedOrder[i]);
    }
    DOCTEST_CHECK_EQ(router.modules().size(), static_cast<size_t>(6));
    // 账本：第一条是 preStart，其后依次是各模块 onStart（顺序 = 注册顺序）
    DOCTEST_REQUIRE_EQ(mods.log().size(), static_cast<size_t>(7));
    DOCTEST_CHECK_EQ(mods.log()[0], std::string("pre:claude-service"));
    for (size_t i = 0; i < 6; ++i) {
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

    // 装配即完成退出接线
    DOCTEST_CHECK(app::assembly::exitHookArmed());
    // 幂等：已挂过不再重复挂 atexit（重复挂 = 退出时 stopAll 跑两次）
    DOCTEST_CHECK_FALSE(app::assembly::armExitHook());
    DOCTEST_CHECK(app::assembly::exitHookArmed());

    // handler 主体：取 core::appRouter()（assembleAndStart 已注册本 Router）→ stopAll
    app::assembly::runExitHook();
    DOCTEST_CHECK(mods.logHas("stop:home"));
    DOCTEST_CHECK(mods.logHas("stop:settings"));
    DOCTEST_CHECK(!mods.logHas("stop:markdown"));
    return true;
})

REGISTER_TEST(assembly_dispatch_and_tray_mapping,
              "assembly: dispatchFileOpen / pageForTrayAction 既有语义", {
    test_support::ScopedAppRouter scoped;

    // 1. 扩展名 → 目标页（纯映射）
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
    // 大小写敏感：识别不了时回首页
    DOCTEST_CHECK(app::assembly::dispatchPageForFile("a.MD") == ::core::PageId::Home);

    // 2. 托盘动作 → 页面（非导航动作无映射）
    DOCTEST_CHECK(trayPageIs(Action::NavigateHome, ::core::PageId::Home));
    DOCTEST_CHECK(trayPageIs(Action::NavigateMarkdown, ::core::PageId::Markdown));
    DOCTEST_CHECK(trayPageIs(Action::NavigateTodo, ::core::PageId::Todo));
    DOCTEST_CHECK(trayPageIs(Action::NavigateClaude, ::core::PageId::ClaudeProxy));
    DOCTEST_CHECK(trayPageIsNone(Action::ShowMainWindow));
    DOCTEST_CHECK(trayPageIsNone(Action::Exit));
    DOCTEST_CHECK(trayPageIsNone(Action::None));

    // 3. 页名（模块被关时 toast 文案用）
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Home)), std::string("首页"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Markdown)), std::string("笔记"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Todo)), std::string("待办"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::ClaudeProxy)), std::string("Claude"));
    DOCTEST_CHECK_EQ(std::string(app::assembly::trayPageDisplayName(::core::PageId::Settings)), std::string("设置"));

    // 4. dispatchFileOpen：笔记目录内文件 → "open:<相对路径>" 负载 + 切页
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

    // 5. Markdown 被关（未注册）→ Router 回退 Home 且不投递负载
    ::core::Router gatedRouter;
    StubModules gatedMods;
    app::assembly::assembleAndStart(gatedRouter, allToolsOffConfig(),
                                    gatedMods.homeSettingsOnly(),
                                    app::assembly::PreStartHook{});
    app::assembly::dispatchFileOpen(&gatedRouter, (notesDir / "note.md").string(), notesDir);
    DOCTEST_CHECK_EQ(gatedRouter.currentPage(), ::core::PageId::Home);  // 自带回退
    DOCTEST_CHECK_EQ(gatedRouter.consumePendingPayload(::core::PageId::Markdown),
                     std::string());  // 不往关掉的模块投递

    // 6. .json → Claude 页（无负载切页，且不碰笔记目录）
    ::core::Router jsonRouter;
    StubModules jsonMods;
    app::assembly::assembleAndStart(jsonRouter, allOnConfig(), jsonMods.set(),
                                    app::assembly::PreStartHook{});
    app::assembly::dispatchFileOpen(&jsonRouter,
                                    (notesDir / "不存在.json").string(), notesDir);
    DOCTEST_CHECK_EQ(jsonRouter.currentPage(), ::core::PageId::ClaudeProxy);
    DOCTEST_CHECK_EQ(jsonRouter.consumePendingPayload(::core::PageId::ClaudeProxy),
                     std::string());

    // 7. 空路径 / 空 Router → 原地不动、不崩
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

    // 1. 无文件参数（只有 exe）→ 不写 pending
    resetArgvCaptureForTest();
    pendingOpenFilePath().clear();
    captureOpenFileFromArgv({"devdesk.exe"});
    DOCTEST_CHECK(pendingOpenFilePath().empty());

    // 2. flag 参数（'-' 开头）→ 不写 pending
    resetArgvCaptureForTest();
    captureOpenFileFromArgv({"devdesk.exe", "--update"});
    DOCTEST_CHECK(pendingOpenFilePath().empty());

    // 3. 有文件路径 → 写入 pending，且由它推出的目标页正确
    resetArgvCaptureForTest();
    captureOpenFileFromArgv({"devdesk.exe", "note.md"});
    DOCTEST_CHECK_EQ(pendingOpenFilePath(), std::string("note.md"));
    DOCTEST_CHECK(app::assembly::dispatchPageForFile(pendingOpenFilePath()) ==
                  ::core::PageId::Markdown);

    // 4. 幂等：重复调用只捕获一次（第二次的路径不覆盖已捕获值）
    captureOpenFileFromArgv({"devdesk.exe", "other.md"});
    DOCTEST_CHECK_EQ(pendingOpenFilePath(), std::string("note.md"));

    // 收尾：不留进程级状态（pending 会被真实启动分发读取）
    resetArgvCaptureForTest();
    pendingOpenFilePath().clear();
    return true;
})

// --minimized 启动旗标捕获（UAT6 开机自启动）：注册项带本旗标，命中 → 主窗保持
// 托盘态。捕获门/待执行取走语义与 openFile 同款（幂等门 + 一次性取走）；执行在
// app::compose 首帧（窗口创建后才有主窗句柄），这里只测捕获/取走两侧。
REGISTER_TEST(assembly_capture_minimized_flag_from_argv,
              "assembly: --minimized 旗标捕获（有旗标→待执行 / 无旗标 / 与文件参数共存 / 幂等 / 取走一次）", {
    using app::assembly::captureMinimizedFlagFromArgv;
    using app::assembly::consumePendingMinimizedStartup;
    using app::assembly::resetMinimizedFlagForTest;

    // 1. 无旗标 → 捕获返回 false，无待执行
    resetMinimizedFlagForTest();
    DOCTEST_CHECK_FALSE(captureMinimizedFlagFromArgv({"devdesk.exe"}));
    DOCTEST_CHECK_FALSE(consumePendingMinimizedStartup());

    // 2. 有旗标 → 捕获 true；取走一次即清（第二次取走 false——compose 首帧一次性）
    resetMinimizedFlagForTest();
    DOCTEST_CHECK(captureMinimizedFlagFromArgv({"devdesk.exe", "--minimized"}));
    DOCTEST_CHECK(consumePendingMinimizedStartup());
    DOCTEST_CHECK_FALSE(consumePendingMinimizedStartup());

    // 3. 旗标与文件参数共存（开机拉起后又被分发打开文件）：旗标照常命中
    resetMinimizedFlagForTest();
    DOCTEST_CHECK(captureMinimizedFlagFromArgv(
        {"devdesk.exe", "--minimized", "note.md"}));
    DOCTEST_CHECK(consumePendingMinimizedStartup());

    // 4. 幂等：首次捕获已定，第二次调用（不同 argv）不覆盖——但返回首次捕获结果
    resetMinimizedFlagForTest();
    DOCTEST_CHECK(captureMinimizedFlagFromArgv({"devdesk.exe", "--minimized"}));
    DOCTEST_CHECK(captureMinimizedFlagFromArgv({"devdesk.exe"}));  // 幂等返回：首次已捕获--minimized=true
    DOCTEST_CHECK(consumePendingMinimizedStartup());

    // 5. 同族前缀不认（--minimized=1 不是 --minimized）
    resetMinimizedFlagForTest();
    DOCTEST_CHECK_FALSE(
        captureMinimizedFlagFromArgv({"devdesk.exe", "--minimized=1"}));
    DOCTEST_CHECK_FALSE(consumePendingMinimizedStartup());

    // 收尾：不留进程级状态（真实启动的 compose 首帧会读它）
    resetMinimizedFlagForTest();
    return true;
})

// 空态兜底渲染的注入接线：core 侧的 Router 不自行绘制「无页面」占位（eui::Ui 只可透传，
// 文案与样式不能住在 core），占位搬到装配段注入。本用例证明装配后「当前页无模块」真的会
// 画出 app 层的那一块。
REGISTER_TEST(assembly_injects_empty_page_fallback,
              "assembly: 装配时注入空态兜底（无模块可显示 → 「无页面」由 app 层画）", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    // 传空 GatedEntry 列表：装配后一个模块都没注册 → compose 落到「当前页无模块」。
    // 这是经装配后的 Router 唯一走得到兜底的形态——生产恒注册 home/settings，Home 没注册时
    // navigateTo 也只会停在当前页。
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
    // 这里只钉了占位节点的 id "empty"。改这串字面量本用例会红；至于那块占位画出来长什么样，
    // 不在本用例的判定范围内。
    DOCTEST_CHECK(ui.find("empty") != nullptr);
    return true;
})

// 装配契约：preStart 注册的服务 → 模块 onStart 可解析；未注册 → 显式缺失

REGISTER_TEST(assembly_service_locator_registration_precedes_onstart,
              "assembly: R0/D6 —— preStart 注册的服务在模块 onStart 可解析（未注册则显式缺失）", {
    // 场景 1：preStart 里注册（生产的适配器注册点在 main.cpp 同一位置——严格早于模块
    // onStart）→ 模块 onStart 解析到同一实例
    {
        test_support::ScopedAppRouter scoped;
        ::core::Router router;
        std::vector<std::string> log;
        ProbeService probe;
        ProbeConsumerModule probeModule(&log);
        // RAII 记账：作用域结束把槽位还原成「未注册」。用例 fail-fast 提前 return 也不会
        // 把栈上 probe 的地址留在进程级槽里，给同进程后续用例招来幽灵解析。
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

// 真实 kModuleRegistry 表：门控键列填错或表序漂移，在此必挂

REGISTER_TEST(assembly_registry_table_shape,
              "assembly: 真实 kModuleRegistry —— 11 行 / 行序==PageId 枚举序 / home+schedule+settings+about gatingKey==nullptr / 其余七行==kModuleId*", {
    using ::app::registry::kModuleRegistry;
    constexpr size_t kRegistrySize =
        sizeof(kModuleRegistry) / sizeof(kModuleRegistry[0]);
    const ::core::PageId kEnumOrder[] = {
        ::core::PageId::Home,       ::core::PageId::Markdown,
        ::core::PageId::Todo,       ::core::PageId::Schedule,
        ::core::PageId::ClaudeProxy, ::core::PageId::Agent,
        ::core::PageId::Memory,     ::core::PageId::Wallpaper,
        ::core::PageId::Mcp,        ::core::PageId::Settings,
        ::core::PageId::About,
    };
    DOCTEST_CHECK_EQ(kRegistrySize, static_cast<size_t>(11));
    for (size_t i = 0; i < kRegistrySize; ++i) {
        // 表序 == PageId 枚举序（重排 = 侧边栏序静默变化，这是 core_pageid_order 契约的
        // 装配侧镜像；Agent/Mcp 必须排在 Settings 之前）
        DOCTEST_CHECK(kModuleRegistry[i].page == kEnumOrder[i]);
    }
    // home/schedule/settings/about 恒注册（gatingKey==nullptr；schedule 的模块开关
    // 卡片后续波次补）；七个工具模块各自带对应门控键。
    // 空指针必须走 null-safe 取值：直接 std::string(nullptr) 是 UB，clang -Wnonnull 会当
    // 编译错误拦下——这个 lambda 就是为了让 nullptr 与非空串落在同一条比较路径上。
    auto gating = [](const char* key) {
        return key != nullptr ? std::string(key) : std::string("<null>");
    };
    DOCTEST_CHECK(kModuleRegistry[0].gatingKey == nullptr);
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[1].gatingKey),
                     std::string(module::settings::kModuleIdMarkdown));
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[2].gatingKey),
                     std::string(module::settings::kModuleIdTodo));
    DOCTEST_CHECK(kModuleRegistry[3].gatingKey == nullptr);
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[4].gatingKey),
                     std::string(module::settings::kModuleIdClaudeProxy));
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[5].gatingKey),
                     std::string(module::settings::kModuleIdAgent));
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[6].gatingKey),
                     std::string(module::settings::kModuleIdMemory));
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[7].gatingKey),
                     std::string(module::settings::kModuleIdWallpaper));
    DOCTEST_CHECK_EQ(gating(kModuleRegistry[8].gatingKey),
                     std::string(module::settings::kModuleIdMcp));
    DOCTEST_CHECK(kModuleRegistry[9].gatingKey == nullptr);
    DOCTEST_CHECK(kModuleRegistry[10].gatingKey == nullptr);
    return true;
})

REGISTER_TEST(assembly_build_gated_entries_real_table,
              "assembly: buildGatedEntries 真实表 —— all-off 恰 4 条（home/schedule/settings/about 恒注册，mcp 默认关）/ all-on 11 条按表序", {
    constexpr size_t kRegistrySize =
        sizeof(::app::registry::kModuleRegistry) /
        sizeof(::app::registry::kModuleRegistry[0]);
    // 全关（七个工具模块）→ 只剩 home/schedule/settings/about 四条恒注册行。
    // 锁的是**真实表**：gatingKey 列填错这里必挂——手搓清单的用例（homeSettingsOnly）
    // 覆盖不到这一层。
    const auto gated = app::assembly::buildGatedEntries(
        allToolsOffConfig(), ::app::registry::kModuleRegistry,
        kRegistrySize);
    DOCTEST_REQUIRE_EQ(gated.size(), static_cast<size_t>(4));
    DOCTEST_CHECK(gated[0].page == ::core::PageId::Home);
    DOCTEST_CHECK(gated[1].page == ::core::PageId::Schedule);
    DOCTEST_CHECK(gated[2].page == ::core::PageId::Settings);
    DOCTEST_CHECK(gated[3].page == ::core::PageId::About);
    // 实例列恒为 nullptr——由调用方按 page 从自有 static 持有填入
    DOCTEST_CHECK(gated[0].module == nullptr);
    DOCTEST_CHECK(gated[1].module == nullptr);
    DOCTEST_CHECK(gated[2].module == nullptr);
    DOCTEST_CHECK(gated[3].module == nullptr);
    // 全开 → 11 条且序 == 表序
    const auto allGated = app::assembly::buildGatedEntries(
        allOnConfig(), ::app::registry::kModuleRegistry, kRegistrySize);
    DOCTEST_REQUIRE_EQ(allGated.size(), static_cast<size_t>(11));
    const ::core::PageId kEnumOrder[] = {
        ::core::PageId::Home,       ::core::PageId::Markdown,
        ::core::PageId::Todo,       ::core::PageId::Schedule,
        ::core::PageId::ClaudeProxy, ::core::PageId::Agent,
        ::core::PageId::Memory,     ::core::PageId::Wallpaper,
        ::core::PageId::Mcp,        ::core::PageId::Settings,
        ::core::PageId::About,
    };
    for (size_t i = 0; i < allGated.size(); ++i) {
        DOCTEST_CHECK(allGated[i].page == kEnumOrder[i]);
    }
    return true;
})

// 表短名 / 图标列 ↔ 模块虚函数的双副本一致性。静态列不可省：托盘关闭态没有任何活实例可查，
// 只能读表；这条用例是双副本唯一的缝合点（无 DB，只取纯 getter）。
REGISTER_TEST(assembly_registry_matches_module_getters,
              "assembly: 表 shortName/iconGlyph/page 与真实模块 getter 一致（防双副本 drift；wallpaper 平台不支持时工厂可为空）", {
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> mod(e.factory());
        if (e.page == ::core::PageId::Wallpaper && mod == nullptr) {
            // 唯一允许的空工厂行：平台能力不支持时按契约返回 nullptr（开关键仍是 wallpaper）
            DOCTEST_CHECK_FALSE(::platform::wallpaper::supported());
            // 先判空再构造 std::string：std::string(nullptr) 是 UB（clang -Wnonnull 直接拦）
            DOCTEST_REQUIRE(e.gatingKey != nullptr);
            DOCTEST_CHECK_EQ(std::string(e.gatingKey),
                             std::string(module::settings::kModuleIdWallpaper));
            continue;
        }
        DOCTEST_REQUIRE(mod != nullptr);
        DOCTEST_CHECK(e.page == mod->pageId());
        DOCTEST_CHECK_EQ(std::string(e.shortName), mod->shortName());
        DOCTEST_CHECK_EQ(std::string(e.iconGlyph), mod->iconGlyph());
    }
    return true;
})

// 托盘委托判定：Navigate* 委托成功切页，其余动作不导航

REGISTER_TEST(assembly_dispatch_tray_navigation,
              "assembly: dispatchTrayNavigation —— Navigate* 委托成功切页返回 true / ShowMainWindow·Exit·None 返回 false 不导航", {
    test_support::ScopedAppRouter scoped;
    ::core::Router router;
    StubModules mods;
    app::assembly::assembleAndStart(router, allOnConfig(), mods.set(),
                                    app::assembly::PreStartHook{});
    DOCTEST_REQUIRE_EQ(router.modules().size(), static_cast<size_t>(6));

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

// 托盘进未启用页 → 回首页 + toast，文案必须是「即时启用」口径（与活门控一致），不许回退成
// 「重启后生效」。只注册 Home（markdown 未装配）来构造未启用场景。
REGISTER_TEST(assembly_navigate_from_tray_unregistered_toast_copy,
              "assembly: navigateFromTray 未启用页 → 回首页 + toast 即时生效口径（无重启字样）", {
    test_support::ScopedAppRouter scoped;
    std::vector<std::string> log;
    StubModule home(::core::PageId::Home, "首页", &log);
    ::core::Router router;
    router.registerModule(&home);
    app::clearAppNotice();  // 进程级 toast 回厂（本用例只认自己触发的通知）

    app::assembly::navigateFromTray(&router, ::core::PageId::Markdown);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Home);
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK_EQ(app::appNoticeTitle(), std::string("模块已关闭"));
    const std::string msg = app::appNoticeMessage();
    DOCTEST_CHECK(msg.find("笔记") != std::string::npos);  // shortName 单源
    DOCTEST_CHECK(msg.find("设置 → 模块开关") != std::string::npos);  // 入口指引保留
    DOCTEST_CHECK(msg.find("即时") != std::string::npos);  // 即时口径在文
    DOCTEST_CHECK(msg.find("重启") == std::string::npos);  // 不许回退成「重启后生效」

    app::clearAppNotice();  // 收尾回厂，不留给后续用例
    return true;
})

// 活门控（live-toggle）dispatchModuleToggle：计数桩 + 查找缝
// ModuleLookup 是裸函数指针（无捕获约束）→ 桩与计数只能走文件级静态、自由函数直接引用。
// 用例开头回厂计数与 toast 状态（进程级，fail-fast 提前 return 也不污染后续用例）。
namespace {

int g_agentStarts = 0;
int g_agentStops = 0;
int g_homeStarts = 0;
int g_homeStops = 0;
// 时序账本：seam 调用与桩模块 onStart 各取一个自增序号，用来断言 seam 早于 onStart
int g_seqCounter = 0;

class ToggleCountModule : public ::core::Module {
public:
    ToggleCountModule(::core::PageId page, std::string name, int* starts,
                      int* stops, int* seq = nullptr)
        : page_(page), name_(std::move(name)), starts_(starts), stops_(stops),
          seq_(seq) {}
    // onStop 时序观测：onStop 里登记「此刻本模块是否仍注册」。off 路径要求 onStop 先于
    // unregisterModule——顺序颠倒（先注销再停）时 isRegistered 已为 false，
    // 断言 stopWhileRegistered == stops 必红
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
    // 生产形态：Home 恒注册（导航兜底与关模块时的重定向 guard 都依赖它）
    router.registerModule(&g_toggleHome);
    g_toggleAgent.observeRouter(&router);  // onStop 时序观测

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

    // 再开 → 把 Agent 设为当前页 → 再关 → 重定向 Home + toast
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_agentStarts, 2);
    router.navigateTo(::core::PageId::Agent);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Agent);
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, false,
                                        router, &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_agentStops, 2);
    // 两次 onStop 都发生在「本模块仍注册」时 = onStop 先于 unregisterModule。顺序颠倒必红：
    // 运行线程是在 onStop 内 join 完成后页面才消失。
    DOCTEST_CHECK_EQ(g_toggleAgent.stopWhileRegistered(), 2);
    DOCTEST_CHECK_EQ(router.currentPage(), ::core::PageId::Home);
    // toast 文案：shortName 取自注册表静态列（不依赖活实例）
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

    // 未知 moduleId → no-op
    app::assembly::dispatchModuleToggle("no_such_module", true, router,
                                        &toggleLookupStub);
    DOCTEST_CHECK_EQ(g_agentStarts, 2);
    DOCTEST_CHECK_EQ(g_agentStops, 2);

    app::clearAppNotice();  // 收尾回厂，不留给后续用例
    return true;
})

// 惰性装配：dispatch on 路径 → 第二桥 → 真服务。seam 注入**真服务 handler**，逻辑等价 main.cpp
// 的 ensureClaudeServiceAssembledImpl（装配单例 → setConfiguredProxyPort → recover → autoStart
// 才起代理），驱动真 ClaudeService（devdesk_tests 链 devdesk_module_claude_proxy）。
// 唯一刻意偏差：startProxy 用端口 0 让 OS 分配，不用 configuredProxyPort()——配置口径默认
// 8888，会与生产 / 其他 worktree 的并行测试撞端口。devdesk_tests 不链接 main.cpp，所以这里钉的
// 是 dispatch→seam 契约与真 service 可观察的幂等语义，「真 main.cpp 的 getClaudeService 被触发」
// 不在本链接面。
// 进程全局态纪律：ClaudeService::setGlobal / 桥 handler / toast 都是进程级，RAII 守卫析构必须
// 全还原（fail-fast 提前 return 也不泄漏给后续用例）。
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
    // 端口自读：SettingsStore(::core::appDatabase()).load()（与 main 的
    // assembleClaudeServiceNow 同源；测试配置经用例注册的临时库隔离）
    const module::settings::SettingsStore store(::core::appDatabase());
    const auto cfg = store.load();
    g_asmSvc->setConfiguredProxyPort(cfg.proxyPort);
    g_asmSvc->recoverPendingSettingsRestore();
    if (!autoStart) return;
    if (g_asmSvc->isProxyRunning()) return;               // main 的早退语义
    auto v = g_asmSvc->validateProxyPreconditions();      // 前置校验：不过就不 bind
    if (v.isFail()) {
        // 会话内路径（用户动作）→ 应用级 notice（镜像 main.cpp 的 notifyFailure=true 分支；
        // 真 main.cpp 不在本链接面）
        app::showAppNotice("代理未启动", v.errorMessage());
        return;                                           // 零副作用失败
    }
    auto r = g_asmSvc->startProxy("127.0.0.1", 0);        // 端口 0 = OS 分配
    ++g_asmStartCalls;
    g_asmStartOk = r.isOk();
}

// 全局态还原守卫：析构 = stopProxy + setGlobal(原值) + handler 复位 nullptr +
// clearAppNotice + 桩服务销毁（进程全局态全还原，含 fail-fast 早退路径）
// 声明序契约：guard **必须晚于** TestDb 声明（析构逆序 ⇒ 服务先于库死亡）——
// ClaudeService 持 SqliteDb& 以及经它构造的 ProfileRepository，
// 库先关再停服务 = 悬垂引用。
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
// 返回 bool：DOCTEST_CHECK 宏含 `return false`，只能用在返回 bool 的函数里；断言由调用方
// （用例体）做
bool writeLazyAssemblyConfig(bool autoStart) {
    module::settings::SettingsConfig cfg;
    cfg.enabledModules = {module::settings::kModuleIdHome,
                          module::settings::kModuleIdMarkdown,
                          module::settings::kModuleIdTodo,
                          module::settings::kModuleIdSettings,
                          module::settings::kModuleIdAgent};
    cfg.claudeProxyAutoStart = autoStart;
    return module::settings::SettingsStore(::core::appDatabase()).save(cfg).isOk();
}

} // namespace

REGISTER_TEST(assembly_dispatch_claude_lazy_assemble_autostart,
              "assembly: 会话内开 claude → dispatch on 先惰性装配真服务（global 非空+代理起+seam 序<onStart 序+重复 on 幂等+off/非 claude 不触缝）", {
    resetLazyAssemblyLedger();
    // 声明序：TestDb **必须**在 LazyAssemblyGuard 之前声明——作用域退出按声明逆序析构，
    // 守卫析构里的 stopProxy / 服务 reset 需要 DB 还活着（ClaudeService 持 SqliteDb& 引用，
    // ProfileRepository 经它构造）。反过来就是服务在「库已关闭」之后才停机：
    // 潜伏 UAF，只在收尾处炸成高调试成本 flake。
    test_support::TestDb asmDb("assembly_lazy", "autostart_db");
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_lazy", "autostart"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(true));

    // 会话内 auto-start 前必须过前置校验（激活 profile + 连通测试），所以这里要先起 mock 上游
    // 并种下激活 profile。在**空 profile store** 上断言「代理已起」是错的方向——那正是事故
    // 语义（空库那条用例钉的就是这一侧）。
    platform::http::Server upstream("127.0.0.1", 0);
    upstream.route("POST", "/v1/messages",
                   [](const platform::http::RequestCtx&,
                      platform::http::ResponseWriter& w) {
                       w.setStatus(200);
                       w.setHeader("Content-Type", "application/json");
                       w.write(R"({"data":[{"id":"claude-sonnet-4-5"}]})");
                       w.end();
                   });
    DOCTEST_REQUIRE(upstream.start());
    g_asmDb = &asmDb;
    app::toggle::setClaudeAssemblyHandler(&assemblyHandlerStub);

    // 先装配服务并种激活 profile（等价生产：用户已在 Claude 页启用过一套）
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
    // ② 时序：seam 调用严格先于桩模块 onStart
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
    // 声明序同上：DB 必须先于守卫声明 → 后于守卫析构
    test_support::TestDb asmDb("assembly_lazy", "noautostart_db");
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_lazy", "noautostart"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(false));

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
        DOCTEST_CHECK_FALSE(svc->isProxyRunning());  // 代理未起
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

// 核心回归（事故现场）：空 profile store + autoStart=true → 前置校验挡下——代理不起、
// 不 bind、零接管，且用户可见面给出原因
REGISTER_TEST(assembly_dispatch_claude_toggle_blocked_by_preconditions,
              "assembly: 会话内开 claude + autoStart，空 profile store → 前置校验挡下（代理不起/不接管）", {
    resetLazyAssemblyLedger();
    // 声明序同上：DB 必须先于守卫声明 → 后于守卫析构
    test_support::TestDb asmDb("assembly_pv", "noactive_db");       // 空库 = 事故现场
    LazyAssemblyGuard guard;

    test_support::ScopedConfigPath scoped(
        test_support::tempConfigFile("assembly_pv", "noactive"));
    DOCTEST_CHECK(writeLazyAssemblyConfig(true));

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
    // 用户可见面（桩镜像 main.cpp 的 showAppNotice 分支）
    DOCTEST_CHECK(app::appNoticeVisible());
    DOCTEST_CHECK(app::appNoticeTitle() == std::string("代理未启动"));
    DOCTEST_CHECK(app::appNoticeMessage().find("前置校验未通过") != std::string::npos);
    return true;
})

namespace {

// 通知 initialize 接线 spy（M1）：只数 initialize 下发次数
struct InitSpyBackend : platform::notify::Backend {
    int initializeCalls = 0;
    void initialize() override { ++initializeCalls; }
    core::Result<platform::notify::Status> send(
        const platform::notify::Notification&) override {
        return core::Result<platform::notify::Status>::ok(
            platform::notify::Status::Accepted);
    }
    platform::notify::Authorization authorizationStatus() override {
        return platform::notify::Authorization::Authorized;
    }
    void requestAuthorization() override {}
};

} // namespace

// M1 装配接线：assembleAndStart 必须在 startAll 之前调一次 notify::initialize
//（macOS 授权预热 + DidBecomeActive 缓存重置观察者都挂在它身上，不接线即死代码）。
// 红证据：装配层不调 initialize 时 initializeCalls 恒 0，首断言必红。
// 幂等面：同进程第二次装配，facade 的 g_initialized 闩保证不重复下发。
REGISTER_TEST(assembly_wires_notification_initialize_once,
              "assembly: assembleAndStart 恰调一次 notify::initialize（M1 接线 + 幂等不重复）", {
    test_support::ScopedAppRouter scoped;
    platform::notify::resetForTest();   // 清 g_initialized 幂等闩（先前用例可能已置位）
    InitSpyBackend spy;
    platform::notify::setBackendForTest(&spy);

    {
        ::core::Router router;
        StubModules mods;
        const auto order = app::assembly::assembleAndStart(
            router, allToolsOffConfig(), mods.homeSettingsOnly(),
            app::assembly::PreStartHook{});
        DOCTEST_REQUIRE_EQ(order.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(spy.initializeCalls, 1);
    }
    {
        // 同进程第二次装配：initialize 不重复（幂等闩在 facade，装配层无自行去重责任）
        ::core::Router router2;
        StubModules mods2;
        const auto order2 = app::assembly::assembleAndStart(
            router2, allToolsOffConfig(), mods2.homeSettingsOnly(),
            app::assembly::PreStartHook{});
        DOCTEST_REQUIRE_EQ(order2.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(spy.initializeCalls, 1);
    }

    platform::notify::setBackendForTest(nullptr);
    platform::notify::resetForTest();
    return true;
})
