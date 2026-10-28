// 调度引擎（S4）测试：到期触发 / 未到不触 / 错过三策略 / 时钟回拨不重放 /
// 同规则串行 / 队列满 suppressed / 一次性归档 / 编辑重算 / 投递重试分类记账。
// 每条用例对应一个业务场景并注明防的具体事故（场景 = 什么状态、什么环境、做什么）。
//
// 假时钟驱动：引擎 tick(now) 直接注入 unix 秒，不经真实 ticker；冷启动口径 =
// 首次 tick 前 last_eval_at 为空 → 不补历史。
//
// S5 对齐点：ChannelRegistry 的构造/注册/投递方法名以 channel_registry.h 实际
// 定义为准（见 makeRegistry/FakeChannel）——若签名不同只改这两个 helper。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/result.h"
#include "module_schedule/channel_registry.h"
#include "module_schedule/schedule_engine.h"
#include "module_schedule/schedule_store.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_schedengine_";

core::Result<core::Database> openMigrated(const std::string& tag) {
    auto path = test_support::tempDbPath(kDbPrefix, tag);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto opened = core::Database::open(path);
    if (opened.isFail())
        return core::Result<core::Database>::fail(opened.errorCode(), opened.errorMessage());
    auto migrated = opened.value().migrate();
    if (migrated.isFail())
        return core::Result<core::Database>::fail(migrated.errorCode(), migrated.errorMessage());
    return opened;
}

int64_t countRuns(core::data::SqliteDb& db, const std::string& status = "") {
    auto prep = db.prepare(status.empty()
                               ? "SELECT COUNT(*) AS v FROM schedule_run"
                               : "SELECT COUNT(*) AS v FROM schedule_run WHERE status = '" +
                                     status + "'");
    if (prep.isFail()) return -1;
    auto rows = prep.value().fetchAll();
    if (rows.isFail() || rows.value().empty()) return -1;
    return rows.value()[0].getInt64("v").valueOr(-1);
}

core::Result<std::optional<int64_t>> queryOptInt(core::data::SqliteDb& db,
                                                 const std::string& sql) {
    auto prep = db.prepare(sql);
    if (prep.isFail())
        return core::Result<std::optional<int64_t>>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail())
        return core::Result<std::optional<int64_t>>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty() || rows.value()[0].isNull("v"))
        return core::Result<std::optional<int64_t>>::ok(std::nullopt);
    auto v = rows.value()[0].getInt64("v");
    if (v.isFail())
        return core::Result<std::optional<int64_t>>::fail(v.errorCode(), v.errorMessage());
    return core::Result<std::optional<int64_t>>::ok(v.value());
}

int64_t nextFireOf(core::data::SqliteDb& db, int64_t ruleId) {
    return queryOptInt(db, "SELECT next_fire_at AS v FROM schedule_rule WHERE id = " +
                               std::to_string(ruleId))
        .value()
        .valueOr(-1);
}

// ---- S5 对齐点：Receipt 形状与 ChannelRegistry 方法名以 channel_registry.h 为准 ----

module::schedule::Receipt makeReceipt(bool success, bool retryable, int64_t retryAfter = 0,
                                      const std::string& message = "") {
    module::schedule::Receipt r;
    r.success = success;
    r.retryable = retryable;
    r.retryAfterSec = retryAfter;
    r.httpStatus = 0;
    r.remoteErrcode = 0;
    r.message = message;
    return r;
}

// 可脚本化渠道：按调用次数返回指定序列的 Receipt。
class FakeChannel : public module::schedule::Channel {
public:
    int calls = 0;
    std::vector<module::schedule::Receipt> script;  // 依调用次序返回；越界用最后一个

    core::Result<module::schedule::Receipt> deliver(
        const module::schedule::Delivery& delivery) override {
        (void)delivery;
        if (script.empty())
            return core::Result<module::schedule::Receipt>::ok(makeReceipt(true, false));
        const std::size_t idx = std::min<std::size_t>(calls, script.size() - 1);
        ++calls;
        return core::Result<module::schedule::Receipt>::ok(script[idx]);
    }
};

struct TestChannels {
    module::schedule::ChannelRegistry registry;
    std::shared_ptr<FakeChannel> fake = std::make_shared<FakeChannel>();

    explicit TestChannels(int64_t channelId) {
        // S5 对齐点：注册方法名以实际 ChannelRegistry 为准。
        registry.addChannel(channelId, fake);
    }
};

