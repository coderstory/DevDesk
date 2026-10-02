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
    // F2：接口改按值返回快照（M2 静态审计），桩同口径
    std::vector<std::string> lastSyncWarnings() const override { return {}; }
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
              "agent_ui: 点任务行展开详情（detail + 徽章 + 120 字节预览钉子）", {
    test_support::TestDb tdb("agentui", "detail");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    // 种 1 条任务；goal 用 130 个 ASCII 字符 → 解码后纯文本 130 字节 > 120，
    // 让 120 字节截断路径真实发生（2026-10-02 run 日志改造：role 挪进独立
    // 徽章，预览不再带 role 前缀，80→120 字节）
    module::agent::AgentStore store(tdb.handle());
    const std::string longGoal(130, 'a');
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

    // msg.0 = 首条消息（seq 0 = user goal）。钉 2026-10-02 改造后的口径：
    //   徽章 .badge == 「用户」（role 前缀挪出预览）；预览 .preview = 解码
    //   文本前 120 字节 + …（UTF-8 3 字节）= 123 字节（截断上限或徽章丢失
    //   都会让这里变红）。长输出看不全的老问题在这里的镜像：预览之外还有
    //   .toggle chip 可展开。
    const auto* badge = ui3.find("agent.task." + tid + ".msg.0.badge");
    DOCTEST_REQUIRE(badge != nullptr);
    DOCTEST_CHECK_EQ(badge->text, std::string("用户"));
    const auto* msg0 = ui3.find("agent.task." + tid + ".msg.0.preview");
    DOCTEST_REQUIRE(msg0 != nullptr);
    DOCTEST_CHECK_MESSAGE(msg0->text.size() == 123,
                          "msg.0.preview 应为前 120 字节 + … = 123 字节，实测 " +
                              std::to_string(msg0->text.size()) + " 字节: " +
                              msg0->text);
    DOCTEST_CHECK(msg0->text.find_first_of('\n') == std::string::npos);
    // 展开/收起 chip 存在且可点（展开态切换在 expand/copy 专测里钉）
    const auto* toggle = ui3.find("agent.task." + tid + ".msg.0.toggle");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_CHECK(toggle->text == std::string("展开"));
    DOCTEST_REQUIRE(toggle->onClick != nullptr);

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

// ============ 7) run 详情完整展示（M2 Task 11） ============

REGISTER_TEST(agent_ui_run_detail_full_display,
              "agent_ui: 展开任务显示最近 5 次 run（状态/耗时/迭代/token/错误）并可选中查看消息", {
    test_support::TestDb tdb("agentui", "rundetail");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("详情", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());
    // 种 2 次 run：一次 limited 带 error + token，一次 completed（无 token）
    auto r1 = store.startRun(taskId.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r1.value(), 0, "user",
                                        R"([{"type":"text","text":"g"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "limited", 7, int64_t(190000),
                                    int64_t(15000),
                                    std::string("token 累计超上限 200K")).isOk());
    auto r2 = store.startRun(taskId.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_rundetail");
    const auto* rowEl = ui.find("agent.task." + tid);
    DOCTEST_REQUIRE(rowEl != nullptr);
    DOCTEST_REQUIRE(rowEl->onClick != nullptr);
    rowEl->onClick();
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_rundetail2");
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.task." + tid + ".runlist") != nullptr,
                            "展开详情缺 run 列表容器 agent.task.<id>.runlist");
    const auto* row0 = ui2.find("agent.task." + tid + ".runlist.0");
    DOCTEST_REQUIRE(row0 != nullptr);                        // 最近一次在前
    DOCTEST_CHECK_MESSAGE(row0->text.find("成功") != std::string::npos,
                          "runlist.0 应为最近一次 run（completed→成功），实测：" +
                              row0->text);
    DOCTEST_CHECK(row0->text.find("token —") != std::string::npos);   // NULL token
    const auto* row1 = ui2.find("agent.task." + tid + ".runlist.1");
    DOCTEST_REQUIRE(row1 != nullptr);
    DOCTEST_CHECK(row1->text.find("受限") != std::string::npos);      // limited 行
    DOCTEST_CHECK(row1->text.find("迭代 7") != std::string::npos);
    DOCTEST_CHECK_MESSAGE(row1->text.find("205000") != std::string::npos,
                          "token 合计未落进文案，实测：" + row1->text);  // 190000+15000
    const auto* errRow = ui2.find("agent.task." + tid + ".runlist.1.err");
    DOCTEST_REQUIRE(errRow != nullptr);
    DOCTEST_CHECK(errRow->text.find("200K") != std::string::npos);
    // 默认选中最近一次（r2，无消息）；点选 limited run → 消息区切到该 run。
    // 2026-10-02 改造：role 由徽章承担（msg.<i>.badge ==「用户」）
    row1->onClick();
    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_rundetail3");
    const auto* msg0 = ui3.find("agent.task." + tid + ".msg.0");
    DOCTEST_REQUIRE_MESSAGE(msg0 != nullptr, "选中 run 后消息区缺 agent.task.<id>.msg.0");
    const auto* badge0 = ui3.find("agent.task." + tid + ".msg.0.badge");
    DOCTEST_REQUIRE_MESSAGE(badge0 != nullptr, "消息缺角色徽章 msg.0.badge");
    DOCTEST_CHECK(badge0->text == std::string("用户"));
    mod.onStop();
    return true;
})

