// tests/probes/todo_probes.cpp — DevDesk probes（类 11：todo 拖拽插入位占位条）
// 目标：拖拽占位条必须落在目标卡缝位，且悬停期间不得把列内整列卡片流式下推。
// 回归（todo_ui_board.cpp drawDropBar）：占位条画在 scrollView 内容 column 里，
// 非 ignoreLayout 子元素按流式排布、position 无效 → 条永远贴内容顶 + 整列卡片
// 被下推（条高 + gap 共 16px）、松手弹回。修复 = .ignoreLayout() 跳流式走
// overlay 绝对定位。
//
// 断言机制（拖拽序列同无头测试 test_todo_ui.cpp todo_ui_drag_insert_position）：
//   1. press 源列卡（onPress 记锚点 = 卡片真实 frame bounds，指针空间）
//   2. drag 到目标列「卡 0 与卡 1 之间缝」（y = 列顶 + 56 + 88，内容坐标 88
//      = 缝 [84,92] 中心 → hoverRow = 1）
//   3. 断言占位条 frame.y == 目标缝位（条 y = row*92-8 = 84，绝对坐标 =
//      列顶 + 56 + 84 == 卡 1 顶 - 8）；且悬停期间卡 0/卡 1 wrap y 不变
//      （修复前整列被流式下推 → 红）
//   4. release → 占位条消失 + moveTask 按行落库（插到 乙 之前）
//
// 列下标注意：空库迁移自建 4 默认列（同无头 test_todo_ui.cpp
// todo_ui_drag_insert_position 用 col.4/col.5）——探针建的「源列/目标列」
// 落在下标 4/5，不是 0/1（写 0/1 会 find 到空默认列 → 首帧必红）。
//
// 容差：条与缝的换算 ±0.5px（harness floatEq；spec 类 11 允许 ±4px，几何完全
// 确定时取严）。证伪门禁：drawDropBar 去掉 .ignoreLayout()（回退修复形态）→
// 条贴内容顶 + 整列下推 → 本 probe 红。
//
// 类 12（2026-09-28 追加）：拖拽跟手副本。用**真实注入指针**（queuePointerButton
// / queuePointerMotion + 逐帧 pollOnce）拖卡片，中途断言 todo.dragghost 的帧位
// = 按下卡位 + 累计位移、绘制序在 todo.root 之后（跨列不被盖）、zIndex > 0、
// opacity < 1；松手回源列（落回自身原槽位 = 同列重排的 no-op 分支）后副本消失
// 且数据未动。证伪门禁：副本改回列内 .translate()（被 scrollView clip 剪掉）
// 或 dragGhostOn 恒 false → 本段红。
//
// 类 15（2026-09-28 追加，真机三缺陷）：卡左/右内边距数值（缺陷 1）、拖动中
// 占位条与卡槽同源同宽同位（缺陷 2，旧形态条右边留空）、同列拖到列尾松手
// 真落库重排（缺陷 3，旧约定「同列 = 取消」）；释放点故意取列左缘 + 8
// （内边距带内）——命中换算漏减 inset 时该点判列外 → 重排不发生 → 红。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include "module_todo/todo_service.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层实现（openWindow/activeWindowEffect 等随 dsl_app_impl.h 自给，
// 与 pet_probes.cpp:63-69 同款注入；glfw_app_main.cpp 只编进 DevDesk 主程序）。
// dsl_app_impl.h 头内 emit 的函数体引用 dslAppConfig/compose 两个符号——
// 探针不建应用级 UI（compose 走 probe::composeModuleFrame 直接驱动 runtime），
// compose 空实现仅满足链接
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_todo_probes");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

// 与 todo_ui_logic.h 的卡片/列头几何常量同值（断言换算用；probe 不 include
// 模块内部头，常量值域由本文件自锁）
constexpr float kCardHeight = 84.0f;
constexpr float kCardGap = 8.0f;
constexpr float kColumnHeaderHeight = 56.0f;
// 列内容水平内边距（todo_ui_logic.h kColumnContentInset 同值；缺陷 1/2 的
// 卡左缘 / 占位条 x 都以它为基准）
constexpr float kColumnContentInset = 16.0f;

