// md_viewer_probe.cpp — 只读查看器窗口探针（U3/U4：渲染对照 / 双窗并存 / 搜索键盘
// 全流程 / 无编辑入口几何断言）。
//
// 运行：md_viewer_probe.exe [--screenshot <路径>] [--size WxH]
// 不带 --screenshot = 纯 compose 自检（零开销，不写文件）。执行归汇总段
//（DEVDESK_BUILD_PROBES 构建后运行；本探针不跑全量）。
//
// 断言面（headless 可达部分；真 OS 双击多窗/真焦点键盘归 UAT）：
//   ① 渲染对照：session compose 经 U2 渲染核建出块流（blk 包裹 id 在树）+ 正文列
//      宽 min(窗宽-80, 760)；
//   ② 无编辑入口：kEditorInputId 不在查看器树（只读构造性保证）+ Idle 无搜索条；
//   ③ 搜索键盘全流程：窗口级 onKeyEvent 直驱（Ctrl+F → 0/0 Enter 无操作 → 注入
//      query 高亮 → Enter 逐跳 scrollOffset 回读单调推进 → Shift+Enter 回退 →
//      Esc 收起回阅读态）；
//   ④ 双窗并存：两 session 两请求 title/pageId 互异，各自 compose 独立建树；
//   ⑤ 错误占位：缺文件窗内占位（流程 B7）。
//
// 与 markdown_probe 同骨架（init → 窗口 → 帧循环 → 断言 → captureIfRequested）；
// app:: 层桩同款（probe 不建应用级 UI）。窗口请求经 app::consumeWindowRequests
// 取自 EUI-NEO openWindow 队列后逐请求驱动（probe 单窗 harness 承载不下真多窗，
// 真多窗并存断言在窗口请求级 + session 注册表级，创建/销毁主循环归 UAT）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "core/input/input_types.h"
#include "core/theme.h"
#include "module_markdown/md_ids.h"
#include "module_markdown/md_viewer_window.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

// app:: 层桩（同 markdown_probe：composeModuleFrame 不经 app::compose）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_md_viewer_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

std::filesystem::path makeSampleDir() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "devdesk_md_viewer_probe";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        // 5 处「指标」分散在长文档中（跳转 offset 回读须单调推进 → 命中间隔
        // 填充段落保证滚动范围超出视口）
        std::ofstream f(dir / "sample.md", std::ios::trunc);
        f << "# 采样指标文档\n\n";
        for (int i = 0; i < 5; ++i) {
            f << "第" << (i + 1) << "处指标在这里。\n\n";
            for (int j = 0; j < 12; ++j) {
                f << "填充段落 " << i << "-" << j
                  << "：一行普通文字用于拉开滚动距离。\n\n";
            }
        }
    }
    {
        std::ofstream f(dir / "b.md", std::ios::trunc);
        f << "# 另一窗\n\n正文。\n";
    }
    return dir;
}

