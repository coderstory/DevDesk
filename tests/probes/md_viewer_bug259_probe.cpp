// md_viewer_bug259_probe.cpp — 池 bug#259 专项取证探针（G/H 搜索高亮 + D 标题块高 +
// F 底部可达 + E mermaid 源码块 + 视口裁剪滚动带）。
//
// 运行：md_viewer_bug259_probe.exe [--shot-dir <目录>]（缺省 = evidence/bug259）
// 零参数跑也执行断言面（树级），加 --shot-dir 才落 PNG（进程内 glReadPixels，
// 与桌面截屏无关）。
//
// 断言面：
//   ① H/G 高亮存在：注入 query 后命中矩形元素在树（普通 + 当前两档都在树）；
//   ② 跳转到底（长文档 183 块级）：末块命中矩形随跳转出现（视口裁剪不吞跳转目标块）；
//   ③ 滚动 offset 回读单调推进（同 md_viewer_probe 契约）；
//   ④ mermaid 代码块按普通代码块建树（CodeFence 包裹在树）。
// 截图（--shot-dir 给定时）：短文档顶部 / hitword 首跳 / hitword 末跳（底） /
// CJK 命中 / 长文档顶 / 长文档中段 / 长文档底，共 7 张。
//
// 与 md_viewer_probe 同骨架（init → 窗口 → 帧循环 → 断言 → 多态捕获）。

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
        DslAppConfig{}.title("devdesk_md_viewer_bug259_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

std::filesystem::path makeSampleDir() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "devdesk_md_viewer_bug259_probe";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        // 短文档：# 1 / ## 1 短标题 + CJK + mermaid 块 + hitword/CJK 关键词 +
        // 末行 END-OF-FILE-MARKER（老板验收素材同构）
        std::ofstream f(dir / "short.md", std::ios::trunc);
        f << "# 1\n\n";
        f << "紧跟一号标题的段落文本，验证标题容器高度足够，不侵入上方内容。"
             "CJK 行宽测试：中文连续排布时折行与块高测量必须同源，否则相邻块互相重叠。\n\n";
        f << "## 1\n\n";
        f << "二号标题下的段落。标题渲染高度按实际字号排版测量，容器包裹高度"
             "与渲染同源后不再出现压线。\n\n";
        f << "普通段落，包含英文 hitword 与中文关键词命中词，供页内搜索高亮验证。"
             "hitword hitword hitword 三连命中用于检查多矩形绘制与折行场景。\n\n";
        f << "```mermaid\ngraph TD\n    A[开始] --> B{判断}\n"
             "    B -->|是| C[执行]\n    B -->|否| D[结束]\n```\n\n";
        f << "上方 mermaid 代码块应按普通代码块显示源码。\n\n";
        for (int i = 0; i < 8; ++i) {
            f << "## 中文段落压力区 "; f << (i + 1); f << "\n\n";
            f << "段落：在长文档滚动场景下，视口裁剪只建可视区附近的块，滚动总高"
                 "由几何表供给。这里出现命中词 hitword 供跨块搜索验证，中文与 "
                 "English 混排折行场景覆盖 CJK 高亮矩形。\n\n";
        }
        f << "### 三级标题收尾\n\n";
        f << "结尾段落：末尾标记 END-OF-FILE-MARKER 前的最后一句，含搜索目标 hitword。\n";
    }
    {
        // 长文档：46 节 ≈ 180+ 块（视口裁剪滚动带 + 底部可达断言素材）
        std::ofstream f(dir / "long.md", std::ios::trunc);
        f << "# 长文档压测\n\n";
        for (int i = 1; i <= 45; ++i) {
            f << "## 第"; f << i; f << "节 压力段落\n\n";
            f << "正文：滚动与缩放性能验证用长文档段落，中文与 English 混排，"
                 "包含足够的文字量使折行真实发生，让每块高度测量与 inline 布局"
                 "缓存都在热路径上。这一段刻意写到三行以上，确保 wrap 发生。\n\n";
            f << "- 列表项 A：第"; f << i; f << "节要点一，含代码词 `cacheHit`。\n";
            f << "- 列表项 B：第"; f << i; f << "节要点二。\n\n";
            f << "```cpp\n// block sample\nint compute(int x) { return x * "
              << i << " + 42; }\n```\n\n";
        }
        f << "## 文件末尾\n\n";
        f << "结尾段落：末尾标记 END-OF-FILE-MARKER，长文档滚动到底必须能完整看到本行。\n";
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

// 树内是否存在任意 md.viewer.hit.<k>.<i> 矩形元素（H：高亮存在性）
bool anyHitRect(eui::Ui& ui) {
    for (int k = 0; k < 400; ++k) {
        for (int i = 0; i < 16; ++i) {
            if (ui.find("md.viewer.hit." + std::to_string(k) + "." +
                        std::to_string(i)) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path shotDir = "evidence" / std::filesystem::path("bug259");
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shot-dir" && i + 1 < argc) {
            shotDir = argv[++i];
        }
    }

    auto args = probe::parseProbeArgs(argc, argv);
    (void)args;  // 仅兼容 harness 透传（--shot-dir 自行解析；--screenshot 不用）
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("md_viewer_bug259_probe", 1100, 900);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    bool passed = true;
    eui::Ui* ui = nullptr;
    const std::filesystem::path dir = makeSampleDir();
    const std::string shortPath = (dir / "short.md").string();
    const std::string longPath = (dir / "long.md").string();
    std::error_code mkEc;
    std::filesystem::create_directories(shotDir, mkEc);
    auto shot = [&](core::dsl::Runtime& rt, const std::string& name) {
        const std::filesystem::path out = shotDir / (name + ".png");
        const bool ok = probe::captureFramePng(window, rt, out.string());
        std::cout << (ok ? "[shot] " : "[shot FAIL] ") << out.string() << "\n";
        if (!ok) passed = false;
    };

    module::markdown::openViewerWindow(shortPath);
    module::markdown::openViewerWindow(longPath);
    auto reqs = app::consumeWindowRequests();
    if (reqs.size() != 2) {
        std::cerr << "bug259_probe: FAIL——窗口请求数 " << reqs.size() << " != 2\n";
        passed = false;
    }

    auto composeFrame = [&](const app::DslWindowRequest& req) {
        probe::composeModuleFrame(
            runtime, window, req.pageId, 1100, 900,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                req.compose(u, screen);
            });
    };
    auto settle = [&](const app::DslWindowRequest& req, int frames) {
        for (int i = 0; i < frames; ++i) composeFrame(req);
    };

    // ── 短文档窗 ────────────────────────────────────────────────
    if (passed) {
        settle(reqs[0], 4);
        // D/E/F 顶部素材：标题不压线 + mermaid 源码块建树 + 整文档高度
        if (ui->find("md.viewer.blk.0") == nullptr) {
            std::cerr << "bug259_probe: FAIL——短文档块流未建树\n";
            passed = false;
        }
        // E：mermaid 块按普通代码块建树（CodeFence 包裹在树 = 渲染通路不特判语言）
        bool mermaidBlockBuilt = false;
        for (int k = 0; k < 64; ++k) {
            if (ui->find("md.viewer.blk." + std::to_string(k)) != nullptr) {
                mermaidBlockBuilt = true;  // 逐块包裹树存在即渲染核不裁
            }
        }
        if (!mermaidBlockBuilt) {
            std::cerr << "bug259_probe: FAIL——短文档逐块包裹不在树\n";
            passed = false;
        }
        shot(runtime, "bug259-short-top");  // D + E +（窄窗下含 F）
    }
    if (passed) {
        // G/H：Ctrl+F 唤起（真用户路径：viewerHandleKey → activate）→ 注入 hitword
        reqs[0].onKeyEvent(pressKey(eui::InputKey::F, /*control=*/true));
        settle(reqs[0], 2);
        if (ui->find(module::markdown::kViewerSearchInputId) == nullptr) {
            std::cerr << "bug259_probe: FAIL——Ctrl+F 后搜索条不在树\n";
            passed = false;
        }
        if (!module::markdown::viewerSearchInjectForTest(shortPath, "hitword")) {
            std::cerr << "bug259_probe: FAIL——搜索注入未命中 session\n";
            passed = false;
        }
        settle(reqs[0], 8);
        if (!anyHitRect(*ui)) {
            std::cerr << "bug259_probe: FAIL——hitword 高亮矩形不在树（#259-H 高亮消失）\n";
            passed = false;
        }
        shot(runtime, "bug259-short-hitword-first");  // 普通档 + 当前档同屏
        // Enter ×6：逐跳推进到底（末处 hitword 在末行，F 同屏取证）
        float prev = -1.0f;
        for (int step = 0; step < 6 && passed; ++step) {
            reqs[0].onKeyEvent(pressKey(eui::InputKey::Enter));
            settle(reqs[0], 3);
            const float off =
                ui->find(module::markdown::kViewerScrollId)->scrollOffset;
            if (off < prev) {
                std::cerr << "bug259_probe: FAIL——第 " << step + 1
                          << " 跳 offset 回退（" << prev << " → " << off << "）\n";
                passed = false;
            }
            prev = off;
        }
        if (!anyHitRect(*ui)) {
            std::cerr << "bug259_probe: FAIL——跳到底后高亮矩形不在树（裁剪吞了目标块？）\n";
            passed = false;
        }
        shot(runtime, "bug259-short-hitword-last-bottom");  // G/H + F（END 标记）
        // CJK 命中域（同 query 通路换 CJK 词）
        if (!module::markdown::viewerSearchInjectForTest(shortPath, "命中词")) {
            std::cerr << "bug259_probe: FAIL——CJK 注入未命中 session\n";
            passed = false;
        }
        settle(reqs[0], 8);
        if (!anyHitRect(*ui)) {
            std::cerr << "bug259_probe: FAIL——CJK 高亮矩形不在树\n";
            passed = false;
        }
        shot(runtime, "bug259-short-cjk");
        reqs[0].onKeyEvent(pressKey(eui::InputKey::Escape));
        settle(reqs[0], 2);
    }

    // ── 长文档窗（183 块级视口裁剪）─────────────────────────────
    if (passed) {
        settle(reqs[1], 4);
        shot(runtime, "bug259-long-top");
        // 唤起搜索（真用户路径）→ 中段：跳「第20节」→ 裁剪滚动带中段
        reqs[1].onKeyEvent(pressKey(eui::InputKey::F, /*control=*/true));
        settle(reqs[1], 2);
        if (!module::markdown::viewerSearchInjectForTest(longPath, "第20节")) {
            std::cerr << "bug259_probe: FAIL——长文档注入未命中 session\n";
            passed = false;
        }
        if (!module::markdown::viewerSearchInjectForTest(longPath, "第20节")) {
            std::cerr << "bug259_probe: FAIL——长文档注入未命中 session\n";
            passed = false;
        }
        reqs[1].onKeyEvent(pressKey(eui::InputKey::Enter));
        settle(reqs[1], 6);
        shot(runtime, "bug259-long-mid");
        // 底部：END-OF-FILE-MARKER 唯一命中（F：滚到底可见文件末尾）
        if (!module::markdown::viewerSearchInjectForTest(longPath,
                                                         "END-OF-FILE-MARKER")) {
            std::cerr << "bug259_probe: FAIL——末标记注入未命中 session\n";
            passed = false;
        }
        reqs[1].onKeyEvent(pressKey(eui::InputKey::Enter));
        settle(reqs[1], 6);
        const float offBottom =
            ui->find(module::markdown::kViewerScrollId)->scrollOffset;
        if (offBottom <= 0.0f) {
            std::cerr << "bug259_probe: FAIL——长文档末标记跳转 offset 未推进\n";
            passed = false;
        }
        if (!anyHitRect(*ui)) {
            std::cerr << "bug259_probe: FAIL——长文档底部高亮矩形不在树（F/G 断）\n";
            passed = false;
        }
        shot(runtime, "bug259-long-bottom");  // F + G/H（底）
    }

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
    std::cout << "md_viewer_bug259_probe: PASS（截图目录 " << shotDir.string()
              << "）\n";
    return 0;
}
