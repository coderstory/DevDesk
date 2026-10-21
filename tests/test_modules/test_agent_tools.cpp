// tests/test_modules/test_agent_tools.cpp
// app.stats 只读统计 + ToolRegistry 行为 + AgentModule 生命周期（onStart/onStop、teardown
// 顺序、UI 元素 id 契约、惰性装配、Scheduler 收口）。
// 跨文件前提：
//   - 工具级失败一律 Result ok + isError=true + 中文说明；Result::fail 只留给编程不变量破坏。
//   - ToolRegistry 是进程级单例，不在 test_main 的 resetCaseGlobals 清单里，只能靠用例内
//     ToolGuard 兜底摘除（显式清理会漏 fail-fast 提前 return 的路径）。
//   - AgentModule::onStop 的收口顺序固定：全取消 → 全 join → 停 Scheduler → 清容器 →
//     unregisterTool。把 unregister 提到 join 之前会被 teardown 顺序探针记成 false。
//   - tests/doctest.h 是极简框架：DOCTEST_CHECK 失败展开为 `return false`，进 lambda 只会
//     退出 lambda（用例照样绿），断言必须平铺。
#include "doctest.h"
#include "test_support.h"

#include "core/service_locator.h"
#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "module_agent/app_stats_tool.h"
#include "platform/http_client.h"
#include "platform/json.h"

#include <eui/dsl.h>

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

REGISTER_TEST(app_stats_counts_seeded_rows,
              "app.stats: 统计 todo_tasks/markdown_index/agent_tasks 三表行数", {
    test_support::TestDb tdb("agenttools", "stats");

    // todo_tasks 的 column_id=1 靠 todo_columns v2 迁移种子存在，不是本用例建的行；
    // 其余必填列（created_at 等）给占位值。
    DOCTEST_REQUIRE(
        tdb.handle()
            .exec("INSERT INTO todo_tasks (column_id, title, position, "
                  "created_at, updated_at) VALUES "
                  "(1, 't1', 0, 1, 1), (1, 't2', 1, 1, 1)")
            .isOk());
    DOCTEST_REQUIRE(
        tdb.handle()
            .exec("INSERT INTO markdown_index (path, title, size_bytes, "
                  "modified_at, created_at) VALUES "
                  "('a.md', 'A', 10, 1, 1)")
            .isOk());
    DOCTEST_REQUIRE(
        tdb.handle()
            .exec("INSERT INTO agent_tasks (name, goal, trigger_type, "
                  "created_at, updated_at) VALUES "
                  "('整理逾期', '把逾期 todo 全部完成', 'manual', 1, 1)")
            .isOk());

    auto tool = module::agent::makeAppStatsTool(tdb.handle());
    DOCTEST_REQUIRE(tool != nullptr);
    DOCTEST_CHECK_EQ(tool->name(), std::string("app.stats"));

    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("todos").asInt(), 2);
    DOCTEST_CHECK_EQ(obj.get("notes").asInt(), 1);
    DOCTEST_CHECK_EQ(obj.get("agentTasks").asInt(), 1);
    return true;
})

