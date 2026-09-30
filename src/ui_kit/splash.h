// src/ui_kit/splash.h
// 启动 splash（presentation 能力层）：极光波 shadertoy 全屏 + logo + 假进度条。
// 归属 ui_kit 的理由同 toast.h：需要 EUI-NEO 运行时能力（shadertoy 图元），
// 且 app 层（app_sidebar compose）与任何 module_* 都可复用；core / platform
// 不得反向依赖本层。
//
// 时序（首帧起算，steady_clock）：
//   0.0-2.0s  极光波动画 + 进度条 0→100%（模块加载在 compose 前已完成，
//             真进度无意义，假进度对齐 2.0s）
//   2.0-2.3s  整体 opacity 淡出
//   2.3s 后   永久关闭，后续帧返回 false，正常主界面
#pragma once

#include <eui/dsl.h>

namespace ui_kit {

// splash 进行中返回 true（调用方 compose 应跳过主界面）；结束返回 false。
// 首帧自动开始计时；splash 期间每帧调用 app::requestUpdate() 驱动重绘。
bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen);

} // namespace ui_kit