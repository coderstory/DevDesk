// DevDesk 入口：EUI-NEO 提供 main，本 TU 只需导出 app::dslAppConfig() 和 app::compose()

#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"
#include "claude_service.h"
#include "core/claude_proxy_service.h"
#include "core/component_theme.h"
#include "core/database.h"
#include "core/db_backup.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/tray_menu_spec.h"
#include "core/window_layout.h"
#include "module_home/home_service.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_window.h"
#include "module_toggle_bridge.h"
#include "platform/message_box.h"
#include "platform/tray_icon.h"
#include "github_sync_exit_hook.h"
#include "settings_config.h"

#include <components/styledmenu.h>

#include <eui/dsl.h>
#include <eui/dsl_app.h>
#include <core/render/text.h>
#include <core/window/window_backend.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <memory>
#include <optional>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// 模块开关：关闭 = 不注册进 Router 且专属服务不启动，重启后生效；配置为进程内一次性缓存。
// 恒启用 home/settings，未知 ID 恒 false。
const module::settings::SettingsConfig& gatingConfigSnapshot() {
    static const module::settings::SettingsConfig cfg = [] {
        const module::settings::SettingsStore store(
            ::core::appDatabase());
        return store.load();
    }();
    return cfg;
}

bool isModuleEnabled(const char* moduleId) {
    return module::settings::moduleEnabled(gatingConfigSnapshot(), moduleId);
}

void setupLogging() {
    core::log::addFileOutput(::core::paths::logFile());
    core::log::addConsoleOutput();
    core::log::setLevel(core::log::Level::Info);
}

// 数据库损坏的用户决策（app 层 UI；文件级操作全在 core::dbmaint）。
// exclude：跳过该备份（上一轮已尝试过的——还原失败或还原后重开仍坏的那份，再弹一次没有意义）。
// attemptedBackup：[out] 本次弹框所基于的备份路径（未进入弹框流程则不变），调用方
//   下一轮传回 exclude，避免对同一份备份重复问「还原」。
// 返回 true = 已还原，调用方应重开库继续；false = 用户选择退出或还原失败（对话框已弹过）
bool handleDbCorruption(
    const std::filesystem::path& dbPath,
    const std::optional<std::filesystem::path>& exclude,
    std::optional<std::filesystem::path>& attemptedBackup) {
    auto backup = ::core::dbmaint::latestBackup(dbPath, exclude);
    if (backup.isFail()) {
        // 扫描失败（权限/IO）≠ 真的没有备份：分开报，别让用户误以为无备份可救
        LOG_ERROR("备份目录扫描失败，无法确认是否有可用备份: " +
                  backup.errorMessage());
        ::platform::msgbox::show(
            "数据库损坏",
            "数据库文件损坏，且备份目录无法访问（无法确认是否有可用备份）。\n" +
                backup.errorMessage() + "\n应用即将退出。",
            false);
        return false;
    }
    if (!backup.value().has_value()) {
        LOG_ERROR("数据库损坏且无可用备份: " +
                  std::string(exclude.has_value() ? "（已排除本轮尝试过的备份）" : "") +
                  dbPath.string());
        ::platform::msgbox::show(
            "数据库损坏",
            exclude.has_value()
                ? "数据库文件损坏，且除已尝试过的备份外没有其他可用备份可还原。\n应用即将退出。"
                : "数据库文件损坏，且没有可用备份可还原。\n应用即将退出。",
            false);
        return false;
    }
    const std::filesystem::path& bak = *backup.value();
    attemptedBackup = bak;
    const auto choice = ::platform::msgbox::show(
        "数据库损坏",
        "检测到数据库损坏。\n\n是否用最近一次备份还原并继续启动？\n损坏的文件会改名隔离保留"
        "（原样不删）。\n\n备份: " + bak.filename().string(),
        true);
    if (choice != ::platform::msgbox::Choice::Ok) {
        LOG_INFO("数据库损坏：用户选择退出");
        return false;
    }
    auto restored = ::core::dbmaint::restoreFromBackup(dbPath, bak);
    if (restored.isFail()) {
        LOG_ERROR("备份还原失败: " + restored.errorMessage());
        ::platform::msgbox::show(
            "还原失败",
            "备份还原失败，应用即将退出。\n\n" + restored.errorMessage(),
            false);
        return false;
    }
    LOG_INFO("已从备份还原数据库，损坏库隔离于: " + restored.value().string());
    return true;
}

