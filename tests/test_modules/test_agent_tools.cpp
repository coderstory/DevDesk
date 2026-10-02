// tests/test_modules/test_agent_tools.cpp
// agent 工具用例（agent M1 Task 10：app.stats 只读统计；Task 12 的
// AgentModule 生命周期用例共用本文件）。
//
// 错误回喂约定（spec §10 M1 统一）：工具级失败 → Result ok + isError=true +
// 中文说明；Result::fail 只留给编程不变量破坏。
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
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ============ app.stats ============

// 直查三表 COUNT(*) 的只读工具：种子数据经原生 SQL 插入（不绕业务服务层，
// 断言的是工具对表行数的真实读数）。注意断言宏不能进 lambda（展开含
// return false，在 lambda 里会只退出 lambda 假装绿），故逐条平铺。
REGISTER_TEST(app_stats_counts_seeded_rows,
              "app.stats: 统计 todo_tasks/markdown_index/agent_tasks 三表行数", {
    test_support::TestDb tdb("agenttools", "stats");

    // 种数据：todo_tasks 2 行 / markdown_index 1 行 / agent_tasks 1 行
    //（todo_columns v2 种子保证 column_id=1 存在；created_at 等必填列给占位值）
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

    // schema 抽查 + 只读
    const std::string schema = tool->schemaJson();
    DOCTEST_CHECK(schema.find("\"name\":\"app.stats\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("input_schema") != std::string::npos);
    DOCTEST_CHECK_FALSE(tool->isDestructive());
    return true;
})

// ============ AgentModule 生命周期（Task 12）============

namespace {

// 测试用 fake 代理服务：实现 IClaudeProxyService 全部虚方法（与
// test_llm_client.cpp 的 FakeProxy 同款；本组用例走 isProxyRunning /
// proxyPort / resolveModelSlot 三条链路）。
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

// RAII：用例退出（含 fail-fast 提前 return 的路径）时把注册的工具从进程级
// 注册表摘除——ToolRegistry 不在 test_main 的 resetCaseGlobals 清单里，
// 靠用例内显式清理会漏 fail-fast 路径。
struct ToolGuard {
    std::string name;
    ~ToolGuard() { core::ToolRegistry::instance().unregisterTool(name); }
};

// 慢工具：execute 睡 300ms（本组用例唯一允许的长睡——模拟真实工具耗时），
// 醒来后记录那一刻 app.stats 是否仍在注册表。teardown 顺序探针：正确顺序
// （cancel → join → clear → unregister）下，工具执行期间 onStop 还阻塞在
// join，app.stats 必然尚未注销；若实现把 unregister 提到 join 前，本探针
// 记到 false。
class SlowTool : public core::ITool {
public:
    std::string name() const override { return "test.slow"; }
    std::string schemaJson() const override { return R"({"name":"test.slow"})"; }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        sawAppStatsRegistered =
            core::ToolRegistry::instance().find("app.stats").isOk();
        endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return core::Result<core::ToolResult>::ok(
            core::ToolResult{false, R"("ok")"});
    }
    std::atomic<int> calls{0};
    std::atomic<bool> sawAppStatsRegistered{false};
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

    // keep-alive：摘除注册表条目后快照仍可用（shared_ptr 持活）
    reg.unregisterTool("test.alpha");
    bool stillUsable = false;
    for (const auto& t : tools) {
        if (t->name() == "test.alpha") stillUsable = true;
    }
    DOCTEST_CHECK(stillUsable);
    return true;
})

REGISTER_TEST(agent_module_registers_own_tool,
              "agent_module: onStart 注册 app.stats、onStop 注销（生命周期）", {
    test_support::TestDb tdb("agenttools", "modtool");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};   // 兜底摘除（含 fail-fast 路径）
    core::ToolRegistry::instance().unregisterTool("app.stats");  // 起点干净

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));

    // 未启动：未注册；onStart 后：注册且可按名找到
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    mod->onStart();
    auto found = core::ToolRegistry::instance().find("app.stats");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK_EQ(found.value()->name(), std::string("app.stats"));
    DOCTEST_CHECK_FALSE(found.value()->isDestructive());

    // onStop 后：注销（find 恢复 NotFound）
    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    return true;
})

