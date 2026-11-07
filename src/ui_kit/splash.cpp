// src/ui_kit/splash.cpp
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19）——
// 全屏微绿底(sidebarBg #F0FDF7) + 居中深绿 logo(wechat.primaryPressed #059A4C)
// + 底部深绿细进度条（H 批 2026-10-20：用户反馈"底色纯白没有绿色印染"，见下方对比度核）。
// UAT6 动态印染（用户："splash 没有绿色印染的动态效果"——静态绿底不够）：开场
// 绿色自 logo 瓦中心径向扩散/晕染，边缘带确定性噪声（有机晕染而非硬圆），扩散完
// 收敛为纯静态底色（终态与 H 批一致，W 批底色断言不动）。实现为纯 DSL 同心圆 +
// 噪声卫星墨点——契约（test_splash_contract）禁回归 shadertoy 渐变与 fragment
// shader 资产引用（D-11 用户拍板反转），"shader uniform" 的等效物 = inkProgress
//（elapsed 归一化）驱动 inkFrontFraction / inkWashAlpha（曲线在 splash.h 内联，
// 无头单测可钉）。
//
// 时序（#196 裁决 2026-11-07：总长 1.2s = kInkDurationSec，墨染全程播完；
// 旧 1.93s 常量延时退役——clue 139 实测模块 onStart 在 splash 前已全部完成，
// 期间零加载工作，且旧墨染 2.184s 被 1.68s 淡出截断从未播完、日志文案与实际
// 不符）：
//   0.0-0.2s  淡入（smooth01，全元素统一 opacity）
//   0.0-0.9s  进度 0→100%（kDurationSec，全遮盖期，主界面不画）
//   0.0-1.2s  墨染扩散（kInkDurationSec）
//   0.9-1.2s  交叉淡化（kFadeOutSec）：主界面照画、splash 盖顶 opacity 1→0
//             （smooth01 曲线），交叉淡化的盖顶元素以 splash.overlay.* 命名域
//             与全遮盖期元素区分（frame dump 可辨）。
//
// D-11 反转：去除极光波 shadertoy 渐变（§8 禁项 #4：大面积渐变背景），改 WeChat-minimal
// 与整改后基调协调；颜色全部走 W1 wechat 令牌，禁硬编码色字面量（W0.5 清单 W-W1~W-W3 + #15-#17）。
//
// H 批对比度核（WCAG 相对亮度）：wechat.primary #36D399(L=0.390) 在微绿底 #E8F0EC(L=0.855)
// 上仅 2.06:1，纯白底上限也只 2.39:1——任何浅底都到不了图形 AA 3:1（"更浅绿底"方案数学
// 不可达），故取方案 b 变体：图标与进度条加深为同族令牌 wechat.primaryPressed
// #059A4C(L=0.237)，在 #E8F0EC 上 3.16:1 ≥ AA 3:1（D-12 令牌族内取值，未引入新色值）。
#include <ui_kit/splash.h>

#include <ui_kit/logo.h>  // 图标批 2026-10-20：居中 logo 换几何绘制（C 方案终端窗 >_）

#include <core/logging.h>
#include <core/theme.h>

