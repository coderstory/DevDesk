// src/main.cpp
// DevDesk 入口 - EUI-NEO 提供 main 函数，我们只需提供
// app::dslAppConfig() 和 app::compose()

#include "app_sidebar.h"
#include "claude_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_window.h"
#include "platform/file_open.h"
#include "settings_config.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <memory>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_settings_module();

namespace {

// ---- 模块开关（模块插拔方案 A：编译期内置 + 运行时注册门控）----
// 关闭 = 不注册进 Router（onStart/compose/onKeyEvent 全部不可达，零执行）
// + 其专属服务不启动（Claude 代理）。生效时机：重启（注册是启动期行为，
// 设计 §5.4）。新模块必须在 getRouter() 装配处走 isModuleEnabled() 门控，
// 否则无法被用户关闭（设计 §8 风险约定）。
bool isModuleEnabled(const char* moduleId) {
    // 进程内读一次 config.json（static 局部缓存；设置页改开关只影响下次启动）
    static const std::vector<std::string> enabled = [] {
        const module::settings::SettingsStore store(
            ::core::paths::configFile());
        return store.load().enabledModules;
    }();
    if (std::string(moduleId) == module::settings::kModuleIdHome ||
        std::string(moduleId) == module::settings::kModuleIdSettings) {
        return true;  // 恒启用：落地页 + 开关 UI 宿主
    }
    return std::find(enabled.begin(), enabled.end(), moduleId) != enabled.end();
}

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
        // 注册门控（方案 A）：home/settings 恒注册；三个工具模块按
        // enabledModules 决定——不注册 = onStart/compose/菜单项全部不可达，
        // "零执行零菜单项"是结构性保证，不靠模块自觉检查开关
        router.registerModule(home.get());
        if (isModuleEnabled(module::settings::kModuleIdMarkdown)) {
            router.registerModule(markdown.get());
        } else {
            LOG_INFO("模块已关闭（enabledModules 不含 markdown），不注册");
        }
        if (isModuleEnabled(module::settings::kModuleIdTodo)) {
            router.registerModule(todo.get());
        } else {
            LOG_INFO("模块已关闭（enabledModules 不含 todo），不注册");
        }
        if (isModuleEnabled(module::settings::kModuleIdClaudeProxy)) {
            router.registerModule(claude.get());
        } else {
            LOG_INFO("模块已关闭（enabledModules 不含 claude_proxy），不注册");
        }
        router.registerModule(settings.get());
        router.startAll();

