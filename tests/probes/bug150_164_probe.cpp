// bug150_164_probe.cpp — bug150（tab 关闭 X 垂直居中）/ bug164（大纲面板内容驱动高度）
//
// 运行：bug150_164_probe [--size WxH] [--out <目录>]
// 场景（真实点击路径）：
//   A. fontScale 1.3 打开笔记 → 文件 tab 关闭 X 图标盒须与 24px hover stack 重合
//      （bug150 修前：icon 盒用 scaled(24)，非 100% 档行盒漂移 12·(s-1) px → 断言红）
//   B. fontScale 1.0 大纲面板三态高度：空态最小 / 少内容=内容驱动 / 多内容=封顶
// 临时笔记目录经 DEVDESK_NOTES_DIR 隔离，析构整棵删除，不触真实用户数据。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_bug150_164_probe");
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

std::string manyHeadings() {
    std::string text = "# Many\n\n";
    for (int i = 1; i <= 30; ++i) {
        text += "## 节" + std::to_string(i) + "\n\n正文。\n\n";
    }
    return text;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;

    char tmpl[] = "/tmp/bug150_164_probe_notes_XXXXXX";
    const char* made = mkdtemp(tmpl);
    if (made == nullptr) return 2;
    const fs::path notesDir = made;
    setenv("DEVDESK_NOTES_DIR", notesDir.string().c_str(), 1);
    writeFile(notesDir / "Alpha.md", "# Alpha\n\n第一段。\n\n## 小节\n\n正文。\n");
    writeFile(notesDir / "Sub" / "Beta.md", manyHeadings());  // 30 标题=多内容态

    auto window = probe::makeWindow("bug150_164_probe",
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
    float moduleScreenH = 0.0f;  // 模块所见 screen 高（≠窗高，含 chrome 扣减）
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "markdown", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                moduleScreenH = screen.height;
                mod->compose(u, screen);
            });
    };

    struct FR {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    };
    auto frameOf = [&](const std::string& id, FR& out) -> bool {
        const core::dsl::Element* e = ui != nullptr ? ui->find(id) : nullptr;
        if (e == nullptr) return false;
        out.x = e->frame.x;
        out.y = e->frame.y;
        out.width = e->frame.width;
        out.height = e->frame.height;
        return true;
    };
    auto clickElement = [&](const core::dsl::Element& e) {
        const float cx = e.frame.x + e.frame.width * 0.5f;
        const float cy = e.frame.y + e.frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    // 依次试多个候选 id（组件内部子节点命名可能带后缀），点到即止
    auto clickFirst = [&](std::initializer_list<const char*> ids) -> bool {
        for (const char* id : ids) {
            const core::dsl::Element* e = ui != nullptr ? ui->find(id) : nullptr;
            if (e != nullptr) {
                clickElement(*e);
                return true;
            }
        }
        std::cerr << "  clickFirst: 全部候选缺失\n";
        return false;
    };
    auto expect = [&](bool cond, const std::string& tag) {
        if (!cond) {
            std::cerr << "  [红] " << tag << "\n";
            passed = false;
        } else {
            std::cout << "  [绿] " << tag << "\n";
        }
    };
    fs::path outDir = "bug150_164_shots";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc)
            outDir = argv[i + 1];
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    auto shot = [&](const std::string& name) {
        if (!probe::captureFramePng(window, runtime, (outDir / name).string())) {
            passed = false;
        }
    };

    // ============ B1. bug164 空态（未开笔记，fontScale 1.0）============
    std::cout << "== B1. 大纲面板空态（fontScale 1.0，未开笔记）==\n";
    composeFrame();
    composeFrame();
    if (!clickFirst({"md.outline.toggle", "md.outline.toggle.track",
                     "md.outline.toggle.bg", "md.outline.toggle.hit"})) {
        std::cerr << "  大纲开关缺失\n";
        passed = false;
    }
    composeFrame();
    composeFrame();
    const float panelMaxH = moduleScreenH - 120.0f;  // screenH - y(88) - 下距 32（与实现同式）
    FR panel{};
    if (!frameOf("md.outline.panel", panel)) {
        std::cerr << "  大纲面板未出现\n";
        passed = false;
    } else {
        std::cout << "  空态 panelH=" << panel.height << "（期望≈86）\n";
        expect(std::fabs(panel.height - 86.0f) <= 4.0f,
               "空态高度收最小（现 " + std::to_string(panel.height) + "）");
        expect(panel.height < panelMaxH - 100.0f,
               "空态不再恒满高（满高=" + std::to_string(panelMaxH) + "）");
    }
    shot("b-outline-empty.png");

    // ============ A. bug150：fontScale 1.3 下 tab 关闭 X 居中 ============
    std::cout << "== A. bug150 tab 关闭 X（fontScale 1.3）==\n";
    core::theme::setFontScale(1.3f);
    composeFrame();
    composeFrame();
    if (!clickFirst({"md.tree.node.1.row", "md.tree.node.0.row"})) {
        passed = false;
    }
    composeFrame();
    composeFrame();
    FR closeStack{}, closeIcon{};
    const bool have = frameOf("md.note.tab.0.close", closeStack) &&
                      frameOf("md.note.tab.0.close.icon", closeIcon);
    if (!have) {
        std::cerr << "  tab close 元素缺失（笔记未打开？）\n";
        passed = false;
    } else {
        const float stackCx = closeStack.x + closeStack.width * 0.5f;
        const float stackCy = closeStack.y + closeStack.height * 0.5f;
        const float iconCx = closeIcon.x + closeIcon.width * 0.5f;
        const float iconCy = closeIcon.y + closeIcon.height * 0.5f;
        std::cout << "  stack=(" << closeStack.x << "," << closeStack.y << ") "
                  << closeStack.width << "x" << closeStack.height
                  << "  icon=(" << closeIcon.x << "," << closeIcon.y << ") "
                  << closeIcon.width << "x" << closeIcon.height << "\n";
        expect(std::fabs(iconCy - stackCy) <= 1.0f,
               "X 图标垂直中心与 stack 中心偏差 "
                   + std::to_string(iconCy - stackCy) + " ≤1");
        expect(std::fabs(iconCx - stackCx) <= 1.0f,
               "X 图标水平中心与 stack 中心偏差 "
                   + std::to_string(iconCx - stackCx) + " ≤1");
        expect(closeIcon.height <= 25.0f && closeIcon.width <= 25.0f,
               "X 图标盒尺寸回归 24px 栈口径（现 "
                   + std::to_string(closeIcon.width) + "x"
                   + std::to_string(closeIcon.height) + "）");
    }
    shot("a-x-tab-130.png");

    // ============ B2. bug164 少内容（Alpha 2 标题，回 1.0）============
    std::cout << "== B2. 大纲面板少内容（fontScale 1.0）==\n";
    core::theme::setFontScale(1.0f);
    composeFrame();
    composeFrame();
    composeFrame();
    if (frameOf("md.outline.panel", panel)) {
        std::cout << "  少内容 panelH=" << panel.height << "（期望≈120）\n";
        expect(std::fabs(panel.height - 120.0f) <= 4.0f,
               "少内容高度=内容驱动（现 " + std::to_string(panel.height) + "）");
    } else {
        std::cerr << "  少内容态面板缺失\n";
        passed = false;
    }
    shot("c-outline-few.png");
    // 多内容：开 Beta（30 个标题）→ 内容 44+900+16=960 > 上限 → 封顶
    std::cout << "== B3. 大纲面板多内容封顶 ==\n";
    clickFirst({"md.tree.node.3.row", "md.tree.node.4.row"});
    composeFrame();
    composeFrame();
    if (frameOf("md.outline.panel", panel)) {
        std::cout << "  多内容 panelH=" << panel.height << "（上限="
                  << panelMaxH << "）\n";
        expect(std::fabs(panel.height - panelMaxH) <= 2.0f,
               "多内容封顶不超上限（现 " + std::to_string(panel.height) + "）");
    } else {
        std::cerr << "  多内容态面板缺失\n";
        passed = false;
    }
    shot("d-outline-many.png");

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
    std::cout << "bug150_164_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}
