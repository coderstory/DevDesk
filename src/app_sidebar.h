// src/app_sidebar.h
// 常驻侧边栏 + 折叠/展开。从 main.cpp 拆出以便无头单测直接驱动 compose/drawSidebar——
// main.cpp 的 getRouter() 会拉起全部真实模块，测试不可用；这里改走 core::appRouter() 注入。
#pragma once

#include <eui/dsl.h>

#include <filesystem>
#include <string>

namespace app {

// 宽度 token：展开 208，折叠 72 = 图标 28 + 余量
constexpr float kSidebarWidthExpanded = 208.0f;
constexpr float kSidebarWidthCollapsed = 72.0f;

// 运行时折叠状态（进程内单值，启动由 main 从 app_config 表恢复）
bool sidebarCollapsed();

// collapsed = 目标态。persist=true 时同时落库，默认 false = 只改内存（启动恢复、纯内存翻转
// 都不该顺手写库）。落点 = SQLite 的 app_config 表，core::appDatabase() 为 null 时只改内存
void setSidebarCollapsed(bool collapsed, bool persist = false);

// 测试缝：复位进程级折叠状态回厂（展开），纯内存零持久化
void resetSidebarGlobals();

// 当前侧边栏宽度 / 模块内容区宽度（compose 布局与单测共用同一真值）
float sidebarWidth();
float pageContentWidth(float windowWidth);

// 侧边栏自绘（模块导航 + 折叠态图标栏 + 折叠开关按钮）
void drawSidebar(eui::Ui& ui, const eui::Screen& screen);

// 应用根布局 row(侧边栏, 页面内容)，EUI-NEO app main 每帧回调
void compose(eui::Ui& ui, const eui::Screen& screen);

// 应用级 toast 通知：不属于任何页面模块的反馈（如托盘入口）经此通道显示。调用方只写状态，
// compose 顶层每帧绘制，自动消失后 clearAppNotice。单实例
void showAppNotice(const std::string& title, const std::string& message);
void clearAppNotice();
bool appNoticeVisible();
// 当前通知内容（测试/调试断言；无通知时为空串）
std::string appNoticeTitle();
std::string appNoticeMessage();

} // namespace app
