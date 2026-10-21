// 应用装配层：装配顺序/门控接线/退出接线可无头单测（devdesk_tests 不含 main.cpp）。
// 装配顺序（结构保证，勿改）：preStart → registerAppRouter → 门控注册 → startAll → armExitHook；
// preStart 里的 ClaudeService 创建+setGlobal 必须先于任何模块 onStart（onStart 经 global() 取单例）
#pragma once

#include "app_registry.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/tray_menu_spec.h"
#include "settings_config.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app::assembly {

// 待注册清单（buildGatedEntries 产出，调用方按 page 填实例后传 assembleAndStart）；生命周期由
// 调用方持有（须长于 Router 且先于 armExitHook 构造）
struct GatedEntry {
    ::core::PageId page;      // 表行页（供调用方按页装实例/断言表序）
    const char* gatingKey;    // nullptr = 恒注册（表行原样透传）
    ::core::Module* module;   // nullptr = 容错跳过（registerModule 同款）
};

// 表 → 待注册清单（纯转换+门控，不触 onStart/不调工厂）：门控行 moduleEnabled 才含，序=表序
std::vector<GatedEntry> buildGatedEntries(
    const module::settings::SettingsConfig& cfg,
    const ::app::registry::ModuleEntry* table, std::size_t tableSize);

// 前置装配回调：早于一切模块注册/onStart。生产传 Claude 前置段，测试传记账闭包
using PreStartHook = std::function<void()>;

// 装配并启动（不抛异常），返回实际注册的 PageId 顺序（供顺序断言）；关闭的模块不注册 =
// onStart/compose/菜单项不可达（结构性保证）
std::vector<::core::PageId> assembleAndStart(
    ::core::Router& router, const module::settings::SettingsConfig& cfg,
    const std::vector<GatedEntry>& modules, const PreStartHook& preStart);

// 导航到托盘指定页；模块未注册时唤起主窗 + toast 解释后仍落到首页（不静默回退）
void navigateFromTray(::core::Router* router, ::core::PageId page);

// EUI-NEO 无 onExit：关窗与托盘 Exit 汇合于同一 return；须在全部模块静态对象构造完成后调用
//（LIFO：stopAll 时模块仍活着，onStop 可还原 settings.json）
bool armExitHook();    // 幂等：已挂过返回 false（不重复挂 atexit）
bool exitHookArmed();  // 供断言/排查
void runExitHook();    // handler 主体：core::appRouter() 非空 → Router::stopAll

// 测试缝：回翻「退出接线已挂」记账门（atexit 不可撤销，只能翻记账位），用例间隔离用
void resetAssemblyGlobals();

// 待打开文件（Finder「打开方式」/argv 两条来源写这里），dispatchFileOpen 读它分发
std::string& pendingOpenFilePath();

// 从 argv 捕获要打开的文件（仅首次执行；须在 setupLogging 之后否则 LOG_INFO 丢失）
void captureOpenFileFromArgv();

// 测试缝：argv 来源参数化（UTF-8 数组，argv[0]=exe；取 argv[1]、跳过 '-' 开头 flag），仅供单测
void captureOpenFileFromArgv(const std::vector<std::string>& utf8Args);

// 测试缝：复位「已捕获」门（幂等状态是进程级的）
void resetArgvCaptureForTest();

// ── --minimized 启动旗标（UAT6 开机自启动）──
// 开机自启动注册项带 --minimized；命中则启动后主窗保持托盘态（不显示、仅托盘，
// 点托盘恢复）。这里只捕获；执行须等主窗句柄由 EUI-NEO 主循环回写后，由
// app::compose 首帧经 consumePendingMinimizedStartup 取走、走 close-to-tray 同款
// WM_CLOSE→隐藏路径（见 app_sidebar.cpp compose 顶部）。
void captureMinimizedFlagFromArgv();

// 测试缝：argv 来源参数化（UTF-8 数组，argv[0]=exe）；返回本次捕获是否命中旗标。
// 生产捕获同款幂等；须在 setupLogging 之后调用（命中时记 LOG）
bool captureMinimizedFlagFromArgv(const std::vector<std::string>& utf8Args);

// 测试缝：回翻捕获门与待执行旗标（用例间隔离）
void resetMinimizedFlagForTest();

// 首帧一次性取走「--minimized 启动」待执行请求（取走即清，compose 侧每帧调用无害）
bool consumePendingMinimizedStartup();

// 根据扩展名返回目标 PageId（仅目标选择；内容加载由 dispatchFileOpen 完成）
::core::PageId dispatchPageForFile(const std::string& path);

// 绝对路径解析成笔记目录内相对路径，以 "open:<相对路径>" 走 Markdown 的 consumeNavPayload 通道。
// 降级：模块未注册 → Router 回退 Home 且不投递负载；解析失败 → 只切页+WARN
void dispatchFileOpen(::core::Router* router, const std::string& path,
                      const std::filesystem::path& notesDir);

// 导航类托盘动作 → 目标页；非导航动作（显示主窗口/退出/None）→ nullopt
std::optional<::core::PageId> pageForTrayAction(::core::tray::TrayMenuAction action);

// 页名（关闭模块的提示文案用）
const char* trayPageDisplayName(::core::PageId page);

// 导航到托盘指定页；模块未注册时唤起主窗 + toast 解释后仍落到首页（不静默回退）
void navigateFromTray(::core::Router* router, ::core::PageId page);

// 托盘动作委托：Navigate* → pageForTrayAction + navigateFromTray 返回 true；其余返回 false 由调用方处理
bool dispatchTrayNavigation(::core::tray::TrayMenuAction action,
                            ::core::Router& router);

// 模块实例查找缝：PageId → 模块对象指针（表新增页不在持有集 → nullptr）
using ModuleLookup = ::core::Module* (*)(::core::PageId page);

// 模块开关即时生效执行体：on = registerModule → onStart；off = onStop（运行线程 join）→
// unregisterModule，当前页==该页时再 navigateTo(Home)+notice；未知/恒开模块 → LOG_WARN+no-op
void dispatchModuleToggle(const std::string& moduleId, bool on,
                          ::core::Router& router, ModuleLookup lookup);

} // namespace app::assembly
