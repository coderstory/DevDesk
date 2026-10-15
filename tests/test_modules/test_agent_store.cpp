// tests/test_modules/test_agent_store.cpp
// AgentStore 落库层用例：任务 → run → 消息 的回写读出、sweep 收悬挂 run、listTasks 列映射、
// startRunExclusive 原子互斥起跑、updateTask/setTaskEnabled/getTask 往返、taskFromRow 行映射、
// listRuns 同秒并列的 tie-break，以及 trigger_config 编解码唯一真源。
// 跨文件前提：
//   - 每个用例经 test_support::TestDb 拿独立临时库（已 migrate 到含 agent 三表的版本），不碰用户真实库。
//   - trigger_config 的编解码只认 module::agent 的 encode/decodeTriggerConfig，UI 与调度器都不得手拼。
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "module_agent/trigger_config.h"
#include "core/llm_endpoint.h"
#include "core/tool.h"
#include "platform/json.h"
#include <atomic>
#include <string>

namespace {

// 只会数调用次数的 LlmEndpoint：单闸用例断言它一次都没被调用（撞闸不发请求）。
class NeverSendLlm : public core::LlmEndpoint {
public:
    int calls = 0;
    core::Result<core::LlmResponse> send(const std::vector<core::LlmMessage>&,
                                         const std::string&, const std::string&,
                                         const std::string&) override {
        ++calls;
        return core::Result<core::LlmResponse>::fail(core::ErrorCode::Internal,
                                                     "撞闸的 run 不该发请求");
    }
};

} // namespace

REGISTER_TEST(agent_store_task_run_message_roundtrip, "agent_store: 任务→run→消息 回写读出一致", {
    test_support::TestDb tdb("agentstore", "rt");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto runId = store.startRun(taskId.value());
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE(store.appendMessage(runId.value(), 0, "user", R"([{"type":"text","text":"hi"}])", std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(runId.value(), 1, "assistant", R"([{"type":"tool_use","id":"t1"}])", std::nullopt).isOk());
    auto msgs = store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_CHECK_EQ(msgs.value().size(), 2u);
    DOCTEST_CHECK_EQ(msgs.value()[0].role, std::string("user"));
    DOCTEST_CHECK_EQ(msgs.value()[1].seq, 1);
    DOCTEST_REQUIRE(store.finishRun(runId.value(), "completed", 3, 120, 45, std::nullopt).isOk());
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(runs.value()[0].iterations, 3);
    DOCTEST_CHECK(runs.value()[0].endedAt.has_value());
    return true;
})

REGISTER_TEST(agent_store_sweep_stale_runs, "agent_store: sweep 把遗留 running 标 failed", {
    test_support::TestDb tdb("agentstore", "sw");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    auto runId = store.startRun(taskId.value());   // 不 finishRun，模拟崩溃
    DOCTEST_REQUIRE(store.sweepStaleRuns().isOk());
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("failed"));
    DOCTEST_REQUIRE(runs.value()[0].error.has_value());
    DOCTEST_CHECK(runs.value()[0].error->find("悬挂") != std::string::npos);
    return true;
})

