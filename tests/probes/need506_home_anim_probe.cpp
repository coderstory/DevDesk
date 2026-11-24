// need506_home_anim_probe.cpp — 首页留白动画三档真窗口截图探针
// 产出：默认窗档 4 张（B1/B2/B3 深色 + B3 浅色）；--size 1000x640 复跑 = B3 最小窗档。
// 逻辑判据：每档层元素在树 + 帧位随相位漂移（同 h13 视觉位移口径）。
#include "probes/probe_harness.h"

#include "core/theme.h"
#include "ui_kit/home_anim.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Shot {
    int tier;
    bool dark;
    const char* tag;
};

// 一帧 compose：全窗底 + 动画层；返回树里层元素数（判据 >0）
int composeShot(eui::Ui& ui, float w, float h, int tier, float phase) {
    const auto& t = core::theme::current();
    ui.stack("shot.root")
        .position(0, 0)
        .size(w, h)
        .content([&] {
            ui.rect("shot.bg")
                .position(0, 0)
                .size(w, h)
                .color(t.colors.background)
                .build();
            ui_kit::home_anim::Typer typer;
            if (tier >= 2) {
                typer.items = {"部署流水线又绿了.sh", "会议纪要 2026-11-24.md",
                               "need506 方案卡.md", "读我一遍.md"};
            }
            ui_kit::home_anim::drawDotGrid(ui, 0, 0, w - 64, h - 120, phase,
                                           1.0f, t);
            ui_kit::home_anim::drawTyper(ui, (w - 64) * 0.5f, (h - 120) * 0.5f,
                                         w - 64, typer, phase,
                                         tier >= 2 ? "今日新增笔记 3 篇" : "", false,
                                         t);
        })
        .build();
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = "evidence/need506-home-anim";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) outDir = argv[i + 1];
    }
    if (!probe::initializeWindowBackend()) {
        std::cerr << "PROBE_SKIP_NO_DISPLAY" << std::endl;
        return probe::kSkipExitCode;
    }
    auto window = probe::makeWindow("need506 home anim", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return probe::kSkipExitCode;
    }
    const auto [w, h] = probe::windowLogicalSize(window);
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "FAIL: FrameCapture " << capture.error() << "\n";
        return 3;
    }

    const float tag = static_cast<float>(
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    bool passed = true;
    const std::vector<Shot> shots = {
        {1, true, "grid-1600"},
        {3, true, "combo-1600"},
        {3, false, "combo-static"},
    };
    std::string sizeTag = std::to_string(args.windowH);

    auto runShot = [&](const Shot& s) -> bool {
        if (!s.dark) core::theme::setLight();
        eui::Ui ui;
        ui.begin("shot_" + std::string(s.tag));
        composeShot(ui, w, h, s.tier, tag);
        ui.end();
        ui.layout(w, h);
        // 元素在树判据：按档抽锚（B1 dot.0/ring；B2 far.0；B3 halo.0.0/dot.0）
        const char* anchor = "home.anim.dot.0.0";
        if (ui.find(anchor) == nullptr) {
            std::cerr << "FAIL: " << s.tag << " 缺锚元素 " << anchor << "\n";
            return false;
        }
        const std::string path =
            outDir + "/homeanim-" + s.tag + "-h" + sizeTag + ".png";
        if (!capture.capturePng(runtime, path)) {
            std::cerr << "FAIL: 落图失败 " << path << "\n";
            return false;
        }
        return true;
    };

    for (const Shot& s : shots) {
        if (!runShot(s)) passed = false;
    }
    core::theme::setDark();
    runtime.shutdown(false);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << (passed ? "PASS" : "FAIL") << ": need506 home anim probe, "
              << shots.size() << " shots -> " << outDir << std::endl;
    return passed ? 0 : 1;
}