        // 应用退出还原接线（W2 终验问题 1）：EUI-NEO 无 onExit 回调，
        // main() 由 glfw_app_main 提供（eui_app_run → return → main return），
        // 返回后进程只剩静态析构/atexit 栈。关窗（glfwWindowShouldClose）
        // 与托盘 Exit（trayExitRequested → break）都汇合于 eui_app_run 的
        // 同一 return，因此这里注册的 atexit 在两条退出路径都会执行。
        // 必须在全部模块静态对象构造完成之后注册（LIFO：handler 先于
        // modules/router 静态析构运行，stopAll 时模块仍活着）→
        // Router::stopAll → ClaudeModule::onStop → stopProxy →
        // settings.json 还原，接管状态下退出不留死代理 BASE_URL。
        std::atexit([] {
            if (auto* r = ::core::appRouter()) {
                r->stopAll();
            }
        });

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

// ---- 常驻侧边栏 + 根布局 ----
// drawSidebar / compose 已移至 app_sidebar.{h,cpp}（2026-09-23 折叠/展开
// 改造时拆出：单测需要无头驱动，而这里的 getRouter() 拉起全部真实模块）

// DslAppConfig 在 eui::dsl_app.h 中定义（命名空间 app）
const DslAppConfig& dslAppConfig() {
    // 第一步：恢复持久化主题 + 全局字号缩放（只读 config.json，无 DB 依赖）。
    // 必须先于下面 DslAppConfig 的构造执行：clearColor 在构造时从 theme 快照
    //（运行时底色跟随走 app::setClearColor，EUI-NEO 磨砂 Phase B 起已有该 API，
    // 主题切换/窗口效果切档都即时生效，见下方 themeRestored 与 theme 回调）。
    static const bool themeRestored = []() {
        ::core::paths::ensureDirectoriesExist();
        setupLogging();
        // 标题栏深浅主题联动（磨砂设计 Phase A）：core 不依赖 EUI-NEO，
        // 通过 theme 回调注入——先装回调再恢复主题，启动恢复与运行时切换
        // （设置页 theme::setDark/setLight → 回调 → app::setTitleBarAppearance）
        // 走同一条路径，标题栏即时生效无需重启。仅 Windows 生效，其他平台
        // EUI-NEO 内部静默降级。
        // 窗口底色随主题即时跟随（磨砂 Phase B/D）：clearColor 走运行时 API，
        // alpha 按当前窗口效果档位（重读 config.json——主题切换是低频用户操作）
        ::core::theme::setChangeHandler([](bool isDark) {
            app::setTitleBarAppearance(
                core::platform::TitleBarAppearance{isDark});
            const module::settings::SettingsStore store(
                ::core::paths::configFile());
            module::settings::applyWindowVisuals(store.load().windowEffect);
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
        // 窗口效果启动接线（磨砂 Phase D）：clearColor alpha 按档 + backdrop
        // 请求（frosted → Acrylic）。窗口以透明帧缓冲创建（下方
        // windowEffect(Transparent) 常开 hint，创建期不可逆；off 档 alpha=1
        // 视觉等价实色），backdrop 在主循环首帧应用，降级真值经
        // app::activeWindowEffect() 回查（设置页提示）
        module::settings::applyWindowVisuals(cfg.windowEffect);
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
        // 窗口效果（磨砂 Phase D）：透明帧缓冲 hint 常开（创建期属性不可逆，
        // 三档运行时切换不重建窗口——off 档靠 clearColor alpha=1 视觉等价，
        // 见 themeRestored 里 applyWindowVisuals）。实际档位/backdrop 由
        // app::setWindowEffect 运行时驱动，此启动快照只负责开 hint
        .windowEffect(core::platform::WindowEffect::Transparent)
        // 启动快照：持久化主题恢复（上文 themeRestored）后按当前深浅设置；
        // 运行时切换由 theme 回调走 setTitleBarAppearance（覆盖本快照）
        .darkTitleBar(::core::theme::current().isDark)
        .centerWindow()
        // 托盘图标（EUI-NEO 自定义托盘菜单 API）：全中文自定义菜单，
        // keepDefault=false 去掉内置英文 Show/Exit（用户反馈菜单里混英文）。
        // 「显示主窗口」走 app::requestShow（处理隐藏到托盘/最小化/失焦三态），
        // 回调主线程触发、运行期 Router 已就绪，与侧边栏 onClick 同款
        // null-guard 写法；requestUpdate() 主动唤起重绘。
        .tray(true)
        .trayTitle("DevDesk")
        .trayMenu({
            {"显示主窗口", [] { app::requestShow(); }},
            {"-", nullptr},
            {"首页", [] { if (auto* r = getRouter()) { r->navigateTo(::core::PageId::Home); requestUpdate(); } }},
            {"笔记", [] { if (auto* r = getRouter()) { r->navigateTo(::core::PageId::Markdown); requestUpdate(); } }},
            {"待办", [] { if (auto* r = getRouter()) { r->navigateTo(::core::PageId::Todo); requestUpdate(); } }},
            {"Claude", [] { if (auto* r = getRouter()) { r->navigateTo(::core::PageId::ClaudeProxy); requestUpdate(); } }},
            {"-", nullptr},
            {"退出 DevDesk", [] { app::requestExit(); }},
        }, false)
        // Finder 双击 / 右键「打开方式」：macOS 走 kAEOpenDocuments Apple Event，
        // 由 EUI-NEO 转成回调（主循环里派发），这里切到对应模块。
        .onOpenFile([](const std::string& path) { handleAppleEventOpenFile(path); })
        // 模块级快捷键通道：EUI-NEO 只把焦点元素未消费的按键回调到这里
        //（runtime_input dispatch 语义），转交 Router 派发到当前页模块。
        // 背景：InputBuilder 无逐组件 onKeyEvent passthrough（Phase 2 上游 PR），
        // 不涉及编辑器文本变换的快捷键（Todo Ctrl+↑/↓、Markdown Ctrl+Shift+P）走此通道。
        .onKeyEvent([](const eui::KeyEvent& key) {
            // Ctrl+B 折叠/展开侧边栏（VS Code 同款语义）。选 Ctrl+B 而非
            // Ctrl+Shift+B：EUI-NEO 派发序 = 焦点元素优先——Markdown 编辑器
            // 聚焦时 Ctrl+B 已被 handleEditorKey 消费（加粗），到这里的一定
            // 不是编辑场景，天然让位无需特判（编辑器失焦 = 全局切换）
            if (key.isDown() && key.modifiers.shortcut() &&
                !key.modifiers.shift && !key.modifiers.alt &&
                key.key == eui::InputKey::B) {
                app::setSidebarCollapsed(!app::sidebarCollapsed(),
                                         ::core::paths::configFile());
                requestUpdate();
                return;
            }
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
        // 模块插拔门控（方案 A §5.2）：claude 模块关闭 = 该功能整体下线，
        // 含后台服务——不创建 ClaudeService（global() 保持 null）、不 bind
        // 8888、不写 proxy_token.txt。W3 的启动顺序约定（service 先于
        // getRouter）只在模块启用时原样保留
        const module::settings::SettingsStore settingsStore(
            paths::configFile());
        const auto settingsCfg = settingsStore.load();
        // 侧边栏折叠启动恢复（app_sidebar）：config.json → 运行时状态。
        // 空路径 = 只改内存不回写（保存发生在用户切换时）
        app::setSidebarCollapsed(settingsCfg.sidebarCollapsed);
        if (!module::settings::moduleEnabled(
                settingsCfg, module::settings::kModuleIdClaudeProxy)) {
            LOG_INFO("Claude 模块已关闭：代理/ClaudeService 整体下线"
                     "（8888 不监听；重新启用需改设置并重启）");
            // 陈旧凭据清理（设计 §8 风险表）：上一次运行留下的 token 已失效，
            // 删掉避免外部脚本误连（删不掉只记日志，不阻断启动）
            std::error_code ec;
            const auto tokenFile =
                paths::appDataDir() / "proxy_token.txt";
            if (std::filesystem::exists(tokenFile, ec) && !ec) {
                std::filesystem::remove(tokenFile, ec);
                LOG_INFO(ec ? "无法删除过期的 proxy_token.txt: " + ec.message()
                            : "已删除过期的 proxy_token.txt（模块关闭）");
            }
        } else {
            // ClaudeService 创建 + Claude Code 接管通道注入（spec §10.4）。
            // 必须在 getRouter()（内部 Router::startAll → 各模块 onStart）之前：
            // ClaudeService 的创建 + setGlobal 在 getClaudeService() 内完成，
            // 模块 onStart 才能拿到非 null 的 global()——W3 修过的启动顺序不能
            // 因为「默认不启动代理」回退，所以无论是否启动代理都必须先调它。
            auto* claudeSvc = getClaudeService();
            // 崩溃补还原（provider-config Phase C §5.3）：上次异常退出时
            // sidecar 残留 → 先还原，且必须先于任何 startProxy（否则残留的
            // 托管键会被误快照为用户原值）
            if (claudeSvc) {
                claudeSvc->recoverPendingSettingsRestore();
            }
            // 硬性需求 1（provider-config Phase B）：代理默认不启动，
            // claudeProxyAutoStart 门控；启动/停止按钮在 Claude 页
            if (settingsCfg.claudeProxyAutoStart) {
                startClaudeProxy();
            } else {
                LOG_INFO("Claude 代理未自动启动（claudeProxyAutoStart=false，"
                         "可在设置页或 Claude 页手动启动）");
            }
        }
        getRouter();
        // 桌宠（2026-09-22-desktop-pet-design.md §4.1）：不是 Router 页面模块，
        // 经同一 enabledModules 门控（含 "pet" 才开窗，默认关 = 零执行）。
        // openWindow 在主循环起来前入队、首帧消费；皮肤缺失时自动生成占位皮肤
        if (isModuleEnabled(module::settings::kModuleIdPet)) {
            // 桌宠自身配置在 petsDir()/pet_config.json（模块隔离，不走
            // config.json；pet 字段 2026-09-23 从 SettingsConfig 迁出）
            const module::pet::PetConfig petCfg =
                module::pet::PetConfigStore(module::pet::petConfigFile()).load();
            module::pet::startPetWindow(module::pet::PetWindowConfig{
                petCfg.skinName, petCfg.x, petCfg.y, petCfg.clickThrough});
        } else {
            LOG_INFO("桌宠已关闭（enabledModules 不含 pet），不开宠物窗口");
        }
        return true;
    }();
    (void)initialized;

    return config;
}

// 根布局 row(侧边栏, 页面内容) 已移至 app_sidebar.cpp 的 app::compose
//（EUI-NEO app main 每帧回调，符号解析与定义位置无关）

} // namespace app
