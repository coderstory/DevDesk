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

#include "app_registry.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/tray_menu_spec.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_window.h"  // 纯数据头（PetWindowConfig 等声明，零 EUI-NEO 依赖）
#include "settings_config.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app::assembly {

// ---- 模块装配（原 main.cpp getRouter 的内联装配段）----

// 待注册清单（表 → 门控的产物，buildGatedEntries 产出；调用方按 page 填入
// 实例后再传 assembleAndStart）。注册顺序 = 列表顺序（消除旧定长结构的
// 「字段顺序即注册顺序」隐式不变量）；生命周期由调用方持有——main.cpp 里
// 是 getRouter() 的函数级 static unique_ptr（必须长于 Router，且先于
// armExitHook 构造，见下 LIFO 约定）；module == nullptr 按
// Router::registerModule 语义 = 不注册（容错跳过）
struct GatedEntry {
    ::core::PageId page;      // 表行页（供调用方按页装实例 / 断言表序）
    const char* gatingKey;    // nullptr = 恒注册（表行原样透传）
    ::core::Module* module;   // nullptr = 容错跳过（registerModule 同款）
};

// 表 → 待注册清单（纯转换 + 门控；不触 onStart、不调工厂）：恒注册行
//（gatingKey==nullptr）必含；门控行 moduleEnabled(cfg, key) 才含；序 = 表序。
// module 实例由调用方按 page 从自有 static 持有填入（生命周期仍归 main.cpp
// 函数级 static，LIFO 约定不变）。真实表遍历因此可无头单测——测试 TU
// include app_registry.h 拿到同一份 kModuleRegistry
std::vector<GatedEntry> buildGatedEntries(
    const module::settings::SettingsConfig& cfg,
    const ::app::registry::ModuleEntry* table, std::size_t tableSize);

// 前置装配回调：早于一切模块注册/onStart 执行（W3 启动顺序的结构保证）。
// 生产传 main.cpp 的 Claude 前置段（ClaudeService 创建 + setGlobal + 端口
// 注入 + 崩溃补还原 + 条件自启）；测试传记账闭包验证顺序
using PreStartHook = std::function<void()>;

// 装配并启动（不抛异常）。返回**实际注册**的 PageId 顺序（供顺序断言）。
// 收 GatedEntry 列表后**纯注册**（门控已由 buildGatedEntries 前置——本函数
// 内 moduleEnabled 恰 0 处、全项目恰 1 处）。关闭的模块不注册 =
// onStart/compose/菜单项全部不可达，"零执行零菜单项"是结构性保证，
// 不靠模块自觉检查开关（设计 2026-09-22 §5.2）
std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const std::vector<GatedEntry>& modules, const PreStartHook& preStart);

// ---- 桌宠开窗（原 main.cpp 的 pet 门控段；PAB-04 收窄版）----

// 桌宠门控开窗：enabledModules 含 kModuleIdPet 才 loadPetConfig() 并调
// startFn(config) —— 装载与启动都注入，生产路径（开窗 + 配置读取）整体进
// 测试面。门控谓词与装配同源（同一个 moduleEnabled / 同一份 cfg 参数——
// 生产传 isModuleEnabled 同源的 gatingConfigSnapshot()）。只承诺「门控
// 两态」；开窗之后与 getRouter 之间的装配顺序无护栏（main.cpp :618 vs
// :622），顺序锁待 app_boot 抽出时另案
void maybeStartPet(
    const module::settings::SettingsConfig& cfg,
    const std::function<module::pet::PetConfig()>& loadPetConfig,
    const std::function<void(const module::pet::PetConfig&)>& startFn);

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

// 测试缝（TIN-07）：进程级「退出接线已挂」记账门回厂（未挂）。atexit 一旦
// 注册不可撤销，这里只翻幂等记账位——后续 armExitHook 视为未挂可再挂
//（handlers 各自经 core::appRouter() 判空幂等，重复触发无害）；
// resetCaseGlobals 用例间隔离用
void resetAssemblyGlobals();

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

// 托盘动作委托（PAB-03）：Navigate* → pageForTrayAction + navigateFromTray，
// 返回 true；其余（ShowMainWindow/Exit/None）返回 false 由调用方处理——
// 动作→页面映射保持单点在 pageForTrayAction（有测），main.cpp 不再写平行 switch
bool dispatchTrayNavigation(::core::tray::TrayMenuAction action,
                            ::core::Router& router);

} // namespace app::assembly