constexpr const char* kDbPrefix = "devdesk_probe_todo_";

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

// RGB 三通道近似（border 色比较；忽略 alpha）
bool channelsClose(const eui::Color& a, const eui::Color& b, float eps) {
    return std::fabs(a.r - b.r) < eps && std::fabs(a.g - b.g) < eps &&
           std::fabs(a.b - b.b) < eps;
}

// RAII：临时库（test_support::TestEnv，registerAppDb = true —— todo 模块
// onStart 经 ::core::appDatabase() 拿库）+ TodoService glue（test_todo_ui.cpp
// 同款，keep-local TIN-06 约定）；视图状态路径经环境变量隔离，析构清。
struct ProbeEnv : test_support::TestEnv {
    std::unique_ptr<module::todo::TodoService> svc;
    std::filesystem::path statePath;

    ProbeEnv() : test_support::TestEnv(kDbPrefix, "", true) {
        svc = std::make_unique<module::todo::TodoService>(handle());
        statePath = std::filesystem::path(path.string() + ".view_state.json");
        std::error_code ec;
        std::filesystem::remove(statePath, ec);
        test_support::setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH",
                                statePath.string());
    }

    ~ProbeEnv() {
        test_support::setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH", "");
        std::error_code ec;
        std::filesystem::remove(statePath, ec);
    }
};

