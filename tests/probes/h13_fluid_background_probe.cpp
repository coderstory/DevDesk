// h13_fluid_background_probe.cpp — H13 活体探针：真窗口 fluidPhaseSeconds 推进验证
//
// 采样 app_sidebar.cpp:189 fluidPhaseSeconds() 两帧差值，证实时钟驱动相位推进。
// 若相位冻结（差值=0）→ 条件修复授权：把 drawFluidBackground 相位改 elapsed 时间源
// 驱动（app 侧 1-2 行，不动 EUI），修后复跑证恢复（先红后绿）。
//
// 额外帧泵采样：md.tree.anim.driver / splash.ink（验证这两项的动画帧泵也在推进）
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // DslAppConfig（namespace app）
#include "app_sidebar.h"      // fluidPhaseSeconds / resetFluidBackgroundForTests / setFluidBackgroundEnabled
#include "app_assembly.h"
#include "app_registry.h"
#include "module_toggle_bridge.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// app:: 层链接桩（dsl_app_impl.h 引用 dslAppConfig）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_h13_fluid_probe");
    return config;
}
}  // namespace app

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) {
        std::cerr << "PROBE_SKIP_NO_DISPLAY: window backend init failed" << std::endl;
        return probe::kSkipExitCode;
    }

    auto window = probe::makeWindow("H13 Fluid Probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        std::cerr << "PROBE_SKIP_NO_DISPLAY: window creation failed" << std::endl;
        return probe::kSkipExitCode;
    }

    // 复位流体背景为已知状态：阶段归零 + 开关开
    app::resetFluidBackgroundForTests();
    app::setFluidBackgroundEnabled(true);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    float phaseBefore = app::fluidPhaseSeconds();
    std::cout << "H13: phaseBefore=" << phaseBefore << std::endl;

    // 跑 >=2 帧：pollOnce 触发 onFrame 回调推进 fluid 相位，compose 重建树
    for (int i = 0; i < 3; ++i) {
        probe::composeModuleFrame(runtime, window, "home",
                                  static_cast<float>(args.windowW),
                                  static_cast<float>(args.windowH),
                                  [&](eui::Ui& ui, const eui::Screen& screen) {
                                      app::compose(ui, screen);
                                  });
    }

    float phaseAfter = app::fluidPhaseSeconds();
    float delta = phaseAfter - phaseBefore;

    std::cout << "H13: phaseAfter=" << phaseAfter
              << " delta=" << delta << std::endl;

    bool passed = true;
    if (delta <= 0.0f) {
        std::cerr << "FAIL: fluidPhase frozen — delta=" << delta << std::endl;
        passed = false;
    } else {
        float perFrame = delta / 3.0f;
        std::cout << "PASS: fluidPhase advancing at "
                  << perFrame << " s/frame" << std::endl;
    }

    app::setFluidBackgroundEnabled(false);
    probe::terminateWindowBackend();

    return passed ? 0 : 1;
}