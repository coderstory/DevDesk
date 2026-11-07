// bug142_click_chain_probe.cpp — bug142 复现判定探针：mac 全 UI 点击无反应
//
// 背景（2026-11-06 21:43 老板实测）：mac 全 UI 点击无反应，疑 #140 标题栏重挂
// （euiChromeReapply 挂 NSWindowDidUpdateNotification 每事件循环 setStyleMask）。
// 根因考古结论：EUI-NEO 9d34e79 已双重修复（幂等闩 window_effect_macos.mm:105-115
// + DidUpdate 钩子移除 :1051-1055），DevDesk 7a6eb7ea 已推进 submodule 指针。
// 本探针在**当前 HEAD 构建**上实操验证点击链路真实响应（不是静态走读下结论）：
//
//   链路1 侧栏页切换：app.sidebar.item.Settings.hit 真点击 → 设置页当帧入 DOM
//   链路2 页内按钮：settings.tabs.1.hit（模块 tab）真点击 → 模块卡片当帧入 DOM
//   链路3 侧栏返回：app.sidebar.item.Home.hit 真点击 → 设置页内容当帧退出
//
// 证据：每步元素 frame dump 到 stdout + 三张状态截图落 --out 目录。
// 链接面同 bug148_167_sweep_probe（真 app::compose + app_assembly 全真模块）。
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
#include "module_settings/settings_config.h"  // applyWindowVisuals

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_bug142_click_chain_probe");
    return config;
}
}  // namespace app

namespace {

constexpr int kStableFrames = 24;          // 过渡/稳帧门槛（skill 24 帧口径）
constexpr int kCaptureRenderPasses = 17;   // bug185 retained layer 稳帧门槛

// id 前缀计数（模块卡片当帧入 DOM 的判据：模块 tab 点开后 settings.card.modules.* 出现）
int countIdPrefix(core::dsl::Ui& ui, const std::string& prefix) {
    int count = 0;
    for (const auto& root : ui.roots()) {
        std::vector<const core::dsl::Element*> stack{root.get()};
        while (!stack.empty()) {
            const core::dsl::Element* node = stack.back();
            stack.pop_back();
            if (node->id.rfind(prefix, 0) == 0) ++count;
            for (const auto& child : node->children) {
                stack.push_back(child.get());
            }
        }
    }
    return count;
}

void dumpElement(core::dsl::Ui& ui, const std::string& id) {
    const auto* e = ui.find(id);
    if (e == nullptr) {
        std::cout << "DUMP " << id << " MISSING\n";
        return;
    }
    std::cout << "DUMP " << id << " frame{x=" << e->frame.x
              << ",y=" << e->frame.y << ",w=" << e->frame.width
              << ",h=" << e->frame.height << "}\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = "bug142-click-chain";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) outDir = argv[++i];
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("bug142_click_chain", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::MdHeadlessEnv mdEnv("bug142chain_", "shots", "shots.db");
    core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        if (m == nullptr) continue;  // 分组父项行工厂为空（uat62 同口径跳过）
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();
    core::theme::setLight();
    module::settings::applyWindowVisuals("off");

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    app::requestUpdate();
    app::requestShow();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "bug142_click_chain_probe: FrameCapture 建失败: "
                  << capture.error() << "\n";
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

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, "app", w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      app::compose(u, screen);
                                  });
    };
    auto pump = [&](int frames) {
        for (int i = 0; i < frames; ++i) composeFrame();
    };
    auto shotAt = [&](const std::string& path) {
        std::vector<unsigned char> rgba;
        int pw = 0, ph = 0;
        if (!capture.readFrame(runtime, rgba, pw, ph, kCaptureRenderPasses) ||
            !probe::writeRgbaToPng(path, rgba.data(), pw, ph)) {
            std::cerr << "bug142_click_chain_probe: 落图失败 " << path << "\n";
            return false;
        }
        std::cout << "shot " << path << " " << pw << "x" << ph << "\n";
        return true;
    };
    // 真点击（命中区中心，坐标取当帧 DOM frame）→ 排队后 compose 消费
    auto clickElement = [&](const core::dsl::Element& element) {
        probe::click(window, runtime,
                     static_cast<double>(element.frame.x + element.frame.width * 0.5f),
                     static_cast<double>(element.frame.y + element.frame.height * 0.5f));
    };

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        std::cout << (cond ? "PASS " : "FAIL ") << what << "\n";
        if (!cond) passed = false;
    };

    // §0 初始稳帧：首页整 app compose
    pump(kStableFrames);
    check(ui != nullptr && ui->find("app.sidebar.item.Settings.hit") != nullptr,
          "§0 侧栏 Settings 条目 hit 区应存在");
    check(ui != nullptr && ui->find("settings.tabs.wrap") == nullptr,
          "§0 初始首页：设置页内容不应在 DOM");
    dumpElement(*ui, "app.sidebar.item.Settings.hit");
    shotAt(outDir + "/01-home-initial.png");

    // 链路1：侧栏页切换（真点击 Settings 条目）
    if (const auto* item = ui->find("app.sidebar.item.Settings.hit")) {
        clickElement(*item);
        pump(3);
    }
    check(ui != nullptr && ui->find("settings.tabs.wrap") != nullptr,
          "链路1 真点击侧栏 Settings：设置页应当帧入 DOM（点击响应）");
    dumpElement(*ui, "settings.tabs.wrap");
    shotAt(outDir + "/02-settings-after-sidebar-click.png");

    // 链路2：页内按钮（真点击「模块」tab → 模块卡片入 DOM）
    const int cardsBefore =
        ui != nullptr ? countIdPrefix(*ui, "settings.card.") : -1;
    if (const auto* tab = ui->find("settings.tabs.1.hit")) {
        dumpElement(*ui, "settings.tabs.1.hit");
        clickElement(*tab);
        pump(3);
    }
    const int cardsAfter =
        ui != nullptr ? countIdPrefix(*ui, "settings.card.") : -1;
    const int moduleCards =
        ui != nullptr ? countIdPrefix(*ui, "settings.card.modules") : 0;
    std::cout << "DIAG cardsBefore=" << cardsBefore
              << " cardsAfter=" << cardsAfter
              << " modulesCard=" << moduleCards << "\n";
    check(cardsBefore >= 0 && cardsAfter != cardsBefore,
          "链路2a 真点击「模块」tab：卡区 id 集应变化（点击响应：tab 切换生效）");
    check(moduleCards > 0,
          "链路2b 真点击设置页「模块」tab：模块卡片应入 DOM（点击响应，命中 " +
              std::to_string(moduleCards) + " 个 settings.card.modules* 元素）");
    shotAt(outDir + "/03-settings-modules-tab.png");

    // 链路3：侧栏返回首页（真点击 Home 条目 → 设置页内容当帧退出）
    if (const auto* home = ui->find("app.sidebar.item.Home.hit")) {
        clickElement(*home);
        pump(3);
    }
    check(ui != nullptr && ui->find("settings.tabs.wrap") == nullptr,
          "链路3 真点击侧栏 Home：应返回首页（设置页内容退出 DOM）");

    // 收尾
    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "bug142_click_chain_probe: PASS（三条点击链路全部真实响应）\n";
    }
    return passed ? 0 : 1;
}
