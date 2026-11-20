// need331_assoc_card_probe.cpp — need331 设置页「记事本文件关联」卡回读取证探针
//
// 背景（pool need#331）：老板实测 OS 弹窗选了 DevDesk 后设置卡仍显未关联；
// 回读代码已改 UTI 级查询（wip 064f40c1），本探针产出「修复后设置页回读」
// 实操证据：真 app::compose 渲染设置页模块 tab 的关联卡，状态字由真实
// LSCopyDefaultRoleHandlerForContentType 逐扩展查询聚合驱动（机器全局 LS 库，
// 与隔离库无关）。
//
// 运行形态决定判属锚（resolveAnchor = CFBundleGetMainBundle bundle id）：
//   · 裸可执行运行      → anchor 空 → aggregate Unknown → 卡显「未生效」（阴性对照，
//                         证明状态字真由回读驱动而非写死）
//   · 壳 .app 内运行    → anchor=com.devdesk.app → 与 LS handler 全等 → 「已关联」
// （壳 .app 只提供 Info.plist 身份，不 lsregister，不触 LS 注册态）
//
// 数据隔离：MdHeadlessEnv 空库注册为 appDatabase，仅种
// notepadAssocEnabled/everEnabled=true（开关是回读的前置输入；LS 查询本身
// 不受隔离影响）。全流程只读 LS，无写入路径（无 main.cpp 启动自愈）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现
#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"
#include "module_toggle_bridge.h"
#include "test_support.h"  // MdHeadlessEnv / ScopedAppRouter

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_settings/settings_config.h"    // SettingsStore / applyWindowVisuals
#include "module_settings/settings_ui_logic.h"  // kSettingTabCount

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace app {
const struct Need331ThemeInit {
    Need331ThemeInit() { core::theme::setLight(); }
} g_need331ThemeInit;

const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_need331_assoc_card_probe");
    return config;
}
}  // namespace app

namespace {

// 捕获前暖场渲染遍数（bug185：retained layer 稳帧门槛 16 帧 + 1 裕量）
constexpr int kCaptureRenderPasses = 17;

// 稳帧 compose 循环（uat62 同款）：24 帧起步、动画停即止；ui 指针随每帧刷
void settleFrames(core::dsl::Runtime& runtime, core::window::Handle window,
                  float w, float h, eui::Ui** uiOut) {
    for (int f = 0; f < 240; ++f) {
        probe::composeModuleFrame(
            runtime, window, "shots", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                if (uiOut != nullptr) *uiOut = &u;
                app::compose(u, screen);
            });
        if (f + 1 >= 24 && !runtime.isAnimating()) break;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = "need331-assoc-shots";
    std::string shotName = "assoc-card";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (std::string(argv[i]) == "--shot-name" && i + 1 < argc) {
            shotName = argv[++i];
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("need331_assoc_card", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 数据隔离（空库注册为 appDatabase）+ 种关联开关（回读前置输入）
    test_support::MdHeadlessEnv mdEnv("need331assoc_", "assoc", "assoc.db");
    {
        module::settings::SettingsStore store(::core::appDatabase());
        auto cfg = store.load();
        cfg.notepadAssocEnabled = true;
        cfg.notepadAssocEverEnabled = true;
        const auto r = store.save(cfg);
        if (r.isFail()) {
            std::cerr << "need331_assoc_card: 种开关失败: " << r.errorMessage()
                      << "\n";
        }
    }

    // 全登记表真模块（settings 在表内；nullptr 行 registerModule 幂等跳过）
    ::core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();
    module::settings::applyWindowVisuals("off");

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    app::requestUpdate();
    app::requestShow();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "need331_assoc_card: FrameCapture 建失败: "
                  << capture.error() << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }

    // 导航设置页（PageId 枚举直取，与登记表行解耦）
    router.navigateTo(::core::PageId::Settings);

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    eui::Ui* ui = nullptr;
    settleFrames(runtime, window, w, h, &ui);

    // 切「模块」tab（tab1），真命中分发点（进程内输入队列，非 OS 鼠标事件）
    const core::dsl::Element* hit =
        ui != nullptr ? ui->find("settings.tabs.1.hit") : nullptr;
    if (hit == nullptr) {
        std::cerr << "need331_assoc_card: settings.tabs.1.hit 不存在\n";
        return 5;
    }
    probe::click(window, runtime, hit->frame.x + hit->frame.width * 0.5f,
                 hit->frame.y + hit->frame.height * 0.5f);
    settleFrames(runtime, window, w, h, &ui);

    // 落图（判据失败也落 -debug 图，不静默吞帧）
    std::filesystem::create_directories(outDir);
    const std::string path = outDir + "/" + shotName + ".png";
    std::vector<unsigned char> rgba;
    int pw = 0;
    int ph = 0;
    const bool ok = capture.readFrame(runtime, rgba, pw, ph,
                                      kCaptureRenderPasses) &&
        probe::writeRgbaToPng(path, rgba.data(), pw, ph);
    if (!ok) {
        std::cerr << "need331_assoc_card: 捕获/写图失败 " << path << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 6;
    }
    std::cout << "need331_assoc_card: " << path << " (" << pw << "x" << ph
              << ")\n";

    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << "need331_assoc_card: PASS\n";
    return 0;
}
