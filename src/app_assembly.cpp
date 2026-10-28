// 应用装配层实现，约定见 app_assembly.h
#include "app_assembly.h"

#include "app_sidebar.h"
#include "core/logging.h"
#include "core/database.h"
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

// 捕获落点：写进程级 pending 路径；空路径（无文件参数/只有 flag）不写。
// source 只进日志，区分 argv 与平台队列两条来源。
void capturePendingOpenFile(const std::string& path, const char* source) {
    if (g_argvCaptureDone) return;
    g_argvCaptureDone = true;
    if (path.empty()) return;

    pendingOpenFilePath() = path;
    LOG_INFO("捕获到待打开文件（" + std::string(source) + "）: " +
             pendingOpenFilePath());
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
    // 先取 argv（命令行 / 文件关联），再取平台队列（macOS odoc Apple Event 桥入队）：
    // 两条来源合并进单一 pendingOpenFilePath。argv 优先，队列按入队序补位；
    // 单槽只承载一条，其余逐条告警丢弃（不静默）。
    const std::string fromArgv = platform::file_open::openFileFromArgv();
    std::vector<std::string> fromQueue =
        platform::file_open::takePendingOpenFiles();

    if (!fromArgv.empty()) {
        capturePendingOpenFile(fromArgv, "命令行 argv");
        for (const auto& path : fromQueue) {
            LOG_WARN("待打开文件队列：argv 已命中，本次忽略 " + path);
        }
        return;
    }
    if (fromQueue.empty()) {
        capturePendingOpenFile(std::string(), "命令行 argv/打开请求队列");
        return;  // 只翻幂等门，不写待打开路径
    }
    capturePendingOpenFile(fromQueue.front(), "系统打开请求队列");
    for (std::size_t i = 1; i < fromQueue.size(); ++i) {
        LOG_WARN("待打开文件队列：单槽仅承载一条，本次忽略 " + fromQueue[i]);
    }
}

void captureOpenFileFromArgv(const std::vector<std::string>& utf8Args) {
    // 测试缝：只喂 argv 参数，不触碰平台队列（保持参数化用例语义）
    capturePendingOpenFile(platform::file_open::selectOpenFileArg(utf8Args),
                           "命令行 argv（参数化）");
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
    // ASCII 小写化：只折 'A'-'Z'，不依赖 locale（tolower 受 locale 影响）
    const auto asciiLower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    // 后缀比较大小写不敏感（APFS/NTFS 默认不敏感）：只小写化文件名尾部与后缀，
    // 不整串拷贝。Windows 同步为大小写不敏感（原本对该平台即应如此）。
    const auto endsWith = [&](const char* suffix) {
        const size_t n = std::string(suffix).size();
        if (path.size() < n) return false;
        const size_t offset = path.size() - n;
        for (size_t i = 0; i < n; ++i) {
            if (asciiLower(path[offset + i]) != asciiLower(suffix[i])) {
                return false;
            }
        }
        return true;
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
    // 一个门控键可对应多行（多页模块，如 release 的任务/基础配置）：逐行拨，
    // 不能命中首行就返回。分组父项行无实例，跳过（其展开态由侧边栏独立维护）。
    // matched = 键命中登记表（与实例存在与否无关）：否则「键已知但实例缺失」会被
    // 追加一条「未知或恒开模块」的错误诊断，排障时误导向键名写错。
    bool matched = false;
    std::string missingPages;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.gatingKey == nullptr || moduleId != e.gatingKey) {
            continue;  // 恒开行不参与拨开关；其余行键不匹配继续找
        }
        matched = true;
        if (::app::registry::isGroupPage(e.page)) {
            continue;  // 分组父项不可导航、无实例
        }
        ::core::Module* mod = lookup(e.page);
        if (mod == nullptr) {
            if (!missingPages.empty()) missingPages += "/";
            missingPages += ::core::pageIdToString(e.page);
            continue;
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
    }
    if (!matched) {
        LOG_WARN("模块开关: 未知或恒开模块 " + moduleId + "，忽略");
    } else if (!missingPages.empty()) {
        // 键已命中、只是这些页面没有实例（如非支持平台/已注销）：单条准确日志，
        // 与「键未知」区分开
        LOG_WARN("模块开关: " + moduleId + " 已命中登记表，但页面 " +
                 missingPages + " 无实例，跳过即时生效");
    }
}

} // namespace app::assembly
