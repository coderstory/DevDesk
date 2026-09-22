// src/main.cpp
// DevDesk 入口 - EUI-NEO 提供 main 函数，我们只需提供
// app::dslAppConfig() 和 app::compose()

#include "claude_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "platform/file_open.h"
#include "settings_config.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <fstream>
#include <memory>
#include <cstdlib>
#include <string>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();

namespace {

// 全局：Finder "打开方式" 传入的文件路径
// 两条来源都写这里：命令行 argv（启动时）/ Apple Event（Finder 双击、打开方式）
// MVP：仅记录 + log + 切页；实际渲染文件内容留待 P 阶段
std::string& pendingOpenFilePath() {
    static std::string path;
    return path;
}

// 从 argv 捕获要打开的文件（仅在第一次 dslAppConfig() 调用时执行）
// 注意：必须在 setupLogging() 之后调用，否则 LOG_INFO 会丢失
// 平台差异在 platform/file_open_* 内隔离（macOS 读 _NSGetArgv，Windows 读标准 argv）
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

// 根据文件扩展名返回目标 PageId（架构准备：MVP 只返回目标，不实际加载内容）
// 后续 P 阶段：让对应模块读 pendingOpenFilePath() 并实际渲染文件
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

void setupLogging() {
    core::log::addFileOutput(::core::paths::logFile());
    core::log::addConsoleOutput();
    core::log::setLevel(core::log::Level::Info);
}

// 打开应用数据库并跑 migrations
// 注意：main.cpp 用 -fno-exceptions 编译，所以用 unsafeValue() 而非 .value()
::core::Database& openAppDatabase() {
    static std::unique_ptr<::core::Database> db;
    static bool initialized = false;
    if (!initialized) {
        auto r = ::core::Database::open(::core::paths::sqliteDbFile());
        if (r.isFail()) {
            LOG_FATAL("无法打开数据库: " + r.errorMessage());
            std::abort();
        }
        // unsafeValue() 是 Result 的非抛出版本，返回 T&。再用 std::move 转成 xvalue
        // 触发 Database 的 move ctor。
        auto raw = new ::core::Database(std::move(r.unsafeValue()));
        db.reset(raw);
        auto migrateResult = db->migrate();
        if (migrateResult.isFail()) {
            LOG_FATAL("数据库迁移失败: " + migrateResult.errorMessage());
            std::abort();
        }
        // 注册给 module_* 层（onStart 里通过 core::appDatabase() 拿同一份连接）
        ::core::registerAppDatabase(&db->db());
        LOG_INFO("数据库已就绪: " + ::core::paths::sqliteDbFile().string());
        // 调用日志滚动清理（spec §11.5 保留策略）：保留天数读设置页配置
        //（T11 集成接线：config.json 的 logRetentionDays，默认 30，
        // 0 = 永久保留跳过清理）；幂等，失败只记日志不阻断启动
        const module::settings::SettingsStore settingsStore(
            ::core::paths::configFile());
        const int retentionDays = settingsStore.load().logRetentionDays;
        if (retentionDays > 0) {
            const auto pruned = db->pruneCallLogs(retentionDays);
            if (pruned.isOk()) {
                if (pruned.unsafeValue() > 0) {
                    LOG_INFO("已清理 " + std::to_string(pruned.unsafeValue()) +
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

::core::Router* getRouter() {
    static ::core::Router router;
    static std::unique_ptr<::core::Module> home(create_home_module());
    static std::unique_ptr<::core::Module> markdown(create_markdown_module());
    static std::unique_ptr<::core::Module> todo(create_todo_module());
    static std::unique_ptr<::core::Module> claude(create_claude_module());
    static std::unique_ptr<::core::Module> settings(create_settings_module());

    static bool initialized = false;
    if (!initialized) {
        // 注册给 module_* 层（快捷按钮/卡片点击通过 core::appRouter() 导航）
        ::core::registerAppRouter(&router);
        router.registerModule(home.get());
        router.registerModule(markdown.get());
        router.registerModule(todo.get());
        router.registerModule(claude.get());
        router.registerModule(settings.get());
        router.startAll();

        // 架构准备：如果有 pending 文件，dispatch 到对应 Page
        // MVP：仅切换页面；实际加载文件由对应模块在 onStart/P 阶段实现
        const std::string& pendingFile = pendingOpenFilePath();
        if (!pendingFile.empty()) {
            ::core::PageId target = dispatchPageForFile(pendingFile);
            LOG_INFO("MVP dispatch: 切换到 " +
                     std::string(pageIdToString(target)) +
                     "（实际文件加载留待 P 阶段）");
            router.navigateTo(target);
        }
        initialized = true;
    }
    return &router;
}

// Apple Event 路径：Finder 双击 / 右键「打开方式」/ 运行中再次 open -a
// 由 EUI-NEO 的 app::DslAppConfig::onOpenFile 回调驱动（kAEOpenDocuments），
// 在主循环每帧渲染前触发，这里可以安全地切页。
void handleAppleEventOpenFile(const std::string& path) {
    if (path.empty()) return;

    pendingOpenFilePath() = path;
    LOG_INFO("捕获到待打开文件（Apple Event）: " + path);

    const ::core::PageId target = dispatchPageForFile(path);
    LOG_INFO("Apple Event dispatch: 切换到 " +
             std::string(pageIdToString(target)) +
             "（实际文件加载留待 P 阶段）");

    ::core::Router* router = getRouter();
    if (router != nullptr) {
        router->navigateTo(target);
    }
}

// ---- Claude 代理（全局唯一，spec §10.4）----

// ClaudeService 单例（ClaudeModule 通过 ClaudeService::global() 读状态）
// 持有 Database 的 db() 引用；生命周期与 openAppDatabase() 一致
::module::claude::ClaudeService* getClaudeService() {
    static std::unique_ptr<::module::claude::ClaudeService> svc;
    static bool initialized = false;
    if (!initialized) {
        auto& db = openAppDatabase();
        svc = std::make_unique<::module::claude::ClaudeService>(db.db());
        // Claude Code settings.json 接管通道（provider-config Phase C）：
        // 路径注入在此一次性完成，startProxy/stopProxy/setActiveProfile 的
        // 接管/还原触发收在 service 层，UI 不感知
        svc->setSettingsSyncPaths(
            ::module::claude::settings_sync::defaultPaths());
        ::module::claude::ClaudeService::setGlobal(svc.get());
        initialized = true;
    }
    return svc.get();
}

// 启动本地 Claude 代理（全局唯一实例）。
// 调用方：dslAppConfig 初始化（设置 claudeProxyAutoStart=true 时）；手动启动
// 走 Claude 页的启动按钮（provider-config Phase B 硬性需求 1）
void startClaudeProxy() {
    auto* svc = getClaudeService();
    if (!svc) {
        LOG_ERROR("startClaudeProxy: ClaudeService 初始化失败");
        return;
    }
    if (svc->isProxyRunning()) {
        LOG_INFO("Claude 代理已在运行 port=" + std::to_string(svc->proxyPort()));
        return;
    }

    // port=8888 是约定端口；若被占用则 bind 失败，调用方在日志里可见
    auto r = svc->startProxy("127.0.0.1", 8888);
    if (r.isFail()) {
        LOG_ERROR("Claude 代理启动失败: " + r.errorMessage());
        return;
    }
    // -fno-exceptions：用 unsafeValue() 而非 value()
    const int port = static_cast<int>(r.unsafeValue());
    // 启动成功日志由 ClaudeService::startProxy 统一输出一次
    // （claude_service.cpp "Claude 代理已启动 <host>:<port> (auth=on)"），
    // 这里不再重复记录，避免一次启动打两条"已启动"
    ::module::claude::writeProxyTokenFile(svc->proxyAuthToken(), port);
}

} // namespace

namespace app {

// ---- 常驻侧边栏（spec §2 预期效果 / §6.5.2 侧边栏规格）----
// 布局：row(侧边栏 260px, 页面内容)。侧边栏列出 Router 已注册的全部模块
//（含设置），当前页高亮，点击 navigateTo。EUI-NEO components/sidebar 是
//右侧滑出抽屉（scrim + 关闭按钮，见 ../EUI-NEO/components/sidebar.h），
//不适合常驻导航栏，因此这里按 spec token 自绘（设计 token 属 DevDesk 自己的事）。
void drawSidebar(eui::Ui& ui, const eui::Screen& screen) {
    ::core::Router* router = getRouter();
    if (!router) return;
    const auto& t = ::core::theme::current();

    // 渲染自证：首帧挂载日志（侧边栏每帧重绘，static 只记一次）
    static const bool mounted = [] {
        LOG_INFO("侧边栏导航已挂载（" +
                 std::to_string(getRouter()->modules().size()) +
                 " 个模块入口，宽 260px）");
        return true;
    }();
    (void)mounted;

    constexpr float kWidth = 260.0f;   // spec §6.5.2：侧边栏宽 260（不是 200-220）
    const float itemH = 40.0f;         // 导航项高 40
    const float gap = 4.0f;            // 导航项 gap 4
    const float padX = 16.0f;          // 左右 padding 16

    ui.stack("app.sidebar")
        .size(kWidth, screen.height)
        .content([&] {
            ui.rect("app.sidebar.bg")
                .size(kWidth, screen.height)
                .color(t.colors.surface)
                .build();
            ui.rect("app.sidebar.divider")
                .position(kWidth - 1.0f, 0.0f)
                .size(1.0f, screen.height)
                .color(t.colors.borderMuted)
                .build();

            ui.text("app.sidebar.brand")
                .position(padX, 24.0f)
                .size(kWidth - padX * 2.0f, 32.0f)
                .text("DevDesk")
                .fontSize(t.sizes.fontSizePage)
                .fontWeight(700)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();

            float y = 84.0f;
            for (auto* m : router->modules()) {
                if (!m) continue;
                const ::core::PageId page = m->pageId();
                const bool active = (page == router->currentPage());
                const std::string id =
                    std::string("app.sidebar.item.") +
                    ::core::pageIdToString(page);

                // 命中区 + 悬停反馈（states: 常态/悬停/按下）
                ui.rect(id + ".hit")
                    .position(padX, y)
                    .size(kWidth - padX * 2.0f, itemH)
                    .states(active ? t.colors.primarySoftBg
                                   : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            t.colors.hover, t.colors.active)
                    .radius(t.sizes.radius)
                    .onClick([router, page] { router->navigateTo(page); })
                    .build();
                // 图标与文字分开两个元素：.icon() 用的 Icon 字体
                //（FontAwesome）不能和正文字体混排在一个 Text 里
                constexpr float kIconW = 28.0f;
                ui.text(id + ".icon")
                    .position(padX, y)
                    .size(kIconW, itemH)
                    .icon(m->iconGlyph())
                    .fontSize(t.sizes.fontSizeBody)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(active ? t.colors.primary : t.colors.textSecondary)
                    .build();
                ui.text(id + ".label")
                    .position(padX + kIconW, y)
                    .size(kWidth - padX * 2.0f - kIconW, itemH)
                    .text(m->displayName())
                    .fontSize(t.sizes.fontSizeBody)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(active ? t.colors.primary : t.colors.textSecondary)
                    .build();

                y += itemH + gap;
            }
        })
        .build();
}

// DslAppConfig 在 eui::dsl_app.h 中定义（命名空间 app）
const DslAppConfig& dslAppConfig() {
    // 第一步：恢复持久化主题 + 全局字号缩放（只读 config.json，无 DB 依赖）。
    // 必须先于下面 DslAppConfig 的构造执行：clearColor 在构造时从 theme 快照。
    // EUI-NEO 无运行时改 clearColor 的 API（DslWindowRuntime::request_ 是
    // initialize 时拷贝的快照且无私有成员 setter，见
    // ../EUI-NEO/core/app/dsl_window_runtime.h render 处），因此运行中切换
    // 主题时窗口底色不跟随，重启后生效（卡片/文字色仍即时生效）。
    static const bool themeRestored = []() {
        ::core::paths::ensureDirectoriesExist();
        setupLogging();
        // 标题栏深浅主题联动（磨砂设计 Phase A）：core 不依赖 EUI-NEO，
        // 通过 theme 回调注入——先装回调再恢复主题，启动恢复与运行时切换
        // （设置页 theme::setDark/setLight → 回调 → app::setTitleBarAppearance）
        // 走同一条路径，标题栏即时生效无需重启。仅 Windows 生效，其他平台
        // EUI-NEO 内部静默降级
        ::core::theme::setChangeHandler([](bool isDark) {
            app::setTitleBarAppearance(
                core::platform::TitleBarAppearance{isDark});
        });
        const module::settings::SettingsStore store(
            ::core::paths::configFile());
        const auto cfg = store.load();
        if (cfg.darkTheme) {
            ::core::theme::setDark();
        } else {
            ::core::theme::setLight();
        }
        ::core::theme::setFontScale(
            static_cast<float>(cfg.fontScalePercent) / 100.0f);
        return true;
    }();
    (void)themeRestored;

    static const DslAppConfig config = DslAppConfig{}
        .title("DevDesk")
        // Debug 构建下 EUI-NEO 默认把 FPS/CPU/GPU 拼进窗口标题
        //（dsl_app.h showDebugStatsInTitleValue），用户不需要，显式关掉
        .showDebugStatsInTitle(false)
        .pageId("devdesk")
        .clearColor(::core::theme::current().colors.background)  // 跟随主题
        .windowSize(1600, 1000)
        .minWindowSize(1200, 720)
        .highDpi(true)
        .resizable(true)
        // 启动快照：持久化主题恢复（上文 themeRestored）后按当前深浅设置；
        // 运行时切换由 theme 回调走 setTitleBarAppearance（覆盖本快照）
        .darkTitleBar(::core::theme::current().isDark)
        .centerWindow()
        // Finder 双击 / 右键「打开方式」：macOS 走 kAEOpenDocuments Apple Event，
        // 由 EUI-NEO 转成回调（主循环里派发），这里切到对应模块。
        .onOpenFile([](const std::string& path) { handleAppleEventOpenFile(path); })
        // 模块级快捷键通道：EUI-NEO 只把焦点元素未消费的按键回调到这里
        //（runtime_input dispatch 语义），转交 Router 派发到当前页模块。
        // 背景：InputBuilder 无逐组件 onKeyEvent passthrough（Phase 2 上游 PR），
        // 不涉及编辑器文本变换的快捷键（Todo Ctrl+↑/↓、Markdown Ctrl+Shift+P）走此通道。
        .onKeyEvent([](const eui::KeyEvent& key) {
            if (auto* router = getRouter()) {
                router->dispatchKeyEvent(key);
            }
        });

    static bool initialized = []() {
        using namespace core;
        // 必须在 setupLogging() 之后（themeRestored 里已完成）：内部有 LOG_INFO
        captureOpenFileFromArgv();
        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + paths::appDataDir().string());
        openAppDatabase();
        // ClaudeService 创建 + Claude Code 接管通道注入（spec §10.4）。
        // 必须在 getRouter()（内部 Router::startAll → 各模块 onStart）之前：
        // ClaudeService 的创建 + setGlobal 在 getClaudeService() 内完成，
        // 模块 onStart 才能拿到非 null 的 global()——W3 修过的启动顺序不能
        // 因为「默认不启动代理」回退，所以无论是否启动代理都必须先调它。
        auto* claudeSvc = getClaudeService();
        // 崩溃补还原（provider-config Phase C §5.3）：上次异常退出时 sidecar
        // 残留 → 先还原，且必须先于任何 startProxy（否则残留的托管键会被
        // 误快照为用户原值）
        if (claudeSvc) {
            claudeSvc->recoverPendingSettingsRestore();
        }
        // 硬性需求 1（provider-config Phase B）：代理默认不启动，
        // claudeProxyAutoStart 门控；启动/停止按钮在 Claude 页
        const module::settings::SettingsStore settingsStore(
            paths::configFile());
        if (settingsStore.load().claudeProxyAutoStart) {
            startClaudeProxy();
        } else {
            LOG_INFO("Claude 代理未自动启动（claudeProxyAutoStart=false，"
                     "可在设置页或 Claude 页手动启动）");
        }
        getRouter();
        return true;
    }();
    (void)initialized;

    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    auto* router = getRouter();
    if (!router) return;
    // 布局：row(常驻侧边栏, 页面内容)——spec §2"左侧侧边栏切换工具，右侧主区"
    constexpr float kSidebarWidth = 260.0f;
    ui.row("app.root")
        .size(screen.width, screen.height)
        .content([&] {
            drawSidebar(ui, screen);
            const float pageW = screen.width - kSidebarWidth;
            ui.stack("app.page")
                .size(pageW, screen.height)
                .clip()
                .content([&] {
                    // 页面模块按"去掉侧边栏后的宽度"布局（Screen 只含宽高）
                    eui::Screen pageScreen;
                    pageScreen.width = pageW;
                    pageScreen.height = screen.height;
                    router->compose(ui, pageScreen);
                })
                .build();
        })
        .build();
}

} // namespace app