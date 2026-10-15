// tests/test_modules/test_agent_ui.cpp
// 智能体页 UI 无头用例：页面骨架 / 任务弹窗（新建·编辑·校验·关闭四路径）/ 详情弹窗
// （配置摘要、run 列表、消息块级展开）/ 侧边栏接线 / 常驻入队 / 弹窗几何预算。
//
// 跨文件前提（节点 id 契约，agent_module.cpp 侧为准）：
//   页面   agent.page / agent.page.header / agent.title / agent.page.newTask /
//          agent.tools.*（可用工具区块）/ agent.tasklist（scrollView）+.content /
//          agent.task.<id>（.title / .status / .detail / .edit / .toggle /
//          [.enqueue 仅常驻] / .run）/ run 结束通知（Task 1；fix round 起经 takeRunEndNotice 走 app 级通道）
//   任务弹窗 agent.dialog.task.*（新建 / 编辑共用）：
//          .goal（多行）/ .trigger / .cron（仅定时）/ .slot / .iter /
//          .destructive / .cancel / .save / .close / .error（有错才渲染）
//   详情弹窗 agent.dialog.detail.*：.info / .runlist.<i>[.err] / .msgscroll /
//          .msg.<i>[.badge / .toggle / .preview / .blk.<j>[.out]] / .close
//   旧页内表单（agent.form.* / agent.edit.*）与页内详情展开区已删除——本文件用
//   「节点不存在」把它们钉死，防回潮。
//
// 无头口径：
//   - 裸 eui::Ui + ui.begin/mod.compose/ui.end + ui.layout（frame 靠 layout 填充）；
//   - 按钮/行/弹窗关闭面点击 = 直调元素上真实挂的回调：button 的 onClick 挂在
//     <id>.bg，弹窗遮罩挂 <dialog>.backdrop；
//   - 文本输入 = 对 <id>.hit 直调 onTextInput（真实 input 事件路径）；
//   - 下拉切换 = 直调 <id>.item.<i> 的 onClick；开关切换 = 直调 <id>.hit 的 onClick。
//   真窗口几何 + 真命中分发的探针侧在 tests/probes/agent_probes.cpp。
#include "doctest.h"
#include "test_support.h"

#include "app_sidebar.h"
#include "core/claude_proxy_service.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "platform/http_client.h"

#include <components/input.h>   // InputState：读回弹窗内输入框真实文本（预填判据）
#include <eui/dsl.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

// 测试用 fake 代理服务
class FakeProxy : public core::IClaudeProxyService {
public:
    bool running = true;
    int port = 8787;

    bool isProxyRunning() const override { return running; }
    int proxyPort() const override { return port; }
    void setConfiguredProxyPort(int) override {}
    void stopProxy() override {}
    core::Result<int> startProxy(const std::string&, int) override {
        return core::Result<int>::fail(core::ErrorCode::Internal, "unused");
    }
    bool isClaudeCodeManaged() const override { return false; }
    std::vector<std::string> lastSyncWarnings() const override { return {}; }
    std::string settingsFilePath() const override { return {}; }
    std::optional<std::string> resolveModelSlot(const std::string&) override {
        return std::string("claude-sonnet-5-5");
    }
};

// RAII：用例退出（含 fail-fast 提前 return 路径）时把 app.stats 从进程级
// 注册表摘除（ToolRegistry 不在 resetCaseGlobals 清单里）
struct ToolGuard {
    std::string name;
    ~ToolGuard() { core::ToolRegistry::instance().unregisterTool(name); }
};

using InputState = components::input_detail::InputModel::InputState;

const eui::Screen kScreen{1600.0f, 1000.0f};

// 裸 Ui compose 一帧 + 显式 layout（元素 frame 需显式布局）
void composeTo(eui::Ui& ui, module::agent::AgentModule& mod,
               const eui::Screen& screen, const char* pageId) {
    ui.begin(pageId);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

// 直调元素自身 onClick（契约：可点元素 id 元素本身带 onClick；
// components::button 的 onClick 落在 <id>.bg，调用方传 <id>.bg）
bool clickId(eui::Ui& ui, const std::string& id) {
    const auto* e = ui.find(id);
    if (e == nullptr || e->onClick == nullptr) return false;
    e->onClick();
    return true;
}

// 按 UTF-8 字符边界逐字符送进 input 的 onTextInput（真实 input 事件路径）
void typeInto(core::dsl::Element* hit, const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        core::TextInputEvent ev;
        ev.text = text.substr(i, len);
        hit->onTextInput(ev);
        i += len;
    }
}

// 触发类型 / 槽位下拉切换：直调 item 上真实挂的 onChange 回调
bool callDropdownChange(eui::Ui& ui, const std::string& ddId, int idx) {
    const auto* item = ui.find(ddId + ".item." + std::to_string(idx));
    if (item == nullptr || item->onClick == nullptr) return false;
    item->onClick();
    return true;
}

std::string inputText(eui::Ui& ui, const std::string& id) {
    return ui.state<InputState>(id).text;
}

bool nearPx(float a, float b) { return std::fabs(a - b) <= 0.5f; }

// inner 完全落在 outer 内（0.5 容差，与 nearPx 同口径）
bool frameContained(const core::dsl::Element& outer,
                    const core::dsl::Element& inner) {
    return inner.frame.x >= outer.frame.x - 0.5f &&
           inner.frame.y >= outer.frame.y - 0.5f &&
           inner.frame.x + inner.frame.width <=
               outer.frame.x + outer.frame.width + 0.5f &&
           inner.frame.y + inner.frame.height <=
               outer.frame.y + outer.frame.height + 0.5f;
}

// 打开新建态任务弹窗（点页顶「+ 新建任务」）；成功即已 recompose 一帧
bool openNewTaskDialog(eui::Ui& ui, module::agent::AgentModule& mod,
                       const char* pageId) {
    composeTo(ui, mod, kScreen, pageId);
    return clickId(ui, "agent.page.newTask.bg");
}

// 打开某任务的详情弹窗（点行内「详情」chip）
bool openDetailDialog(eui::Ui& ui, module::agent::AgentModule& mod,
                      const std::string& tid) {
    return clickId(ui, "agent.task." + tid + ".detail");
}

} // namespace

