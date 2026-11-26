// terminal_pointer_probe.cpp — 2026-11-26 老板实测两项终端指针缺陷的复现探针（先红后绿）
//
// 需求卡：docs/superpowers/plans/2026-11-26-terminal-pointer-interaction.md
// 症状：①终端页顶部 TAB 点了没反应；②选好文字后右键无法复制（选区还会不会丢）。
//
// 做法与 terminal_scrollbar_probe 同款：离屏 compose 真 TerminalPage + Fake 后端 +
// 真窗口，指针事件用 core::queuePointerMotion / core::queuePointerButton 注入（不依赖
// OS 指针），每帧 runtime.update 推进。注入坐标 = 元素 frame 中心 / GridLayout 折算，
// 因此断言不依赖「布局长什么样」，只依赖「点下去有没有反应」。
//
// 三段断言（缺一不可）：
//   A 标签可点击：从 term.tab.1.bg 几何中心注入「移动→按下(左)→松开(左)」，
//     controller.activeIndex() 应从 0 变 1。
//     A1b 是老板真手感的变体：按下后指针抖 3px 再松开（真实鼠标/触控板常态）。
//     EUI-NEO 默认 dragThreshold=2.0f（dsl.h:194），抖动 >2px → InteractionState
//     dragMoved=true → clicked=false（input_types.h:281）→ onClick 永不触发。
//     这条是「TAB 点不动」在代码层可复现的根因候选，修法见需求卡 Task 1（阈值 ≥6px）。
//   B 拖选 + 选即复制：网格第 0 行第 0 列拖到第 0 行第 5 列，页面应出现 term.sel.0，
//     且松开时 onCopyText 回调收到非空文本（选即复制，M2 Task 2）。
//   C 右键不清选区、能复制：有选区时注入右键按下+松开，选区应保持（左键语义只认
//     左键），且「右键=复制」或「弹出含复制项的菜单」至少其一成立（需求卡目标行为表）。
//     当前实现 onContextMenu 只置 ctxMenuOpen_（terminal_page.cpp:768），页面无任何
//     元素消费它 → 既没复制也没菜单，C2 应红。
//
// 红绿约定：A1b / C2 现在（修前）应为红；A1 / B1 / B2 / C1 为对照项（绿），
// 用来证明「不是探针整体失效」。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）

#include "module_terminal/terminal_page.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/terminal_settings.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（glfw_app_main.cpp 只编进主程序）；
// 探针直绘页面不走 app::compose，空实现仅为满足链接。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const app::DslAppConfig config =
        DslAppConfig{}.title("devdesk_term_pointer_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
    if (!ok) g_failures++;
}

std::string rectText(const core::dsl::Element* e) {
    if (e == nullptr) return "<无>";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.1f,%.1f %.1fx%.1f)", e->frame.x, e->frame.y,
                  e->frame.width, e->frame.height);
    return buf;
}

// 元素树扫描（诊断：右键后到底有没有菜单/选区元素，别让「找不到」变成静默）
void collectElements(const core::dsl::Element& node,
                     std::vector<const core::dsl::Element*>& out) {
    out.push_back(&node);
    for (const auto& child : node.children) {
        if (child) collectElements(*child, out);
    }
}

std::vector<const core::dsl::Element*> allElements(const eui::Ui& ui) {
    std::vector<const core::dsl::Element*> out;
    for (const core::dsl::Element* root : ui.orderedRoots()) {
        if (root != nullptr) collectElements(*root, out);
    }
    return out;
}

// id 含任一关键字的元素（诊断/断言用；菜单类元素是否存在靠它判定）
std::vector<const core::dsl::Element*> matchingElements(
    const eui::Ui& ui, const std::vector<std::string>& needles) {
    std::vector<const core::dsl::Element*> hits;
    for (const core::dsl::Element* e : allElements(ui)) {
        if (e->id.empty()) continue;
        for (const std::string& needle : needles) {
            if (e->id.find(needle) != std::string::npos) {
                hits.push_back(e);
                break;
            }
        }
    }
    return hits;
}

// 打印命中元素（零匹配就明确说零匹配）
std::string dumpMatching(const std::vector<const core::dsl::Element*>& hits) {
    std::string text;
    for (const core::dsl::Element* e : hits) {
        text += "\n    " + e->id + " " + rectText(e);
    }
    return text.empty() ? std::string("\n    <无匹配元素>") : text;
}