#include <eui/dsl_app.h>   // app::requestUpdate（splash 期间每帧驱动重绘）

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace ui_kit {

namespace {

// 时序常量（与 splash.h 注释一致；墨染时长 kInkDurationSec 在 splash.h 供单测共用）
// #196 裁决：kDurationSec + kFadeOutSec = kInkDurationSec（进度先完、墨染收尾）
constexpr float kFadeInSec = 0.2f;       // 开场淡入（smooth01）
constexpr float kDurationSec = 0.9f;     // 进度 0→100%（全遮盖期）
constexpr float kFadeOutSec = 0.3f;      // 交叉淡化（与主界面 crossfade）

// 墨染视觉常量：内层最浓峰值 / 同心圆层数 / 噪声卫星数（只撒外两环）
constexpr float kInkMaxAlpha = 0.35f;
constexpr int kInkRings = 4;
constexpr int kInkBlobsPerRing = 8;

// 确定性 0~1 hash（sin 散列取小数）：噪声卫星的角度/凸起/大小全由索引 hash 决定
//——帧间稳定不闪烁，只随前沿半径一起生长（把硬圆边打散成有机晕染边）
float hash01(int n) {
    const float x = std::sin(static_cast<float>(n) * 127.1f) * 43758.5453f;
    return x - std::floor(x);
}

// smoothstep 0→1：淡入/淡出曲线统一用它（线性淡出在低不透明度段观感生硬）
float smooth01(float x) {
    const float p = std::clamp(x, 0.0f, 1.0f);
    return p * p * (3.0f - 2.0f * p);
}

struct SplashState {
    bool active = true;
    bool startSet = false;
    std::chrono::steady_clock::time_point start;
};

SplashState& splashState() {
    static SplashState state;
    return state;
}

// 取 splash 已历秒数；首帧顺带置起点并打开始日志（文案与实际时序对齐，#196）。
// 测试缝：splashClockOverrideForTests 非空时直接取覆写值（probe 确定性采样）。
float splashElapsedNow(SplashState& state) {
    if (const std::function<float()>& overrideFn = splashClockOverrideForTests();
        overrideFn) {
        return overrideFn();
    }
    const auto now = std::chrono::steady_clock::now();
    if (!state.startSet) {
        state.startSet = true;
        state.start = now;
        LOG_INFO("启动 splash 开始（1.2s：0.2s 淡入 + 0.9s 进度 + 0.3s 交叉淡化，墨染全程播完）");
    }
    return std::chrono::duration<float>(now - state.start).count();
}

// 收尾（恰一次）：关激活态 + 结束日志带实测时长（与开始日志同源，杜绝文案漂移）
void closeSplash(SplashState& state, float elapsed) {
    state.active = false;
    LOG_INFO("启动 splash 结束（实测 " + std::to_string(elapsed) + "s），切主界面");
}

// 阶段透明度：淡入 smooth01 上坡 → 全遮盖期 1 → 交叉淡化 smooth01 下坡到 0
float splashOpacity(float elapsed) {
    if (elapsed <= kFadeInSec) return smooth01(elapsed / kFadeInSec);
    if (elapsed <= kDurationSec) return 1.0f;
    return smooth01(1.0f - (elapsed - kDurationSec) / kFadeOutSec);
}

// splash 全元素树（全遮盖期与交叉淡化盖顶共用一份；overlayPhase 只换 bg 元素
// 命名域供 frame dump 辨认阶段）。opacity 驱动全部元素的淡入/淡出。
void drawSplashTree(eui::Ui& ui, const eui::Screen& screen, float elapsed,
                    float opacity, bool overlayPhase) {
    const auto& t = ::core::theme::current();

    // 全屏微绿底（sidebarBg，light #F0FDF7 / dark #0F1F18），与侧栏同源，
    // 与主界面基调呼应。交叉淡化盖顶期以 splash.overlay.* 命名域落树。
    ui.rect(overlayPhase ? "splash.overlay.bg" : "splash.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(t.colors.sidebarBg)
        .opacity(opacity)
        .build();

    // 居中 logo：几何图标（C 方案终端窗 >_）+ "DevDesk" 文字
    const float cx = screen.width * 0.5f;
    const float cy = screen.height * 0.5f;
    constexpr float kLogoH = 60.0f;
    constexpr float kSubH = 26.0f;
    constexpr float kBarW = 240.0f;
    constexpr float kBarH = 3.0f;  // 微信克制：细进度条
    const float logoY = cy - kLogoH - 44.0f;
    const float subY = cy - kSubH - 10.0f;
    const float barY = cy + 90.0f;

    // —— 墨染动态印染（UAT6）：绿色自 logo 瓦中心径向扩散/晕染，1.2s 播满 ——
    // 进度源 = splash 自身 steady_clock 时间轴的归一化（等效 shader uniform）：
    // inkFrontFraction 定前沿半径、inkWashAlpha 定浓度（扩散摊薄），曲线在 splash.h。
    // 画在底色与 logo 之间（z 序 bg → 墨染 → logo），logo 保持清晰不被洗染。
    // 扩散结束（inkProgress >= 1）本块整体不再执行 → 不画任何墨染元素、不再挂
    // 帧泵，终态 = 纯静态 sidebarBg 底（W 批底色断言恒成立）。
    const float inkProgress = std::clamp(elapsed / kInkDurationSec, 0.0f, 1.0f);
    if (inkProgress < 1.0f) {
        const float inkX = cx - 86.0f;  // logo 瓦几何中心（56px 瓦挂载中心，见下方图标块）
        const float inkY = logoY + 30.0f;
        const float farX = std::max(inkX, screen.width - inkX);
        const float farY = std::max(inkY, screen.height - inkY);
        const float maxDist = std::sqrt(farX * farX + farY * farY) + 1.0f;
        const float front = maxDist * inkFrontFraction(inkProgress);
        const float envelope = inkWashAlpha(inkProgress);
        for (int j = 0; j < kInkRings; ++j) {
            const float frac = 1.0f - static_cast<float>(j) / kInkRings;  // 1.0→0.25 内密外疏
            const float r = front * frac;
            if (r < 1.0f) continue;  // 首帧半径≈0，不建空元素
            const float ringAlpha = kInkMaxAlpha * envelope * frac;
            ui.rect("splash.ink.ring." + std::to_string(j))
                .position(inkX - r, inkY - r)
                .size(r * 2.0f, r * 2.0f)
                .color(t.colors.primaryPressed)
                .radius(r)
                .opacity(ringAlpha * opacity)
                .build();
            // 噪声卫星（只撒外两环）：前沿圈上按确定性 hash 撒墨点，凸起 0.82~1.20
            // 倍半径参差——硬圆边打散成有机晕染；hash 只吃索引，帧间稳定不闪
            if (j <= 1) {
                for (int b = 0; b < kInkBlobsPerRing; ++b) {
                    const float ang = hash01(j * 17 + b * 3 + 1) * 6.2831853f;
                    const float lead = 0.82f + 0.38f * hash01(j * 31 + b * 7 + 2);
                    const float br = r * (0.10f + 0.08f * hash01(j * 13 + b * 5 + 3));
                    ui.rect("splash.ink.blob." + std::to_string(j) + "." +
                            std::to_string(b))
                        .position(inkX + std::cos(ang) * r * lead - br,
                                  inkY + std::sin(ang) * r * lead - br)
                        .size(br * 2.0f, br * 2.0f)
                        .color(t.colors.primaryPressed)
                        .radius(br)
                        .opacity(ringAlpha * 0.6f * opacity)
                        .build();
                }
            }
        }
        // 帧泵（照 md.tree.anim.driver 折叠动画先例）：墨染在途挂 1x1 空 onFrame
        // 元素，runtime 每帧回调它并置 animating → 主循环持续出帧推进扩散；与既有
        // 的 app::requestUpdate（本函数调用方每帧调）双保险。收敛后不再挂载即被剪除
        ui.stack("splash.ink.anim.driver")
            .size(1.0f, 1.0f)
            .onFrame([](float) {})
            .build();
    }

    // logo 图标（图标批 2026-10-20）：C 方案终端窗 >_ 几何 logo（ui_kit::drawDevDeskLogo
    // 矢量绘制）。瓦色 wechat.primaryPressed（#059A4C，H 批对比档）；白窗 cardBg；
    // 56px 瓦居中于旧图标区中心 (cx-86, logoY+30)。
    {
        constexpr float kSplashLogoSize = 56.0f;
        ui_kit::drawDevDeskLogo(
            ui, "splash.logo.icon",
            cx - 86.0f - kSplashLogoSize * 0.5f,
            logoY + kLogoH * 0.5f - kSplashLogoSize * 0.5f,
            kSplashLogoSize, t.colors.primaryPressed, t.colors.surface, opacity);
    }
    // logo 文字：wechat.textPrimary（#1A1A1A）；微绿底上 15.0:1。
    ui.text("splash.logo")
        .position(cx - 56.0f, logoY)
        .size(280.0f, kLogoH)
        .text("DevDesk")
        .fontSize(core::theme::scaled(44.0f))
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .opacity(opacity)
        .build();

    // 副标题：wechat.textSecondary（#666666）。
    ui.text("splash.subtitle")
        .position(cx - kBarW, subY)
        .size(kBarW * 2.0f, kSubH)
        .text("开发者桌面工具集")
        .fontSize(core::theme::scaled(17.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .opacity(opacity)
        .build();

    // 底部细进度条：wechat.primaryPressed 填充 + surface 轨道（H 批 AA 对比档口径
    // 不变）；进度 0→100% over kDurationSec（模块加载在 compose 前已完成，
    // 真进度无意义——clue 139）。
    const float barX = cx - kBarW * 0.5f;
    ui.rect("splash.progress.bg")
        .position(barX, barY)
        .size(kBarW, kBarH)
        .color(t.colors.surface)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();
    ui.rect("splash.progress.fill")
        .position(barX, barY)
        .size(kBarW * std::clamp(elapsed / kDurationSec, 0.0f, 1.0f), kBarH)
        .color(t.colors.primaryPressed)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();
}

} // namespace

bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen) {
    // 测试缝：置位时最短路返回，不触碰 SplashState（标志本体在 splash.h）。
    if (splashDisabledForTestsFlag()) return false;
    SplashState& state = splashState();
    if (!state.active) return false;

    const float elapsed = splashElapsedNow(state);
    if (elapsed >= kDurationSec + kFadeOutSec) {
        closeSplash(state, elapsed);
        return false;
    }
    // 交叉淡化期（0.9~1.2s）：主界面照画，splash 由 compose 尾部 drawSplashOverlay
    // 盖顶（z 序最顶）；本函数此期不产元素、返回 false 放行主界面。
    if (elapsed >= kDurationSec) return false;

    drawSplashTree(ui, screen, elapsed, splashOpacity(elapsed), false);
    // 每帧请求重绘（驱动淡入 / 进度 / 墨染）；结束后不再请求。
    app::requestUpdate();
    return true;
}

bool splashCrossfadeActive() {
    if (splashDisabledForTestsFlag()) return false;
    SplashState& state = splashState();
    if (!state.active) return false;
    const float elapsed = splashElapsedNow(state);
    if (elapsed >= kDurationSec + kFadeOutSec) {
        closeSplash(state, elapsed);
        return false;
    }
    return elapsed >= kDurationSec;
}

void drawSplashOverlay(eui::Ui& ui, const eui::Screen& screen) {
    if (!splashCrossfadeActive()) return;  // 窗口期外零元素零开销
    SplashState& state = splashState();
    const float elapsed = splashElapsedNow(state);
    if (elapsed >= kDurationSec + kFadeOutSec) {
        closeSplash(state, elapsed);
        return;
    }
    drawSplashTree(ui, screen, elapsed, splashOpacity(elapsed), true);
    // 交叉淡化期每帧请求重绘（主界面动画可能静默，淡化推进要自驱动）。
    app::requestUpdate();
}

} // namespace ui_kit
