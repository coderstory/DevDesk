// src/app_sidebar.h
// 常驻侧边栏 + 折叠/展开（spec §2 / §6.5.2；折叠态 2026-09-23）
//
// 从 main.cpp 拆出的原因：compose/drawSidebar 需要被无头单测直接驱动，
// 而 main.cpp 的 getRouter() 会拉起全部真实模块（DB/代理/桌宠），测试不可
// 用。这里改走 core::appRouter() 注入——生产里 getRouter() 初始化时已把
// 同一实例注册进去（registerAppRouter），测试用 ScopedAppRouter 换桩。
#pragma once

#include <eui/dsl.h>

#include <filesystem>
#include <string>

namespace app {

// 宽度 token：展开 208（2026-09-24 用户要求自 260 缩 20%；spec 原值 260）；
// 折叠 72 = 图标 28 + 余量
constexpr float kSidebarWidthExpanded = 208.0f;
constexpr float kSidebarWidthCollapsed = 72.0f;

// ---- 运行时折叠状态（进程内单值；启动由 main 从 app_config 表恢复）----
bool sidebarCollapsed();

// collapsed = 目标态。persist = true 时同时落库；**默认 false = 只改内存**。
// 默认值沿用旧签名（第二参 `path = {}`，空路径 = 不落盘）的语义：启动恢复、
// 纯内存翻转都不该顺手写一次库。要落库的写路径必须显式传 true。
//
// 2026-10-03（批次 E2）：落点从 config.json 改为 SQLite 的 app_config 表，
// 原「configFile 非空即持久化」的路径入参随之改为布尔开关 —— 路径不再是
// 「配置在哪」的决定因素，改由 core::appDatabase() 决定（null = 只改内存）。
void setSidebarCollapsed(bool collapsed, bool persist = false);

// 测试缝（TIN-07）：复位进程级折叠状态回厂（展开）。与 setSidebarCollapsed
// 的区别：纯内存复位、零持久化副作用——resetCaseGlobals 用例间回厂用
void resetSidebarGlobals();

// 当前侧边栏宽度 / 模块内容区宽度（compose 布局与单测共用同一真值）
float sidebarWidth();
float pageContentWidth(float windowWidth);

// 侧边栏自绘（模块导航 + 折叠态图标栏 + 折叠开关按钮）
void drawSidebar(eui::Ui& ui, const eui::Screen& screen);

// 应用根布局 row(侧边栏, 页面内容)：EUI-NEO app main 每帧回调。
//（原实现自 main.cpp 移入，页面区宽度改为随折叠态动态计算）
void compose(eui::Ui& ui, const eui::Screen& screen);

// ---- 应用级 toast 通知（审计 [7]，2026-09-24）----
// 托盘失效入口点击等"不属于任何页面模块"的反馈经此通道显示：调用方
//（main.cpp 托盘动作路由）只写状态，compose 顶层每帧绘制，自动消失后回
// 落到 clearAppNotice。单实例（与 EUI-NEO toast 队列限制一致）。
void showAppNotice(const std::string& title, const std::string& message);
void clearAppNotice();
bool appNoticeVisible();
// 当前通知内容（供测试/调试断言；无通知时为空串）
std::string appNoticeTitle();
std::string appNoticeMessage();

} // namespace app
