// tests/test_modules/test_agent_ui.cpp
// 智能体页 UI 测试（agent M1 Task 13）：无头几何断言 + 元素 id 契约钉子。
//
// 钉的是 Task 12 compose 的元素 id 契约表（agent_module.cpp 头注释逐字）：
//   agent.page / agent.title / agent.form.goal / agent.form.create /
//   agent.tasklist / agent.task.<taskId> / agent.task.<taskId>.title /
//   agent.task.<taskId>.run（text=="启动"）/ agent.task.<taskId>.status /
//   agent.task.<taskId>.detail（展开才存在）/ agent.task.<taskId>.msg.<i>
// id 改名、结构删除、文案改动即红。
//
// 无头口径（probe-recipe §5 + 既有先例）：
//   - 裸 eui::Ui + ui.begin/mod.compose/ui.end + ui.layout（frame 靠 layout
//     填充，test_ui_modern.cpp:1108-1117 同款）；
//   - 按钮/行点击 = 直调元素上真实挂的回调：button 的 onClick 挂在
//     <id>.bg（components/button.h:145-157），T12 cancel 用例同款直调；
//   - 文本输入 = 对 <id>.hit 直调 onTextInput（test_markdown.cpp:4439-4446
//     同款，走真实 input 事件路径 → onChange → goalInput_）；
//   - msg 预览钉 T12 实现口径：role + ": " + contentJson 前 80 **字节**
//     （UTF-8 边界回退 + 省略号），case 4 用 60 字符 ASCII goal 精确钉
//     89 = len("user: ") + 80 + len("…")。
//
// probe 侧（几何 + 真命中分发）在 tests/probes/agent_probes.cpp，两层互补。
#include "doctest.h"
#include "test_support.h"

#include "app_sidebar.h"
#include "core/claude_proxy_service.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "platform/http_client.h"

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

// 测试用 fake 代理服务（与 test_agent_tools.cpp / test_llm_client.cpp 同款：
// 实现 IClaudeProxyService 全部虚方法；本组用例走 isProxyRunning /
// proxyPort / resolveModelSlot 三条链路）
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
    const std::vector<std::string>& lastSyncWarnings() const override {
        static const std::vector<std::string> w;
        return w;
    }
    std::string settingsFilePath() const override { return {}; }
    std::optional<std::string> resolveModelSlot(const std::string&) override {
        return std::string("claude-sonnet-5-5");
    }
};

// RAII：用例退出（含 fail-fast 提前 return 路径）时把 app.stats 从进程级
// 注册表摘除（ToolRegistry 不在 resetCaseGlobals 清单里，test_agent_tools
// 同款兜底）
struct ToolGuard {
    std::string name;
    ~ToolGuard() { core::ToolRegistry::instance().unregisterTool(name); }
};

// 裸 Ui compose 一帧 + 显式 layout（元素 frame 需显式布局，
// test_ui_modern.cpp:1108-1117 同款）
void composeTo(eui::Ui& ui, module::agent::AgentModule& mod,
               const eui::Screen& screen, const char* pageId) {
    ui.begin(pageId);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

// 按 UTF-8 字符边界逐字符送进 input 的 onTextInput（真实 input 事件路径：
// onTextInput → 内置编辑 → onChange；helper 不放断言宏——宏展开含
// return false，只退出 helper 假装绿，调用方先断言元素非空）
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

// 触发类型下拉切换：直调 item 上真实挂的 onChange 回调（components/dropdown.h
// 的 items[i].onClick——与按钮/行「直调真实回调」同款口径），不模拟 EUI-NEO 下拉
// 的展开与指针事件机制。返回 false = 该项不存在（调用方 REQUIRE 后转红）
bool callDropdownChange(eui::Ui& ui, const std::string& ddId, int idx) {
    const auto* item = ui.find(ddId + ".item." + std::to_string(idx));
    if (item == nullptr || item->onClick == nullptr) return false;
    item->onClick();
    return true;
}

// cron 草稿写入：走模块测试缝（等价于用户改写输入框文本）。不用 input 的
// onTextInput 真事件路径：它只能在光标处**追加**，无法覆盖已有草稿；且
// test_main 每用例重置 ServiceLocator/全局单例，不重置 Ui 状态。
void setCronInput(module::agent::AgentModule& mod, const std::string& cron) {
    mod.setEditCronDraft(cron);
}

bool nearPx(float a, float b) { return std::fabs(a - b) <= 0.5f; }

} // namespace

