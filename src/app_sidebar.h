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

// 宽度 token：展开 260（spec §6.5.2，不是 200-220）；折叠 72 = 图标 28 + 余量
constexpr float kSidebarWidthExpanded = 260.0f;
constexpr float kSidebarWidthCollapsed = 72.0f;

// ---- 运行时折叠状态（进程内单值；启动由 main 从 config.json 恢复）----
bool sidebarCollapsed();

// collapsed = 目标态。configFile 非空则同时持久化（生产切换传
// core::paths::configFile()，测试传临时文件；空路径 = 只改内存，启动恢复用）
void setSidebarCollapsed(bool collapsed,
                         const std::filesystem::path& configFile = {});

// 当前侧边栏宽度 / 模块内容区宽度（compose 布局与单测共用同一真值）
float sidebarWidth();
float pageContentWidth(float windowWidth);

// 侧边栏自绘（模块导航 + 折叠态图标栏 + hover tooltip + 折叠开关按钮）
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