module::schedule::RuleRecord makeIntervalRule(int64_t everySec, int64_t anchor,
                                              const std::string& actionKind = "remind",
                                              const std::string& missedPolicy = "once",
                                              std::vector<int64_t> channelIds = {1}) {
    module::schedule::RuleRecord r;
    r.name = "interval-rule";
    r.kind = "interval";
    r.specJson = "{\"every_sec\":" + std::to_string(everySec) +
                 ",\"anchor\":" + std::to_string(anchor) + "}";
    r.actionKind = actionKind;
    r.actionJson = actionKind == "run_task" ? "{\"key\":\"test:task\"}" : "{}";
    r.channelIds = std::move(channelIds);
    r.missedPolicy = missedPolicy;
    return r;
}

module::schedule::RuleRecord makeOnceRule(int64_t atUnix, const std::string& missedPolicy = "once") {
    module::schedule::RuleRecord r;
    r.name = "once-rule";
    r.kind = "once";
    r.specJson = "{\"at\":" + std::to_string(atUnix) + "}";
    r.actionKind = "remind";
    r.actionJson = "{}";
    r.channelIds = {1};
    r.missedPolicy = missedPolicy;
    return r;
}

} // namespace

// 防：到期不触发 / 触发后不推进 next_fire_at / last_eval_at 不落库——
// 三者任一失效都会造成提醒整条链路静默失效或重复触发。
REGISTER_TEST(schedule_engine_fires_at_due_time,
              "调度引擎: next_fire_at 到期触发一次, 触发后重算下次并推进 last_eval_at", {
    auto db = openMigrated("due").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(makeIntervalRule(60, 0)).value();
    engine.tick(10);   // 冷启动: 算 next = 60
    engine.tick(60);   // 到期
    CHECK(countRuns(db.db()) == 1);
    CHECK(countRuns(db.db(), "success") == 1);
    CHECK(nextFireOf(db.db(), rule.id) == 120);
    CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                   std::to_string(rule.id)).value().valueOr(-1) == 60);
})

// 防：提前触发——到期判定写成 <= 而非 == 后 next_fire_at 缓存被误当成当前时刻。
REGISTER_TEST(schedule_engine_not_fire_before_due,
              "调度引擎: 未到 next_fire_at 不触发", {
    auto db = openMigrated("notdue").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(makeIntervalRule(60, 0)).value();
    engine.tick(10);
    engine.tick(59);
    CHECK(countRuns(db.db()) == 0);
})

// 防：停机跨越到期点后 skip 策略漏写 missed 记录——运行历史缺口让用户无从
// 得知「错过了几次」。
REGISTER_TEST(schedule_engine_missed_skip_writes_missed_rows,
              "调度引擎: missed_policy=skip 停机跨 3 个到期点, 写 3 条 missed 不执行动作", {
    auto db = openMigrated("skip").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(makeIntervalRule(100, 0, "remind", "skip")).value();
    engine.tick(50);    // 冷启动: last_eval=50, next=100
    engine.tick(400);   // 错过 100/200/300
    CHECK(countRuns(db.db()) == 3);
    CHECK(countRuns(db.db(), "missed") == 3);
    CHECK(channels.fake->calls == 0);  // skip 不执行动作
    CHECK(nextFireOf(db.db(), rule.id) == 500);
})

// 防：once 策略多补（把每个错过点都补一遍）或漏掉「原定时刻」标注——
// 用户收到补跑提醒却不知道是补哪次的。
REGISTER_TEST(schedule_engine_missed_once_single_catchup,
              "调度引擎: missed_policy=once 只补最早一个错过点, 摘要标注原定时刻", {
    auto db = openMigrated("once").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(makeIntervalRule(100, 0, "remind", "once")).value();
    engine.tick(50);
    engine.tick(400);   // 错过 100/200/300 → 只补 100
    CHECK(countRuns(db.db()) == 1);
    auto rows = db.db().prepare("SELECT planned_at, result_summary FROM schedule_run")
                    .value().fetchAll().value();
    REQUIRE(!rows.empty());
    CHECK(rows[0].getInt64("planned_at").valueOr(-1) == 100);
    CHECK(rows[0].getString("result_summary").valueOr("").find("补跑") != std::string::npos);
    CHECK(channels.fake->calls == 1);
})

// 防：all 策略少补——补跑上限误写成窗口合并导致停机长时动作缺失。
REGISTER_TEST(schedule_engine_missed_all_fires_each,
              "调度引擎: missed_policy=all 停机跨 3 个到期点补 3 次", {
    auto db = openMigrated("all").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(makeIntervalRule(100, 0, "remind", "all")).value();
    engine.tick(50);
    engine.tick(400);
    CHECK(countRuns(db.db()) == 3);
    CHECK(countRuns(db.db(), "success") == 3);
    CHECK(channels.fake->calls == 3);
})

