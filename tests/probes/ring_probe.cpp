// ring_probe.cpp — 池168 窗口绿白间隔圆角虚框 ring 取证探针（dev-b7）。
// 灰底窗（非白非绿：白间隔与绿段都可辨）叠 ui_kit::drawWindowRing，进程内断言：
//   DOM：app.ring.base / app.ring.top.0 / app.ring.left.0 存在
//   A1 四直边绿段覆盖：top/left 边带中线扫描，绿主导（≥50%，理想 70%）
//   A2 白间隔可见：top 边带扫描白像素成段出现（≥10%）
//   A3 圆角弧段：45° 采样点落白底环上（无绿段干扰角弧）
//   --screenshot <png> 落图（GL 帧回读，锁屏期可用）
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 b1_material_probe）
#include <ui_kit/window_ring.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（探针不建应用级 UI）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_ring_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

struct Rgb {
    int r, g, b;
};
// 品牌绿（mint ~#07C160）：g 显著高于 r/b
bool isGreenish(const Rgb& c) { return c.g > c.r + 30 && c.g > c.b + 20; }
bool isWhitish(const Rgb& c) {
    return c.r >= 240 && c.g >= 240 && c.b >= 240;
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("ring_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, "ring", w, h,
                                  [&](eui::Ui& u, const eui::Screen& s) {
                                      ui = &u;
                                      u.stack("ring.root")
                                          .size(s.width, s.height)
                                          .content([&] {
                                              u.rect("ring.bg")
                                                  .position(0.0f, 0.0f)
                                                  .size(s.width, s.height)
                                                  .color(eui::Color{0.45f,
                                                                     0.45f,
                                                                     0.45f,
                                                                     1.0f})
                                                  .build();
                                              ui_kit::drawWindowRing(
                                                  u, s.width, s.height,
                                                  core::theme::current(),
                                                  false);   // 探针窗恒还原态（need184③ 分支见 test_window_ring）
                                          })
                                          .build();
                                  });
    };

    // 稳帧：首帧布局未跟上会假红（lessons 像素采样口径）
    composeFrame();
    for (int i = 0; i < 5; ++i) composeFrame();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "ring_probe: FrameCapture 失败: " << capture.error()
                  << "\nFAIL\n";
        probe::terminateWindowBackend();
        return 4;
    }
    const float scale = capture.scale();
    std::vector<unsigned char> rgba;
    int iw = 0, ih = 0;
    if (!capture.readFrame(runtime, rgba, iw, ih)) {
        std::cerr << "ring_probe: readFrame 失败\nFAIL\n";
        probe::terminateWindowBackend();
        return 5;
    }
    if (!args.screenshotPath.empty() &&
        !probe::writeRgbaToPng(args.screenshotPath, rgba.data(), iw, ih)) {
        std::cerr << "ring_probe: 落图失败: " << args.screenshotPath << "\n";
    }

    auto sample = [&](float lx, float ly) -> Rgb {
        const int px = static_cast<int>(lx * scale);
        const int py = static_cast<int>(ly * scale);
        if (px < 0 || py < 0 || px >= iw || py >= ih) return {-1, -1, -1};
        const std::size_t base =
            (static_cast<std::size_t>(py) * iw + px) * 4u;
        return {rgba[base], rgba[base + 1], rgba[base + 2]};
    };

    bool passed = true;

    // DOM：ring 元素在树
    if (ui == nullptr || ui->find("app.ring.base") == nullptr ||
        ui->find("app.ring.top.0") == nullptr ||
        ui->find("app.ring.left.0") == nullptr) {
        std::cerr << "ring_probe: DOM FAIL ring 元素缺失\n";
        passed = false;
    }

    // A1/A2：top 边带中线（band 2..4 → 中线 y=3）扫描
    const float mid = ui_kit::kWindowRingInset +
                      ui_kit::kWindowRingBorder * 0.5f;
    int green = 0, white = 0, total = 0;
    for (float lx = 14.0f; lx < w - 14.0f; lx += 1.0f) {
        ++total;
        const Rgb c = sample(lx, mid);
        if (isGreenish(c)) ++green;
        else if (isWhitish(c)) ++white;
    }
    std::cout << "scan top total=" << total << " green=" << green
              << " white=" << white << "\n";
    if (total <= 0 || green * 2 < total) {  // ≥50%（理想 70%）
        std::cerr << "ring_probe: A1 FAIL top 绿段覆盖不足 green=" << green
                  << "/" << total << "\n";
        passed = false;
    } else {
        std::cout << "A1 PASS top 边绿段覆盖 " << green << "/" << total
                  << "\n";
    }
    if (white * 10 < total) {  // ≥10%（理想 30% 扣抗锯齿）
        std::cerr << "ring_probe: A2 FAIL top 白间隔不可见 white=" << white
                  << "/" << total << "\n";
        passed = false;
    } else {
        std::cout << "A2 PASS top 边白间隔可见 " << white << "/" << total
                  << "\n";
    }

    // A1b：left 边带中线（x=3）纵向扫描绿主导
    int greenL = 0, totalL = 0;
    for (float ly = 14.0f; ly < h - 14.0f; ly += 1.0f) {
        ++totalL;
        if (isGreenish(sample(mid, ly))) ++greenL;
    }
    std::cout << "scan left total=" << totalL << " green=" << greenL << "\n";
    if (totalL <= 0 || greenL * 2 < totalL) {
        std::cerr << "ring_probe: A1b FAIL left 绿段覆盖不足 green=" << greenL
                  << "/" << totalL << "\n";
        passed = false;
    } else {
        std::cout << "A1b PASS left 边绿段覆盖 " << greenL << "/" << totalL
                  << "\n";
    }

    // A3：45° 圆角弧点 = 角圆心向窗角方向移 (r−0.5)·0.7071，应在白底环上
    {
        const float r = ui_kit::kWindowRingRadius;
        const float d = (r - 0.5f) * 0.70710678f;
        const float cx = ui_kit::kWindowRingInset + r - d;
        const float cy = ui_kit::kWindowRingInset + r - d;
        const Rgb c = sample(cx, cy);
        const bool cornerOk = c.r >= 200 && c.g >= 200 && c.b >= 200;
        std::cout << "corner rgb=" << c.r << "," << c.g << "," << c.b << "\n";
        if (!cornerOk) {
            std::cerr << "ring_probe: A3 FAIL 圆角弧段非白 (" << c.r << ","
                      << c.g << "," << c.b << ")\n";
            passed = false;
        } else {
            std::cout << "A3 PASS 圆角弧段白底环\n";
        }
    }

    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "ring_probe: PASS\n";
    return 0;
}