// listTasks 的 9 列逐列还原：列名与 SELECT 拼写漂移是编译期看不见的错
// （只在运行期表现为 Result::fail(NotFound)），任务列表 UI 直接吃它。
// 入参取有区分度的值（modelSlot=sonnet / maxIterations=7 / allowDestructive=true），
// 与默认列（enabled=1、trigger_config='' 由 createTask 固定写空）各占一条。
REGISTER_TEST(agent_store_list_tasks_maps_columns, "agent_store: listTasks 列映射与值还原", {
    test_support::TestDb tdb("agentstore", "lt");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成",
                                   "manual", "sonnet", 7, true);
    DOCTEST_REQUIRE(taskId.isOk());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK_EQ(tasks.value()[0].id, taskId.value());
    DOCTEST_CHECK_EQ(tasks.value()[0].name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(tasks.value()[0].goal, std::string("把逾期 todo 全部完成"));
    DOCTEST_CHECK_EQ(tasks.value()[0].triggerType, std::string("manual"));
    DOCTEST_CHECK_EQ(tasks.value()[0].triggerConfig, std::string(""));
    DOCTEST_CHECK_EQ(tasks.value()[0].modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(tasks.value()[0].maxIterations, 7);
    DOCTEST_CHECK(tasks.value()[0].allowDestructive);
    DOCTEST_CHECK(tasks.value()[0].enabled);
    return true;
})

// 同 task 并发起跑的唯一闸。手动「启动」与 Scheduler/cron 派发都经 AgentLoop::run → 此处；
// 查+插在同一把 db_.mutex() 内 = 原子。终态（非 running）后必须能再次起跑——
// 闸是「同 task 不并发」，不是「只跑一次」。
REGISTER_TEST(agent_store_start_run_exclusive_rejects_concurrent,
              "agent_store: startRunExclusive 已有 running run → fail(AlreadyExists)", {
    test_support::TestDb tdb("agentstore", "exclusive");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "schedule", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto first = store.startRunExclusive(taskId.value());
    DOCTEST_REQUIRE(first.isOk());
    auto second = store.startRunExclusive(taskId.value());   // 并发第二次
    DOCTEST_REQUIRE(second.isFail());
    DOCTEST_CHECK(second.errorCode() == core::ErrorCode::AlreadyExists);
    // 拒的那次没有落行：第二路从未起跑，无悬挂 running 要收尾
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    // 终态后可再次起跑
    DOCTEST_REQUIRE(store.finishRun(first.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    DOCTEST_CHECK(store.startRunExclusive(taskId.value()).isOk());
    return true;
})

// updateTask 的七列覆盖（enabled 归 setTaskEnabled，updateTask 不碰）+ getTask
// 单查往返 + 不存在 id → fail(NotFound)。
REGISTER_TEST(agent_store_update_enable_get_roundtrip,
              "agent_store: updateTask/setTaskEnabled/getTask 往返 + createTask 带触发配置", {
    test_support::TestDb tdb("agentstore", "update");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("旧名", "旧目标", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(store.updateTask(taskId.value(), "整理逾期", "新目标", "schedule",
                                     R"({"cron":"0 9 * * 1-5"})", "sonnet", 20, true)
                        .isOk());
    auto got = store.getTask(taskId.value());
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(got.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(got.value().triggerConfig, std::string(R"({"cron":"0 9 * * 1-5"})"));
    DOCTEST_CHECK_EQ(got.value().modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(got.value().maxIterations, 20);
    DOCTEST_CHECK(got.value().allowDestructive);
    DOCTEST_CHECK(got.value().enabled);   // updateTask 不碰 enabled，见块首
    DOCTEST_REQUIRE(store.setTaskEnabled(taskId.value(), false).isOk());
    auto off = store.getTask(taskId.value());
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_CHECK_FALSE(off.value().enabled);
    DOCTEST_CHECK(store.getTask(99999).errorCode() == core::ErrorCode::NotFound);
    // 一类一规则：UPDATE 目标行不存在 → fail(NotFound)，不静默 no-op
    //（0 行变更判定，与 getTask 同口径）
    DOCTEST_CHECK(store.updateTask(99999, "n", "g", "manual", "", "default", 1, false)
                      .errorCode() == core::ErrorCode::NotFound);
    DOCTEST_CHECK(store.setTaskEnabled(99999, false).errorCode() == core::ErrorCode::NotFound);
    return true;
})

// createTask 的新尾参 triggerConfig 必须真落库：只加形参不绑库 = 静默丢配置，
// cron 任务会拿到空表达式。6 参调用点仍绑空串（由 listTasks 用例钉住）。
// taskFromRow 是 store 行 → AgentTask 的唯一映射（module / scheduler 共用），
// 逐字段搬运 id/name/goal/modelSlot/maxIterations/allowDestructive。
REGISTER_TEST(agent_store_create_task_trigger_config_and_task_from_row,
              "agent_store: createTask 带触发配置落库 + taskFromRow 逐字段搬运", {
    test_support::TestDb tdb("agentstore", "fromrow");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成", "schedule",
                                   "sonnet", 7, true, R"({"cron":"*/5 * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().triggerConfig, std::string(R"({"cron":"*/5 * * * *"})"));
    const module::agent::AgentTask t = module::agent::taskFromRow(row.value());
    DOCTEST_CHECK_EQ(t.id, taskId.value());
    DOCTEST_CHECK_EQ(t.name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(t.goal, std::string("把逾期 todo 全部完成"));
    DOCTEST_CHECK_EQ(t.modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(t.maxIterations, 7);
    DOCTEST_CHECK(t.allowDestructive);
    return true;
})

// 单闸的 loop 层钉子：task 已有 running 行时，AgentLoop::run 必须走 startRunExclusive
// 撞闸——原样上抛 fail(AlreadyExists)、不落行、不发请求（第二路从未起跑，库里不留
// 第二行 running）。若改回非互斥的 startRun，本用例三条断言全红。
REGISTER_TEST(agent_loop_gate_rejects_when_task_already_running,
              "agent_loop: 已有 running run → run() 撞闸 fail(AlreadyExists) 且不落行不发请求", {
    test_support::TestDb tdb("agentstore", "gate");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto held = store.startRunExclusive(taskId.value());   // 假装他路在跑
    DOCTEST_REQUIRE(held.isOk());
    NeverSendLlm llm;
    module::agent::AgentLoop loop{llm, core::ToolRegistry::instance(), store};
    module::agent::AgentTask task;
    task.id = taskId.value(); task.goal = "g"; task.modelSlot = "default";
    std::atomic<bool> cancel{false};
    auto r = loop.run(task, cancel);
    DOCTEST_REQUIRE(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::AlreadyExists);
    DOCTEST_CHECK_EQ(llm.calls, 0);                       // 撞闸不发请求
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);          // 也没多落一行
    DOCTEST_CHECK_EQ(runs.value()[0].id, held.value());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("running"));
    return true;
})

// 并列 tie-break：agent_runs.started_at 是秒精度，同一秒内连起两次 run 会并列——
// 只按 started_at DESC 时并列段顺序未定义，LIMIT 窗口本身就可能把较旧那条当「最近一次」
// 返回（消费方：状态列 / runlist[0] / 默认选中 run）。用例用原生 SQL 把两条 run 的
// started_at 钉成同值，直接钉住 tie-break。
REGISTER_TEST(agent_store_list_runs_tie_breaks_by_id,
              "agent_store: 同秒两次 run → listRuns 取最新（含 LIMIT 1 窗口）", {
    test_support::TestDb tdb("agentstore", "runorder");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto older = store.startRun(taskId.value());
    DOCTEST_REQUIRE(older.isOk());
    DOCTEST_REQUIRE(store.finishRun(older.value(), "limited", 7, int64_t(190000),
                                    int64_t(15000), std::string("limit")).isOk());
    auto newer = store.startRun(taskId.value());
    DOCTEST_REQUIRE(newer.isOk());
    DOCTEST_REQUIRE(store.finishRun(newer.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    // 把两条钉成同一秒（不依赖用例是否跨秒，杜绝负载敏感的假红/假绿）
    DOCTEST_REQUIRE(tdb.handle()
                        .exec("UPDATE agent_runs SET started_at = 1000")
                        .isOk());
    auto all = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(all.value()[0].startedAt, all.value()[1].startedAt);  // 并列前提
    DOCTEST_CHECK_EQ(all.value()[0].id, newer.value());   // 并列时最新在前
    DOCTEST_CHECK_EQ(all.value()[1].id, older.value());
    // LIMIT 1 窗口也必须返回较新那条：并列 + 无 tie-break 时这里会拿到旧 run
    auto latest = store.listRuns(taskId.value(), 1);
    DOCTEST_REQUIRE(latest.isOk());
    DOCTEST_REQUIRE_EQ(latest.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(latest.value()[0].id, newer.value());
    DOCTEST_CHECK_EQ(latest.value()[0].status, std::string("completed"));
    return true;
})

// 本组钉住「store 行 ↔ TriggerConfig 结构」的往返与坏配置的可判定性：
// 编解码散在多处时谁都不能保证自己那份和别处同口径——这里以「encode 出来的字符串经
// createTask 落库、getTask 读出、decode 回来的结构必须逐字段相等」把两端钉在一起。
REGISTER_TEST(agent_store_trigger_config_roundtrip,
              "agent_store: trigger_config 往返（encode → createTask → getTask → decode 逐字段一致）", {
    test_support::TestDb tdb("agentstore", "trigcfg");
    module::agent::AgentStore store(tdb.handle());

    // 常规表达式 + 转义敏感表达式（引号 / 反斜杠 / 制表符——手拼 JSON 时必炸的字符；
    // json 层转义才对）。decode 回来的结构必须逐字相等
    for (const char* expr : {"0 9 * * 1-5", "* * * * *",
                             R"(a"b\c	d)"}) {
        module::agent::TriggerConfig cfg;
        cfg.cron = expr;
        auto encoded = module::agent::encodeTriggerConfig(cfg);
        DOCTEST_REQUIRE(encoded.isOk());
        // 编码产物必须是合法 JSON 对象（不是手拼的近似物）
        auto parsed = platform::json::parse(encoded.value());
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_CHECK(parsed.value().isObject());

        auto taskId = store.createTask("往返", "g", "schedule", "default", 5, false,
                                       encoded.value());
        DOCTEST_REQUIRE(taskId.isOk());
        auto row = store.getTask(taskId.value());
        DOCTEST_REQUIRE(row.isOk());
        DOCTEST_CHECK_EQ(row.value().triggerConfig, encoded.value());

        auto decoded = module::agent::decodeTriggerConfig(row.value().triggerConfig);
        DOCTEST_REQUIRE(decoded.isOk());
        DOCTEST_CHECK_EQ(decoded.value().cron, std::string(expr));
    }
    return true;
})

REGISTER_TEST(agent_store_trigger_config_decode_rejects_malformed,
              "trigger_config decode: 空/坏 JSON/非对象/缺 cron/空 cron → Result fail", {
    using module::agent::decodeTriggerConfig;
    // 空串 = 非 schedule（或未配置）任务的正常态，但对「要 cron 的人」是坏配置
    DOCTEST_CHECK(decodeTriggerConfig("").isFail());
    DOCTEST_CHECK(decodeTriggerConfig("{ not json !!!").isFail());
    DOCTEST_CHECK(decodeTriggerConfig(R"(["cron"])").isFail());      // 非对象
    DOCTEST_CHECK(decodeTriggerConfig("{}").isFail());               // 缺 cron
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":123})").isFail());  // 类型不符
    DOCTEST_CHECK(decodeTriggerConfig(R"({"cron":""})").isFail());   // 空表达式
    // 错误信息可读（调度器把它拼进点名 task id 的 WARN）
    auto bad = decodeTriggerConfig(R"({"cron":""})");
    DOCTEST_CHECK(!bad.errorMessage().empty());
    // 正常态：合法对象 + 非空字符串
    auto good = decodeTriggerConfig(R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(good.isOk());
    DOCTEST_CHECK_EQ(good.value().cron, std::string("* * * * *"));
    // 多余键不致命（向后兼容：以后加键，旧读取路径不会因此判坏）
    auto extra = decodeTriggerConfig(R"({"cron":"* * * * *","tz":"UTC"})");
    DOCTEST_REQUIRE(extra.isOk());
    DOCTEST_CHECK_EQ(extra.value().cron, std::string("* * * * *"));
    return true;
})

// context_note / auto_note：createTask 默认值（不传尾参 = 空串/1）+ getTask 读回一致
REGISTER_TEST(agent_store_context_note_defaults,
              "agent_store: createTask 不传 context note 尾参 → 默认空串/autoNote=1", {
    test_support::TestDb tdb("agentstore", "ctxdef");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("默认记忆", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().contextNote, std::string(""));
    DOCTEST_CHECK_EQ(row.value().autoNote, 1);
    return true;
})

// createTask 显式传 context_note + auto_note → 落库读回一致
REGISTER_TEST(agent_store_context_note_roundtrip,
              "agent_store: createTask 带 context_note + auto_note → 读回一致", {
    test_support::TestDb tdb("agentstore", "ctxrt");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("记忆任务", "g", "manual", "default", 10, false,
                                   "", "上次运行完成", 0);
    DOCTEST_REQUIRE(taskId.isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().contextNote, std::string("上次运行完成"));
    DOCTEST_CHECK_EQ(row.value().autoNote, 0);
    return true;
})

// updateTask 覆写 context_note + auto_note
REGISTER_TEST(agent_store_context_note_update,
              "agent_store: updateTask 覆写 context_note + auto_note", {
    test_support::TestDb tdb("agentstore", "ctxupd");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("旧记忆", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    // updateTask 带 context_note + auto_note 尾参
    DOCTEST_REQUIRE(store.updateTask(taskId.value(), "旧记忆", "g", "manual", "",
                                     "default", 10, false, "新记忆内容", 1)
                        .isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().contextNote, std::string("新记忆内容"));
    DOCTEST_CHECK_EQ(row.value().autoNote, 1);
    // updateTask 不传尾参 → 默认空串/1（覆盖为默认值）
    DOCTEST_REQUIRE(store.updateTask(taskId.value(), "旧记忆", "g", "manual", "",
                                     "default", 10, false)
                        .isOk());
    auto row2 = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row2.isOk());
    DOCTEST_CHECK_EQ(row2.value().contextNote, std::string(""));
    DOCTEST_CHECK_EQ(row2.value().autoNote, 1);
    return true;
})

// appendContextNote：多次追加顺序累积
REGISTER_TEST(agent_store_append_context_note_accumulates,
              "agent_store: appendContextNote 多次追加顺序累积", {
    test_support::TestDb tdb("agentstore", "appacc");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("累积", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), "第一行。").isOk());
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), "第二行。").isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().contextNote, std::string("第一行。第二行。"));
    return true;
})

// appendContextNote：空串 no-op
REGISTER_TEST(agent_store_append_context_note_empty_noop,
              "agent_store: appendContextNote 空串 no-op", {
    test_support::TestDb tdb("agentstore", "appemp");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("空串", "g", "manual", "default", 10, false,
                                   "", "已有内容", 1);
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), "").isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().contextNote, std::string("已有内容"));
    return true;
})

