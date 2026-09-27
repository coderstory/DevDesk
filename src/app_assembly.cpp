// src/app_assembly.cpp
// 应用装配层实现（专家审计 2026-09-27 M2）：自 main.cpp 逐字搬运，行为零变化，
// 仅把「DB / 配置 / 路由来源」改为显式参数（便于无头单测替换）。搬运动机与
// 装配顺序约定见 app_assembly.h。
#include "app_assembly.h"

#include "app_sidebar.h"  // app::showAppNotice（托盘失效入口的解释反馈）
#include "core/logging.h"
#include "module_markdown/md_file_open.h"
#include "platform/file_open.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>  // app::requestShow / requestUpdate / requestExit

#include <string>
#include <vector>

namespace app::assembly {

namespace {

// 退出接线是否已挂（armExitHook 幂等；atexit 无法撤销，只记账不重复挂）
bool g_exitHookArmed = false;

} // namespace

std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const ModuleSet& modules, const PreStartHook& preStart) {
    using module::settings::moduleEnabled;

    // 1) 前置：ClaudeService 创建 + setGlobal（模块 onStart 依赖它非 null）
    if (preStart) {
        preStart();
    }

    // 2) 注册给 module_* 层（快捷按钮/卡片点击通过 core::appRouter() 导航）
    ::core::registerAppRouter(&router);

    std::vector<::core::PageId> registered;
    const auto registerOne = [&](::core::Module* module) {
        if (module == nullptr) {
            return;  // Router::registerModule 同款容错
        }
        router.registerModule(module);
        registered.push_back(module->pageId());
    };

    // 3) 注册门控（方案 A）：home/settings 恒注册；三个工具模块按 enabledModules
    // 决定——不注册 = onStart/compose/菜单项全部不可达，"零执行零菜单项"是
    // 结构性保证，不靠模块自觉检查开关
    registerOne(modules.home);
    if (moduleEnabled(cfg, module::settings::kModuleIdMarkdown)) {
        registerOne(modules.markdown);
    } else {
        LOG_INFO("模块已关闭（enabledModules 不含 markdown），不注册");
    }
    if (moduleEnabled(cfg, module::settings::kModuleIdTodo)) {
        registerOne(modules.todo);
    } else {
        LOG_INFO("模块已关闭（enabledModules 不含 todo），不注册");
    }
    if (moduleEnabled(cfg, module::settings::kModuleIdClaudeProxy)) {
        registerOne(modules.claude);
    } else {
        LOG_INFO("模块已关闭（enabledModules 不含 claude_proxy），不注册");
    }
    registerOne(modules.settings);

    // 4) 各模块 onStart（此刻 preStart 已跑完，ClaudeService::global() 非空）
    router.startAll();

    // 5) 退出接线（LIFO 约定见头文件）
    armExitHook();
    return registered;
}

bool armExitHook() {
    if (g_exitHookArmed) {
        return false;
    }
    std::atexit([] {
        if (auto* r = ::core::appRouter()) {
            r->stopAll();
        }
    });
    g_exitHookArmed = true;
    return true;
}

bool exitHookArmed() { return g_exitHookArmed; }

void runExitHook() {
    if (auto* r = ::core::appRouter()) {
        r->stopAll();
    }
}

// 全局：Finder "打开方式" 传入的文件路径
// 两条来源都写这里：命令行 argv（启动时）/ Apple Event（Finder 双击、打开方式）
std::string& pendingOpenFilePath() {
    static std::string path;
    return path;
}

void captureOpenFileFromArgv() {
    static bool captured = false;
    if (captured) return;
    captured = true;

    // argv[1] 是文件路径；跳过 flag 类参数（以 '-' 开头）
    const std::string path = platform::file_open::openFileFromArgv();
    if (path.empty()) return;

    pendingOpenFilePath() = path;
    LOG_INFO("捕获到待打开文件（命令行 argv）: " + pendingOpenFilePath());
}

::core::PageId dispatchPageForFile(const std::string& path) {
    auto endsWith = [&](const char* suffix) {
        size_t n = std::string(suffix).size();
        return path.size() >= n &&
               path.compare(path.size() - n, n, suffix) == 0;
    };
    if (endsWith(".md") || endsWith(".markdown")) return ::core::PageId::Markdown;
    if (endsWith(".json")) return ::core::PageId::ClaudeProxy;
    if (endsWith(".js") || endsWith(".html") || endsWith(".htm") ||
        endsWith(".txt")) return ::core::PageId::Markdown;  // 文本类用 Markdown 模块查看
    return ::core::PageId::Home;
}

void dispatchFileOpen(::core::Router* router, const std::string& path,
                      const std::filesystem::path& notesDir) {
    if (router == nullptr || path.empty()) return;
    const ::core::PageId target = dispatchPageForFile(path);
    if (target == ::core::PageId::Markdown &&
        router->isRegistered(::core::PageId::Markdown)) {
        // main.cpp 用 -fno-exceptions 编译：取失败信息外的值走 unsafeValue()
        auto r = module::markdown::captureFileIntoNotesDir(notesDir, path);
        if (r.isOk()) {
            const std::string payload = "open:" + r.unsafeValue();
            LOG_INFO("文件打开分发: " + path + " → " + payload);
            router->navigateTo(target, payload);
            return;
        }
        LOG_WARN("文件打开分发：无法纳入笔记目录，仅切页 — " + path + ": " +
                 r.errorMessage());
    }
    router->navigateTo(target);
}

std::optional<::core::PageId> pageForTrayAction(::core::tray::TrayMenuAction action) {
    using Action = ::core::tray::TrayMenuAction;
    switch (action) {
        case Action::NavigateHome: return ::core::PageId::Home;
        case Action::NavigateMarkdown: return ::core::PageId::Markdown;
        case Action::NavigateTodo: return ::core::PageId::Todo;
        case Action::NavigateClaude: return ::core::PageId::ClaudeProxy;
        default: return std::nullopt;
    }
}

const char* trayPageDisplayName(::core::PageId page) {
    switch (page) {
        case ::core::PageId::Home: return "首页";
        case ::core::PageId::Markdown: return "笔记";
        case ::core::PageId::Todo: return "待办";
        case ::core::PageId::ClaudeProxy: return "Claude";
        case ::core::PageId::Settings: return "设置";
    }
    return "?";
}

void navigateFromTray(::core::Router* router, ::core::PageId page) {
    if (router == nullptr) {
        return;
    }
    if (router->isRegistered(page)) {
        router->navigateTo(page);
        app::requestUpdate();
        return;
    }
    router->navigateTo(::core::PageId::Home);
    app::requestShow();
    app::showAppNotice("模块已关闭",
                       std::string("「") + trayPageDisplayName(page) +
                           "」模块未启用，已转到首页。可在 设置 → 模块开关 中启用，重启后生效。");
    app::requestUpdate();
}

} // namespace app::assembly
