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

// argv 捕获门：仅在第一次调用时生效（captureOpenFileFromArgv 幂等）
bool g_argvCaptureDone = false;

// 捕获落点：写进程级 pending 路径 + 日志。空路径（无文件参数 / 只有 flag）不写
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
    (void)cfg;  // 门控已由 buildGatedEntries 前置（R8-C2），cfg 不再被读取

    // 1) 前置：ClaudeService 创建 + setGlobal（模块 onStart 依赖它非 null）
    if (preStart) {
        preStart();
    }

    // 2) 注册给 module_* 层（快捷按钮/卡片点击通过 core::appRouter() 导航）
    ::core::registerAppRouter(&router);

    // 2b) 空态兜底渲染注入（专家审计 2026-09-27 CP-01）：当前页无对应模块时
    // Router 不再自行绘制，改由 app 层注入这里。文案与样式**逐字搬自**
    // Router::compose 旧内联实现（id "empty" / 文案「无页面」/ 默认样式），
    // 观感零变化；core 侧只剩 std::function 成员与调用，恢复 spec §4.2.1
    // 「eui::Ui 仅可透传」边界（core 不 include EUI 组件头、不调其成员）
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

    // 3) 注册门控（方案 A）：清单已由 buildGatedEntries 按表门控（home/settings
    // 恒注册 + 四个工具模块（markdown/todo/claude_proxy/agent）按
    // enabledModules），这里**纯注册**——注册顺序 =
    // 列表顺序 = 表序；不注册 = onStart/compose/菜单项全部不可达，"零执行
    // 零菜单项"是结构性保证，不靠模块自觉检查开关
    for (const auto& entry : modules) {
        registerOne(entry.module);
    }

    // 4) 各模块 onStart（此刻 preStart 已跑完，ClaudeService::global() 非空）
    router.startAll();

    // 5) 退出接线（LIFO 约定见头文件）
    armExitHook();
    return registered;
}

std::vector<GatedEntry> buildGatedEntries(
    const module::settings::SettingsConfig& cfg,
    const ::app::registry::ModuleEntry* table, std::size_t tableSize) {
    // 一行遍历：恒注册行（gatingKey==nullptr）必含；门控行
    // moduleEnabled(cfg, key) 才含；序 = 表序。注册门控的唯一调用点
    //（assembleAndStart 内为 0；开窗门控另见 maybeStartPet——PAB-04，
    // 不同用途）。实例列恒为 nullptr，由调用方按 page 从自有 static
    // 持有填入（生命周期仍归调用方，LIFO 约定不变）
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

void maybeStartPet(
    const module::settings::SettingsConfig& cfg,
    const std::function<module::pet::PetConfig()>& loadPetConfig,
    const std::function<void(const module::pet::PetConfig&)>& startFn) {
    if (!module::settings::moduleEnabled(
            cfg, module::settings::kModuleIdPet)) {
        LOG_INFO("桌宠已关闭（enabledModules 不含 pet），不开宠物窗口");
        return;
    }
    if (!loadPetConfig || !startFn) {
        return;  // 回调未注入（测试/异常调用方）：容错不崩
    }
    startFn(loadPetConfig());
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

// 测试缝（TIN-07）：退出接线记账门回厂（未挂）。atexit 注册不可撤销，只翻幂等
// 记账位——用例间复位后 armExitHook 可再次触发（handlers 按 LIFO 叠加，每次
// 执行都以 core::appRouter() 当前值判空幂等、重复无害）；防 test_app_assembly
// 的「装配即挂上」断言吃到上一用例残留的已挂态
void resetAssemblyGlobals() { g_exitHookArmed = false; }

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
    // 生产路径：平台层读进程自身命令行（argv[1] 是文件路径；跳过 flag 类参数）
    capturePendingOpenFile(platform::file_open::openFileFromArgv());
}

void captureOpenFileFromArgv(const std::vector<std::string>& utf8Args) {
    // 测试缝：argv 来源参数化；选择逻辑与生产同源（selectOpenFileArg）
    capturePendingOpenFile(platform::file_open::selectOpenFileArg(utf8Args));
}

void resetArgvCaptureForTest() { g_argvCaptureDone = false; }

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
        // 已先判 isOk()：取失败信息外的值走 value()（失败即 LOG_FATAL + abort）
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
    // R8-C2：短名单源 = app_registry 表（与 Module::shortName() 同值，
    // 一致性由 test_app_assembly 的 registry 用例锁死）。表是静态元数据，
    // 不需要活实例——关闭模块的提示文案（navigateFromTray）仍可查。
    // 返回值与迁移前逐字相同（首页/笔记/待办/Claude/设置）
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
    // 活门控（Task 5 fix 轮 [2]）：与设置卡/开关即时生效同口径——去掉旧
    // 「重启后生效」字样，保留启用入口指引
    app::showAppNotice("模块已关闭",
                       std::string("「") + trayPageDisplayName(page) +
                           "」模块未启用，已转到首页。可在 设置 → 模块开关 "
                           "中即时启用。");
    app::requestUpdate();
}

void dispatchModuleToggle(const std::string& moduleId, bool on,
                          ::core::Router& router, ModuleLookup lookup) {
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
            router.registerModule(mod);  // 幂等（重复注册 = no-op，Task 1）
            mod->onStart();
        } else {
            // 先停后注销（D-1）：运行线程在 onStop 内 join，页消失前收束
            mod->onStop();
            router.unregisterModule(mod);
            // 当前页被注销 → 回首页 + toast。isRegistered(Home) 是防御分支
            //（Home 恒注册，navigateTo 兜底同款口径），用 router 实例查询
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
