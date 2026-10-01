// src/main.cpp
// DevDesk 入口 - EUI-NEO 提供 main 函数，我们只需提供
// app::dslAppConfig() 和 app::compose()

#include "app_assembly.h"
#include "app_registry.h"  // kModuleRegistry：页面模块登记表（含 7 个工厂声明）
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
#include "module_toggle_bridge.h"  // app::toggle::setToggleHandler（活门控注入）
#include "platform/tray_icon.h"  // 托盘图标按平台分发（macOS 蒙版 / Windows ico）
#include "settings_config.h"

#include <components/styledmenu.h>

#include <eui/dsl.h>
#include <eui/dsl_app.h>
#include <core/render/text.h>  // TextPrimitive::setDefaultFontFiles（默认 UI 字体覆盖）
// 主显示器工作区 / 显示器缩放系数（启动期窗口尺寸决策的取值来源）：
// EUI-NEO 目前只经 core/platform/platform.h → eui/app.h 传递该头，这里显式引入
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

// ---- 模块开关（模块插拔方案 A：编译期内置 + 运行时注册门控）----
// 关闭 = 不注册进 Router（onStart/compose/onKeyEvent 全部不可达，零执行）
// + 其专属服务不启动（Claude 代理）。生效时机：重启（注册是启动期行为，
// 设计 §5.4）。**装配处的门控注册已移至 app_assembly**（可无头单测，专家
// 审计 2026-09-27 M2）；这里保留同一份判定，供 pet 开窗与托盘菜单可用性。
//
// 进程内读一次 config.json（原 isModuleEnabled 的 static 局部缓存；设置页改
// 开关只影响下次启动）。判定复用设置页同一个谓词 module::settings::moduleEnabled
//（恒启用 home/settings、未知 ID 恒 false，语义已由 test_module_gating 覆盖）
const module::settings::SettingsConfig& gatingConfigSnapshot() {
    static const module::settings::SettingsConfig cfg = [] {
        const module::settings::SettingsStore store(
            ::core::paths::configFile());
        return store.load();
    }();
    return cfg;
}

bool isModuleEnabled(const char* moduleId) {
    return module::settings::moduleEnabled(gatingConfigSnapshot(), moduleId);
}

// 待打开文件的捕获（argv / Apple Event 两条来源）与文件打开分发
//（dispatchPageForFile / dispatchFileOpen）已移至 app_assembly（可无头单测，
// 专家审计 2026-09-27 M2）——notesDir 改为显式参数，行为与搬运前逐字一致。

void setupLogging() {
    core::log::addFileOutput(::core::paths::logFile());
    core::log::addConsoleOutput();
    core::log::setLevel(core::log::Level::Info);
}

