// md_layout_collapse_probe.cpp — bug149/bug162 复现探针（模块级离屏真窗）
//
// 运行：md_layout_collapse_probe [--size WxH] [--out <目录>]
// 场景（全部走真实点击路径，不直调私有成员）：
//   A. 打开笔记 → 编辑列几何取证（编辑区 y/h、tab 带、树列对齐）+ 截图
//   B. 点边界把手 → 树面板折叠/再展开（md.tree 宽度 224→0→224）+ 截图
//   C. 点文件夹折叠命中区 → 子树行消失/复现（collapsedDirs_ 链路）
// 临时笔记目录经 DEVDESK_NOTES_DIR 隔离，析构整棵删除，不触真实用户数据。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_md_layout_collapse_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

extern "C" ::core::Module* create_markdown_module();

namespace {

namespace fs = std::filesystem;

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;

    // 临时笔记目录（onStart 前 setenv，paths::markdownDir 覆盖优先级最高）
    char tmpl[] = "/tmp/md_layout_probe_notes_XXXXXX";
    const char* made = mkdtemp(tmpl);
    if (made == nullptr) return 2;
    const fs::path notesDir = made;
    test_support::setEnvVar("DEVDESK_NOTES_DIR", notesDir.string());
    writeFile(notesDir / "Alpha.md", "# Alpha\n\n第一段。\n\n## 小节\n\n正文。\n");
    writeFile(notesDir / "Sub" / "Beta.md", "# Beta\n\n子目录笔记。\n");

