// md_viewer_selection_probe.cpp — need264 拖选复制取证探针（选中即自动复制）。
//
// 运行：md_viewer_selection_probe.exe [--shot-dir <目录>]（缺省 = evidence/need264）
// 零参数跑也执行断言面（树级 + 剪贴板），加 --shot-dir 才落 PNG（进程内
// glReadPixels，与桌面截屏无关）。
//
// 断言面（真实指针拖拽：probe::drag 经 core 输入队列 → MouseArea press/drag/
// release → viewerSelHitTest（渲染同源 displayCpForPoint）→ 剪贴板）：
//   ① 跨块拖选（块0 顶 → 块1 底）：md.viewer.sel.* 矩形在树（块1 折行 ≥ 2 矩形）
//     + 剪贴板 == displayTextOf(块0) + "\n" + displayTextOf(块1)（CJK/emoji/
//     折行/标记符剥离逐字节核对）；
//   ② 空选择边界：选区内单击（无拖动）→ 选区矩形清空 + 剪贴板保持哨兵不写入；
//   ③ Ctrl+C 兼容通道：重选后覆盖剪贴板为哨兵 → 窗口级 Ctrl+C → 剪贴板 == 选区文本；
//   ④ 截图：浅色/深色主题选区渲染各一张（像素核对 = 目视 + 选区为 info 蓝低
//     透明叠加，不盖字）。
//
// 与 md_viewer_bug259_probe 同骨架（init → runtime.initialize → 窗口 → 帧循环 →
// 断言 → 多态捕获）；runtime.initialize 为指针派发所必需（todo_drag_probe 先例）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "core/input/input_types.h"
#include "core/theme.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_viewer_window.h"

#include <components/markdown.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// app:: 层桩（同 markdown_probe：composeModuleFrame 不经 app::compose）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_md_viewer_selection_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

constexpr float kW = 1100.0f;
constexpr float kH = 900.0f;

std::filesystem::path makeSampleDir() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "devdesk_md_viewer_selection_probe";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        // 块0 = 标题（短行，含 emoji）；块1 = 长中文段落（窗口宽下必折 ≥ 2 行）；
        // 块2 = 英文段落；块3 = 含中文注释 fenced code（P1 回归素材：码点计账）；
        // 块4 = 列表（多行块组）——供后续人工核对素材。
        std::ofstream f(dir / "sel.md", std::ios::trunc);
        f << "# need264 选区验证 🎉\n\n";
        f << "中文段落用于拖选主断言：本段刻意写到足够长确保在探针窗宽下真实折行，"
             "出现第二行与第三行带，跨行拖选的选区矩形必须逐行产出且不吞字，"
             "中文连续排布时码点域切片不得出现半字或乱码，emoji ✅ 也按整字处理。\n\n";
        f << "English paragraph for cross-block drag: the selection endpoint "
             "clamps to the last codepoint of this block when released below "
             "its final line.\n\n";
        f << "```cpp\n";
        f << "// 中文注释：选区计账必须按码点，字节计会越位\n";
        f << "int x = 42;  // 行尾注解 值=42\n";
        f << "```\n\n";
        f << "- 列表项甲\n- 列表项乙\n";
    }
    return dir;
}

// 期望选区文本 = 块显示文本（与 viewerSelText 同一条 displayTextOf 域）
std::string displayOf(const std::string& blockSource) {
    return components::MarkdownBuilder::displayTextOf(blockSource);
}