// 打开应用数据库并落当前 schema
// 已先判 isFail()：取失败信息外的值走 value()（失败即 LOG_FATAL + abort，
// 与 TU 的编译选项无关，CP-08 取值口收敛）
::core::Database& openAppDatabase() {
    static std::unique_ptr<::core::Database> db;
    static bool initialized = false;
    if (!initialized) {
        auto r = ::core::Database::open(::core::paths::sqliteDbFile());
        if (r.isFail()) {
            LOG_FATAL("无法打开数据库: " + r.errorMessage());
            std::abort();
        }
        // value() 非 const 重载返回 T&，再用 std::move 转成 xvalue 触发
        // Database 的 move ctor。
        auto raw = new ::core::Database(std::move(r.value()));
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

// 启动前置段（定义在 getClaudeService/startClaudeProxy 之后：三者的定义顺序
// 保持搬运前原样）。getRouter() 经 app::assembly::PreStartHook 调用它 ——
// 结构上保证 ClaudeService 创建 + setGlobal 早于模块 onStart（W3 事故点）
void prepareClaudeServiceBeforeModules();

::core::Router* getRouter() {
    static ::core::Router router;
    // 工厂 + 生命周期持有仍在本文件：静态对象必须构造在 armExitHook()（由
    // assembleAndStart 注册后调用）之前——atexit 的 LIFO 才能保证 handler
    // 先于 modules/router 析构运行，stopAll 时模块仍活着
    static std::unique_ptr<::core::Module> home(create_home_module());
    static std::unique_ptr<::core::Module> markdown(create_markdown_module());
    static std::unique_ptr<::core::Module> todo(create_todo_module());
    static std::unique_ptr<::core::Module> claude(create_claude_module());
    static std::unique_ptr<::core::Module> settings(create_settings_module());
    static std::unique_ptr<::core::Module> agent(create_agent_module());
    static std::unique_ptr<::core::Module> about(create_about_module());

    static bool initialized = false;
    if (!initialized) {
        // 表 → 门控清单 → 按页填实例 → 装配（preStart → 注册 → startAll →
        // atexit；顺序有 headless 用例守着）。元数据见 app_registry.h、
        // 装配语义见 app_assembly.h；表新增页不在持有集 → nullptr 容错跳过。
        // 查找提为**无捕获** lambda：持有集 home/markdown/… 是本函数
        // static unique_ptr——静态存储期变量免捕获，故 lambda 零捕获、
        // 可转函数指针，与活门控 dispatchModuleToggle 的 ModuleLookup
        // 共用同一份查找（单点）
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

        // 启动时若有待打开文件（argv 捕获），解析成笔记负载分发（审计 [5]）
        app::assembly::dispatchFileOpen(
            &router, app::assembly::pendingOpenFilePath(),
            ::core::paths::markdownDir());

        // 活门控 handler 注入（Task 2 桥的真实现）：settings 页拨开关 →
        // persist → app::toggle::onModuleToggled → 这里。handler 是函数指针
        //（无捕获约束）：router 经 getRouter() 现取（此时装配已完成，再次
        // 调用只返回既有静态指针）；moduleForPage 是 constexpr 局部——
        // 无捕获 lambda 引用它经 lvalue-to-rvalue 读出常量表达式、非
        // odr-use，免捕获且值烙进编译期（无悬垂）。注册顺序在装配之后 =
        // 运行期拨开关才会触达，启动期行为零变化
        app::toggle::setToggleHandler(
            [](const std::string& moduleId, bool on) {
                app::assembly::dispatchModuleToggle(moduleId, on, *getRouter(),
                                                    moduleForPage);
            });
        initialized = true;
    }
    return &router;
}

// Apple Event 路径：Finder 双击 / 右键「打开方式」/ 运行中再次 open -a
// 由 EUI-NEO 的 app::DslAppConfig::onOpenFile 回调驱动（kAEOpenDocuments），
// 在主循环每帧渲染前触发，这里可以安全地切页。
void handleAppleEventOpenFile(const std::string& path) {
    if (path.empty()) return;

    app::assembly::pendingOpenFilePath() = path;
    LOG_INFO("捕获到待打开文件（Apple Event）: " + path);

    // 与 argv 启动路径同一分发逻辑：解析 + open: 负载（审计 [5]）
    app::assembly::dispatchFileOpen(getRouter(), path,
                                    ::core::paths::markdownDir());
}

// ---- Claude 代理（全局唯一，spec §10.4）----

// R0 / 审计 D6：core 只读服务定位器的「代理运行面」提供者。消费方（设置页）
// 不再 include 兄弟模块头、不再直呼 ClaudeService::global()——它经
// core::ServiceLocator::requireService<IClaudeProxyService>() 取本适配器；
// 适配器每次转发到 global() 单例，故「单例未装配（claude 模块关闭）」与解耦
// 前的 svc == nullptr 同义（各方法按服务不可用返回 false / 0 / 空串 / no-op）。
// 本对象常驻（进程级静态）、不持有 ClaudeService——所有权仍在 getClaudeService
// 的 unique_ptr 里，定位器只做只读查找。
::core::GlobalClaudeProxyService<::module::claude::ClaudeService>&
claudeProxyProvider() {
    static ::core::GlobalClaudeProxyService<::module::claude::ClaudeService>
        provider;
    return provider;
}

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
        // R0 / 审计 D6：与 setGlobal 同一时刻把代理运行面注册进 core 只读服务
        // 定位器（设置页经它取，不再直呼 global()）。注册点选在此 =
        //「单例已创建 ⟺ 定位器可解析」，与解耦前「global() 非空」逐字对应；
        // claude 模块关闭时本函数根本不被调用（prepareClaudeServiceBeforeModules
        // 提前 return）→ 定位器保持未注册 = 解耦前的 global() == nullptr
        ::core::ServiceLocator::set<::core::IClaudeProxyService>(
            &claudeProxyProvider());
        initialized = true;
    }
    return svc.get();
}

