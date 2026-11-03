// src/terminal_fullscreen_bridge.h
// 终端全屏态 → 侧边栏折叠桥：函数指针缝 + 默认桩（模式同 toast_notice_bridge）。
// module_terminal 只经此桥请求收起/恢复侧边栏，不 include app_sidebar.h；
// 实现 TU 只依赖标准库，tests / probe / live 编译它不产生模块边。
// 真实现由 main.cpp 装配段注入（转发 app_sidebar setSidebarCollapsed(false)）。
#pragma once

namespace app::terminal_fullscreen {

using CollapseHandler = void (*)(bool collapsed);

// 传 nullptr = 恢复默认桩（未装配 = 静默丢弃）。
void setHandler(CollapseHandler handler);
void setCollapsed(bool collapsed);

} // namespace app::terminal_fullscreen
