// h13_fluid_background_probe.cpp — H13 活体探针：真窗口下流体背景「绿晕可见位移」验证
//
// 判别力约定（audit B-F10 / A-L6）：只证 fluidPhaseSeconds() 推进是恒真的——
// 壁钟必走、即便 drawFluidBackground 不再消费相位（动画真冻结）探针也绿。
// 本版改为视觉层断言：两次独立 compose（间隔 200ms 壁钟相位前进）采样
// app.fluid.halo.N 节点帧位，逐 halo 比较中心位移；位移 ≤ 阈值即红。
// 「动画视觉静止」实现（halo 位置不随相位变化 / 相位源冻结）下位移为 0 → 必红。
//
// 采样不经过 Runtime 帧循环：相位源是壁钟（fluidPhaseSeconds），halo 帧位在
// compose 期即由相位派生，Ui 树两次独立构建即可比对——不依赖 onFrame 泵投递，
// 也不动 app.fluid.anim.driver / g_fluidPhase（归并行动画批）。
#include "probes/probe_harness.h"

#include <eui/dsl_app.h>  // DslAppConfig（app:: 层链接桩，main.cpp 不在 probe 链接面）

#include "app_sidebar.h"  // fluidPhaseSeconds / resetFluidBackgroundForTests /
                          // setFluidBackgroundEnabled / drawFluidBackground

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// app:: 层链接桩（dsl_app_impl 模板引用 dslAppConfig；main.cpp 不在 probe 链接面）
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

    const eui::Screen screen{static_cast<float>(args.windowW),
                             static_cast<float>(args.windowH)};

    // 一次「构建树 + 布局 + 读 halo 中心」：halo 帧位由 compose 期相位派生，
    // 布局只是把显式 position 落到 frame。
    const auto composeAndReadCenters = [&](std::vector<float>& cx,
                                           std::vector<float>& cy) -> bool {
        eui::Ui ui;
        ui.begin("h13_fluid_probe");
        app::drawFluidBackground(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        constexpr int kHalos = 7;   // 与 app_sidebar.cpp drawFluidBackground 同数
        cx.assign(kHalos, 0.0f);
        cy.assign(kHalos, 0.0f);
        for (int j = 0; j < kHalos; ++j) {
            const auto* halo = ui.find("app.fluid.halo." + std::to_string(j));
            if (halo == nullptr) {
                std::cerr << "FAIL: app.fluid.halo." << j
                          << " 不在树中——流体背景未产出 halo 节点" << std::endl;
                return false;
            }
            cx[j] = halo->frame.x + halo->frame.width * 0.5f;
            cy[j] = halo->frame.y + halo->frame.height * 0.5f;
        }
        return true;
    };

    std::vector<float> cx1, cy1, cx2, cy2;
    if (!composeAndReadCenters(cx1, cy1)) {
        probe::terminateWindowBackend();
        return 1;
    }

    // 两次采样之间壁钟相位前进 200ms：halo 漂移速度量级为宽×0.16×角速度
    // （约 4~9 px/s），200ms 位移 ≥ 0.8px（除各 halo 相位导数为零的罕见点；
    // 下方取 7 halo 最大位移规避单 halo 余弦过零）。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (!composeAndReadCenters(cx2, cy2)) {
        probe::terminateWindowBackend();
        return 1;
    }

    // 视觉位移判据：任一 halo 中心欧氏位移 ≥ 阈值（0.3px = 期望值 ~1px 的
    // 三分之一，稳过正常实现；视觉静止实现位移恒 0 → 必红）。
    constexpr float kMinVisualDisplacement = 0.3f;
    float maxDisplacement = 0.0f;
    int maxHalo = -1;
    for (size_t j = 0; j < cx1.size(); ++j) {
        const float dx = cx2[j] - cx1[j];
        const float dy = cy2[j] - cy1[j];
        const float dist = std::sqrt(dx * dx + dy * dy);
        std::cout << "H13: halo." << j << " dx=" << dx << " dy=" << dy
                  << " dist=" << dist << std::endl;
        if (dist > maxDisplacement) {
            maxDisplacement = dist;
            maxHalo = static_cast<int>(j);
        }
    }

    app::setFluidBackgroundEnabled(false);
    probe::terminateWindowBackend();

    if (maxDisplacement < kMinVisualDisplacement) {
        std::cerr << "FAIL: fluid halo 视觉位移不足——max=" << maxDisplacement
                  << "px（halo." << maxHalo << "）< " << kMinVisualDisplacement
                  << "px：动画视觉静止或未消费相位" << std::endl;
        return 1;
    }
    std::cout << "PASS: fluid halo 可见位移 max=" << maxDisplacement
              << "px（halo." << maxHalo << "）≥ " << kMinVisualDisplacement
              << "px" << std::endl;
    return 0;
}
