// agent_dialog_probe.cpp — 智能体页两张弹窗的真窗口探针 + 构图留痕
//
// 背景（M3 Task 5 交付验证）：test_agent_ui.cpp 的无头用例断言节点存在性，但
// 「cron 模板下拉 + 下次触发预览行」「复制回复 chip + 错误行去 Claude 页引导」
// 这两组 M3 新 UI 从没有过真实渲染帧的视觉证据。本探针用真窗口 + 真命中分发
// 把两张弹窗驱动到目标状态：
//   A. 任务弹窗：触发方式=定时 → cron 输入真实键入 → 模板下拉展开，
//      断言「下次触发：」预览行文案 + 弹层可见性 + L2 z 碰撞扫描；
//      --shot-task <path> 落 PNG（弹层展开态——审阅弹层 z 序用）；
//      --shot-task-plain <path> 在开弹层前落 PNG（无覆盖帧——行为开关行
//      「允许破坏性操作 / 只跑一次」不被模板弹层盖住，审阅开关本身用）。
//   B. 详情弹窗：种子库给一条 failed run（错误文案 = needsClaudePageHint 的
//      「LLM 配置不可用…」类）+ 一条带 text 块的 assistant 消息，断言
//      runlist.<i>.err、runlist.<i>.err.hint（去 Claude 页 →）、
//      msgs.copyfinal chip 三者同时在场；--shot-detail <path> 落 PNG。
//   C. AI 草稿子弹窗双层 z 序（G5，ui-test-gap-map）：任务弹窗之上叠开
//      「AI 生成草稿」子弹窗，断言层级契约——子弹窗根 zIndex 严格大于外层、
//      遮罩全屏启用、L2 z 碰撞扫描；失败态（空输入校验错 / 生成中 / 请求失败）
//      逐态复扫；遮罩命中序用行为证明（点外层暴露区必关子弹窗而非穿透）；
//      子弹窗关闭后外层任务弹窗完好。--shot-draft <path> 落双层稳定帧，
//      --shot-draft-error <path> 落校验错误态帧。
//      判据形态：两张弹窗各自是 compose 顶层根（无公共祖先），assertDrawnAbove
//      不适用；「子弹窗在上」= 根 zIndex 数值契约（roots 参与 z 稳定排序，
//      等值时退化为 DOM 序——这正是要钉死的脆弱点）+ 遮罩点击穿透行为证明。
//
// 无头用例与真窗口探针的分工契约见 test_agent_ui.cpp 文件头——本文件就是它说的
// 「真窗口几何 + 真命中分发的探针侧」。
//
// 数据种子走真 AgentStore 落库口（createTask/startRun/appendMessage/finishRun），
// 不绕过产品代码写表——图的身份可查（哪条 run、哪个错误文案渲染出来的）。
// 错误文案与 direct_llm_client.cpp 的 kConfigErrorPrefix 逐字一致（前缀钉死契约）。
// C 段的 LLM 通道指向本机拒连端口（127.0.0.1:9，FakeProfileSource.baseUrl 改写），
// 生成 worker 秒败且**绝不出真网**。
//
// 运行：agent_dialog_probe.exe [--shot-task <png>] [--shot-task-plain <png>]
//                    [--shot-detail <png>] [--shot-draft <png>]
//                    [--shot-draft-error <png>]
// 不带构图开关 = 纯断言门禁（ctest -L probe 跑的就是这条）；构图开关失败 =
// 退出码非零（视觉证据链缺失不能当通过，probe_harness 契约）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要，同 settings probe）
#include "test_support.h"