REGISTER_TEST(agent_ui_compose_skeleton,
              "agent_ui: 空库页面骨架（列表是 scrollView / 旧页内表单节点已删 / 弹窗未开不渲染）", {
    test_support::TestDb tdb("agentui", "skeleton");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_skeleton");

    const auto* page = ui.find("agent.page");
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(page->frame.width, 1600.0f),
                          "agent.page.frame.width 实测 " +
                              std::to_string(page->frame.width));
    const auto* title = ui.find("agent.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("智能体"));

    // 「+ 新建任务」是唯一的新建通道
    DOCTEST_CHECK(ui.find("agent.page.newTask") != nullptr);
    DOCTEST_CHECK(ui.find("agent.page.newTask.bg") != nullptr);

    // 旧页内新建表单 / 编辑表单节点全部不存在（骨架契约）
    DOCTEST_CHECK(ui.find("agent.form") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.goal.label") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.create") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.error") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.form") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.save") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.error") == nullptr);

    // 任务列表容器 = scrollView（.content 是 scrollView 独有的内容列，
    // 普通 ui.column 不会产生该节点）
    DOCTEST_REQUIRE(ui.find("agent.tasklist") != nullptr);
    DOCTEST_CHECK_MESSAGE(ui.find("agent.tasklist.content") != nullptr,
                          "agent.tasklist 不是 scrollView（缺 .content 内容列）");

    DOCTEST_CHECK(ui.find("agent.tools.header") != nullptr);

    // 弹窗未打开：弹窗内容不渲染（打开/关闭状态由节点存在性直接可判）
    DOCTEST_CHECK(ui.find("agent.dialog.task.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.dialog.detail.msgscroll") == nullptr);

    // 空库无任务行（fresh 库首个任务 id = 1）
    DOCTEST_CHECK(ui.find("agent.task.1") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_new_task_dialog_fields,
              "agent_ui: 点「+ 新建任务」→ 弹窗全字段存在；cron 仅触发方式=定时 时渲染", {
    test_support::TestDb tdb("agentui", "dialogfields");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_dlg1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dlg2");

    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.task") != nullptr,
                            "弹窗外框 agent.dialog.task 缺失");
    DOCTEST_CHECK(ui2.find("agent.dialog.task.goal.caption") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.trigger") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.slot") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.iter") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.destructive") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.cancel") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.save") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.close") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.error") == nullptr);

    // 任务目标 = 多行输入（≥90 高；单行时代 40）
    const auto* goal = ui2.find("agent.dialog.task.goal");
    DOCTEST_REQUIRE(goal != nullptr);
    DOCTEST_CHECK_MESSAGE(goal->frame.height >= 90.0f,
                          "弹窗 goal 输入应 ≥90px，实测 " +
                              std::to_string(goal->frame.height));

    DOCTEST_CHECK(ui2.find("agent.dialog.task.cron") == nullptr);
    DOCTEST_REQUIRE_MESSAGE(callDropdownChange(ui2, "agent.dialog.task.trigger", 1),
                            "agent.dialog.task.trigger.item.1 不存在（下拉未渲染？）");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dlg3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.task.cron") != nullptr,
                          "触发方式=定时 时未渲染 agent.dialog.task.cron");
    DOCTEST_CHECK(ui3.find("agent.dialog.task.cron.caption") != nullptr);

    DOCTEST_REQUIRE(callDropdownChange(ui3, "agent.dialog.task.trigger", 0));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_dlg4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.cron") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_create_task_persists_fields,
              "agent_ui: 弹窗新建保存 → 落库行 trigger/slot/iter/destructive 与表单取值一致 + 弹窗关闭", {
    test_support::TestDb tdb("agentui", "dlgsave");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_save1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_save2");

    // goal 走真实 input 事件路径
    auto* goalHit = ui2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    DOCTEST_REQUIRE(goalHit->onTextInput != nullptr);
    typeInto(goalHit, "汇总今日待办");

    DOCTEST_REQUIRE(callDropdownChange(ui2, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_save3");
    DOCTEST_REQUIRE_MESSAGE(ui3.find("agent.dialog.task.cron") != nullptr,
                            "定时态缺 cron 输入");
    mod.setEditCronDraft("0 9 * * 1-5");
    // 槽位 = opus（X 宏表第 3 项，index 2）
    DOCTEST_REQUIRE(callDropdownChange(ui3, "agent.dialog.task.slot", 2));
    // 迭代上限 = 7（非默认 10）
    mod.setDialogIterDraft(7);
    // 破坏性 = true（switch 默认 false，点一下取反）
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3, "agent.dialog.task.destructive.hit"),
                            "破坏性开关命中面缺失");

    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_save4");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui4, "agent.dialog.task.save.bg"),
                            "保存按钮 agent.dialog.task.save.bg 缺失");

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    const auto& row = tasks.value()[0];
    DOCTEST_CHECK_EQ(row.goal, std::string("汇总今日待办"));
    DOCTEST_CHECK_EQ(row.triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(row.triggerConfig, std::string(R"({"cron":"0 9 * * 1-5"})"));
    DOCTEST_CHECK_EQ(row.modelSlot, std::string("opus"));
    DOCTEST_CHECK_EQ(row.maxIterations, 7);
    DOCTEST_CHECK(row.allowDestructive);

    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_save5");
    DOCTEST_CHECK(ui5.find("agent.dialog.task.goal") == nullptr);
    DOCTEST_CHECK(ui5.find("agent.task." + std::to_string(row.id) + ".title") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_save_rejects_bad_cron,
              "agent_ui: 非法 cron 保存被拒（弹窗内报错 / 不落库 / 弹窗不关）", {
    test_support::TestDb tdb("agentui", "badcron");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_bad1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_bad2");
    auto* goalHit = ui2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    typeInto(goalHit, "非法 cron 任务");
    DOCTEST_REQUIRE(callDropdownChange(ui2, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_bad3");
    mod.setEditCronDraft("0 9 * * 1-5x");
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.save.bg"));

    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_bad4");
    const auto* err = ui4.find("agent.dialog.task.error");
    DOCTEST_REQUIRE_MESSAGE(err != nullptr,
                            "非法 cron 未在弹窗内报错（agent.dialog.task.error 缺失）");
    DOCTEST_CHECK(err->text.find("cron") != std::string::npos);
    DOCTEST_CHECK(ui4.find("agent.dialog.task.goal") != nullptr);
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_cancel_close_backdrop_esc,
              "agent_ui: 取消 / ✕ / 点遮罩 / Esc 四条关闭路径都不落库", {
    test_support::TestDb tdb("agentui", "dlgclose");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    // 打开新建弹窗并敲入 goal；返回 false = 夹具没建成，调用方 REQUIRE 转红
    auto typedGoalAndCompose = [&](const char* pageId, eui::Ui& ui) -> bool {
        eui::Ui page;
        if (!openNewTaskDialog(page, mod, pageId)) return false;
        composeTo(ui, mod, kScreen, pageId);
        auto* hit = ui.find("agent.dialog.task.goal.hit");
        if (hit == nullptr) return false;
        typeInto(hit, "不该落库的目标");
        return true;
    };

    // ① 取消
    eui::Ui ui1;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close1", ui1));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui1, "agent.dialog.task.cancel.bg"),
                            "取消按钮 agent.dialog.task.cancel.bg 缺失");
    eui::Ui c1;
    composeTo(c1, mod, kScreen, "agent_ui_close1b");
    DOCTEST_CHECK(c1.find("agent.dialog.task.goal") == nullptr);

    // ② ✕
    eui::Ui ui2;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close2", ui2));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.dialog.task.close"),
                            "✕ 关闭面 agent.dialog.task.close 缺失");
    eui::Ui c2;
    composeTo(c2, mod, kScreen, "agent_ui_close2b");
    DOCTEST_CHECK(c2.find("agent.dialog.task.goal") == nullptr);

    // ③ 点遮罩（dialog 组件 backdrop onClick → onOpenChange(false)）
    eui::Ui ui3;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close3", ui3));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3, "agent.dialog.task.backdrop"),
                            "遮罩 agent.dialog.task.backdrop 缺失");
    eui::Ui c3;
    composeTo(c3, mod, kScreen, "agent_ui_close3b");
    DOCTEST_CHECK(c3.find("agent.dialog.task.goal") == nullptr);

    // ④ Esc（焦点不在输入框时经模块级 onKeyEvent）
    eui::Ui ui4;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close4", ui4));
    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK(mod.onKeyEvent(esc));
    eui::Ui c4;
    composeTo(c4, mod, kScreen, "agent_ui_close4b");
    DOCTEST_CHECK(c4.find("agent.dialog.task.goal") == nullptr);

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_edit_dialog_prefills_and_updates,
              "agent_ui: 行内「编辑」→ 弹窗预填（goal/cron/slot/iter 与库一致）→ 保存走 updateTask", {
    test_support::TestDb tdb("agentui", "dlgedit");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("旧任务", "旧目标", "schedule", "sonnet", 20, true,
                                    R"({"cron":"0 9 * * 1-5"})");
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_edit1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".edit"),
                            "任务行缺 agent.task.<id>.edit");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_edit2");
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.task.goal") != nullptr,
                            "编辑态未打开任务弹窗");
    // 预填：与库同一份 AgentTaskRow
    DOCTEST_CHECK_EQ(inputText(ui2, "agent.dialog.task.goal"), std::string("旧目标"));
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.task.cron") != nullptr,
                            "schedule 任务未渲染 cron 输入");
    DOCTEST_CHECK_EQ(inputText(ui2, "agent.dialog.task.cron"),
                     std::string("0 9 * * 1-5"));
    DOCTEST_CHECK_EQ(inputText(ui2, "agent.dialog.task.iter"), std::string("20"));

    DOCTEST_REQUIRE(callDropdownChange(ui2, "agent.dialog.task.slot", 3));
    mod.setDialogIterDraft(3);
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_edit3");
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.save.bg"));

    auto updated = store.getTask(created.value());
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().goal, std::string("旧目标"));
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig,
                     std::string(R"({"cron":"0 9 * * 1-5"})"));            // 预填后原样回写
    DOCTEST_CHECK_EQ(updated.value().modelSlot, std::string("haiku"));
    DOCTEST_CHECK_EQ(updated.value().maxIterations, 3);
    DOCTEST_CHECK(updated.value().allowDestructive);                       // 预填 true 未翻
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_edit4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.goal") == nullptr);

    // 启停是行内独立动作：不进编辑弹窗，文案随 enabled 翻转且落库取反
    const auto* toggle = ui4.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_CHECK_EQ(toggle->text, std::string("禁用"));
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    auto disabled = store.getTask(created.value());
    DOCTEST_REQUIRE(disabled.isOk());
    DOCTEST_CHECK_FALSE(disabled.value().enabled);
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_edit5");
    const auto* toggle2 = ui5.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle2 != nullptr);
    DOCTEST_CHECK_EQ(toggle2->text, std::string("启用"));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_run_button_opens_detail_dialog,
              "agent_ui: 点「启动」→ 详情弹窗立即打开；跑完最终 assistant 回复默认展开可见", {
    test_support::TestDb tdb("agentui", "runopens");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("端到端任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"最终答复"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_run1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 启动后第一帧详情弹窗就该开，不等 run 跑完。此刻 run 行可能还没落库、消息区显示
    // 「还没有消息」，所以这里只能钉弹窗本体 + 配置摘要——钉消息会偶发红
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_run2");
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.detail") != nullptr,
                            "点启动后未立即打开详情弹窗");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.info") != nullptr);

    // 有界轮询（10ms 步进 ≤5s）+ 每轮 recompose 跟进刷新（弹窗随刷新周期更新）
    bool completed = false;
    eui::Ui ui3;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui3, mod, kScreen, "agent_ui_run_poll");
        auto runs = store.listRuns(created.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "run 线程 5s 内未落库 completed");

    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_run4");
    const auto* status = ui4.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("成功") != std::string::npos);
    const auto* run0 = ui4.find("agent.dialog.detail.runlist.0");
    DOCTEST_REQUIRE_MESSAGE(run0 != nullptr, "详情弹窗缺 run 列表行");
    DOCTEST_CHECK(run0->text.find("成功") != std::string::npos);
    // 最终 assistant 文本块默认展开 = 回复无需任何点击即可见
    const auto* finalBlk = ui4.find("agent.dialog.detail.msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "最终 assistant 文本块未默认展开（msg.1.blk.0 缺失）");
    DOCTEST_CHECK_MESSAGE(finalBlk->text.find("最终答复") != std::string::npos,
                          "最终回复内容未渲染，实测: " + finalBlk->text);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_dialog_shows_config_runs_messages,
              "agent_ui: 详情弹窗配置摘要 + run 列表倒序/最近高亮 + 消息默认展开态（最终回复展开、其余折叠）", {
    test_support::TestDb tdb("agentui", "detaildlg");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("详情任务", "把逾期的待办全部完成", "schedule",
                                    "sonnet", 20, true,
                                    R"({"cron":"0 9 * * 1-5"})");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    // 两次 run：先 limited（带 error 与 token），后 completed（最近一次）——run 列表的
    // 倒序与「最近一次高亮」判据依赖这个顺序
    auto r1 = store.startRun(taskId);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r1.value(), 0, "user",
                                        R"([{"type":"text","text":"第一个 run"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "limited", 7, int64_t(190000),
                                    int64_t(15000),
                                    std::string("token 累计超上限 200K")).isOk());
    const std::string longOutput(600, 'z');
    auto r2 = store.startRun(taskId);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r2.value(), 0, "user",
                                        R"([{"type":"text","text":"查一下"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r2.value(), 1, "assistant",
                        R"([{"type":"text","text":"最终答复内容"},)"
                        R"({"type":"tool_use","id":"tu1","name":"todo.list",)"
                        R"("input":{"limit":5}}])",
                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r2.value(), 2, "user",
                        R"([{"type":"tool_result","tool_use_id":"tu1",)"
                        R"("content":[{"type":"text","text":")" + longOutput +
                        R"("}],"is_error":false}])",
                        std::string("tu1")).isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_dd1");
    DOCTEST_REQUIRE_MESSAGE(openDetailDialog(ui, mod, tid),
                            "任务行缺 agent.task.<id>.detail");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dd2");

    // 配置摘要与编辑弹窗同源，都走 getTask
    const auto* info = ui2.find("agent.dialog.detail.info");
    DOCTEST_REQUIRE_MESSAGE(info != nullptr, "详情弹窗缺配置摘要 agent.dialog.detail.info");
    DOCTEST_CHECK(info->text.find("把逾期的待办全部完成") != std::string::npos);
    DOCTEST_CHECK(info->text.find("schedule") != std::string::npos);
    DOCTEST_CHECK(info->text.find("0 9 * * 1-5") != std::string::npos);
    DOCTEST_CHECK(info->text.find("sonnet") != std::string::npos);
    DOCTEST_CHECK(info->text.find("迭代上限：20") != std::string::npos);
    DOCTEST_CHECK(info->text.find("破坏性：允许") != std::string::npos);

    // run 列表：倒序（最近一次在前）+ 最近一次高亮（颜色须与次新不同）
    const auto* run0 = ui2.find("agent.dialog.detail.runlist.0");
    const auto* run1 = ui2.find("agent.dialog.detail.runlist.1");
    DOCTEST_REQUIRE_MESSAGE(run0 != nullptr && run1 != nullptr,
                            "详情弹窗缺 run 列表行");
    DOCTEST_CHECK_MESSAGE(run0->text.find("成功") != std::string::npos,
                          "runlist.0 应为最近一次 run（completed→成功），实测：" +
                              run0->text);
    DOCTEST_CHECK(run0->text.find("token —") != std::string::npos);
    DOCTEST_CHECK(run1->text.find("受限") != std::string::npos);
    DOCTEST_CHECK(run1->text.find("205000") != std::string::npos);
    const auto* errRow = ui2.find("agent.dialog.detail.runlist.1.err");
    DOCTEST_REQUIRE(errRow != nullptr);
    DOCTEST_CHECK(errRow->text.find("200K") != std::string::npos);
    DOCTEST_CHECK_MESSAGE(run0->textColor.r != run1->textColor.r ||
                              run0->textColor.g != run1->textColor.g ||
                              run0->textColor.b != run1->textColor.b,
                          "最近一次 run 未高亮（与次新行同色）");

    // 消息流：最终 assistant 文本块默认展开；其余（用户 / 工具消息）默认折叠
    DOCTEST_REQUIRE(ui2.find("agent.dialog.detail.msgscroll") != nullptr);
    const auto* finalBlk = ui2.find("agent.dialog.detail.msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "最终 assistant 文本块未默认展开");
    DOCTEST_CHECK(finalBlk->text.find("最终答复内容") != std::string::npos);
    // 用户消息：只有 .preview，无块
    const auto* userPreview = ui2.find("agent.dialog.detail.msg.0.preview");
    DOCTEST_REQUIRE_MESSAGE(userPreview != nullptr, "用户消息未走折叠态");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.msg.0.blk.0") == nullptr);
    // tool_result 消息默认折叠
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.msg.2.blk.0.out") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_message_toggle_and_copy,
              "agent_ui: 详情弹窗内消息展开→tool_use chip + tool_result 全文 + 高度增大 + 复制 chip", {
    test_support::TestDb tdb("agentui", "msgtoggle");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("日志任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    auto r = store.startRun(taskId);
    DOCTEST_REQUIRE(r.isOk());
    const std::string longOutput(600, 'z');
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                        R"([{"type":"text","text":"查一下"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r.value(), 1, "assistant",
                        R"([{"type":"text","text":"我来查"},)"
                        R"({"type":"tool_use","id":"tu1","name":"todo.list",)"
                        R"("input":{"limit":5}}])",
                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r.value(), 2, "user",
                        R"([{"type":"tool_result","tool_use_id":"tu1",)"
                        R"("content":[{"type":"text","text":")" + longOutput +
                        R"("}],"is_error":false}])",
                        std::string("tu1")).isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const std::string base = "agent.dialog.detail";

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_mt1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_mt2");
    const auto* m2c = ui2.find(base + ".msg.2");
    DOCTEST_REQUIRE(m2c != nullptr);
    const float collapsedH = m2c->frame.height;
    DOCTEST_CHECK(collapsedH > 0.0f);
    // assistant 混合块（seq1）默认展开：tool_use chip 可见
    const auto* chip = ui2.find(base + ".msg.1.blk.1");
    DOCTEST_REQUIRE_MESSAGE(chip != nullptr, "tool_use 块缺失 msg.1.blk.1");
    DOCTEST_CHECK_MESSAGE(chip->text.find("todo.list") != std::string::npos &&
                              chip->text.find("🔧") != std::string::npos,
                          "tool chip 缺工具名/emoji，实测: " + chip->text);

    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, base + ".msg.2.toggle"),
                            "msg.2 展开 chip 缺失");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_mt3");
    const auto* m2e = ui3.find(base + ".msg.2");
    DOCTEST_REQUIRE(m2e != nullptr);
    DOCTEST_CHECK_MESSAGE(m2e->frame.height > collapsedH,
                          "展开高度应大于收起高 " + std::to_string(collapsedH) +
                              "，实测 " + std::to_string(m2e->frame.height));
    const auto* out = ui3.find(base + ".msg.2.blk.0.out");
    DOCTEST_REQUIRE(out != nullptr);
    DOCTEST_CHECK_MESSAGE(out->text.find("zzz") != std::string::npos &&
                              out->text.size() >= 600,
                          "tool_result 输出全文未渲染，实测 " +
                              std::to_string(out->text.size()) + " 字节");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3, base + ".msg.2.copy"),
                            "复制 chip 缺失");
    const auto* copyBtn = ui3.find(base + ".msg.2.copy");
    DOCTEST_CHECK_EQ(copyBtn->text, std::string("复制"));

    // 再点收起 → 回到折叠态
    DOCTEST_REQUIRE(clickId(ui3, base + ".msg.2.toggle"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_mt4");
    DOCTEST_CHECK(ui4.find(base + ".msg.2.blk.0.out") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_copy_final_reply_chip,
               "agent_ui: 详情弹窗「消息（N）」标题行复制回复 chip（tool 收尾取最后 assistant / 无回复不渲染）", {
    test_support::TestDb tdb("agentui", "copyfinal");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    const std::string base = "agent.dialog.detail";

    // 任务 A：run 以 tool_result 收尾——最终回复在中间的 assistant（钉「最后一条
    // assistant ≠ 最后一条消息」风险）
    auto createdA = store.createTask("任务A", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(createdA.isOk());
    const std::string tidA = std::to_string(createdA.value());
    auto rA = store.startRun(createdA.value());
    DOCTEST_REQUIRE(rA.isOk());
    DOCTEST_REQUIRE(store.appendMessage(rA.value(), 0, "user",
                                        R"([{"type":"text","text":"查一下"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        rA.value(), 1, "assistant",
                        R"([{"type":"text","text":"我先查。"},)"
                        R"({"type":"tool_use","id":"tu1","name":"todo.list","input":{"limit":5}},)"
                        R"({"type":"text","text":"查完了，共 2 条。"}])",
                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        rA.value(), 2, "user",
                        R"([{"type":"tool_result","tool_use_id":"tu1",)"
                        R"("content":[{"type":"text","text":"ok"}],"is_error":false}])",
                        std::string("tu1")).isOk());
    DOCTEST_REQUIRE(store.finishRun(rA.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    // 任务 B：run 只有 user 消息（刚起 / 全 tool_use）→ 无最终回复
    auto createdB = store.createTask("任务B", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(createdB.isOk());
    const std::string tidB = std::to_string(createdB.value());
    auto rB = store.startRun(createdB.value());
    DOCTEST_REQUIRE(rB.isOk());
    DOCTEST_REQUIRE(store.appendMessage(rB.value(), 0, "user",
                                        R"([{"type":"text","text":"刚起跑"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(rB.value(), "limited", 0, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 任务 A：chip 存在、可点、几何落在弹窗内且在标题行右侧
    eui::Ui uiA1;
    composeTo(uiA1, mod, kScreen, "agent_ui_cf1");
    DOCTEST_REQUIRE(openDetailDialog(uiA1, mod, tidA));
    eui::Ui uiA2;
    composeTo(uiA2, mod, kScreen, "agent_ui_cf2");
    const std::string chipId = base + ".msgs.copyfinal";
    const auto* chip = uiA2.find(chipId);
    DOCTEST_REQUIRE_MESSAGE(chip != nullptr, "有最终回复时缺「复制回复」chip");
    DOCTEST_CHECK_EQ(chip->text, std::string("复制回复"));
    const auto* title = uiA2.find(base + ".msgs.title");
    const auto* panel = uiA2.find(base);
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_REQUIRE(panel != nullptr);
    // 无头几何断言：chip 与标题带纵向重叠（chip 高 32 比标题行 18 高，居中对齐上移 7）、
    // chip 在标题文本右侧、整体在弹窗内
    DOCTEST_CHECK_MESSAGE(chip->frame.y < title->frame.y + 18.0f &&
                              chip->frame.y + chip->frame.height >
                                  title->frame.y,
                          "chip 未与「消息（N）」标题同带: chip.y=" +
                              std::to_string(chip->frame.y) + " title.y=" +
                              std::to_string(title->frame.y));
    DOCTEST_CHECK_MESSAGE(chip->frame.x > title->frame.x,
                          "chip 应在标题右侧");
    DOCTEST_CHECK(frameContained(*panel, *chip));
    DOCTEST_REQUIRE_MESSAGE(clickId(uiA2, chipId + ".bg"),
                            "复制回复 chip 无命中面 <id>.bg");
    // 既有逐条复制 chip 零回归：msg.2 展开后仍有 .copy
    DOCTEST_REQUIRE(clickId(uiA2, base + ".msg.2.toggle"));
    eui::Ui uiA3;
    composeTo(uiA3, mod, kScreen, "agent_ui_cf3");
    DOCTEST_CHECK(uiA3.find(base + ".msg.2.copy") != nullptr);
    DOCTEST_CHECK(uiA3.find(chipId) != nullptr);

    // 任务 B：无 assistant 回复 → chip 不渲染
    eui::Ui uiB1;
    composeTo(uiB1, mod, kScreen, "agent_ui_cf4");
    DOCTEST_REQUIRE(openDetailDialog(uiB1, mod, tidB));
    eui::Ui uiB2;
    composeTo(uiB2, mod, kScreen, "agent_ui_cf5");
    DOCTEST_CHECK_MESSAGE(uiB2.find(chipId) == nullptr,
                          "无最终回复时「复制回复」chip 应不渲染");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_dialog_close_and_reopen,
              "agent_ui: 关详情弹窗不改任务列表；重开回到最近一次 run", {
    test_support::TestDb tdb("agentui", "detailreopen");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("重开任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    auto r1 = store.startRun(taskId);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r1.value(), 0, "user",
                                        R"([{"type":"text","text":"旧 run 消息"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "limited", 3, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    auto r2 = store.startRun(taskId);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r2.value(), 0, "user",
                                        R"([{"type":"text","text":"新 run 消息"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(r2.value(), 1, "assistant",
                                        R"([{"type":"text","text":"新 run 回复"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const std::string base = "agent.dialog.detail";

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_ro1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_ro2");
    DOCTEST_REQUIRE(ui2.find(base + ".msg.1.blk.0") != nullptr);
    // 显式点选旧 run（runlist.1 = limited）
    DOCTEST_REQUIRE(clickId(ui2, base + ".runlist.1"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_ro3");
    const auto* oldPreview = ui3.find(base + ".msg.0.preview");
    DOCTEST_REQUIRE_MESSAGE(oldPreview != nullptr, "选中旧 run 后消息区未切换");
    DOCTEST_CHECK(oldPreview->text.find("旧 run 消息") != std::string::npos);

    DOCTEST_REQUIRE(clickId(ui3, base + ".close"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_ro4");
    DOCTEST_CHECK(ui4.find(base + ".msgscroll") == nullptr);
    const auto* rowTitle = ui4.find("agent.task." + tid + ".title");
    DOCTEST_REQUIRE_MESSAGE(rowTitle != nullptr, "关弹窗后任务行消失（回写了列表？）");
    DOCTEST_CHECK_EQ(rowTitle->text, std::string("重开任务"));

    DOCTEST_REQUIRE(openDetailDialog(ui4, mod, tid));
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_ro5");
    const auto* reopened = ui5.find(base + ".msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(reopened != nullptr,
                            "重开后未回到最近一次 run（新 run 回复未默认展开）");
    DOCTEST_CHECK(reopened->text.find("新 run 回复") != std::string::npos);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_runlist_error_rows_no_overlap,
              "agent_ui: 详情弹窗 3 条 error 的 run 列表不压消息标题（runsH 必须计入 error 行的 gap；回退即红）", {
    test_support::TestDb tdb("agentui", "errrows");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("错误行任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    // 3 次 run 全部带 error 文案（每次 = 状态行 18 + 错误行 16，列内 gap 4）
    for (int i = 0; i < 3; ++i) {
        auto r = store.startRun(created.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), "limited", i + 1,
                                        int64_t(100), int64_t(10),
                                        std::string("错误文案 ") +
                                            std::to_string(i)).isOk());
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_err1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_err2");

    const auto* panel = ui2.find("agent.dialog.detail.panel");
    const auto* msgsTitle = ui2.find("agent.dialog.detail.msgs.title");
    const auto* lastErr = ui2.find("agent.dialog.detail.runlist.2.err");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr && msgsTitle != nullptr,
                            "面板/消息标题缺失");
    DOCTEST_REQUIRE_MESSAGE(lastErr != nullptr, "第 3 条 error 行缺失");
    // runsH 必须计入 error 行与其上方状态行之间的 gap，否则最后一个 error 行会压到消息标题上
    DOCTEST_CHECK_MESSAGE(
        lastErr->frame.y + lastErr->frame.height <= msgsTitle->frame.y + 0.5f,
        "error 行压到消息标题：error 底 " +
            std::to_string(lastErr->frame.y + lastErr->frame.height) +
            " > 标题顶 " + std::to_string(msgsTitle->frame.y));
    // 整段也不得越出面板底
    DOCTEST_CHECK_MESSAGE(
        lastErr->frame.y + lastErr->frame.height <=
            panel->frame.y + panel->frame.height,
        "error 行溢出弹窗面板底");
    const auto* err0 = ui2.find("agent.dialog.detail.runlist.0.err");
    const auto* err1 = ui2.find("agent.dialog.detail.runlist.1.err");
    DOCTEST_REQUIRE(err0 != nullptr && err1 != nullptr);
    DOCTEST_CHECK(err0->frame.y + err0->frame.height <= err1->frame.y + 0.5f);
    DOCTEST_CHECK(err1->frame.y + err1->frame.height <= lastErr->frame.y + 0.5f);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_edit_read_failure_shows_page_error,
              "agent_ui: 点「编辑」读库失败（陈旧缓存）→ 页内错误行可见、弹窗不开、下次成功动作清除", {
    test_support::TestDb tdb("agentui", "editerr");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("将消失的任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_eerr1");
    DOCTEST_REQUIRE(ui.find("agent.task." + tid + ".edit") != nullptr);
    // 制造陈旧缓存：UI 帧里仍有该行，库里已被删（外部删除 / 另一实例）
    DOCTEST_REQUIRE(tdb.handle().exec("DELETE FROM agent_tasks WHERE id=" + tid).isOk());

    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".edit"),
                            "编辑 chip 缺失");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_eerr2");
    const auto* pageErr = ui2.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(pageErr != nullptr,
                            "编辑读库失败没有任何可见反馈（静默路径）");
    DOCTEST_CHECK(pageErr->text.find("读取任务失败") != std::string::npos);
    // 弹窗不进编辑态（没数据可编）
    DOCTEST_CHECK(ui2.find("agent.dialog.task.goal") == nullptr);
    // 同类静默路径：启停切换失败也走同一条页内错误行
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.task." + tid + ".toggle"),
                            "启停 chip 缺失");
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_eerr2b");
    const auto* toggleErr = ui2b.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(toggleErr != nullptr, "启停失败没有任何可见反馈");
    DOCTEST_CHECK(toggleErr->text.find("启停切换失败") != std::string::npos);
    DOCTEST_REQUIRE(clickId(ui2b, "agent.page.newTask.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_eerr3");
    DOCTEST_CHECK(ui3.find("agent.page.error") == nullptr);
    DOCTEST_CHECK(ui3.find("agent.dialog.task.goal") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_tools_section_toggle,
              "agent_ui: 可用工具区块收起默认→展开列出注册表全部工具（名称+描述+参数）", {
    test_support::TestDb tdb("agentui", "tools");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();   // 注册 app.stats → 注册表恰 1 个（测试环境干净起点）

    const size_t registered =
        core::ToolRegistry::instance().listTools().size();
    DOCTEST_REQUIRE(registered >= 1);

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_tools1");
    const auto* header = ui.find("agent.tools.header");
    DOCTEST_REQUIRE(header != nullptr);
    DOCTEST_REQUIRE(header->onClick != nullptr);
    const auto* label = ui.find("agent.tools.header.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_MESSAGE(
        label->text.find("可用工具") != std::string::npos &&
            label->text.find(std::to_string(registered)) != std::string::npos,
        "头部缺「可用工具（N）」计数，实测: " + label->text);


    DOCTEST_CHECK(ui.find("agent.tools.card.0") == nullptr);
    header->onClick();
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_tools2");
    size_t cards = 0;
    for (size_t k = 0; k < registered + 4; ++k) {
        if (ui2.find("agent.tools.card." + std::to_string(k)) != nullptr) {
            ++cards;
        }
    }
    DOCTEST_CHECK_MESSAGE(cards == registered,
                          "工具卡数 " + std::to_string(cards) +
                              " != 注册表数 " + std::to_string(registered));
    const auto* name0 = ui2.find("agent.tools.card.0.name");
    DOCTEST_REQUIRE(name0 != nullptr);
    DOCTEST_CHECK_EQ(name0->text, std::string("app.stats"));
    const auto* params0 = ui2.find("agent.tools.card.0.params");
    DOCTEST_REQUIRE(params0 != nullptr);
    DOCTEST_CHECK(params0->text.find("参数：") != std::string::npos);

    const auto* header2 = ui2.find("agent.tools.header");
    DOCTEST_REQUIRE(header2 != nullptr && header2->onClick != nullptr);
    header2->onClick();
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_tools3");
    DOCTEST_CHECK(ui3.find("agent.tools.card.0") == nullptr);

    mod.onStop();
    return true;
})

// 判据只能钉我们自己算的布局预算（面板设计尺寸、每行底 ≤ 按钮行顶、超长文本不挤位、
// 下拉展开态全在屏内）：「弹窗不超出视口」被 dialog 组件自身的 clamp 兜住，恒真。

REGISTER_TEST(agent_ui_dialog_geometry_design_size_and_budget,
              "agent_ui: 任务弹窗几何——设计尺寸 760x400 / 行底 ≤ 按钮行顶 / 超长目标与超长任务名不挤位 / 下拉展开态全在屏内", {
    test_support::TestDb tdb("agentui", "dlggeom");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    // 超长目标（300 字符 → 多行输入内部滚动压力）；超长任务名绕过
    // deriveTaskName 直接种（模拟库里已有的长名）
    const std::string longGoal(300, 'g');
    const std::string longName(120, 'N');
    auto created = store.createTask("几何任务", longGoal, "schedule", "default", 10,
                                    false, R"({"cron":"0 9 * * 1-5"})");
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto longNamed = store.createTask(longName, "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(longNamed.isOk());
    const std::string nid = std::to_string(longNamed.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_geo1");
    DOCTEST_REQUIRE(clickId(ui, "agent.task." + tid + ".edit"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_geo2");

    // ① 面板设计尺寸：我们自己钉的值——「不超视口」被 dialog 组件的 clamp 兜住，零判别力
    const auto* panel = ui2.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 760.0f),
                          "任务弹窗宽应为设计值 760，实测 " +
                              std::to_string(panel->frame.width));
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.height, 400.0f),
                          "任务弹窗高应为设计值 400，实测 " +
                              std::to_string(panel->frame.height));
    DOCTEST_CHECK(panel->frame.x >= -0.5f && panel->frame.y >= -0.5f &&
                  panel->frame.x + panel->frame.width <= kScreen.width + 0.5f &&
                  panel->frame.y + panel->frame.height <= kScreen.height + 0.5f);

    // ② 目标输入多行 + 行底 ≤ 按钮行顶（弹窗高被改小 / 行被加高即红）
    const auto* goal = ui2.find("agent.dialog.task.goal");
    DOCTEST_REQUIRE(goal != nullptr);
    DOCTEST_CHECK_MESSAGE(goal->frame.height >= 90.0f,
                          "编辑弹窗 goal 输入应 ≥90px，实测 " +
                              std::to_string(goal->frame.height));
    const auto* metaRow = ui2.find("agent.dialog.task.destructive.wrap");
    const auto* saveBtn = ui2.find("agent.dialog.task.save");
    DOCTEST_REQUIRE_MESSAGE(metaRow != nullptr, "元信息行（破坏性开关）缺失");
    DOCTEST_REQUIRE_MESSAGE(saveBtn != nullptr, "保存按钮缺失");
    DOCTEST_CHECK_MESSAGE(
        metaRow->frame.y + metaRow->frame.height <= saveBtn->frame.y + 0.5f,
        "最后一行字段底 " +
            std::to_string(metaRow->frame.y + metaRow->frame.height) +
            " 压到按钮行顶 " + std::to_string(saveBtn->frame.y));
    DOCTEST_CHECK_MESSAGE(
        saveBtn->frame.y + saveBtn->frame.height <=
            panel->frame.y + panel->frame.height - 19.5f,
        "按钮行溢出面板底（底边距应收 20）");
    DOCTEST_CHECK(nearPx(saveBtn->frame.height, 40.0f));
    // 超长目标不挤动输入框自身几何（宽 = 面板内宽 704 - 标签 72 - 间距 12 = 620、高 = 设计 96）。
    // 不要改用「读回 InputState 的文本长度」当判据：那是只读输入框模型，只能验「值没丢」，
    // 验不出「框没被挤」；行位置恒等在下面单独钉
    DOCTEST_CHECK_MESSAGE(nearPx(goal->frame.width, 620.0f),
                          "goal 输入框宽应恒为 620（超长文本不得挤动盒宽），实测 " +
                              std::to_string(goal->frame.width));
    DOCTEST_CHECK_MESSAGE(nearPx(goal->frame.height, 96.0f),
                          "goal 输入框高应恒为 96，实测 " +
                              std::to_string(goal->frame.height));
    DOCTEST_CHECK_MESSAGE(nearPx(metaRow->frame.y, panel->frame.y + 224.0f),
                          "元信息行 y 应恒为面板顶 +224，实测 " +
                              std::to_string(metaRow->frame.y));

    // ③ 下拉展开态
    // 判据必须是只有展开态才成立且能失败的属性。`.popup` / `.item.<i>` 是 dropdown
    // 无条件构建的子节点（无 open 守卫），收起态一样存在、一样非零、一样在屏内——
    // 拿它们的存在性/几何单判是恒真断言。故改钉开档属性对：弹层 opacity（收起 0 /
    // 展开 1）、zIndex（收起 0 / 展开 >0）、命中层 disabled（收起 true / 展开 false），
    // 后面的几何断言（最后一项在屏内）只有在这对属性成立时才有意义。
    const auto* popupClosed = ui2.find("agent.dialog.task.trigger.popup");
    DOCTEST_REQUIRE_MESSAGE(popupClosed != nullptr, "弹层节点缺失");
    const auto* popupHitClosed = ui2.find("agent.dialog.task.trigger.popup.hit");
    DOCTEST_REQUIRE_MESSAGE(popupHitClosed != nullptr, "弹层命中层缺失");
    DOCTEST_CHECK_MESSAGE(popupClosed->opacity == 0.0f,
                          "收起态弹层应不可见（opacity 0），实测 " +
                              std::to_string(popupClosed->opacity));
    DOCTEST_CHECK_MESSAGE(popupClosed->zIndex == 0,
                          "收起态弹层不应在开档（zIndex 0），实测 " +
                              std::to_string(popupClosed->zIndex));
    DOCTEST_CHECK(popupHitClosed->disabled);

    const float metaYBefore = metaRow->frame.y;
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.dialog.task.trigger.field"),
                            "触发方式下拉字段命中面缺失");
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_geo2b");
    const auto* popup = ui2b.find("agent.dialog.task.trigger.popup");
    DOCTEST_REQUIRE_MESSAGE(popup != nullptr, "弹层节点缺失");
    DOCTEST_CHECK_MESSAGE(popup->opacity == 1.0f,
                          "点开字段后弹层仍不可见（opacity 应 1.0），实测 " +
                              std::to_string(popup->opacity));
    DOCTEST_CHECK_MESSAGE(popup->zIndex > 0,
                          "点开字段后弹层未升到开档（zIndex 应 > 0），实测 " +
                              std::to_string(popup->zIndex));
    const auto* popupHitOpen = ui2b.find("agent.dialog.task.trigger.popup.hit");
    DOCTEST_REQUIRE(popupHitOpen != nullptr);
    DOCTEST_CHECK_FALSE(popupHitOpen->disabled);
    // 展开态几何：最后一项完整在屏内。该断言只有在上面那对开档属性成立时才有意义
    const auto* lastItem = ui2b.find("agent.dialog.task.trigger.item.2");
    DOCTEST_REQUIRE_MESSAGE(lastItem != nullptr, "下拉最后一项（常驻）缺失");
    DOCTEST_CHECK(lastItem->frame.width > 0.0f && lastItem->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        lastItem->frame.y >= -0.5f &&
            lastItem->frame.y + lastItem->frame.height <= kScreen.height + 0.5f,
        "下拉最后一项跑出屏外: y=" + std::to_string(lastItem->frame.y) +
            " 底=" + std::to_string(lastItem->frame.y + lastItem->frame.height));
    // 弹层是覆盖层：展开时盖住下方元信息行是有意行为，不可改成挤动布局——挤动会让
    // 下方行 y 变化，下面那条行位置恒等的断言正是钉这一点
    DOCTEST_CHECK_MESSAGE(popup->frame.y + popup->frame.height > metaYBefore,
                          "弹层应向下覆盖元信息行（覆盖层语义变了？）");
    const auto* metaAfter = ui2b.find("agent.dialog.task.destructive.wrap");
    DOCTEST_REQUIRE(metaAfter != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(metaAfter->frame.y, metaYBefore),
                          "下拉展开挤动了下方行：展开前 " +
                              std::to_string(metaYBefore) + " 展开后 " +
                              std::to_string(metaAfter->frame.y));
    DOCTEST_REQUIRE(clickId(ui2b, "agent.dialog.task.trigger.field"));
    eui::Ui ui2b2;
    composeTo(ui2b2, mod, kScreen, "agent_ui_geo2b2");
    const auto* popupReclosed = ui2b2.find("agent.dialog.task.trigger.popup");
    DOCTEST_REQUIRE(popupReclosed != nullptr);
    DOCTEST_CHECK_MESSAGE(popupReclosed->opacity == 0.0f && popupReclosed->zIndex == 0,
                          "再点字段后弹层未收起（opacity/z 未复位）");
    // 更深的弹层（模型槽位 = X 宏表 6 项，弹层跨过面板底）也不能掉出屏底
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2b2, "agent.dialog.task.slot.field"),
                            "模型槽位下拉字段命中面缺失");
    eui::Ui ui2c;
    composeTo(ui2c, mod, kScreen, "agent_ui_geo2c");
    const auto* slotPopup = ui2c.find("agent.dialog.task.slot.popup");
    DOCTEST_REQUIRE(slotPopup != nullptr);
    DOCTEST_CHECK_MESSAGE(slotPopup->opacity == 1.0f && slotPopup->zIndex > 0,
                          "槽位下拉未展开（opacity/z 未升档）");
    const auto* slotLast = ui2c.find("agent.dialog.task.slot.item.5");
    DOCTEST_REQUIRE_MESSAGE(slotLast != nullptr, "槽位下拉最后一项（子代理）缺失");
    DOCTEST_CHECK(slotLast->frame.width > 0.0f && slotLast->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        slotLast->frame.y >= -0.5f &&
            slotLast->frame.y + slotLast->frame.height <= kScreen.height + 0.5f,
        "槽位下拉最后一项掉出屏底: 底=" +
            std::to_string(slotLast->frame.y + slotLast->frame.height));

    // ④ 超长任务名不挤掉行内动作（标题盒仍在动作 chip 左侧）
    DOCTEST_REQUIRE(clickId(ui2c, "agent.dialog.task.cancel.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_geo3");
    const auto* longTitle = ui3.find("agent.task." + nid + ".title");
    const auto* detailChip = ui3.find("agent.task." + nid + ".detail");
    const auto* longRow = ui3.find("agent.task." + nid);
    DOCTEST_REQUIRE_MESSAGE(longTitle != nullptr && detailChip != nullptr,
                            "超长任务名的行/标题/详情 chip 缺失");
    DOCTEST_CHECK(longTitle->frame.width > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        longTitle->frame.x + longTitle->frame.width <= detailChip->frame.x + 0.5f,
        "超长任务名挤出行内动作：标题右缘 " +
            std::to_string(longTitle->frame.x + longTitle->frame.width) +
            " > 详情 chip 左缘 " + std::to_string(detailChip->frame.x));
    DOCTEST_CHECK_MESSAGE(frameContained(*longRow, *detailChip),
                          "详情 chip 跑到任务行外");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_dialog_geometry_and_empty_state,
              "agent_ui: 详情弹窗设计尺寸 760x660 / 消息区底 ≤ 面板底 / 空态占位垂直居中", {
    test_support::TestDb tdb("agentui", "dgeom");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto withMsgs = store.createTask("有消息", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(withMsgs.isOk());
    auto noMsgs = store.createTask("无消息", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(noMsgs.isOk());
    for (int64_t id : {withMsgs.value(), noMsgs.value()}) {
        auto r = store.startRun(id);
        DOCTEST_REQUIRE(r.isOk());
        if (id == withMsgs.value()) {
            DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                                R"([{"type":"text","text":"你好"}])",
                                                std::nullopt).isOk());
        }
        DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                        std::nullopt, std::nullopt).isOk());
    }
    const std::string tid = std::to_string(withMsgs.value());
    const std::string nid = std::to_string(noMsgs.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 有消息：设计尺寸 + 消息区底 ≤ 面板底
    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_dg1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dg2");
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 760.0f),
                          "详情弹窗宽应为设计值 760，实测 " +
                              std::to_string(panel->frame.width));
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.height, 660.0f),
                          "详情弹窗高应为设计值 660，实测 " +
                              std::to_string(panel->frame.height));
    const auto* scroll = ui2.find("agent.dialog.detail.msgscroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_CHECK(scroll->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        scroll->frame.y + scroll->frame.height <=
            panel->frame.y + panel->frame.height - 19.5f,
        "消息滚动区溢出面板底：底 " +
            std::to_string(scroll->frame.y + scroll->frame.height) + " 面板底 " +
            std::to_string(panel->frame.y + panel->frame.height));
    const auto* runsTitle = ui2.find("agent.dialog.detail.runs.title");
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    const auto* msgsTitle = ui2.find("agent.dialog.detail.msgs.title");
    DOCTEST_REQUIRE(runsTitle != nullptr && runlist != nullptr &&
                    msgsTitle != nullptr);
    DOCTEST_CHECK_MESSAGE(
        runlist->frame.y + runlist->frame.height <= msgsTitle->frame.y + 0.5f,
        "run 列表压到消息标题");

    // 无消息：走 .empty 分支，占位文本垂直居中
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.close"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dg3");
    DOCTEST_REQUIRE(openDetailDialog(ui3, mod, nid));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_dg4");
    const auto* empty = ui4.find("agent.dialog.detail.empty");
    DOCTEST_REQUIRE_MESSAGE(empty != nullptr, "无消息 run 未渲染空态占位");
    DOCTEST_CHECK_MESSAGE(empty->verticalAlign == eui::VerticalAlign::Center,
                          "空态占位应垂直居中（verticalAlign(Center)）");
    DOCTEST_CHECK(empty->frame.height > 0.0f);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_sidebar_registers_agent,
              "agent_ui: 真 app::compose 侧边栏出现「智能体」项（Router 注册 AgentModule）", {
    test_support::TestDb tdb("agentui", "sidebar");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    ::core::Router router;
    router.registerModule(&mod);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::Agent);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("agent_ui_sidebar");
    app::compose(ui, screen);
    ui.end();

    const auto* label = ui.find("app.sidebar.item.Agent.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("智能体"));
    DOCTEST_CHECK(ui.find("app.sidebar.item.Agent.hit") != nullptr);
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::Agent);
    DOCTEST_CHECK(ui.find("agent.title") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_resident_enqueue_starts_run,
              "agent_ui: 常驻任务行的「入队」按钮经 Scheduler 真派发起 run（落 completed）", {
    test_support::TestDb tdb("agentui", "enqueue");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("常驻任务", "直接回复完成，不调用任何工具",
                                   "resident", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"done"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_enqueue");
    const auto* enqueue = ui.find("agent.task." + tid + ".enqueue");
    DOCTEST_REQUIRE_MESSAGE(enqueue != nullptr,
                            "常驻任务行缺 agent.task.<id>.enqueue 按钮");
    DOCTEST_REQUIRE(enqueue->onClick != nullptr);
    enqueue->onClick();

    bool completed = false;
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "入队后 5s 内未落库 completed（未接到 scheduler？）");
    mod.onStop();
    return true;
})

REGISTER_TEST(agent_has_live_run_excludes_orphaned_finish_failure,
              "agent: 判停忽略孤儿 run（收尾写库失败 → 行永远 running）", {
    // AgentLoop 收尾 finishRun 写库失败时，DB 那行永远停在 'running'，而 run 线程早已结束。
    // 若「有 running 行」就当作「还有 run 在跑」，uiPolling_ 永不复位 → 此后每帧一次
    // refreshData()（N+1 次查询）直到重启。触发条件窄（磁盘满 / DB 写错），后果重。
    //
    // 本用例钉死 hasLiveRun 的排除语义：孤儿不算活 run（否则每帧轮询），真在跑的仍然算
    // （否则 UI 不刷新，真 run 跑完了都不更新状态）。
    // 夹具口径照真实结构：latestRuns_ 是「taskId → 最近一次 run」，AgentRunRow 自带 id
    // （runId）；孤儿集合按 runId 存（FinishFailureFn 回报的就是 runId）——把 taskId 与
    // runId 混用正是判停失效的成因，⑦ 专门钉这条。
    auto running = [](int64_t runId, int64_t taskId) {
        module::agent::AgentRunRow r;
        r.id = runId;
        r.taskId = taskId;
        r.status = "running";
        return r;
    };
    auto done = [](int64_t runId, int64_t taskId) {
        module::agent::AgentRunRow r;
        r.id = runId;
        r.taskId = taskId;
        r.status = "completed";
        return r;
    };

    DOCTEST_CHECK(module::agent::hasLiveRun({{1, running(10, 1)}}, {}));
    DOCTEST_CHECK_FALSE(module::agent::hasLiveRun({{1, running(10, 1)}}, {10}));
    // 孤儿 + 另一个真在跑的 → 仍然有活 run：不能因为有一个孤儿就整体停轮询
    DOCTEST_CHECK(module::agent::hasLiveRun(
        {{1, running(10, 1)}, {2, running(11, 2)}}, {10}));
    DOCTEST_CHECK_FALSE(module::agent::hasLiveRun({{1, done(10, 1)}}, {}));
    // 空表 → 没有活 run（不能崩，也不能因空表恒真）
    DOCTEST_CHECK_FALSE(module::agent::hasLiveRun({}, {}));
    DOCTEST_CHECK(module::agent::hasLiveRun({{1, running(10, 1)}}, {99}));
    // ⑦ 按 taskId（map 的键）而不是 runId 去匹配孤儿，必须匹配不上：runId 10 / taskId 1，
    // 传 {1} 不该把它排除掉、仍算活 run。传 {1} 若被排除 = 拿错了键，排除逻辑形同虚设
    DOCTEST_CHECK(module::agent::hasLiveRun({{1, running(10, 1)}}, {1}));
    return true;
})

// ===================== Task 1：run 完成桌面通知（迁移检测纯逻辑 + toast 接线） =====================

REGISTER_TEST(agent_run_end_notify_transition_rules,
              "agent: run 完成迁移检测纯逻辑（completed 静默 / failed·limited 必响 / 同 runId 不重复）", {
    using StatusMap = std::map<int64_t, std::string>;

    // ① 上轮 running 本轮 completed → 不通知（completed 静默）
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{10, "completed"}}, {})
                      .empty());
    // ② 上轮 running 本轮 failed → 通知一次（恰好一个且是它）
    {
        const auto r = module::agent::detectRunEndNotifications(
            {{10, "running"}}, {{10, "failed"}}, {});
        DOCTEST_REQUIRE(r.size() == 1);
        DOCTEST_CHECK(r[0] == 10);
    }
    // ③ 上轮 running 本轮 limited → 同样必响
    {
        const auto r = module::agent::detectRunEndNotifications(
            {{10, "running"}}, {{10, "limited"}}, {});
        DOCTEST_REQUIRE(r.size() == 1);
        DOCTEST_CHECK(r[0] == 10);
    }
    // ④ 同 runId 二次迁移（已进已通知集合）→ 不再通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{10, "failed"}}, {10})
                      .empty());
    // ⑤ 稳态不算迁移：failed→failed 不通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "failed"}}, {{10, "failed"}}, {})
                      .empty());
    // ⑥ prev 缺席（未观测过 running，如 run 结束后才进页）→ 不通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      StatusMap{}, {{10, "failed"}}, {})
                      .empty());
    // ⑦ cancelled 静默（不在必响名单：completed/cancelled 都不打扰）
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{10, "cancelled"}}, {})
                      .empty());
    // ⑧ 混合迁移：failed + completed 并存 → 只通知 failed 那个 runId
    {
        const auto r = module::agent::detectRunEndNotifications(
            {{10, "running"}, {11, "running"}},
            {{10, "completed"}, {11, "failed"}}, {});
        DOCTEST_REQUIRE(r.size() == 1);
        DOCTEST_CHECK(r[0] == 11);
    }
    // ⑨ run 掉出本轮快照（被新 run 顶出最近列表）→ 不通知，不误报
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{11, "running"}}, {})
                      .empty());
    // ⑩ 空快照不崩、不通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      StatusMap{}, StatusMap{}, {})
                      .empty());
    return true;
})

