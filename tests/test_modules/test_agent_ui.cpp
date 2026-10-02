// tests/test_modules/test_agent_ui.cpp
// 智能体页 UI 测试（agent M1 Task 13；2026-10-02 批次 A「骨架重做」重写）。
//
// 2026-10-02 骨架契约（agent_module.cpp 头注释逐字）：
//   页面   agent.page / agent.page.header / agent.title / agent.page.newTask /
//          agent.tools.*（可用工具区块）/ agent.tasklist（scrollView）+.content /
//          agent.task.<id>（.title / .status / .detail / .edit / .toggle /
//          [.enqueue 仅常驻] / .run）
//   任务弹窗 agent.dialog.task.*（新建 / 编辑共用）：
//          .goal（多行）/ .trigger / .cron（仅定时）/ .slot / .iter /
//          .destructive / .cancel / .save / .close / .error（有错才渲染）
//   详情弹窗 agent.dialog.detail.*：.info / .runlist.<i>[.err] / .msgscroll /
//          .msg.<i>[.badge / .toggle / .preview / .blk.<j>[.out]] / .close
// 旧页内表单（agent.form.* / agent.edit.*）与页内详情展开区已删除——本文件
// 用「节点不存在」把它们钉死，防回潮。
//
// 无头口径（probe-recipe §5 + 既有先例）：
//   - 裸 eui::Ui + ui.begin/mod.compose/ui.end + ui.layout（frame 靠 layout 填充）；
//   - 按钮/行/弹窗关闭面点击 = 直调元素上真实挂的回调：button 的 onClick 挂在
//     <id>.bg（components/button.h:145-157），弹窗遮罩挂 <dialog>.backdrop；
//   - 文本输入 = 对 <id>.hit 直调 onTextInput（真实 input 事件路径）；
//   - 下拉切换 = 直调 <id>.item.<i> 的 onClick（components/dropdown.h）；
//   - 开关切换 = 直调 <id>.hit 的 onClick（components/switch.h）。
//
// probe 侧（真窗口几何 + 真命中分发）在 tests/probes/agent_probes.cpp。
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
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

// 测试用 fake 代理服务（与 test_agent_tools.cpp / test_llm_client.cpp 同款）
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

// ============ 1) 页面骨架 ============

REGISTER_TEST(agent_ui_compose_skeleton,
              "agent_ui: 空库页面骨架（列表是 scrollView / 旧页内表单节点已删 / 弹窗未开不渲染）", {
    test_support::TestDb tdb("agentui", "skeleton");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");  // 起点干净

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

    // 顶部「+ 新建任务」入口（唯一的新建通道）
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

    // 可用工具区块仍在
    DOCTEST_CHECK(ui.find("agent.tools.header") != nullptr);

    // 弹窗未打开：弹窗内容不渲染（打开/关闭状态由节点存在性直接可判）
    DOCTEST_CHECK(ui.find("agent.dialog.task.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.dialog.detail.msgscroll") == nullptr);

    // 空库无任务行（fresh 库首个任务 id = 1）
    DOCTEST_CHECK(ui.find("agent.task.1") == nullptr);

    mod.onStop();
    return true;
})

// ============ 2) 任务弹窗全字段 + cron 条件渲染 ============

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
    // 无错时不渲染错误行
    DOCTEST_CHECK(ui2.find("agent.dialog.task.error") == nullptr);

    // 任务目标 = 多行输入（≥90 高；单行时代 40）
    const auto* goal = ui2.find("agent.dialog.task.goal");
    DOCTEST_REQUIRE(goal != nullptr);
    DOCTEST_CHECK_MESSAGE(goal->frame.height >= 90.0f,
                          "弹窗 goal 输入应 ≥90px，实测 " +
                              std::to_string(goal->frame.height));

    // 默认触发方式 = 手动 → 无 cron 输入
    DOCTEST_CHECK(ui2.find("agent.dialog.task.cron") == nullptr);
    DOCTEST_REQUIRE_MESSAGE(callDropdownChange(ui2, "agent.dialog.task.trigger", 1),
                            "agent.dialog.task.trigger.item.1 不存在（下拉未渲染？）");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dlg3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.task.cron") != nullptr,
                          "触发方式=定时 时未渲染 agent.dialog.task.cron");
    DOCTEST_CHECK(ui3.find("agent.dialog.task.cron.caption") != nullptr);

    // 切回手动 → cron 消失
    DOCTEST_REQUIRE(callDropdownChange(ui3, "agent.dialog.task.trigger", 0));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_dlg4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.cron") == nullptr);

    mod.onStop();
    return true;
})