REGISTER_TEST(agent_module_onstop_cancels_and_joins,
              "agent_module: onStop 取消+join 完成后才注销工具，run 落库 cancelled", {
    test_support::TestDb tdb("agenttools", "modstop");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");  // 起点干净

    // 慢工具挂进注册表（mock 响应点名调用它；RAII 摘除）
    auto slow = std::make_shared<SlowTool>();
    SlowTool* tool = slow.get();
    DOCTEST_REQUIRE(core::ToolRegistry::instance().registerTool(slow).isOk());
    ToolGuard slowGuard{"test.slow"};

    // 种任务（直接经 store，与 UI 无关）
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("慢工具任务", "调用 test.slow 一次",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();

    // Deps 注入：fake 代理 + 脚本化 HTTP（首轮吐 tool_use(test.slow)，次轮
    // end_turn——teardown 正常时次轮到不了：cancel 在首轮工具执行期间已置位）
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
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

    // compose → 任务行出现 → 点「启动」（手动触发路径 = compose onClick 回调）
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

    // 有界等待（10ms 步进 ≤5s）工具被调一次；此刻它正在睡 300ms
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

    // 立即 onStop：应阻塞 join 到 run 收尾（跨过工具剩余睡眠）
    mod->onStop();
    const long long stopEndMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    // join 完成：onStop 返回后工具不再被调用（100ms 观察窗计数不变）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);
    // join 真等了 run 收尾的确定性判据：onStop 返回时刻不早于工具 execute
    // 返回时刻。旧判据 stopMs>=250 对负载敏感（观察到 calls=1 → 进入 onStop
    // 之间耗掉大半睡眠是正常调度，CI 慢机上假红）；本判据在任意负载下都成立
    //——未 join 则 onStop 近 0ms 返回，必然早于 endMs。
    DOCTEST_CHECK_MESSAGE(stopEndMs >= tool->endMs.load(),
                          "onStop 在工具 execute 返回前就返回了（未 join run 线程？stopEnd=" +
                              std::to_string(stopEndMs) + " toolEnd=" +
                              std::to_string(tool->endMs.load()) + "ms）");
    // run 落库 cancelled（cancel 下轮迭代前退出 → finishRun("cancelled")）
    auto runs = store.listRuns(taskId, 5);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    // teardown 顺序探针：工具执行期间 app.stats 还在注册表
    //（= unregister 发生在 join 之后；提前注销则此处记 false）
    DOCTEST_CHECK_MESSAGE(tool->sawAppStatsRegistered.load(),
                          "工具执行期间 app.stats 已被注销（unregister 未等 join）");
    return true;
})

REGISTER_TEST(agent_module_ui_id_contract,
              "agent_module: 最小 UI 元素 id 契约（标题/表单/任务行/启动/状态/详情展开）", {
    test_support::TestDb tdb("agenttools", "modui");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");

    // 种任务 + 一次已完成 run + 两条消息（状态列与详情展开的断言数据）
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
    deps.proxy = &proxy;
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));
    mod->onStart();

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    ui.begin("agent_ui");
    mod->compose(ui, screen);
    ui.end();

    // 契约表逐项（2026-10-02 批次 A 骨架重做后的元素 id 契约）
    DOCTEST_CHECK(ui.find("agent.page") != nullptr);
    DOCTEST_CHECK(ui.find("agent.title") != nullptr);
    // 页内新建表单已删：新建入口 = 标题行「+ 新建任务」按钮（开任务弹窗）
    DOCTEST_CHECK(ui.find("agent.form.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.create") == nullptr);
    DOCTEST_CHECK(ui.find("agent.page.newTask") != nullptr);
    DOCTEST_CHECK(ui.find("agent.tasklist") != nullptr);
    DOCTEST_CHECK(ui.find("agent.tasklist.content") != nullptr);   // scrollView 内容列
    DOCTEST_CHECK(ui.find("agent.task." + tid) != nullptr);
    DOCTEST_CHECK(ui.find("agent.task." + tid + ".title") != nullptr);
    // 启动按钮：text == "启动"（button 结构：<id>.text 子元素载文本）
    const auto* runText = ui.find("agent.task." + tid + ".run.text");
    DOCTEST_REQUIRE(runText != nullptr);
    DOCTEST_CHECK_EQ(runText->text, std::string("启动"));
    // 最近 run 完成 → 状态列含「成功」；状态元素恒存在（无 run 显示「—」）
    const auto* status = ui.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("成功") != std::string::npos);
    // 详情改弹窗：未打开时弹窗内容节点不存在
    DOCTEST_CHECK(ui.find("agent.dialog.detail.msgscroll") == nullptr);

    // 点行内「详情」→ 弹窗打开（配置摘要 + 消息流），再点 ✕ 关闭
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
        DOCTEST_CHECK(ui2.find("agent.dialog.detail.info") != nullptr);
        DOCTEST_CHECK(ui2.find("agent.dialog.detail.msgscroll") != nullptr);
        // 2026-10-02 run 日志改造：msg.<i> 是容器，role 在 .badge、预览在 .preview
        const auto* badge0 = ui2.find("agent.dialog.detail.msg.0.badge");
        DOCTEST_REQUIRE(badge0 != nullptr);
        DOCTEST_CHECK(badge0->text == std::string("用户"));
        const auto* preview0 = ui2.find("agent.dialog.detail.msg.0.preview");
        DOCTEST_REQUIRE(preview0 != nullptr);
        DOCTEST_CHECK(preview0->text.find("把逾期待办全部完成") != std::string::npos);
        // 关弹窗（✕）→ 弹窗内容消失，任务列表原样还在
        const auto* closeBtn = ui2.find("agent.dialog.detail.close");
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
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");  // 起点干净
    module::agent::AgentModule mod({&tdb.handle(), &proxy, nullptr});

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

    // 先种一条悬挂 running run（首 onStart 前）：下面的 count==0 才是真断言
    //（onStart#1 的 sweepStaleRuns 把它清成 failed），不是空集恒真检查
    DOCTEST_REQUIRE(store.startRun(taskId).isOk());

    mod.onStart();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_CHECK_EQ(runningCount(), 0);   // 首次 onStart 清扫：种下的悬挂 run 已清

    DOCTEST_REQUIRE(store.startRun(taskId).isOk());   // 双 onStart 前再种一条
    mod.onStart();   // 双 onStart：no-op（不重复注册、不重建成员、不重跑清扫）
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    DOCTEST_CHECK_EQ(runningCount(), 1);   // ← 回退守卫即红：二次清扫会把它清成 failed

    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    mod.onStop();    // 双 onStop：no-op
    mod.onStart();   // off→on 循环：工具恢复
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isOk());
    mod.onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    return true;
})

