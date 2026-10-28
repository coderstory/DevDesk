// src/platform/window_visibility.h
// 主窗口可见态查询（平台无关接口）：实现按平台拆文件
// （_win = EnumWindows 捕获 HWND + IsIconic/IsWindowVisible 原生查询；
//  _macos = 当前 GLFW 上下文窗口的 GLFW_ICONIFIED/GLFW_VISIBLE），
// 业务代码（app_sidebar 动态背景门控）零平台宏。
#pragma once

namespace platform::window_visibility {

// 主窗口当前是否可见（既未最小化，也未被隐藏到托盘）。
//
// 线程：仅主线程调用（与 compose/渲染同一线程）。
// 时机：compose 每帧顶层调用一次（app::drawFluidBackground 的可见态门控），
//       不用于启动期/后台线程。
// 语义：true = 可安全渲染；false = 已确认主窗最小化或隐藏。
//
// 失败开放（fail-open）：任何无法判定的情况——平台无查询能力、窗口句柄
// 尚未就绪、无头测试环境、GLFW 未初始化 / SDL2 后端——一律返回 true，
// 绝不因判定失败而阻塞渲染。两平台的零成本兜底仍由 EUI-NEO 主循环的
// hidden/iconified 跳帧保证（不可见时不跑主窗 update/compose/render），
// 本查询是第二道：不可见帧即便 compose 被调用，调用方也不画流质、不挂泵。
//
// 纯值返回、不抛异常、无外部副作用（Windows 首次调用缓存主窗 HWND，
// 此后只读系统状态）。
//
// macOS 实现的入参说明：判定需要主窗口的 GLFWwindow*；本实现用
// glfwGetCurrentContext() 取（EUI-NEO 主循环在 app::update/compose 之前
// 对主窗 makeCurrent，故门控查询时当前上下文即主窗）。拿不到句柄
// （nullptr——glfwInit 前 / 无 GL 上下文 / SDL2 后端）按 true 降级；
// 将来 EUI-NEO 若暴露主窗句柄，只需改 _macos.cpp 的取句柄一行。
bool mainWindowVisible();

} // namespace platform::window_visibility