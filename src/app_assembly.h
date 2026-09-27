// src/app_assembly.h
// 应用装配层（专家审计 2026-09-27 M2）
//
// 从 main.cpp 拆出的原因：历史上两次启动顺序事故都落在装配段——W3（模块
// onStart 时 ClaudeService::global() 仍为 null）与 W2（退出未还原
// settings.json），而 devdesk_tests 不含 main.cpp → 门控**语义**有测
//（test_module_gating），门控**接线**零测。这里把装配顺序、模块门控注册、
// 退出接线与几个纯函数收进来，并把 DB / 配置来源改为显式参数（注入模块指针
// + SettingsConfig + 笔记目录），测试用桩模块 + 临时目录直接无头驱动；
// main.cpp 只留编排调用（模块工厂持有、config.json 快照、桌宠开窗）。
//
// 装配顺序（结构保证，勿改）：
//   preStart → registerAppRouter → 门控注册 → startAll → armExitHook
//   preStart 里是 ClaudeService 创建 + setGlobal —— 必须先于任何模块 onStart
//  （模块 onStart 经 ClaudeService::global() 取共享单例，即 W3 事故点）。
#pragma once

#include "core/module_interface.h"
#include "core/router.h"
#include "core/tray_menu_spec.h"
#include "settings_config.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app::assembly {

// ---- 模块装配（原 main.cpp getRouter 的内联装配段）----

// 五个页面模块指针。注册顺序 = 字段顺序 = home → markdown → todo → claude →
// settings（与 main.cpp 原顺序逐字一致）：home/settings 恒注册（落地页 +
// 开关 UI 宿主），其余三个按 SettingsConfig::enabledModules 门控。
// 生命周期由调用方持有——main.cpp 里是 getRouter() 的函数级 static
// unique_ptr（必须长于 Router，且先于 armExitHook 构造，见下 LIFO 约定）；
// nullptr 按 Router::registerModule 语义 = 不注册
struct ModuleSet {
    ::core::Module* home = nullptr;
    ::core::Module* markdown = nullptr;
    ::core::Module* todo = nullptr;
    ::core::Module* claude = nullptr;
    ::core::Module* settings = nullptr;
};

// 前置装配回调：早于一切模块注册/onStart 执行（W3 启动顺序的结构保证）。
// 生产传 main.cpp 的 Claude 前置段（ClaudeService 创建 + setGlobal + 端口
// 注入 + 崩溃补还原 + 条件自启）；测试传记账闭包验证顺序
using PreStartHook = std::function<void()>;

// 装配并启动（不抛异常）。返回**实际注册**的 PageId 顺序（供顺序断言）。
// 关闭的模块不注册 = onStart/compose/菜单项全部不可达，"零执行零菜单项"
// 是结构性保证，不靠模块自觉检查开关（设计 2026-09-22 §5.2）
std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const ModuleSet& modules, const PreStartHook& preStart);

// ---- 退出接线（原 main.cpp getRouter 尾部的 atexit）----
// EUI-NEO 无 onExit 回调：关窗（glfwWindowShouldClose）与托盘 Exit
//（trayExitRequested → break）都汇合于 eui_app_run 的同一 return，返回后进程
// 只剩静态析构 / atexit 栈，因此这里注册的 handler 在两条退出路径都会执行。
// 必须在全部模块静态对象构造完成之后调用（LIFO：handler 先于 modules/router
// 静态析构运行，stopAll 时模块仍活着）→ 各模块 onStop（ClaudeModule::onStop
// → stopProxy → settings.json 还原，接管状态下退出不留死代理 BASE_URL）。
bool armExitHook();    // 幂等：已挂过返回 false（不重复挂 atexit）
bool exitHookArmed();  // 供断言/排查
void runExitHook();    // handler 主体：core::appRouter() 非空 → Router::stopAll

// ---- 待打开文件（原 main.cpp pendingOpenFilePath / captureOpenFileFromArgv）----
// Finder「打开方式」/ argv 两条来源都写这里；dispatchFileOpen 读它做启动分发。
// 审计 2026-09-23 [5] 后不再只留档：解析进笔记目录后以 "open:" 负载真加载
std::string& pendingOpenFilePath();

// 从 argv 捕获要打开的文件（仅在第一次调用时执行；须在 setupLogging 之后，
// 否则 LOG_INFO 会丢失）。平台差异在 platform/file_open_* 内隔离
void captureOpenFileFromArgv();

// 测试缝（审计 2026-09-27 F 残留 ②：本函数会消费测试进程自身 argv 并写进程级
// pending，无法直测）：argv 来源参数化——传「已解码为 UTF-8 的 argv 数组」
//（argv[0] = exe）。选择逻辑与生产同源（platform::file_open::selectOpenFileArg：
// 取 argv[1]、跳过 '-' 开头的 flag），幂等门与上方无参版本共用。
// main.cpp 只走无参版本，本重载仅供单测直接驱动捕获语义。
void captureOpenFileFromArgv(const std::vector<std::string>& utf8Args);

// 测试缝：复位「已捕获」门（幂等状态是进程级的，单测需逐个用例独立驱动）
void resetArgvCaptureForTest();

// ---- 文件打开分发（原 main.cpp dispatchPageForFile / dispatchFileOpen）----

// 根据文件扩展名返回目标 PageId（仅作目标选择；内容加载由 dispatchFileOpen
// 带负载完成）
::core::PageId dispatchPageForFile(const std::string& path);

// 把绝对路径经 md_file_open 解析成笔记目录内相对路径，再以
// "open:<相对路径>" 负载走 Markdown 模块 consumeNavPayload 的既有通道。
// 降级：Markdown 模块被关（未注册）→ Router 自带回退 Home 且不投递负载；
// 解析失败 → 只切页 + WARN 日志。
// notesDir 参数化 = 笔记目录（生产传 core::paths::markdownDir()，测试传临时目录）
void dispatchFileOpen(::core::Router* router, const std::string& path,
                      const std::filesystem::path& notesDir);

// ---- 托盘动作（原 main.cpp pageForTrayAction / trayPageDisplayName / navigateFromTray）----

// 导航类托盘动作 → 目标页；非导航动作（显示主窗口/退出/None）→ nullopt
std::optional<::core::PageId> pageForTrayAction(::core::tray::TrayMenuAction action);

// 页名（关闭模块的提示文案用）
const char* trayPageDisplayName(::core::PageId page);

// 导航到托盘指定页。router 参数化（原实现内部调 getRouter()）。模块未注册时
// 不再静默回退 Home（审计 [7]）：唤起主窗 + 应用级 toast 解释「怎么启用」，
// 仍落到首页（有落点，但用户知道为什么）
void navigateFromTray(::core::Router* router, ::core::PageId page);

} // namespace app::assembly