// ============ 3) 新建态保存：落库字段与表单取值一致 ============

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

    // 触发方式 = 定时 → cron 行出现
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

    // 保存成功 → 弹窗关闭（内容节点消失）+ 任务行出现在列表
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_save5");
    DOCTEST_CHECK(ui5.find("agent.dialog.task.goal") == nullptr);
    DOCTEST_CHECK(ui5.find("agent.task." + std::to_string(row.id) + ".title") != nullptr);

    mod.onStop();
    return true;
})

// ============ 4) 非法 cron：弹窗内报错 + 不落库 + 弹窗不关 ============

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
    // 弹窗不关
    DOCTEST_CHECK(ui4.find("agent.dialog.task.goal") != nullptr);
    // 不落库
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

// ============ 5) 取消 / ✕ / 遮罩 / Esc：都不落库且关弹窗 ============

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

    // 打开新建弹窗并敲入 goal（返回 false = 夹具没建成，调用方 REQUIRE 转红）
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

    // 四条路径都不落库
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

// ============ 6) 编辑态：预填 + updateTask ============

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

    // 改槽位 + 迭代上限后保存
    DOCTEST_REQUIRE(callDropdownChange(ui2, "agent.dialog.task.slot", 3));  // haiku
    mod.setDialogIterDraft(3);
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_edit3");
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.save.bg"));

    auto updated = store.getTask(created.value());
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().goal, std::string("旧目标"));         // 未动
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig,
                     std::string(R"({"cron":"0 9 * * 1-5"})"));            // 预填后原样回写
    DOCTEST_CHECK_EQ(updated.value().modelSlot, std::string("haiku"));
    DOCTEST_CHECK_EQ(updated.value().maxIterations, 3);
    DOCTEST_CHECK(updated.value().allowDestructive);                       // 预填 true 未翻
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_edit4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.goal") == nullptr);          // 已关

    // 启停仍是行内独立动作（文案随 enabled 翻转 + 落库取反）
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

// ============ 7) 「启动」→ 立即打开详情弹窗 + 最终回复默认可见 ============

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

    // 启动后第一帧详情弹窗就该开（不等 run 跑完；此刻 run 行可能还没落库，
    // 消息区显示「还没有消息」，故这里只钉弹窗本体 + 配置摘要）
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

    // 收尾一帧：状态列 + 弹窗 run 列表 + 最终回复默认展开（无需任何点击）
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_run4");
    const auto* status = ui4.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("成功") != std::string::npos);
    const auto* run0 = ui4.find("agent.dialog.detail.runlist.0");
    DOCTEST_REQUIRE_MESSAGE(run0 != nullptr, "详情弹窗缺 run 列表行");
    DOCTEST_CHECK(run0->text.find("成功") != std::string::npos);
    // 最终 assistant 文本块默认展开 = 回复立即可见（用户反馈②的正面钉子）
    const auto* finalBlk = ui4.find("agent.dialog.detail.msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "最终 assistant 文本块未默认展开（msg.1.blk.0 缺失）");
    DOCTEST_CHECK_MESSAGE(finalBlk->text.find("最终答复") != std::string::npos,
                          "最终回复内容未渲染，实测: " + finalBlk->text);

    mod.onStop();
    return true;
})

// ============ 8) 详情弹窗：配置摘要 / run 列表倒序 + 高亮 / 消息默认展开态 ============

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

    // 两次 run：先 limited（带 error/token），后 completed（最近一次）
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

    // ① 配置摘要（与编辑弹窗同源 getTask）
    const auto* info = ui2.find("agent.dialog.detail.info");
    DOCTEST_REQUIRE_MESSAGE(info != nullptr, "详情弹窗缺配置摘要 agent.dialog.detail.info");
    DOCTEST_CHECK(info->text.find("把逾期的待办全部完成") != std::string::npos);
    DOCTEST_CHECK(info->text.find("schedule") != std::string::npos);
    DOCTEST_CHECK(info->text.find("0 9 * * 1-5") != std::string::npos);
    DOCTEST_CHECK(info->text.find("sonnet") != std::string::npos);
    DOCTEST_CHECK(info->text.find("迭代上限：20") != std::string::npos);
    DOCTEST_CHECK(info->text.find("破坏性：允许") != std::string::npos);

    // ② run 列表：倒序（最近一次在前）+ 最近一次高亮（颜色与次新不同）
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

    // ③ 消息流：最终 assistant 文本块默认展开；其余（工具消息）默认折叠
    DOCTEST_REQUIRE(ui2.find("agent.dialog.detail.msgscroll") != nullptr);
    const auto* finalBlk = ui2.find("agent.dialog.detail.msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "最终 assistant 文本块未默认展开");
    DOCTEST_CHECK(finalBlk->text.find("最终答复内容") != std::string::npos);
    // 用户消息默认折叠（有预览、无块）
    const auto* userPreview = ui2.find("agent.dialog.detail.msg.0.preview");
    DOCTEST_REQUIRE_MESSAGE(userPreview != nullptr, "用户消息未走折叠态");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.msg.0.blk.0") == nullptr);
    // tool_result 消息默认折叠（点开才展开 → 见 case 9）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.msg.2.blk.0.out") == nullptr);

    mod.onStop();
    return true;
})