// ============ 1) 空库 compose 骨架 ============

REGISTER_TEST(agent_ui_compose_skeleton,
              "agent_ui: 空库 compose 骨架（page 铺满 1600 / title / 表单 / 无任务行）", {
    test_support::TestDb tdb("agentui", "skeleton");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");  // 起点干净

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_skeleton");

    // 根 column：宽 = 逻辑宽 1600（几何钉子，防根容器宽度改坏）
    const auto* page = ui.find("agent.page");
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(page->frame.width, 1600.0f),
                          "agent.page.frame.width 实测 " +
                              std::to_string(page->frame.width));
    // 标题：存在 + 文案逐字（文案改动即红）
    const auto* title = ui.find("agent.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("智能体"));
    // 新建表单：goal 输入 + 提交按钮
    DOCTEST_CHECK(ui.find("agent.form.goal") != nullptr);
    DOCTEST_CHECK(ui.find("agent.form.create") != nullptr);
    DOCTEST_CHECK(ui.find("agent.tasklist") != nullptr);
    // 空库无任务行（id=1 是 fresh 库首个任务 id）
    DOCTEST_CHECK(ui.find("agent.task.1") == nullptr);

    mod.onStop();
    return true;
})

// ============ 2) 表单新建任务 ============

REGISTER_TEST(agent_ui_create_task_via_form,
              "agent_ui: goal 输入 + 点「新建任务」→ 落库恰 1 条 + 行标题渲染", {
    test_support::TestDb tdb("agentui", "create");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_create");

    // 逐字符输入 goal（真实 input 事件路径，见 typeInto 注释）
    auto* goalHit = ui.find("agent.form.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    DOCTEST_REQUIRE(goalHit->onTextInput != nullptr);
    typeInto(goalHit, "测试目标");

    // 点「新建任务」：button 契约 = onClick 挂在 <id>.bg
    //（components/button.h:145-157），T12 cancel 用例同款直调
    const auto* createBtn = ui.find("agent.form.create.bg");
    DOCTEST_REQUIRE_MESSAGE(createBtn != nullptr,
                            "agent.form.create.bg 未渲染（button 结构变了？）");
    DOCTEST_REQUIRE(createBtn->onClick != nullptr);
    createBtn->onClick();

    // 落库恰 1 条且 goal 逐字一致（经业务路径 createTaskFromForm → store）
    module::agent::AgentStore store(tdb.handle());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(tasks.value()[0].goal, std::string("测试目标"));
    const int64_t newId = tasks.value()[0].id;

    // re-compose：任务行出现且 title = goal 派生（12 字节 ≤ 24 → 逐字相等）
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_create2");
    const auto* rowTitle =
        ui2.find("agent.task." + std::to_string(newId) + ".title");
    DOCTEST_REQUIRE(rowTitle != nullptr);
    DOCTEST_CHECK_EQ(rowTitle->text, std::string("测试目标"));

    mod.onStop();
    return true;
})

// ============ 3) 「启动」触发真实 run ============

REGISTER_TEST(agent_ui_run_button_triggers_loop,
              "agent_ui: 点「启动」跑真实 loop（end_turn → 落库 completed + 状态列「成功」）", {
    test_support::TestDb tdb("agentui", "run");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    // 种 1 条任务（直接经 store，与 UI 无关）
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("端到端任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    // Deps 注入：脚本化 HTTP 恒回 end_turn（零工具调用 → loop 一轮即
    // completed），零真实网络（test_agent_tools cancel 用例同款缝）
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

    // compose → 任务行出现 → 点「启动」（手动触发路径 = compose onClick 回调）
    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_run");
    const auto* runBtn = ui.find("agent.task." + tid + ".run.bg");
    DOCTEST_REQUIRE_MESSAGE(runBtn != nullptr, "启动按钮未渲染（任务列表空？）");
    DOCTEST_REQUIRE(runBtn->onClick != nullptr);
    runBtn->onClick();

    // 有界轮询（10ms 步进 ≤5s；禁长 sleep）等 run 线程落库 completed
    bool completed = false;
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId, 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "run 线程 5s 内未落库 completed");

    // re-compose：状态列文本含「成功」（completed → 成功 的映射钉子）
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_run2");
    const auto* status = ui2.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("成功") != std::string::npos);

    mod.onStop();
    return true;
})

// ============ 4) 任务行展开详情（接 3 的 run 数据） ============

