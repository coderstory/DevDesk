// 应用装配层实现，约定见 app_assembly.h
#include "app_assembly.h"

#include "app_sidebar.h"
#include "core/logging.h"
#include "core/database.h"
#include "core/notepad_file_types.h"  // 扩展名单一真源（漏斗/托盘/第二实例判据）
#include "mcp_service_bridge.h"
#include "module_markdown/md_file_open.h"  // captureFileIntoNotesDir（C-3 .md 漏斗改道）
#include "module_notepad/notepad_module.h"
#include "module_toggle_bridge.h"
#include "platform/autostart.h"
#include "platform/file_open.h"
#include "platform/notification.h"
#include "settings_config.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>
#include <eui/platform.h>  // core::platform::requestUiUpdate（bug307② 送达入队唤醒主循环）

#include <cctype>
#include <cstdlib>
#include <iterator>
#include <mutex>
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
bool g_mainWindowStartHidden = false;  // bug259-A：首帧不 show 闩值（mainWindowStartsHidden）
bool g_mainWindowComposed = false;  // bug283：主窗首帧 compose 闩（启动窗口期判据）

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

    // 未保存保护 quit guard（need263 批 4 F-05/D-3）：主循环消费退出请求时
    // 逐窗问 notepad 保护链（脏窗弹三选）；全干净/空注册表即放行，不拦
    // 无记事本窗的常规退出。实现 = module::notepad::notepadQuitGuard。
    app::setQuitGuard(module::notepad::notepadQuitGuard);

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
        g_mainWindowStartHidden = true;
        LOG_INFO("--minimized 启动：窗口保持托盘态");
    }
    return g_minimizedStartupPending;
}

void resetMinimizedFlagForTest() {
    g_minimizedCaptureDone = false;
    g_minimizedStartupPending = false;
    g_mainWindowStartHidden = false;
    g_mainWindowComposed = false;
}

bool consumePendingMinimizedStartup() {
    return std::exchange(g_minimizedStartupPending, false);
}

// ── F-01 第二实例路径队列 ──
// 线程契约见 app_assembly.h：写 = 单实例监听线程，读 = 主线程 compose drain。
// 漏斗在锁外调用（drain 先整批取走再逐个转发，漏斗重入不会死锁）
namespace {
std::mutex g_secondInstanceFilesMutex;
std::vector<std::string> g_secondInstanceFiles;
std::function<void(const std::string&)> g_osOpenFunnel;
} // namespace

void enqueueSecondInstancePaths(std::vector<std::string> paths) {
    if (paths.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_secondInstanceFilesMutex);
        g_secondInstanceFiles.insert(g_secondInstanceFiles.end(),
                                     std::make_move_iterator(paths.begin()),
                                     std::make_move_iterator(paths.end()));
    }
    LOG_INFO("单实例：第二实例送达 " + std::to_string(paths.size()) +
             " 个文件路径，待主线程分发");
    // bug307②：入队即唤醒主循环（监听线程的 SetEvent 只唤醒监听线程自身）。
    // 主循环可见空闲态在 glfwWaitEvents 无限期挂起，等下一次用户输入才 compose
    // 的话送达路径有延迟；requestUiUpdate 是跨线程生产验证过的唤醒出口（流质泵
    // 线程 / claude 后台任务同款——单 requestFrame 的 postEmptyEvent 在本机
    // GLFW 上实测不生效，2026-11-07 bug#194）。托盘/最小化态不依赖唤醒，由
    // EUI 主循环驻留分支的宿主帧泵兜底。
    ::core::platform::requestUiUpdate();
}

void setOsOpenFunnel(std::function<void(const std::string& path)> funnel) {
    g_osOpenFunnel = std::move(funnel);
}

bool secondInstancePathsNeedMainWindow(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        // need:380 C-3：.md/.markdown → 主窗笔记页承接，主窗必须在场
        if (core::notepad::isMarkdownExtension(path)) return true;
        if (!core::notepad::isNotepadExtension(path)) return true;
    }
    return false;
}

void drainPendingSecondInstanceFiles() {
    // 漏斗未注册（极早帧）：保留队列不消费，注册后的下一帧再送达
    if (!g_osOpenFunnel) return;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(g_secondInstanceFilesMutex);
        paths = std::exchange(g_secondInstanceFiles, {});
    }
    if (paths.empty()) return;
    // 唤醒裁决（bug270）：带路径唤醒不再由监听线程无条件弹主窗（那会让「双击关联
    // 文件启动」= 主窗被唤起 + 记事本窗两个窗口）。纯记事本清单扩展名送达 = 关联
    // 启动，只开记事本窗（与冷启动 armTrayIfViewerOnlyLaunch 同口径）；含其它类型
    // 才需要主窗承接路由页。纯唤起（无载荷）仍走监听线程 requestShow，不受影响。
    if (secondInstancePathsNeedMainWindow(paths)) app::requestShow();
    for (const auto& path : paths) {
        LOG_INFO("单实例：分发第二实例送达文件 — " + path);
        g_osOpenFunnel(path);
    }
}

void resetSecondInstanceFilesForTest() {
    std::lock_guard<std::mutex> lock(g_secondInstanceFilesMutex);
    g_secondInstanceFiles.clear();
    g_osOpenFunnel = nullptr;
}