// 确定性假会话：24x80 全字母网格（cell (r,c) = 'a' + c%26），无回滚史。
// selectionText 按矩形拼文本，用来证明「选即复制」回调拿到了真内容。
class PointerFakeHandle : public platform::TerminalSessionHandle {
public:
    core::Result<void> write(const uint8_t*, int32_t, bool) override {
        return core::Result<void>::ok();
    }
    core::Result<void> resize(const platform::TerminalSize&) override {
        return core::Result<void>::ok();
    }
    void terminate() override {}
    bool exited(int32_t*) override { return false; }
    platform::TerminalFrame poll() override { return frame; }
    int pendingGeneration() override { return 1; }
    int scrollbackCount() override { return 0; }
    bool scrollbackLine(int, std::string&) override { return false; }
    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                       std::string& out) override {
        if (frame.cols <= 0 || frame.rows <= 0 || frame.cells.empty()) return false;
        if (rowStart < 0 || rowEnd < rowStart || rowEnd >= frame.rows) return false;
        std::string text;
        for (int r = rowStart; r <= rowEnd; ++r) {
            const int c0 = (r == rowStart) ? std::max(0, colStart) : 0;
            const int c1 =
                (r == rowEnd) ? std::min(frame.cols - 1, colEnd) : frame.cols - 1;
            if (c1 < c0) return false;
            for (int c = c0; c <= c1; ++c) {
                const uint32_t cp =
                    frame.cells[static_cast<size_t>(r) * frame.cols + c].cp;
                if (cp < 0x80) text.push_back(static_cast<char>(cp));
            }
            if (r != rowEnd) text.push_back('\n');
        }
        out = std::move(text);
        return !out.empty();
    }

    platform::TerminalFrame frame;
};

class PointerFakeBackend : public platform::TerminalBackend {
public:
    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&, const std::string&) override {
        auto handle = std::make_unique<PointerFakeHandle>();
        handle->frame.rows = 24;
        handle->frame.cols = 80;
        handle->frame.cursorRow = 0;
        handle->frame.cursorCol = 0;
        handle->frame.cells.resize(static_cast<size_t>(24) * 80);
        for (int r = 0; r < 24; ++r) {
            for (int c = 0; c < 80; ++c) {
                const uint32_t cp = static_cast<uint32_t>('a' + (c % 26));
                handle->frame.cells[static_cast<size_t>(r) * 80 + c] =
                    platform::TerminalCell{cp, 0, 0, 0, 1};
            }
        }
        ++spawnCount;
        return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::ok(
            std::move(handle));
    }
    bool available() const override { return true; }
    const char* unavailableReason() const override { return nullptr; }

    int spawnCount = 0;
};

// 单帧内完成一次「移动→按下→松开」（每步一帧：与真机事件节奏一致；同帧压栈也能
// 消费，但分帧后 InteractionState 的 hover/drag 判定才与真实交互同序）。
void pureClick(core::window::Handle window, core::dsl::Runtime& runtime, double x,
               double y) {
    const core::KeyModifiers noMods;
    core::queuePointerMotion(window, x, y, core::PointerButtons{}, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x, y, core::PointerButton::Left,
                             core::PointerAction::Press, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x, y, core::PointerButton::Left,
                             core::PointerAction::Release, noMods);
    probe::pollOnce(window, runtime);
}

