// ring_occlusion_probe.cpp — 池352：windowRing 边框线被内容效果层遮挡取证。
// 真窗 + 真 app::compose（装配链同 uat62_page_shots_probe）：
//   诊断：打印根层兄弟 paint 序（id + subtreeMaxZIndex）与 body 子树 max-z 元素
//   L（左缘/侧边栏）：Home 页稳帧，x=1.5 逻辑px 纵向扫描侧边栏纵向范围，
//      黑线占比 ≥80%（细黑边恒定不随主题；亮主题侧栏底亮色提供对比）
//   R（右缘/终端卡）：导航到 Terminal 页稳帧，x=w−1.5 扫描终端卡纵向范围同判据
//   C（点击不回归）：hitTestInteractiveAt 全窗网格采样永不出 app.ring.base；
//      侧边栏 Terminal 导航行中心命中其 .hit（ring 抬 z 后不截胡）；
//      行为链：probe::click 该导航行 → router.currentPage()==Terminal（真点击穿透）
// 修前红（L/R 黑线被盖）→ 修后绿。--screenshot <png> 落两页诊断图（-home/-term）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现
#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"              // 真 app::compose
#include "module_toggle_bridge.h"
#include "test_support.h"             // MdHeadlessEnv / ScopedAppRouter

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ui_kit/window_ring.h"

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_ring_occlusion_probe");
    return config;
}
}  // namespace app

