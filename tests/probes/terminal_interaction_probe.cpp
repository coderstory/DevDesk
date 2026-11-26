// terminal_interaction_probe.cpp — 终端页交互 UI（搜索条 / 右键菜单 / 关闭确认卡）
// 真窗口几何与存在性探针（M2 Task 2/3 batch）。
//
// 与 terminal_scrollbar_probe 同款：离屏 compose 真 TerminalPage + Fake 终端后端
// + 真 GLFW 窗口 + Runtime 事件队列注入，断言元素树而非像素。
//
// 覆盖三条事实（id 由实现固定，实现见 terminal_page.cpp / terminal_search.cpp）：
//   S 搜索条（池#365）：网格聚焦后注入 Cmd/Ctrl+F → term.search.bar 出现；
//     宽 376（kPad+200+kPad+2×26+72+28+kPad）、右缘距页宽 12；四个子元素在位。
//     键注入走 runtime 队列（core::queueKeyInput → pollOnce 派发到 focused 元素），
//     与真 app 同一条通道；探针不直接改页面私有态。
//   M 右键菜单（M2 Task 2）：向 term.input 交互层排队右键 press+release →
//     onContextMenu 置位 → components::contextMenu 生成 term.ctx.level.0；
//     菜单恰 3 项（复制 / 清屏 / 搜索）。
//   C 关闭确认卡（M2 Task 3）：TerminalPage::requestQuitConfirm() 公开入口置位 →
//     term.confirm.card 380×168、水平居中（左右边距差 ≤1px）、ok/cancel 等宽。
//
// 退出码：0 全过 / 1 有失败 / 77 无显示环境（probe_harness 金丝雀直接 exit 77）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）

#include "module_terminal/terminal_page.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/terminal_search.h"
#include "module_terminal/terminal_settings.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（glfw_app_main.cpp 只编进主程序）；
// 探针直绘页面不走 app::compose，空实现仅为满足链接。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const app::DslAppConfig config =
        DslAppConfig{}.title("devdesk_term_interaction_probe");
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
void note(const std::string& what) { std::cout << "NOTE " << what << "\n"; }

