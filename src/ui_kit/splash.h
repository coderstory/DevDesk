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

// 测试缝（2026-09-30）：置 true 后 drawSplashIfNeeded 恒返 false，且不启动
// 计时/动画、不加载 shader、不写日志、不 requestUpdate——无头单测与 probe
// 进程生命周期远短于 splash 2.3s，不缝则 compose 期间 splash 恒真、主界面
// 永不绘制（sidebar 几何用例全红的回归）。只有测试基建（test_main.cpp
// resetCaseGlobals / probe_harness.h）允许调用；生产 main.cpp 不得触碰。
//
// 实现为 header inline（函数局部 static）而不是 splash.cpp 静态标志：setter
// 若是 ui_kit 的出线符号，probe 链接会拉入 splash.cpp.o，其 app::requestUpdate
// 依赖在 probe 侧无解（实现 TU glfw_app_main 只编进主程序；dsl_app_impl 注入
// 无法统一用于全部 probe——about/todo probe 已自带注入，home/claude/markdown
// probe 注入会缺 app::compose 定义）。inline 化后 probe 链接零新增依赖
inline bool& splashDisabledForTestsFlag() {
    static bool flag = false;
    return flag;
}

inline void setSplashDisabledForTests(bool disabled) {
    splashDisabledForTestsFlag() = disabled;
}

} // namespace ui_kit