namespace {

struct Rgb {
    int r, g, b;
};
bool isBlackish(const Rgb& c) { return c.r <= 60 && c.g <= 60 && c.b <= 60; }

// 树内 max-z 元素诊断（subtreeMaxZIndex 达上界的元素 id 列表，截断）
void collectMaxZ(const core::dsl::Element* e, int maxZ, std::string& out) {
    if (e == nullptr || out.size() > 200) return;
    if (e->zIndex == maxZ && maxZ > 0) {
        if (!out.empty()) out += ",";
        out += e->id;
    }
    for (const auto& c : e->children) collectMaxZ(c.get(), maxZ, out);
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // 亮主题（黑线 vs 亮底对比最大化；settings onStart 后需再压一次，同 uat62 时序）
    core::theme::setLight();

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("ring_occlusion", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::MdHeadlessEnv mdEnv("ringocc_", "occ", "occ.db");
    ::core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    ::core::PageId terminalPage = ::core::PageId::Home;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        if (m && m->pageId() == ::core::PageId::Terminal) terminalPage = e.page;
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();
    core::theme::setLight();  // 压回：settings onStart 会按配置覆档（uat62 同款时序）

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    app::requestUpdate();
    app::requestShow();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "ring_occlusion: FrameCapture 失败: " << capture.error()
                  << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }

    const auto [w, h] = probe::windowLogicalSize(window);
    const float scale = capture.scale();
    bool passed = true;
    eui::Ui* lastUi = nullptr;

    auto settleCompose = [&](int minFrames) {
        for (int f = 0; f < 240; ++f) {
            probe::composeModuleFrame(
                runtime, window, "occ", w, h,
                [&](eui::Ui& ui, const eui::Screen& screen) {
                    app::compose(ui, screen);
                    lastUi = &ui;
                });
            if (f + 1 >= minFrames && !runtime.isAnimating()) break;
        }
    };

    auto sample = [&](const std::vector<unsigned char>& rgba, int iw,
                      float lx, float ly) -> Rgb {
        const int px = static_cast<int>(lx * scale);
        const int py = static_cast<int>(ly * scale);
        if (px < 0 || py < 0 || px >= iw) return {-1, -1, -1};
        const std::size_t rows = rgba.size() / 4u /
                                 static_cast<std::size_t>(iw > 0 ? iw : 1);
        if (py >= static_cast<int>(rows)) return {-1, -1, -1};
        const std::size_t base =
            (static_cast<std::size_t>(py) * iw + px) * 4u;
        return {rgba[base], rgba[base + 1], rgba[base + 2]};
    };

    // 边带扫描：x 固定 1.5 逻辑px（inset1+线宽1 的中线），纵向 [y0,y1)，
    // 黑占比 ≥80% 过（扣圆角/抗锯齿余量由 y0/y1 预留控制）
    auto scanEdge = [&](const std::vector<unsigned char>& rgba, int iw,
                        bool left, float y0, float y1, const char* tag,
                        const std::string& shotPath) -> int {
        const float x = left ? ui_kit::kWindowRingInset +
                                   ui_kit::kWindowRingBorder * 0.5f
                             : w - ui_kit::kWindowRingInset -
                                   ui_kit::kWindowRingBorder * 0.5f;
        int black = 0, total = 0;
        for (float ly = y0; ly < y1; ly += 1.0f) {
            ++total;
            if (isBlackish(sample(rgba, iw, x, ly))) ++black;
        }
        std::cout << "scan " << tag << " total=" << total << " black=" << black
                  << "\n";
        if (!shotPath.empty())
            probe::writeRgbaToPng(shotPath, rgba.data(), iw,
                                  static_cast<int>(rgba.size() / 4u /
                                                   static_cast<std::size_t>(
                                                       iw > 0 ? iw : 1)));
        if (total <= 0 || black * 5 < total * 4) {
            std::cerr << "ring_occlusion: " << tag
                      << " FAIL 黑线被内容盖 black=" << black << "/" << total
                      << "\n";
            return 1;
        }
        std::cout << tag << " PASS\n";
        return 0;
    };

    const std::string shotPrefix = args.screenshotPath;

    // ---------- 场景 L0（信息性）：Home 稳态（全 z0 平局 → DOM 序 ring 在顶） ----------
    settleCompose(24);
    if (lastUi != nullptr) {
        std::cout << "--- 根层 paint 序（先=底）---\n";
        for (const core::dsl::Element* r : lastUi->orderedRoots()) {
            std::string maxIds;
            collectMaxZ(r, r->subtreeMaxZIndex, maxIds);
            std::cout << "  z<" << r->subtreeMaxZIndex << "> " << r->id
                      << (maxIds.empty() ? "" : " maxZ@:" + maxIds) << "\n";
        }
    }
    {
        std::vector<unsigned char> rgba;
        int iw = 0, ih = 0;
        if (!capture.readFrame(runtime, rgba, iw, ih, 17)) {
            std::cerr << "ring_occlusion: readFrame 失败\nFAIL\n";
            return 5;
        }
        // 纵向避开顶 40（标题/圆角 r16+余量）与底 40（toggle/圆角）；信息性不设门
        scanEdge(rgba, iw, true, 44.0f, h - 44.0f, "L0-home-info",
                 shotPrefix.empty() ? "" : shotPrefix + "-home.png");
    }

    // ---------- 场景 L/R1（判据门）：Settings 页 ----------
    // body 常驻 z30/z40 卡（settings_ui_cards.cpp:147 zIndex(40)）→ 根层按子树
    // maxZ 排序（dsl.h:1793）把整个内容子树抬到 ring(z0) 之上——同帧侧边栏底色
    // 与页面卡直画盖掉左/右缘黑线。这就是「侧边栏盖左缘 + 内容卡盖右缘」一族的
    // 确定性最小复现（真机另叠加翻转帧后脏矩形不还原的持久化变体，同修）。
    ::core::PageId settingsPage = ::core::PageId::Home;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page == ::core::PageId::Settings) settingsPage = e.page;
    }
    router.navigateTo(settingsPage);
    settleCompose(24);
    {
        std::vector<unsigned char> rgba;
        int iw = 0, ih = 0;
        if (!capture.readFrame(runtime, rgba, iw, ih, 17)) {
            std::cerr << "ring_occlusion: readFrame(settings) 失败\nFAIL\n";
            return 6;
        }
        passed &= scanEdge(rgba, iw, true, 44.0f, h - 44.0f, "L-settings",
                           shotPrefix.empty() ? "" : shotPrefix + "-settings.png")
                  == 0;
        // 信息性：Settings 页内容右边距不贴窗缘（黑线穿「空隙」存活），不作判据门；
        // 右缘判据门 = R2 Terminal 页（term.bg 全幅贴缘，boss 症状页）。
        scanEdge(rgba, iw, false, 44.0f, h - 44.0f, "R1-settings-info", "");
    }

    // ---------- C 行为链：真点击侧边栏首行导航回 Home（点击必须穿透 ring） ----------
    // hitTestInteractiveAt 扫侧边栏列，命中的第一个交互元素即首导航行成员。
    float navX = 24.0f, navY = -1.0f;
    std::string navHitId;
    for (float ly = 60.0f; ly < h - 60.0f && navY < 0.0f; ly += 8.0f) {
        const std::string id = runtime.hitTestInteractiveAt(navX, ly, scale);
        if (!id.empty() &&
            id.find("app.sidebar.") != std::string::npos) {
            navY = ly;
            navHitId = id;
        }
    }
    if (navY < 0.0f) {
        std::cerr << "ring_occlusion: 侧边栏导航行未命中（x=" << navX
                  << " 全列扫描无交互元素）\nFAIL\n";
        return 7;
    }
    std::cout << "nav hit: " << navHitId << " @(" << navX << "," << navY
              << ")\n";
    probe::click(window, runtime, navX, navY);
    settleCompose(24);
    if (router.currentPage() != ::core::PageId::Home) {
        std::cerr << "ring_occlusion: C FAIL 点击导航未回 Home（currentPage="
                  << static_cast<int>(router.currentPage()) << "）\n";
        passed = false;
    } else {
        std::cout << "C-behavior PASS 点击穿透 ring 完成导航\n";
    }

    // ---------- 场景 R2（判据门）：Terminal 页右缘（终端大卡/tab 条纵向范围） ----------
    router.navigateTo(terminalPage);
    settleCompose(24);

    // ---------- 场景 R：Terminal 页 / 终端大卡盖右缘 ----------
    {
        std::vector<unsigned char> rgba;
        int iw = 0, ih = 0;
        if (!capture.readFrame(runtime, rgba, iw, ih, 17)) {
            std::cerr << "ring_occlusion: readFrame(term) 失败\nFAIL\n";
            return 7;
        }
        passed &= scanEdge(rgba, iw, false, 44.0f, h - 44.0f, "R-right",
                           shotPrefix.empty() ? "" : shotPrefix + "-term.png")
                  == 0;
        if (lastUi != nullptr &&
            (lastUi->find("term.root") == nullptr ||
             lastUi->find("app.ring.base") == nullptr)) {
            std::cerr << "ring_occlusion: DOM FAIL term.root/app.ring.base 缺失"
                      << "\n";
            passed = false;
        }
    }

    // ---------- C 判据：ring 永不截胡命中 ----------
    if (lastUi != nullptr) {
        const core::dsl::Element* ring = lastUi->find("app.ring.base");
        if (ring == nullptr || ring->interactive) {
            std::cerr << "ring_occlusion: C FAIL app.ring.base 缺失或 interactive"
                      << "\n";
            passed = false;
        }
    }
    for (float lx = 8.0f; lx < w; lx += 24.0f) {
        for (float ly = 8.0f; ly < h; ly += 24.0f) {
            const std::string hit =
                runtime.hitTestInteractiveAt(lx, ly, scale);
            if (hit.find("app.ring.base") != std::string::npos) {
                std::cerr << "ring_occlusion: C FAIL ring 截胡命中 @(" << lx
                          << "," << ly << ")\n";
                passed = false;
            }
        }
    }

    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "ring_occlusion: PASS\n";
    return 0;
}