// 记事本改造 need:273：pathHasMarkdownExtension / dispatchPageForFile 已退役——
// 扩展名判据统一 core::notepad::isNotepadExtension 单一真源。need:380 wave5-d
// C-3 修订：OS 双击 .md 改由主窗笔记页承接（见 dispatchFileOpen），其余清单
// 扩展名仍送达记事本窗。

void dispatchFileOpen(::core::Router* router, const std::string& path,
                      const std::filesystem::path& notesDir) {
    if (router == nullptr || path.empty()) return;
    // OS 语义漏斗。判据=调用点即漏斗：全仓 OS 入口（main 启动 argv / Apple
    // Event / 第二实例 drain）都汇于本函数。
    // need:380 wave5-d C-3：.md/.markdown → 主窗笔记页承接（Router "open:<相对
    // 路径>" 负载，md_file_open.h 既有通道）；目录外文件拷贝进笔记目录
    //（captureFileIntoNotesDir）。笔记页未注册（模块被关）→ 兜底记事本窗，
    // 打开能力不因模块门控丢失。
    if (core::notepad::isMarkdownExtension(path) &&
        router->isRegistered(::core::PageId::Markdown)) {
        auto rel = module::markdown::captureFileIntoNotesDir(notesDir, path);
        if (rel.isOk()) {
            router->navigateTo(::core::PageId::Markdown, "open:" + rel.value());
            return;
        }
        LOG_WARN("文件打开分发：笔记目录装载失败，落笔记页 — " + path +
                 " (" + rel.errorMessage() + ")");
        router->navigateTo(::core::PageId::Markdown);
        return;
    }
    // 记事本清单扩展名 → 独立记事本窗（查重聚焦/新窗在 session 层内收）；
    // 记事本窗不依赖任何 Router 注册态（独立窗不进 Router）。
    if (core::notepad::isNotepadExtension(path)) {
        module::notepad::openNotepadWindow(path);
        return;
    }
    // 未注册扩展名 → Home（F-01 口径不变）
    LOG_INFO("文件打开分发：未注册扩展名，落首页 — " + path);
    router->navigateTo(::core::PageId::Home);
}

// 关联启动裁决（need:5，R-3 强化：纯记事本清单送达 = 关联启动）：argv 路径 ∈
// 记事本清单 → 记事本窗即全部 UI，主窗保持托盘态。复用 --minimized 同一状态位
//（compose 首帧 closeMainWindow 同款路径，托盘左键可唤起），不另造平行机制。
// 前置=argv 捕获门已翻（无参数启动不置旗标，主窗正常弹出零回归）
// 关联启动 arm 的通道无关实现（bug283）：置 --minimized 同一状态位 + 首帧不
// show 闩值。argv 通道（armTrayIfViewerOnlyLaunch）与 odoc 通道（main 的
// handleAppleEventOpenFile 冷启送达）共用，不复制两份。温开门槛内聚在此：主窗
// 首帧 compose 后送达 = 温开，不再 arm（调用方无须自行判断）。
void armTrayForNotepadAssociation(const std::string& path, const char* channel) {
    // need:380 C-3：.md → 主窗笔记页承接，主窗不得保持托盘态
    if (core::notepad::isMarkdownExtension(path)) return;
    if (!core::notepad::isNotepadExtension(path)) return;
    if (!mainWindowStartupArmingWindowOpen()) return;  // 温开不抢隐藏
    g_minimizedStartupPending = true;
    g_mainWindowStartHidden = true;  // bug259-A：首帧不 show（闩值，见头文件）
    LOG_INFO(std::string("关联启动（记事本关联 ") + channel +
             "）：主窗保持托盘态，仅记事本窗");
}

void armTrayIfViewerOnlyLaunch() {
    if (g_argvCaptureDone) armTrayForNotepadAssociation(pendingOpenFilePath(), "argv");
}

bool mainWindowStartsHidden() { return g_mainWindowStartHidden; }

// 主窗首帧 compose 闩（bug283 启动窗口期判据）：sidebar compose 首帧置位、进程
// 内不清。置位前 odoc 首文件送达 = 冷启关联启动（可 arm）；置位后 = 温开（不得
// 抢隐藏——含托盘态双击文件、二次送达等场景）。
void markMainWindowComposed() { g_mainWindowComposed = true; }

bool mainWindowStartupArmingWindowOpen() { return !g_mainWindowComposed; }

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

std::string migrationFailMessage(const std::string& dbPath,
                                 const std::string& dataDir) {
    // need:291：用户可操作的恢复指引（数据根与库文件用实际路径，不抽象描述）
    return "数据库升级失败，应用即将退出。\n\n"
           "数据目录：" + dataDir + "\n"
           "数据库文件：" + dbPath + "\n\n"
           "可尝试：先把上面的数据库文件移走或改名备份，再重新启动应用"
           "（会重建空数据库）；历史数据可在数据目录的备份目录中找回。";
}

std::string openFailMessage(const std::string& dbPath) {
    // need:300③：非损坏打开失败（权限/磁盘满/被占用）——同 migrationFailMessage
    // 范式：实际路径 + 可操作指引。
    return "数据库无法打开（不是损坏，多为权限或磁盘问题），应用即将退出。\n\n"
           "数据库文件：" + dbPath + "\n\n"
           "可尝试：检查磁盘剩余空间与该文件的读写权限；确认没有其它程序占用；"
           "或将上面的数据库文件移走备份后重新启动应用（会重建空数据库）。";
}

} // namespace app::assembly
