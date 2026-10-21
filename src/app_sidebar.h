// 常驻侧边栏 + 折叠/展开（无头单测经 core::appRouter() 驱动，不经 main.cpp 的真实装配）
#pragma once

#include <eui/app.h>  // CaptionHitQuery（自绘标题栏命中回调）
#include <eui/dsl.h>

#include <filesystem>
#include <optional>
#include <string>

namespace app {

// 宽度 token：展开 208，折叠 72 = 图标 28 + 余量
constexpr float kSidebarWidthExpanded = 208.0f;
constexpr float kSidebarWidthCollapsed = 72.0f;

// ---- 自绘标题栏（UAT3 D-R3-1，仅本文件实现）----
// 标题条高度（逻辑 px）。仅 customTitlebarActive() 为 true 时参与布局。
float titlebarHeight();
// 自绘标题栏是否参与本进程布局：默认 = 平台能力（Windows 自绘壳 = true，
// macOS 保系统标题栏 = false）；测试经 setCustomTitlebarMode 显式钉两态
//（不钉 = 跟平台走——compose 几何断言必须确定性，不允许按测试运行平台漂移）
bool customTitlebarActive();
// 测试缝：nullopt = 跟平台能力；显式 true/false = 钉住（用例收尾必须复位 nullopt，
// 进程级状态跨用例存活）
void setCustomTitlebarMode(std::optional<bool> mode);
// EUI-NEO 标题栏命中回调的实现（main.cpp dslAppConfig 挂接）：窗口客户区
// 逻辑坐标 → 该点是否拖拽区（标题条带内、右缘窗口钮热区左侧之外）
bool titlebarCaptionHit(const CaptionHitQuery& query);

// 运行时折叠状态（进程内单值，启动由 main 从 app_config 表恢复）
bool sidebarCollapsed();

// collapsed = 目标态。persist=true 时同时落库（app_config 表；core::appDatabase() 为 null 时只改内存），
// 默认 false = 只改内存（启动恢复、纯内存翻转不该顺手写库）
void setSidebarCollapsed(bool collapsed, bool persist = false);

// 测试缝：复位进程级折叠状态回厂（展开），纯内存零持久化
void resetSidebarGlobals();

// ---- 动态背景（UAT6 主窗活体流质）----
// 运行时镜像（配置真值在 SettingsStore 的 fluidBackgroundEnabled）：首帧惰性
// 读库一次并缓存（此后每帧零 DB 查询），设置页拨动经 setFluidBackgroundEnabled
// 写镜像（持久化由设置侧 persist 落库）；库未就绪按默认值，不刷 ERROR 日志。
bool fluidBackgroundEnabled();
void setFluidBackgroundEnabled(bool enabled);
// 相位钟（秒）：只在帧泵 onFrame 里推进——最小化/藏托盘帧不回调即冻结，
// 恢复可见从冻结相位继续（无跳变）。测试/调试读口。
float fluidPhaseSeconds();
// 测试缝：窗可见态覆写（nullopt = 跟平台查询走；显式值 = 钉住不可见/可见，
// 锁「不可见帧不挂泵不推进」铁律）。用例收尾必须复位 nullopt（进程级状态跨
// 用例存活）
void setFluidWindowVisibleMode(std::optional<bool> mode);
// 测试缝：复位动态背景全部进程级状态（镜像/可见态覆写/相位钟），纯内存零持久化
void resetFluidBackgroundForTests();

// 当前侧边栏宽度 / 模块内容区宽度（compose 布局与单测共用同一真值）
float sidebarWidth();
float pageContentWidth(float windowWidth);

// 侧边栏自绘（模块导航 + 折叠态图标栏 + 折叠开关按钮）
void drawSidebar(eui::Ui& ui, const eui::Screen& screen);

// 应用根布局 row(侧边栏, 页面内容)，EUI-NEO app main 每帧回调。
// customTitlebarActive() 时顶层先画自绘标题条（40px），根布局与页面内容整体下移
void compose(eui::Ui& ui, const eui::Screen& screen);

// 应用级 toast 通知：不属于任何页面模块的反馈（如托盘入口）经此通道显示。调用方只写状态，
// compose 顶层每帧绘制，自动消失后 clearAppNotice
void showAppNotice(const std::string& title, const std::string& message);
void clearAppNotice();
bool appNoticeVisible();
// 当前通知内容（测试/调试断言；无通知时为空串）
std::string appNoticeTitle();
std::string appNoticeMessage();

} // namespace app
