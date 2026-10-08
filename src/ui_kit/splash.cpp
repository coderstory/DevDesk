// src/ui_kit/splash.cpp
// 启动 splash 绘制：全屏极光波 shadertoy（assets/shaders/splash.frag，
// Claude Clay #D97757 暖橘配色）+ logo + 副标题 + 假进度条。
//
// shader 方言 = EUI-NEO shadertoy 图元（mainImage(iTime/iResolution)），
// OpenGL 后端只用 fragment source（spirv 路径传空串）；同一份 .frag 在
// macOS / Windows 11 通用，零平台宏。
#include <ui_kit/splash.h>

#include <core/logging.h>
#include <core/theme.h>

#include <eui/dsl_app.h>   // app::requestUpdate（splash 期间每帧驱动重绘）
#include <eui/platform.h>  // eui::platform::resolveResourcePath
#include <eui/types.h>     // eui::ShaderToyGraph / ShaderToyError

#include <algorithm>
#include <chrono>

namespace ui_kit {

namespace {

// 时序常量（与 splash.h 注释一致）
constexpr float kDurationSec = 2.0f;  // 极光波动画 + 进度 0→100%
constexpr float kFadeOutSec = 0.3f;   // 整体淡出

struct SplashState {
    bool active = true;
    bool startSet = false;
    std::chrono::steady_clock::time_point start;
    bool graphLoaded = false;
    eui::ShaderToyGraph graph;
};

SplashState& splashState() {
    static SplashState state;
    return state;
}

} // namespace

bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen) {
    // 测试缝（splash.h）：置位时最短路返回，不触碰 SplashState（不启动
    // 计时/日志/shader）。标志本体是 splash.h 的 inline 函数局部 static
    if (splashDisabledForTestsFlag()) return false;
    SplashState& state = splashState();
    if (!state.active) return false;

    const auto now = std::chrono::steady_clock::now();
    if (!state.startSet) {
        state.startSet = true;
        state.start = now;
        LOG_INFO("启动 splash 开始（极光波，2.0s + 0.3s 淡出）");
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

    // shader graph 首次加载。路径经 resolveResourcePath 解析（exe 旁或 cwd 的
    // assets/ 下），解析不到时回退相对路径——从 build 目录启动即可命中。
    if (!state.graphLoaded) {
        state.graphLoaded = true;
        std::string fragPath =
            eui::platform::resolveResourcePath("assets/shaders/splash.frag");
        if (fragPath.empty()) fragPath = "assets/shaders/splash.frag";
        state.graph.addPass("image", fragPath, "");
    }

    // 全屏极光波（Claude Clay 暖橘系），淡出期随整体 opacity 一起透明
    ui.shadertoy("splash.shader")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .graph(state.graph)
        .resolutionScale(1.0f)
        .timeScale(1.0f)
        .opacity(opacity)
        .onCompileError([](const eui::ShaderToyError& error) {
            LOG_WARN("splash shader 编译失败: " + error.message + " [" +
                     error.sourcePath + ":" + std::to_string(error.line) + "]");
        })
        .build();

    const auto& t = ::core::theme::current();
    const float cx = screen.width * 0.5f;
    const float cy = screen.height * 0.5f;
    constexpr float kLogoH = 60.0f;
    constexpr float kSubH = 26.0f;
    constexpr float kBarW = 240.0f;
    constexpr float kBarH = 5.0f;
    const float logoY = cy - kLogoH - 44.0f;
    const float subY = cy - kSubH - 10.0f;
    const float barY = cy + 90.0f;

    // logo：fa-laptop-code 图标 + "DevDesk"（与侧边栏 brand 同款组合）
    ui.text("splash.logo.icon")
        .position(cx - 108.0f, logoY)
        .size(44.0f, kLogoH)
        .icon(eui::utf8(0xF5FC))
        .fontSize(40.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.primary)
        .opacity(opacity)
        .build();
    ui.text("splash.logo")
        .position(cx - 56.0f, logoY)
        .size(280.0f, kLogoH)
        .text("DevDesk")
        .fontSize(44.0f)
        .fontWeight(700)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(eui::Color{1.0f, 1.0f, 1.0f, 1.0f})  // 白色文字（深蓝底可读）
        .opacity(opacity)
        .build();

    // 副标题
    ui.text("splash.subtitle")
        .position(cx - kBarW, subY)
        .size(kBarW * 2.0f, kSubH)
        .text("开发者桌面工具集")
        .fontSize(17.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(eui::Color{1.0f, 1.0f, 1.0f, 1.0f})  // 白色副标题
        .opacity(opacity)
        .build();

    // 假进度条：0→100% 对齐 2.0s（模块加载在 compose 前已完成，真进度无意义）
    const float barX = cx - kBarW * 0.5f;
    ui.rect("splash.progress.bg")
        .position(barX, barY)
        .size(kBarW, kBarH)
        .color(eui::Color{1.0f, 1.0f, 1.0f, 0.10f})
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();
    ui.rect("splash.progress.fill")
        .position(barX, barY)
        .size(kBarW * progress, kBarH)
        .gradient(eui::Color{0.04f, 0.42f, 1.0f, 1.0f},
                  eui::Color{0.13f, 0.69f, 0.44f, 1.0f},
                  eui::GradientDirection::Horizontal)
        .radius(kBarH * 0.5f)
        .opacity(opacity)
        .build();

    // 每帧请求重绘（驱动动画 / 进度 / 淡出；对应 app_assembly 的 requestUpdate
    // 用法），结束后最后一次返回 false 前不再请求
    app::requestUpdate();
    return true;
}

} // namespace ui_kit