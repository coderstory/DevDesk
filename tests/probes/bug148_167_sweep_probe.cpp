// bug148_167_sweep_probe.cpp — bug148/167 复现探针：字号缩放拖动扫档 + 侧栏选中态
//
// 复现路径（老板报单 2026-11-06 #148/#167）：设置页拖动「字号缩放」滑块扫过
// 大量中间值 → 文本全消失（#148）；侧栏选中态文本异常（#167）。
// 拖动语义 = 同一进程内连续 setFontScale 大量中间值（EUI 字体栈缓存 16 槽
// fontSize key 打满/淘汰），单值档位探针（uat62 --font-scale）复现不了缓存压力。
//
// 流程：settings 页上扫 0.50→2.00 步进 0.01（151 档 ×2 帧）→ 落 1.23 →
// home/memory 拍图；home 在扫档前先拍一张同档位对照（pre/post sweep 归因）。
// 断言 = 两个墨迹占比：页标题区（纯文本区，文本消失≈0）+ 侧栏导航区。
// UAT62_DARK=1 → 暗主题（与 uat62 同名约定，暗色选中态对照）。
//
// 链接面与 uat62_page_shots_probe 同组（真 app::compose + app_assembly）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>
#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"
#include "module_toggle_bridge.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_settings/settings_config.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace app {
namespace {
const bool g_sweepDark = std::getenv("UAT62_DARK") != nullptr;
}  // namespace
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_bug148_167_sweep_probe");
    return config;
}
}  // namespace app

namespace {

constexpr int kCaptureRenderPasses = 17;  // bug185 retained layer 稳帧门槛

// 墨迹占比：区域内与背景参考点（区域左上角内侧）亮度差 >24 的像素占比。
// 坐标用帧比例（0..1），分辨率无关。文本消失 → 占比≈0。
double inkRatio(const std::vector<unsigned char>& rgba, int pw, int ph,
                double fx0, double fy0, double fx1, double fy1) {
    int x0 = static_cast<int>(fx0 * pw);
    int y0 = static_cast<int>(fy0 * ph);
    int x1 = static_cast<int>(fx1 * pw);
    int y1 = static_cast<int>(fy1 * ph);
    x0 = std::clamp(x0, 0, pw - 1);
    y0 = std::clamp(y0, 0, ph - 1);
    x1 = std::clamp(x1, x0 + 1, pw);
    y1 = std::clamp(y1, y0 + 1, ph);
    const auto lum = [&](int x, int y) {
        const unsigned char* px = rgba.data() +
            (static_cast<std::size_t>(y) * pw + x) * 4u;
        return 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
    };
    const double bg = lum(x0 + 2, y0 + 2);
    std::size_t ink = 0;
    std::size_t total = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            ++total;
            if (std::fabs(lum(x, y) - bg) > 24.0) ++ink;
        }
    }
    return total > 0 ? static_cast<double>(ink) / total : 0.0;
}

struct Shot {
    std::string name;
    double titleInk = 0.0;
    double sidebarInk = 0.0;
    bool ok = false;
};