REGISTER_TEST(app_stats_zero_state_and_schema,
              "app.stats: 空库全 0、schema 可读、只读非破坏性", {
    test_support::TestDb tdb("agenttools", "zero");

    auto tool = module::agent::makeAppStatsTool(tdb.handle());
    auto r = tool->execute(core::ToolArgs{""});  // 空参数也要能跑
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("todos").asInt(), 0);
    DOCTEST_CHECK_EQ(obj.get("notes").asInt(), 0);
    DOCTEST_CHECK_EQ(obj.get("agentTasks").asInt(), 0);

    const std::string schema = tool->schemaJson();
    DOCTEST_CHECK(schema.find("\"name\":\"app.stats\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("input_schema") != std::string::npos);
    DOCTEST_CHECK_FALSE(tool->isDestructive());
    return true;
})

namespace {

// ToolGuard 不可省：ToolRegistry 是进程级单例、不在 test_main 的 resetCaseGlobals
// 清单里，用例内显式 unregister 会漏掉 fail-fast 提前 return 的路径。
struct ToolGuard {
    std::vector<std::string> names;
    ToolGuard(std::initializer_list<std::string> toolNames) : names(toolNames) {}
    ~ToolGuard() {
        for (const auto& name : names) {
            core::ToolRegistry::instance().unregisterTool(name);
        }
    }
};

// 慢工具：execute 睡 300ms（本组用例唯一允许的长睡——模拟真实工具耗时），
// 醒来后记录那一刻 app.stats / app.notify 是否仍在注册表。teardown 顺序探针：
// 正确顺序（cancel → join → clear → unregister）下，工具执行期间 onStop 还阻塞在
// join，两个自带工具必然尚未注销；若实现把 unregister 提到 join 前，本探针记到 false。
class SlowTool : public core::ITool {
public:
    std::string name() const override { return "test.slow"; }
    std::string schemaJson() const override { return R"({"name":"test.slow"})"; }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        sawAppStatsRegistered =
            core::ToolRegistry::instance().find("app.stats").isOk();
        sawAppNotifyRegistered =
            core::ToolRegistry::instance().find("app.notify").isOk();
        endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return core::Result<core::ToolResult>::ok(
            core::ToolResult{false, R"("ok")"});
    }
    std::atomic<int> calls{0};
    std::atomic<bool> sawAppStatsRegistered{false};
    std::atomic<bool> sawAppNotifyRegistered{false};
    std::atomic<long long> endMs{0};  // execute 返回时刻（steady_clock 自 epoch 毫秒）
};

} // namespace

REGISTER_TEST(tool_registry_list_tools_snapshot,
              "tool_registry: listTools 只读枚举（map 序快照，keep-alive）", {
    core::ToolRegistry& reg = core::ToolRegistry::instance();

    class NamedTool : public core::ITool {
    public:
        explicit NamedTool(std::string n) : n_(std::move(n)) {}
        std::string name() const override { return n_; }
        std::string schemaJson() const override {
            return R"({"name":")" + n_ + R"("})";
        }
        core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
            return core::Result<core::ToolResult>::ok(
                core::ToolResult{false, R"("ok")"});
        }
    private:
        std::string n_;
    };

    core::ToolRegistry::instance().unregisterTool("test.zeta");
    core::ToolRegistry::instance().unregisterTool("test.alpha");
    auto a = std::make_shared<NamedTool>("test.alpha");
    auto z = std::make_shared<NamedTool>("test.zeta");
    DOCTEST_REQUIRE(reg.registerTool(a).isOk());
    DOCTEST_REQUIRE(reg.registerTool(z).isOk());
    ToolGuard g1{"test.alpha"};
    ToolGuard g2{"test.zeta"};

    auto tools = reg.listTools();
    bool sawAlpha = false, sawZeta = false;
    int alphaIdx = -1, zetaIdx = -1;
    for (size_t i = 0; i < tools.size(); ++i) {
        if (tools[i]->name() == "test.alpha") { sawAlpha = true; alphaIdx = static_cast<int>(i); }
        if (tools[i]->name() == "test.zeta") { sawZeta = true; zetaIdx = static_cast<int>(i); }
    }
    DOCTEST_CHECK(sawAlpha);
    DOCTEST_CHECK(sawZeta);
    DOCTEST_CHECK_MESSAGE(alphaIdx < zetaIdx,
                          "listTools 应按 map 序（name 字典序）alpha 在 zeta 前");

    // listTools 返回 shared_ptr 快照：摘除注册表条目后快照里的工具仍可用
    reg.unregisterTool("test.alpha");
    bool stillUsable = false;
    for (const auto& t : tools) {
        if (t->name() == "test.alpha") stillUsable = true;
    }
    DOCTEST_CHECK(stillUsable);
    return true;
})

