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

    // need161：状态栏字号步进端到端（入口存在 → A+ → 通道生效 + 落库 settings
    // 同键 → A− 回落 → 12/24 边界 disabled）。删钮 / 摘 onClick / 摘落库 /
    // 摘边界 disabled 任一环必红。
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
        if (ui != nullptr && ui->find("md.status.fontminus") == nullptr) {
            std::cerr << "markdown_probe: md.status.fontminus 不存在（字号步进"
                         "入口缺失）\n";
            passed = false;
        }
        if (ui != nullptr && ui->find("md.status.fontplus") == nullptr) {
            std::cerr << "markdown_probe: md.status.fontplus 不存在（字号步进"
                         "入口缺失）\n";
            passed = false;
        }
        const float baselinePx = core::theme::editorFontSize();
        if (ui != nullptr && baselinePx != 15.0f) {
            std::cerr << "markdown_probe: 基线字号非 15（探针前置污染）\n";
            passed = false;
        }
        if (ui != nullptr && passed) {
            const core::dsl::Element* plus = ui->find("md.status.fontplus.bg");
            if (plus == nullptr) {
                std::cerr << "markdown_probe: 步进按钮 .bg 缺失（onClick 载体）\n";
                passed = false;
            } else {
                // A+ → 16：通道生效 + settings 全键形态落库（163 契约）
                clickElement(*plus);
                composeFrame();
                if (core::theme::editorFontSize() != 16.0f) {
                    std::cerr << "markdown_probe: A+ 后通道字号 "
                              << core::theme::editorFontSize()
                              << " != 16（onClick 链断）\n";
                    passed = false;
                }
                auto raw = core::appconfig::get(
                    tdb.handle(), "config.json:editorFontSize");
                if (raw.isFail() || raw.value() != "16") {
                    std::cerr << "markdown_probe: 落库值 \"16\" 缺失/不符（raw="
                              << (raw.isOk() ? raw.value() : "<fail>")
                              << "，与 settings 键契约断）\n";
                    passed = false;
                }
                // A− → 回 15。元素每帧重建（W3.5 悬垂教训）：A+ 后已 compose 过
                // 新帧，minus 必须重新 find，旧指针 = 悬垂（frame 读出垃圾坐标）。
                const core::dsl::Element* minus = ui->find("md.status.fontminus.bg");
                if (minus == nullptr) {
                    std::cerr << "markdown_probe: md.status.fontminus.bg 缺失\n";
                    passed = false;
                } else {
                clickElement(*minus);
                composeFrame();
                if (core::theme::editorFontSize() != 15.0f) {
                    std::cerr << "markdown_probe: A- 后通道字号 "
                              << core::theme::editorFontSize()
                              << " != 15（onClick 链断）\n";
                    passed = false;
                }
                // 边界 disabled：顶到 24 → A+ 灰；落到 12 → A− 灰
                core::theme::setEditorFontSize(24.0f);
                composeFrame();
                const core::dsl::Element* plusAtMax =
                    ui->find("md.status.fontplus.bg");
                if (plusAtMax == nullptr || !plusAtMax->disabled) {
                    std::cerr << "markdown_probe: 24px 处 A+ 未 disabled\n";
                    passed = false;
                }
                core::theme::setEditorFontSize(12.0f);
                composeFrame();
                const core::dsl::Element* minusAtMin =
                    ui->find("md.status.fontminus.bg");
                if (minusAtMin == nullptr || !minusAtMin->disabled) {
                    std::cerr << "markdown_probe: 12px 处 A- 未 disabled\n";
                    passed = false;
                }
                core::theme::setEditorFontSize(15.0f);
                }
            }
            composeFrame();
        }
    }

    // 截图前把虚拟指针挪离步进钮：E2E 末次点击的合成指针悬停停在 A- 上，hover
    // 视觉会被冻进截图（帧捕获 = glReadPixels 后备缓冲，不含真实光标）。挪到
    // 窗口角落再稳两帧，截「静止态」。
    core::queuePointerMotion(window, 8.0, 8.0, {}, {});
    composeFrame();
    composeFrame();

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
