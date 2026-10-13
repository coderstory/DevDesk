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
// 本类只提供**已门控**的清单形态；真实表的门控结果由 kModuleRegistry 与 buildGatedEntries
// 两个用例锁，不在这里复刻。
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

// 全启用配置：显式列全门控键（含默认清单里没有的 agent），「all-on = 7 项」按这个构造口径保住
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

    }

// 只启用 home + markdown（dispatchFileOpen 的正常路径用）
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

