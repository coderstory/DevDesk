// splash_transition_probe.cpp — #196 splash 过渡序列帧探针
//
// 运行：splash_transition_probe.exe [--size WxH]（PNG 直落 evidence 目录，见 kOutDir）
// 产物：splash 各时序相位的确定性序列帧（淡入 → 进度+墨染 → 交叉淡化 → 主界面）。
// 确定性来源：ui_kit::splashClockOverrideForTests() 时钟缝（splashDisabledForTests
// 同款测试缝先例）——每帧注入固定 elapsed 再 compose，不赌真实 1.2s 窗口的采样时机。
// 交叉淡化帧下垫主界面替身（深底 + 亮条），验证 splash 递减盖顶、主界面透出。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>

#include <ui_kit/splash.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// 序列帧输出目录：优先 SPLASH_SEQ_OUT 环境变量（evidence-capture 需要归档时显式指定）；
// 否则落当前工作目录下 splash-occlusion-shots/（ctest 工作目录 = build/）。
// 历史硬编码 Mac 绝对证据路径已退役——它会把跑测产物写进仓库受版本控制的 evidence/，
// Windows/CI 也直接失败。
static std::string splashSeqOutDir() {
    if (const char* env = std::getenv("SPLASH_SEQ_OUT"); env != nullptr && *env != '\0') {
        return env;
    }
    return "splash-occlusion-shots";
}

// app:: 层桩（probe 不建应用级 UI；dsl_app_impl 模板引用 dslAppConfig）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_splash_transition_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) {
        std::cerr << "PROBE_SKIP_NO_DISPLAY: window backend init failed" << std::endl;
        return probe::kSkipExitCode;
    }
    auto window = probe::makeWindow("splash_transition_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        std::cerr << "PROBE_SKIP_NO_DISPLAY: window creation failed" << std::endl;
        return probe::kSkipExitCode;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    // 本 probe 专拍 splash：解除断言 initializeWindowBackend 的全局禁用
    //（probe_harness.h:175 对所有 probe 统一置位；不解除则 splash 全程短路，
    // 序列帧全退化成主界面替身——2026-11-07 实证过这个坑）
    ui_kit::setSplashDisabledForTests(false);

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);

    const std::string outDir = splashSeqOutDir();
    {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
    }

    // 采样点：淡入中段(0.05/0.15) / 全遮盖(0.3/0.5/0.7) / 交叉淡化(0.95/1.05/1.15)
    // / 收尾(1.25=主界面)
    const std::vector<std::pair<float, const char*>> samples = {
        {0.05f, "splash-seq-01-fadein-early"},
        {0.15f, "splash-seq-02-fadein-late"},
        {0.30f, "splash-seq-03-hold-progress"},
        {0.50f, "splash-seq-04-hold-ink"},
        {0.70f, "splash-seq-05-hold-late"},
        {0.95f, "splash-seq-06-crossfade-early"},
        {1.05f, "splash-seq-07-crossfade-mid"},
        {1.15f, "splash-seq-08-crossfade-late"},
        {1.25f, "splash-seq-09-main-ui"},
    };

    bool passed = true;
    for (const auto& [t, name] : samples) {
        ui_kit::splashClockOverrideForTests() = [t] { return t; };
        probe::composeModuleFrame(
            runtime, window, "splash_transition", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                if (ui_kit::drawSplashIfNeeded(u, screen)) {
                    return;  // 全遮盖期（0~0.9s）
                }
                // 主界面替身（crossfade 透出对照物）：深底 + 中部亮条
                u.rect("probe.main.standin")
                    .position(0.0f, 0.0f)
                    .size(screen.width, screen.height)
                    .color(eui::Color(0.13f, 0.15f, 0.17f, 1.0f))
                    .build();
                u.rect("probe.main.standin.bar")
                    .position(screen.width * 0.2f, screen.height * 0.45f)
                    .size(screen.width * 0.6f, screen.height * 0.1f)
                    .color(eui::Color(0.30f, 0.55f, 0.42f, 1.0f))
                    .build();
                if (ui_kit::splashCrossfadeActive()) {
                    ui_kit::drawSplashOverlay(u, screen);  // 盖顶递减
                }
            });
        // retained 层稳定门槛（bug185）：≥16 遍 render 再取后备缓冲
        probe::FrameCapture capture(window);
        std::vector<unsigned char> rgba;
        int iw = 0, ih = 0;
        if (!capture.ok() || !capture.readFrame(runtime, rgba, iw, ih, 16)) {
            std::cerr << "FAIL: 帧捕获失败 t=" << t << std::endl;
            passed = false;
            break;
        }
        const std::string path = outDir + "/" + name + ".png";
        if (!probe::writeRgbaToPng(path, rgba.data(), iw, ih)) {
            std::cerr << "FAIL: PNG 落盘失败 " << path << std::endl;
            passed = false;
            break;
        }
        std::cout << "splash-seq: t=" << t << "s -> " << path << std::endl;
    }

    ui_kit::splashClockOverrideForTests() = nullptr;
    ui_kit::setSplashDisabledForTests(true);
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) return 1;
    std::cout << "PASS: splash 过渡序列帧 " << samples.size() << " 张" << std::endl;
    return 0;
}