REGISTER_TEST(agent_module_registers_own_tool,
              "agent_module: onStart 注册 app.stats/app.notify、onStop 注销（生命周期）", {
    test_support::TestDb tdb("agenttools", "modtool");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};   // 兜底摘除（含 fail-fast 路径）
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");  // 起点干净
    core::ToolRegistry::instance().unregisterTool("hot.list");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));

    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isFail());
    mod->onStart();
    auto found = core::ToolRegistry::instance().find("app.stats");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK_EQ(found.value()->name(), std::string("app.stats"));
    DOCTEST_CHECK_FALSE(found.value()->isDestructive());
    auto notify = core::ToolRegistry::instance().find("app.notify");
    DOCTEST_REQUIRE(notify.isOk());
    DOCTEST_CHECK_EQ(notify.value()->name(), std::string("app.notify"));
    DOCTEST_CHECK_FALSE(notify.value()->isDestructive());

    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isFail());
    return true;
})

REGISTER_TEST(agent_module_onstop_cancels_and_joins,
              "agent_module: onStop 取消+join 完成后才注销工具，run 落库 cancelled", {
    test_support::TestDb tdb("agenttools", "modstop");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");  // 起点干净
    core::ToolRegistry::instance().unregisterTool("hot.list");

    // 慢工具挂进注册表（mock 响应点名调用它；RAII 摘除）
    auto slow = std::make_shared<SlowTool>();
    SlowTool* tool = slow.get();
    DOCTEST_REQUIRE(core::ToolRegistry::instance().registerTool(slow).isOk());
    ToolGuard slowGuard{"test.slow"};

    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("慢工具任务", "调用 test.slow 一次",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();

    // Deps 注入：fake 配置来源 + 脚本化 HTTP。首轮吐 tool_use(test.slow)，次轮 end_turn；
    // teardown 正常时次轮到不了——cancel 在首轮工具执行期间已置位。别当死代码删。
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    int httpCalls = 0;
    deps.http = [&httpCalls](const platform::http::Request&) {
        const int n = ++httpCalls;
        platform::http::Response res;
        res.statusCode = 200;
        res.body = n == 1
            ? R"({"id":"m1","stop_reason":"tool_use","content":[)"
              R"({"type":"tool_use","id":"tu1","name":"test.slow","input":{}}],)"
              R"("usage":{"input_tokens":10,"output_tokens":5}})"
            : R"({"id":"m2","stop_reason":"end_turn","content":[)"
              R"({"type":"text","text":"done"}],)"
              R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));
    mod->onStart();
    DOCTEST_REQUIRE(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_REQUIRE(core::ToolRegistry::instance().find("app.notify").isOk());

    // compose 出任务行后点「启动」——手动触发路径就是 compose 的 onClick 回调
    {
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        eui::Ui ui;
        ui.begin("agent_stop");
        mod->compose(ui, screen);
        ui.end();
        const auto* runBtn = ui.find("agent.task." + std::to_string(taskId) +
                                     ".run.bg");
        DOCTEST_REQUIRE_MESSAGE(runBtn != nullptr,
                                "启动按钮未渲染（任务列表空？）");
        DOCTEST_REQUIRE(runBtn->onClick != nullptr);
        runBtn->onClick();
    }

    // 有界等待（10ms 步进 ≤5s）工具被调一次；此刻它正在睡 300ms——
    // onStop 必须跨过这段睡眠才算真 join
    bool called = false;
    for (int i = 0; i < 500; ++i) {
        if (tool->calls.load() >= 1) {
            called = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(called, "run 线程 5s 内未调到慢工具");
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);

    mod->onStop();
    const long long stopEndMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    // 100ms 观察窗内工具不再被调用
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);
    // join 真等了 run 收尾的判据：onStop 返回时刻不早于工具 execute 返回时刻。
    // 不用耗时阈值判定——calls==1 到进入 onStop 之间耗掉大半睡眠是正常调度，慢机上会假红；
    // 未 join 则 onStop 近 0ms 返回，必然早于 endMs。
    DOCTEST_CHECK_MESSAGE(stopEndMs >= tool->endMs.load(),
                          "onStop 在工具 execute 返回前就返回了（未 join run 线程？stopEnd=" +
                              std::to_string(stopEndMs) + " toolEnd=" +
                              std::to_string(tool->endMs.load()) + "ms）");
    // run 落库 cancelled（cancel 下轮迭代前退出 → finishRun("cancelled")）
    auto runs = store.listRuns(taskId, 5);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    // teardown 顺序：unregister 发生在 join 之后，提前注销则此处记 false
    DOCTEST_CHECK_MESSAGE(tool->sawAppStatsRegistered.load(),
                          "工具执行期间 app.stats 已被注销（unregister 未等 join）");
    DOCTEST_CHECK_MESSAGE(tool->sawAppNotifyRegistered.load(),
                          "工具执行期间 app.notify 已被注销（unregister 未等 join）");
    return true;
})

REGISTER_TEST(agent_module_ui_id_contract,
              "agent_module: 最小 UI 元素 id 契约（标题/表单/任务行/启动/状态/详情展开）", {
    test_support::TestDb tdb("agenttools", "modui");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    core::ToolRegistry::instance().unregisterTool("hot.list");

    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("整理逾期", "把逾期待办全部完成",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);
    auto runId = store.startRun(taskId);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE(store
                        .appendMessage(runId.value(), 0, "user",
                                       R"([{"type":"text","text":"把逾期待办全部完成"}])",
                                       std::nullopt)
                        .isOk());
    DOCTEST_REQUIRE(store
                        .appendMessage(runId.value(), 1, "assistant",
                                       R"([{"type":"text","text":"好的，我已开始"}])",
                                       std::nullopt)
                        .isOk());
    DOCTEST_REQUIRE(store
                        .finishRun(runId.value(), "completed", 1, std::nullopt,
                                   std::nullopt, std::nullopt)
                        .isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));
    mod->onStart();

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    ui.begin("agent_ui");
    mod->compose(ui, screen);
    ui.end();

    DOCTEST_CHECK(ui.find("agent.page") != nullptr);
    DOCTEST_CHECK(ui.find("agent.title") != nullptr);
    // 新建入口是标题行「+ 新建任务」按钮（开弹窗），页内不再有 form.* 节点
    DOCTEST_CHECK(ui.find("agent.form.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.create") == nullptr);
    DOCTEST_CHECK(ui.find("agent.page.newTask") != nullptr);
    DOCTEST_CHECK(ui.find("agent.tasklist") != nullptr);
    DOCTEST_CHECK(ui.find("agent.tasklist.content") != nullptr);   // scrollView 内容列
    DOCTEST_CHECK(ui.find("agent.task." + tid) != nullptr);
    DOCTEST_CHECK(ui.find("agent.task." + tid + ".title") != nullptr);
    // button 的文本挂在 <id>.text 子元素上
    const auto* runText = ui.find("agent.task." + tid + ".run.text");
    DOCTEST_REQUIRE(runText != nullptr);
    DOCTEST_CHECK_EQ(runText->text, std::string("启动"));
    // 最近 run 完成 → 状态列含「成功」；状态元素恒存在（无 run 显示「—」）
    const auto* status = ui.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("成功") != std::string::npos);
    DOCTEST_CHECK(ui.find("agent.dialog.detail.msgscroll") == nullptr);

    {
        const auto* detailBtn = ui.find("agent.task." + tid + ".detail");
        DOCTEST_REQUIRE(detailBtn != nullptr);
        DOCTEST_REQUIRE(detailBtn->onClick != nullptr);
        detailBtn->onClick();
        eui::Ui ui2;
        ui2.begin("agent_ui2");
        mod->compose(ui2, screen);
        ui2.end();
        DOCTEST_CHECK(ui2.find("agent.dialog.detail") != nullptr);
        DOCTEST_CHECK(ui2.find("agent.dialog.detail.header.meta") != nullptr);
        // 切「运行详情」tab（方案 A 三档）：消息区默认折叠（D6），
        // 先点 .msgs.toggle 再看消息行
        DOCTEST_CHECK(ui2.find("agent.dialog.detail.msgscroll") == nullptr);
        const auto* navTab1 = ui2.find("agent.dialog.detail.nav.item.1.bg");
        DOCTEST_REQUIRE(navTab1 != nullptr);
        DOCTEST_REQUIRE(navTab1->onClick != nullptr);
        navTab1->onClick();
        eui::Ui ui2a;
        ui2a.begin("agent_ui2a");
        mod->compose(ui2a, screen);
        ui2a.end();
        const auto* msgsToggle = ui2a.find("agent.dialog.detail.msgs.toggle");
        DOCTEST_REQUIRE(msgsToggle != nullptr);
        DOCTEST_REQUIRE(msgsToggle->onClick != nullptr);
        msgsToggle->onClick();
        eui::Ui ui2b;
        ui2b.begin("agent_ui2b");
        mod->compose(ui2b, screen);
        ui2b.end();
        DOCTEST_CHECK(ui2b.find("agent.dialog.detail.msgscroll") != nullptr);
        // msg.<i> 是容器元素：role 在 .badge、预览文本在 .preview
        const auto* badge0 = ui2b.find("agent.dialog.detail.msg.0.badge");
        DOCTEST_REQUIRE(badge0 != nullptr);
        DOCTEST_CHECK(badge0->text == std::string("用户"));
        const auto* preview0 = ui2b.find("agent.dialog.detail.msg.0.preview");
        DOCTEST_REQUIRE(preview0 != nullptr);
        DOCTEST_CHECK(preview0->text.find("把逾期待办全部完成") != std::string::npos);
        const auto* closeBtn = ui2b.find("agent.dialog.detail.close");
        DOCTEST_REQUIRE(closeBtn != nullptr);
        DOCTEST_REQUIRE(closeBtn->onClick != nullptr);
        closeBtn->onClick();
        eui::Ui ui3;
        ui3.begin("agent_ui3");
        mod->compose(ui3, screen);
        ui3.end();
        DOCTEST_CHECK(ui3.find("agent.dialog.detail.msgscroll") == nullptr);
        DOCTEST_CHECK(ui3.find("agent.task." + tid + ".title") != nullptr);
    }

    mod->onStop();
    return true;
})