// ============ 9) run 日志块级渲染：展开全文 + 工具调用展示（2026-10-02）============

REGISTER_TEST(agent_ui_message_expand_and_copy,
              "agent_ui: 消息展开→块级全文（tool_use chip + tool_result 输出）+ 高度增大 + 复制 chip", {
    test_support::TestDb tdb("agentui", "expand");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("日志任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    // 种 1 次 run + 3 条消息（agent_loop 落库形态原文）：
    //   seq0 user      = goal blocks
    //   seq1 assistant = text + tool_use 混合块（工具调用可见性钉子）
    //   seq2 user      = tool_result（长输出全文可见性钉子，600 字节）
    auto r = store.startRun(taskId.value());
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
    const eui::Screen screen{1600.0f, 1000.0f};

    // 展开任务：收起态基线
    eui::Ui ui1;
    composeTo(ui1, mod, screen, "agent_ui_expand1");
    const auto* rowEl = ui1.find("agent.task." + tid);
    DOCTEST_REQUIRE(rowEl != nullptr);
    DOCTEST_REQUIRE(rowEl->onClick != nullptr);
    rowEl->onClick();
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_expand2");
    DOCTEST_REQUIRE(ui2.find("agent.task." + tid + ".msgscroll") != nullptr);
    const auto* m2c = ui2.find("agent.task." + tid + ".msg.2");
    DOCTEST_REQUIRE(m2c != nullptr);
    const float collapsedH = m2c->frame.height;
    DOCTEST_CHECK(collapsedH > 0.0f);
    // 收起态不渲染块内容
    DOCTEST_CHECK(ui2.find("agent.task." + tid + ".msg.1.blk.1") == nullptr);

    // 点 msg.1 的展开 chip（actionChip 契约：id 元素带 onClick）
    const auto* tgl1 = ui2.find("agent.task." + tid + ".msg.1.toggle");
    DOCTEST_REQUIRE(tgl1 != nullptr);
    DOCTEST_REQUIRE(tgl1->onClick != nullptr);
    tgl1->onClick();
    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_expand3");
    // assistant 混合块展开：text 块 + tool_use chip（🔧 + 工具名 + 参数）
    const auto* chip = ui3.find("agent.task." + tid + ".msg.1.blk.1");
    DOCTEST_REQUIRE_MESSAGE(chip != nullptr, "tool_use 块缺失 msg.1.blk.1");
    DOCTEST_CHECK_MESSAGE(chip->text.find("todo.list") != std::string::npos &&
                              chip->text.find("🔧") != std::string::npos,
                          "tool chip 缺工具名/emoji，实测: " + chip->text);

    // 点 msg.2 的展开 chip → tool_result 输出全文渲染 + 高度增大 + 复制 chip
    const auto* tgl2 = ui3.find("agent.task." + tid + ".msg.2.toggle");
    DOCTEST_REQUIRE(tgl2 != nullptr);
    DOCTEST_REQUIRE(tgl2->onClick != nullptr);
    tgl2->onClick();
    eui::Ui ui4;
    composeTo(ui4, mod, screen, "agent_ui_expand4");
    const auto* m2e = ui4.find("agent.task." + tid + ".msg.2");
    DOCTEST_REQUIRE(m2e != nullptr);
    DOCTEST_CHECK_MESSAGE(m2e->frame.height > collapsedH,
                          "展开高度应大于收起高 " +
                              std::to_string(collapsedH) + "，实测 " +
                              std::to_string(m2e->frame.height));
    const auto* out = ui4.find("agent.task." + tid + ".msg.2.blk.0.out");
    DOCTEST_REQUIRE(out != nullptr);
    DOCTEST_CHECK_MESSAGE(out->text.find("zzz") != std::string::npos &&
                              out->text.size() >= 600,
                          "tool_result 输出全文未渲染，实测 " +
                              std::to_string(out->text.size()) + " 字节");
    // 复制 chip 存在且可点（剪贴板写副作用 headless 不可断，可调用 + 不崩即达）
    const auto* copyBtn = ui4.find("agent.task." + tid + ".msg.2.copy");
    DOCTEST_REQUIRE(copyBtn != nullptr);
    DOCTEST_CHECK_EQ(copyBtn->text, std::string("复制"));
    DOCTEST_REQUIRE(copyBtn->onClick != nullptr);
    copyBtn->onClick();

    // 再点收起 → 恢复收起态（toggle 双向）
    const auto* tgl2b = ui4.find("agent.task." + tid + ".msg.2.toggle");
    DOCTEST_REQUIRE(tgl2b != nullptr && tgl2b->onClick != nullptr);
    tgl2b->onClick();
    eui::Ui ui5;
    composeTo(ui5, mod, screen, "agent_ui_expand5");
    DOCTEST_CHECK(ui5.find("agent.task." + tid + ".msg.2.blk.0.out") == nullptr);

    mod.onStop();
    return true;
})

// ============ 10) 可用工具区块（2026-10-02 用户反馈④）============

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

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_tools1");
    const auto* header = ui.find("agent.tools.header");
    DOCTEST_REQUIRE(header != nullptr);
    DOCTEST_REQUIRE(header->onClick != nullptr);
    const auto* label = ui.find("agent.tools.header.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_MESSAGE(
        label->text.find("可用工具") != std::string::npos &&
            label->text.find(std::to_string(registered)) != std::string::npos,
        "头部缺「可用工具（N）」计数，实测: " + label->text);
    // 收起态：无卡片
    DOCTEST_CHECK(ui.find("agent.tools.card.0") == nullptr);

    header->onClick();
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_tools2");
    // 展开态：卡片数 == 注册表工具数，名称/参数渲染
    size_t cards = 0;
    for (size_t k = 0; k < registered + 4; ++k) {
        if (ui2.find("agent.tools.card." + std::to_string(k)) != nullptr) {
            ++cards;
        }
    }
    DOCTEST_CHECK_MESSAGE(cards == registered,
                          "工具卡数 " + std::to_string(cards) +
                              " != 注册表数 " + std::to_string(registered));
    const auto* card0 = ui2.find("agent.tools.card.0");
    const auto* name0 = ui2.find("agent.tools.card.0.name");
    DOCTEST_REQUIRE(name0 != nullptr);
    DOCTEST_CHECK_EQ(name0->text, std::string("app.stats"));
    const auto* params0 = ui2.find("agent.tools.card.0.params");
    DOCTEST_REQUIRE(params0 != nullptr);
    DOCTEST_CHECK(params0->text.find("参数：") != std::string::npos);
    (void)card0;

    // 再点收起
    const auto* header2 = ui2.find("agent.tools.header");
    DOCTEST_REQUIRE(header2 != nullptr && header2->onClick != nullptr);
    header2->onClick();
    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_tools3");
    DOCTEST_CHECK(ui3.find("agent.tools.card.0") == nullptr);

    mod.onStop();
    return true;
})

