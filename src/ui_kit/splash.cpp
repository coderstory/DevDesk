// src/ui_kit/splash.cpp
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19）——
// 全屏微绿底(wechat.sidebarBgTinted #E8F0EC) + 居中深绿 logo(wechat.primaryPressed #059A4C)
// + 底部深绿细进度条（H 批 2026-10-20：用户反馈"底色纯白没有绿色印染"，见下方对比度核）。
// 时序：0.0-2.0s 进度 0→100%；2.0-2.3s 整体 opacity 1→0 淡出；2.3s 后 drawSplashIfNeeded 恒返 false。
//
// D-11 反转：去除极光波 shadertoy 渐变（§8 禁项 #4：大面积渐变背景），改 WeChat-minimal
// 与整改后基调协调；颜色全部走 W1 wechat 令牌，禁硬编码色字面量（W0.5 清单 W-W1~W-W3 + #15-#17）。
//
// H 批对比度核（WCAG 相对亮度）：wechat.primary #07C160(L=0.390) 在微绿底 #E8F0EC(L=0.855)
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

namespace ui_kit {

namespace {

// 时序常量（与 splash.h 注释一致）
constexpr float kDurationSec = 2.0f;  // 进度 0→100%
constexpr float kFadeOutSec = 0.3f;   // 整体淡出

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
        LOG_INFO("启动 splash 开始（WeChat-minimal，2.0s 进度 + 0.3s 淡出）");
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
    const auto& wc = t.colors.wechat;

    // 全屏微绿底（wechat.sidebarBgTinted #E8F0EC）——H 批"绿色印染"：原 wechat.pageBg
    // #F7F7F7 灰被用户看成纯白，换侧栏同款微绿灰令牌，与主界面基调呼应。
    ui.rect("splash.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(wc.sidebarBgTinted)
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

    // logo 图标（图标批 2026-10-20）：C 方案终端窗 >_ 几何 logo（ui_kit::drawDevDeskLogo
    // 矢量绘制）替换 fa-laptop-code 字形（0xF5FC 主接线点退役；about 页遗留一处属
    // 本批红线文件，后续批接 helper）。瓦色 wechat.primaryPressed
    //（#059A4C，H 批对比档：wechat.primary #07C160 在微绿底仅 2.06:1 破图形 AA，深绿
    // 3.16:1 ≥ 3:1——契约测试钉 wechat.primaryPressed 令牌落点，进度条填充同源）；
    // 白窗 cardBg；56px 瓦居中于旧图标区中心 (cx-86, logoY+30)，与「DevDesk」文字
    //（cx-56 起）保持 2px 间距，布局常量零改动。
    {
        constexpr float kSplashLogoSize = 56.0f;
        ui_kit::drawDevDeskLogo(
            ui, "splash.logo.icon",
            cx - 86.0f - kSplashLogoSize * 0.5f,
            logoY + kLogoH * 0.5f - kSplashLogoSize * 0.5f,
            kSplashLogoSize, wc.primaryPressed, wc.cardBg, opacity);
    }
    // logo 文字：wechat.textPrimary（#1A1A1A）；原 W-W1 白色字面量(splash.cpp:131) → 深字（微绿底上 15.0:1）。
    ui.text("splash.logo")
        .position(cx - 56.0f, logoY)
        .size(280.0f, kLogoH)
        .text("DevDesk")
        .fontSize(44.0f)
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(wc.textPrimary)
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
        .color(wc.textSecondary)
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
        .color(wc.cardBg)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();
    ui.rect("splash.progress.fill")
        .position(barX, barY)
        .size(kBarW * progress, kBarH)
        .color(wc.primaryPressed)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();

    // 每帧请求重绘（驱动动画 / 进度 / 淡出）；结束后不再请求。
    app::requestUpdate();
    return true;
}

} // namespace ui_kit