// 打开应用数据库并落当前 schema；失败即 LOG_FATAL+abort，不抛异常
::core::Database& openAppDatabase() {
    static std::unique_ptr<::core::Database> db;
    static bool initialized = false;
    if (!initialized) {
        const std::filesystem::path dbPath = ::core::paths::sqliteDbFile();
        std::unique_ptr<::core::Database> opened;
        // 损坏允许「还原后重开」一轮：首开坏 → 还原 → 二开；二开仍坏即退出（循环上限 2）。
        // triedBackup 记录本轮已尝试过的备份：下一轮经 exclude 递补更早一份，
        // 不对同一份刚验证失败的备份重复弹「还原」
        std::optional<std::filesystem::path> triedBackup;
        for (int round = 0; round < 2 && opened == nullptr; ++round) {
            auto r = ::core::Database::open(dbPath);
            std::unique_ptr<::core::Database> cand;
            if (r.isFail()) {
                // open 失败 ≠ 一定损坏（权限/磁盘也可能），quick_check 也判坏才走还原流程
                auto qc = ::core::dbmaint::quickCheckOk(dbPath);
                if (qc.isFail() || qc.value()) {
                    LOG_FATAL("无法打开数据库: " + r.errorMessage());
                    std::abort();
                }
                LOG_ERROR("数据库打开失败且判定损坏: " + r.errorMessage());
            } else {
                cand = std::make_unique<::core::Database>(std::move(r.value()));
                auto chk = cand->quickCheck();
                if (chk.isFail()) {
                    // 瞬态错误（BUSY/IOERR 等）≠ 损坏：不进还原流程，LOG_ERROR 后带着已打开的库继续启动
                    LOG_ERROR("主库完整性检查无法执行（瞬态错误，不判定损坏，不触发还原）: " +
                              chk.errorMessage());
                } else if (!chk.value()) {
                    LOG_ERROR("数据库损坏（quick_check 非 ok）: " + dbPath.string());
                    // 还原是文件级改名/拷贝：先关掉本连接，否则 Windows 上 rename 主库会失败
                    cand.reset();
                }
            }
            if (cand == nullptr) {
                if (!handleDbCorruption(dbPath, triedBackup, triedBackup)) {
                    // 用户选择退出 / 还原失败（对话框已弹过）。
                    // _Exit 而非 exit：此刻还在 dslAppConfig 静态初始化内，atexit
                    //（armExitHook 等）会摸到尚未构造的 Router/modules；日志逐行 flush，无丢失
                    LOG_INFO("数据库损坏：应用退出");
                    std::_Exit(1);
                }
                continue;  // 已还原，重开
            }
            opened = std::move(cand);
        }
        if (opened == nullptr) {
            ::platform::msgbox::show(
                "数据库损坏",
                "还原后的数据库仍无法通过完整性检查，应用即将退出。",
                false);
            LOG_FATAL("还原后仍损坏，无法继续启动");
            std::_Exit(1);
        }
        db = std::move(opened);
        auto migrateResult = db->migrate();
        if (migrateResult.isFail()) {
            LOG_FATAL("数据库迁移失败: " + migrateResult.errorMessage());
            std::abort();
        }
        ::core::registerAppDatabase(&db->db());
        LOG_INFO("数据库已就绪: " + ::core::paths::sqliteDbFile().string());
        // 调用日志滚动清理：天数读 app_config.logRetentionDays（0=永久跳过）；失败不阻断启动
        const module::settings::SettingsStore settingsStore(
            ::core::appDatabase());
        const int retentionDays = settingsStore.load().logRetentionDays;
        if (retentionDays > 0) {
            const auto pruned = db->pruneCallLogs(retentionDays);
            if (pruned.isOk()) {
                if (pruned.value() > 0) {
                    LOG_INFO("已清理 " + std::to_string(pruned.value()) +
                             " 条 " + std::to_string(retentionDays) +
                             " 天前的调用日志");
                }
            } else {
                LOG_WARN("调用日志清理失败（不阻断启动）: " +
                         pruned.errorMessage());
            }
        } else {
            LOG_INFO("调用日志保留策略: 永久保留（logRetentionDays=0），跳过清理");
        }
        initialized = true;
    }
    return *db;
}

