// src/ui_kit/splash.h
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19；H 批 2026-10-20 绿色印染）——
// 全屏微绿底(wechat.sidebarBgTinted) + 居中深绿 logo(wechat.primaryPressed，AA 对比档)
// + 底部深绿细进度条。归属 ui_kit 因需 EUI-NEO 运行时
// 能力，且 app 层与 module_* 可复用；core / platform 不得反向依赖。
//
// 时序（首帧起算）：0.0-2.0s 进度 0→100%；2.0-2.3s 整体淡出；2.3s 后永久关闭。
#pragma once

#include <eui/dsl.h>

namespace ui_kit {

// splash 进行中返回 true（调用方跳过主界面）；首帧自动计时，期间每帧 requestUpdate 驱动重绘。
bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen);

// 测试缝：置 true 后 drawSplashIfNeeded 恒返 false，不启动计时/动画/日志/requestUpdate——
// 无头单测与 probe 生命周期远短于 2.3s，不缝则主界面永不绘制。只有测试基建可调，生产不得触碰。
// 用 header inline（局部 static）而非分线符号，避免 probe 链接拉入 splash.cpp.o 的依赖。
inline bool& splashDisabledForTestsFlag() {
    static bool flag = false;
    return flag;
}

inline void setSplashDisabledForTests(bool disabled) {
    splashDisabledForTestsFlag() = disabled;
}

} // namespace ui_kit