// ============ 9) 详情弹窗消息展开/收起 + 复制（保留 1.7.7 块级渲染） ============

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

    // 展开 tool_result 消息（seq2）→ 高度增大 + 输出全文 + 复制 chip
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

// ============ 10) 详情弹窗关闭不回写列表；重开回到最近一次 run ============

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
    // 默认选中最近一次（r2）
    DOCTEST_REQUIRE(ui2.find(base + ".msg.1.blk.0") != nullptr);
    // 显式点选旧 run（runlist.1 = limited）
    DOCTEST_REQUIRE(clickId(ui2, base + ".runlist.1"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_ro3");
    const auto* oldPreview = ui3.find(base + ".msg.0.preview");
    DOCTEST_REQUIRE_MESSAGE(oldPreview != nullptr, "选中旧 run 后消息区未切换");
    DOCTEST_CHECK(oldPreview->text.find("旧 run 消息") != std::string::npos);

    // 关弹窗（✕）：任务列表不受影响
    DOCTEST_REQUIRE(clickId(ui3, base + ".close"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_ro4");
    DOCTEST_CHECK(ui4.find(base + ".msgscroll") == nullptr);
    const auto* rowTitle = ui4.find("agent.task." + tid + ".title");
    DOCTEST_REQUIRE_MESSAGE(rowTitle != nullptr, "关弹窗后任务行消失（回写了列表？）");
    DOCTEST_CHECK_EQ(rowTitle->text, std::string("重开任务"));

    // 重开 → 回到最近一次 run（r2 的新回复默认展开）
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

// ============ 10b) run 列表 error 行不重叠（评审发现的真 bug：runsH 漏算 error 行的 gap）============

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
    // 关键断言：最后一个 error 行底 ≤ 消息标题顶（漏算 gap 时 error 行会压上去）
    DOCTEST_CHECK_MESSAGE(
        lastErr->frame.y + lastErr->frame.height <= msgsTitle->frame.y + 0.5f,
        "error 行压到消息标题：error 底 " +
            std::to_string(lastErr->frame.y + lastErr->frame.height) +
            " > 标题顶 " + std::to_string(msgsTitle->frame.y));
    // 且整段不越面板底
    DOCTEST_CHECK_MESSAGE(
        lastErr->frame.y + lastErr->frame.height <=
            panel->frame.y + panel->frame.height,
        "error 行溢出弹窗面板底");
    // 三条 error 行彼此不重叠
    const auto* err0 = ui2.find("agent.dialog.detail.runlist.0.err");
    const auto* err1 = ui2.find("agent.dialog.detail.runlist.1.err");
    DOCTEST_REQUIRE(err0 != nullptr && err1 != nullptr);
    DOCTEST_CHECK(err0->frame.y + err0->frame.height <= err1->frame.y + 0.5f);
    DOCTEST_CHECK(err1->frame.y + err1->frame.height <= lastErr->frame.y + 0.5f);

    mod.onStop();
    return true;
})

// ============ 10c) 编辑读库失败：可见反馈（不静默）============

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
    // 任务行仍在（陈旧缓存），点「+ 新建任务」成功后错误行清除
    DOCTEST_REQUIRE(clickId(ui2b, "agent.page.newTask.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_eerr3");
    DOCTEST_CHECK(ui3.find("agent.page.error") == nullptr);
    DOCTEST_CHECK(ui3.find("agent.dialog.task.goal") != nullptr);

    mod.onStop();
    return true;
})

// ============ 11) 可用工具区块（2026-10-02 用户反馈④，骨架重做后不变） ============

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
    DOCTEST_CHECK(ui.find("agent.tools.card.0") == nullptr);   // 收起态无卡片

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

// ============ 12) 弹窗几何：设计尺寸 + 内部高度预算 ============
// 评审结论（2026-10-02 批次 A 复审）：原「弹窗不超出视口」断言被 dialog 组件
// 自身的 clamp 兜住，零判别力 → 换成可失败的形态：把**我们自己的布局预算**
// 钉死（面板设计尺寸、每一行底 ≤ 按钮行顶、超长文本不挤位、下拉展开态全在
// 屏内），任一处回归即红。

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

    // ---- ① 面板设计尺寸（我们自己钉的值；被组件 clamp 兜住的「不超视口」不算判据）----
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

    // ---- ② 目标输入多行 + 行底 ≤ 按钮行顶（弹窗高被改小 / 行被加高即红）----
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
    // 超长目标不改变行位置、不丢字符（输入框内部滚动）
    DOCTEST_CHECK_EQ(inputText(ui2, "agent.dialog.task.goal").size(), longGoal.size());
    DOCTEST_CHECK_MESSAGE(nearPx(metaRow->frame.y, panel->frame.y + 224.0f),
                          "元信息行 y 应恒为面板顶 +224，实测 " +
                              std::to_string(metaRow->frame.y));

    // ---- ③ 下拉展开态（评审点：弹层不能既没修也没测）----
    // 触发方式弹层（3 项）向下覆盖元信息行：最后一项必须完整落在屏内，
    // 且覆盖是「不挤动布局」的（下方行 y 不变）
    const float metaYBefore = metaRow->frame.y;
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.dialog.task.trigger.field"),
                            "触发方式下拉字段命中面缺失");
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_geo2b");
    const auto* popup = ui2b.find("agent.dialog.task.trigger.popup");
    DOCTEST_REQUIRE_MESSAGE(popup != nullptr, "下拉弹层缺失");
    DOCTEST_CHECK(popup->frame.height > 0.0f);
    const auto* lastItem = ui2b.find("agent.dialog.task.trigger.item.2");
    DOCTEST_REQUIRE_MESSAGE(lastItem != nullptr, "下拉最后一项（常驻）缺失");
    DOCTEST_CHECK(lastItem->frame.width > 0.0f && lastItem->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        lastItem->frame.y >= -0.5f &&
            lastItem->frame.y + lastItem->frame.height <= kScreen.height + 0.5f,
        "下拉最后一项跑出屏外: y=" + std::to_string(lastItem->frame.y) +
            " 底=" + std::to_string(lastItem->frame.y + lastItem->frame.height));
    const auto* metaAfter = ui2b.find("agent.dialog.task.destructive.wrap");
    DOCTEST_REQUIRE(metaAfter != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(metaAfter->frame.y, metaYBefore),
                          "下拉展开挤动了下方行：展开前 " +
                              std::to_string(metaYBefore) + " 展开后 " +
                              std::to_string(metaAfter->frame.y));
    // 更深的弹层（模型槽位 = X 宏表 6 项，弹层跨过面板底）也不能掉出屏底
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2b, "agent.dialog.task.slot.field"),
                            "模型槽位下拉字段命中面缺失");
    eui::Ui ui2c;
    composeTo(ui2c, mod, kScreen, "agent_ui_geo2c");
    const auto* slotLast = ui2c.find("agent.dialog.task.slot.item.5");
    DOCTEST_REQUIRE_MESSAGE(slotLast != nullptr, "槽位下拉最后一项（子代理）缺失");
    DOCTEST_CHECK(slotLast->frame.width > 0.0f && slotLast->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        slotLast->frame.y >= -0.5f &&
            slotLast->frame.y + slotLast->frame.height <= kScreen.height + 0.5f,
        "槽位下拉最后一项掉出屏底: 底=" +
            std::to_string(slotLast->frame.y + slotLast->frame.height));

    // ---- ④ 超长任务名不挤掉行内动作（标题盒仍在动作 chip 左侧）----
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

// ============ 12b) 详情弹窗几何 + 空态占位垂直居中 ============

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

    // ---- 有消息：设计尺寸 + 消息区底 ≤ 面板底 ----
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

    // ---- 无消息：走 .empty 分支，占位文本垂直居中（B1 评审点名的漏扫）----
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

// ============ 13) 侧边栏出现「智能体」项 ============

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

// ============ 14) 常驻任务「入队」经 Scheduler 真派发 ============

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