// 启动前置段：经 PreStartHook 调用，保证 ClaudeService 创建+setGlobal 早于模块 onStart
void prepareClaudeServiceBeforeModules();

void assembleClaudeServiceNow();
void ensureClaudeServiceAssembledImpl(bool autoStart);

::core::Router* getRouter() {
    static ::core::Router router;
    // 模块 static 须先于 armExitHook 构造：atexit LIFO 保证 handler 先于 modules/router 析构
    static std::unique_ptr<::core::Module> home(create_home_module());
    static std::unique_ptr<::core::Module> markdown(create_markdown_module());
    static std::unique_ptr<::core::Module> todo(create_todo_module());
    static std::unique_ptr<::core::Module> claude(create_claude_module());
    static std::unique_ptr<::core::Module> settings(create_settings_module());
    static std::unique_ptr<::core::Module> agent(create_agent_module());
    static std::unique_ptr<::core::Module> about(create_about_module());

    static bool initialized = false;
    if (!initialized) {
        constexpr app::assembly::ModuleLookup moduleForPage =
            +[](::core::PageId page) -> ::core::Module* {
                switch (page) {
                    case ::core::PageId::Home: return home.get();
                    case ::core::PageId::Markdown: return markdown.get();
                    case ::core::PageId::Todo: return todo.get();
                    case ::core::PageId::ClaudeProxy: return claude.get();
                    case ::core::PageId::Settings: return settings.get();
                    case ::core::PageId::Agent: return agent.get();
                    case ::core::PageId::About: return about.get();
                }
                return nullptr;
            };
        auto gated = app::assembly::buildGatedEntries(
            gatingConfigSnapshot(), ::app::registry::kModuleRegistry,
            sizeof(::app::registry::kModuleRegistry) /
                sizeof(::app::registry::kModuleRegistry[0]));
        for (auto& entry : gated) {
            entry.module = moduleForPage(entry.page);
        }
        app::assembly::assembleAndStart(
            router, gatingConfigSnapshot(), gated,
            prepareClaudeServiceBeforeModules);

        // 退出补推钩子：本钩子晚于 armExitHook 注册，atexit LIFO → 先于
        // router->stopAll() 跑，此刻模块与数据库都还活着；推不完由
        // settings onStop 的 join 收束（上限 5s 等待见 runExitSyncPush）。
        std::atexit([] { ::module::settings::runGithubSyncExitPush(); });

        app::assembly::dispatchFileOpen(
            &router, app::assembly::pendingOpenFilePath(),
            ::core::paths::markdownDir());

        // 无捕获函数指针（moduleForPage 编译期固化，无悬垂）；注册须在装配之后
        app::toggle::setToggleHandler(
            [](const std::string& moduleId, bool on) {
                app::assembly::dispatchModuleToggle(moduleId, on, *getRouter(),
                                                    moduleForPage);
            });
        // 惰性装配桥：会话内拨 claude 开关 → 装配单例（autoStart 才 startProxy）；目标静态存储期无悬垂
        app::toggle::setClaudeAssemblyHandler(&ensureClaudeServiceAssembledImpl);
        initialized = true;
    }
    return &router;
}

// Apple Event 打开文件路径（Finder 双击/open -a），在主循环渲染前触发，可安全切页
void handleAppleEventOpenFile(const std::string& path) {
    if (path.empty()) return;

    app::assembly::pendingOpenFilePath() = path;
    LOG_INFO("捕获到待打开文件（Apple Event）: " + path);

    app::assembly::dispatchFileOpen(getRouter(), path,
                                    ::core::paths::markdownDir());
}

// 只读定位器适配器：每次转发 global() 单例，未装配时各方法返回 false/0/空串/no-op
::core::GlobalClaudeProxyService<::module::claude::ClaudeService>&
claudeProxyProvider() {
    static ::core::GlobalClaudeProxyService<::module::claude::ClaudeService>
        provider;
    return provider;
}

