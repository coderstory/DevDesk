// src/ui_kit/splash.h
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19；H 批 2026-10-20 绿色印染；
// UAT6 动态印染）——全屏微绿底(wechat.sidebarBgTinted) + 居中深绿 logo(wechat.
// primaryPressed，AA 对比档) + 底部深绿细进度条 + 开场 1.2s 绿色墨染径向扩散。
// 归属 ui_kit 因需 EUI-NEO 运行时能力，且 app 层与 module_* 可复用；core /
// platform 不得反向依赖。
//
// 时序（首帧起算）：0.0-1.2s 绿色墨染自 logo 中心径向扩散（UAT6 动态印染，见下）；
// 0.0-2.0s 进度 0→100%；2.0-2.3s 整体淡出；2.3s 后永久关闭。
#pragma once

#include <eui/dsl.h>

namespace ui_kit {

// 墨染动态印染时长（UAT6 G 批：整体放慢 ×1.4，1.2→1.68，各阶段比例保持）：
// 用户档 1~1.5s 取 1.2s→splash 总长 2.0s→2.8s，扩散占比过半
// 会喧宾夺主，先快后缓的曲线把主要动态压在前半程。constexpr 放头文件供实现与单测共用。
constexpr float kInkDurationSec = 1.68f;

// 墨染前沿半径占比（progress 0→1 → 0→1，easeOutCubic 先快后缓）：墨滴先急速铺开
// 再缓收。内联零依赖，无头单测可直接钉曲线（splash.cpp 的"uniform"即此值）。
inline float inkFrontFraction(float progress) {
    const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    const float inv = 1.0f - p;
    return 1.0f - inv * inv * inv;
}

// 墨染浓度包络（progress 0→1 → 1→0，平方稀释）：墨量随扩散摊薄（有机晕染观感），
// progress ≥ 1 归零 → 扩散结束后不画任何墨染元素，终态 = 纯静态底色 sidebarBgTinted
//（与 W 批既有底色断言兼容：动态是叠加层，不动底）。
inline float inkWashAlpha(float progress) {
    const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    const float inv = 1.0f - p;
    return inv * inv;
}

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