REGISTER_TEST(agent_ui_run_detail_expands,
              "agent_ui: 点任务行展开详情（detail + msg.0 role 前缀 + 80 字节截断钉子）", {
    test_support::TestDb tdb("agentui", "detail");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    // 种 1 条任务；goal 用 60 个 ASCII 字符 → msg.0 的 contentJson =
    // `[{"type":"text","text":"aaa…"}]` = 23 + 62 + 2 = 87 字节 > 80，
    // 让 80 字节截断路径真实发生（89 的算式见下方断言注释）
    module::agent::AgentStore store(tdb.handle());
    const std::string longGoal(60, 'a');
    auto created = store.createTask("详情任务", longGoal, "manual", "default",
                                    10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

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

    // 跑一次 run（同 case 3 路径），让详情有真实消息可读
    {
        const eui::Screen screen{1600.0f, 1000.0f};
        eui::Ui ui;
        composeTo(ui, mod, screen, "agent_ui_detail");
        const auto* runBtn = ui.find("agent.task." + tid + ".run.bg");
        DOCTEST_REQUIRE(runBtn != nullptr);
        DOCTEST_REQUIRE(runBtn->onClick != nullptr);
        runBtn->onClick();
        bool completed = false;
        for (int i = 0; i < 500; ++i) {
            auto runs = store.listRuns(taskId, 1);
            if (runs.isOk() && !runs.value().empty() &&
                runs.value().front().status == "completed") {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        DOCTEST_REQUIRE_MESSAGE(completed, "run 线程 5s 内未落库 completed");
    }

    // 点任务行本体（onClick 挂在行元素上，T12 契约用例同款）→ 展开
    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_detail2");
    const auto* row = ui2.find("agent.task." + tid);
    DOCTEST_REQUIRE(row != nullptr);
    DOCTEST_REQUIRE(row->onClick != nullptr);
    row->onClick();

    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_detail3");
    DOCTEST_CHECK(ui3.find("agent.task." + tid + ".detail") != nullptr);

    // msg.0 = 首条消息（seq 0 = user goal）。钉 T12 实现口径：
    // text = role + ": " + contentJson 前 80 字节（UTF-8 边界回退）+ "…"。
    // 本例 contentJson 87 字节 → 截 80（切点落在 ASCII 区，无边界回退）→
    // 总长 = len("user: ") 6 + 80 + len("…") 3 = 89 字节（精确算术钉）。
    // 截断上限改动（80→N）或 role 前缀丢失都会让这里变红。
    const auto* msg0 = ui3.find("agent.task." + tid + ".msg.0");
    DOCTEST_REQUIRE(msg0 != nullptr);
    DOCTEST_CHECK_MESSAGE(msg0->text.rfind("user: ", 0) == 0,
                          "msg.0 缺 role 前缀，实测: " + msg0->text);
    DOCTEST_CHECK_MESSAGE(msg0->text.size() == 89,
                          "msg.0 应为 user: + 前 80 字节 + … = 89 字节，实测 " +
                              std::to_string(msg0->text.size()) + " 字节: " +
                              msg0->text);

    mod.onStop();
    return true;
})

// ============ 5) 侧边栏出现「智能体」项 ============

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

    // 真实 app::compose + 桩 Router（test_app_sidebar.cpp 模式）：
    // Router 注册 AgentModule 实体，侧边栏按 Router 注册表渲染导航项
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

    // 侧边栏项 id = app.sidebar.item.<pageIdToString(page)>（app_sidebar.cpp:188）
    const auto* label = ui.find("app.sidebar.item.Agent.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("智能体"));
    DOCTEST_CHECK(ui.find("app.sidebar.item.Agent.hit") != nullptr);
    // 导航目标已注册且页面真被 compose 出来（Router 当前页 = Agent）
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::Agent);
    DOCTEST_CHECK(ui.find("agent.title") != nullptr);

    mod.onStop();
    return true;
})

// ============ 6) 编辑表单 + cron 保存期校验 + 启用/禁用 + 常驻入队（M2 Task 10） ============

REGISTER_TEST(agent_ui_edit_form_and_cron_validation,
              "agent_ui: 编辑表单打开回填、非法 cron 拒存、合法 cron 落库、禁用切换、常驻入队按钮", {
    test_support::TestDb tdb("agentui", "edit");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("旧任务", "旧目标", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    const eui::Screen screen{1600.0f, 1000.0f};
    auto composeOnce = [&](eui::Ui& ui) { composeTo(ui, mod, screen, "agent_ui_edit"); };
    eui::Ui ui;
    composeOnce(ui);
    const auto* edit = ui.find("agent.task." + tid + ".edit");
    DOCTEST_REQUIRE_MESSAGE(edit != nullptr, "任务行缺 agent.task.<id>.edit 元素");
    DOCTEST_REQUIRE(edit->onClick != nullptr);
    edit->onClick();
    eui::Ui ui2;
    composeOnce(ui2);
    DOCTEST_CHECK(ui2.find("agent.edit.form") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.goal") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.trigger") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.slot") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.iter") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.destructive") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.cron") == nullptr);   // manual 不渲染 cron 输入

    // 切触发类型到「定时」（下拉 onChange 回调直调）→ cron 输入出现；非法表达式拒存
    DOCTEST_REQUIRE_MESSAGE(callDropdownChange(ui2, "agent.edit.trigger", 1),
                            "agent.edit.trigger.item.1 不存在（下拉未渲染？）");
    eui::Ui ui3;
    composeOnce(ui3);
    DOCTEST_CHECK(ui3.find("agent.edit.cron") != nullptr);
    setCronInput(mod, "0 9 * * 1-5x");
    const auto* save = ui3.find("agent.edit.save");
    DOCTEST_REQUIRE(save != nullptr);
    DOCTEST_REQUIRE(save->onClick != nullptr);
    save->onClick();
    eui::Ui ui4;
    composeOnce(ui4);
    const auto* err = ui4.find("agent.edit.error");
    DOCTEST_REQUIRE_MESSAGE(err != nullptr, "非法 cron 未在表单内报错（agent.edit.error 缺失）");
    DOCTEST_CHECK(err->text.find("cron") != std::string::npos);
    auto untouched = store.getTask(taskId.value());
    DOCTEST_REQUIRE(untouched.isOk());
    DOCTEST_CHECK_EQ(untouched.value().goal, std::string("旧目标"));   // 未落库
    DOCTEST_CHECK_EQ(untouched.value().triggerType, std::string("manual"));

    // 合法 cron → 落库 + 退编辑态
    setCronInput(mod, "0 9 * * 1-5");
    const auto* save2 = ui4.find("agent.edit.save");
    DOCTEST_REQUIRE(save2 != nullptr);
    save2->onClick();
    auto updated = store.getTask(taskId.value());
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig,
                     std::string(R"({"cron":"0 9 * * 1-5"})"));
    eui::Ui ui4b;
    composeOnce(ui4b);
    DOCTEST_CHECK(ui4b.find("agent.edit.form") == nullptr);   // 已退编辑态

    // 启用/禁用切换：文案按 enabled，点击取反
    eui::Ui ui5;
    composeOnce(ui5);
    const auto* toggle = ui5.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    DOCTEST_CHECK_EQ(toggle->text, std::string("禁用"));
    toggle->onClick();
    auto disabled = store.getTask(taskId.value());
    DOCTEST_REQUIRE(disabled.isOk());
    DOCTEST_CHECK_FALSE(disabled.value().enabled);
    eui::Ui ui6;
    composeOnce(ui6);
    const auto* toggle2 = ui6.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle2 != nullptr);
    DOCTEST_CHECK_EQ(toggle2->text, std::string("启用"));   // 文案随 enabled 翻转

    // 常驻任务才有入队按钮（本条是 schedule 任务）
    DOCTEST_CHECK(ui5.find("agent.task." + tid + ".enqueue") == nullptr);

    // 只读任务信息行（Step 3b）：展开任务 → info 逐项来自同一份 AgentTaskRow
    const auto* rowEl = ui5.find("agent.task." + tid);
    DOCTEST_REQUIRE(rowEl != nullptr);
    DOCTEST_REQUIRE(rowEl->onClick != nullptr);
    rowEl->onClick();
    eui::Ui ui7;
    composeOnce(ui7);
    const auto* info = ui7.find("agent.task." + tid + ".info");
    DOCTEST_REQUIRE_MESSAGE(info != nullptr, "展开详情缺只读信息行 agent.task.<id>.info");
    DOCTEST_CHECK(info->text.find("旧目标") != std::string::npos);
    DOCTEST_CHECK(info->text.find("0 9 * * 1-5") != std::string::npos);   // cron 原文
    DOCTEST_CHECK(info->text.find("default") != std::string::npos);       // 模型槽位
    DOCTEST_CHECK(info->text.find("迭代上限：10") != std::string::npos);
    DOCTEST_CHECK(info->text.find("破坏性：禁止") != std::string::npos);
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

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_enqueue");
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