// 与进程同寿的堆持有：exit hook（stopAll → syncRestore 还原 settings.json）须看到活着的服务
::module::claude::ClaudeService* getClaudeService() {
    // 故意泄漏：unique_ptr 对象在堆上，永不释放
    static auto* holder = new std::unique_ptr<::module::claude::ClaudeService>();
    if (!*holder) {
        auto& db = openAppDatabase();
        *holder = std::make_unique<::module::claude::ClaudeService>(db.db());
        // settings.json 接管：路径注入一次性完成，触发收在 service 层
        (*holder)->setSettingsSyncPaths(
            ::module::claude::settings_sync::defaultPaths());
        ::module::claude::ClaudeService::setGlobal(holder->get());
        // 与 setGlobal 成对：单例已创建 ⟺ 定位器可解析；模块关闭时不调用 → 定位器未注册
        ::core::ServiceLocator::set<::core::IClaudeProxyService>(
            &claudeProxyProvider());
    }
    return holder->get();
}

// 桌宠气泡内容源：跨模块逾期计数，懒建只读 HomeService；todo 关闭或查询失败 → nullopt
std::optional<int64_t> getOverdueTaskCount() {
    if (!isModuleEnabled(module::settings::kModuleIdTodo)) {
        return std::nullopt;
    }
    static std::unique_ptr<module::home::HomeService> svc;
    static bool initialized = false;
    if (!initialized) {
        auto& db = openAppDatabase();
        svc = std::make_unique<module::home::HomeService>(db.db());
        initialized = true;
    }
    const auto stats = svc->getStats();
    if (stats.isFail()) {
        return std::nullopt;
    }
    // 失败即 LOG_FATAL+abort，不抛异常（本 TU 非 Debug 档带 -fno-exceptions）
    return stats.value().overdueTodos;
}

// 桌宠内容源：读最近请求终态（take 语义=读走即清+过期丢弃）；透传原值：
// 2xx/3xx=完成、>=400=出错、0=上游不可达；nullopt=未就绪/无待消费事件/过期
std::optional<int> getProxyEventStatus() {
    auto* svc = module::claude::ClaudeService::global();
    if (svc == nullptr) {
        return std::nullopt;
    }
    return svc->takeProxyEvent();
}

// 启动本地 Claude 代理（全局唯一）；notifyFailure=true（用户动作）时校验失败额外弹 notice，
// 启动期自启只记 LOG
void startClaudeProxy(bool notifyFailure = false) {
    auto* svc = getClaudeService();
    if (!svc) {
        LOG_ERROR("startClaudeProxy: ClaudeService 初始化失败");
        return;
    }
    if (svc->isProxyRunning()) {
        LOG_INFO("Claude 代理已在运行 port=" + std::to_string(svc->proxyPort()));
        return;
    }

    // 前置校验：失败 → 不起代理、零副作用（不 bind、不接管 settings.json）；预期内结果记 WARN
    auto v = svc->validateProxyPreconditions();
    if (v.isFail()) {
        LOG_WARN("Claude 代理未启动（前置校验未通过）: " + v.errorMessage());
        if (notifyFailure) {
            app::showAppNotice("代理未启动", v.errorMessage());
        }
        return;
    }

    auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
    if (r.isFail()) {
        LOG_ERROR("Claude 代理启动失败: " + r.errorMessage());
        return;
    }
}

// claude 模块关闭 = 功能整体下线：不创建 ClaudeService、不 bind 端口
void prepareClaudeServiceBeforeModules() {
    const module::settings::SettingsStore settingsStore(
        ::core::appDatabase());
    const auto settingsCfg = settingsStore.load();
    if (!module::settings::moduleEnabled(
            settingsCfg, module::settings::kModuleIdClaudeProxy)) {
        LOG_INFO("Claude 模块已关闭：代理/ClaudeService 整体下线（端口 " +
                 std::to_string(settingsCfg.proxyPort) +
                 " 不监听）；重新启用可在设置页开启，会话内即时生效（惰性装配）");
        return;
    }
    assembleClaudeServiceNow();
    if (settingsCfg.claudeProxyAutoStart) {
        startClaudeProxy();
    } else {
        LOG_INFO("Claude 代理未自动启动（claudeProxyAutoStart=false，"
                 "可在设置页或 Claude 页手动启动）");
    }
}