// 桌宠气泡内容源②（pet 设计 §4.5，审计 [18]）：跨模块逾期计数桥。
// getClaudeService 同型——app 层懒建只读 HomeService（其公开头仅依赖 core，
// 本就为跨页聚合设计），main 注入 startPetWindow 回调喂给 pet；pet 不
// include module_home，module_* 隔离不破（契约见 pet_window.h
// OverdueCountGetter）。
// 降级（M2 验收条款「关 todo 模块时回退通用语料」）：todo 模块关闭 →
// nullopt；聚合查询失败 → nullopt（Result 口径，不抛异常）。nullopt 一律
// 由 pet 侧解释为「数据不可用」→ 气泡回退静态语料①，不显示死数据
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
    // value() 失败即 LOG_FATAL + std::abort()，不抛异常（CP-08 取值口收敛），
    // 与 TU 的编译选项无关——本文件在非 Debug 档被 EUI 的 -fno-exceptions
    // genex 编译，抛异常会连编译都过不了；上面已提前 isFail() return，
    // 成功路径与 main.cpp:81 的既有约定一致
    return stats.value().overdueTodos;
}

// 桌宠气泡内容源③（M2 §4.5，产品审计 [18]）：代理事件桥。读 ClaudeService
// 单例的最近代理请求终态（ProxyRouter recordProxyEvent 写入；take 语义 =
// 读走即清 + 过期丢弃，一个事件最多喂一次气泡）。app 级回调——main 查
// claude_proxy 经 PetWindowConfig 喂给 pet；pet 不 include module_claude_proxy，
// module_* 隔离不破（契约见 pet_window.h ProxyEventGetter）。
// main 只透传不解释语义：statusCode 原值交给 pet 纯函数分类（2xx/3xx=完成、
// >=400=出错、0=上游不可达）；nullopt = 单例未就绪（claude_proxy 关）/
// 无待消费事件/事件过期，pet 侧按 ②逾期 → ①静态语料回退（不显示死数据条款）
std::optional<int> getProxyEventStatus() {
    auto* svc = module::claude::ClaudeService::global();
    if (svc == nullptr) {
        return std::nullopt;
    }
    return svc->takeProxyEvent();
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

    // 端口读设置页配置（审计 2026-09-23 [14]，默认 8888）；若被占用则 bind
    // 失败，调用方在日志里可见
    auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
    if (r.isFail()) {
        LOG_ERROR("Claude 代理启动失败: " + r.errorMessage());
        return;
    }
    // 启动成功日志由 ClaudeService::startProxy 统一输出一次，这里不重复记录
}