    auto window = probe::makeWindow("md_layout_collapse_probe",
                                    args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

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

    struct FR {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    };
    auto frameOf = [&](const char* id, FR& out) -> bool {
        const core::dsl::Element* e = ui != nullptr ? ui->find(id) : nullptr;
        if (e == nullptr) return false;
        out.x = e->frame.x;
        out.y = e->frame.y;
        out.width = e->frame.width;
        out.height = e->frame.height;
        return true;
    };
    auto printFrame = [&](const char* id) {
        FR r{};
        if (frameOf(id, r)) {
            const core::dsl::Element* e = ui->find(id);
            std::cout << "  frame " << id << " = (" << r.x << "," << r.y
                      << ") " << r.width << "x" << r.height << " z="
                      << e->zIndex << " subtreeMaxZ=" << e->subtreeMaxZIndex
                      << "\n";
        } else {
            std::cout << "  frame " << id << " = <缺失>\n";
        }
    };
    auto clickElement = [&](const core::dsl::Element& e) {
        const float cx = e.frame.x + e.frame.width * 0.5f;
        const float cy = e.frame.y + e.frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    auto clickById = [&](const char* id) -> bool {
        const core::dsl::Element* e = ui != nullptr ? ui->find(id) : nullptr;
        if (e == nullptr) {
            std::cerr << "  clickById: " << id << " 不存在\n";
            return false;
        }
        clickElement(*e);
        return true;
    };
    // 行存在性扫描（node.0..node.15）
    auto nodeCount = [&]() -> int {
        int n = 0;
        for (int i = 0; i < 16; ++i) {
            const std::string id = "md.tree.node." + std::to_string(i);
            if (ui->find(id) == nullptr) break;
            ++n;
        }
        return n;
    };

    composeFrame();
    composeFrame();

    std::cout << "== A. 空态帧几何 ==\n";
    if (const core::dsl::Element* root = ui->find("md.root")) {
        std::cout << "  md.root children:\n";
        for (const auto& c : root->children) {
            std::cout << "    child id=\"" << c->id << "\" frame=("
                      << c->frame.x << "," << c->frame.y << ") "
                      << c->frame.width << "x" << c->frame.height
                      << "\n";
        }
    }
    printFrame("md.root");
    printFrame("md.header");
    printFrame("md.h1");
    printFrame("md.mode");
    printFrame("md.tree");
    printFrame("md.tree.handle.bg");
    printFrame("md.body");

    // —— A. 打开笔记（点文件行 Alpha = node.1：node.0 根目录/Sub 文件夹）——
    if (!clickById("md.tree.node.1.row")) {
        std::cerr << "md_layout_collapse_probe: 文件行 md.tree.node.1.row 缺失"
                     "（rows=" << nodeCount() << "）\n";
        passed = false;
    }
    composeFrame();
    composeFrame();

    std::cout << "== A. 开笔记后帧几何 ==\n";
    printFrame("md.tree");
    printFrame("md.note.tabs");
    printFrame("md.editor");
    printFrame("md.editor.input");
    printFrame("md.editor.shell");

    FR input{};
    if (!frameOf("md.editor.input", input)) {
        std::cerr << "md_layout_collapse_probe: 编辑器输入框未出现"
                     "（点击文件行未打开笔记？）\n";
        passed = false;
    } else {
        // 代码预期几何：body 顶 y=48、tab 带 36、chip 区 48 → 输入框顶
        // y=132（bug149 修复后值，与窗高无关）；输入框底 = 列底(窗高-32)
        // - 30 = 窗高-62（800 窗→738、1000 窗→938）。原写死 738 只对 800
        // 窗成立，ctest 缺省 1200x1000 会误报 bug149（2026-11-07 改随窗高
        // 推导，bug210 波及面验证时发现）。
        const float expectY = 132.0f;
        const float expectBottom = static_cast<float>(args.windowH) - 62.0f;
        if (std::fabs(input.y - expectY) > 2.0f) {
            std::cerr << "bug149 嫌疑: 编辑区 y=" << input.y << " 预期≈"
                      << expectY << "（与目录树顶对齐关系破坏？）\n";
            passed = false;
        }
        if (input.y + input.height > expectBottom + 2.0f ||
            input.height < 100.0f) {
            std::cerr << "bug149 嫌疑: 编辑区 h=" << input.height << " 底="
                      << input.y + input.height << " 预期底≈" << expectBottom
                      << "（编辑器下沉/被压底？）\n";
            passed = false;
        }
    }

    fs::path outDir = "md_probe_shots";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc)
            outDir = argv[i + 1];
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (!probe::captureFramePng(window, runtime,
                                (outDir / "a-open.png").string())) {
        passed = false;
    }

    // —— B. 树面板折叠/展开（边界把手，lerp 40 帧收敛）——
    // 诊断：把手中心横向扫描命中查询（只读，不写 hover 缓存）
    {
        FR handleBg{};
        frameOf("md.tree.handle.bg", handleBg);
        const float hx = handleBg.x + handleBg.width * 0.5f;
        const float hy = handleBg.y + handleBg.height * 0.5f;
        for (float dx : {-10.0f, -6.0f, -2.0f, 0.0f, 2.0f, 6.0f, 10.0f}) {
            std::cout << "  hitTest(" << hx + dx << "," << hy << ") = \""
                      << runtime.hitTestInteractiveAt(hx + dx, hy, 1.0f)
                      << "\"\n";
        }
    }
    FR tree{};
    frameOf("md.tree", tree);
    const float wOpen = tree.width;
    clickById("md.tree.handle.bg");
    for (int i = 0; i < 40; ++i) composeFrame();
    frameOf("md.tree", tree);
    const float wClosed = tree.width;
    std::cout << "== B. 把手折叠: md.tree 宽 " << wOpen << " -> " << wClosed
              << " ==\n";
    printFrame("md.tree.handle.bg");
    if (wClosed > 2.0f) {
        std::cerr << "bug162 嫌疑: 点击把手后树面板宽 " << wClosed
                  << " 未收拢（折叠失效）\n";
        passed = false;
    }
    if (!probe::captureFramePng(window, runtime,
                                (outDir / "b-collapsed.png").string())) {
        passed = false;
    }
    clickById("md.tree.handle.bg");
    for (int i = 0; i < 40; ++i) composeFrame();
    frameOf("md.tree", tree);
    std::cout << "== B. 把手展开: md.tree 宽 = " << tree.width << " ==\n";
    if (std::fabs(tree.width - wOpen) > 2.0f) {
        std::cerr << "bug162 嫌疑: 再次点击把手后宽 " << tree.width
                  << " != 展开宽 " << wOpen << "（展开失效）\n";
        passed = false;
    }

    // —— C. 文件夹折叠/展开（目录行命中区；node.2=Sub、node.3=Beta）——
    const int rowsBefore = nodeCount();
    clickById("md.tree.node.2.toggle");
    composeFrame();
    composeFrame();
    const int rowsCollapsed = nodeCount();
    std::cout << "== C. 目录折叠: rows " << rowsBefore << " -> "
              << rowsCollapsed << " ==\n";
    if (rowsCollapsed >= rowsBefore) {
        std::cerr << "bug162 嫌疑: 点击目录折叠命中区后行数 " << rowsCollapsed
                  << " 未减少（目录折叠失效）\n";
        passed = false;
    }
    if (!probe::captureFramePng(window, runtime,
                                (outDir / "c-folder-collapsed.png").string())) {
        passed = false;
    }
    clickById("md.tree.node.2.toggle");
    composeFrame();
    composeFrame();
    const int rowsRestored = nodeCount();
    std::cout << "== C. 目录展开: rows = " << rowsRestored << " ==\n";
    if (rowsRestored != rowsBefore) {
        std::cerr << "bug162 嫌疑: 再次点击后行数 " << rowsRestored
                  << " != " << rowsBefore << "（目录展开失效）\n";
        passed = false;
    }

    // —— D. tooltip 锚定（bug163）：问号钮气泡须钉页头正下方、中心贴近按钮 ——
    // 气泡 560 宽右贴边 clamp 后中心与按钮中心天然差 ~24px（指针尖才对准按钮），
    // 阈值 40 容忍布局级抖动、仍能抓住锚点漂移（修前实测偏差 ~56px）。
    clickById("md.help.icon.bg");  // Press+Release 后指针留驻源上 → hover 保持
    for (int i = 0; i < 3; ++i) composeFrame();
    FR helpBtnF{}, helpTipF{};
    const bool dHave = frameOf("md.help.icon", helpBtnF) &&
                       frameOf("md.help.icon.tooltip", helpTipF);
    std::cout << "== D. tooltip 锚定: help=(" << helpBtnF.x << ","
              << helpBtnF.y << ") " << helpBtnF.width << "x" << helpBtnF.height
              << " tip=(" << helpTipF.x << "," << helpTipF.y << ") "
              << helpTipF.width << "x" << helpTipF.height << " ==\n";
    if (!dHave) {
        std::cerr << "bug163: md.help.icon / md.help.icon.tooltip 缺帧\n";
        passed = false;
    } else {
        const float btnCx = helpBtnF.x + helpBtnF.width * 0.5f;
        const float tipCx = helpTipF.x + helpTipF.width * 0.5f;
        if (std::fabs(helpTipF.y - 44.0f) > 2.0f) {
            std::cerr << "bug163: tooltip y=" << helpTipF.y
                      << " 应钉页头正下 44\n";
            passed = false;
        }
        if (std::fabs(tipCx - btnCx) > 40.0f) {
            std::cerr << "bug163: tooltip 中心 " << tipCx << " 偏离按钮中心 "
                      << btnCx << " >40（锚定漂移）\n";
            passed = false;
        }
    }
    if (!probe::captureFramePng(window, runtime,
                                (outDir / "d-tooltip-hover.png").string())) {
        passed = false;
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    std::error_code rmEc;
    fs::remove_all(notesDir, rmEc);

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "md_layout_collapse_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}