eui::KeyEvent pressKey(eui::InputKey key, bool control = false,
                       bool shift = false) {
    eui::KeyEvent ev{};
    ev.key = key;
    ev.action = eui::KeyAction::Press;
    ev.modifiers.control = control;
    ev.modifiers.shift = shift;
    return ev;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("md_viewer_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    bool passed = true;
    eui::Ui* ui = nullptr;
    const std::filesystem::path dir = makeSampleDir();
    const std::string samplePath = (dir / "sample.md").string();
    const std::string bPath = (dir / "b.md").string();

    // 开两窗（双窗并存素材）+ 一缺文件窗（B7 素材）
    module::markdown::openViewerWindow(samplePath);
    module::markdown::openViewerWindow(bPath);
    module::markdown::openViewerWindow((dir / "missing.md").string());
    auto reqs = app::consumeWindowRequests();
    if (reqs.size() != 3) {
        std::cerr << "md_viewer_probe: FAIL——窗口请求数 " << reqs.size()
                  << " != 3（openViewerWindow 未逐文件建窗？）\n";
        passed = false;
    }
    if (passed) {
        // 双窗并存：title/pageId 互异（D-1/F-04）
        if (reqs[0].title != "sample.md" || reqs[1].title != "b.md") {
            std::cerr << "md_viewer_probe: FAIL——标题非文件名（D-3）\n";
            passed = false;
        }
        if (reqs[0].pageId == reqs[1].pageId) {
            std::cerr << "md_viewer_probe: FAIL——双窗 pageId 冲突\n";
            passed = false;
        }
        if (reqs[0].width != 900 || reqs[0].height != 640 ||
            !reqs[0].resizable || reqs[0].positionSet) {
            std::cerr << "md_viewer_probe: FAIL——窗口规格偏离 D-3（900×640/"
                         "可缩放/系统默认位）\n";
            passed = false;
        }
    }

    auto composeFrame = [&](const app::DslWindowRequest& req) {
        probe::composeModuleFrame(
            runtime, window, req.pageId, args.windowW, args.windowH,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                req.compose(u, screen);
            });
    };

    if (passed) {
        // ① 渲染对照 + ② 无编辑入口（Idle）：两帧稳树
        composeFrame(reqs[0]);
        composeFrame(reqs[0]);
        if (ui == nullptr || ui->find("md.viewer.blk.0") == nullptr) {
            std::cerr << "md_viewer_probe: FAIL——块流未建树（渲染核接线断？）\n";
            passed = false;
        }
        if (ui->find(module::markdown::kEditorInputId) != nullptr) {
            std::cerr << "md_viewer_probe: FAIL——查看器树含编辑器 input"
                         "（只读构造性保证被破坏）\n";
            passed = false;
        }
        if (ui->find(module::markdown::kViewerSearchInputId) != nullptr) {
            std::cerr << "md_viewer_probe: FAIL——Idle 态搜索条不应在树\n";
            passed = false;
        }
    }

    if (passed) {
        // ③ 搜索键盘全流程（窗口级 onKeyEvent 直驱；真实焦点消费序归 UAT）
        reqs[0].onKeyEvent(pressKey(eui::InputKey::F, /*control=*/true));
        composeFrame(reqs[0]);
        composeFrame(reqs[0]);  // 稳帧：搜索条进树
        if (ui->find(module::markdown::kViewerSearchInputId) == nullptr) {
            std::cerr << "md_viewer_probe: FAIL——Ctrl+F 后搜索条未在树\n";
            passed = false;
        }
        // 0/0：无词 Enter 无操作——offset 回读不变
        const float offsetBefore =
            ui->find(module::markdown::kViewerScrollId)->scrollOffset;
        reqs[0].onKeyEvent(pressKey(eui::InputKey::Enter));
        composeFrame(reqs[0]);
        const float offsetIdle =
            ui->find(module::markdown::kViewerScrollId)->scrollOffset;
        if (offsetIdle != offsetBefore) {
            std::cerr << "md_viewer_probe: FAIL——0/0 Enter 移动了视图（PRD 硬边界）\n";
            passed = false;
        }
        // 注入词（与输入框 onChange 同一条 ApplyQuery 通路）→ 高亮矩形在树
        module::markdown::viewerSearchInjectForTest(samplePath, "指标");
        composeFrame(reqs[0]);
        if (ui->find("md.viewer.hit.0.0") == nullptr) {
            std::cerr << "md_viewer_probe: FAIL——命中矩形未绘制（查询缝断？）\n";
            passed = false;
        }
        // Enter ×4：逐跳 scrollOffset 回读单调推进（首跳允许原地，之后必须推进）
        float prev = -1.0f;
        for (int step = 0; step < 4 && passed; ++step) {
            reqs[0].onKeyEvent(pressKey(eui::InputKey::Enter));
            composeFrame(reqs[0]);
            const float off =
                ui->find(module::markdown::kViewerScrollId)->scrollOffset;
            if (off < prev) {
                std::cerr << "md_viewer_probe: FAIL——第 " << step + 1
                          << " 跳 offset 回退（" << prev << " → " << off << "）\n";
                passed = false;
            }
            prev = off;
        }
        // Shift+Enter 回退：offset 不增
        if (passed) {
            reqs[0].onKeyEvent(pressKey(eui::InputKey::Enter, false, /*shift=*/true));
            composeFrame(reqs[0]);
            const float off =
                ui->find(module::markdown::kViewerScrollId)->scrollOffset;
            if (off > prev) {
                std::cerr << "md_viewer_probe: FAIL——Shift+Enter 反向推进（" << off
                          << " > " << prev << "）\n";
                passed = false;
            }
        }
        // Esc：收起回阅读态（搜索条与高亮全清）
        if (passed) {
            reqs[0].onKeyEvent(pressKey(eui::InputKey::Escape));
            composeFrame(reqs[0]);
            if (ui->find(module::markdown::kViewerSearchInputId) != nullptr ||
                ui->find("md.viewer.hit.0.0") != nullptr) {
                std::cerr << "md_viewer_probe: FAIL——Esc 后未回到阅读态\n";
                passed = false;
            }
        }
    }

    if (passed) {
        // ④ 双窗各自 compose 独立建树（b.md 窗内容在自身 pageId 下可达）
        composeFrame(reqs[1]);
        composeFrame(reqs[1]);
        if (ui->find("md.viewer.blk.0") == nullptr) {
            std::cerr << "md_viewer_probe: FAIL——第二窗 compose 未建树\n";
            passed = false;
        }
        // ⑤ 缺文件窗：错误占位在树（流程 B7）
        composeFrame(reqs[2]);
        composeFrame(reqs[2]);
        if (ui->find("md.viewer.placeholder") == nullptr) {
            std::cerr << "md_viewer_probe: FAIL——缺文件窗无错误占位\n";
            passed = false;
        }
    }

    // --screenshot 落图（渲染对照素材；未传 = 零开销）
    probe::captureIfRequested(args, window, runtime, &passed);

    std::error_code cleanupEc;
    std::filesystem::remove_all(dir, cleanupEc);
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "md_viewer_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}