// 惰性装配单点：幂等（static 守卫；setConfiguredProxyPort/recover 重复调用均 no-op），端口自读
void assembleClaudeServiceNow() {
    auto* claudeSvc = getClaudeService();
    // 端口改值在 applyProxyPort（settings_module.cpp）与本函数两处生效，口径须同步
    if (claudeSvc) {
        const module::settings::SettingsStore settingsStore(
            ::core::appDatabase());
        claudeSvc->setConfiguredProxyPort(settingsStore.load().proxyPort);
    }
    // 崩溃补还原：sidecar 残留须先还原，且先于任何 startProxy（否则残留托管键被误快照）
    if (claudeSvc) {
        claudeSvc->recoverPendingSettingsRestore();
    }
}

// 会话内惰性装配 + auto-start（无捕获，可转函数指针）；startClaudeProxy 幂等，重复 on 不二次 bind
void ensureClaudeServiceAssembledImpl(bool autoStart) {
    assembleClaudeServiceNow();
    if (autoStart) {
        startClaudeProxy(/*notifyFailure=*/true);
    }
}

// 托盘菜单唯一数据源 trayMenuSpec；动作→页面映射在 app_assembly（pageForTrayAction）
void executeTrayAction(::core::tray::TrayMenuAction action) {
    using Action = ::core::tray::TrayMenuAction;
    if (app::assembly::dispatchTrayNavigation(action, *getRouter())) {
        return;
    }
    switch (action) {
        case Action::ShowMainWindow:
            app::requestShow();
            break;
        case Action::Exit:
            app::requestExit();
            break;
        case Action::None:
        default:
            break;
    }
}

// 原生降级菜单（styled 失败时用；数据全来自 spec，无内置项）
std::vector<core::platform::TrayMenuItem> nativeTrayMenuItems() {
    std::vector<core::platform::TrayMenuItem> items;
    for (const auto& spec : ::core::tray::trayMenuSpec()) {
        core::platform::TrayMenuItem item;
        item.text = spec.text;
        if (!item.isSeparator()) {
            const auto action = spec.action;
            item.callback = [action] { executeTrayAction(action); };
        }
        items.push_back(std::move(item));
    }
    return items;
}

// styled 托盘菜单；返回 false = 构建失败 → 原生降级；spec 按值捕获作回调映射
bool showTrayStyledMenu(int x, int y) {
    const auto spec = ::core::tray::trayMenuSpec();
    std::vector<components::StyledMenuItem> items;
    for (const auto& s : spec) {
        components::StyledMenuItem item;
        item.text = s.text;
        item.iconCodepoint = s.iconCodepoint;
        items.push_back(std::move(item));
    }
    const components::StyledMenuStyle style(
        core::theme::componentTokens(core::theme::current()));
    return components::showStyledMenu("tray", std::move(items), style,
                                      static_cast<float>(x), static_cast<float>(y),
                                      [spec](const std::vector<int>& path) {
                                          if (path.size() == 1 && path[0] >= 0 &&
                                              static_cast<size_t>(path[0]) < spec.size()) {
                                              executeTrayAction(spec[static_cast<size_t>(path[0])].action);
                                          }
                                      });
}

// 启动期窗口尺寸决策（逻辑单位）：windowSize 单位 Windows=物理像素、macOS=点，换算成
// 窗口单位 + 非客户区外框后与工作区比较，放不下则启动即最大化；纯函数在 core::window_layout
constexpr int kInitialWindowLogicalWidth = 1600;
constexpr int kInitialWindowLogicalHeight = 1000;
constexpr int kMinWindowLogicalWidth = 1330;
constexpr int kMinWindowLogicalHeight = 845;

struct ResolvedWindowSizing {
    core::window_layout::InitialWindowDecision initial;
    core::window_layout::WindowSize minSize;
};

