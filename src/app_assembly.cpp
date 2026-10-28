// 应用装配层实现，约定见 app_assembly.h
#include "app_assembly.h"

#include "app_sidebar.h"
#include "core/logging.h"
#include "core/database.h"
#include "mcp_service_bridge.h"
#include "module_markdown/md_file_open.h"
#include "module_toggle_bridge.h"
#include "platform/autostart.h"
#include "platform/file_open.h"
#include "platform/notification.h"
#include "settings_config.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace app::assembly {

namespace {

// 退出接线是否已挂（atexit 无法撤销，只记账不重复挂）
bool g_exitHookArmed = false;

// argv 捕获门：仅第一次调用生效
bool g_argvCaptureDone = false;

// --minimized 捕获门（与 openFile 同款幂等）+ 待执行旗标（命中后由 compose 首帧取走）
bool g_minimizedCaptureDone = false;
bool g_minimizedStartupPending = false;

// 捕获落点：写进程级 pending 路径；空路径（无文件参数/只有 flag）不写
void capturePendingOpenFile(const std::string& path) {
    if (g_argvCaptureDone) return;
    g_argvCaptureDone = true;
    if (path.empty()) return;

    pendingOpenFilePath() = path;
    LOG_INFO("捕获到待打开文件（命令行 argv）: " + pendingOpenFilePath());
}

} // namespace

std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const std::vector<GatedEntry>& modules, const PreStartHook& preStart) {
    (void)cfg;  // 门控已由 buildGatedEntries 前置，不再读取

    if (preStart) {
        preStart();
    }

    ::core::registerAppRouter(&router);

    // 2b) 空态兜底渲染注入：当前页无对应模块时 Router 不自行绘制，改由 app 层注入
    router.setFallbackCompose([](eui::Ui& ui, const eui::Screen&) {
        ui.text("empty").text("无页面").build();
    });

    std::vector<::core::PageId> registered;
    const auto registerOne = [&](::core::Module* module) {
        if (module == nullptr) {
            return;  // Router::registerModule 同款容错
        }
        router.registerModule(module);
        registered.push_back(module->pageId());
    };

    // 3) 注册门控：清单已前置，这里纯注册、序=表序；不注册 = onStart/compose/菜单项不可达
    for (const auto& entry : modules) {
        registerOne(entry.module);
    }

    // 通知授权预热（装配点一次性）：主线程 initialize——macOS 安装 DidBecomeActive
    // 缓存重置观察者 + 异步查询授权（NotDetermined 时主队列发起授权请求），并填掉
    // 授权缓存，后续 send 的授权检查走缓存、不再有 ≤1s 同步查询。必须早于任何模块
    // 可发通知（startAll 之前）；幂等，Windows/POSIX no-op
    platform::notify::initialize();

    router.startAll();

    armExitHook();
    return registered;
}

std::vector<GatedEntry> buildGatedEntries(
    const module::settings::SettingsConfig& cfg,
    const ::app::registry::ModuleEntry* table, std::size_t tableSize) {
    // 恒注册行必含，门控行 moduleEnabled(cfg,key) 才含，序=表序；实例列恒 nullptr 由调用方填
    std::vector<GatedEntry> gated;
    for (std::size_t i = 0; i < tableSize; ++i) {
        const auto& e = table[i];
        if (e.gatingKey == nullptr ||
            module::settings::moduleEnabled(cfg, e.gatingKey)) {
            gated.push_back(GatedEntry{e.page, e.gatingKey, nullptr});
        }
    }
    return gated;
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

// 测试缝：退出接线记账门回厂（未挂）。atexit 不可撤销，只翻记账位使 armExitHook 可再触发
void resetAssemblyGlobals() { g_exitHookArmed = false; }

void runExitHook() {
    if (auto* r = ::core::appRouter()) {
        r->stopAll();
    }
}

// 待打开文件路径：命令行 argv（启动时）/ Apple Event（Finder 双击、打开方式）两条来源都写这里
std::string& pendingOpenFilePath() {
    static std::string path;
    return path;
}

void captureOpenFileFromArgv() {
    // 平台层读进程自身命令行（argv[1] 是文件路径；跳过 flag 类参数）
    capturePendingOpenFile(platform::file_open::openFileFromArgv());
}

void captureOpenFileFromArgv(const std::vector<std::string>& utf8Args) {
    capturePendingOpenFile(platform::file_open::selectOpenFileArg(utf8Args));
}

void resetArgvCaptureForTest() { g_argvCaptureDone = false; }

void captureMinimizedFlagFromArgv() {
    // 平台层读进程自身命令行（EUI-NEO 的 main 不接 argc/argv 形参，先例同 openFileFromArgv）
    captureMinimizedFlagFromArgv(platform::autostart::processArgsUtf8());
}

bool captureMinimizedFlagFromArgv(const std::vector<std::string>& utf8Args) {
    if (g_minimizedCaptureDone) return g_minimizedStartupPending;
    g_minimizedCaptureDone = true;
    if (platform::autostart::parseMinimizedFlag(utf8Args)) {
        g_minimizedStartupPending = true;
        LOG_INFO("--minimized 启动：窗口保持托盘态");
    }
    return g_minimizedStartupPending;
}

void resetMinimizedFlagForTest() {
    g_minimizedCaptureDone = false;
    g_minimizedStartupPending = false;
}

bool consumePendingMinimizedStartup() {
    return std::exchange(g_minimizedStartupPending, false);
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
        // 已先判 isOk()：value() 失败即 LOG_FATAL+abort
        auto r = module::markdown::captureFileIntoNotesDir(notesDir, path);
        if (r.isOk()) {
            const std::string payload = "open:" + r.value();
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
        case Action::NavigateWallpaper: return ::core::PageId::Wallpaper;
        default: return std::nullopt;
    }
}

const char* trayPageDisplayName(::core::PageId page) {
    // 短名单源 = app_registry 表（静态元数据，无需活实例，关闭模块的提示文案仍可查）
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page == page) {
            return e.shortName;
        }
    }
    return "?";
}

