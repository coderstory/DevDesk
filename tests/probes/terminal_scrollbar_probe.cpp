// terminal_scrollbar_probe.cpp — 池#364 终端滚动条缺失/滚轮回看失效判定探针
//
// 离屏 compose TerminalPage（真页面代码）+ 合成滚轮/拖拽（probe 队列注入，不依赖 OS
// 指针）。FakeHandle 提供确定性回滚史（scrollback=600 行「SB%05d」+ 24 行可视帧）。
// 覆盖两条独立缺陷面：
//   A 滚轮回看能力：Windows 滚轮一档 = ScrollEvent.y=+1.0（GLFW 档位口径）应回滚
//     3 行（终端惯例 1 档 3 行；EUI-NEO 全仓滚动同为档位语义——scrollview/TAB 横滚）。
//     旧码把 y 当像素除以 cellH：一档 1px / 17px≈0 行，17 档才滚 1 行（等效不可滚），
//     且「offset -= lines」方向与 EUI-NEO 档位口径相反。
//   B 滚动条 affordance：终端网格区应存在 term.grid.scroll（track+thumb），thumb
//     几何随回滚偏移联动；无回滚史（sb=0 且内容不溢出）不建条。
// 红绿约定：改前跑=A/B 断言红（滚轮 0 行 / thumb 元素不存在），改后跑=绿。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）

#include "module_terminal/terminal_page.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/terminal_settings.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

// app:: 层实现随 dsl_app_impl.h 自给（glfw_app_main.cpp 只编进主程序）；
// 探针直绘页面不走 app::compose，空实现仅为满足链接。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const app::DslAppConfig config =
        DslAppConfig{}.title("devdesk_term_scrollbar_probe");
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

bool approxEq(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

std::string sbLineText(int index) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "SB%05d", index);
    return buf;
}

// 确定性假会话：回滚史 600 行（文本 SB%05d），可视帧 24x80 全 'x'
class ScrollFakeHandle : public platform::TerminalSessionHandle {
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
    int scrollbackCount() override { return scrollbackN; }
    bool scrollbackLine(int index, std::string& out) override {
        if (index < 0 || index >= scrollbackN) return false;
        out = sbLineText(index);
        return true;
    }
    bool selectionText(int, int, int, int, std::string&) override { return false; }

    platform::TerminalFrame frame;
    int scrollbackN = 600;
};

