// src/ui_kit/splash.h
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19；H 批 2026-10-20 绿色印染；
// UAT6 动态印染）——全屏微绿底(wechat.sidebarBgTinted) + 居中深绿 logo(wechat.
// primaryPressed，AA 对比档) + 底部深绿细进度条 + 开场墨染径向扩散。
// 归属 ui_kit 因需 EUI-NEO 运行时能力，且 app 层与 module_* 可复用；core /
// platform 不得反向依赖。
//
// 时序（#196 裁决 2026-11-07，首帧起算，总长 = 1.2s = kInkDurationSec）：
//   0.0-0.2s  整体淡入（smoothstep，全元素统一 opacity）
//   0.0-0.9s  进度 0→100%（全遮盖期，主界面不画）
//   0.0-1.2s  绿色墨染自 logo 中心径向扩散（全程播完——旧 2.184s 被 1.68s 淡出
//             截断从未播完，见 clue 139）
//   0.9-1.2s  交叉淡化（crossfade）：主界面照画，splash 以递减透明度盖顶
//             （调用方 compose 尾部 drawSplashOverlay），不再「淡出到空底再硬切」
#pragma once

#include <eui/dsl.h>

#include <functional>

namespace ui_kit {

// 墨染动态印染时长（#196 裁决 2026-11-07）：splash 总长 = 墨染时长 = 1.2s。
// 依据（clue 139 实测）：模块 onStart 全部先于 splash 完成期间零加载工作，
// 纯常量延时无存在必要，压到体验所需最小值——墨染完整播完 + 微信式克制节奏。
// constexpr 放头文件供实现与单测共用（test_splash_contract 钉档）。
constexpr float kInkDurationSec = 1.2f;

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

// splash 全遮盖期绘制（0~0.9s）：画 splash 并返回 true（调用方跳过主界面）；
// 交叉淡化期返回 false（主界面照画，盖顶走 drawSplashOverlay）；结束后恒返 false。
// 首帧自动计时，期间每帧 requestUpdate 驱动重绘。
bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen);

// 当前处于交叉淡化窗口期（0.9~1.2s）：主界面帧画完后由调用方调 drawSplashOverlay
// 把 splash 以递减透明度叠顶；窗口期外恒 false（零开销）。
bool splashCrossfadeActive();

// 交叉淡化盖顶绘制：把 splash 全元素以当前透明度叠在主界面之上（z 序最顶）。
// 仅在 splashCrossfadeActive() 窗口期产出元素，其余时刻零元素；每帧 requestUpdate
// 驱动淡化推进。由 app_sidebar compose 尾部调用（drawWindowRing 之后，最顶层）。
void drawSplashOverlay(eui::Ui& ui, const eui::Screen& screen);

// 测试缝：置 true 后 drawSplashIfNeeded 恒返 false，不启动计时/动画/日志/requestUpdate——
// 无头单测与 probe 生命周期远短于 splash 总长，不缝则主界面永不绘制。只有测试基建可调，
// 生产不得触碰。用 header inline（局部 static）而非分线符号，避免 probe 链接拉入
// splash.cpp.o 的依赖。
inline bool& splashDisabledForTestsFlag() {
    static bool flag = false;
    return flag;
}

inline void setSplashDisabledForTests(bool disabled) {
    splashDisabledForTestsFlag() = disabled;
}

// 测试缝（#196 序列帧探针）：非空时 splash 时钟取该覆写返回值（秒），probe 可确定
// 性采样淡入/进度/交叉淡化各相位，落图不依赖真实 1.2s 窗口；生产恒空（steady_clock）。
inline std::function<float()>& splashClockOverrideForTests() {
    static std::function<float()> overrideFn;
    return overrideFn;
}

} // namespace ui_kit
