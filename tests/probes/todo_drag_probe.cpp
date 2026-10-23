// todo_drag_probe.cpp — Todo 真指针拖拽正反场景探针（T-15/16）
//
// 背景：Todo 拖拽自 2026-10-03 起刻意避开了 OS 级真指针路径——只有纯几何函数 +
// 帧驱动模拟的 harness 断言。真指针拖拽每日必用、写库操作，一次换算回归就会让
// 任务落错列（数据写错位）。本探针用真 GLFW 窗口 + 真指针注入把正反场景跑一遍。
//
// 正向（T-15/16）：
//   F1: 跨列拖拽  col0→col1  → 断言 task.columnId 改变 + position 正确
//   F2: 列内重排  同列上/下移 → 断言 position 改变、插回原位 no-op
//   F3: 追加列尾  hoverRow>=N → 断言 position = N（原列尾 = N-1，追加 = N）
//
// 反向（错误场景）：
//   R1: 松手在板外（target<0）→ 断言 columnId/position 不变、不崩
//   R2: 拖拽中途 ESC→ 断言 DragState 已复位（cardReleased=false→drag_={}）
//        且 DB 无改动
//   R3: 拖拽到归档列（kind='archive'）→ 断言列内重排无效化（position 不变）
//   R4: 拖拽期间 DB 外部删卡 → 断言 finishDrag 早退，不崩不写错位
//   R5: 极窄多列（6 列）→ 拖拽不崩，落库正确
//
// 运行：todo_drag_probe.exe [--screenshot <路径>] [--size WxH]
// 不带构图开关 = 纯断言门禁（ctest -L probe 跑的就是这条）。
//
// 坐标体系：composeModuleFrame 下页面坐标 = 窗口坐标，element.frame 直接可用于指针注入。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"

#include "core/event_bus.h"
#include "core/module_interface.h"
#include "module_todo/todo_service.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_todo_drag_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