// 防：时钟回拨/NTP 校准把已过窗口重放——同一提醒弹两遍（§4.8 回拨只更新不重放）。
REGISTER_TEST(schedule_engine_clock_rollback_no_replay,
              "调度引擎: 时钟回拨只刷新 last_eval_at 不重放历史触发", {
    auto db = openMigrated("rollback").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(makeIntervalRule(100, 0, "remind", "all")).value();
    engine.tick(50);
    engine.tick(100);   // 正常触发一次
    CHECK(countRuns(db.db()) == 1);
    engine.tick(90);    // 回拨到触发前: 不重放
    CHECK(countRuns(db.db()) == 1);
    engine.tick(200);   // 按新时间续跑: 100 已被回拨窗口排除, 200 到期
    CHECK(countRuns(db.db()) == 2);
    CHECK(nextFireOf(db.db(), rule.id) == 300);
})

// 防：同规则并发执行（running 唯一被破坏）——第二条到期点抢进重动作，
// 内部任务/agent 双重执行产生副作用翻倍。
REGISTER_TEST(schedule_engine_same_rule_serialized,
              "调度引擎: run_task 异步未完成期间第二条到期点入队不并发", {
    auto db = openMigrated("serial").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    int active = 0;
    int maxActive = 0;
    engine.taskRegistry().registerTask(
        "test:task", [&](const module::schedule::TaskInvoke&) {
            ++active;
            maxActive = std::max(maxActive, active);
            module::schedule::TaskOutcome o;
            o.status = "async";  // 任务未完成: 保持 running 占用
            return core::Result<module::schedule::TaskOutcome>::ok(o);
        });

    auto rule = store.createRule(makeIntervalRule(10, 0, "run_task", "once")).value();
    engine.tick(5);
    engine.tick(10);   // 触发 1 (async, running)
    engine.tick(20);   // 到期点 2: 同规则 running → 入队 pending
    CHECK(countRuns(db.db(), "running") == 1);
    CHECK(countRuns(db.db(), "pending") == 1);
    CHECK(maxActive == 1);

    module::schedule::RunOutcome done;
    done.status = "success";
    REQUIRE(engine.completeRun(
        queryOptInt(db.db(), "SELECT id AS v FROM schedule_run WHERE status='running'")
            .value().valueOr(0), done, 21).isOk());
    engine.tick(21);   // drain 启动 pending 的到期点 2
    CHECK(countRuns(db.db(), "running") == 1);  // 第二个任务在跑
    CHECK(maxActive == 1);  // 全程无并发
    CHECK(countRuns(db.db(), "pending") == 0);
})

// 防：队列满静默丢弃（§4.7 必须记 suppressed）——触发无声消失、运行历史无痕迹。
REGISTER_TEST(schedule_engine_queue_full_suppressed,
              "调度引擎: 待执行队列满后 skip 并记 suppressed 不静默丢弃", {
    auto db = openMigrated("qfull").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    engine.taskRegistry().registerTask(
        "test:task", [](const module::schedule::TaskInvoke&) {
            module::schedule::TaskOutcome o;
            o.status = "async";
            return core::Result<module::schedule::TaskOutcome>::ok(o);
        });

    auto rule = store.createRule(makeIntervalRule(1, 0, "run_task", "all")).value();
    engine.tick(0);
    engine.tick(1);   // 触发 1: async 占住 running
    for (int64_t t = 2; t <= 300; ++t) engine.tick(t);  // 持续到期 → 排队到满
    CHECK(countRuns(db.db(), "suppressed") >= 1);
    CHECK(countRuns(db.db(), "pending") <=
          static_cast<int64_t>(module::schedule::ScheduleEngine::kMaxPendingFires));
    // 队列满时规则必须还在被调度（next_fire_at 继续推进, 不卡死）
    CHECK(nextFireOf(db.db(), rule.id) > 300);
})

// 防：一次性规则触发后未归档, 重启/后续 tick 重复触发（§4.4/验收 4）。
REGISTER_TEST(schedule_engine_once_rule_archived_no_refire,
              "调度引擎: 一次性规则触发后归档停用, 重启引擎也不再触发", {
    auto db = openMigrated("archive").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    {
        module::schedule::ScheduleEngine engine(db.db(), channels.registry);
        (void)store.createRule(makeOnceRule(100)).value();
        engine.tick(50);
        engine.tick(100);   // 触发一次
        CHECK(countRuns(db.db()) == 1);
        engine.tick(300);
        CHECK(countRuns(db.db()) == 1);
    }
    CHECK(queryOptInt(db.db(), "SELECT archived_at AS v FROM schedule_rule").value()
              .has_value());
    CHECK(queryOptInt(db.db(), "SELECT enabled AS v FROM schedule_rule").value().valueOr(1) == 0);
    // 模拟重启: 新引擎同库再 tick, 仍不触发
    module::schedule::ScheduleEngine engine2(db.db(), channels.registry);
    engine2.tick(400);
    CHECK(countRuns(db.db()) == 1);
})