// 拍一页：导航 → 稳帧（≥24 帧且动画归零）→ readFrame → 写 PNG + 墨迹断言。
// 标题区 = 内容区左上纯文本带（x 0.22..0.40, y 0.05..0.11）；侧栏区含导航文本
// 与选中态（x 0.01..0.18, y 0.18..0.92）。阈值：标题 0.3%、侧栏 1.0%——
// 文本全消失时标题区仅剩底色 ≈0%。
bool capturePage(core::Router& router, core::dsl::Runtime& runtime,
                 core::window::Handle window, probe::FrameCapture& capture,
                 ::core::PageId page, const std::string& slug,
                 const std::string& outDir, int logicalW, Shot& shot) {
    // 每次拍页前重申主题：启动链可能在 startAll 之后异步恢复配置主题覆盖探针档
    if (app::g_sweepDark) core::theme::setDark();
    else core::theme::setLight();
    router.navigateTo(page);
    for (int f = 0; f < 240; ++f) {
        probe::composeModuleFrame(runtime, window, "sweep", logicalW, 1000,
                                  [&](eui::Ui& ui, const eui::Screen& screen) {
                                      app::compose(ui, screen);
                                  });
        if (f + 1 >= 24 && !runtime.isAnimating()) break;
    }
    std::vector<unsigned char> rgba;
    int pw = 0;
    int ph = 0;
    if (!capture.readFrame(runtime, rgba, pw, ph, kCaptureRenderPasses)) {
        std::cerr << "  FAIL " << slug << " readFrame\n";
        return false;
    }
    const std::string path = outDir + "/" + slug + ".png";
    if (!probe::writeRgbaToPng(path, rgba.data(), pw, ph)) {
        std::cerr << "  FAIL " << slug << " 写 PNG\n";
        return false;
    }
    shot.name = slug;
    shot.titleInk = inkRatio(rgba, pw, ph, 0.17, 0.05, 0.40, 0.11);
    shot.sidebarInk = inkRatio(rgba, pw, ph, 0.01, 0.18, 0.18, 0.92);
    shot.ok = shot.titleInk > 0.003 && shot.sidebarInk > 0.01;
    std::cout << (shot.ok ? "  PASS " : "  FAIL ") << slug
              << " titleInk=" << shot.titleInk
              << " sidebarInk=" << shot.sidebarInk << " -> " << path << "\n";
    return shot.ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = "bug148-167-sweep";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) outDir = argv[++i];
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("bug148_167_sweep", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::MdHeadlessEnv mdEnv("bug148sweep_", "shots", "shots.db");
    core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();
    // 主题档必须在 startAll 之后设：static init 期 setDark 会被模块启动链覆盖回亮色
    if (app::g_sweepDark) core::theme::setDark();
    else core::theme::setLight();
    module::settings::applyWindowVisuals("off");

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    app::requestUpdate();
    app::requestShow();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "bug148_167_sweep: FrameCapture 建失败: " << capture.error() << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const auto [w, h] = probe::windowLogicalSize(window);
    const std::string themeTag = app::g_sweepDark ? "dark" : "light";
    bool passed = true;
    Shot shot;

    // 对照组：扫档前直接落 1.23（无缓存压力）——uat62 已证单值档正常，此处同口径复核
    core::theme::setFontScale(1.23f);
    passed &= capturePage(router, runtime, window, capture, ::core::PageId::Home,
                          "01-home-fs123-presweep-" + themeTag, outDir, w, shot);

    // 复现组：settings 页上模拟拖动（老板操作路径）：0.50→2.00 步进 0.01，
    // 151 个中间值 ×2 帧——字体栈缓存 16 槽按 fontSize key 必被打满+持续淘汰
    router.navigateTo(::core::PageId::Settings);
    for (int step = 50; step <= 200; step += 1) {
        core::theme::setFontScale(static_cast<float>(step) / 100.0f);
        for (int f = 0; f < 2; ++f) {
            probe::composeModuleFrame(runtime, window, "sweep", w, h,
                                      [&](eui::Ui& ui, const eui::Screen& screen) {
                                          app::compose(ui, screen);
                                      });
        }
    }
    // 扫档后落中间值 1.23（缓存刚被打满淘汰过的状态）
    core::theme::setFontScale(1.23f);
    passed &= capturePage(router, runtime, window, capture, ::core::PageId::Home,
                          "02-home-fs123-postsweep-" + themeTag, outDir, w, shot);
    passed &= capturePage(router, runtime, window, capture, ::core::PageId::Memory,
                          "03-memory-fs123-postsweep-" + themeTag, outDir, w, shot);
    // 回落默认档 1.0（老板另一路径：拖过去再拖回来）
    core::theme::setFontScale(1.0f);
    passed &= capturePage(router, runtime, window, capture, ::core::PageId::Home,
                          "04-home-fs100-back-" + themeTag, outDir, w, shot);
    passed &= capturePage(router, runtime, window, capture, ::core::PageId::Memory,
                          "05-memory-fs100-back-" + themeTag, outDir, w, shot);

    std::cout << (passed ? "bug148_167_sweep PASS [" : "bug148_167_sweep FAIL [")
              << themeTag << "]\n";
    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    return passed ? 0 : 1;
}