ResolvedWindowSizing resolveWindowSizing() {
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);

    // 缩放系数按工作区中心点取该显示器 DPI，工作区未知时降级 1.0（决策函数原样保留请求尺寸）。
    // 注意 macOS 恒 1.0（窗口单位是点），Retina 的 2.0 在 contentScale 上
    const float scale =
        (workW > 0 && workH > 0)
            ? core::window::windowScaleForPoint(
                  static_cast<float>(workX) + static_cast<float>(workW) * 0.5f,
                  static_cast<float>(workY) + static_cast<float>(workH) * 0.5f)
            : 1.0f;
    const core::window_layout::FrameInsets frame =
        core::window_layout::frameInsetsForScale(scale);
    const core::window_layout::InitialWindowRequest request{
        kInitialWindowLogicalWidth, kInitialWindowLogicalHeight,
        kMinWindowLogicalWidth, kMinWindowLogicalHeight};
    const core::window_layout::WorkArea workArea{workX, workY, workW, workH};

    ResolvedWindowSizing sizing;
    sizing.initial = core::window_layout::decideInitialWindow(request, workArea,
                                                              scale, frame);
    sizing.minSize =
        core::window_layout::minWindowSizeForWorkArea(request, workArea, scale, frame);
    LOG_INFO("初始窗口尺寸决策：工作区 " + std::to_string(workW) + "x" +
             std::to_string(workH) + "@(" + std::to_string(workX) + "," +
             std::to_string(workY) + ") " + core::window_layout::windowUnitName() +
             "，缩放 " + std::to_string(scale) + "，外框 +" +
             std::to_string(frame.width) + "x+" + std::to_string(frame.height) +
             "，客户区 " + std::to_string(sizing.initial.width) + "x" +
             std::to_string(sizing.initial.height) + "，最小窗 " +
             std::to_string(sizing.minSize.width) + "x" +
             std::to_string(sizing.minSize.height) +
             (sizing.initial.maximize ? " → 启动即最大化（外框放不进工作区）"
                                      : " → 用请求尺寸"));
    return sizing;
}

} // namespace