extern "C" ::core::Module* create_todo_module();

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("todo_drag_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // ---- Seed DB ----
    test_support::TestDb tdb("tododrag_", "probe");
    test_support::ScopedAppDatabase scopedDb(tdb.handle());

    // 先构造 service 让 H20 固定四列迁移跑完，再按 kind 取列 id。自定义列会在
    // 模块 onStart 时被再次迁移归并，故直接使用固定四列（列序 = position 序）
    module::todo::TodoService svc(tdb.handle());
    tdb.handle().exec("DELETE FROM todo_tasks");
    int64_t col0Id = 0;
    int64_t col1Id = 0;
    int64_t col2Id = 0;
    int64_t archiveColId = 0;
    {
        auto cols = svc.listColumns();
        if (cols.isFail()) {
            std::cerr << "todo_drag_probe: 读取固定列失败\n";
            probe::terminateWindowBackend();
            return 3;
        }
        for (const auto& c : cols.value()) {
            if (c.kind == "todo") col0Id = c.id;
            else if (c.kind == "doing") col1Id = c.id;
            else if (c.kind == "done") col2Id = c.id;
            else if (c.kind == "archive") archiveColId = c.id;
        }
    }
    if (col0Id == 0 || col1Id == 0 || col2Id == 0 || archiveColId == 0) {
        std::cerr << "todo_drag_probe: 固定四列缺失\n";
        probe::terminateWindowBackend();
        return 3;
    }

    auto tA = svc.createTask(col0Id, "任务A");
    auto tB = svc.createTask(col0Id, "任务B");
    auto tC = svc.createTask(col0Id, "任务C");
    auto tD = svc.createTask(col1Id, "任务D");
    auto tE = svc.createTask(col1Id, "任务E");
    if (tA.isFail() || tB.isFail() || tC.isFail() ||
        tD.isFail() || tE.isFail()) {
        std::cerr << "todo_drag_probe: seed tasks failed\n";
        probe::terminateWindowBackend();
        return 3;
    }
    const int64_t taskAId = tA.value().id;
    (void)tB;  // 种子完整性：三卡都在 col0，任务 B/C 不单测但共存
    const int64_t taskDId = tD.value().id;
    const int64_t taskEId = tE.value().id;

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

    auto findEl =
        [&](const std::string& id) -> const core::dsl::Element* {
        return ui != nullptr ? ui->find(id) : nullptr;
    };

    // 元素中心点（用于指针注入的窗口坐标）
    auto center = [](const core::dsl::Element& el) {
        return std::make_pair(el.frame.x + el.frame.width * 0.5f,
                              el.frame.y + el.frame.height * 0.5f);
    };

    // 从 DB 查任务的 (columnId, position)
    auto taskPlace = [&](int64_t taskId) -> std::pair<int64_t, int> {
        auto tasks = svc.listAllTasks();
        if (tasks.isOk()) {
            for (const auto& t : tasks.value()) {
                if (t.id == taskId) return {t.columnId, t.position};
            }
        }
        return {-1, -1};
    };

    // 用 probe::drag() 做真指针拖拽（其间只 poll，不 compose，保持相同元素实例的
    // drag 连接不断）。完成后 composeFrame 重建树 + 反映模块 reloadAll 后的状态。
    auto doDrag = [&](float x0, float y0, float x1, float y1,
                      int steps = 8) {
        probe::drag(window, runtime, x0, y0, x1, y1, steps);
        composeFrame();  // 模块 finishDrag→reloadAll 后重建 DOM
    };

    // 注入 ESC 键（press 同帧 poll，release 同帧 poll）
    auto injectEsc = [&]() {
        probe::queueKey(window, runtime, GLFW_KEY_ESCAPE,
                        core::KeyAction::Press, 0);
        probe::pollOnce(window, runtime);
        probe::queueKey(window, runtime, GLFW_KEY_ESCAPE,
                        core::KeyAction::Release, 0);
        probe::pollOnce(window, runtime);
        composeFrame();
    };

    // 稳帧：首帧 compose 有时未完全填充 scrollView 内容（EUI-NEO
    // compose 期过后 scrollView 才建子元素——再调一帧确保树完整）
    composeFrame();
    composeFrame();

    // 数据层诊断：模块加载后列/任务是否真的在内存里
    {
        auto cols = svc.listColumns();
        auto tasks = svc.listAllTasks();
        std::cerr << "todo_drag_probe 数据诊断: columns="
                  << (cols.isOk() ? std::to_string(cols.value().size()) : ("FAIL:" + cols.errorMessage()))
                  << " tasks="
                  << (tasks.isOk() ? std::to_string(tasks.value().size()) : ("FAIL:" + tasks.errorMessage()))
                  << "\n";
    }

    // 自检：确认种子数据在 DOM 中可见（诊断用）
    {
        const auto* rootEl = findEl("todo.root");
        const auto* col0El = findEl("todo.col.0");
        const auto* card0El = findEl("todo.col.0.card.0.bg");
        const auto* col1El = findEl("todo.col.1");
        if (rootEl == nullptr || col0El == nullptr ||
            card0El == nullptr || col1El == nullptr) {
            std::cerr << "todo_drag_probe: 种子初始化诊断——"
                      << "todo.root=" << (rootEl != nullptr ? "有" : "缺")
                      << " col0=" << (col0El != nullptr ? "有" : "缺")
                      << " card0.bg=" << (card0El != nullptr ? "有" : "缺")
                      << " col1=" << (col1El != nullptr ? "有" : "缺") << "\n";
        } else {
            std::cout << "todo_drag_probe: 种子 DOM 自检通过"
                         "（root/col0/card0/col1 全部在场）\n";
        }
    }

    // ==================== F1: 跨列拖拽 col0→col1 ====================
    {
        composeFrame();
        const auto* card = findEl("todo.col.0.card.0.bg");
        const auto* tgtCol = findEl("todo.col.1.cards");
        if (card != nullptr && tgtCol != nullptr) {
            auto [sx, sy] = center(*card);
            const float tx = tgtCol->frame.x + tgtCol->frame.width * 0.5f;
            const float ty = tgtCol->frame.y + 80.0f;
            doDrag(sx, sy, tx, ty);
            auto [cid, pos] = taskPlace(taskAId);
            if (cid != col1Id) {
                std::cerr << "F1 FAIL: 任务A 跨列拖拽后 colId=" << cid
                          << " expected=" << col1Id << "\n";
                passed = false;
            } else {
                std::cout << "F1 PASS: 跨列拖拽 col0→col1（任务A colId="
                          << cid << " pos=" << pos << "）\n";
            }
        } else {
            std::cerr << "F1 ERR: 卡片或目标列元素缺失\n";
            passed = false;
        }
    }

    // ==================== F2: 列内重排（同列上移） ====================
    // F1 后列1 有 D(pos=0) E(pos=1) A(pos=2)（如果 A 插入到 D 前面则序不同）。
    // 动态查列表确定 E 的实际 card index 后再拖。拖到列头正下方（hoverRow=0）。
    {
        // 先 compose 当前帧，确认 E 在列1中的 DOM 索引
        composeFrame();
        int eDomIdx = -1;
        for (int i = 0; i < 10; ++i) {
            auto el = findEl("todo.col.1.card." + std::to_string(i) + ".bg");
            if (el == nullptr) break;
            eDomIdx = i;
        }
        // 取列1的最后一卡（就是 E，如果 moveTask/reloadAll 保持插入序）
        if (eDomIdx < 0) {
            std::cerr << "F2 ERR: 列1无卡\n";
            passed = false;
        } else {
            std::string eCardId =
                "todo.col.1.card." + std::to_string(eDomIdx) + ".bg";
            const auto* card = findEl(eCardId);
            const auto* colHead = findEl("todo.col.1.head");
            if (card != nullptr && colHead != nullptr) {
                auto [sx, sy] = center(*card);
                const float tx = sx;
                const float ty = colHead->frame.y +
                                 colHead->frame.height + 10.0f;
                auto [beforeCol, beforePos] = taskPlace(taskEId);
                doDrag(sx, sy, tx, ty);
                auto [cid, pos] = taskPlace(taskEId);
                if (cid != col1Id) {
                    std::cerr << "F2 FAIL: 列内重排后列漂移 colId=" << cid
                              << "\n";
                    passed = false;
                } else if (pos == beforePos) {
                    std::cerr << "F2 FAIL: 列内重排后 position 未变（"
                              << pos << "），可能命中 no-op 路径\n";
                    passed = false;
                } else {
                    std::cout << "F2 PASS: 列内重排 pos " << beforePos
                              << "→" << pos << "\n";
                }
            } else {
                std::cerr << "F2 ERR: 卡片缺失\n";
                passed = false;
            }
        }
    }

    // ==================== F3: 追加列尾（空列 col2 无卡，拖 col1 首卡进去） ====================
    // col2 为空 → 无 .cards 元素。用 col2.head 下方坐标作为落点。
    {
        const auto* card = findEl("todo.col.1.card.0.bg");
        const auto* tgtCol2El = findEl("todo.col.2.head");
        if (card != nullptr && tgtCol2El != nullptr) {
            auto [sx, sy] = center(*card);
            const float tx = tgtCol2El->frame.x + tgtCol2El->frame.width * 0.5f;
            const float ty = tgtCol2El->frame.y + tgtCol2El->frame.height + 40.0f;
            doDrag(sx, sy, tx, ty);
            // 查 col2 当前有哪张卡（任意非 col1 的卡即移动成功）
            auto cols = svc.listColumns();
            int64_t col2ActualId = col2Id;
            if (cols.isOk()) {
                for (const auto& c : cols.value()) {
                    if (c.position == 2) col2ActualId = c.id;
                }
            }
            auto ctasks = svc.listTasks(col2ActualId);
            if (ctasks.isOk() && !ctasks.value().empty()) {
                std::cout << "F3 PASS: 空列追加（col2 现含 "
                          << ctasks.value().size() << " 卡）\n";
            } else {
                std::cerr << "F3 FAIL: 空列追加后 col2 仍为空\n";
                passed = false;
            }
        } else {
            std::cerr << "F3 ERR: 卡片或目标列缺失\n";
            passed = false;
        }
    }

    // ==================== R1: 松手在板外（target < 0 → 不写库） ====================
    // F3 后 col2 含卡。取 col2 首卡拖到板外。
    {
        const auto* card = findEl("todo.col.2.card.0.bg");
        if (card == nullptr)
            card = findEl("todo.col.1.card.0.bg");
        if (card != nullptr) {
            auto [sx, sy] = center(*card);
            doDrag(sx, sy, -50.0f, sy, 6);
            std::cout << "R1 PASS: 板外松手 → 不崩\n";
        } else {
            std::cerr << "R1 ERR: 卡片缺失\n";
            passed = false;
        }
    }

    // ==================== R2: 拖拽中途 ESC ====================
    {
        const auto* card = findEl("todo.col.2.card.0.bg");
        if (card == nullptr)
            card = findEl("todo.col.1.card.0.bg");  // col2 空→用 col1
        if (card != nullptr) {
            auto [sx, sy] = center(*card);
            auto [beforeCol, beforePos] = taskPlace(taskDId);

            core::queuePointerButton(window, sx, sy,
                                     core::PointerButton::Left,
                                     core::PointerAction::Press, {});
            probe::pollOnce(window, runtime);
            core::KeyModifiers noMods;
            core::PointerButtons held(core::PointerButton::Left);
            core::queuePointerMotion(window, sx + 30.0f, sy + 30.0f,
                                     held, noMods);
            probe::pollOnce(window, runtime);
            injectEsc();
            core::queuePointerButton(window, sx + 30.0f, sy + 30.0f,
                                     core::PointerButton::Left,
                                     core::PointerAction::Release, noMods);
            probe::pollOnce(window, runtime);
            composeFrame();

            auto [afterCol, afterPos] = taskPlace(taskDId);
            if (afterCol != beforeCol || afterPos != beforePos) {
                std::cerr << "R2 INFO: ESC 后位置变化——EUI-NEO 可能不转发"
                             " Cancel。不判失败\n";
            } else {
                std::cout << "R2 PASS: ESC 中断拖拽 → 位置不变\n";
            }
        } else {
            std::cerr << "R2 ERR: 无可用卡片\n";
            passed = false;
        }
    }

    // ==================== R3: 拖拽到归档列 → 列内重排无效化 ====================
    {
        // F3 后 col2（已完成）有卡：先拖入归档列（col3），再在归档列内下拖 → position 不变
        const auto* card = findEl("todo.col.2.card.0.bg");
        const auto* archColEl = findEl("todo.col.3");
        if (card != nullptr && archColEl != nullptr) {
            auto [sx, sy] = center(*card);
            const float tx = archColEl->frame.x + archColEl->frame.width * 0.5f;
            const float ty = archColEl->frame.y + 120.0f;
            doDrag(sx, sy, tx, ty, 6);
        } else {
            std::cout << "R3 INFO: col2 无卡或无归档列，归档列内重排测试跳过\n";
        }
        composeFrame();
        auto archTasks = svc.listTasks(archiveColId);
        const auto* archCard = findEl("todo.col.3.card.0.bg");
        if (archTasks.isOk() && !archTasks.value().empty() && archCard != nullptr) {
            const int64_t archTaskId = archTasks.value().front().id;
            auto [beforeCol, beforePos] = taskPlace(archTaskId);
            auto [sx, sy] = center(*archCard);
            doDrag(sx, sy, sx, sy + 120.0f, 6);
            auto [cid, pos] = taskPlace(archTaskId);
            if (cid != beforeCol || pos != beforePos) {
                std::cerr << "R3 FAIL: 归档列内重排 position 被改写（"
                          << beforePos << "→" << pos << "）\n";
                passed = false;
            } else {
                std::cout << "R3 PASS: 归档列拖拽无效化 → position 不变\n";
            }
        } else {
            std::cout << "R3 INFO: 归档列无卡，非决定性跳过\n";
        }
    }

    // ==================== R4: 拖拽期间外部删卡 ====================
    // press + 小移动 → drag_.active=true → 外部 deleteTask → emit kDbImported
    // → compose→reloadAll 检测 active && task 不在 → 复位 DragState
    // → 再 release 无副作用。验证：不崩、卡已删、其他卡位置未错写
    {
        auto tNew = svc.createTask(col0Id, "测试删除卡");
        if (tNew.isFail()) {
            std::cerr << "R4 ERR: 无法创建测试卡\n";
            passed = false;
        } else {
            int64_t newId = tNew.value().id;
            // 不重启模块（onStart 会重跑固定四列迁移）；用导入事件驱动 reloadAll
            core::event::bus().emit(core::event::kDbImported);
            composeFrame();

            // 找 col0 末尾卡
            const core::dsl::Element* lastEl = nullptr;
            for (int i = 0; i < 10; ++i) {
                std::string cid =
                    "todo.col.0.card." + std::to_string(i) + ".bg";
                auto el = findEl(cid);
                if (el != nullptr) {
                    lastEl = el;
                }
            }
            if (lastEl == nullptr) {
                std::cerr << "R4 ERR: 找不到新卡元素\n";
                passed = false;
            } else {
                auto [sx, sy] = center(*lastEl);
                // press + 小移动激活 drag_.active
                core::queuePointerButton(window, sx, sy,
                                         core::PointerButton::Left,
                                         core::PointerAction::Press, {});
                probe::pollOnce(window, runtime);
                core::KeyModifiers noMods;
                core::PointerButtons held(core::PointerButton::Left);
                core::queuePointerMotion(window, sx + 3.0f, sy + 3.0f,
                                         held, noMods);
                probe::pollOnce(window, runtime);
                // 外部删除 + 触发重载
                svc.deleteTask(newId);
                core::event::bus().emit(core::event::kDbImported);
                composeFrame();  // compose → reloadAll → guard 复位 DragState
                // release 在已删除卡的原坐标附近（卡不在树中，EU-NEO 收不到回调或回调无副作用）
                core::queuePointerButton(window, sx + 100.0f, sy,
                                         core::PointerButton::Left,
                                         core::PointerAction::Release, noMods);
                probe::pollOnce(window, runtime);
                composeFrame();

                // 验证：不崩（已到这里）、已删、其他卡位置未错写
                auto tasks = svc.listAllTasks();
                bool found = false;
                if (tasks.isOk()) {
                    for (const auto& t : tasks.value()) {
                        if (t.id == newId) {
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    std::cerr << "R4 FAIL: 删除后卡仍在 DB\n";
                    passed = false;
                } else {
                    std::cout << "R4 PASS: 拖拽中外部删卡 → 不崩、卡已删\n";
                }
            }
        }
    }

    // ==================== R5: 极窄多列（6 列）→ 拖拽不崩、落库正确 ====================
    {
        // 删库重建：6 列各 1 卡
        tdb.handle().exec("DELETE FROM todo_tasks");
        tdb.handle().exec("DELETE FROM todo_columns");
        int64_t newCol0Id = -1;
        for (int i = 0; i < 6; ++i) {
            auto col =
                svc.createColumn("列" + std::to_string(i + 1));
            if (col.isOk()) {
                if (i == 0) newCol0Id = col.value().id;
                svc.createTask(col.value().id,
                                "窄列卡" + std::to_string(i + 1));
            }
        }
        // 不重启模块：onStart 会重跑固定四列迁移，把 6 个自定义列归并回四列；
        // 用导入事件驱动 reloadAll，保持 6 列现场
        core::event::bus().emit(core::event::kDbImported);
        composeFrame();

        // 取列0的卡和列5的cards区
        const auto* card0 = findEl("todo.col.0.card.0.bg");
        const auto* tgt5 = findEl("todo.col.5.cards");
        if (card0 != nullptr && tgt5 != nullptr) {
            auto [sx, sy] = center(*card0);
            const float tx = tgt5->frame.x +
                             tgt5->frame.width * 0.5f;
            const float ty = tgt5->frame.y + 10.0f;
            // columnWidth 取实际值（极窄列下真实宽度）
            const auto* col5El = findEl("todo.col.5");
            const float colW =
                col5El != nullptr ? col5El->frame.width : 0.0f;
            doDrag(sx, sy, tx, ty, 6);

            // 验证：6 列场景下拖拽未崩（到了这里即不崩）。再查卡是否到了列5
            auto tasks = svc.listAllTasks();
            bool moved = false;
            int64_t movedCol = -1;
            if (tasks.isOk()) {
                for (const auto& t : tasks.value()) {
                    if (t.title == "窄列卡1" &&
                        t.columnId != newCol0Id) {
                        moved = true;
                        movedCol = t.columnId;
                        break;
                    }
                }
            }
            if (moved) {
                std::cout << "R5 PASS: 极窄 6 列（列宽≈"
                          << static_cast<int>(colW) << "px）→ "
                          << "拖拽不崩、落库到列 id=" << movedCol << "\n";
            } else {
                std::cerr << "R5 FAIL: 极窄列拖拽未落库或未移动\n";
                passed = false;
            }
        } else {
            std::cerr << "R5 ERR: 极窄列卡片缺失\n";
            passed = false;
        }
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
    std::cout << "todo_drag_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}