// 启动前置段（原 dslAppConfig 的 initialized lambda 里的 Claude 分支，逐字
// 搬运；专家审计 2026-09-27 M2）。由 getRouter() 经 app_assembly 的 preStart
// 钩子调用 → 结构上早于模块注册与 onStart，W3 修过的启动顺序不回退。
// 模块插拔门控（方案 A §5.2）：claude 模块关闭 = 该功能整体下线，含后台服务
// ——不创建 ClaudeService（global() 保持 null）、不 bind 8888
void prepareClaudeServiceBeforeModules() {
    const module::settings::SettingsStore settingsStore(
        ::core::paths::configFile());
    const auto settingsCfg = settingsStore.load();
    if (!module::settings::moduleEnabled(
            settingsCfg, module::settings::kModuleIdClaudeProxy)) {
        LOG_INFO("Claude 模块已关闭：代理/ClaudeService 整体下线（端口 " +
                 std::to_string(settingsCfg.proxyPort) +
                 " 不监听；重新启用需改设置并重启）");
        return;
    }
    // ClaudeService 创建 + Claude Code 接管通道注入（spec §10.4）。
    // ClaudeService 的创建 + setGlobal 在 getClaudeService() 内完成，模块
    // onStart 才能拿到非 null 的 global()——W3 修过的启动顺序不能因为
    // 「默认不启动代理」回退，所以无论是否启动代理都必须先调它。
    auto* claudeSvc = getClaudeService();
    // 配置端口注入（审计 [14]）：Claude 页手动启动 / 下方自启都经
    // service 的 configuredProxyPort 读值；设置页改端口时会再同步一次
    if (claudeSvc) {
        claudeSvc->setConfiguredProxyPort(settingsCfg.proxyPort);
    }
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

// ---- 托盘菜单 ----
// 唯一数据源 core::tray::trayMenuSpec（styled 主路径 + 原生降级同源）；
// 动作→页面映射（pageForTrayAction）、页名、导航与 Navigate* 委托
//（dispatchTrayNavigation，PAB-03）全在 app_assembly（可无头单测）

void executeTrayAction(::core::tray::TrayMenuAction action) {
    using Action = ::core::tray::TrayMenuAction;
    // Navigate* 全委托 app_assembly（PAB-03：映射单点 pageForTrayAction 有测）
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

// 原生降级菜单（onTrayMenu 返回 false 时弹这份；keepDefault=false：
// 全部来自 spec，无内置英文 Show/Exit）
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

// styled 托盘菜单（圆角/主题/FA 图标，独立弹窗不受任何窗 bounds 限制）。
// 返回 false = styled 构建失败 → 托盘走原生菜单降级。spec 按值捕获作为
// path {i} 映射基础
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

// ---- 启动期主窗口尺寸决策（2026-09-27 修复：贴底内容被任务栏吃掉）----
// 缺陷：.windowSize(1600, 1000) 直达后端 glfwCreateWindow，语义是**客户区窗口
// 单位**（Windows = 物理像素、macOS = 点，见 core/window_layout.h 文件头）；
// Windows 本机 1920×1080 @125%（工作区 1920×1020 物理、任务栏 60）下窗口外框
// 1618×1047 超出工作区高度 → 客户区底部约 52 物理 px 压在任务栏后面，md 笔记树
// 底部的「+ 笔记 / + 文件夹」按钮、编辑区状态栏、侧边栏折叠钮全部被切。
// 修法：请求尺寸按目标显示器缩放系数换算成窗口单位、加上非客户区外框经验值后
// 与工作区比较——放得下用请求尺寸，放不下则**启动即最大化**（用户可再拖）。
// 2026-09-28 组 A 补：**最小尺寸走同一条换算**再与工作区取 min——之前 min 原值
// 直传（漏了这一步），macOS 上 845 点 > 工作区 833 点 → 窗口结构性收不进屏；
// Windows 上同一数值被当物理像素 → 实际下限 676 逻辑，低于 845 的设计意图。
// 换算与工作区钳制是一对、必须同时存在：只换算不钳制会把 macOS 的病复制到
// Windows（845 逻辑 = 1056 物理 > 本机 1020 工作区）。
// 决策本身是不含 EUI-NEO/平台依赖的纯函数（core::window_layout，单测
// tests/test_core/test_window_layout.cpp），这里只负责取值与落地。
constexpr int kInitialWindowLogicalWidth = 1600;
constexpr int kInitialWindowLogicalHeight = 1000;
// 最小尺寸语义（**逻辑单位**，经 window_layout 换算成窗口单位后落地）
constexpr int kMinWindowLogicalWidth = 1330;
constexpr int kMinWindowLogicalHeight = 845;

// 决策落地形态：初始尺寸/形态 + 最小尺寸（同一套取值、同一条换算，避免两处
// 各算一次导致「最小窗和初始窗不同口径」）
struct ResolvedWindowSizing {
    core::window_layout::InitialWindowDecision initial;
    core::window_layout::WindowSize minSize;
};

ResolvedWindowSizing resolveWindowSizing() {
    // 目标显示器 = 主显示器（窗口走 centerWindow，无显式位置，落在主屏）
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    core::window::getPrimaryMonitorWorkArea(workX, workY, workW, workH);

    // 缩放系数按工作区中心点取自该显示器 DPI（窗口实际所在显示器）。
    // 工作区未知时不查（中心点无意义），按 1.0 降级——决策函数见工作区无效
    // 会原样保留请求尺寸，与修复前行为一致。
    // 注意：macOS 上本值恒 1.0（窗口单位是点），Retina 的 2.0 在 contentScale 上
    // （首帧由 app_sidebar 打印，EUI-NEO 侧见 [layoutdiag]）——别把两者读混。
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
    // 单位名走平台注入（macOS = pt / Windows = 物理px）：这行日志曾把 macOS 的
    // 「点」标成「物理px」，把排查者引向"Retina 没折算"的错误前提（2026-09-28 审计）
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
        // 默认 UI 字体：这里**不调** setDefaultFontFiles（调了即置 override、
        // 优先生效）。EUI-NEO resolveDefaultUiFontPath() 的优先级是
        // 「override → 系统 CJK 字体 → 打包兜底」，所以成品实际用的是系统字体：
        // Windows = C:/Windows/Fonts/msyh.ttc（微软雅黑），macOS = 苹方，
        // Linux = Noto CJK；打包的装饰体只作兜底（2026-09-27 EUI-NEO 改）。
        // 下面这行保持注释：NotoSansSC-VF 是变量字体，FreeType 加载不稳，
        // override 设了也会渲染回退 Bold → measureTextWidth(Noto)≠渲染(Bold)
        // → 布局错位，需换静态 Noto/Source Han 才可启用。
        // ::core::TextPrimitive::setDefaultFontFiles(
        //     "NotoSansSC-VF.ttf", "Font Awesome 7 Free-Solid-900.otf");
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

    // 初始窗口尺寸/形态决策（必须在 config 之前求值：config 静态初始化时消费）。
    // 取值时机：dslAppConfig() 首次调用发生在 EUI-NEO eui_app_run 的 glfwInit()
    // 之后（glfw_app_main.cpp:503 → :509 读 initialWindowWidth），此时显示器
    // 枚举/DPI 查询可用；万一不可用（无头、无显示器）决策函数原样保留请求尺寸。
    static const ResolvedWindowSizing initialWindow = resolveWindowSizing();

    static const DslAppConfig config = DslAppConfig{}
        .title("DevDesk")
        // Debug 构建下 EUI-NEO 默认把 FPS/CPU/GPU 拼进窗口标题
        //（dsl_app.h showDebugStatsInTitleValue），用户不需要，显式关掉
        .showDebugStatsInTitle(false)
        .pageId("devdesk")
        .clearColor(::core::theme::current().colors.background)  // 跟随主题
        // 初始尺寸（窗口单位，见 initialWindow 计算）：能覆盖显示器工作区就用
        // 请求的 1600×1000（逻辑单位 × 缩放），否则启动即最大化——修 2026-09-27
        // 「窗口比工作区高、贴底内容被任务栏吃掉」缺陷（决策细节见上方
        // resolveWindowSizing 与 core/window_layout.h）
        .windowSize(initialWindow.initial.width, initialWindow.initial.height)
        // 最小尺寸：**同一函数算出的窗口单位值**（换算 + 与工作区取 min，2026-09-28
        // 组 A）。不再把逻辑常量原值直传——直传 = 绕开换算，正是 macOS「845 点 >
        // 工作区 833 点，窗口收不进屏」与 Windows「845 被当物理像素 = 676 逻辑」的
        // 共同病因（细节见 core::window_layout::minWindowSizeForWorkArea）
        .minWindowSize(initialWindow.minSize.width, initialWindow.minSize.height)
        // 最大化通道：DslAppConfig::maximized → app::windowMaximized()
        //（dsl_app_impl.h:288）→ WindowCreateRequest::maximized
        //（glfw_app_main.cpp:522）→ glfwWindowHint(GLFW_MAXIMIZED)
        //（window_backend.cpp:776），创建期生效、用户可再拖回
        .maximized(initialWindow.initial.maximize)
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
        // 托盘图标（EUI-NEO 自定义托盘菜单 API）：菜单美化（2026-09-23）后
        // styled 独立菜单窗为主路径（onTrayMenu，圆角/主题/FA 图标），原生
        // trayMenu 保留为降级数据源（styled 构建失败时 EUI-NEO 自动弹这份，
        // keepDefault=false 去掉内置英文 Show/Exit——菜单项与 styled 同源，
        // 均来自 core::tray::trayMenuSpec）。
        // 左键 = 直接唤起主窗口（requestShow 处理隐藏到托盘/最小化/失焦三态），
        // 右键 = styled 菜单；回调主线程触发、运行期 Router 已就绪
        .tray(true)
        .trayTitle("DevDesk")
        // 托盘图标**按平台分发**（platform::tray::defaultIconPath()，业务代码零
        // 平台宏）：macOS = 22×22 纯黑+alpha 的 template 蒙版
        // （assets/tray-icon-template.png；EUI-NEO 拿到 NSImage 后无条件
        // setTemplate:YES，模板渲染只取 alpha 蒙版 —— 圆角方形应用图标会退化成
        // 纯色实心方块，这正是 Bug A 的成因）；Windows = 品牌 .ico
        // （assets/tray-icon-win.ico，16/32/48 三档 32bpp BGRA DIB —— Windows 侧
        // ExtractIconExW 只认 .ico 资源位，给 PNG 必然空图标）。
        // 相对路径经 EUI-NEO resolveIconPath 的候选链解析：build 目录启动命中
        // CWD/assets/，.app bundle 启动命中 exeDir/assets/（build-app.sh 把
        // <build>/assets 整树拷到 Contents/MacOS/assets），两种场景都覆盖。
        .trayIcon(platform::tray::defaultIconPath())
        .trayMenu(nativeTrayMenuItems(), false)
        .onTrayMenu([](int x, int y, bool leftButton) {
            if (leftButton) {
                executeTrayAction(::core::tray::TrayMenuAction::ShowMainWindow);
                return true;
            }
            return showTrayStyledMenu(x, y);
        })
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
        app::assembly::captureOpenFileFromArgv();
        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + paths::appDataDir().string());
        openAppDatabase();
        const module::settings::SettingsStore settingsStore(
            paths::configFile());
        const auto settingsCfg = settingsStore.load();
        // 侧边栏折叠启动恢复（app_sidebar）：config.json → 运行时状态。
        // 空路径 = 只改内存不回写（保存发生在用户切换时）
        app::setSidebarCollapsed(settingsCfg.sidebarCollapsed);
        // Claude 前置段（ClaudeService 创建 + setGlobal + 端口注入 + 崩溃补
        // 还原 + 条件自启）已随装配收进 getRouter() 的 preStart 钩子
        //（prepareClaudeServiceBeforeModules）——结构上仍早于模块 onStart，
        // W3 修过的启动顺序（service 先于 getRouter）不回退，见 app_assembly.h
        getRouter();
        // 桌宠（2026-09-22-desktop-pet-design.md §4.1）：非 Router 页面模块，
        // 门控/装载/开窗收进 app_assembly::maybeStartPet（PAB-04，无头可测）；
        // 开窗回调仍在本文件（气泡内容源②③ 跨模块只读桥在此注入）
        app::assembly::maybeStartPet(
            gatingConfigSnapshot(),
            [] {
                // 桌宠自身配置在 petsDir()/pet_config.json（模块隔离，不走
                // config.json；pet 字段 2026-09-23 从 SettingsConfig 迁出）
                return module::pet::PetConfigStore(
                           module::pet::petConfigFile())
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

// 根布局 row(侧边栏, 页面内容) 已移至 app_sidebar.cpp 的 app::compose
//（EUI-NEO app main 每帧回调，符号解析与定义位置无关）

} // namespace app