// Element::frame（LayoutRect）→ 回调参数 Rect（无头 boundsOf 同款）
eui::Rect boundsOf(const core::dsl::Element* e) {
    return eui::Rect{e->frame.x, e->frame.y, e->frame.width, e->frame.height};
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("todo_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 配置隔离（probe 与机器本地 config.json / 视图状态无关）
    auto cfg = test_support::tempConfigFile("pfx_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    // 造数：源列 1 卡（被拖卡）、目标列 2 卡（甲 / 乙）——同列拖 = 取消不预览，
    // 类 11 必须跨列拖才有占位条（spec §3-probe-full 类 11 搭法：2 列 2 卡）
    ProbeEnv env;
    auto src = env.svc->createColumn("源列");
    auto dst = env.svc->createColumn("目标列");
    if (src.isFail() || dst.isFail()) {
        std::cerr << "todo_probes: createColumn failed\n";
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 1;
    }
    const int64_t srcColId = src.value().id;
    const int64_t dstColId = dst.value().id;
    auto mover = env.svc->createTask(srcColId, "被移动任务");
    auto cardA = env.svc->createTask(dstColId, "甲");
    auto cardB = env.svc->createTask(dstColId, "乙");
    if (mover.isFail() || cardA.isFail() || cardB.isFail()) {
        std::cerr << "todo_probes: createTask failed\n";
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 1;
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "todo";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 拖拽中间
    // 态进树 + layout，帧即有效）。元素树每帧重建，跨帧持有的 Element* 失效
    // ——每次 compose 后必须重新 find（同无头 mid-drag re-find 注释）
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };

    // ---- 首帧：找源卡（拖拽锚点）+ 拖前几何基准 ----
    composeFrame();
    const core::dsl::Element* srcCard =
        ui != nullptr ? ui->find("todo.col.4.card.0.bg") : nullptr;
    const core::dsl::Element* card0Wrap =
        ui != nullptr ? ui->find("todo.col.5.card.0.wrap") : nullptr;
    const core::dsl::Element* card1Wrap =
        ui != nullptr ? ui->find("todo.col.5.card.1.wrap") : nullptr;
    if (srcCard == nullptr || card0Wrap == nullptr || card1Wrap == nullptr ||
        !floatEq(card0Wrap->frame.y,
                 card1Wrap->frame.y - kCardHeight - kCardGap)) {
        std::cerr << "todo_probes: board elements not found / misordered "
                     "(col.4.card.0.bg, col.5.card.{0,1}.wrap)\n";
        passed = false;
    } else {
        // 拖前快照：目标列首卡绝对位置（断言悬停期间不被流式下推）
        const float card0YBefore = card0Wrap->frame.y;

        // ---- 拖拽序列（同无头直调回调）：press 记锚点 → drag 至卡缝悬停 ----
        eui::PointerEvent pe;
        srcCard->onPress(pe, boundsOf(srcCard));
        const core::dsl::Element* dstBg =
            ui != nullptr ? ui->find("todo.col.5.bg") : nullptr;
        if (dstBg == nullptr) {
            std::cerr << "todo_probes: 'todo.col.5.bg' not found\n";
            passed = false;
        } else {
            core::dsl::DragEvent ev;
            ev.x = dstBg->frame.x + dstBg->frame.width * 0.5;  // 目标列中央
            // 目标列内容坐标 88 = 卡 0 底缝中心 [84,92] → hoverRow = 1（卡 0
            // 与卡 1 之间）。换算链路（todo_ui_logic.h dragDropRowIndex）由按
            // 下锚点回推板面顶边：relY = y - boardTop - 56 + targetScroll →
            // row = floor((relY-84)/92)+1；88 在行 1 区间中部，±几 px 不越界
            ev.y = dstBg->frame.y + kColumnHeaderHeight + kCardHeight +
                   kCardGap * 0.5f;
            srcCard->onDrag(ev);
            composeFrame();

            // 重 find（drag compose 重建了元素树）
            const core::dsl::Element* dropBar =
                ui != nullptr ? ui->find("todo.col.5.dropbar") : nullptr;
            const core::dsl::Element* card0After =
                ui != nullptr ? ui->find("todo.col.5.card.0.wrap") : nullptr;
            const core::dsl::Element* card1After =
                ui != nullptr ? ui->find("todo.col.5.card.1.wrap") : nullptr;
            const core::dsl::Element* dstBgAfter =
                ui != nullptr ? ui->find("todo.col.5.bg") : nullptr;
            if (dropBar == nullptr || card0After == nullptr ||
                card1After == nullptr || dstBgAfter == nullptr) {
                std::cerr << "todo_probes: elements missing after drag "
                             "compose (dropbar="
                          << (dropBar != nullptr ? "x" : "-") << ", card0="
                          << (card0After != nullptr ? "x" : "-") << ", card1="
                          << (card1After != nullptr ? "x" : "-") << ", bg="
                          << (dstBgAfter != nullptr ? "x" : "-")
                          << "); drag mid-state not in tree\n";
                passed = false;
            } else {
                // 断言 (a)：占位条 frame.y == 目标缝位。条 y = row*92-8 = 84
                //（内容坐标），绝对 = 列顶 + 56 + 84；等义 = 卡 1 顶前 8px
                const float expectedBarY =
                    dstBgAfter->frame.y + kColumnHeaderHeight + kCardHeight;
                if (!floatEq(dropBar->frame.y, expectedBarY)) {
                    std::cerr << "todo_probes: dropbar frame.y="
                              << dropBar->frame.y << " expected slot y="
                              << expectedBarY
                              << " (col.1.bg.y + 56 + 84)\n";
                    passed = false;
                }
                if (!floatEq(dropBar->frame.y,
                             card1After->frame.y - kCardGap)) {
                    std::cerr << "todo_probes: dropbar frame.y="
                              << dropBar->frame.y << " != card1 wrap.y - 8 = "
                              << (card1After->frame.y - kCardGap) << '\n';
                    passed = false;
                }
            }
            // 断言 (b)：悬停期间列内卡片不被流式下推（修复前整列下推 16px）
            if (card0After == nullptr || card1After == nullptr ||
                dstBgAfter == nullptr) {
                std::cerr << "todo_probes: target column elements lost "
                             "mid-drag\n";
                passed = false;
            } else {
                if (!floatEq(card0After->frame.y, card0YBefore)) {
                    std::cerr << "todo_probes: card0 wrap.y drifted during "
                                 "hover: "
                              << card0YBefore << " -> " << card0After->frame.y
                              << " (bug shape: bar in flow pushes cards down "
                                 "16px)\n";
                    passed = false;
                }
                // 卡 1 顶 = 列顶 + 列头高 + 92（与占位条并存时仍不被挤流）
                const float expectedCard1Y =
                    dstBgAfter->frame.y + kColumnHeaderHeight + kCardHeight +
                    kCardGap;
                if (!floatEq(card1After->frame.y, expectedCard1Y)) {
                    std::cerr << "todo_probes: card1 wrap.y="
                              << card1After->frame.y << " expected "
                              << expectedCard1Y
                              << " (col.1.bg.y + 56 + 92)\n";
                    passed = false;
                }
            }

            // ---- release：占位条消失 + 按行落库（插到 乙 之前 = 行 1）----
            eui::PointerEvent up;
            up.x = ev.x;
            up.y = ev.y;
            up.action = eui::PointerAction::Release;
            // mid-drag compose 重建元素树——release 前重新 find（同无头注释：
            // 旧指针的回调被 move 走、直调会 bad_function_call）
            const core::dsl::Element* rel =
                ui != nullptr ? ui->find("todo.col.4.card.0.bg") : nullptr;
            if (rel == nullptr) {
                std::cerr << "todo_probes: source card gone before release\n";
                passed = false;
            } else {
                rel->onRelease(up, eui::Rect{});
                composeFrame();
                if (ui != nullptr &&
                    ui->find("todo.col.5.dropbar") != nullptr) {
                    std::cerr << "todo_probes: dropbar still present after "
                                 "release\n";
                    passed = false;
                }
            }
        }

        // ---- 落库验证：目标列顺序 = [甲, 被移动任务, 乙]（行 1 = 插到 乙 前）----
        auto ts = env.svc->listTasks(dstColId);
        if (ts.isFail()) {
            std::cerr << "todo_probes: listTasks failed: "
                      << ts.errorMessage() << '\n';
            passed = false;
        } else if (ts.value().size() != 3u) {
            std::cerr << "todo_probes: target column task count = "
                      << ts.value().size() << " (expected 3)\n";
            passed = false;
        } else {
            const std::string& t0 = ts.value()[0].title;
            const std::string& t1 = ts.value()[1].title;
            const std::string& t2 = ts.value()[2].title;
            if (t0 != "甲" || t1 != "被移动任务" || t2 != "乙") {
                std::cerr << "todo_probes: drop order = [" << t0 << ", " << t1
                          << ", " << t2
                          << "] expected [甲, 被移动任务, 乙]\n";
                passed = false;
            }
        }
    }

    // ---- 类 12（2026-09-28）：拖拽跟手副本——**真实注入指针**驱动（不是直调
    // 回调）：press 卡中心 → 逐帧 move 到邻列 → 中途断言副本锚点/层序/半透 →
    // 再 move 回源列松手（同列 = 取消，不动数据，类 14 的前置状态不被破坏）。
    // 与无头用例 todo_ui_drag_follow_ghost 的分工：无头只证"拿到锚点后画在哪"
    // （直调回调），本探针证**框架真把 DragEvent::totalX/totalY 喂进来**、
    // 真实命中测试能按下卡片、副本帧位 = 卡位 + 累计位移。
    if (ui != nullptr) {
        const core::dsl::Element* ghostSrc = ui->find("todo.col.5.card.0.bg");
        const core::dsl::Element* ghostDstCol = ui->find("todo.col.4.bg");
        if (ghostSrc == nullptr || ghostDstCol == nullptr) {
            std::cerr << "todo_probes: 跟手副本场景缺元素（源卡/邻列）\n";
            passed = false;
        } else {
            // 按下锚点 = 卡片真实 bounds（指针空间 = 窗口坐标，探针 dpi=1）。
            // 几何一次性拷成值：元素指针跨帧失效（下面每次 compose 都重建元素树）
            const eui::Rect srcBounds = boundsOf(ghostSrc);
            const double px = srcBounds.x + srcBounds.width * 0.5;
            const double py = srcBounds.y + srcBounds.height * 0.5;
            const double tx =
                ghostDstCol->frame.x + ghostDstCol->frame.width * 0.5;
            const double ty = ghostDstCol->frame.y + kColumnHeaderHeight + 40.0;
            const core::KeyModifiers noMods;
            const core::PointerButtons held(core::PointerButton::Left);
            core::queuePointerButton(window, px, py, core::PointerButton::Left,
                                     core::PointerAction::Press, noMods);
            composeFrame();
            // 分 4 帧插值移动（每帧一 pollOnce：命中/悬停/拖拽沿路径推进）
            constexpr int kSteps = 4;
            for (int step = 1; step <= kSteps; ++step) {
                const double t = static_cast<double>(step) / kSteps;
                core::queuePointerMotion(window, px + (tx - px) * t,
                                         py + (ty - py) * t, held, noMods);
                composeFrame();
            }
            const core::dsl::Element* ghost =
                ui != nullptr ? ui->find("todo.dragghost") : nullptr;
            if (ghost == nullptr) {
                std::cerr << "todo_probes: 拖拽中 'todo.dragghost' 不在树"
                             "（跟手副本未画，或真实指针没走到 onDrag）\n";
                passed = false;
            } else {
                // 锚点 = 按下时卡片位置 + 按下点起累计位移（真实指针位移）
                const float expectedX =
                    srcBounds.x + static_cast<float>(tx - px);
                const float expectedY =
                    srcBounds.y + static_cast<float>(ty - py);
                if (!floatEq(ghost->frame.x, expectedX) ||
                    !floatEq(ghost->frame.y, expectedY)) {
                    std::cerr << "todo_probes: 副本锚点 (" << ghost->frame.x
                              << "," << ghost->frame.y << ") 期望 ("
                              << expectedX << "," << expectedY
                              << ") = 卡位 + 累计位移\n";
                    passed = false;
                }
                // 层序：副本是页面顶层 root（排在 todo.root 之后）+ z 压看板子树
                const core::dsl::Element* boardRoot = ui->find("todo.root");
                const auto& roots = ui->orderedRoots();
                auto idxOf = [&roots](const core::dsl::Element* target) {
                    for (size_t i = 0; i < roots.size(); ++i) {
                        if (roots[i] == target) return static_cast<int>(i);
                    }
                    return -1;
                };
                if (boardRoot == nullptr || idxOf(ghost) <= idxOf(boardRoot)) {
                    std::cerr << "todo_probes: 副本绘制序不在 todo.root 之后"
                                 "（跨列拖会被目标列卡片盖住）\n";
                    passed = false;
                }
                if (ghost->zIndex <= 0) {
                    std::cerr << "todo_probes: 副本 zIndex=" << ghost->zIndex
                              << "（应 > 看板子树最大 z）\n";
                    passed = false;
                }
                if (!(ghost->opacity < 1.0f)) {
                    std::cerr << "todo_probes: 副本 opacity=" << ghost->opacity
                              << "（应 < 1：占位条要从副本底下透出）\n";
                    passed = false;
                }
            }
            // 松手（移回源列内 = 同列取消）：副本消失 + 数据未动。
            // ⚠ 释放点必须在**当帧**重新 find 出的列几何上算：元素指针跨帧失效
            //（每帧重建元素树），持按下前的 Element* 读 frame 会读到已释放内存
            //（实测算到隔壁列 → "取消"变成真移动、数据被搬走）
            const core::dsl::Element* srcColNow =
                ui != nullptr ? ui->find("todo.col.5.bg") : nullptr;
            if (srcColNow == nullptr) {
                std::cerr << "todo_probes: 松手前 'todo.col.5.bg' 不在树\n";
                passed = false;
            } else {
                const double bx =
                    srcColNow->frame.x + srcColNow->frame.width * 0.5;
                const double by = srcBounds.y + 20.0;
                for (int step = 1; step <= kSteps; ++step) {
                    const double t = static_cast<double>(step) / kSteps;
                    core::queuePointerMotion(window, tx + (bx - tx) * t,
                                             ty + (by - ty) * t, held, noMods);
                    composeFrame();
                }
                core::queuePointerButton(window, bx, by,
                                         core::PointerButton::Left,
                                         core::PointerAction::Release, noMods);
                composeFrame();
            }
            if (ui != nullptr && ui->find("todo.dragghost") != nullptr) {
                std::cerr << "todo_probes: 松手后 'todo.dragghost' 仍在树"
                             "（偏移未归零）\n";
                passed = false;
            }
            // 同列松手 = no-op（2026-09-28 缺陷 3 起同列拖拽是受支持操作：
            // 释放点落在自身原槽位 → 行号换算回原位 → 不落库，数据不变；
            // 旧语义「同列 = 取消」与之一致体现在数据上，预览行为已变）
            auto after = env.svc->listTasks(dstColId);
            if (after.isFail() || after.value().size() != 3u) {
                std::cerr << "todo_probes: 同列拖拽（落回自身槽位）却改了数据（列卡数="
                          << (after.isOk()
                                  ? std::to_string(after.value().size())
                                  : std::string("listTasks 失败"))
                          << "，期望 3）\n";
                auto all = env.svc->listAllTasks();
                for (const auto& tk : all.value()) {
                    std::cerr << "  task '" << tk.title << "' → columnId "
                              << tk.columnId << " (源列 " << srcColId
                              << " / 目标列 " << dstColId << ")\n";
                }
                passed = false;
            }
        }
    }

    // ---- 类 15（2026-09-28 真机三缺陷，**真实指针注入**驱动）：缺陷 1/2 几何
    // 数值 + 缺陷 3 同列上下重排落库。
    // ⚠ 本段必须排在类 14 之前：类 14 点开会开详情弹窗，其遮罩会挡住真实
    // 指针（本段走 queuePointerButton/Motion 真命中/真拖拽派发，不直调回调）。
    // 断言：
    //   (a) 卡左缘 - 列左缘 == kColumnContentInset（缺陷 1：旧形态 = 0，
    //       卡贴列左边缘；修后两侧 16 同源）
    //   (b) 列右缘 - 卡右缘 == kColumnContentInset（两侧对称；旧形态右侧 16
    //       但左侧 0 —— 卡宽写「内容宽 - 16」而 x 恒 0）
    //   (c) 拖动中占位条 x == 列左缘 + inset、宽 == 该列卡宽（缺陷 2：旧形态
    //       x=0 / 宽=内容宽-16，与卡槽不同源，条右边留空）
    //   (d) 同列拖到列尾松手 → 落库顺序真变（缺陷 3：旧形态同列 = 取消）
    //   (e) 释放点 x 取「列左缘 + 8」= 16px 内边距带内（卡占不到、但仍是本列
    //       落区）：命中换算漏减 kColumnContentInset 时该点被判列外
    //       （target=-1）→ 重排不发生 → (d) 红（变异门禁，见报告）
    if (ui != nullptr) {
        const core::dsl::Element* col5Bg = ui->find("todo.col.5.bg");
        const core::dsl::Element* dragSrc = ui->find("todo.col.5.card.0.bg");
        if (col5Bg == nullptr || dragSrc == nullptr) {
            std::cerr << "todo_probes: 类 15 场景缺元素（col.5.bg / "
                         "col.5.card.0.bg）\n";
            passed = false;
        } else {
            const eui::Rect colRect = boundsOf(col5Bg);
            const eui::Rect srcRect = boundsOf(dragSrc);
            const float leftGap = srcRect.x - colRect.x;
            if (!floatEq(leftGap, kColumnContentInset)) {
                std::cerr << "todo_probes: 卡左边距 = " << leftGap
                          << "，期望 " << kColumnContentInset
                          << "（缺陷 1 形态 = 0：卡贴列左缘）\n";
                passed = false;
            }
            const float rightGap = (colRect.x + colRect.width) -
                                   (srcRect.x + srcRect.width);
            if (!floatEq(rightGap, kColumnContentInset)) {
                std::cerr << "todo_probes: 卡右边距 = " << rightGap
                          << "，期望 " << kColumnContentInset
                          << "（两侧须同源对称）\n";
                passed = false;
            }
            // 真实指针：按卡中心 → 4 帧插值移到本列列尾（x 取列左缘 + 8）
            const double px = srcRect.x + srcRect.width * 0.5;
            const double py = srcRect.y + srcRect.height * 0.5;
            const double tx = colRect.x + 8.0;
            const double ty = colRect.y + kColumnHeaderHeight + 400.0;
            const core::KeyModifiers noMods;
            const core::PointerButtons held(core::PointerButton::Left);
            core::queuePointerButton(window, px, py, core::PointerButton::Left,
                                     core::PointerAction::Press, noMods);
            composeFrame();
            constexpr int kSteps = 4;
            for (int step = 1; step <= kSteps; ++step) {
                const double t = static_cast<double>(step) / kSteps;
                core::queuePointerMotion(window, px + (tx - px) * t,
                                         py + (ty - py) * t, held, noMods);
                composeFrame();
            }
            // (c) 占位条几何（缺陷 2）：与卡槽同源同宽同位
            const core::dsl::Element* bar =
                ui != nullptr ? ui->find("todo.col.5.dropbar") : nullptr;
            const core::dsl::Element* cardNow =
                ui != nullptr ? ui->find("todo.col.5.card.0.bg") : nullptr;
            const core::dsl::Element* colNow =
                ui != nullptr ? ui->find("todo.col.5.bg") : nullptr;
            const core::dsl::Element* lastWrap =
                ui != nullptr ? ui->find("todo.col.5.card.2.wrap") : nullptr;
            if (bar == nullptr || cardNow == nullptr || colNow == nullptr ||
                lastWrap == nullptr) {
                std::cerr << "todo_probes: 类 15 拖动中元素缺失（dropbar/card/"
                             "col/lastWrap）\n";
                passed = false;
            } else {
                if (!floatEq(bar->frame.x, colNow->frame.x + kColumnContentInset)) {
                    std::cerr << "todo_probes: 占位条 x = " << bar->frame.x
                              << "，期望列左缘 " << colNow->frame.x << " + "
                              << kColumnContentInset
                              << "（缺陷 2 旧形态 = 0：条贴列左缘）\n";
                    passed = false;
                }
                if (!floatEq(bar->frame.width, cardNow->frame.width)) {
                    std::cerr << "todo_probes: 占位条宽 = " << bar->frame.width
                              << " ≠ 卡宽 " << cardNow->frame.width
                              << "（缺陷 2：条与覆盖的卡槽须同源）\n";
                    passed = false;
                }
                if (!floatEq(bar->frame.x, cardNow->frame.x)) {
                    std::cerr << "todo_probes: 占位条 x = " << bar->frame.x
                              << " ≠ 卡 x " << cardNow->frame.x << '\n';
                    passed = false;
                }
                // 列尾槽位：条落在末卡（card 2）底缝
                if (!floatEq(bar->frame.y, lastWrap->frame.y +
                                               kCardHeight)) {
                    std::cerr << "todo_probes: 列尾占位条 y = " << bar->frame.y
                              << "，期望末卡底缝 " << lastWrap->frame.y +
                                                          kCardHeight
                              << '\n';
                    passed = false;
                }
            }
            core::queuePointerButton(window, tx, ty, core::PointerButton::Left,
                                     core::PointerAction::Release, noMods);
            composeFrame();
            // (d) 同列重排真落库：甲（拖前 card 0）→ 列尾
            auto reordered = env.svc->listTasks(dstColId);
            if (reordered.isFail() || reordered.value().size() != 3u) {
                std::cerr << "todo_probes: 类 15 重排后 listTasks 失败/卡数异常\n";
                passed = false;
            } else {
                const std::string& r0 = reordered.value()[0].title;
                const std::string& r1 = reordered.value()[1].title;
                const std::string& r2 = reordered.value()[2].title;
                if (r0 != "被移动任务" || r1 != "乙" || r2 != "甲") {
                    std::cerr << "todo_probes: 同列重排落库顺序 = [" << r0
                              << ", " << r1 << ", " << r2
                              << "] 期望 [被移动任务, 乙, 甲]（缺陷 3：同列"
                                 "拖到列尾须落库重排）\n";
                    passed = false;
                }
            }
        }
    }

    // ---- 类 14（spec §3-probe-full，用户硬指定 HIGH）：选中态边框——
    // 点卡 0 → openDetailDialog → selectedTaskId_ = 该卡 → 卡 .bg 选中 2px
    // primary 加粗；未选中 1px borderDefault。优先级竖条 .bar 随 UI 整改 16
    // 已删——断言不存在防回归。
    // 注：类 15（同列重排）已把 甲 挪到列尾，col.5 顺序此刻 = [被移动任务,
    // 乙, 甲]；本段断言与卡序无关（选中/未选中对比同卡片位置自身）。
    if (ui != nullptr) {
        if (ui->find("todo.col.5.card.0.bar") != nullptr) {
            std::cerr << "todo_probes: 优先级竖条 'todo.col.5.card.0.bar' "
                         "仍在树（UI 整改 16 已删，防回归）\n";
            passed = false;
        }
        const core::dsl::Element* card0Bg =
            ui->find("todo.col.5.card.0.bg");
        if (card0Bg == nullptr) {
            std::cerr << "todo_probes: 'todo.col.5.card.0.bg' not found "
                         "before selection\n";
            passed = false;
        } else {
            card0Bg->onClick();  // cardClicked → openDetailDialog（选中态）
            composeFrame();
            const core::dsl::Element* sel =
                ui != nullptr ? ui->find("todo.col.5.card.0.bg") : nullptr;
            const core::dsl::Element* unsel =
                ui != nullptr ? ui->find("todo.col.5.card.1.bg") : nullptr;
            if (sel == nullptr || unsel == nullptr) {
                std::cerr << "todo_probes: card bg lost after selection\n";
                passed = false;
            } else {
                const core::theme::Tokens& t = core::theme::current();
                if (sel->border.width != 2.0f ||
                    !channelsClose(sel->border.color, t.colors.primary,
                                   0.02f)) {
                    std::cerr << "todo_probes: 选中卡 border width="
                              << sel->border.width << " color ("
                              << sel->border.color.r << ","
                              << sel->border.color.g << ","
                              << sel->border.color.b << ") 期望 2px primary ("
                              << t.colors.primary.r << ","
                              << t.colors.primary.g << ","
                              << t.colors.primary.b << ")\n";
                    passed = false;
                }
                if (unsel->border.width != 1.0f ||
                    !channelsClose(unsel->border.color,
                                   t.colors.borderDefault, 0.02f)) {
                    std::cerr << "todo_probes: 未选中卡 border width="
                              << unsel->border.width << " color ("
                              << unsel->border.color.r << ","
                              << unsel->border.color.g << ","
                              << unsel->border.color.b
                              << ") 期望 1px borderDefault\n";
                    passed = false;
                }
            }
        }
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}