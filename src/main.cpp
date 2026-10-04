// DevDesk 入口：EUI-NEO 提供 main，本 TU 只需导出 app::dslAppConfig() 和 app::compose()

#include "app_assembly.h"
#include "app_registry.h"  // kModuleRegistry：页面模块登记表
#include "app_sidebar.h"
#include "claude_service.h"
#include "core/claude_proxy_service.h"
#include "core/component_theme.h"
#include "core/database.h"
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
#include "module_toggle_bridge.h"  // app::toggle 活门控注入
#include "platform/tray_icon.h"  // 托盘图标按平台分发（macOS 蒙版 / Windows ico）
#include "settings_config.h"

#include <components/styledmenu.h>

#include <eui/dsl.h>
#include <eui/dsl_app.h>
#include <core/render/text.h>  // setDefaultFontFiles（默认 UI 字体覆盖）
// 主显示器工作区/缩放系数（窗口尺寸决策取值来源），显式引入
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

// 模块开关：关闭 = 不注册进 Router（onStart/compose/onKeyEvent 不可达）+ 专属服务不启动，
// 生效时机为重启。配置进程内读一次缓存（改开关只影响下次启动），本判定供 pet 开窗与托盘
// 菜单可用性，装配处门控注册在 app_assembly。恒启用 home/settings、未知 ID 恒 false。
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