// ============ AgentModule 装配 Scheduler（M2 Task 6）============

// spec §3 teardown 契约 + plan Task 6：onStart 建 scheduler_ 并 start()；onStop 在
// 「全取消 → 全 join → 清容器」之后、`unregisterTool("app.stats")` 之前判空调
// stopAll()。断言三件：cron 真被 tick 线程派发（装配缺失即红）、在跑 run 收尾落
// cancelled、onStop 之后工具与上游请求都不再发生（收尾不外溢）。
// 时钟经 Deps::schedulerNow 注入假时钟，零墙钟等待（tick 轮询粒度 200ms 除外）。
REGISTER_TEST(agent_module_onstop_stops_scheduler_runs,
              "agent_module: onStop 收口 Scheduler 在跑 run（cancel→join→停线程→注销工具）", {
    test_support::TestDb tdb("agenttools", "schedstop");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");   // 起点干净
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
    deps.proxy = &proxy;
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
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);   // onStop 后不再被调
    DOCTEST_CHECK_EQ(httpCalls, 1);            // join 真等到了 run 收尾，收尾后无新请求
    auto runs = store.listRuns(taskId, 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    return true;
})

// Task 6 Step 1b（2026-10-01 终审 finding：claude-lazy-assembly）：onStart 时代理
// 未装配 → 早退（llm_/loop_/scheduler_ 均不建，工具照常注册）→ 会话内后置注册
// 服务 → **首次使用路径**（页面帧 / 手动启动）拿到服务；同一入口必须补建
// scheduler_，否则惰性路径下 cron 永不触发（本用例第二条 REQUIRE 钉这个面）。
REGISTER_TEST(agent_module_lazy_assembly_first_use,
              "agent_module: 代理后置注册 → 首次使用解析（手动起 run + 惰性补建 Scheduler 跑 cron）", {
    test_support::TestDb tdb("agenttools", "lazyassembly");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");   // 起点干净

    module::agent::AgentStore store(tdb.handle());
    auto manual = store.createTask("手动", "直接回复完成", "manual", "default", 10, false);
    DOCTEST_REQUIRE(manual.isOk());
    auto scheduled = store.createTask("定时", "直接回复完成", "schedule", "default", 10,
                                      false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(scheduled.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = nullptr;   // 起点无服务（test_main 每用例 ServiceLocator::resetForTest）
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
    auto before = store.listRuns(manual.value(), 10);
    DOCTEST_REQUIRE(before.isOk());
    DOCTEST_REQUIRE(before.value().empty());   // 起点确无 run（下面的落行断言非空集恒真）

    // 后置注册：模拟会话内用户拨开 claude 模块（不 off→on 本模块）
    core::ScopedService<core::IClaudeProxyService> late(&proxy);

    // 首次使用路径 = 页面帧（compose）→ 手动「启动」
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
    // 惰性补建的 Scheduler 真在跑：cron 任务被 tick 线程派发（不补建即红）
    DOCTEST_REQUIRE_MESSAGE(waitCompleted(scheduled.value()),
                            "惰性路径下 cron 未被派发（scheduler_ 未在同一入口补建？）");
    mod->onStop();
    return true;
})
