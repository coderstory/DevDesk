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
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// 序列帧直落证据目录（git-ignored，evidence-capture 口径）
static const char* kSplashSeqOutDir =
    "/Users/coderstory/dev/DevDesk/evidence/sidebar-splash-occlusion";

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

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);

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
        const std::string path = std::string(kSplashSeqOutDir) + "/" + name + ".png";
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