// appendContextNote：taskId 不存在 → NotFound
REGISTER_TEST(agent_store_append_context_note_not_found,
              "agent_store: appendContextNote 不存在 task → NotFound", {
    test_support::TestDb tdb("agentstore", "appnf");
    module::agent::AgentStore store(tdb.handle());
    auto r = store.appendContextNote(99999, "内容");
    DOCTEST_REQUIRE(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::NotFound);
    return true;
})

// appendContextNote：4KB 滚动丢最旧 + UTF-8 边界不切半
REGISTER_TEST(agent_store_append_context_note_4kb_roll,
              "agent_store: appendContextNote 超 4KB 从头丢最旧至 UTF-8 边界", {
    test_support::TestDb tdb("agentstore", "app4k");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("4KB", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    // 写 4000 个 ASCII 'A'
    std::string bigA(4000, 'A');
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), bigA).isOk());
    // 再追加 100 个 'B'，总长 4100 > 4096 → 丢最旧 4 个 'A'
    std::string bigB(100, 'B');
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), bigB).isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().contextNote.size(), static_cast<size_t>(4096));
    // 内容以 'A' 开头（丢的是最旧的 4 个），以 'B' 结尾
    DOCTEST_CHECK_EQ(row.value().contextNote[0], 'A');
    DOCTEST_CHECK_EQ(row.value().contextNote[4095], 'B');
    return true;
})

// appendContextNote：UTF-8 边界裁剪——中文 3 字节字符不切半
REGISTER_TEST(agent_store_append_context_note_utf8_boundary,
              "agent_store: appendContextNote 超 4KB 裁剪落在 UTF-8 字符边界", {
    test_support::TestDb tdb("agentstore", "apputf");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("UTF8", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    // 写 4090 个 'A'，再加 3 字节中文"中"（E4 B8 AD），总长 4093
    std::string base(4090, 'A');
    base += "中";   // 3 bytes
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), base).isOk());
    // 再加 "国"（E5 9B BD，3 bytes），总长 4096
    DOCTEST_REQUIRE(store.appendContextNote(taskId.value(), "国").isOk());
    auto row = store.getTask(taskId.value());
    DOCTEST_REQUIRE(row.isOk());
    // 总长应 ≤ 4096
    DOCTEST_CHECK(row.value().contextNote.size() <= static_cast<size_t>(4096));
    // 末尾应是 "国" 的 3 字节完整保留
    std::string tail = row.value().contextNote.substr(
        row.value().contextNote.size() - 3);
    DOCTEST_CHECK_EQ(tail, std::string("国"));
    return true;
})