namespace app {

// 常驻侧边栏 + 根布局在 app_sidebar
const DslAppConfig& dslAppConfig() {
    // 顺序敏感：先开库再读主题/字号（themeRestored 先于 config 求值，不先开库读到默认值）
    static const bool themeRestored = []() {
        ::core::paths::ensureDirectoriesExist();
        setupLogging();
        openAppDatabase();
        // 不调 setDefaultFontFiles：override 会压过系统 CJK；且变量字体 FreeType 加载不稳 → 测宽≠渲染，启用前需换静态字体
        // 标题栏/底色随主题：先装回调再恢复主题，启动与运行时切换同路径；标题栏仅 Windows 生效
        ::core::theme::setChangeHandler([](bool isDark) {
            app::setTitleBarAppearance(
                core::platform::TitleBarAppearance{isDark});
            const module::settings::SettingsStore store(
                ::core::appDatabase());
            module::settings::applyWindowVisuals(store.load().windowEffect);
        });
        const module::settings::SettingsStore store(
            ::core::appDatabase());
        const auto cfg = store.load();
        if (cfg.darkTheme) {
            ::core::theme::setDark();
        } else {
            ::core::theme::setLight();
        }
        ::core::theme::setFontScale(
            static_cast<float>(cfg.fontScalePercent) / 100.0f);
        // 透明帧缓冲 hint 创建期不可逆，常开；off 档 alpha=1 视觉等价；实际档位经 activeWindowEffect() 回查
        module::settings::applyWindowVisuals(cfg.windowEffect);
        return true;
    }();
    (void)themeRestored;

    // 初始窗口决策必须在 config 前求值（config 静态初始化时消费）；此时 glfwInit 已完成
    // DPI 可用，无头/无显示器时决策函数原样保留请求尺寸
    static const ResolvedWindowSizing initialWindow = resolveWindowSizing();

    static const DslAppConfig config = DslAppConfig{}
        .title("DevDesk")
        .showDebugStatsInTitle(false)
        .pageId("devdesk")
        .clearColor(::core::theme::current().colors.background)
        .windowSize(initialWindow.initial.width, initialWindow.initial.height)
        // 直传逻辑常量会绕开换算，须用 minWindowSizeForWorkArea 的结果
        .minWindowSize(initialWindow.minSize.width, initialWindow.minSize.height)
        .maximized(initialWindow.initial.maximize)
        .highDpi(true)
        .resizable(true)
        // 透明 hint 创建期不可逆，常开；档位切换由 setWindowEffect 驱动，不重建窗口
        .windowEffect(core::platform::WindowEffect::Transparent)
        .darkTitleBar(::core::theme::current().isDark)
        .centerWindow()
        // 左键唤起主窗口，右键弹 styled 菜单；原生 trayMenu 为降级数据源（keepDefault=false）
        .tray(true)
        .trayTitle("DevDesk")
        // macOS 用 22×22 纯黑+alpha template 蒙版（彩色图标退化为实心方块）；Windows 只认 .ico。相对路径经
        // resolveIconPath 候选链解析
        .trayIcon(platform::tray::defaultIconPath())
        .trayMenu(nativeTrayMenuItems(), false)
        .onTrayMenu([](int x, int y, bool leftButton) {
            if (leftButton) {
                executeTrayAction(::core::tray::TrayMenuAction::ShowMainWindow);
                return true;
            }
            return showTrayStyledMenu(x, y);
        })
        .onOpenFile([](const std::string& path) { handleAppleEventOpenFile(path); })
        // EUI-NEO 只回调焦点元素未消费的按键（无逐组件 passthrough），转交 Router 派发
        .onKeyEvent([](const eui::KeyEvent& key) {
            // 编辑器聚焦时 Ctrl+B 已被消费（加粗），到这里一定是非编辑场景
            if (key.isDown() && key.modifiers.shortcut() &&
                !key.modifiers.shift && !key.modifiers.alt &&
                key.key == eui::InputKey::B) {
                app::setSidebarCollapsed(!app::sidebarCollapsed(), true);
                requestUpdate();
                return;
            }
            if (auto* router = getRouter()) {
                router->dispatchKeyEvent(key);
            }
        });

    static bool initialized = []() {
        using namespace core;
        // 须在 setupLogging() 之后（内部有 LOG_INFO）
        app::assembly::captureOpenFileFromArgv();
        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + paths::appDataDir().string());
        // 数据根内置 .gitignore：挡 devdesk.db/-wal 被 `git add .` 误提交；写失败只 WARN 不阻断启动
        if (auto gi = paths::ensureDataRootGitignore(); gi.isFail()) {
            LOG_WARN("数据根 .gitignore 写入失败（防误提交的一道闸缺失）: " +
                     gi.errorMessage());
        }
        openAppDatabase();
        const module::settings::SettingsStore settingsStore(
            ::core::appDatabase());
        const auto settingsCfg = settingsStore.load();
        app::setSidebarCollapsed(settingsCfg.sidebarCollapsed, false);
        getRouter();
        // 桌宠非 Router 页面模块，门控/装载/开窗收进 maybeStartPet
        app::assembly::maybeStartPet(
            gatingConfigSnapshot(),
            [] {
                // 桌宠配置走 app_config 的 pet_config 键（模块隔离，不走 config.json 命名空间）
                return module::pet::PetConfigStore(
                           ::core::appDatabase())
                    .load();
            },
            [](const module::pet::PetConfig& petCfg) {
                module::pet::startPetWindow(module::pet::PetWindowConfig{
                    petCfg.skinName, petCfg.x, petCfg.y, petCfg.scale,
                    &getOverdueTaskCount, &getProxyEventStatus});
            });
        // 启动成功后自动备份一次（模块 onStart 完成、窗口创建前）：进程级闩防重复；
        // 失败（磁盘满/权限）只 LOG_ERROR，绝不阻断启动
        auto startupBackup = ::core::dbmaint::performStartupBackupOnce(
            paths::sqliteDbFile());
        if (startupBackup.isOk()) {
            if (startupBackup.value().has_value()) {
                LOG_INFO("启动备份完成: " + startupBackup.value()->string());
            }
        } else {
            LOG_ERROR("启动备份失败（不阻断启动）: " + startupBackup.errorMessage());
        }
        return true;
    }();
    (void)initialized;

    return config;
}

} // namespace app