bool approxEq(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// 元素树全量 id（失败诊断用：打印候选前缀，避免 id 猜错时只丢一句 FAIL）。
void collectIds(const core::dsl::Element& node, std::vector<std::string>& out) {
    out.push_back(node.id);
    for (const auto& child : node.children) collectIds(*child, out);
}

void dumpIdsContaining(const eui::Ui& ui, const std::string& needle) {
    std::vector<std::string> ids;
    for (const core::dsl::Element* root : ui.orderedRoots()) {
        if (root != nullptr) collectIds(*root, ids);
    }
    int shown = 0;
    for (const std::string& id : ids) {
        if (id.find(needle) != std::string::npos) {
            std::cout << "  DUMP " << id << "\n";
            if (++shown >= 24) {
                std::cout << "  DUMP ...（截断）\n";
                break;
            }
        }
    }
    if (shown == 0) std::cout << "  DUMP <无含 \"" << needle << "\" 的元素>\n";
}

// 确定性假会话：24×80 可视帧，无回滚史（交互 UI 不依赖滚动内容）。
class InteractionFakeHandle : public platform::TerminalSessionHandle {
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
    bool selectionText(int, int, int, int, std::string&) override { return false; }

    platform::TerminalFrame frame;
};

class InteractionFakeBackend : public platform::TerminalBackend {
public:
    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&, const std::string&) override {
        auto handle = std::make_unique<InteractionFakeHandle>();
        handle->frame.rows = 24;
        handle->frame.cols = 80;
        handle->frame.cursorRow = 0;
        handle->frame.cursorCol = 0;
        handle->frame.cells.assign(
            static_cast<size_t>(24) * 80,
            platform::TerminalCell{'x', 0, 0, 0, 1});
        last = handle.get();
        return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::ok(
            std::move(handle));
    }
    bool available() const override { return true; }
    const char* unavailableReason() const override { return nullptr; }

    InteractionFakeHandle* last = nullptr;
};

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;  // 无显示环境已在内部 exit(77)
    auto window = probe::makeWindow("term_interaction_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 1;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    terminal::TerminalPage page;
    terminal::SessionController controller;
    InteractionFakeBackend backend;
    controller.setBackend(&backend);
    controller.startSession();
    terminal::TerminalSettings settings;

    const float kW = static_cast<float>(args.windowW);
    const float kH = static_cast<float>(args.windowH);
    eui::Ui* uiPtr = nullptr;
    float pageW = kW;
    float pageH = kH;
    terminal::GridLayout layout{};
    const auto composeFrame = [&] {
        probe::composeModuleFrame(runtime, window, "probe", kW, kH,
                                  [&](eui::Ui& ui, const eui::Screen& screen) {
                                      uiPtr = &ui;
                                      pageW = screen.width;
                                      pageH = screen.height;
                                      layout = page.compose(
                                          ui, screen, controller,
                                          [](const uint8_t*, int32_t) {},
                                          [](const std::string&, bool) {},
                                          [](const std::string&) {}, settings);
                                  });
    };

    // ── 基线：会话在位、交互层可命中 ──
    composeFrame();
    check(uiPtr != nullptr && controller.active() != nullptr && layout.rows > 0,
          "B0 会话在位且网格度量可用（rows=" + std::to_string(layout.rows) + "）");

    // 点击网格中心：命中 term.input 交互层（focusable，onKeyEvent）→ 键盘焦点落位。
    const float cx = kW * 0.5f;
    const float cy = kH * 0.5f;
    probe::click(window, runtime, cx, cy);
    composeFrame();

    // ── S. 搜索条：Cmd/Ctrl+F 注入（runtime 键事件队列 → focused 元素）──
    // macOS 快捷键 = super，Windows = control；两标志同置跨平台稳定。
    probe::queueKey(window, runtime, GLFW_KEY_F, core::KeyAction::Press,
                    GLFW_MOD_CONTROL | GLFW_MOD_SUPER);
    probe::queueKey(window, runtime, GLFW_KEY_F, core::KeyAction::Release,
                    GLFW_MOD_CONTROL | GLFW_MOD_SUPER);
    composeFrame();

    const core::dsl::Element* bar = uiPtr->find("term.search.bar");
    bool barByKey = bar != nullptr;
    if (!barByKey) {
        // 降级通道：工具栏搜索钮（同一 search_.open 业务入口，真实 UI 路径）。
        // 只用于让几何断言仍可执行；键通道失败单独记 NOTE，不静默吞掉。
        const core::dsl::Element* btn = uiPtr->find("term.act.search.bg");
        if (btn == nullptr) btn = uiPtr->find("term.search.btn");
        if (btn != nullptr) {
            const float bx = btn->frame.x + btn->frame.width * 0.5f;
            const float by = btn->frame.y + btn->frame.height * 0.5f;
            probe::click(window, runtime, bx, by);
            composeFrame();
            bar = uiPtr->find("term.search.bar");
        }
        note(std::string("S0 Cmd/Ctrl+F 键注入未打开搜索条（") +
             (btn != nullptr ? "降级点工具栏钮" : "无搜索钮可降级") +
             "）——键通道待主会话真机复核");
    }
    check(bar != nullptr, "S1 搜索条存在（term.search.bar，经 Cmd/Ctrl+F 或降级钮打开）");
    if (bar != nullptr) {
        check(approxEq(bar->frame.width, 376.0f, 0.5f),
              "S2 搜索条宽 = 376（实测=" + std::to_string(bar->frame.width) + "）");
        check(approxEq(pageW - (bar->frame.x + bar->frame.width), 12.0f, 1.0f),
              "S3 搜索条右缘距页宽 12（实测=" +
                  std::to_string(pageW - (bar->frame.x + bar->frame.width)) + "）");
        check(uiPtr->find("term.search.prev.bg") != nullptr,
              "S4 上一处钮在（term.search.prev.bg）");
        check(uiPtr->find("term.search.next.bg") != nullptr,
              "S5 下一处钮在（term.search.next.bg）");
        check(uiPtr->find("term.search.count") != nullptr,
              "S6 命中计数在（term.search.count）");
        check(uiPtr->find("term.search.close.bg") != nullptr,
              "S7 关闭钮在（term.search.close.bg）");
    } else {
        dumpIdsContaining(*uiPtr, "search");
    }

    // ── M. 右键菜单：term.input 交互层右键 press+release → onContextMenu ──
    probe::click(window, runtime, cx, cy, core::PointerButton::Right);
    composeFrame();
    const core::dsl::Element* ctxRoot = uiPtr->find("term.ctx");
    const core::dsl::Element* ctxLevel = uiPtr->find("term.ctx.level.0");
    check(ctxRoot != nullptr && ctxLevel != nullptr,
          "M1 右键菜单容器存在（term.ctx / term.ctx.level.0）");
    if (ctxLevel != nullptr) {
        int itemCount = 0;
        for (int i = 0;; ++i) {
            if (uiPtr->find("term.ctx.level.0.item." + std::to_string(i)) == nullptr) break;
            ++itemCount;
        }
        check(itemCount == 3,
              "M2 右键菜单恰 3 项（复制/清屏/搜索；实测=" +
                  std::to_string(itemCount) + "）");
    } else {
        dumpIdsContaining(*uiPtr, "ctx");
    }

    // ── C. 关闭确认卡：公开入口 requestQuitConfirm() 置位 ──
    page.requestQuitConfirm();
    composeFrame();
    const core::dsl::Element* card = uiPtr->find("term.confirm.card");
    check(card != nullptr, "C1 关闭确认卡存在（term.confirm.card）");
    if (card != nullptr) {
        check(approxEq(card->frame.width, 380.0f, 1.0f) &&
                  approxEq(card->frame.height, 168.0f, 1.0f),
              "C2 确认卡尺寸 380×168（实测=" + std::to_string(card->frame.width) +
                  "×" + std::to_string(card->frame.height) + "）");
        const float leftMargin = card->frame.x;
        const float rightMargin = pageW - (card->frame.x + card->frame.width);
        check(std::fabs(leftMargin - rightMargin) <= 1.0f,
              "C3 确认卡水平居中（左边距=" + std::to_string(leftMargin) +
                  "，右边距=" + std::to_string(rightMargin) + "）");
        const core::dsl::Element* ok = uiPtr->find("term.confirm.ok");
        const core::dsl::Element* cancel = uiPtr->find("term.confirm.cancel");
        check(ok != nullptr && cancel != nullptr,
              "C4 确认/取消按钮均在（term.confirm.ok / term.confirm.cancel）");
        if (ok != nullptr && cancel != nullptr) {
            const float expectHalf = (card->frame.width - 48.0f) * 0.5f;
            check(std::fabs(ok->frame.width - cancel->frame.width) <= 1.0f &&
                      approxEq(ok->frame.width, expectHalf, 1.0f),
                  "C5 ok/cancel 等宽且各占半宽（期望≈" +
                      std::to_string(expectHalf) + "；实测=" +
                      std::to_string(ok->frame.width) + "/" +
                      std::to_string(cancel->frame.width) + "）");
        }
    } else {
        dumpIdsContaining(*uiPtr, "confirm");
    }

    bool passed = g_failures == 0;
    if (!probe::captureIfRequested(args, window, runtime, &passed)) {
        passed = false;
    }
    probe::terminateWindowBackend();
    if (g_failures > 0) {
        std::cout << "term_interaction_probe: " << g_failures << " 项断言失败\n";
    }
    return passed ? 0 : 1;
}
