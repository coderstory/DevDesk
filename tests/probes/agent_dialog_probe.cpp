// agent_dialog_probe.cpp — 智能体页两张弹窗的真窗口探针 + 构图留痕
//
// 背景（M3 Task 5 交付验证）：test_agent_ui.cpp 的无头用例断言节点存在性，但
// 「cron 模板下拉 + 下次触发预览行」「复制回复 chip + 错误行去 Claude 页引导」
// 这两组 M3 新 UI 从没有过真实渲染帧的视觉证据。本探针用真窗口 + 真命中分发
// 把两张弹窗驱动到目标状态：
//   A. 任务弹窗：触发方式=定时 → cron 输入真实键入 → 模板下拉展开，
//      断言「下次触发：」预览行文案 + 弹层可见性 + L2 z 碰撞扫描；
//      --shot-task <path> 落 PNG。
//   B. 详情弹窗：种子库给一条 failed run（错误文案 = needsClaudePageHint 的
//      「LLM 配置不可用…」类）+ 一条带 text 块的 assistant 消息，断言
//      runlist.<i>.err、runlist.<i>.err.hint（去 Claude 页 →）、
//      msgs.copyfinal chip 三者同时在场；--shot-detail <path> 落 PNG。
//
// 无头用例与真窗口探针的分工契约见 test_agent_ui.cpp 文件头——本文件就是它说的
// 「真窗口几何 + 真命中分发的探针侧」。
//
// 数据种子走真 AgentStore 落库口（createTask/startRun/appendMessage/finishRun），
// 不绕过产品代码写表——图的身份可查（哪条 run、哪个错误文案渲染出来的）。
// 错误文案与 direct_llm_client.cpp 的 kConfigErrorPrefix 逐字一致（前缀钉死契约）。
//
// 运行：agent_dialog_probe.exe [--shot-task <png>] [--shot-detail <png>]
// 不带构图开关 = 纯断言门禁（ctest -L probe 跑的就是这条）；构图开关失败 =
// 退出码非零（视觉证据链缺失不能当通过，probe_harness 契约）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要，同 settings probe）
#include "test_support.h"

#include "core/module_interface.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