REGISTER_TEST(agent_module_onstart_reentry_idempotent,
              "agent_module: onStart→onStop→onStart 重入 + 双 onStart/双 onStop no-op", {
    test_support::TestDb tdb("agenttools", "reentry");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");  // 起点干净
    core::ToolRegistry::instance().unregisterTool("hot.list");
    module::agent::AgentModule mod({&tdb.handle(), &profile, nullptr});

    // 双 onStart 的可观察面（find 之外）：重入幂等 = 不重跑 onStart 体——
    // sweepStaleRuns 会把 running run 清成 failed，二次清扫对「重入期间新出现的
    // running run」是数据误伤（活门控 on→on 重复 onStart 时它可能是活 run）
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("重入任务", "双 onStart 不应二次清扫",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    auto runningCount = [&]() -> int {
        auto runs = store.listRuns(taskId, 10);
        if (runs.isFail()) return -1;
        int n = 0;
        for (const auto& r : runs.value()) {
            if (r.status == "running") ++n;
        }
        return n;
    };

    // 先种一条悬挂 running run，否则下面的 count==0 是空集恒真——真正把它清成 failed
    // 的是首 onStart 的 sweepStaleRuns
    DOCTEST_REQUIRE(store.startRun(taskId).isOk());

    mod.onStart();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isOk());
    DOCTEST_CHECK_EQ(runningCount(), 0);   // 首次 onStart 清扫：种下的悬挂 run 已清

    DOCTEST_REQUIRE(store.startRun(taskId).isOk());   // 双 onStart 前再种一条
    mod.onStart();   // 双 onStart：no-op（不重复注册、不重建成员、不重跑清扫）
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isOk());
    DOCTEST_CHECK_EQ(runningCount(), 1);   // ← 回退守卫即红：二次清扫会把它清成 failed

    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isFail());
    mod.onStop();    // 双 onStop：no-op
    mod.onStart();   // off→on 循环：工具恢复
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isOk());
    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isFail());
    return true;
})

