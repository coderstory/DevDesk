// ring_probe.cpp — 池168 窗口边框取证探针（2026-11-04 细线黑边版，绿白虚框已撤销）。
// 灰底窗（非黑非白：黑线与底色都可辨）叠 ui_kit::drawWindowRing，进程内断言：
//   DOM：app.ring.base 存在；撤销的绿段 id（top.0/left.0）必须不存在
//   A1 top 边带中线扫描：黑主导（≥80%，连续细线，扣抗锯齿）
//   A1b left 边带中线扫描：黑主导
//   A3 45° 圆角弧点：落黑描边上
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
// 细线黑边：RGB 三通道均暗（灰底 0.45*255≈115 提供对比）
bool isBlackish(const Rgb& c) { return c.r <= 60 && c.g <= 60 && c.b <= 60; }

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
    // 探针窗真实 contentScale（与 A3 采样、FrameCapture::scale 同源同值）：
    // drawWindowRing 与 A3 采样必须同口径传平台真实圆角（mac 16 逻辑pt /
    // Windows 8 物理px 折算），漏传或口径不一 → 弧点落抗锯齿边假红。
    float probeScaleX = 1.0f, probeScaleY = 1.0f;
    glfwGetWindowContentScale(static_cast<GLFWwindow*>(window), &probeScaleX,
                              &probeScaleY);
    const float probeScale = probeScaleX > 0.0f ? probeScaleX : 1.0f;
    // 窗体真实圆角（与宿主 app_sidebar.cpp 同口径）：mac = AppKit 原生 16 逻辑pt
    // （2026-11-07 AppKit 内省实证 [NSWindow valueForKey:@"_cornerRadius"]==16.0；
    // 旧值 10 系像素误测，致描边弧带整体被窗口圆角裁剪、四角黑线全消 bug145），
    // 其他 = DWM 8 物理px 折回逻辑。
#ifdef __APPLE__
    const float probeCornerRadius = 16.0f;
#else
    const float probeCornerRadius = 8.0f / probeScale;
#endif
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
                                                  false,  // 探针窗恒还原态（need184③ 分支见 test_window_ring）
                                                  probeScale,
                                                  probeCornerRadius);
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

    // DOM：细黑边在树；撤销的绿段 id 必须不存在
    if (ui == nullptr || ui->find("app.ring.base") == nullptr) {
        std::cerr << "ring_probe: DOM FAIL app.ring.base 缺失\n";
        passed = false;
    }
    if (ui != nullptr && (ui->find("app.ring.top.0") != nullptr ||
                          ui->find("app.ring.left.0") != nullptr)) {
        std::cerr << "ring_probe: DOM FAIL 绿段 id 残留（绿白虚框已撤销）\n";
        passed = false;
    }

    // A1：top 边带中线扫描黑主导（连续细线，≥80% 扣抗锯齿）
    const float mid = ui_kit::kWindowRingInset +
                      ui_kit::kWindowRingBorder * 0.5f;
    int black = 0, total = 0;
    for (float lx = 14.0f; lx < w - 14.0f; lx += 1.0f) {
        ++total;
        if (isBlackish(sample(lx, mid))) ++black;
    }
    std::cout << "scan top total=" << total << " black=" << black << "\n";
    if (total <= 0 || black * 5 < total * 4) {  // ≥80%
        std::cerr << "ring_probe: A1 FAIL top 黑线覆盖不足 black=" << black
                  << "/" << total << "\n";
        passed = false;
    } else {
        std::cout << "A1 PASS top 边黑线覆盖 " << black << "/" << total
                  << "\n";
    }

    // A1b：left 边带中线（x=mid）纵向扫描黑主导
    int blackL = 0, totalL = 0;
    for (float ly = 14.0f; ly < h - 14.0f; ly += 1.0f) {
        ++totalL;
        if (isBlackish(sample(mid, ly))) ++blackL;
    }
    std::cout << "scan left total=" << totalL << " black=" << blackL << "\n";
    if (totalL <= 0 || blackL * 5 < totalL * 4) {
        std::cerr << "ring_probe: A1b FAIL left 黑线覆盖不足 black=" << blackL
                  << "/" << totalL << "\n";
        passed = false;
    } else {
        std::cout << "A1b PASS left 边黑线覆盖 " << blackL << "/" << totalL
                  << "\n";
    }

    // A3：45° 圆角弧点 = 角圆心向窗角方向移 (r−0.5)·0.7071，应在黑描边上
    // （r 随探针 contentScale 走 DWM 8 物理px 折算——窗口圆角贴合同款口径）
    {
        const float r = ui_kit::windowBorderRadius(probeCornerRadius);
        const float d = (r - 0.5f) * 0.70710678f;
        const float cx = ui_kit::kWindowRingInset + r - d;
        const float cy = ui_kit::kWindowRingInset + r - d;
        const Rgb c = sample(cx, cy);
        std::cout << "corner rgb=" << c.r << "," << c.g << "," << c.b << "\n";
        if (!isBlackish(c)) {
            std::cerr << "ring_probe: A3 FAIL 圆角弧段非黑 (" << c.r << ","
                      << c.g << "," << c.b << ")\n";
            passed = false;
        } else {
            std::cout << "A3 PASS 圆角弧段黑描边\n";
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
