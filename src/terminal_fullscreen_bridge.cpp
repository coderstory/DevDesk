// src/terminal_fullscreen_bridge.cpp — 终端全屏→侧栏折叠桥实现缝（toast 模式）。
#include "terminal_fullscreen_bridge.h"

namespace app::terminal_fullscreen {

namespace {
CollapseHandler g_collapse = nullptr;
}  // namespace

void setHandler(CollapseHandler handler) { g_collapse = handler; }

void setCollapsed(bool collapsed) {
    if (g_collapse == nullptr) return;  // 默认桩：未装配 = 静默丢弃
    g_collapse(collapsed);
}

} // namespace app::terminal_fullscreen
