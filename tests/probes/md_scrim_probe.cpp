// md_scrim_probe.cpp — bug165 MD 新建笔记弹窗遮罩取证探针
//
// 老板报单：MD 编辑器新建弹窗背景变灰（池 #165，后升级为全站遮罩口径核查）。
// 本探针只做一件事：打开新建笔记弹窗 → 稳帧后断言遮罩节点在、输出遮罩实测
// 颜色（rgba 浮点）→ 落 PNG。before/after 各跑一次落 evidence/md-scrim/，
// 供弹窗壳迁移（drawDialogFrame → drawDialogShell，36b1f602 同款）前后对照。
//
// 运行：md_scrim_probe [--shot-dialog <png>]
// 无开关 = 纯结构断言（backdrop 在 + 全显 + 面板节点在），供 --dev 内循环。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 mcp_dialog_probe）

#include "app_registry.h"

#include <iostream>
#include <memory>
#include <string>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_md_scrim_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string shotDialog;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-dialog" && i + 1 < argc) {
            shotDialog = argv[++i];
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("md_scrim_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();  // 无服务环境照常渲染（test_markdown md_module_compose_no_service 同前 提）

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    const auto [logicalW, logicalH] = probe::windowLogicalSize(window);
    const float w = logicalW > 1.0f ? logicalW : static_cast<float>(args.windowW);
    const float h = logicalH > 1.0f ? logicalH : static_cast<float>(args.windowH);
    bool passed = true;

    eui::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "md", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                mod->compose(u, screen);
            });
    };

    // 打开新建笔记弹窗：Ctrl+N（macOS=Cmd+N）走用户同款公开键通道
    //（openNewNoteDialog 为私有；test_support::setShortcutModifier 同口径修饰位，
    // 探针自持不引 doctest/夹具依赖）→ 稳帧跑完 160ms 过渡
    composeFrame();
    eui::KeyEvent cn;
    cn.key = eui::InputKey::N;
    cn.action = eui::KeyAction::Press;
#if defined(__APPLE__)
    cn.modifiers.super = true;
#else
    cn.modifiers.control = true;
#endif
    if (!mod->onKeyEvent(cn)) {
        std::cerr << "md_scrim_probe: Ctrl+N 未被模块消费，弹窗未触发\n";
        passed = false;
    }
    composeFrame();
    for (int i = 0; i < 24; ++i) {
        probe::pollOnce(window, runtime);
    }

    // 遮罩结构断言（clue 125：compose 后重新 find，指针不跨帧）
    const auto* backdrop = ui->find("md.dialog.new.backdrop");
    if (backdrop == nullptr || backdrop->opacity != 1.0f) {
        std::cerr << "md_scrim_probe: 遮罩节点缺失或未到全显态\n";
        passed = false;
    } else {
        const auto& c = backdrop->color;
        std::cout << "probe: backdrop rgba=(" << c.r << ", " << c.g << ", " << c.b
                  << ", " << c.a << ") size=(" << backdrop->frame.width << "x"
                  << backdrop->frame.height << ")\n";
    }
    if (ui->find("md.dialog.new.panel") == nullptr) {
        std::cerr << "md_scrim_probe: 弹窗面板节点缺失\n";
        passed = false;
    }

    // 落图（同 mcp_dialog_probe shotWith：FrameCapture 16 渲染趟）
    if (!shotDialog.empty() && passed) {
        probe::FrameCapture capture(window);
        if (!capture.ok()) {
            std::cerr << "md_scrim_probe: 帧捕获会话失败: " << capture.error() << "\n";
            passed = false;
        } else {
            std::vector<unsigned char> rgba;
            int cw = 0, ch = 0;
            if (!capture.readFrame(runtime, rgba, cw, ch, 16) ||
                !probe::writeRgbaToPng(shotDialog, rgba.data(), cw, ch)) {
                std::cerr << "md_scrim_probe: 落图失败\n";
                passed = false;
            } else {
                std::cout << "probe: screenshot " << shotDialog << " (" << cw << "x"
                          << ch << ")\n";
            }
        }
    }

    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << (passed ? "probe: PASS" : "probe: FAIL") << "\n";
    return passed ? 0 : 1;
}