bool dispatchTrayNavigation(::core::tray::TrayMenuAction action,
                            ::core::Router& router) {
    const auto page = pageForTrayAction(action);
    if (!page.has_value()) {
        return false;  // 非导航动作（ShowMainWindow/Exit/None）由调用方处理
    }
    navigateFromTray(&router, *page);
    return true;
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
    // 与设置卡/开关即时生效同口径：保留启用入口指引
    app::showAppNotice("模块已关闭",
                       std::string("「") + trayPageDisplayName(page) +
                           "」模块未启用，已转到首页。可在 设置 → 模块开关 "
                           "中即时启用。");
    app::requestUpdate();
}

void dispatchModuleToggle(const std::string& moduleId, bool on,
                          ::core::Router& router, ModuleLookup lookup) {
    // mcp 双面性（UI 批起）：既是 Router 页面模块（注册表有其行）又挂服务生命周期
    // ——先启停服务，再**不提前返回**落入下方遍历做页面注册/注销。缺任一半都会破
    // 「关闭 = 侧边栏入口消失」的门控承诺（settings_ui_cards 模块卡文案的当帧更新）。
    if (moduleId == module::settings::kModuleIdMcp) {
        if (on) {
            const module::settings::SettingsStore store(::core::appDatabase());
            auto r = app::mcp::start(store.load().mcpPort);
            if (r.isFail()) {
                // 用户动作失败要可见：toast 通知（启动期自启不经过这里）；服务失败
                // 不阻断页面注册——页面状态行如实显示「已停止 + 错误」
                app::showAppNotice("MCP 服务未启动", r.errorMessage());
                LOG_ERROR("模块开关: MCP 服务启动失败: " + r.errorMessage());
            }
        } else {
            app::mcp::stop();
        }
    }
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.gatingKey == nullptr || moduleId != e.gatingKey) {
            continue;  // 恒开行不参与拨开关；其余行键不匹配继续找
        }
        ::core::Module* mod = lookup(e.page);
        if (mod == nullptr) {
            LOG_WARN("模块开关: " + moduleId + " 无实例，跳过即时生效");
            return;
        }
        if (on) {
            if (moduleId == module::settings::kModuleIdClaudeProxy) {
                // 惰性装配：启动期关、会话内开启时补装配，必须先于 registerModule/onStart
                //（compose 期读 ClaudeService::global()）；桥未装配时 no-op+LOG_WARN
                const module::settings::SettingsStore store(
                    ::core::appDatabase());
                app::toggle::ensureClaudeServiceAssembled(
                    store.load().claudeProxyAutoStart);
            }
            router.registerModule(mod);  // 幂等（重复注册 = no-op）
            mod->onStart();
        } else {
            // 先停后注销：运行线程在 onStop 内 join，页消失前收束
            mod->onStop();
            router.unregisterModule(mod);
            // 当前页被注销 → 回首页 + toast（isRegistered(Home) 是防御分支）
            if (router.currentPage() == e.page &&
                router.isRegistered(::core::PageId::Home)) {
                router.navigateTo(::core::PageId::Home);
                app::showAppNotice("模块已关闭",
                                   std::string(e.shortName) +
                                       " 已关闭，已返回首页");
            }
        }
        return;
    }
    LOG_WARN("模块开关: 未知或恒开模块 " + moduleId + "，忽略");
}

} // namespace app::assembly
