// src/app_assembly.h
// 应用装配层：从 main.cpp 拆出，使装配顺序/门控接线/退出接线可无头单测（devdesk_tests 不含
// main.cpp）。DB 与配置来源改为显式参数注入，测试用桩模块 + 临时目录驱动；main.cpp 只留编排。
// 装配顺序（结构保证，勿改）：preStart → registerAppRouter → 门控注册 → startAll → armExitHook；
// preStart 里是 ClaudeService 创建 + setGlobal，必须先于任何模块 onStart（模块 onStart 经
// ClaudeService::global() 取共享单例，即 W3 事故点）。
#pragma once

#include "app_registry.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/tray_menu_spec.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_window.h"  // 纯数据头（零 EUI-NEO 依赖）
#include "settings_config.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app::assembly {

// 模块装配

// 待注册清单（buildGatedEntries 产出；调用方按 page 填实例后传 assembleAndStart）。注册顺序 =
// 列表顺序；生命周期由调用方持有（main.cpp 里是 getRouter() 的 static unique_ptr，须长于 Router
// 且先于 armExitHook 构造）
struct GatedEntry {
    ::core::PageId page;      // 表行页（供调用方按页装实例/断言表序）
    const char* gatingKey;    // nullptr = 恒注册（表行原样透传）
    ::core::Module* module;   // nullptr = 容错跳过（registerModule 同款）
};

// 表 → 待注册清单（纯转换+门控，不触 onStart/不调工厂）：恒注册行必含，门控行
// moduleEnabled(cfg,key) 才含，序=表序；module 由调用方按 page 填入
std::vector<GatedEntry> buildGatedEntries(
    const module::settings::SettingsConfig& cfg,
    const ::app::registry::ModuleEntry* table, std::size_t tableSize);

// 前置装配回调：早于一切模块注册/onStart（W3 顺序保证）。生产传 Claude 前置段，测试传记账闭包
using PreStartHook = std::function<void()>;

// 装配并启动（不抛异常），返回实际注册的 PageId 顺序（供顺序断言）。本函数纯注册，门控已在
// buildGatedEntries 前置；关闭的模块不注册 = onStart/compose/菜单项不可达（结构性保证）
std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const std::vector<GatedEntry>& modules, const PreStartHook& preStart);

// 桌宠开窗

// 桌宠门控开窗：cfg 含 kModuleIdPet 才 loadPetConfig() 并调 startFn(config)；门控谓词与装配
// 同源。只承诺「门控两态」，开窗与 getRouter 之间的装配顺序无护栏
void maybeStartPet(
    const module::settings::SettingsConfig& cfg,
    const std::function<module::pet::PetConfig()>& loadPetConfig,
    const std::function<void(const module::pet::PetConfig&)>& startFn);

// 退出接线
// EUI-NEO 无 onExit：关窗与托盘 Exit 都汇合于 eui_app_run 的同一 return，故此处 handler 两条
// 路径都执行。必须在全部模块静态对象构造完成后调用（LIFO：handler 先于 modules/router 析构，
// stopAll 时模块仍活着，各模块 onStop 可还原 settings.json）
bool armExitHook();    // 幂等：已挂过返回 false（不重复挂 atexit）
bool exitHookArmed();  // 供断言/排查
void runExitHook();    // handler 主体：core::appRouter() 非空 → Router::stopAll

// 测试缝：把「退出接线已挂」记账门回厂（atexit 不可撤销，只能翻记账位；handlers 各自判空
// 幂等，重复触发无害），resetCaseGlobals 用例间隔离用
void resetAssemblyGlobals();

// 待打开文件：Finder「打开方式」/ argv 两条来源写这里，dispatchFileOpen 读它分发
//（解析进笔记目录后以 "open:" 负载真加载）
std::string& pendingOpenFilePath();

// 从 argv 捕获要打开的文件（仅第一次调用执行；须在 setupLogging 之后否则 LOG_INFO 丢失）
void captureOpenFileFromArgv();

// 测试缝：argv 来源参数化（传已解码 UTF-8 的 argv 数组，argv[0]=exe），选择逻辑与生产同源
//（取 argv[1]、跳过 '-' 开头 flag），幂等门与无参版共用；仅供单测直接驱动
void captureOpenFileFromArgv(const std::vector<std::string>& utf8Args);

// 测试缝：复位「已捕获」门（幂等状态是进程级的）
void resetArgvCaptureForTest();

// 文件打开分发

// 根据扩展名返回目标 PageId（仅目标选择；内容加载由 dispatchFileOpen 完成）
::core::PageId dispatchPageForFile(const std::string& path);

// 绝对路径经 md_file_open 解析成笔记目录内相对路径，以 "open:<相对路径>" 走 Markdown 的
// consumeNavPayload 通道。降级：模块未注册 → Router 回退 Home 且不投递负载；解析失败 → 只切页+WARN
void dispatchFileOpen(::core::Router* router, const std::string& path,
                      const std::filesystem::path& notesDir);

// 托盘动作

// 导航类托盘动作 → 目标页；非导航动作（显示主窗口/退出/None）→ nullopt
std::optional<::core::PageId> pageForTrayAction(::core::tray::TrayMenuAction action);

// 页名（关闭模块的提示文案用）
const char* trayPageDisplayName(::core::PageId page);

// 导航到托盘指定页；模块未注册时唤起主窗 + toast 解释后仍落到首页（不静默回退）
void navigateFromTray(::core::Router* router, ::core::PageId page);

// 托盘动作委托：Navigate* → pageForTrayAction + navigateFromTray 返回 true；其余返回 false 由调用方处理
bool dispatchTrayNavigation(::core::tray::TrayMenuAction action,
                            ::core::Router& router);

// 模块开关即时生效（活门控）

// 模块实例查找缝：PageId → 模块对象指针（表新增页不在持有集 → nullptr）。函数指针形态与
// ToggleHandler 的无捕获约束同构
using ModuleLookup = ::core::Module* (*)(::core::PageId page);

// 模块开关即时生效执行体（settings 页拨开关 → persist → app::toggle 桥 → 这里）：
//   on  = registerModule → onStart（gatingKey 现查注册表，与设置页谓词双保险）
//   off = onStop 先行（运行线程在 onStop 内 join，页消失前收束）→ unregisterModule；
//         若 router 当前页 == 该页 → navigateTo(Home) + showAppNotice("模块已关闭", ...)
// moduleId 不在表内 / 命中行无 gatingKey（恒开模块）→ LOG_WARN + no-op。
// 本函数不读应用配置，使能判定已由 settings 侧完成；router/lookup 注入（生产=main.cpp，测试=桩）
void dispatchModuleToggle(const std::string& moduleId, bool on,
                          ::core::Router& router, ModuleLookup lookup);

} // namespace app::assembly