// 按下 → 指针抖 (jitterPx, 0) → 松开。真实鼠标/触控板点一下就抖几像素；
// EUI-NEO dragThreshold=2px 时这会翻成 dragMoved=true，click 不再触发。
void jitterClick(core::window::Handle window, core::dsl::Runtime& runtime, double x,
                 double y, double jitterPx) {
    const core::KeyModifiers noMods;
    const core::PointerButtons held(core::PointerButton::Left);
    core::queuePointerMotion(window, x, y, core::PointerButtons{}, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x, y, core::PointerButton::Left,
                             core::PointerAction::Press, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerMotion(window, x + jitterPx, y, held, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x + jitterPx, y, core::PointerButton::Left,
                             core::PointerAction::Release, noMods);
    probe::pollOnce(window, runtime);
}

// 左键拖拽（按住后分段移动再松开；每段一帧，onMove 才能逐段推进 head）
void dragSelect(core::window::Handle window, core::dsl::Runtime& runtime, double x0,
                double y0, double x1, double y1, int steps) {
    const core::KeyModifiers noMods;
    const core::PointerButtons held(core::PointerButton::Left);
    core::queuePointerMotion(window, x0, y0, core::PointerButtons{}, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x0, y0, core::PointerButton::Left,
                             core::PointerAction::Press, noMods);
    probe::pollOnce(window, runtime);
    const int count = steps > 0 ? steps : 1;
    for (int step = 1; step <= count; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(count);
        core::queuePointerMotion(window, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, held,
                                 noMods);
        probe::pollOnce(window, runtime);
    }
    core::queuePointerButton(window, x1, y1, core::PointerButton::Left,
                             core::PointerAction::Release, noMods);
    probe::pollOnce(window, runtime);
}

// 右键按下+松开（分帧；EUI-NEO 的 onContextMenu 在 release 触发，见
// runtime_input.h:507——必须真的走完 release 才算测到右键链路）
void rightClick(core::window::Handle window, core::dsl::Runtime& runtime, double x,
                double y) {
    const core::KeyModifiers noMods;
    core::queuePointerMotion(window, x, y, core::PointerButtons{}, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x, y, core::PointerButton::Right,
                             core::PointerAction::Press, noMods);
    probe::pollOnce(window, runtime);
    core::queuePointerButton(window, x, y, core::PointerButton::Right,
                             core::PointerAction::Release, noMods);
    probe::pollOnce(window, runtime);
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("term_pointer_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 1;  // 金丝雀已建成 → 建窗失败是代码/配置问题，不是环境跳过
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    terminal::TerminalPage page;
    terminal::SessionController controller;
    PointerFakeBackend backend;
    controller.setBackend(&backend);
    controller.startSession();
    controller.startSession();  // 两个会话：TAB1 才有切换目标
    terminal::TerminalSettings settings;

    const float kW = static_cast<float>(args.windowW);
    const float kH = static_cast<float>(args.windowH);
    eui::Ui* uiPtr = nullptr;
    terminal::GridLayout layout{};
    // 选即复制出口：compose 每帧重新绑回调，捕获同一组计数器/文本
    int copyCount = 0;
    std::string copiedText;
    const auto composeFrame = [&] {
        probe::composeModuleFrame(runtime, window, "probe", kW, kH,
                                  [&](eui::Ui& ui, const eui::Screen& screen) {
                                      uiPtr = &ui;
                                      layout = page.compose(
                                          ui, screen, controller,
                                          [](const uint8_t*, int32_t) {},
                                          [](const std::string&, bool) {},
                                          [&](const std::string& text) {
                                              ++copyCount;
                                              copiedText = text;
                                          },
                                          settings);
                                  });
    };

    composeFrame();
    controller.setActive(0);
    composeFrame();
    check(controller.count() == 2 && controller.activeIndex() == 0,
          "S0 双会话在位且从 TAB0 起测（期望 count=2/active=0；实际 count=" +
              std::to_string(controller.count()) + "/active=" +
              std::to_string(controller.activeIndex()) + "）");
    check(layout.rows > 0 && layout.cellH > 0.0f && layout.cellW > 0.0f,
          "S1 网格度量可用（rows=" + std::to_string(layout.rows) + " cellW=" +
              std::to_string(layout.cellW) + " cellH=" + std::to_string(layout.cellH) +
              "）");

    // ── A. 标签可点击（老板报的「顶部 tab 无法点击」）──
    {
        const core::dsl::Element* tab0 = uiPtr->find("term.tab.0.bg");
        const core::dsl::Element* tab1 = uiPtr->find("term.tab.1.bg");
        check(tab0 != nullptr && tab1 != nullptr,
              "A0 TAB 命中区存在：term.tab.1.bg（实测 tab0=" + rectText(tab0) +
                  " tab1=" + rectText(tab1) + "）");
        if (tab1 != nullptr) {
            const double cx = tab1->frame.x + tab1->frame.width * 0.5;
            const double cy = tab1->frame.y + tab1->frame.height * 0.5;
            pureClick(window, runtime, cx, cy);
            composeFrame();
            const int nowActive = controller.activeIndex();
            check(nowActive == 1,
                  "A1 纯点击 TAB1 中心切换会话（期望 active=1；实际=" +
                      std::to_string(nowActive) + "，点(" + std::to_string(cx) + "," +
                      std::to_string(cy) + ")）");
        }
    }

    // A1b：真手感变体——按下后抖 3px 再松开。
    // 注意：A1 刚点过同一 TAB，页面 400ms 内的二次点击会被判为「双击改名」，
    // 因此先等过双击窗口，再测抖动（否则测到的是改名路径而不是拖拽阈值）。
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        controller.setActive(0);
        composeFrame();
        const core::dsl::Element* tab1 = uiPtr->find("term.tab.1.bg");
        if (tab1 != nullptr) {
            const double cx = tab1->frame.x + tab1->frame.width * 0.5;
            const double cy = tab1->frame.y + tab1->frame.height * 0.5;
            jitterClick(window, runtime, cx, cy, 3.0);
            composeFrame();
            const int nowActive = controller.activeIndex();
            check(nowActive == 1,
                  "A1b 按下后有 3px 抖动的点击仍切换 TAB（期望 active=1；实际=" +
                      std::to_string(nowActive) +
                      "；若为 0 先查双击改名窗口/拖拽阈值）");
        } else {
            // A0 已红，这里不再重复失败，但要让日志知道跳过了
            std::cout << "SKIP A1b（TAB1 元素不存在）\n";
        }
    }

    // ── B. 拖选 + 选即复制 ──
    controller.setActive(0);
    composeFrame();
    const core::dsl::Element* inputLayer = uiPtr->find("term.input");
    const float gridTop =
        inputLayer != nullptr ? inputLayer->frame.y : 0.0f;
    check(inputLayer != nullptr && layout.cellW > 0.0f && layout.cellH > 0.0f &&
              layout.rows > 0 && layout.cols > 5,
          "B0 网格交互层就位（term.input y=" + std::to_string(gridTop) + "，cols=" +
              std::to_string(layout.cols) + "）");
    if (inputLayer != nullptr && layout.cellW > 0.0f && layout.cellH > 0.0f &&
        layout.rows > 0 && layout.cols > 5) {
        // cell 中心：列 c → x=c*cellW+cellW/2；行 r → y=gridTop+r*cellH+cellH/2
        const double x0 = layout.cellW * 0.5;
        const double y0 = gridTop + layout.cellH * 0.5;
        const double x1 = layout.cellW * 5.5;
        const double y1 = y0;
        copyCount = 0;
        copiedText.clear();
        dragSelect(window, runtime, x0, y0, x1, y1, 8);
        composeFrame();
        const core::dsl::Element* sel0 = uiPtr->find("term.sel.0");
        check(sel0 != nullptr,
              "B1 第 0 行 0→5 列拖选出高亮块 term.sel.0（期望存在；实际=" +
                  rectText(sel0) + "）");
        check(copyCount > 0 && !copiedText.empty(),
              "B2 松开即复制：onCopyText 被调用且文本非空（期望 ≥1 次/非空；实际 " +
                  std::to_string(copyCount) + " 次，文本=\"" + copiedText + "\"）");
        if (sel0 == nullptr) {
            std::cout << "  诊断 选区相关元素:"
                      << dumpMatching(matchingElements(*uiPtr, {"term.sel"})) << "\n";
        }

        // ── C. 右键不清选区、能复制 ──
        // 右键落点 = 选区内部（第 0 行第 2 列），终端惯例在这里「复制不弹菜单」
        const double rx = layout.cellW * 2.5;
        const double ry = y0;
        const int copiesBefore = copyCount;
        rightClick(window, runtime, rx, ry);
        composeFrame();
        const core::dsl::Element* selAfter = uiPtr->find("term.sel.0");
        check(selAfter != nullptr,
              "C1 右键不破坏既有选区（期望 term.sel.0 仍在；实际=" +
                  rectText(selAfter) + "）");
        // 右键「能复制」：需求卡目标 = 有选区直接复制、不弹菜单；若走菜单路线，
        // 则页面必须真的出现菜单元素（当前 ctxMenuOpen_ 无任何元素消费）
        const bool rightCopied = copyCount > copiesBefore;
        const std::vector<const core::dsl::Element*> menuHits =
            matchingElements(*uiPtr, {"ctx", "menu"});
        const bool menuShown = !menuHits.empty();
        std::string menuIds;
        for (const core::dsl::Element* e : menuHits) {
            menuIds += (menuIds.empty() ? "" : ",") + e->id;
        }
        check(rightCopied || menuShown,
              std::string("C2 有选区右键 = 直接复制或弹出可复制菜单（期望其一；实际") +
                  "复制=" + (rightCopied ? "是" : "否") + "（回调 " +
                  std::to_string(copiesBefore) + "→" + std::to_string(copyCount) +
                  " 次），菜单=" + (menuShown ? "有" : "无") + "，菜单元素=" +
                  (menuIds.empty() ? std::string("<无>") : menuIds) + "）");
        if (!rightCopied && !menuShown) {
            std::cout << "  诊断 右键后无复制无菜单：ctxMenuOpen_ 只置位、页面无元素消费"
                         "（terminal_page.cpp onContextMenu 分支）\n";
        }
    }

    bool passed = g_failures == 0;
    if (!probe::captureIfRequested(args, window, runtime, &passed)) {
        passed = false;
    }
    probe::terminateWindowBackend();
    if (g_failures > 0) {
        std::cout << "terminal_pointer_probe: " << g_failures << " 项断言失败\n";
    }
    return passed ? 0 : 1;
}