#include "core/module_interface.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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
    // 本探针私有构图开关（parseProbeArgs 未知参数忽略，这里自己收）
    std::string shotTask, shotTaskPlain, shotDetail, shotDraft, shotDraftError;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-task" && i + 1 < argc) {
            shotTask = argv[++i];
        } else if (arg == "--shot-task-plain" && i + 1 < argc) {
            shotTaskPlain = argv[++i];
        } else if (arg == "--shot-detail" && i + 1 < argc) {
            shotDetail = argv[++i];
        } else if (arg == "--shot-draft" && i + 1 < argc) {
            shotDraft = argv[++i];
        } else if (arg == "--shot-draft-error" && i + 1 < argc) {
            shotDraftError = argv[++i];
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
    // C 段会真点「生成草稿」→ startDraftGeneration 起 worker 发 LLM 请求。
    // baseUrl 改写成本机拒连端口：worker 秒败、零真网流量（默认的
    // api.anthropic.com 会让探针打真外网——绝不允许）。不得绑 8888（本会话推理链路）。
    profile.profile.baseUrl = "http://127.0.0.1:9";

    // 种子（真 AgentStore 落库口）：1 个手动任务 + 1 条 failed run + 1 个
    // max_runs=1 的定时任务（列表行「一次性」徽章的审阅帧来源，不派发——probe
    // 只渲染不跑调度）。错误文案与 direct_llm_client.cpp 的 kConfigErrorPrefix
    // 逐字一致——needsClaudePageHint 按前缀「LLM 配置不可用」钉死，文案一变引导即消失。
    int64_t taskId = 0;
    int64_t runId = 0;
    {
        module::agent::AgentStore store(tdb.handle());
        auto once = store.createTask("日报推送（只跑一次）", "把今日进展推送到笔记",
                                     "schedule", "default", 10, false,
                                     R"({"cron":"0 18 * * *","max_runs":1})");
        if (once.isFail()) {
            std::cerr << "agent_dialog_probe: 种子（限次任务）createTask 失败: "
                      << once.errorMessage() << "\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
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

    // ---------- C. AI 草稿子弹窗双层 z 序（G5） ----------
    // 前置：任务弹窗开着（A 段已切定时 + cron 预览）。子弹窗关闭后外层保持
    // 完好，A 段后续的模板下拉交互即外层可交互的活性证明。
    {
        const auto* chip = findElement("agent.dialog.task.aichip.chip");
        if (chip == nullptr) {
            std::cerr << "agent_dialog_probe: AI 草稿 chip 缺失\n";
            passed = false;
        } else {
            clickElement(*chip);
            composeFrame();
            for (int i = 0; i < 24; ++i) probe::pollOnce(window, runtime);
            composeFrame();  // 开窗过渡动画收敛后再断言（半透明帧只存在于过渡期）
        }
        const auto* draftRoot = findElement("agent.dialog.task.draft");
        const auto* draftPanel = findElement("agent.dialog.task.draft.panel");
        const auto* draftBackdrop = findElement("agent.dialog.task.draft.backdrop");
        const auto* taskRoot = findElement("agent.dialog.task");
        const auto* taskPanel = findElement("agent.dialog.task.panel");
        if (draftRoot == nullptr || draftPanel == nullptr ||
            draftBackdrop == nullptr || taskRoot == nullptr ||
            taskPanel == nullptr) {
            std::cerr << "agent_dialog_probe: 双层弹窗元素缺失（draft "
                         "root/panel/backdrop、task root/panel）\n";
            passed = false;
        } else {
            // 子弹窗在上：根 zIndex 严格大于外层（roots 参与 z 稳定排序；
            // 等值时排序退化为 DOM 序，靠 compose 先后侥幸在上——正是要钉死的点）
            if (draftRoot->zIndex <= taskRoot->zIndex) {
                std::cerr << "agent_dialog_probe: 草稿子弹窗根 zIndex("
                          << draftRoot->zIndex << ") 未严格大于任务弹窗根("
                          << taskRoot->zIndex << ")\n";
                passed = false;
            }
            if (!floatEq(draftPanel->opacity, 1.0f)) {
                std::cerr << "agent_dialog_probe: 草稿面板 opacity="
                          << draftPanel->opacity << "（期望收敛到 1.0）\n";
                passed = false;
            }
            // 遮罩：全屏、启用、不透明（挡外层命中的实体）
            if (draftBackdrop->disabled || !floatEq(draftBackdrop->opacity, 1.0f) ||
                draftBackdrop->frame.x > 0.5f || draftBackdrop->frame.y > 0.5f ||
                draftBackdrop->frame.x + draftBackdrop->frame.width < w - 0.5f ||
                draftBackdrop->frame.y + draftBackdrop->frame.height < h - 0.5f) {
                std::cerr << "agent_dialog_probe: 草稿遮罩未全屏启用（disabled="
                          << draftBackdrop->disabled
                          << " opacity=" << draftBackdrop->opacity << " frame="
                          << draftBackdrop->frame.x << ","
                          << draftBackdrop->frame.y << " "
                          << draftBackdrop->frame.width << "x"
                          << draftBackdrop->frame.height << "）\n";
                passed = false;
            }
            // 外层面板不被误禁用/隐没——拦截靠 z 序，不靠禁用外层
            if (taskPanel->disabled || !floatEq(taskPanel->opacity, 1.0f)) {
                std::cerr << "agent_dialog_probe: 外层任务面板被误禁用/隐没"
                             "（disabled="
                          << taskPanel->disabled << "）\n";
                passed = false;
            }
            if (!probe::assertNoEqualZOverlap(*ui, "agent.dialog.task.draft")) {
                std::cerr << "agent_dialog_probe: 双层弹窗 L2 z 碰撞扫描失败\n";
                passed = false;
            }
        }

        // 构图 C1：双层弹窗稳定帧
        if (!shotDraft.empty() &&
            !probe::captureFramePng(window, runtime, shotDraft)) {
            std::cerr << "agent_dialog_probe: --shot-draft 落图失败: "
                      << shotDraft << "\n";
            passed = false;
        }

        // 失败态 1：空输入校验错（同步路径，零线程，确定性）
        {
            const auto* gen = findElement("agent.dialog.task.draft.generate");
            if (gen == nullptr) {
                std::cerr << "agent_dialog_probe: 生成按钮缺失\n";
                passed = false;
            } else {
                clickElement(*gen);
                composeFrame();
                const auto* err = findElement("agent.dialog.task.draft.error");
                if (err == nullptr || err->text != "请先用一句话描述任务") {
                    std::cerr << "agent_dialog_probe: 空输入校验错误行缺失或"
                                 "文案漂移（实测 '"
                              << (err != nullptr ? err->text : std::string("<无>"))
                              << "'）\n";
                    passed = false;
                }
                if (!probe::assertNoEqualZOverlap(*ui,
                                                  "agent.dialog.task.draft")) {
                    std::cerr << "agent_dialog_probe: 校验错误态 L2 z 碰撞扫描"
                                 "失败\n";
                    passed = false;
                }
            }
        }

        // 构图 C2：校验错误行在场的失败态帧
        if (!shotDraftError.empty() &&
            !probe::captureFramePng(window, runtime, shotDraftError)) {
            std::cerr << "agent_dialog_probe: --shot-draft-error 落图失败: "
                      << shotDraftError << "\n";
            passed = false;
        }

        // 失败态 2 + 3：生成中（真 worker 在途）→ 请求失败错误行。
        // LLM 通道已指向本机拒连端口（见 profile.baseUrl），worker 秒败。
        {
            const auto* inputHit = findElement("agent.dialog.task.draft.input.hit");
            if (inputHit == nullptr) {
                std::cerr << "agent_dialog_probe: 草稿输入框 hit 区缺失\n";
                passed = false;
            } else {
                clickElement(*inputHit);
                composeFrame();
                probe::queueText(window, runtime, "把逾期的待办全部完成");
                composeFrame();
            }
            const auto* gen = findElement("agent.dialog.task.draft.generate");
            if (gen != nullptr) {
                clickElement(*gen);
                // 同帧 compose 呈现生成中态：worker 从 spawn 到失败至少一次
                // curl 建连（ms 级），update→compose 间距 µs 级，收不到割
                composeFrame();
                const auto* genBusy =
                    findElement("agent.dialog.task.draft.generate.bg");
                const auto* genLabel =
                    findElement("agent.dialog.task.draft.generate.text");
                // button 组件的 disabled 落在 .bg、文案落在 .text（根节点两者皆无）
                if (genBusy == nullptr || !genBusy->disabled || genLabel == nullptr ||
                    genLabel->text != "生成中…") {
                    std::cerr << "agent_dialog_probe: 生成中态未呈现（bg.disabled="
                              << (genBusy != nullptr ? genBusy->disabled : false)
                              << " label='"
                              << (genLabel != nullptr ? genLabel->text : "<无>")
                              << "'）\n";
                    passed = false;
                }
                if (!probe::assertNoEqualZOverlap(*ui,
                                                  "agent.dialog.task.draft")) {
                    std::cerr << "agent_dialog_probe: 生成中态 L2 z 碰撞扫描"
                                 "失败\n";
                    passed = false;
                }
                // 等 worker 失败被收割（本机拒连 <100ms 量级；上限 ~2s）
                bool failedShown = false;
                for (int i = 0; i < 200 && !failedShown; ++i) {
                    composeFrame();
                    const auto* err = findElement("agent.dialog.task.draft.error");
                    failedShown = err != nullptr && !err->text.empty();
                    if (!failedShown) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                const auto* err = findElement("agent.dialog.task.draft.error");
                if (err == nullptr || err->text.empty()) {
                    std::cerr << "agent_dialog_probe: 请求失败错误行未出现"
                                 "（拒连端口也该失败——环境异常？）\n";
                    passed = false;
                }
                if (!probe::assertNoEqualZOverlap(*ui,
                                                  "agent.dialog.task.draft")) {
                    std::cerr << "agent_dialog_probe: 请求失败态 L2 z 碰撞扫描"
                                 "失败\n";
                    passed = false;
                }
            }
        }

        // 遮罩命中序（行为证明）：任务弹窗「取消」钮暴露在草稿面板之下方
        // （几何不重叠）。点它必须命中草稿遮罩 → 只关子弹窗；若穿透到外层
        // 按钮，任务弹窗会被误关——两种错各给一条独立断言。
        {
            const auto* cancel = findElement("agent.dialog.task.cancel");
            if (cancel == nullptr) {
                std::cerr << "agent_dialog_probe: 任务弹窗取消钮缺失"
                             "（遮罩命中序无从验证）\n";
                passed = false;
            } else {
                clickElement(*cancel);
                composeFrame();
                if (findElement("agent.dialog.task.draft") != nullptr) {
                    std::cerr << "agent_dialog_probe: 点外层暴露区未关草稿"
                                 "子弹窗（遮罩命中序破损：点击穿透）\n";
                    passed = false;
                }
                if (findElement("agent.dialog.task") == nullptr) {
                    std::cerr << "agent_dialog_probe: 遮罩点击误关了外层"
                                 "任务弹窗\n";
                    passed = false;
                }
            }
        }
        // 外层完好：子弹窗关闭后任务弹窗面板仍在且可见
        const auto* taskPanelAfter = findElement("agent.dialog.task.panel");
        if (taskPanelAfter == nullptr ||
            !floatEq(taskPanelAfter->opacity, 1.0f)) {
            std::cerr << "agent_dialog_probe: 子弹窗关闭后外层任务弹窗面板"
                         "缺失/不可见\n";
            passed = false;
        }
    }

    // 构图 A0：任务弹窗无弹层帧（弹层还没开——行为开关行的「只跑一次」可见）。
    // 与 A 的分工：A 审弹层 z 序，A0 审开关行本体（模态遮挡下看不见的就落这里）
    if (!shotTaskPlain.empty()) {
        if (!probe::captureFramePng(window, runtime, shotTaskPlain)) {
            std::cerr << "agent_dialog_probe: --shot-task-plain 落图失败: "
                      << shotTaskPlain << "\n";
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

        // P2：时间线行三列 + 摘要三卡 + 右栏详情块（spec §12.4 P2 子集）
        const auto* rowTime = findElement("agent.dialog.detail.runlist.0.time");
        if (rowTime == nullptr) {
            std::cerr << "agent_dialog_probe: '.runlist.0.time' 缺失"
                         "（时间线行未容器化？）\n";
            passed = false;
        } else if (rowTime->text.size() != 14 || rowTime->text[2] != '-' ||
                   rowTime->text[5] != ' ') {
            std::cerr << "agent_dialog_probe: .runlist.0.time 应为 MM-DD HH:MM:SS"
                         "（14 字符），实测 '"
                      << rowTime->text << "'\n";
            passed = false;
        }
        const auto* rowStatus = findElement("agent.dialog.detail.runlist.0.status");
        if (rowStatus == nullptr || rowStatus->text.empty()) {
            std::cerr << "agent_dialog_probe: '.runlist.0.status' 缺失/为空\n";
            passed = false;
        }
        for (const char* sid : {"agent.dialog.detail.summary.last.value",
                                "agent.dialog.detail.summary.next.value",
                                "agent.dialog.detail.summary.stats.value"}) {
            const auto* node = findElement(sid);
            if (node == nullptr || node->text.empty()) {
                std::cerr << "agent_dialog_probe: '" << sid
                          << "' 缺失/为空（摘要三卡真数据未接线？）\n";
                passed = false;
            }
        }
        const auto* detailStatus =
            findElement("agent.dialog.detail.detail.status.text");
        if (detailStatus == nullptr || detailStatus->text != "失败") {
            std::cerr << "agent_dialog_probe: .detail.status.text 应为「失败」"
                         "（种子 run 状态一致），实测 '"
                      << (detailStatus != nullptr ? detailStatus->text : "<缺失>")
                      << "'\n";
            passed = false;
        }
        const auto* errFull = findElement("agent.dialog.detail.detail.error.text");
        if (errFull == nullptr) {
            std::cerr << "agent_dialog_probe: '.detail.error.text' 缺失\n";
            passed = false;
        } else if (errFull->text.find("LLM 配置不可用") == std::string::npos) {
            std::cerr << "agent_dialog_probe: .detail.error.text 未含种子错误原文\n";
            passed = false;
        }
        if (findElement("agent.dialog.detail.detail.error.scroll") == nullptr) {
            std::cerr << "agent_dialog_probe: '.detail.error.scroll' 缺失\n";
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
