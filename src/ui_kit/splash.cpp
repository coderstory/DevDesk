// src/ui_kit/splash.cpp
// 启动 splash：WeChat-minimal（D-11 反转，2026-10-19）——
// 全屏浅灰底(wechat.pageBg #F7F7F7) + 居中 logo + 底部绿色细进度条(wechat.primary #07C160)。
// 时序：0.0-2.0s 进度 0→100%；2.0-2.3s 整体 opacity 1→0 淡出；2.3s 后 drawSplashIfNeeded 恒返 false。
//
// D-11 反转：去除极光波 shadertoy 渐变（§8 禁项 #4：大面积渐变背景），改 WeChat-minimal
// 与整改后基调协调；颜色全部走 W1 wechat 令牌，禁硬编码色字面量（W0.5 清单 W-W1~W-W3 + #15-#17）。
#include <ui_kit/splash.h>

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

    // 全屏浅灰底（wechat.pageBg #F7F7F7）——D-11 反转：替代原极光波 shadertoy 渐变。
    ui.rect("splash.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(wc.pageBg)
        .opacity(opacity)
        .build();

    // 居中 logo：fa-laptop-code 图标 + "DevDesk" 文字
    const float cx = screen.width * 0.5f;
    const float cy = screen.height * 0.5f;
    constexpr float kLogoH = 60.0f;
    constexpr float kSubH = 26.0f;
    constexpr float kBarW = 240.0f;
    constexpr float kBarH = 3.0f;  // 微信克制：细进度条
    const float logoY = cy - kLogoH - 44.0f;
    const float subY = cy - kSubH - 10.0f;
    const float barY = cy + 90.0f;

    // logo 图标：wechat.primary（绿 #07C160）；浅灰底上绿图标达图形 AA 3:1。
    ui.text("splash.logo.icon")
        .position(cx - 108.0f, logoY)
        .size(44.0f, kLogoH)
        .icon(eui::utf8(0xF5FC))
        .fontSize(40.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(wc.primary)
        .opacity(opacity)
        .build();
    // logo 文字：wechat.textPrimary（#1A1A1A）；原 W-W1 白色字面量(splash.cpp:131) → 深字（浅灰底可读）。
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

    // 底部细进度条：wechat.primary 填充 + cardBg 轨道。
    // 原 W-W3 白色 10% 透明轨道(splash.cpp:152) → wechat.cardBg（#FFFFFF，浅灰底上微可见）。
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
        .color(wc.primary)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();

    // 每帧请求重绘（驱动动画 / 进度 / 淡出）；结束后不再请求。
    app::requestUpdate();
    return true;
}

} // namespace ui_kit