REGISTER_TEST(agent_ui_run_failed_shows_toast,
              "agent_ui: run 失败 → 通知入队（failed 必响，标题含失败、正文含任务名）", {
    test_support::TestDb tdb("agentui", "failtoast");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("会失败的任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    deps.http = [](const platform::http::Request&) {
        // 拖慢失败返回：保证轮询帧先观测到 running（否则首帧即终态 → 无迁移、无通知）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return core::Result<platform::http::Response>::fail(
            core::ErrorCode::Internal, "connection refused");
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_failtoast1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 有界轮询（10ms 步进 ≤5s）+ 每轮 recompose：迁移发生的帧通知必须入队
    //（fix round 起页面内不再画 agent.toast，通知统一由装配层取走发 app 级 toast）
    bool toasted = false;
    eui::Ui ui2;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_failtoast_poll");
        const auto notice = mod.takeRunEndNotice();
        if (notice.has_value()) {
            DOCTEST_CHECK_MESSAGE(notice->first.find("失败") != std::string::npos,
                                   "通知标题不含「失败」，实测: " + notice->first);
            DOCTEST_CHECK_MESSAGE(notice->second.find("会失败的任务") != std::string::npos,
                                   "通知正文不含任务名，实测: " + notice->second);
            const auto again = mod.takeRunEndNotice();
            DOCTEST_CHECK_MESSAGE(!again.has_value(),
                                  "同 runId 不应通知两次（取走即清）");
            toasted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(toasted, "run 失败后 5s 内未产生 run 结束通知");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_run_completed_silent_no_toast,
              "agent_ui: run 成功 → 不产生 run 结束通知（completed 静默）", {
    test_support::TestDb tdb("agentui", "silentok");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("静默任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                    R"({"type":"text","text":"done"}],)"
                    R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_silent1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 轮询到落库 completed，再补 3 帧确认迁移帧过后 toast 仍空（completed 静默）
    bool completed = false;
    eui::Ui ui2;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_silent_poll");
        auto runs = store.listRuns(taskId.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "run 5s 内未落库 completed");
    for (int i = 0; i < 3; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_silent_tail");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto notice = mod.takeRunEndNotice();
    DOCTEST_CHECK_MESSAGE(!notice.has_value(),
                          "completed 迁移不该产生通知，实测: " +
                              (notice.has_value() ? notice->first
                                                  : std::string("<无>")));

    mod.onStop();
    return true;
})

// ===================== Task 1 fix round：通知走 app 级通道（页面无关） =====================

// 「非智能体页也能发通知」：run 失败迁移发生在 Home 页（非 Agent 页）时，装配层
// 每帧拉取路径（app::compose 内 poll + take + showAppNotice）必须把通知送达
// app.notice toast。守的是接线方向：模块只暴露状态，装配层拉取后发应用级通知。
REGISTER_TEST(agent_run_end_notice_app_channel_off_page,
              "agent_ui: run 失败迁移在非智能体页时经装配层拉取路径弹 app.notice（跨页可见）", {
    test_support::TestDb tdb("agentui", "appchan");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("跨页通知任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    deps.http = [](const platform::http::Request&) {
        // 拖慢失败返回：保证轮询帧先观测到 running（否则首帧即终态 → 无迁移、无通知）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return core::Result<platform::http::Response>::fail(
            core::ErrorCode::Internal, "connection refused");
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 阶段一：在 Agent 页启动 run（唯一合法启动入口是页面按钮）
    eui::Ui uiStart;
    composeTo(uiStart, mod, kScreen, "agent_appchan_start");
    DOCTEST_REQUIRE_MESSAGE(clickId(uiStart, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 阶段二：切到 Home 页（非 Agent 页）——迁移检测与通知都必须与 Agent 页可见性解耦
    ::core::Router router;
    router.registerModule(&mod);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::Home);
    DOCTEST_CHECK(router.currentPage() != ::core::PageId::Agent);

    // 阶段三：循环 app::compose（等价装配层每帧拉取）。迁移发生的那帧，
    // app.notice（应用级 toast，跨页可见）必须带上「失败」标题与任务名
    bool toasted = false;
    eui::Ui uiApp;
    for (int i = 0; i < 500; ++i) {
        uiApp.begin("agent_appchan_frame");
        app::compose(uiApp, screen);
        uiApp.end();
        const auto* title = uiApp.find("app.notice.title");
        if (title != nullptr && !title->text.empty()) {
            DOCTEST_CHECK_MESSAGE(title->text.find("失败") != std::string::npos,
                                   "app 通知标题不含「失败」，实测: " + title->text);
            const auto* msg = uiApp.find("app.notice.message");
            DOCTEST_REQUIRE_MESSAGE(msg != nullptr, "app 通知缺正文节点");
            DOCTEST_CHECK_MESSAGE(msg->text.find("跨页通知任务") != std::string::npos,
                                  "app 通知正文不含任务名，实测: " + msg->text);
            toasted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(toasted, "非 Agent 页期间 run 失败，5s 内未弹出 app.notice");

    mod.onStop();
    return true;
})

// 取走语义：takeRunEndNotice 一次取走后清空，同 runId 不重复（守 notifiedRunIds_
// 经 app 通道的同一约束——装配层拉两帧不得收到两条）
REGISTER_TEST(agent_run_end_notice_take_semantics,
              "agent_ui: takeRunEndNotice 取走即清（同 runId 两帧只取到一次）", {
    test_support::TestDb tdb("agentui", "takenotice");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("取走语义任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 无迁移时取走 = 空
    {
        const auto n = mod.takeRunEndNotice();
        DOCTEST_CHECK(!n.has_value());
    }

    // 直写一条终态 run（running → failed），再经 poll 触发迁移：take 必须恰好取到一次
    {
        auto runId = store.startRun(taskId.value());
        DOCTEST_REQUIRE(runId.isOk());
        // 先 poll 一次让 prevRunStatuses_ 捕获 running（迁移判据 = prev running → now 终态）
        mod.pollRunEnd();
        DOCTEST_REQUIRE(store.finishRun(runId.value(), "failed", 1,
                                        std::optional<int64_t>{},
                                        std::optional<int64_t>{},
                                        "api broken").isOk());

        mod.pollRunEnd();   // 装配层等价路径：轮询检测（不依赖页面 compose）
        const auto first = mod.takeRunEndNotice();
        DOCTEST_REQUIRE_MESSAGE(first.has_value(), "迁移发生后未取到通知");
        DOCTEST_CHECK_MESSAGE(first->first.find("失败") != std::string::npos,
                               "通知标题不含「失败」，实测: " + first->first);
        DOCTEST_CHECK_MESSAGE(first->second.find("取走语义任务") != std::string::npos,
                               "通知正文不含任务名，实测: " + first->second);
        const auto second = mod.takeRunEndNotice();
        DOCTEST_CHECK_MESSAGE(!second.has_value(),
                              "同 runId 第二帧不应再取到通知（取走即清）");
    }

    mod.onStop();
    return true;
})

// Task 2：定时分支模板下拉 + 下次触发预览。布局契约：模板 dropdown 与预览行
// 不得改变弹窗总高（面板仍 400、meta 行仍 +224），预览行只能落在 cron 行与
// meta 行之间的原 gap 内。
REGISTER_TEST(agent_ui_cron_template_dropdown_and_preview,
              "agent_ui: 定时分支模板下拉（≥4 条）选中回填 cron / 预览行随 editCron 即时更新（合法显下次触发、非法显校验、无匹配显一年内）/ 弹窗高不变", {
    test_support::TestDb tdb("agentui", "crontpl");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui1;
    DOCTEST_REQUIRE(openNewTaskDialog(ui1, mod, "agent_ui_crontpl1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_crontpl2");
    DOCTEST_REQUIRE(callDropdownChange(ui2, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_crontpl3");

    // ① 模板下拉存在；item.0 是「选择模板…」占位，item.1..4 对应 kCronTemplates
    DOCTEST_REQUIRE_MESSAGE(ui3.find("agent.dialog.task.cron.template") != nullptr,
                            "模板下拉 agent.dialog.task.cron.template 缺失");
    const auto* tplLabel3 = ui3.find("agent.dialog.task.cron.template.label");
    DOCTEST_REQUIRE_MESSAGE(tplLabel3 != nullptr, "模板下拉 label 缺失");
    DOCTEST_CHECK_EQ(tplLabel3->text, std::string("选择模板…"));
    for (int i = 0; i < module::agent::kCronTemplateCount; ++i) {
        const auto* item = ui3.find("agent.dialog.task.cron.template.item." +
                                    std::to_string(i + 1));
        DOCTEST_REQUIRE_MESSAGE(item != nullptr,
                                "模板项 item." + std::to_string(i + 1) + " 缺失");
        const auto* itemLabel = ui3.find(
            "agent.dialog.task.cron.template.item.label." + std::to_string(i + 1));
        DOCTEST_REQUIRE_MESSAGE(itemLabel != nullptr,
                                "模板项 label." + std::to_string(i + 1) + " 缺失");
        DOCTEST_CHECK_EQ(itemLabel->text,
                         std::string(module::agent::kCronTemplates[i].label));
    }

    // ② 布局契约：面板高仍 400；meta 行仍 +224（模板下拉/预览行未挤动布局）
    const auto* panel3 = ui3.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE(panel3 != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel3->frame.height, 400.0f),
                          "任务弹窗高应恒为 400，实测 " +
                              std::to_string(panel3->frame.height));
    const auto* metaRow3 = ui3.find("agent.dialog.task.destructive.wrap");
    DOCTEST_REQUIRE(metaRow3 != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(metaRow3->frame.y, panel3->frame.y + 224.0f),
                          "meta 行 y 应恒为面板顶 +224，实测 " +
                              std::to_string(metaRow3->frame.y));
    // cron 为空 → 预览行不渲染（skipIfEmpty）
    DOCTEST_CHECK(ui3.find("agent.dialog.task.cron.preview") == nullptr);

    // ③ 选中模板 2（每天 9 点）→ cron 回填 + 预览立即出现
    DOCTEST_REQUIRE(
        callDropdownChange(ui3, "agent.dialog.task.cron.template", 2));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_crontpl4");
    DOCTEST_CHECK_EQ(inputText(ui4, "agent.dialog.task.cron"),
                     std::string("0 9 * * *"));
    const auto* preview4 = ui4.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(preview4 != nullptr, "合法 cron 未渲染预览行");
    DOCTEST_CHECK_MESSAGE(preview4->text.rfind("下次触发：", 0) == 0,
                          "预览行应以「下次触发：」开头，实测: " + preview4->text);
    // 预览行落在 cron 行与 meta 行之间（原 gap 内）
    const auto* cronRow4 = ui4.find("agent.dialog.task.cron");
    DOCTEST_REQUIRE(cronRow4 != nullptr);
    const auto* metaRow4 = ui4.find("agent.dialog.task.destructive.wrap");
    DOCTEST_REQUIRE(metaRow4 != nullptr);
    DOCTEST_CHECK_MESSAGE(
        preview4->frame.y >= cronRow4->frame.y + cronRow4->frame.height - 0.5f,
        "预览行应在 cron 行之下");
    DOCTEST_CHECK_MESSAGE(
        preview4->frame.y + preview4->frame.height <= metaRow4->frame.y + 0.5f,
        "预览行压到 meta 行：底 " +
            std::to_string(preview4->frame.y + preview4->frame.height) +
            " meta 顶 " + std::to_string(metaRow4->frame.y));

    // ④ 手改非法表达式 → 预览位显示校验文案（不等到保存才报）
    mod.setEditCronDraft("0 9 * * 1-5x");
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_crontpl5");
    const auto* preview5 = ui5.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(preview5 != nullptr, "非法 cron 未渲染预览行");
    DOCTEST_CHECK_MESSAGE(preview5->text.find("cron 表达式非法") !=
                              std::string::npos,
                          "非法 cron 预览应显校验文案，实测: " + preview5->text);

    // ⑤ 无匹配（2 月 31 日）→ 预览显「一年内无触发时间」
    mod.setEditCronDraft("0 9 31 2 *");
    eui::Ui ui6;
    composeTo(ui6, mod, kScreen, "agent_ui_crontpl6");
    const auto* preview6 = ui6.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(preview6 != nullptr, "无匹配 cron 未渲染预览行");
    DOCTEST_CHECK_MESSAGE(preview6->text == "一年内无触发时间",
                          "无匹配预览文案错误，实测: " + preview6->text);

    mod.onStop();
    return true;
})

// ---------------------------------------------------------------------------
// Task 4：错误行「去 Claude 页」直达引导
// ---------------------------------------------------------------------------

// 跳转目标占位模块：navigateTo 对未注册页回退 Home，判据 currentPage()==ClaudeProxy
// 需要 ClaudeProxy 已注册
class StubPageModule : public core::Module {
public:
    explicit StubPageModule(core::PageId page) : page_(page) {}
    core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return "stub"; }
    std::string iconGlyph() const override { return "x"; }
    void compose(eui::Ui&, const eui::Screen&) override {}

private:
    core::PageId page_;
};

// 两类错误原文常量：与 llm_client.cpp:56 / :63 逐字一致（文案一变本组用例即红——
// 前缀匹配刻意脆弱，防文案漂移后引导静默失效）
const char* const kProxyNotRunningErr =
    "代理未启，请先在 Claude 页启动代理";
const char* const kSlotResolveErr =
    "无法解析模型槽位「default」（无激活 profile 或槽位未配置）";

REGISTER_TEST(agent_needs_claude_page_hint_predicate,
              "agent: needsClaudePageHint 前缀匹配两类 LLM 预检错误原文，其余一律 false", {
    using module::agent::needsClaudePageHint;
    // 正例：llm_client.cpp:56 / :63 原文钉死
    DOCTEST_CHECK(needsClaudePageHint(kProxyNotRunningErr));
    DOCTEST_CHECK(needsClaudePageHint(kSlotResolveErr));
    // 同前缀 + 追加后缀仍匹配（错误行拼接 / 重试提示等）
    DOCTEST_CHECK(needsClaudePageHint(
        std::string(kProxyNotRunningErr) + "（已重试 3 次）"));
    DOCTEST_CHECK(needsClaudePageHint(
        std::string(kSlotResolveErr) + "，请检查配置"));
    // 负例：不含两前缀的普通错误一律 false（「代理未启动」以「代理未启」开头，
    // 属前缀匹配的正当命中——它是 app 级 toast 标题，不经错误行，不构成误配面）
    DOCTEST_CHECK(!needsClaudePageHint(""));
    DOCTEST_CHECK(!needsClaudePageHint("错误文案 1"));
    DOCTEST_CHECK(!needsClaudePageHint("读取任务失败：库锁"));
    DOCTEST_CHECK(!needsClaudePageHint("启停切换失败：库锁"));
    DOCTEST_CHECK(!needsClaudePageHint("落库 agent_messages 失败：disk full"));
    DOCTEST_CHECK(!needsClaudePageHint("请求失败：connection refused"));
    return true;
})

REGISTER_TEST(agent_ui_detail_error_hint_navigates_to_claude,
              "agent_ui: 详情弹窗两类错误行下出现「去 Claude 页」可点引导，点击跳转 Claude 页", {
    test_support::TestDb tdb("agentui", "errhint");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("错误引导任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    // 两条 run 分别落两类错误原文（runlist 按时间倒序：runlist.1 在前 = 最新）
    auto r1 = store.startRun(created.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kProxyNotRunningErr).isOk());
    auto r2 = store.startRun(created.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kSlotResolveErr).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_hintnav1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_hintnav2");

    // 两处错误行下都有引导（id 契约：可点元素带 text 与 onClick）
    for (const char* row : {"agent.dialog.detail.runlist.0",
                            "agent.dialog.detail.runlist.1"}) {
        const std::string hintId = std::string(row) + ".err.hint";
        const auto* hint = ui2.find(hintId);
        DOCTEST_REQUIRE_MESSAGE(hint != nullptr,
                                std::string(row) + " 下缺「去 Claude 页」引导");
        DOCTEST_CHECK_MESSAGE(hint->text.find("去 Claude 页") != std::string::npos,
                              "引导文案缺「去 Claude 页」，实测: " + hint->text);
        DOCTEST_REQUIRE_MESSAGE(hint->onClick != nullptr,
                                 "引导元素不可点（缺 onClick）");
    }
    // 引导行与错误行不重叠（childrenH 同步的证据：hint 也是子元素）
    const auto* err0 = ui2.find("agent.dialog.detail.runlist.0.err");
    const auto* hint0 = ui2.find("agent.dialog.detail.runlist.0.err.hint");
    const auto* msgsTitle = ui2.find("agent.dialog.detail.msgs.title");
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(err0 != nullptr && hint0 != nullptr && msgsTitle != nullptr &&
                    panel != nullptr);
    DOCTEST_CHECK(err0->frame.y + err0->frame.height <= hint0->frame.y + 0.5f);
    DOCTEST_CHECK(hint0->frame.y + hint0->frame.height <=
                  msgsTitle->frame.y + 0.5f);
    DOCTEST_CHECK(hint0->frame.y + hint0->frame.height <=
                  panel->frame.y + panel->frame.height);

    // 点击引导 → Router 切 Claude 页（须 ClaudeProxy 已注册，否则回退 Home）
    ::core::Router router;
    router.registerModule(&mod);
    StubPageModule claudePage(core::PageId::ClaudeProxy);
    router.registerModule(&claudePage);
    test_support::ScopedAppRouter scoped(&router);
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.0.err.hint"));
    DOCTEST_CHECK_MESSAGE(router.currentPage() == ::core::PageId::ClaudeProxy,
                          "点击引导未跳转 Claude 页，实测: " +
                              std::string(::core::pageIdToString(
                                  router.currentPage())));
    // 第二条错误行的引导同样可跳（从 Claude 页回 Agent 后再点）
    router.navigateTo(::core::PageId::Agent);
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.1.err.hint"));
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::ClaudeProxy);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_error_hint_only_two_classes,
              "agent_ui: 其它错误（网络/落库/普通文案）不渲染「去 Claude 页」引导", {
    test_support::TestDb tdb("agentui", "errhintneg");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("普通失败任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    const std::string otherErrs[] = {
        "请求失败：connection refused",
        "落库 agent_messages 失败：disk full",
        "错误文案 0",
    };
    for (const std::string& err : otherErrs) {
        auto r = store.startRun(created.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), "failed", 1, int64_t(10),
                                        int64_t(1), err).isOk());
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_hintneg1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_hintneg2");
    for (int i = 0; i < 3; ++i) {
        const std::string hintId =
            "agent.dialog.detail.runlist." + std::to_string(i) + ".err.hint";
        DOCTEST_CHECK_MESSAGE(ui2.find(hintId) == nullptr,
                              "非两类错误渲染了引导: " + hintId);
        const std::string errId =
            "agent.dialog.detail.runlist." + std::to_string(i) + ".err";
        DOCTEST_CHECK(ui2.find(errId) != nullptr);   // 错误行本身仍在
    }

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_page_error_hint_navigates_to_claude,
              "agent_ui: 页内错误行同两类错误 → 同样「去 Claude 页」引导（弹窗外路径）", {
    test_support::TestDb tdb("agentui", "pagehint");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("页内引导任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    // 两类错误各一条 run（同一任务，最新在前）
    auto r1 = store.startRun(created.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kSlotResolveErr).isOk());
    auto r2 = store.startRun(created.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "limited", 1, int64_t(10),
                                    int64_t(1), kProxyNotRunningErr).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_pagehint1");
    const auto* pageErr = ui.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(pageErr != nullptr,
                            "run 两类错误未同步到页内错误行（弹窗外零可见出口）");
    DOCTEST_CHECK(pageErr->text.find("代理未启") == 0);
    const auto* hint = ui.find("agent.page.error.hint");
    DOCTEST_REQUIRE_MESSAGE(hint != nullptr, "页内错误行下缺「去 Claude 页」引导");
    DOCTEST_CHECK_MESSAGE(hint->text.find("去 Claude 页") != std::string::npos,
                          "引导文案缺「去 Claude 页」，实测: " + hint->text);
    DOCTEST_REQUIRE_MESSAGE(hint->onClick != nullptr, "页内引导不可点（缺 onClick）");

    // 几何：引导行在错误行之下、不压工具区头部；错误+引导挤占的 listH 已扣减
    const auto* toolsHeader = ui.find("agent.tools.header");
    const auto* tasklist = ui.find("agent.tasklist");
    DOCTEST_REQUIRE(toolsHeader != nullptr && tasklist != nullptr);
    DOCTEST_CHECK(pageErr->frame.y + pageErr->frame.height <=
                  hint->frame.y + 0.5f);
    DOCTEST_CHECK(hint->frame.y + hint->frame.height <=
                  toolsHeader->frame.y + 0.5f);
    DOCTEST_CHECK(tasklist->frame.y + tasklist->frame.height <=
                  kScreen.height + 0.5f);

    // 点击引导 → Router 切 Claude 页
    ::core::Router router;
    router.registerModule(&mod);
    StubPageModule claudePage(core::PageId::ClaudeProxy);
    router.registerModule(&claudePage);
    test_support::ScopedAppRouter scoped(&router);
    DOCTEST_REQUIRE(clickId(ui, "agent.page.error.hint"));
    DOCTEST_CHECK_MESSAGE(router.currentPage() == ::core::PageId::ClaudeProxy,
                          "点击页内引导未跳转 Claude 页，实测: " +
                              std::string(::core::pageIdToString(
                                  router.currentPage())));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_page_error_no_hint_other_errors,
              "agent_ui: 页内错误行非两类错误（读库失败/普通 run 失败）不渲染引导", {
    test_support::TestDb tdb("agentui", "pagehintneg");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("页内无引导任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "failed", 1, int64_t(10),
                                    int64_t(1), "请求失败：connection refused").isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 普通 run 失败不同步页内错误行（非两类错误）
    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_pagehintneg1");
    DOCTEST_CHECK(ui.find("agent.page.error") == nullptr);
    DOCTEST_CHECK(ui.find("agent.page.error.hint") == nullptr);

    // 读库失败走页内错误行（既有路径）但不渲染引导
    DOCTEST_REQUIRE(tdb.handle().exec("DELETE FROM agent_tasks WHERE id=" + tid).isOk());
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_pagehintneg2");
    DOCTEST_REQUIRE(clickId(ui2, "agent.task." + tid + ".edit"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_pagehintneg3");
    DOCTEST_REQUIRE(ui3.find("agent.page.error") != nullptr);
    DOCTEST_CHECK(ui3.find("agent.page.error.hint") == nullptr);

    mod.onStop();
    return true;
})
