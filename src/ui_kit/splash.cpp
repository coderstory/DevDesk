// src/ui_kit/splash.cpp
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19）——
// 全屏微绿底(sidebarBg #F0FDF7) + 居中深绿 logo(wechat.primaryPressed #059A4C)
// + 底部深绿细进度条（H 批 2026-10-20：用户反馈"底色纯白没有绿色印染"，见下方对比度核）。
// UAT6 动态印染（用户："splash 没有绿色印染的动态效果"——静态绿底不够）：开场 1.2s
// 绿色自 logo 瓦中心径向扩散/晕染，边缘带确定性噪声（有机晕染而非硬圆），扩散完
// 收敛为纯静态底色（终态与 H 批一致，W 批底色断言不动）。实现为纯 DSL 同心圆 +
// 噪声卫星墨点——契约（test_splash_contract）禁回归 shadertoy 渐变与 fragment
// shader 资产引用（D-11 用户拍板反转），"shader uniform" 的等效物 = inkProgress
//（elapsed 归一化）驱动 inkFrontFraction / inkWashAlpha（曲线在 splash.h 内联，
// 无头单测可钉）。
// 时序：0.0-1.2s 墨染扩散；0.0-2.0s 进度 0→100%；2.0-2.3s 整体 opacity 1→0 淡出；
// 2.3s 后 drawSplashIfNeeded 恒返 false。
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
// UAT6 G 批：整体放慢 ×1.4，各阶段比例保持（2.0→2.8, 0.3→0.42）
constexpr float kDurationSec = 2.8f;  // 进度 0→100%（原 2.0 ×1.4）
constexpr float kFadeOutSec = 0.42f;  // 整体淡出（原 0.3 ×1.4）

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

struct SplashState {
    bool active = true;
    bool startSet = false;
    std::chrono::steady_clock::time_point start;
};

SplashState& splashState() {
    static SplashState state;
    return state;
}

} // namespace

bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen) {
    // 测试缝：置位时最短路返回，不触碰 SplashState（标志本体在 splash.h）。
    if (splashDisabledForTestsFlag()) return false;
    SplashState& state = splashState();
    if (!state.active) return false;

    const auto now = std::chrono::steady_clock::now();
    if (!state.startSet) {
        state.startSet = true;
        state.start = now;
        LOG_INFO("启动 splash 开始（WeChat-minimal，1.2s 墨染扩散 + 2.0s 进度 + 0.3s 淡出）");
    }
    const float elapsed =
        std::chrono::duration<float>(now - state.start).count();
    if (elapsed >= kDurationSec + kFadeOutSec) {
        state.active = false;
        LOG_INFO("启动 splash 结束，切主界面");
        return false;
    }

    const float progress = std::clamp(elapsed / kDurationSec, 0.0f, 1.0f);
    float opacity = 1.0f;
    if (elapsed > kDurationSec) {
        opacity = 1.0f - (elapsed - kDurationSec) / kFadeOutSec;
    }

    const auto& t = ::core::theme::current();

    // 全屏微绿底（sidebarBg，light #F0FDF7 / dark #0F1F18），与侧栏同源，
    // 与主界面基调呼应。
    ui.rect("splash.bg")
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

    // —— 墨染动态印染（UAT6）：绿色自 logo 瓦中心径向扩散/晕染，1.2s 后收敛 ——
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
        // 的 app::requestUpdate（本函数尾部每帧调）双保险。收敛后不再挂载即被剪除
        ui.stack("splash.ink.anim.driver")
            .size(1.0f, 1.0f)
            .onFrame([](float) {})
            .build();
    }

    // logo 图标（图标批 2026-10-20）：C 方案终端窗 >_ 几何 logo（ui_kit::drawDevDeskLogo
    // 矢量绘制）替换 fa-laptop-code 字形（0xF5FC 主接线点退役；about 页遗留一处属
    // 本批红线文件，后续批接 helper）。瓦色 wechat.primaryPressed
    //（#059A4C，H 批对比档：wechat.primary #36D399 在微绿底仅 2.06:1 破图形 AA，深绿
    // 3.16:1 ≥ 3:1——契约测试钉 wechat.primaryPressed 令牌落点，进度条填充同源）；
    // 白窗 cardBg；56px 瓦居中于旧图标区中心 (cx-86, logoY+30)，与「DevDesk」文字
    //（cx-56 起）保持 2px 间距，布局常量零改动。
    {
        constexpr float kSplashLogoSize = 56.0f;
        ui_kit::drawDevDeskLogo(
            ui, "splash.logo.icon",
            cx - 86.0f - kSplashLogoSize * 0.5f,
            logoY + kLogoH * 0.5f - kSplashLogoSize * 0.5f,
            kSplashLogoSize, t.colors.primaryPressed, t.colors.surface, opacity);
    }
    // logo 文字：wechat.textPrimary（#1A1A1A）；原 W-W1 白色字面量(splash.cpp:131) → 深字（微绿底上 15.0:1）。
    ui.text("splash.logo")
        .position(cx - 56.0f, logoY)
        .size(280.0f, kLogoH)
        .text("DevDesk")
        .fontSize(44.0f)
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .opacity(opacity)
        .build();

    // 副标题：wechat.textSecondary（#666666）；原 W-W2 白色字面量(splash.cpp:143) → 二级灰。
    ui.text("splash.subtitle")
        .position(cx - kBarW, subY)
        .size(kBarW * 2.0f, kSubH)
        .text("开发者桌面工具集")
        .fontSize(17.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .opacity(opacity)
        .build();

    // 底部细进度条：wechat.primaryPressed 填充 + wechat.cardBg 轨道。
    // H 批：填充同步加深为 primaryPressed（对微绿底 3.16:1 ≥ AA；原 primary 仅 2.06:1）。
    // 轨道保持 wechat.cardBg（#FFFFFF）：白轨对微绿底 #E8F0EC 为 1.16:1，较原灰底 #F7F7F7
    // 上的 1.07:1 可见性不降反升（dividerPrimary #E5E5E5 在新底上仅 1.09:1 反而更弱，故不换）。
    // 高度 3px（微信克制）；进度 0→100% over 2.0s（模块加载在 compose 前已完成，真进度无意义）。
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
        .size(kBarW * progress, kBarH)
        .color(t.colors.primaryPressed)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();

    // 每帧请求重绘（驱动动画 / 进度 / 淡出）；结束后不再请求。
    app::requestUpdate();
    return true;
}

} // namespace ui_kit
