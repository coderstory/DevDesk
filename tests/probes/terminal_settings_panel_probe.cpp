// terminal_settings_panel_probe.cpp — bug278 终端设置弹窗四项缺陷判定探针
//
// 离屏 compose TerminalPage（真页面代码，不经模块注册表）+ 合成点击（probe::click
// 队列注入，不依赖 OS 指针——clue168/170 沙箱不可注入真指针）。覆盖：
//   ① PRD §4.1 面板几何守卫（卡居中/分组头/行/label 定宽/控件对齐/底部行，wide 档）
//   ③ 「对新会话生效」提示行删除后不存在（老板令：整行去掉）且行布局不塌
//   ④ 弹窗内改动（含恢复默认）不得触发 persist（落库回调）——落库只在「完成」
//   ② 说明：segmented（分段标签）label 为 EUI-NEO 组件内部绘制（Top 对齐紧盒，
//     components/segmented.h:69-122），盒级几何本探针断言，墨迹级居中归框架侧
//     （终报另述）。
//
// 红绿约定：③④断言改前跑=红（提示行存在 / reset·onChange 都触发 persist），
// 改后跑=绿（提示行不存在 / persist 恒 0、落库只在完成）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"

#include "module_terminal/terminal_page.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/terminal_settings.h"

#include <core/theme.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（glfw_app_main.cpp 只编进主程序）；
// 探针直绘页面不走 app::compose，空实现仅为满足链接。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const app::DslAppConfig config =
        DslAppConfig{}.title("devdesk_term_settings_probe");
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

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("term_settings_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    // 临时库（注册为进程级 appDatabase——④的落库断言走生产 save/load 入口）
    test_support::TestEnv env("devdesk_term_settings_probe_", "cfg", true);
    core::data::SqliteDb& db = env.handle();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // 页面直绘：probe 持有 settings（镜像 module 的单一数据源位）。改前页面在每次
    // 控件 onChange / 恢复默认里回调 persist（落库时机错误=bug278④，红证据）；改后
    // 页面无逐改动持久化通道（compose 不再收 persist 回调），落库只在完成结果通道。
    terminal::TerminalPage page;
    terminal::SessionController controller;  // 无会话（不 spawn；面板层独立于会话渲染）
    terminal::TerminalSettings settings;
    terminal::TerminalSettings snapshot = settings;  // 打开时刻基线（镜像 module 快照）
    int commitCalls = 0;
    int cancelCalls = 0;
    page.setPanelResultHandler([&](bool commit) {
        // 镜像 module_terminal.cpp 结果通道（bug278④）：完成=落库；×=回滚快照
        if (commit) {
            commitCalls++;
            auto r = terminal::saveTerminalSettings(&db, settings);
            if (r.isFail()) std::cerr << "probe: 落库失败 " << r.errorMessage() << "\n";
        } else {
            cancelCalls++;
            settings = snapshot;
        }
        page.closeSettings();
    });

    const float kW = static_cast<float>(args.windowW);
    const float kH = static_cast<float>(args.windowH);
    float screenW = kW;
    float screenH = kH;
    eui::Ui* uiPtr = nullptr;

    // ── A. 面板打开帧：几何守卫 + ③提示行 + 控件改动不落库 ──
    page.openSettings();
    probe::composeModuleFrame(runtime, window, "probe", kW, kH,
                              [&](eui::Ui& ui, const eui::Screen& screen) {
                                  uiPtr = &ui;
                                  // 断言基准 = compose 实际尺寸（bug183 口径：请求值≠实际值）
                                  screenW = screen.width;
                                  screenH = screen.height;
                                  page.compose(ui, screen, controller,
                                               [](const uint8_t*, int32_t) {},
                                               [](const std::string&, bool) {},
                                               [](const std::string&) {},
                                               settings);
                              });

    const core::dsl::Element* card = uiPtr->find("term.set.card");
    check(card != nullptr, "面板卡存在（term.set.card）");
    if (card != nullptr) {
        check(approxEq(card->frame.x + card->frame.width * 0.5f, screenW * 0.5f, 1.0f) &&
                  approxEq(card->frame.y + card->frame.height * 0.5f, screenH * 0.5f, 1.0f),
              "① 卡在页面区内居中（PRD §4.1 位置=水平垂直居中）");
    }

    // ③「对新会话生效」整行删除：行内微字元素不得存在（老板令 2026-11-16）
    check(uiPtr->find("term.set.row.shell.hint") == nullptr,
          "③ shell 行「对新会话生效」微字元素应不存在（整行删除）");
    check(uiPtr->find("term.set.row.wdir.hint") == nullptr,
          "③ 启动目录行「对新会话生效」微字元素应不存在（整行删除）");

    // ① PRD 几何守卫：分组头 / 行 / label 定宽 / 控件对齐
    const core::dsl::Element* g1 = uiPtr->find("term.set.g1");
    check(g1 != nullptr && approxEq(g1->frame.height, 20.0f, 0.5f),
          "① 分组头「外观」存在且高 20（PRD 分组头行高 20）");
    const core::dsl::Element* fontLabel = uiPtr->find("term.set.row.font.label");
    const core::dsl::Element* fontSlot = uiPtr->find("term.set.row.font.slot");
    if (card != nullptr && fontLabel != nullptr && fontSlot != nullptr) {
        const float cardX = card->frame.x;
        check(approxEq(fontLabel->frame.x, cardX + 16.0f, 0.5f) &&
                  approxEq(fontLabel->frame.width, 128.0f, 0.5f),
              "① label 左缘=卡内边距 16、定宽 128（PRD §4.1 设置行）");
        // 文本行垂直居中：label 盒中心 == 控件盒中心，delta≤1px
        const float labelCy = fontLabel->frame.y + fontLabel->frame.height * 0.5f;
        const float slotCy = fontSlot->frame.y + fontSlot->frame.height * 0.5f;
        check(approxEq(labelCy, slotCy, 1.0f),
              "①② 行 label 与控件盒垂直中心对齐 delta≤1px（PRD 行高 44 居中）");
    }
    // ③ 删行后布局不塌：shell / 启动目录行仍在原槽位（label 定宽 128、行存在）
    const core::dsl::Element* shellLabel = uiPtr->find("term.set.row.shell.label");
    const core::dsl::Element* wdirLabel = uiPtr->find("term.set.row.wdir.label");
    check(shellLabel != nullptr && wdirLabel != nullptr,
          "③ 删除提示行后 shell/启动目录行不塌（label 仍在）");
    if (shellLabel != nullptr && fontLabel != nullptr) {
        check(approxEq(shellLabel->frame.width, 128.0f, 0.5f),
              "③ shell 行 label 仍定宽 128（布局口径不变）");
    }

    // 底部行（sticky）：分隔线 + 恢复默认 + 完成
    const core::dsl::Element* sep = uiPtr->find("term.set.sep");
    const core::dsl::Element* reset = uiPtr->find("term.set.reset");
    const core::dsl::Element* done = uiPtr->find("term.set.done");
    check(sep != nullptr && reset != nullptr && done != nullptr,
          "① 底部行三件套存在（分隔线/恢复默认/完成）");
    if (done != nullptr && card != nullptr) {
        check(approxEq(done->frame.height, 32.0f, 0.5f) &&
                  approxEq(done->frame.x + done->frame.width,
                       card->frame.x + card->frame.width - 16.0f, 0.5f),
              "① 完成钮高 32、右缘钉卡内边距（PRD §4.1 底部行）");
    }

    // segmented（「tab」候选）盒级几何记录：盒中心对齐控件行
    const core::dsl::Element* segCursor = uiPtr->find("term.set.row.cursor.style");
    if (segCursor != nullptr) {
        std::cout << "INFO segmented(cursor) frame=(" << segCursor->frame.x << ","
                  << segCursor->frame.y << "," << segCursor->frame.width << ","
                  << segCursor->frame.height << ")\n";
        const core::dsl::Element* segLabel =
            uiPtr->find("term.set.row.cursor.style.label.0");
        if (segLabel != nullptr) {
            const float segCy = segCursor->frame.y + segCursor->frame.height * 0.5f;
            const float labCy = segLabel->frame.y + segLabel->frame.height * 0.5f;
            std::cout << "INFO segmented label.0 box center delta="
                      << (labCy - segCy) << " (盒级；墨迹级归框架侧)\n";
        }
    }

    // ④ 控件改动（草稿态）不触发落库回调：点「块|线」第二段（cursorStyle→Bar）
    const core::dsl::Element* segHit = uiPtr->find("term.set.row.cursor.style.hit.1");
    if (segHit != nullptr) {
        probe::click(window, runtime,
                     static_cast<double>(segHit->frame.x + segHit->frame.width * 0.5f),
                     static_cast<double>(segHit->frame.y + segHit->frame.height * 0.5f));
    }
    check(terminal::loadTerminalSettings(&db).cursorStyle ==
                  terminal::TerminalCursorStyle::Block,
          "④ 草稿态改动不落库（库仍为上次完成值）");
    check(settings.cursorStyle == terminal::TerminalCursorStyle::Bar,
          "④ 草稿态改动本身可见（live preview：cursorStyle=Bar 已进草稿）");

    // ── B. 恢复默认（老板场景）：只改草稿，不得落库 ──
    // 先点击一个会改值的控件已有 Bar；再点恢复默认 → 草稿回默认
    const core::dsl::Element* resetNow = uiPtr->find("term.set.reset");
    if (resetNow != nullptr) {
        probe::click(window, runtime,
                     static_cast<double>(resetNow->frame.x + resetNow->frame.width * 0.5f),
                     static_cast<double>(resetNow->frame.y + resetNow->frame.height * 0.5f));
    }
    check(terminal::loadTerminalSettings(&db).cursorStyle ==
                  terminal::TerminalCursorStyle::Block,
          "④ 恢复默认不落库（库仍为上次完成值；改前红=即落库）");
    check(settings.cursorStyle == terminal::TerminalCursorStyle::Block,
          "④ 恢复默认把草稿回默认（Block）");

    // ── C. 完成 = 唯一落库时机 ──
    const core::dsl::Element* doneNow = uiPtr->find("term.set.done");
    if (doneNow != nullptr) {
        probe::click(window, runtime,
                     static_cast<double>(doneNow->frame.x + doneNow->frame.width * 0.5f),
                     static_cast<double>(doneNow->frame.y + doneNow->frame.height * 0.5f));
    }
    check(commitCalls == 1 && page.settingsOpen() == false,
          "④ 完成触发恰好一次提交且面板关闭");
    const terminal::TerminalSettings savedAfterDone = terminal::loadTerminalSettings(&db);
    check(savedAfterDone.cursorStyle == terminal::TerminalCursorStyle::Block,
          "④ 完成后落库值=草稿值（恢复默认经完成落库=Block）");

    // ── D. 取消（×）= 丢弃草稿回滚快照，库不动 ──
    page.openSettings();
    snapshot = settings;  // 镜像 module 开帧快照
    probe::composeModuleFrame(runtime, window, "probe", kW, kH,
                              [&](eui::Ui& ui, const eui::Screen& screen) {
                                  uiPtr = &ui;
                                  page.compose(ui, screen, controller,
                                               [](const uint8_t*, int32_t) {},
                                               [](const std::string&, bool) {},
                                               [](const std::string&) {},
                                               settings);
                              });
    const core::dsl::Element* segHit2 = uiPtr->find("term.set.row.cursor.style.hit.1");
    if (segHit2 != nullptr) {
        probe::click(window, runtime,
                     static_cast<double>(segHit2->frame.x + segHit2->frame.width * 0.5f),
                     static_cast<double>(segHit2->frame.y + segHit2->frame.height * 0.5f));
    }
    check(settings.cursorStyle == terminal::TerminalCursorStyle::Bar,
          "④ 重新编辑草稿生效（Bar）");
    const core::dsl::Element* closeNow = uiPtr->find("term.set.close.bg");
    if (closeNow != nullptr) {
        probe::click(window, runtime,
                     static_cast<double>(closeNow->frame.x + closeNow->frame.width * 0.5f),
                     static_cast<double>(closeNow->frame.y + closeNow->frame.height * 0.5f));
    }
    check(cancelCalls == 1, "④ × 触发取消通道");
    check(settings.cursorStyle == terminal::TerminalCursorStyle::Block,
          "④ 取消后草稿回滚快照（丢弃）");
    const terminal::TerminalSettings savedAfterCancel = terminal::loadTerminalSettings(&db);
    check(savedAfterCancel.cursorStyle == terminal::TerminalCursorStyle::Block,
          "④ 取消不改库（库值保持上次完成值）");

    probe::terminateWindowBackend();
    std::cout << (g_failures == 0 ? "terminal_settings_panel_probe: PASS\n"
                                  : "terminal_settings_panel_probe: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