// onStart 建 scheduler_ 并 start()；onStop 的 stopAll() 排在「全取消 → 全 join → 清容器」
// 之后、unregisterTool("app.stats") 之前。断言三件：cron 真被 tick 线程派发（装配缺失即红）、
// 在跑 run 收尾落 cancelled、onStop 之后工具与上游请求都不再发生（收尾不外溢）。
// 时钟经 Deps::schedulerNow 注入假时钟；tick 轮询粒度 200ms 是唯一的墙钟等待。
REGISTER_TEST(agent_module_onstop_stops_scheduler_runs,
              "agent_module: onStop 收口 Scheduler 在跑 run（cancel→join→停线程→注销工具）", {
    test_support::TestDb tdb("agenttools", "schedstop");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");   // 起点干净
    core::ToolRegistry::instance().unregisterTool("hot.list");
    core::ToolRegistry::instance().unregisterTool("test.slow");
    auto slow = std::make_shared<SlowTool>();
    SlowTool* tool = slow.get();
    DOCTEST_REQUIRE(core::ToolRegistry::instance().registerTool(slow).isOk());
    ToolGuard slowGuard{"test.slow"};

    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("定时慢", "调用 test.slow 一次", "schedule",
                                    "default", 10, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    std::atomic<int64_t> fakeNow{3600 * 24};
    deps.schedulerNow = [&fakeNow] { return fakeNow.load(); };    // 假时钟注入
    int httpCalls = 0;
    deps.http = [&httpCalls](const platform::http::Request&) {
        ++httpCalls;
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"tool_use","content":[)"
                   R"({"type":"tool_use","id":"tu1","name":"test.slow","input":{}}],)"
                   R"("usage":{"input_tokens":10,"output_tokens":5}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));
    mod->onStart();
    fakeNow.store(3600 * 24 + 60);      // 推进一分钟 → tick 线程 ≤200ms 内探到翻转
    for (int i = 0; i < 500 && tool->calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE_MESSAGE(tool->calls.load() == 1,
                            "tick 线程 5s 内未派发 cron run（装配缺失？）");

    mod->onStop();                       // 阻塞到 cancel+join+停线程完成
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.notify").isFail());
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);   // onStop 后不再被调
    DOCTEST_CHECK_EQ(httpCalls, 1);            // join 真等到了 run 收尾，收尾后无新请求
    auto runs = store.listRuns(taskId, 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    return true;
})

// 惰性装配契约：onStart 时配置来源未装配 → 早退（llm_ / loop_ / scheduler_ 均不建，
// 工具照常注册）；会话内后置注册服务后由**首次使用路径**（页面帧 / 手动启动）补建。
// 补建入口必须同时建 scheduler_，否则惰性路径下 cron 永不触发——本用例第二条 REQUIRE
// 钉的就是这个面。
REGISTER_TEST(agent_module_lazy_assembly_first_use,
              "agent_module: 配置来源后置注册 → 首次使用解析（手动起 run + 惰性补建 Scheduler 跑 cron）", {
    test_support::TestDb tdb("agenttools", "lazyassembly");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");   // 起点干净
    core::ToolRegistry::instance().unregisterTool("hot.list");

    module::agent::AgentStore store(tdb.handle());
    auto manual = store.createTask("手动", "直接回复完成", "manual", "default", 10, false);
    DOCTEST_REQUIRE(manual.isOk());
    auto scheduled = store.createTask("定时", "直接回复完成", "schedule", "default", 10,
                                      false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(scheduled.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = nullptr;   // 起点无服务（test_main 每用例 ServiceLocator::resetForTest）
    std::atomic<int64_t> fakeNow{3600 * 24};
    deps.schedulerNow = [&fakeNow] { return fakeNow.load(); };
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"done"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));
    mod->onStart();   // 早退分支：LLM 通道未建，自带工具照常注册
    DOCTEST_REQUIRE(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_REQUIRE(core::ToolRegistry::instance().find("app.notify").isOk());
    auto before = store.listRuns(manual.value(), 10);
    DOCTEST_REQUIRE(before.isOk());
    DOCTEST_REQUIRE(before.value().empty());   // 起点确无 run（下面的落行断言非空集恒真）

    // 后置注册：模拟会话内用户拨开 claude 模块（不 off→on 本模块）
    core::ScopedService<core::LlmProfileSource> late(&profile);

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    ui.begin("agent_lazy_assembly");
    mod->compose(ui, screen);
    ui.end();
    const auto* runBtn =
        ui.find("agent.task." + std::to_string(manual.value()) + ".run.bg");
    DOCTEST_REQUIRE_MESSAGE(runBtn != nullptr, "启动按钮未渲染（任务列表空？）");
    DOCTEST_REQUIRE(runBtn->onClick != nullptr);
    runBtn->onClick();

    auto waitCompleted = [&store](int64_t taskId) {
        for (int i = 0; i < 500; ++i) {
            auto runs = store.listRuns(taskId, 1);
            if (runs.isOk() && !runs.value().empty() &&
                runs.value().front().status == "completed") {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    DOCTEST_REQUIRE_MESSAGE(waitCompleted(manual.value()),
                            "首次使用路径未起 run（惰性解析缺失？）");
    DOCTEST_REQUIRE_MESSAGE(waitCompleted(scheduled.value()),
                            "惰性路径下 cron 未被派发（scheduler_ 未在同一入口补建？）");
    mod->onStop();
    return true;
})

// hot.list 注册进模块工具表（T3c2）：与 app.stats/app.notify 同一注册/注销口径。
// 非破坏性只读工具（isDestructive 默认 false）；AgentLoop 的工具表白名单经
// allSchemasJson 动态收集，注册进注册表即自动进 prompt，零硬编码。
REGISTER_TEST(agent_module_registers_hot_list,
              "agent_module: onStart 注册 hot.list（非破坏性）、onStop 注销（生命周期）", {
    test_support::TestDb tdb("agenttools", "modhot");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    core::ToolRegistry::instance().unregisterTool("hot.list");   // 起点干净

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));

    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isFail());
    mod->onStart();
    auto found = core::ToolRegistry::instance().find("hot.list");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK_EQ(found.value()->name(), std::string("hot.list"));
    DOCTEST_CHECK_FALSE(found.value()->isDestructive());   // 拉热榜无不可逆副作用

    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isFail());
    return true;
})

REGISTER_TEST(agent_module_hot_list_reentry_idempotent,
              "agent_module: hot.list 随 onStart→onStop→onStart 循环恢复，双 onStart/双 onStop no-op", {
    test_support::TestDb tdb("agenttools", "hotreentry");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    core::ToolRegistry::instance().unregisterTool("hot.list");   // 起点干净
    module::agent::AgentModule mod({&tdb.handle(), &profile, nullptr});

    mod.onStart();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isOk());
    mod.onStart();   // 双 onStart：no-op（started_ 闸，不重复注册）
    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isOk());

    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isFail());
    mod.onStop();    // 双 onStop：no-op
    mod.onStart();   // off→on 循环：工具恢复
    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isOk());
    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("hot.list").isFail());
    return true;
})

REGISTER_TEST(agent_module_hot_list_in_all_schemas,
              "agent_module: hot.list 注册后进入 allSchemasJson（AgentLoop 工具表动态收集，零硬编码）", {
    test_support::TestDb tdb("agenttools", "hotschema");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    core::ToolRegistry::instance().unregisterTool("hot.list");   // 起点干净
    module::agent::AgentModule mod({&tdb.handle(), &profile, nullptr});

    const std::string schemasBefore =
        core::ToolRegistry::instance().allSchemasJson();
    mod.onStart();
    const std::string schemas =
        core::ToolRegistry::instance().allSchemasJson();
    DOCTEST_CHECK_MESSAGE(schemas.find("hot.list") != std::string::npos,
                          "allSchemasJson 未收录 hot.list");
    DOCTEST_CHECK_MESSAGE(schemasBefore.find("hot.list") == std::string::npos,
                          "起点注册表已含 hot.list（前置清理失效）");
    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance()
                      .allSchemasJson()
                      .find("hot.list") == std::string::npos);
    return true;
})
