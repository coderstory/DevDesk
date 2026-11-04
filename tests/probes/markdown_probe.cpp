// markdown_probe.cpp — Markdown 笔记本页截图探针（W0.1，UI 整改改前基线存档）
//
// 运行：markdown_probe.exe [--screenshot <路径>] [--size WxH]
// 不带 --screenshot = 纯 compose 自检（零开销，不写文件）。
// 用途：UI 整改 W0.2 改前基线截图存档——改前/改后对照锚点。仿 home_probes 骨架
// （init→TestDb→create_module→onStart→compose→captureIfRequested），不带几何审计组。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"
#include "core/app_config.h"  // appconfig::get（163 落库契约断言）
#include "core/module_interface.h"
#include "core/theme.h"       // editorFontSize（163 通道断言）

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

    // 池163：状态栏字号设置端到端（入口存在 → 开 → 选档 → 通道生效 + 落库
    // settings 同键 → 弹层收起）。删 dd / 摘 onChange / 摘落库任一环必红。
    {
        auto clickElement = [&](const core::dsl::Element& e) {
            core::queuePointerButton(window, e.frame.x + e.frame.width * 0.5f,
                                     e.frame.y + e.frame.height * 0.5f,
                                     core::PointerButton::Left,
                                     core::PointerAction::Press, {});
            core::queuePointerButton(window, e.frame.x + e.frame.width * 0.5f,
                                     e.frame.y + e.frame.height * 0.5f,
                                     core::PointerButton::Left,
                                     core::PointerAction::Release, {});
        };

        // 基线：通道默认 15，入口存在（空态状态栏也绘制，internal.h 空态分支）
        if (ui != nullptr && ui->find("md.status.fontdd") == nullptr) {
            std::cerr << "markdown_probe: md.status.fontdd 不存在（字号设置"
                         "入口缺失）\n";
            passed = false;
        }
        const float baselinePx = core::theme::editorFontSize();
        if (ui != nullptr && baselinePx != 15.0f) {
            std::cerr << "markdown_probe: 基线字号非 15（探针前置污染）\n";
            passed = false;
        }
        // 开下拉 → 点第 3 档「大 17px」→ 收起
        if (ui != nullptr && passed) {
            if (const core::dsl::Element* field =
                    ui->find("md.status.fontdd.field");
                field != nullptr) {
                clickElement(*field);
                composeFrame();
                const core::dsl::Element* item = ui->find("md.status.fontdd.item.2");
                if (item == nullptr) {
                    std::cerr << "markdown_probe: 弹层未出（md.status.fontdd."
                                 "item.2 缺失，openUp/开合链路断）\n";
                    passed = false;
                } else {
                    clickElement(*item);
                    composeFrame();
                    if (core::theme::editorFontSize() != 17.0f) {
                        std::cerr << "markdown_probe: 选档后通道字号 "
                                  << core::theme::editorFontSize()
                                  << " != 17（onChange 链断）\n";
                        passed = false;
                    }
                    // settings 全键形态（命名空间前缀 + editorFontSize，163 契约）
                    auto raw = core::appconfig::get(
                        tdb.handle(), "config.json:editorFontSize");
                    if (raw.isFail() || raw.value() != "17") {
                        std::cerr << "markdown_probe: 落库值 \"17\" 缺失/不符（raw="
                                  << (raw.isOk() ? raw.value() : "<fail>")
                                  << "，与 settings 键契约断）\n";
                        passed = false;
                    }
                    const core::dsl::Element* closedItem =
                        ui->find("md.status.fontdd.item.2");
                    if (closedItem == nullptr || !closedItem->disabled) {
                        std::cerr << "markdown_probe: 选档后弹层未收起\n";
                        passed = false;
                    }
                }
            } else {
                std::cerr << "markdown_probe: md.status.fontdd.field 缺失\n";
                passed = false;
            }
            composeFrame();
        }
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