class ScrollFakeBackend : public platform::TerminalBackend {
public:
    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&, const std::string&) override {
        auto handle = std::make_unique<ScrollFakeHandle>();
        handle->frame.rows = 24;
        handle->frame.cols = 80;
        handle->frame.cursorRow = 0;
        handle->frame.cursorCol = 0;
        handle->frame.cells.assign(
            static_cast<size_t>(24) * 80, platform::TerminalCell{'x', 0, 0, 0, 1});
        last = handle.get();
        return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::ok(
            std::move(handle));
    }
    bool available() const override { return true; }
    const char* unavailableReason() const override { return nullptr; }

    ScrollFakeHandle* last = nullptr;
};

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("term_scrollbar_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    terminal::TerminalPage page;
    terminal::SessionController controller;
    ScrollFakeBackend backend;
    controller.setBackend(&backend);
    controller.startSession();
    terminal::TerminalSettings settings;

    const float kW = static_cast<float>(args.windowW);
    const float kH = static_cast<float>(args.windowH);
    eui::Ui* uiPtr = nullptr;
    terminal::GridLayout layout{};
    const auto composeFrame = [&] {
        probe::composeModuleFrame(runtime, window, "probe", kW, kH,
                                  [&](eui::Ui& ui, const eui::Screen& screen) {
                                      uiPtr = &ui;
                                      layout = page.compose(
                                          ui, screen, controller,
                                          [](const uint8_t*, int32_t) {},
                                          [](const std::string&, bool) {},
                                          [](const std::string&) {},
                                          settings);
                                  });
    };

    // ── 初始帧：贴底基线 ──
    composeFrame();
    terminal::Session* act = controller.active();
    const int sb = act ? backend.last->scrollbackN : 0;
    const int frameRows = act ? act->frame.rows : 0;
    const int viewRows = layout.rows;
    const int totalLines = sb + frameRows;
    const int baseStart = totalLines - viewRows;  // 贴底（offset=0）时视口顶行
    check(act != nullptr && viewRows > 0 && layout.cellH > 0.0f,
          "S0 会话在位且网格度量可用（viewRows=" + std::to_string(viewRows) + "）");
    check(totalLines > viewRows, "S1 回滚史溢出视口（total=" + std::to_string(totalLines) +
                                     " > view=" + std::to_string(viewRows) + "）");
    {
        const core::dsl::Element* row0 = uiPtr->find("term.grid.row.0.sb");
        check(row0 != nullptr && row0->text == sbLineText(baseStart),
              "S2 贴底基线：视口顶行 = 回滚行 " + sbLineText(baseStart) +
                  "（实测=" + (row0 != nullptr ? row0->text : std::string("<无>")) + "）");
    }

    // 悬停网格中心（滚轮 hitTest 需要指针位置）
    core::queuePointerMotion(window, kW * 0.5f, kH * 0.5f, core::PointerButtons{},
                             core::KeyModifiers{});
    composeFrame();

    // ── A. 滚轮回看能力：一档向上 = 回滚 3 行 ──
    probe::queueScroll(window, runtime, 0.0, 1.0);  // Windows 滚轮一档（GLFW +1.0）
    composeFrame();
    const int off = act != nullptr ? act->scrollOffset : -1;
    check(off == 3, "A1 滚轮一档向上回滚 3 行（offset=3；实测=" + std::to_string(off) + "）");
    {
        const core::dsl::Element* row0 = uiPtr->find("term.grid.row.0.sb");
        check(row0 != nullptr && row0->text == sbLineText(baseStart - 3),
              "A2 回滚后视口顶行上移 3 行（期望 " + sbLineText(baseStart - 3) +
                  "；实测=" + (row0 != nullptr ? row0->text : std::string("<无>")) + "）");
    }

    // ── B. 滚动条 affordance ──
    // thumb 视觉位 = frame.y + travel×(offset/max)（渲染期 transform，见 EUI-NEO
    // runtime_state_bindings.h scrollTransformForElement——frame 恒为布局基位）。
    const core::dsl::Element* track = uiPtr->find("term.grid.scroll");
    const core::dsl::Element* thumb = uiPtr->find("term.grid.scroll.thumb");
    check(track != nullptr && thumb != nullptr, "B1 终端网格滚动条存在（track+thumb）");
    if (track != nullptr && thumb != nullptr) {
        const float trackH = track->frame.height;
        const float expectH = trackH * static_cast<float>(viewRows) /
                              static_cast<float>(totalLines);
        check(approxEq(thumb->frame.height, expectH, 2.0f),
              "B2 thumb 高 = 视口/内容比（期望≈" + std::to_string(expectH) + "；实测=" +
                  std::to_string(thumb->frame.height) + "）");
        check(approxEq(thumb->frame.x + thumb->frame.width,
                       track->frame.x + track->frame.width, 1.0f) &&
                  approxEq(track->frame.x + track->frame.width, kW, 12.0f),
              "B3 滚动条贴网格区右缘");
        check(!thumb->scrollThumbSourceId.empty() &&
                  approxEq(thumb->scrollThumbTravel, trackH - thumb->frame.height, 2.0f),
              "C1 thumb 绑定 scroll 状态（travel=trackH-thumbH）");

        // 再滚 9 档（累计 offset=30）放大位移便于断言。注意：compose 每帧重建元素
        // 树，Element* 跨帧即悬垂——重组后必须重 find。
        probe::queueScroll(window, runtime, 0.0, 9.0);
        composeFrame();
        const core::dsl::Element* thumb30 = uiPtr->find("term.grid.scroll.thumb");
        const float maxEui = static_cast<float>(totalLines - viewRows) *
                             layout.cellH;
        const auto visY = [&, thumb30](int off) {
            if (thumb30 == nullptr) return -1.0f;
            const float eui = static_cast<float>(totalLines - viewRows - off) *
                              layout.cellH;
            return thumb30->frame.y +
                   thumb30->scrollThumbTravel * (eui / maxEui);
        };
        const float yAt3 = visY(3);
        const float yAt30 = visY(30);
        const int offNow = act != nullptr ? act->scrollOffset : -1;
        check(offNow == 30, "C2 累计 10 档=30 行（实测=" + std::to_string(offNow) + "）");
        check(yAt30 < yAt3 - 10.0f,
              "C3 thumb 视觉位随回滚上移（3行 y=" + std::to_string(yAt3) + " → 30行 y=" +
                  std::to_string(yAt30) + "）");

        // ── D. 拖拽 thumb 回看（按视觉中心按压——transform 不落 frame） ──
        // thumb 有 0.12s 过渡：滚轮变位后须先沉降（24 帧 > 全仓最长过渡），
        // 否则按压命中区仍在旧位（探针曾按到网格拖选——D1 假红）。
        for (int settle = 0; settle < 24; ++settle) probe::pollOnce(window, runtime);
        const int offBefore = act != nullptr ? act->scrollOffset : 0;
        const float cx = thumb30 != nullptr
                             ? thumb30->frame.x + thumb30->frame.width * 0.5f
                             : -1.0f;
        const float cy = yAt30 + (thumb30 != nullptr ? thumb30->frame.height * 0.5f
                                                     : 0.0f);
        probe::drag(window, runtime, cx, cy, cx, cy - 60.0f);
        composeFrame();
        const int offAfter = act != nullptr ? act->scrollOffset : -1;
        check(offAfter > offBefore,
              "D1 拖拽 thumb 向上=回滚加深（offset " + std::to_string(offBefore) + " → " +
                  std::to_string(offAfter) + "）");
        {
            const core::dsl::Element* row0 = uiPtr->find("term.grid.row.0.sb");
            check(row0 != nullptr && row0->text != sbLineText(baseStart - 30),
                  "D2 拖拽后视口内容变化（顶行=" +
                      (row0 != nullptr ? row0->text : std::string("<无>")) + "）");
        }
    }

    // ── E. 守卫：无回滚史且内容不溢出时不建条（其他区域不受影响） ──
    if (backend.last != nullptr) backend.last->scrollbackN = 0;
    if (act != nullptr) act->scrollOffset = 0;
    composeFrame();
    check(uiPtr->find("term.grid.scroll.thumb") == nullptr,
          "E1 无回滚史（sb=0、帧 24 行 < 视口）不建滚动条");
    if (backend.last != nullptr) backend.last->scrollbackN = sb;
    composeFrame();

    bool passed = g_failures == 0;
    if (!probe::captureIfRequested(args, window, runtime, &passed)) {
        passed = false;
    }
    probe::terminateWindowBackend();
    if (g_failures > 0) {
        std::cout << "term_scrollbar_probe: " << g_failures << " 项断言失败\n";
    }
    return passed ? 0 : 3;
}