// 防：编辑规则后 next_fire_at 用旧缓存（改了时间却不生效）或 last_eval_at
// 不刷新把历史窗口放大成误补跑（§4.4 编辑事务内重算）。
REGISTER_TEST(schedule_engine_edit_recomputes_next_fire,
              "调度引擎: recomputeNextFire 事务内重算 next_fire_at 并刷新 last_eval_at", {
    auto db = openMigrated("edit").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(1);
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(makeIntervalRule(60, 1000, "remind", "once")).value();
    engine.tick(990);   // 冷启动: next = 1000
    CHECK(nextFireOf(db.db(), rule.id) == 1000);

    // 编辑: 间隔 60 → 120, 事务内重算
    auto edited = rule;
    edited.specJson = "{\"every_sec\":120,\"anchor\":1000}";
    REQUIRE(store.updateRule(edited).isOk());
    REQUIRE(engine.recomputeNextFire(rule.id, 1010).isOk());
    CHECK(nextFireOf(db.db(), rule.id) == 1120);
    CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                   std::to_string(rule.id)).value().valueOr(-1) == 1010);
    engine.tick(1120);  // 新时刻生效
    CHECK(countRuns(db.db()) == 1);
})

// 防：投递重试阶梯（0s/30s/300s）与每次尝试一行记账失效——渠道失败后既不重试
// 也无账可查（§4.9/验收 7）；run 保持 success 与投递失败可分辨（§4.5）。
REGISTER_TEST(schedule_delivery_retry_ladder_and_bookkeeping,
              "投递编排: 可重试失败按 0s/30s/300s 重试, 3 次尝试 3 行记录, run 仍 success", {
    auto db = openMigrated("retry").value();
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(7);
    channels.fake->script = {makeReceipt(false, true, 0, "http 500"),
                             makeReceipt(false, true, 0, "timeout"),
                             makeReceipt(false, true, 0, "errcode 45009")};
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(makeOnceRule(100)).value();
    auto edited = rule;
    edited.channelIds = {7};
    (void)store.updateRule(edited);
    engine.tick(50);
    engine.tick(100);   // 尝试 1 失败 → 重试队列
    CHECK(countRuns(db.db()) == 1);
    CHECK(countRuns(db.db(), "success") == 1);  // 渠道失败不改 run 的 success

    engine.tick(100 + 30);   // 尝试 2 (planned+30)
    engine.tick(100 + 300);  // 尝试 3 (planned+300) 仍失败 → 终态 failed
    auto rows = db.db().prepare(
                            "SELECT attempt, status, next_retry_at FROM schedule_delivery"
                            " ORDER BY attempt").value().fetchAll().value();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].getInt64("attempt").valueOr(-1) == 1);
    CHECK(rows[0].getString("status").valueOr("") == "retry");
    CHECK(rows[0].getInt64("next_retry_at").valueOr(-1) == 130);
    CHECK(rows[1].getInt64("next_retry_at").valueOr(-1) == 400);
    CHECK(rows[2].getString("status").valueOr("") == "failed");
    CHECK(channels.fake->calls == 3);
})

// 防：永久失败被当可重试反复轰炸, 渠道不停用——93000 类错误按 §4.9 应立即
// 停用渠道不重试。
REGISTER_TEST(schedule_delivery_permanent_failure_disables_channel,
              "投递编排: 永久失败不重试并停用渠道", {
    auto db = openMigrated("perm").value();
    module::schedule::ScheduleStore store(db.db());

    // 渠道配置行 id 同时用作注册表渠道 id（S5 对齐点: addChannel 签名以实际为准）
    module::schedule::ChannelRecord ch;
    ch.kind = "wecom";
    ch.name = "wecom-test";
    auto created = store.createChannel(ch).value();

    TestChannels channels(created.id);
    channels.fake->script = {makeReceipt(false, false, 0, "errcode 93000")};
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(makeIntervalRule(100, 0, "remind", "once", {created.id})).value();
    engine.tick(50);
    engine.tick(100);
    CHECK(channels.fake->calls == 1);
    engine.tick(100 + 300);
    CHECK(channels.fake->calls == 1);  // 不重试
    auto dRows = db.db().prepare(
                             "SELECT status FROM schedule_delivery").value().fetchAll().value();
    REQUIRE(!dRows.empty());
    CHECK(dRows[0].getString("status").valueOr("") == "dropped");
    // 渠道配置行已停用（§4.9 93000 类：不重试、enabled=0）
    auto enabled = queryOptInt(db.db(), "SELECT enabled AS v FROM schedule_channel");
    CHECK(enabled.value().valueOr(1) == 0);
})