bool anySelRect(eui::Ui& ui) {
    for (int k = 0; k < 64; ++k) {
        for (int i = 0; i < 64; ++i) {
            if (ui.find("md.viewer.sel." + std::to_string(k) + "." +
                        std::to_string(i)) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

int selRectCountForBlock(eui::Ui& ui, int k) {
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        if (ui.find("md.viewer.sel." + std::to_string(k) + "." + std::to_string(i)) !=
            nullptr) {
            ++n;
        }
    }
    return n;
}

eui::KeyEvent pressKey(eui::InputKey key, bool control = false) {
    eui::KeyEvent ev{};
    ev.key = key;
    ev.action = eui::KeyAction::Press;
    ev.modifiers.control = control;
    return ev;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path shotDir =
        "evidence" / std::filesystem::path("need264");
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shot-dir" && i + 1 < argc) {
            shotDir = argv[++i];
        }
    }

    auto args = probe::parseProbeArgs(argc, argv);
    (void)args;
    core::theme::setLight();  // 首段 = 浅色主题取证（core::theme 默认深色）
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("md_viewer_selection_probe", int(kW), int(kH));
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);  // 指针派发所必需（todo_drag_probe 先例）
    bool passed = true;
    eui::Ui* ui = nullptr;
    const std::filesystem::path dir = makeSampleDir();
    const std::string docPath = (dir / "sel.md").string();
    std::error_code mkEc;
    std::filesystem::create_directories(shotDir, mkEc);
    auto shot = [&](core::dsl::Runtime& rt, const std::string& name) {
        const std::filesystem::path out = shotDir / (name + ".png");
        const bool ok = probe::captureFramePng(window, rt, out.string());
        std::cout << (ok ? "[shot] " : "[shot FAIL] ") << out.string() << "\n";
        if (!ok) passed = false;
    };

    // 期望文本面：块模型与查看器同源（MdBlockModel），块 0/1 的显示文本
    module::markdown::MdBlockModel model;
    std::string docBody;
    {
        std::ifstream f(dir / "sel.md", std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        docBody = ss.str();
    }
    model.update(docBody);
    const auto& blocks = model.blocks();
    if (blocks.size() < 4) {
        std::cerr << "selection_probe: FAIL——样例块数 " << blocks.size() << " < 4\n";
        probe::terminateWindowBackend();
        return 3;
    }
    const std::string expSel =
        displayOf(docBody.substr(blocks[0].start, blocks[0].end - blocks[0].start)) +
        "\n" +
        displayOf(docBody.substr(blocks[1].start, blocks[1].end - blocks[1].start));

    module::markdown::openViewerWindow(docPath);
    auto reqs = app::consumeWindowRequests();
    if (reqs.size() != 1) {
        std::cerr << "selection_probe: FAIL——窗口请求数 " << reqs.size() << " != 1\n";
        probe::terminateWindowBackend();
        return 3;
    }

    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, reqs[0].pageId, kW, kH,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                reqs[0].compose(u, screen);
            });
    };
    auto settle = [&](int frames) {
        for (int i = 0; i < frames; ++i) composeFrame();
    };
    auto clipboard = [&]() { return core::window::clipboardText(window); };

    // 块包裹帧（窗口坐标，dpiScale=1 刻意逻辑=物理）：md.viewer.blk.<stableId>
    // 按下标扫描，取前两个（滚动 0 = 块 0/1 可见）。
    auto blockFrame = [&](size_t idx) -> core::LayoutRect {
        const std::string id = "md.viewer.blk." + std::to_string(idx);
        const core::dsl::Element* el = ui != nullptr ? ui->find(id) : nullptr;
        if (el == nullptr) return {};
        return el->frame;
    };

    // ── ① 跨块拖选：块0 顶左 → 块1 底右（钳制语义 = 两块全选）────────────
    settle(4);
    const core::LayoutRect f0 = blockFrame(0);
    const core::LayoutRect f1 = blockFrame(1);
    if (f0.width <= 0.0f || f1.width <= 0.0f) {
        std::cerr << "selection_probe: FAIL——块 0/1 包裹不在树（f0=" << f0.width
                  << "x" << f0.height << " f1=" << f1.width << "x" << f1.height
                  << "）\n";
        probe::terminateWindowBackend();
        return 3;
    }
    core::window::setClipboardText("CLIPBOARD-SENTINEL-BEFORE-DRAG");
    probe::drag(window, runtime,
                double(f0.x + 2.0f), double(f0.y + 1.0f),
                double(f1.x + f1.width - 6.0f), double(f1.y + f1.height - 1.0f),
                /*steps=*/12);
    settle(3);
    if (!anySelRect(*ui)) {
        std::cerr << "selection_probe: FAIL——拖选后选区矩形不在树\n";
        passed = false;
    }
    const int selLineRects1 = selRectCountForBlock(*ui, 1);
    if (selLineRects1 < 2) {
        std::cerr << "selection_probe: FAIL——块1 折行选区矩形数 " << selLineRects1
                  << " < 2（跨行带缺失）\n";
        passed = false;
    }
    const std::string got = clipboard();
    if (got != expSel) {
        std::cerr << "selection_probe: FAIL——剪贴板内容不符\n"
                  << "  expect(" << expSel.size() << "B)=" << expSel << "\n"
                  << "  got  (" << got.size() << "B)=" << got << "\n";
        passed = false;
    }
    if (passed) {
        std::cout << "[ok] 跨块拖选：sel 矩形在树（块1 折行带 "
                  << selLineRects1 << " 条）+ 剪贴板逐字节一致（"
                  << got.size() << "B）\n";
    }
    if (passed) shot(runtime, "need264-sel-light");  // 浅色主题选区渲染

    // ── ①b 含中文注释代码块拖选（P1 回归：Code 块码点计账）──────────────
    // fence 块整块全选 → 剪贴板 == 该块显示文本逐字节（display 域含中文注释与
    // lang 行；字节计账时 hit.cp 膨胀为字节偏移 → 切片越尾/错位即红）。
    if (passed) {
        size_t codeBlk = static_cast<size_t>(-1);
        for (size_t i = 0; i < blocks.size(); ++i) {
            const std::string src = docBody.substr(blocks[i].start,
                                                   blocks[i].end - blocks[i].start);
            if (src.find("```") != std::string::npos) {
                codeBlk = i;
                break;
            }
        }
        if (codeBlk == static_cast<size_t>(-1)) {
            std::cerr << "selection_probe: FAIL——样例无 code fence 块\n";
            passed = false;
        } else {
            const core::LayoutRect fc = blockFrame(codeBlk);
            if (fc.width <= 0.0f) {
                std::cerr << "selection_probe: FAIL——code 块包裹不在树\n";
                passed = false;
            } else {
                core::window::setClipboardText("CLIPBOARD-SENTINEL-BEFORE-CODEDRAG");
                probe::drag(window, runtime,
                            double(fc.x + 2.0f), double(fc.y + 1.0f),
                            double(fc.x + fc.width - 6.0f),
                            double(fc.y + fc.height - 1.0f), 10);
                settle(3);
                const std::string expCode = displayOf(
                    docBody.substr(blocks[codeBlk].start,
                                   blocks[codeBlk].end - blocks[codeBlk].start));
                const std::string gotCode = clipboard();
                if (gotCode != expCode) {
                    std::cerr << "selection_probe: FAIL——code 块剪贴板不符\n"
                              << "  expect(" << expCode.size() << "B)=" << expCode
                              << "\n  got  (" << gotCode.size() << "B)=" << gotCode
                              << "\n";
                    passed = false;
                } else {
                    std::cout << "[ok] code 块（中文注释）拖选：剪贴板逐字节一致（"
                              << gotCode.size() << "B）\n";
                }
            }
        }
    }

    // ── ② 空选择边界：选区内单击（无拖动）→ 清选区 + 剪贴板不动 ──────────
    if (passed) {
        core::window::setClipboardText("CLIPBOARD-SENTINEL-BEFORE-CLICK");
        probe::click(window, runtime, double(f0.x + 2.0f), double(f0.y + 1.0f));
        settle(2);
        if (anySelRect(*ui)) {
            std::cerr << "selection_probe: FAIL——单击后选区矩形未清空\n";
            passed = false;
        }
        if (clipboard() != "CLIPBOARD-SENTINEL-BEFORE-CLICK") {
            std::cerr << "selection_probe: FAIL——空选择写入了剪贴板（got "
                      << clipboard().size() << "B）\n";
            passed = false;
        }
        if (passed) std::cout << "[ok] 空选择：矩形清空 + 剪贴板保持哨兵\n";
    }

    // ── ③ Ctrl+C 兼容通道 ────────────────────────────────────────────────
    if (passed) {
        probe::drag(window, runtime,
                    double(f0.x + 2.0f), double(f0.y + 1.0f),
                    double(f1.x + f1.width - 6.0f),
                    double(f1.y + f1.height - 1.0f), 8);
        settle(2);
        core::window::setClipboardText("CLIPBOARD-SENTINEL-BEFORE-CTRLC");
        reqs[0].onKeyEvent(pressKey(eui::InputKey::C, /*control=*/true));
        settle(2);
        if (clipboard() != expSel) {
            std::cerr << "selection_probe: FAIL——Ctrl+C 后剪贴板不符（got "
                      << clipboard().size() << "B）\n";
            passed = false;
        } else {
            std::cout << "[ok] Ctrl+C 兼容通道：剪贴板恢复为选区文本\n";
        }
    }

    // ── ④ 深色主题选区渲染截图（重选一次保选区在场）─────────────────────
    if (passed) {
        core::theme::setDark();
        settle(6);
        if (!anySelRect(*ui)) {
            probe::drag(window, runtime,
                        double(f0.x + 2.0f), double(f0.y + 1.0f),
                        double(f1.x + f1.width - 6.0f),
                        double(f1.y + f1.height - 1.0f), 8);
            settle(3);
        }
        shot(runtime, "need264-sel-dark");
        core::theme::setLight();
        settle(2);
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
    std::cout << "md_viewer_selection_probe: PASS（截图目录 " << shotDir.string()
              << "）\n";
    return 0;
}
