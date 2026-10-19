// markdown_probe.cpp — Markdown 笔记本页截图探针（W0.1，UI 整改改前基线存档）
//
// 运行：markdown_probe.exe [--screenshot <路径>] [--size WxH]
// 不带 --screenshot = 纯 compose 自检（零开销，不写文件）。
// 用途：UI 整改 W0.2 改前基线截图存档——改前/改后对照锚点。仿 home_probes 骨架
// （init→TestDb→create_module→onStart→compose→captureIfRequested），不带几何审计组。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"
#include "core/module_interface.h"

#include <iostream>
#include <memory>
#include <string>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_markdown_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

extern "C" ::core::Module* create_markdown_module();

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("markdown_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 空迁移库（改前基线 = 空文件树状态，合理）
    test_support::TestDb tdb("mdprobe_", "probe");
    test_support::ScopedAppDatabase scopedDb(tdb.handle());

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "markdown", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                mod->compose(u, screen);
            });
    };

    // 稳帧：首帧建树，第二帧让布局/transition 收敛
    composeFrame();
    composeFrame();

    // 自检：compose 跑过了（ui 非空 = 树建出来了，不是空树→黑图）
    if (ui == nullptr) {
        std::cerr << "markdown_probe: FAIL——compose 未建出 UI 树"
                     "（create_markdown_module/onStart/compose 链路断？）\n";
        passed = false;
    }

    // --screenshot 落图（未传 = 零开销不写文件；传了失败→passed=false）
    probe::captureIfRequested(args, window, runtime, &passed);

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "markdown_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}