// ============ 11) 多行目标输入（2026-10-02 用户反馈③）============

REGISTER_TEST(agent_ui_goal_input_multiline,
              "agent_ui: 新建/编辑表单 goal 输入为多行高（≥90px），编辑表单整体仍在页内", {
    test_support::TestDb tdb("agentui", "multiline");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("行高任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const eui::Screen screen{1600.0f, 1000.0f};

    // 新建表单：goal 输入高度 ≥ 90（单行时代 40，2026-10-02 改 96）
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_ml1");
    const auto* goal = ui.find("agent.form.goal");
    DOCTEST_REQUIRE(goal != nullptr);
    DOCTEST_CHECK_MESSAGE(goal->frame.height >= 90.0f,
                          "新建表单 goal 输入应 ≥90px，实测 " +
                              std::to_string(goal->frame.height));
    const auto* label = ui.find("agent.form.goal.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("任务目标"));

    // 编辑表单：goal 同款多行；整个表单仍在页面内容区（kEditFormH 变 300
    // 后 listH 公式同步，末尾任务行不得溢出页底）
    const auto* rowEl = ui.find("agent.task." + tid);
    DOCTEST_REQUIRE(rowEl != nullptr);
    const auto* editBtn = ui.find("agent.task." + tid + ".edit");
    DOCTEST_REQUIRE(editBtn != nullptr);
    DOCTEST_REQUIRE(editBtn->onClick != nullptr);
    editBtn->onClick();
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_ml2");
    const auto* editGoal = ui2.find("agent.edit.goal");
    DOCTEST_REQUIRE(editGoal != nullptr);
    DOCTEST_CHECK_MESSAGE(editGoal->frame.height >= 90.0f,
                          "编辑表单 goal 输入应 ≥90px，实测 " +
                              std::to_string(editGoal->frame.height));
    const auto* page = ui2.find("agent.page");
    const auto* form = ui2.find("agent.edit.form");
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_REQUIRE(form != nullptr);
    DOCTEST_CHECK_MESSAGE(form->frame.y + form->frame.height <=
                              page->frame.y + page->frame.height + 0.5f,
                          "编辑表单溢出页底：form 底 " +
                              std::to_string(form->frame.y + form->frame.height) +
                              " > page 底 " +
                              std::to_string(page->frame.y + page->frame.height));

    mod.onStop();
    return true;
})