// 打开应用数据库并落当前 schema；value() 失败即 LOG_FATAL+abort，不抛异常
::core::Database& openAppDatabase() {
    static std::unique_ptr<::core::Database> db;
    static bool initialized = false;
    if (!initialized) {
        auto r = ::core::Database::open(::core::paths::sqliteDbFile());
        if (r.isFail()) {
            LOG_FATAL("无法打开数据库: " + r.errorMessage());
            std::abort();
        }
        // 非 const value() 返回 T&，move 触发 Database 的 move ctor
        auto raw = new ::core::Database(std::move(r.value()));
        db.reset(raw);
        auto migrateResult = db->migrate();
        if (migrateResult.isFail()) {
            LOG_FATAL("数据库迁移失败: " + migrateResult.errorMessage());
            std::abort();
        }
        // 注册给 module_* 层（onStart 经 core::appDatabase() 拿同一份连接）
        ::core::registerAppDatabase(&db->db());
        LOG_INFO("数据库已就绪: " + ::core::paths::sqliteDbFile().string());
        // 调用日志滚动清理：天数读 app_config.logRetentionDays（0=永久跳过）；幂等，失败不阻断启动
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

// 启动前置段：由 getRouter() 经 PreStartHook 调用，保证 ClaudeService 创建+setGlobal 早于模块 onStart
void prepareClaudeServiceBeforeModules();

// 惰性装配单点：prepare 与会话内开关共用（前向声明，定义在 prepare 之后）
void assembleClaudeServiceNow();
// 桥真实现（定义在 prepare 之后），getRouter() 注入其地址
void ensureClaudeServiceAssembledImpl(bool autoStart);

::core::Router* getRouter() {
    static ::core::Router router;
    // 各模块 static 必须先于 armExitHook 构造：atexit LIFO 才能保证 handler 先于 modules/router 析构
    static std::unique_ptr<::core::Module> home(create_home_module());
    static std::unique_ptr<::core::Module> markdown(create_markdown_module());
    static std::unique_ptr<::core::Module> todo(create_todo_module());
    static std::unique_ptr<::core::Module> claude(create_claude_module());
    static std::unique_ptr<::core::Module> settings(create_settings_module());
    static std::unique_ptr<::core::Module> agent(create_agent_module());
    static std::unique_ptr<::core::Module> about(create_about_module());

    static bool initialized = false;
    if (!initialized) {
        // 无捕获 lambda：持有集是 static 存储期，免捕获可转函数指针，与 dispatchModuleToggle 共用同一查找
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

        // 启动时若有待打开文件（argv 捕获），解析成笔记负载分发
        app::assembly::dispatchFileOpen(
            &router, app::assembly::pendingOpenFilePath(),
            ::core::paths::markdownDir());

        // 活门控 handler（无捕获函数指针）：moduleForPage 是 constexpr 局部，免捕获且值烙进编译期
        //（无悬垂）；注册在装配之后 = 仅运行期拨开关触达，启动期行为零变化
        app::toggle::setToggleHandler(
            [](const std::string& moduleId, bool on) {
                app::assembly::dispatchModuleToggle(moduleId, on, *getRouter(),
                                                    moduleForPage);
            });
        // 惰性装配桥注入：会话内拨 claude 开关 → 这里（装配单例 + autoStart 才 startProxy）；
        // 目标静态存储期 ≥ 进程寿命，无悬垂；注册在装配之后，仅运行期拨开关触达
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

    // 与 argv 启动路径同一分发逻辑：解析 + open: 负载
    app::assembly::dispatchFileOpen(getRouter(), path,
                                    ::core::paths::markdownDir());
}

// core 只读服务定位器的代理运行面适配器：每次转发到 global() 单例，未装配（claude 关闭）
// 时各方法按服务不可用返回 false/0/空串/no-op；不持有 ClaudeService，仅只读查找
::core::GlobalClaudeProxyService<::module::claude::ClaudeService>&
claudeProxyProvider() {
    static ::core::GlobalClaudeProxyService<::module::claude::ClaudeService>
        provider;
    return provider;
}

// ClaudeService 单例：永不析构的堆持有——exit hook（stopAll → syncRestore 还原
// settings.json）必须看到活着的服务，故与进程同寿；db() 引用由 openAppDatabase() 保证有效。
// ~ClaudeService 的摘牌逻辑保留（测试用栈上实例仍需要）。
::module::claude::ClaudeService* getClaudeService() {
    // 故意泄漏：unique_ptr 对象在堆上，永不释放
    static auto* holder = new std::unique_ptr<::module::claude::ClaudeService>();
    if (!*holder) {
        auto& db = openAppDatabase();
        *holder = std::make_unique<::module::claude::ClaudeService>(db.db());
        // settings.json 接管通道：路径注入一次性完成，接管/还原触发收在 service 层，UI 不感知
        (*holder)->setSettingsSyncPaths(
            ::module::claude::settings_sync::defaultPaths());
        ::module::claude::ClaudeService::setGlobal(holder->get());
        // 与 setGlobal 同时注册进只读定位器：单例已创建 ⟺ 定位器可解析；
        // 模块关闭时本函数不被调用 → 定位器保持未注册
        ::core::ServiceLocator::set<::core::IClaudeProxyService>(
            &claudeProxyProvider());
    }
    return holder->get();
}

// 桌宠气泡内容源②：跨模块逾期计数桥，懒建只读 HomeService（不 include module_home，隔离不破）。
// todo 关闭或聚合查询失败 → nullopt（Result 口径不抛异常），pet 侧解释为「数据不可用」回退静态语料
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
    // value() 失败即 LOG_FATAL+abort，不抛异常（本 TU 非 Debug 档带 -fno-exceptions）
    return stats.value().overdueTodos;
}

// 桌宠气泡内容源③：代理事件桥，读 ClaudeService 最近请求终态（take 语义=读走即清+过期丢弃，
// 一个事件最多喂一次）。main 只透传 statusCode 原值交给 pet 分类（2xx/3xx=完成、>=400=出错、
// 0=上游不可达）；nullopt=单例未就绪/无待消费事件/过期
std::optional<int> getProxyEventStatus() {
    auto* svc = module::claude::ClaudeService::global();
    if (svc == nullptr) {
        return std::nullopt;
    }
    return svc->takeProxyEvent();
}

// 启动本地 Claude 代理（全局唯一）。notifyFailure=true（会话内用户动作）时校验失败额外弹
// notice；启动期自启走默认 false 只记 LOG（后台动作弹窗 UX 差，是取舍不是限制）
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

    // 前置校验：无激活 profile/连通不过 → 不起代理、零副作用（不 bind、不接管
    // settings.json）；校验失败是预期内结果，记 WARN 不记 ERROR
    auto v = svc->validateProxyPreconditions();
    if (v.isFail()) {
        LOG_WARN("Claude 代理未启动（前置校验未通过）: " + v.errorMessage());
        if (notifyFailure) {
            app::showAppNotice("代理未启动", v.errorMessage());
        }
        return;
    }

    // 端口读设置页配置（默认 8888）；被占用则 bind 失败，日志可见
    auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
    if (r.isFail()) {
        LOG_ERROR("Claude 代理启动失败: " + r.errorMessage());
        return;
    }
    // 启动成功日志由 startProxy 统一输出，不重复记录
}

// 启动前置段：由 getRouter() 经 preStart 钩子调用，早于模块注册与 onStart。
// claude 模块关闭 = 功能整体下线：不创建 ClaudeService（global() 保持 null）、不 bind 8888
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
    // 创建 ClaudeService + 接管通道/端口注入 + 崩溃补还原（与会话内惰性装配共用单点）
    assembleClaudeServiceNow();
    // 代理默认不启动，claudeProxyAutoStart 门控；启停按钮在 Claude 页
    if (settingsCfg.claudeProxyAutoStart) {
        startClaudeProxy();
    } else {
        LOG_INFO("Claude 代理未自动启动（claudeProxyAutoStart=false，"
                 "可在设置页或 Claude 页手动启动）");
    }
}