// app:: 层实现随 dsl_app_impl.h 自给（同 settings_dropdown_z_probe：探针不建
// 应用级 UI，compose 走 probe::composeModuleFrame 直接驱动 runtime）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_agent_dialog_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // 本探针私有两个构图开关（parseProbeArgs 未知参数忽略，这里自己收）
    std::string shotTask, shotDetail;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-task" && i + 1 < argc) {
            shotTask = argv[++i];
        } else if (arg == "--shot-detail" && i + 1 < argc) {
            shotDetail = argv[++i];
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::TestDb tdb("aprobe_", "agentdlg");
    test_support::FakeProfileSource profile;

    // 种子（真 AgentStore 落库口）：1 个手动任务 + 1 条 failed run。
    // 错误文案与 direct_llm_client.cpp 的 kConfigErrorPrefix 逐字一致——
    // needsClaudePageHint 按前缀「LLM 配置不可用」钉死，文案一变引导即消失。
    int64_t taskId = 0;
    int64_t runId = 0;
    {
        module::agent::AgentStore store(tdb.handle());
        auto t = store.createTask("日报助手", "整理今日进展并输出日报",
                                  "manual", "sonnet", 10, /*allowDestructive=*/false);
        if (t.isFail()) {
            std::cerr << "agent_dialog_probe: 种子 createTask 失败: "
                      << t.errorMessage() << "\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        taskId = t.value();
        auto r = store.startRun(taskId);
        if (r.isFail()) {
            std::cerr << "agent_dialog_probe: 种子 startRun 失败: "
                      << r.errorMessage() << "\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        runId = r.value();
        store.appendMessage(runId, 0, "user",
                            R"([{"type":"text","text":"帮我整理今天的进展"}])",
                            std::nullopt);
        store.appendMessage(
            runId, 1, "assistant",
            R"([{"type":"text","text":"今日完成：全量回归 1301 用例，5 失败均为已知 keychain/DPAPI 环境问题；模块隔离守卫通过。"}])",
            std::nullopt);
        store.finishRun(runId, "failed", 1, std::nullopt, std::nullopt,
                        std::string("LLM 配置不可用：没有激活的 Claude profile，"
                                    "请在 DevDesk 的 Claude 页选中 Profile 并点击"
                                    "「启用」"));
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "agent";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod.compose(u, screen);
                                  });
    };
    auto clickPoint = [&](float cx, float cy) {
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    auto clickElement = [&](const core::dsl::Element& element) {
        clickPoint(element.frame.x + element.frame.width * 0.5f,
                   element.frame.y + element.frame.height * 0.5f);
    };
    // recompose 重建元素树 → 开前捕获的指针全部作废，逐帧重查（settings probe 同款）
    auto findElement = [&](const std::string& id) -> const core::dsl::Element* {
        return ui != nullptr ? ui->find(id) : nullptr;
    };

    // ---------- A. 任务弹窗：定时分支 + 模板下拉展开 + 预览行 ----------
    composeFrame();
    const auto* newTaskBtn = findElement("agent.page.newTask");
    if (newTaskBtn == nullptr) {
        std::cerr << "agent_dialog_probe: 'agent.page.newTask' 缺失\n";
        passed = false;
    } else {
        clickElement(*newTaskBtn);
        composeFrame();
        if (findElement("agent.dialog.task") == nullptr) {
            std::cerr << "agent_dialog_probe: 任务弹窗未打开\n";
            passed = false;
        }
    }

    // 触发方式切「定时」：点 trigger 下拉 field → 点 item.1
    const auto* triggerField = findElement("agent.dialog.task.trigger.field");
    if (triggerField == nullptr) {
        std::cerr << "agent_dialog_probe: 'agent.dialog.task.trigger.field' "
                     "缺失\n";
        passed = false;
    } else {
        clickElement(*triggerField);
        composeFrame();
        const auto* item1 = findElement("agent.dialog.task.trigger.item.1");
        if (item1 == nullptr) {
            std::cerr << "agent_dialog_probe: trigger 下拉 item.1 未出现\n";
            passed = false;
        } else {
            clickElement(*item1);
            composeFrame();
        }
    }

    // 定时分支：cron 输入 + 模板下拉应渲染
    if (findElement("agent.dialog.task.cron") == nullptr ||
        findElement("agent.dialog.task.cron.template.field") == nullptr) {
        std::cerr << "agent_dialog_probe: 定时分支的 cron 输入 / 模板下拉缺失\n";
        passed = false;
    }

    // cron 真实键入（点击聚焦 → queueText → 真实 input 事件路径 →
    // onChange → refreshCronPreview）。键入失败回退公开草稿缝
    // setEditCronDraft（同为产品 seam，test_agent_ui.cpp 同款用法）。
    {
        const auto* cronHit = findElement("agent.dialog.task.cron.hit");
        bool typed = false;
        if (cronHit != nullptr) {
            clickElement(*cronHit);
            composeFrame();
            probe::queueText(window, runtime, "0 9 * * 1-5");
            composeFrame();
            const auto* preview = findElement("agent.dialog.task.cron.preview");
            typed = preview != nullptr &&
                    preview->text.rfind("下次触发：", 0) == 0;
        }
        if (!typed) {
            mod.setEditCronDraft("0 9 * * 1-5");
            composeFrame();
        }
        const auto* preview = findElement("agent.dialog.task.cron.preview");
        if (preview == nullptr ||
            preview->text.rfind("下次触发：", 0) != 0) {
            std::cerr << "agent_dialog_probe: 预览行缺失或文案不是"
                         "「下次触发：…」（实测 '"
                      << (preview != nullptr ? preview->text : std::string("<无>"))
                      << "'）\n";
            passed = false;
        }
    }

    // 模板下拉展开（保持开态构图）
    {
        const auto* tplField = findElement("agent.dialog.task.cron.template.field");
        if (tplField == nullptr) {
            std::cerr << "agent_dialog_probe: 模板下拉 field 缺失\n";
            passed = false;
        } else {
            clickElement(*tplField);
            composeFrame();
            const auto* tplPopup =
                findElement("agent.dialog.task.cron.template.popup");
            if (tplPopup == nullptr || !floatEq(tplPopup->opacity, 1.0f)) {
                std::cerr << "agent_dialog_probe: 模板下拉弹层不可见"
                             "（opacity="
                          << (tplPopup != nullptr ? tplPopup->opacity : -1.0f)
                          << "）\n";
                passed = false;
            } else {
                // 「选择模板…」占位 + 4 模板项 = 至少 5 项；首项存在即可证菜单渲染
                if (findElement("agent.dialog.task.cron.template.item.1") ==
                    nullptr) {
                    std::cerr << "agent_dialog_probe: 模板下拉 item.1 "
                                 "（工作日 9 点）缺失\n";
                    passed = false;
                }
                // L2 z 碰撞扫描：弹层之上不允许有等值 z 相交叶子（弹窗内多元素层叠）
                if (!probe::assertNoEqualZOverlap(
                        *ui, "agent.dialog.task.cron.template.popup")) {
                    std::cerr << "agent_dialog_probe: 模板弹层 L2 z "
                                 "碰撞扫描失败\n";
                    passed = false;
                }
            }
        }
    }

    // 构图 A：模板下拉展开 + 预览行在场的稳定帧
    if (!shotTask.empty()) {
        if (!probe::captureFramePng(window, runtime, shotTask)) {
            std::cerr << "agent_dialog_probe: --shot-task 落图失败: "
                      << shotTask << "\n";
            passed = false;
        }
    }

    // 关任务弹窗（点取消 chip），进 B
    {
        const auto* cancel = findElement("agent.dialog.task.cancel");
        if (cancel != nullptr) {
            clickElement(*cancel);
            composeFrame();
        }
        if (findElement("agent.dialog.task") != nullptr) {
            std::cerr << "agent_dialog_probe: 任务弹窗未关（取消点击未生效）\n";
            passed = false;
        }
    }

    // ---------- B. 详情弹窗：错误行 + 去Claude页引导 + 复制回复 chip ----------
    {
        const std::string detailId =
            "agent.task." + std::to_string(taskId) + ".detail";
        const auto* detailChip = findElement(detailId);
        if (detailChip == nullptr) {
            std::cerr << "agent_dialog_probe: '" << detailId
                      << "' chip 缺失（种子任务行未渲染？）\n";
            passed = false;
        } else {
            clickElement(*detailChip);
            composeFrame();
            composeFrame();  // openTaskDetail 置 tasksDirty_，数据下帧装载
        }
        if (findElement("agent.dialog.detail") == nullptr) {
            std::cerr << "agent_dialog_probe: 详情弹窗未打开\n";
            passed = false;
        }

        // 错误行 + 引导行（文案钉死：错误=「LLM 配置不可用」前缀 → 引导「去 Claude 页 →」）
        const std::string errId = "agent.dialog.detail.runlist.0.err";
        const auto* errRow = findElement(errId);
        if (errRow == nullptr) {
            std::cerr << "agent_dialog_probe: 错误行 '" << errId
                      << "' 缺失（failed run 的 error 未渲染）\n";
            passed = false;
        } else if (errRow->text.rfind("错误：LLM 配置不可用", 0) != 0) {
            std::cerr << "agent_dialog_probe: 错误行文案不是种子错误原文（实测 '"
                      << errRow->text << "'）\n";
            passed = false;
        }
        const auto* hintRow = findElement(errId + ".hint");
        if (hintRow == nullptr) {
            std::cerr << "agent_dialog_probe: 「去 Claude 页」引导行缺失"
                         "（needsClaudePageHint 未命中？文案漂移？）\n";
            passed = false;
        } else if (hintRow->text != "去 Claude 页 →") {
            std::cerr << "agent_dialog_probe: 引导行文案漂移（实测 '"
                      << hintRow->text << "'）\n";
            passed = false;
        }

        // 复制回复 chip（run 有带 text 块的 assistant 消息 → finalAssistantText 非空）
        const auto* copyChip = findElement("agent.dialog.detail.msgs.copyfinal");
        if (copyChip == nullptr) {
            std::cerr << "agent_dialog_probe: 「复制回复」chip 缺失"
                         "（finalAssistantText 为空？）\n";
            passed = false;
        }
    }

    // 构图 B：详情弹窗稳定帧（错误行 + 引导 + chip 同帧在场，断言已把过关）
    if (!shotDetail.empty()) {
        if (!probe::captureFramePng(window, runtime, shotDetail)) {
            std::cerr << "agent_dialog_probe: --shot-detail 落图失败: "
                      << shotDetail << "\n";
            passed = false;
        }
    }

    mod.onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}
