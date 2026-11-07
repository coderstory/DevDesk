// todo_collapse_probe.cpp — Todo 全列折叠泛化探针（池 need#169）
//
// 背景：H20 只有归档列可折叠；need#169 泛化为所有列独立折叠（列 id → 折叠状态映射，
// 点击列头收起/展开，动画时长/曲线与归档列同一组）。本探针用真 GLFW 窗口 + 真指针
// 注入走一遍普通列折叠的完整生命周期，并落 before/after 截图证据。
//
// 断言：
//   N1: 点击普通列头 → 列收窄为折叠条（结构切换：expand.bg 在场 / title 消失）
//   N2: 其余列回流：x 连续无缝、右缘贴 pad（无空洞/重叠）、展开列变宽
//   N3: 再折叠一列：两条窄态条共存，剩余列继续均分回流
//   N4: 点窄态条展开 → 结构还原、宽度回全展开口径
//
// 运行：todo_collapse_probe [--screenshot <路径>] [--shot-* <路径>] [--size WxH]
// 截图证据落 evidence/todo-collapse/（路径由调用方传）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"

#include "core/module_interface.h"
#include "module_todo/todo_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_todo_collapse_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

extern "C" ::core::Module* create_todo_module();

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // need#169 证据截图：--shot-before / --shot-after / --shot-two（未传 = 跳过落图）
    std::string shotBefore, shotAfter, shotTwo;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--shot-before" && i + 1 < argc) shotBefore = argv[++i];
        else if (arg == "--shot-after" && i + 1 < argc) shotAfter = argv[++i];
        else if (arg == "--shot-two" && i + 1 < argc) shotTwo = argv[++i];
    }
    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("todo_collapse_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // ---- Seed DB（固定四列；col0/col1 有卡，回流可见）----
    test_support::TestDb tdb("todocollapse_", "probe");
    test_support::ScopedAppDatabase scopedDb(tdb.handle());
    module::todo::TodoService svc(tdb.handle());
    tdb.handle().exec("DELETE FROM todo_tasks");
    int64_t col0Id = 0;
    {
        auto cols = svc.listColumns();
        for (const auto& c : cols.value()) {
            if (c.kind == "todo") col0Id = c.id;
        }
    }
    if (col0Id == 0) {
        std::cerr << "todo_collapse_probe: 固定列缺失\n";
        probe::terminateWindowBackend();
        return 3;
    }
    for (const char* title : {"折叠前任务A", "折叠前任务B", "折叠前任务C"}) {
        svc.createTask(col0Id, title);
    }

    // ---- Start module ----
    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "todo", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                mod->compose(u, screen);
            });
    };
    auto findEl = [&](const std::string& id) -> const core::dsl::Element* {
        return ui != nullptr ? ui->find(id) : nullptr;
    };
    auto center = [](const core::dsl::Element& el) {
        return std::make_pair(el.frame.x + el.frame.width * 0.5f,
                              el.frame.y + el.frame.height * 0.5f);
    };
    auto click = [&](const core::dsl::Element& el) {
        auto [cx, cy] = center(el);
        probe::click(window, runtime, static_cast<double>(cx),
                     static_cast<double>(cy));
    };
    // 帧泵：每 40ms 一帧推进（compose 期按壁钟推进折叠动画，240/200ms 落位）
    auto pump = [&](int ms) {
        for (int t = 0; t < ms; t += 40) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            composeFrame();
        }
    };
    auto shot = [&](const std::string& path, const char* tag) {
        if (path.empty()) return;
        if (!probe::captureFramePng(window, runtime, path)) {
            std::cerr << "todo_collapse_probe: " << tag << " 落图失败: "
                      << path << "\n";
            passed = false;
        }
    };
    auto colWidth = [&](int idx) -> float {
        const auto* bg = findEl("todo.col." + std::to_string(idx) + ".bg");
        return bg != nullptr ? bg->frame.width : -1.0f;
    };
    auto colX = [&](int idx) -> float {
        const auto* bg = findEl("todo.col." + std::to_string(idx) + ".bg");
        return bg != nullptr ? bg->frame.x : -1.0f;
    };

    composeFrame();
    composeFrame();
    for (int i = 0; i < 24; ++i) probe::pollOnce(window, runtime);
    composeFrame();

    // ---- before：缺省态（归档折叠、普通列全展开）----
    const float col0W0 = colWidth(0);
    const float col1W0 = colWidth(1);
    const float col2W0 = colWidth(2);
    const float archW0 = colWidth(3);
    const bool archCollapsed0 = findEl("todo.col.3.expand.bg") != nullptr;
    if (col0W0 <= 0.0f || col1W0 <= 0.0f || !archCollapsed0) {
        std::cerr << "N0 FAIL: 缺省态诊断（col0=" << col0W0
                  << " col1=" << col1W0 << " 归档折叠=" << archCollapsed0
                  << "）\n";
        passed = false;
    } else {
        std::cout << "N0 PASS: 缺省态 = 普通列全展开（col0="
                  << static_cast<int>(col0W0) << "px）+ 归档折叠（"
                  << static_cast<int>(archW0) << "px）\n";
    }
    shot(shotBefore, "before");

    // ---- N1/N2: 点击 col0 列头 → 折叠 + 回流 ----
    if (const auto* head = findEl("todo.col.0.head")) {
        click(*head);
        pump(350);
        const bool stripOn = findEl("todo.col.0.expand.bg") != nullptr;
        const bool titleGone = findEl("todo.col.0.title") == nullptr;
        const float col0W1 = colWidth(0);
        const float col1W1 = colWidth(1);
        const float col2W1 = colWidth(2);
        // x 连续无缝（col1 左缘 = col0 右缘 + gap）；gap 取主题 spacingLarge
        const float gap = core::theme::current().sizes.spacingLarge;
        const float seam = colX(1) - (colX(0) + col0W1);
        if (!stripOn || !titleGone) {
            std::cerr << "N1 FAIL: 折叠结构未切换（strip=" << stripOn
                      << " title 消失=" << titleGone << "）\n";
            passed = false;
        } else {
            std::cout << "N1 PASS: col0 列头点击 → 折叠条（宽 "
                      << static_cast<int>(col0W1) << "px）\n";
        }
        if (col0W1 > col0W0 * 0.5f || col0W1 < 50.0f) {
            std::cerr << "N2 FAIL: col0 折叠宽度异常 " << col0W1 << "\n";
            passed = false;
        } else if (std::fabs(seam - gap) > 2.0f) {
            std::cerr << "N2 FAIL: 列间出现空洞/重叠（seam=" << seam
                      << " gap=" << gap << "）\n";
            passed = false;
        } else if (col1W1 <= col1W0 + 1.0f || col2W1 <= col2W0 + 1.0f) {
            std::cerr << "N2 FAIL: 其余列未回流（col1 " << col1W0 << "→"
                      << col1W1 << "，col2 " << col2W0 << "→" << col2W1
                      << "）\n";
            passed = false;
        } else {
            std::cout << "N2 PASS: 回流（col1 " << static_cast<int>(col1W0)
                      << "→" << static_cast<int>(col1W1) << "px，col2 "
                      << static_cast<int>(col2W0) << "→"
                      << static_cast<int>(col2W1) << "px，seam="
                      << static_cast<int>(seam) << "）\n";
        }
        shot(shotAfter, "after");
    } else {
        std::cerr << "N1 ERR: 找不到 todo.col.0.head\n";
        passed = false;
    }

    // ---- N3: 再折叠 col1 → 两条窄态条共存，col2 继续回流 ----
    if (const auto* head1 = findEl("todo.col.1.head")) {
        click(*head1);
        pump(350);
        const bool both = findEl("todo.col.0.expand.bg") != nullptr &&
                          findEl("todo.col.1.expand.bg") != nullptr;
        const float gap = core::theme::current().sizes.spacingLarge;
        const float seam = colX(2) - (colX(1) + colWidth(1));
        if (!both) {
            std::cerr << "N3 FAIL: 双折叠窄态条未共存\n";
            passed = false;
        } else if (std::fabs(seam - gap) > 2.0f) {
            std::cerr << "N3 FAIL: 双折叠下 col2 接缝异常（seam=" << seam
                      << "）\n";
            passed = false;
        } else {
            std::cout << "N3 PASS: 双列折叠共存，col2 回流至 "
                      << static_cast<int>(colWidth(2)) << "px\n";
        }
        shot(shotTwo, "two");
    } else {
        std::cerr << "N3 ERR: 找不到 todo.col.1.head\n";
        passed = false;
    }

    // ---- N4: 点 col0 窄态条 → 展开还原，col1 保持折叠 ----
    // 展开列均分回流：col0 与 col2（均展开）等宽 = (span − 归档折叠宽 − col1 折叠宽)/2，
    // 比初始全展开更宽是正确回流（col1 让出的宽度仍由展开列分摊）
    if (const auto* strip = findEl("todo.col.0.expand.bg")) {
        click(*strip);
        pump(350);
        const bool expanded = findEl("todo.col.0.title") != nullptr &&
                              findEl("todo.col.0.expand.bg") == nullptr;
        const float col0W2 = colWidth(0);
        const float col2W2 = colWidth(2);
        if (!expanded) {
            std::cerr << "N4 FAIL: col0 未还原展开结构\n";
            passed = false;
        } else if (std::fabs(col0W2 - col2W2) > 2.0f) {
            std::cerr << "N4 FAIL: 展开列宽度不均等（col0=" << col0W2
                      << " col2=" << col2W2 << "）\n";
            passed = false;
        } else if (findEl("todo.col.1.expand.bg") == nullptr ||
                   colWidth(1) > col0W0 * 0.5f) {
            std::cerr << "N4 FAIL: col1 折叠态被误改\n";
            passed = false;
        } else {
            std::cout << "N4 PASS: col0 展开还原（宽 "
                      << static_cast<int>(col0W2)
                      << "px = col2），col1 折叠不受影响\n";
        }
    } else {
        std::cerr << "N4 ERR: 找不到 col0 窄态条\n";
        passed = false;
    }

    // ---- 收尾 ----
    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "todo_collapse_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}