// 惰性装配单点：供「启动期门控开」与「会话内拨开关」两条路径共用，端口自读不依赖调用方；
// 幂等（getClaudeService static 守卫；setConfiguredProxyPort/recover 重复调用均 no-op）
void assembleClaudeServiceNow() {
    // 创建 + setGlobal 在 getClaudeService() 内完成，模块 onStart 才能拿到非 null 的 global()，必调
    auto* claudeSvc = getClaudeService();
    // 端口注入：手动启动/自启都经 configuredProxyPort 读值；改端口在 applyProxyPort
    //（settings_module.cpp）与本函数两处生效，口径须同步
    if (claudeSvc) {
        const module::settings::SettingsStore settingsStore(
            ::core::appDatabase());
        claudeSvc->setConfiguredProxyPort(settingsStore.load().proxyPort);
    }
    // 崩溃补还原：sidecar 残留先还原，必须先于任何 startProxy（否则残留托管键被误快照）
    if (claudeSvc) {
        claudeSvc->recoverPendingSettingsRestore();
    }
}

// 桥真实现：会话内惰性装配 + auto-start。无捕获可转函数指针（注入点见 getRouter()）；
// startClaudeProxy 自身幂等，重复 on 不二次 bind 端口
void ensureClaudeServiceAssembledImpl(bool autoStart) {
    assembleClaudeServiceNow();
    if (autoStart) {
        // 会话内拨开关=用户动作，校验失败弹 notice 说明原因（启动期自启只记 LOG）
        startClaudeProxy(/*notifyFailure=*/true);
    }
}

// 托盘菜单：唯一数据源 core::tray::trayMenuSpec（styled 主路径 + 原生降级同源）；
// 动作→页面映射与 Navigate* 导航委托全在 app_assembly（映射单点 pageForTrayAction）
void executeTrayAction(::core::tray::TrayMenuAction action) {
    using Action = ::core::tray::TrayMenuAction;
    // Navigate* 全委托 app_assembly
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

// 原生降级菜单（styled 失败时弹；keepDefault=false，全部来自 spec，无内置英文项）
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

// styled 托盘菜单（圆角/主题/FA 图标）。返回 false = 构建失败 → 原生降级；spec 按值捕获作 path 映射
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

// 启动期窗口尺寸决策（逻辑单位）：windowSize 是客户区窗口单位（Windows=物理像素、
// macOS=点），换算成窗口单位 + 非客户区外框后与工作区比较，放不下则启动即最大化。
// 最小尺寸走同一换算再与工作区取 min（只换算不钳制会把 macOS 的病复制到 Windows）。
// 决策是不含平台依赖的纯函数，在 core::window_layout
constexpr int kInitialWindowLogicalWidth = 1600;
constexpr int kInitialWindowLogicalHeight = 1000;
// 最小尺寸（逻辑单位）
constexpr int kMinWindowLogicalWidth = 1330;
constexpr int kMinWindowLogicalHeight = 845;

// 初始尺寸/形态 + 最小尺寸用同一套取值与换算（避免两处口径不同）
struct ResolvedWindowSizing {
    core::window_layout::InitialWindowDecision initial;
    core::window_layout::WindowSize minSize;
};

ResolvedWindowSizing resolveWindowSizing() {
    // 目标显示器=主显示器（窗口走 centerWindow，落在主屏）
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);

    // 缩放系数按工作区中心点取该显示器 DPI；工作区未知时降级 1.0（决策函数原样保留请求尺寸）。
    // 注意 macOS 上恒 1.0（窗口单位是点），Retina 的 2.0 在 contentScale 上，别读混
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
    // 单位名走平台注入（macOS=pt / Windows=物理px），别把 macOS 的点标成物理px 误导排查
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

// 常驻侧边栏 + 根布局：drawSidebar/compose 在 app_sidebar（拆出以便无头单测驱动）
const DslAppConfig& dslAppConfig() {
    // 先开库再恢复持久化主题/字号（只读 app_config）：themeRestored 是先声明的函数内 static，
    // 早于下面 initialized 执行，不先开库这些配置每次启动都读成默认值。顺序：ensureDirectoriesExist
    // → setupLogging（openAppDatabase 内部有日志）→ openAppDatabase → 读配置（openAppDatabase 幂等）
    static const bool themeRestored = []() {
        ::core::paths::ensureDirectoriesExist();
        setupLogging();
        openAppDatabase();
        // 不调 setDefaultFontFiles：设了即置 override 优先于系统 CJK 字体（优先级 override→
        // 系统 CJK→打包兜底）。且 NotoSansSC-VF 是变量字体 FreeType 加载不稳 → 渲染回退 Bold
        // 导致测宽≠渲染、布局错位，需换静态 Noto/Source Han 才可启用。
        // 标题栏 + 底色随主题：core 不依赖 EUI-NEO，经 theme 回调注入；先装回调再恢复主题，
        // 启动与运行时切换同路径。标题栏仅 Windows 生效，其他平台 EUI-NEO 静默降级
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
        // 窗口效果接线：clearColor alpha 按档 + backdrop（frosted→Acrylic）；透明帧缓冲 hint
        // 常开不可逆，off 档 alpha=1 视觉等价；backdrop 主循环首帧应用，降级真值经 activeWindowEffect() 回查
        module::settings::applyWindowVisuals(cfg.windowEffect);
        return true;
    }();
    (void)themeRestored;

    // 初始窗口尺寸决策，必须在 config 前求值（config 静态初始化时消费）；此时 glfwInit 已完成，
    // DPI 查询可用，不可用（无头/无显示器）时决策函数原样保留请求尺寸
    static const ResolvedWindowSizing initialWindow = resolveWindowSizing();

    static const DslAppConfig config = DslAppConfig{}
        .title("DevDesk")
        // Debug 构建下 EUI-NEO 默认把 FPS/CPU/GPU 拼进标题，显式关掉
        .showDebugStatsInTitle(false)
        .pageId("devdesk")
        .clearColor(::core::theme::current().colors.background)  // 跟随主题
        // 初始尺寸（窗口单位）：放得下用 1600×1000，否则启动即最大化（决策见 resolveWindowSizing）
        .windowSize(initialWindow.initial.width, initialWindow.initial.height)
        // 最小尺寸用同一函数换算后与工作区取 min；直传逻辑常量会绕开换算（见 minWindowSizeForWorkArea）
        .minWindowSize(initialWindow.minSize.width, initialWindow.minSize.height)
        // 最大化创建期生效（走 glfwWindowHint(GLFW_MAXIMIZED)），用户可再拖回
        .maximized(initialWindow.initial.maximize)
        .highDpi(true)
        .resizable(true)
        // 透明帧缓冲 hint 常开（创建期不可逆），三档运行时切换不重建窗口；实际档位由
        // setWindowEffect 驱动，此启动快照只负责开 hint
        .windowEffect(core::platform::WindowEffect::Transparent)
        // 启动快照：按恢复后的主题深浅设置；运行时切换由 theme 回调覆盖
        .darkTitleBar(::core::theme::current().isDark)
        .centerWindow()
        // 托盘：styled 独立菜单窗为主路径，原生 trayMenu 为降级数据源（keepDefault=false，
        // 菜单项与 styled 同源）。左键唤起主窗口（requestShow 处理三态），右键弹 styled 菜单
        .tray(true)
        .trayTitle("DevDesk")
        // 托盘图标按平台分发（业务代码零平台宏）：macOS 用 22×22 纯黑+alpha template 蒙版
        // （setTemplate:YES 只取 alpha，给圆角彩色图标会退化成实心方块）；Windows 用 .ico
        // （ExtractIconExW 只认 .ico，给 PNG 必空）。相对路径经 resolveIconPath 候选链：
        // build 目录命中 CWD/assets，bundle 命中 exeDir/assets
        .trayIcon(platform::tray::defaultIconPath())
        .trayMenu(nativeTrayMenuItems(), false)
        .onTrayMenu([](int x, int y, bool leftButton) {
            if (leftButton) {
                executeTrayAction(::core::tray::TrayMenuAction::ShowMainWindow);
                return true;
            }
            return showTrayStyledMenu(x, y);
        })
        // Finder 双击/「打开方式」：macOS 走 kAEOpenDocuments，EUI-NEO 转回调后切模块
        .onOpenFile([](const std::string& path) { handleAppleEventOpenFile(path); })
        // 模块级快捷键通道：EUI-NEO 只回调焦点元素未消费的按键（InputBuilder 无逐组件
        // onKeyEvent passthrough），转交 Router 派发到当前页模块
        .onKeyEvent([](const eui::KeyEvent& key) {
            // Ctrl+B 折叠/展开侧边栏：派发序焦点优先，编辑器聚焦时 Ctrl+B 已被消费（加粗），
            // 到这里一定是非编辑场景，无需特判
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
        // 必须在 setupLogging() 之后（内部有 LOG_INFO）
        app::assembly::captureOpenFileFromArgv();
        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + paths::appDataDir().string());
        // 数据根内置 .gitignore：挡 devdesk.db/-wal 被 `git add .` 误提交；写失败只 WARN 不阻断启动
        if (auto gi = paths::ensureDataRootGitignore(); gi.isFail()) {
            LOG_WARN("数据根 .gitignore 写入失败（防误提交的一道闸缺失）: " +
                     gi.errorMessage());
        }
        openAppDatabase();   // 幂等（themeRestored 已开过）
        const module::settings::SettingsStore settingsStore(
            ::core::appDatabase());
        const auto settingsCfg = settingsStore.load();
        // 侧边栏折叠启动恢复：app_config → 运行时状态；persist=false 只改内存不回写
        app::setSidebarCollapsed(settingsCfg.sidebarCollapsed, false);
        // Claude 前置段已收进 getRouter() 的 preStart 钩子（prepareClaudeServiceBeforeModules），
        // 仍早于模块 onStart
        getRouter();
        // 桌宠：非 Router 页面模块，门控/装载/开窗收进 maybeStartPet；开窗回调在本文件（注入内容源②③）
        app::assembly::maybeStartPet(
            gatingConfigSnapshot(),
            [] {
                // 桌宠配置在 app_config 的 pet_config 键（模块隔离，不走 config.json 命名空间）
                return module::pet::PetConfigStore(
                           ::core::appDatabase())
                    .load();
            },
            [](const module::pet::PetConfig& petCfg) {
                module::pet::startPetWindow(module::pet::PetWindowConfig{
                    petCfg.skinName, petCfg.x, petCfg.y, petCfg.scale,
                    &getOverdueTaskCount, &getProxyEventStatus});
            });
        return true;
    }();
    (void)initialized;

    return config;
}

// 根布局 row(侧边栏, 页面内容) 在 app_sidebar.cpp 的 app::compose

} // namespace app
