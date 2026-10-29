// 调度引擎（S4）测试：到期触发 / 未到不触 / 错过三策略 / 时钟回拨不重放 /
// 同规则串行 / 队列满 suppressed / 一次性归档 / 编辑重算 / 投递重试分类记账。
// 每条用例对应一个业务场景并注明防的具体事故（场景 = 什么状态、什么环境、做什么）。
//
// 假时钟驱动：引擎 tick(now) 直接注入 unix 秒，不经真实 ticker；冷启动口径 =
// 首次 tick 前 last_eval_at 为空 → 不补历史。
//
// S5 对齐点：真实 ChannelRegistry 键 = 渠道 kind 字符串，注册走
// registerChannel(unique_ptr)（接管所有权）、投递走 deliver({kind}, Delivery)、
// 无 addChannel/deliverLocal。引擎侧经 schedule_channel 配置行 id → kind 桥接，
// 故 TestChannels 同时建配置行（见下方 helper）——签名再变只改这些 helper。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/result.h"
#include "core/ticker.h"
#include "module_schedule/channel_registry.h"
#include "module_schedule/schedule_engine.h"
#include "module_schedule/schedule_module.h"
#include "module_schedule/schedule_store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_schedengine_";

// 按值返回（Database 可移动）。失败属测试脚手架故障，直接 exit(1) 让用例红，
// 不走 Result（防调用侧误拷贝 Database——其拷贝已删除）。
core::Database openMigrated(const std::string& tag) {
    auto path = test_support::tempDbPath(kDbPrefix, tag);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto opened = core::Database::open(path);
    if (opened.isFail()) {
        std::fprintf(stderr, "openMigrated(%s): %s\n", tag.c_str(),
                     opened.errorMessage().c_str());
        std::exit(1);
    }
    auto migrated = opened.value().migrate();
    if (migrated.isFail()) {
        std::fprintf(stderr, "openMigrated(%s) migrate: %s\n", tag.c_str(),
                     migrated.errorMessage().c_str());
        std::exit(1);
    }
    return std::move(opened.value());
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
        .value().value_or(-1);
}

// ---- S5 对齐点：Receipt 形状与 ChannelRegistry 方法名以 channel_registry.h 为准 ----

module::schedule::Receipt makeReceipt(const std::string& status,
                                      const std::string& detail = "") {
    module::schedule::Receipt r;
    r.status = status;
    r.detail = detail;
    return r;
}

// 可脚本化渠道：按调用次数返回指定序列的 Receipt。id() 必须等于配置行 kind，
// 引擎桥接（配置行 id → kind → 注册表键）才能命中本渠道。
class FakeChannel : public module::schedule::Channel {
public:
    explicit FakeChannel(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }

    int calls = 0;
    std::vector<module::schedule::Receipt> script;  // 依调用次序返回；越界用最后一个
    std::function<void()> hook;  // deliver 入口钩子（calls 已自增）：供用例从渠道内
                                 // 再入 runner 复现并发满；空 = 无行为（存量用例不受影响）

    core::Result<module::schedule::Receipt> deliver(
        const module::schedule::Delivery& delivery) override {
        (void)delivery;
        ++calls;  // 每次投递都计数（含 script 空的成功投递——断言把 calls 当投递次数）
        if (hook) hook();
        if (script.empty())
            return core::Result<module::schedule::Receipt>::ok(makeReceipt("ok"));
        const std::size_t idx = std::min<std::size_t>(calls - 1, script.size() - 1);
        return core::Result<module::schedule::Receipt>::ok(script[idx]);
    }

private:
    std::string id_;
};

struct TestChannels {
    module::schedule::ChannelRegistry registry;
    FakeChannel* fake = nullptr;  // registry 接管所有权；raw 指针仅作观察
    int64_t channelRowId = 0;     // schedule_channel 配置行 id（规则 channel_ids 用它）

    explicit TestChannels(module::schedule::ScheduleStore& store,
                          const std::string& kind = "fake_remind") {
        module::schedule::ChannelRecord ch;
        ch.kind = kind;
        ch.name = "test-" + kind;
        auto created = store.createChannel(ch).value();
        channelRowId = created.id;
        auto f = std::make_unique<FakeChannel>(kind);
        fake = f.get();
        if (!registry.registerChannel(std::move(f)).isOk()) {
            std::fprintf(stderr, "TestChannels: registerChannel(%s) failed\n",
                         kind.c_str());
            std::exit(1);
        }
    }
};

module::schedule::RuleRecord makeIntervalRule(int64_t everySec, int64_t anchor,
                                              const std::string& actionKind = "remind",
                                              const std::string& missedPolicy = "once",
                                              std::vector<int64_t> channelIds = {}) {
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

module::schedule::RuleRecord makeOnceRule(int64_t atUnix, const std::string& missedPolicy = "once",
                                          std::vector<int64_t> channelIds = {},
                                          const std::string& actionKind = "remind") {
    module::schedule::RuleRecord r;
    r.name = "once-rule";
    r.kind = "once";
    r.specJson = "{\"at\":" + std::to_string(atUnix) + "}";
    r.actionKind = actionKind;
    r.actionJson = actionKind == "run_task" ? "{\"key\":\"test:task\"}" : "{}";
    r.channelIds = std::move(channelIds);
    r.missedPolicy = missedPolicy;
    return r;
}

} // namespace

// 防：到期不触发 / 触发后不推进 next_fire_at / last_eval_at 不落库——
// 三者任一失效都会造成提醒整条链路静默失效或重复触发。
REGISTER_TEST(schedule_engine_fires_at_due_time,
              "调度引擎: next_fire_at 到期触发一次, 触发后重算下次并推进 last_eval_at", {
    auto db = openMigrated("due");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(60, 0, "remind", "once", {channels.channelRowId})).value();
    engine.tick(10);   // 冷启动: 算 next = 60
    engine.tick(60);   // 到期
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    DOCTEST_CHECK(countRuns(db.db(), "success") == 1);
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) == 120);
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                   std::to_string(rule.id)).value().value_or(-1) == 60);
    return true;
})

// 防：提前触发——到期判定写成 <= 而非 == 后 next_fire_at 缓存被误当成当前时刻。
REGISTER_TEST(schedule_engine_not_fire_before_due,
              "调度引擎: 未到 next_fire_at 不触发", {
    auto db = openMigrated("notdue");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(
        makeIntervalRule(60, 0, "remind", "once", {channels.channelRowId})).value();
    engine.tick(10);
    engine.tick(59);
    DOCTEST_CHECK(countRuns(db.db()) == 0);
    return true;
})

// 防：停机跨越到期点后 skip 策略漏写 missed 记录——运行历史缺口让用户无从
// 得知「错过了几次」。
REGISTER_TEST(schedule_engine_missed_skip_writes_missed_rows,
              "调度引擎: missed_policy=skip 停机跨 3 个到期点, 写 3 条 missed 不执行动作", {
    auto db = openMigrated("skip");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(100, 0, "remind", "skip", {channels.channelRowId})).value();
    engine.tick(50);    // 冷启动: last_eval=50, next=100
    engine.tick(400);   // 错过 100/200/300
    DOCTEST_CHECK(countRuns(db.db()) == 3);
    DOCTEST_CHECK(countRuns(db.db(), "missed") == 3);
    DOCTEST_CHECK(channels.fake->calls == 0);  // skip 不执行动作
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) == 500);
    return true;
})

// 防：once 策略多补（把每个错过点都补一遍）或漏掉「原定时刻」标注——
// 用户收到补跑提醒却不知道是补哪次的。
REGISTER_TEST(schedule_engine_missed_once_single_catchup,
              "调度引擎: missed_policy=once 只补最早一个错过点, 摘要标注原定时刻", {
    auto db = openMigrated("once");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(
        makeIntervalRule(100, 0, "remind", "once", {channels.channelRowId})).value();
    engine.tick(50);
    engine.tick(400);   // 错过 100/200/300 → 只补 100
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    auto rows = db.db().prepare("SELECT planned_at, result_summary FROM schedule_run")
                    .value().fetchAll().value();
    DOCTEST_REQUIRE(!rows.empty());
    DOCTEST_CHECK(rows[0].getInt64("planned_at").valueOr(-1) == 100);
    DOCTEST_CHECK(rows[0].getString("result_summary").valueOr("").find("补跑") != std::string::npos);
    DOCTEST_CHECK(channels.fake->calls == 1);
    return true;
})

// 防：all 策略少补——补跑上限误写成窗口合并导致停机长时动作缺失。
REGISTER_TEST(schedule_engine_missed_all_fires_each,
              "调度引擎: missed_policy=all 停机跨 3 个到期点补 3 次", {
    auto db = openMigrated("all");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(
        makeIntervalRule(100, 0, "remind", "all", {channels.channelRowId})).value();
    engine.tick(50);
    engine.tick(400);
    DOCTEST_CHECK(countRuns(db.db()) == 3);
    DOCTEST_CHECK(countRuns(db.db(), "success") == 3);
    DOCTEST_CHECK(channels.fake->calls == 3);
    return true;
})

// 防：时钟回拨/NTP 校准把已过窗口重放——同一提醒弹两遍（§4.8 回拨只更新不重放）。
REGISTER_TEST(schedule_engine_clock_rollback_no_replay,
              "调度引擎: 时钟回拨只刷新 last_eval_at 不重放历史触发", {
    auto db = openMigrated("rollback");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(100, 0, "remind", "all", {channels.channelRowId})).value();
    engine.tick(50);
    engine.tick(100);   // 正常触发一次
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    engine.tick(90);    // 回拨到触发前: 不重放
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    engine.tick(200);   // 按新时间续跑: 100 已被回拨窗口排除, 200 到期
    DOCTEST_CHECK(countRuns(db.db()) == 2);
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) == 300);
    return true;
})

// 防：同规则并发执行（running 唯一被破坏）——第二条到期点抢进重动作，
// 内部任务/agent 双重执行产生副作用翻倍。
REGISTER_TEST(schedule_engine_same_rule_serialized,
              "调度引擎: run_task 异步未完成期间第二条到期点入队不并发", {
    auto db = openMigrated("serial");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
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
    DOCTEST_CHECK(countRuns(db.db(), "running") == 1);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 1);
    DOCTEST_CHECK(maxActive == 1);

    module::schedule::RunOutcome done;
    done.status = "success";
    DOCTEST_REQUIRE(engine.completeRun(
        queryOptInt(db.db(), "SELECT id AS v FROM schedule_run WHERE status='running'")
            .value().value_or(0), done, 21).isOk());
    --active;  // 异步任务 1 的真实结束点 = completeRun 回调；不递减则 maxActive
               // 退化成「任务被调用次数」，第二个任务启动后必为 2，量不出并发
    engine.tick(21);   // drain 启动 pending 的到期点 2
    DOCTEST_CHECK(countRuns(db.db(), "running") == 1);  // 第二个任务在跑
    DOCTEST_CHECK(maxActive == 1);  // 全程无并发
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 0);
    return true;
})

// 防：队列满静默丢弃（§4.7 必须记 suppressed）——触发无声消失、运行历史无痕迹。
REGISTER_TEST(schedule_engine_queue_full_suppressed,
              "调度引擎: 待执行队列满后 skip 并记 suppressed 不静默丢弃", {
    auto db = openMigrated("qfull");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
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
    DOCTEST_CHECK(countRuns(db.db(), "suppressed") >= 1);
    DOCTEST_CHECK(countRuns(db.db(), "pending") <=
          static_cast<int64_t>(module::schedule::ScheduleEngine::kMaxPendingFires));
    // 队列满时规则必须还在被调度（next_fire_at 继续推进, 不卡死）
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) > 300);
    return true;
})

// 防：一次性规则触发后未归档, 重启/后续 tick 重复触发（§4.4/验收 4）。
REGISTER_TEST(schedule_engine_once_rule_archived_no_refire,
              "调度引擎: 一次性规则触发后归档停用, 重启引擎也不再触发", {
    auto db = openMigrated("archive");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    {
        module::schedule::ScheduleEngine engine(db.db(), channels.registry);
        (void)store.createRule(makeOnceRule(100, "once", {channels.channelRowId})).value();
        engine.tick(50);
        engine.tick(100);   // 触发一次
        DOCTEST_CHECK(countRuns(db.db()) == 1);
        engine.tick(300);
        DOCTEST_CHECK(countRuns(db.db()) == 1);
    }
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT archived_at AS v FROM schedule_rule").value()
              .has_value());
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT enabled AS v FROM schedule_rule").value().value_or(1) == 0);
    // 模拟重启: 新引擎同库再 tick, 仍不触发
    module::schedule::ScheduleEngine engine2(db.db(), channels.registry);
    engine2.tick(400);
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    return true;
})

// 防：编辑规则后 next_fire_at 用旧缓存（改了时间却不生效）或 last_eval_at
// 不刷新把历史窗口放大成误补跑（§4.4 编辑事务内重算）。
REGISTER_TEST(schedule_engine_edit_recomputes_next_fire,
              "调度引擎: recomputeNextFire 事务内重算 next_fire_at 并刷新 last_eval_at", {
    auto db = openMigrated("edit");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(60, 1000, "remind", "once", {channels.channelRowId})).value();
    engine.tick(990);   // 冷启动: next = 1000
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) == 1000);

    // 编辑: 间隔 60 → 120, 事务内重算
    auto edited = rule;
    edited.specJson = "{\"every_sec\":120,\"anchor\":1000}";
    DOCTEST_REQUIRE(store.updateRule(edited).isOk());
    DOCTEST_REQUIRE(engine.recomputeNextFire(rule.id, 1010).isOk());
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) == 1120);
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                   std::to_string(rule.id)).value().value_or(-1) == 1010);
    engine.tick(1120);  // 新时刻生效
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    return true;
})

// 防：投递重试阶梯（0s/30s/300s）与每次尝试一行记账失效——渠道失败后既不重试
// 也无账可查（§4.9/验收 7）；run 保持 success 与投递失败可分辨（§4.5）。
REGISTER_TEST(schedule_delivery_retry_ladder_and_bookkeeping,
              "投递编排: 可重试失败按 0s/30s/300s 重试, 3 次尝试 3 行记录, run 仍 success", {
    auto db = openMigrated("retry");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_retry");
    channels.fake->script = {makeReceipt("retryable:http_5xx"),
                             makeReceipt("retryable:network_error"),
                             makeReceipt("retryable:errcode_45009")};
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(makeOnceRule(100, "once", {channels.channelRowId})).value();
    auto edited = rule;
    edited.channelIds = {channels.channelRowId};
    (void)store.updateRule(edited);
    engine.tick(50);
    engine.tick(100);   // 尝试 1 失败 → 重试队列
    DOCTEST_CHECK(countRuns(db.db()) == 1);
    DOCTEST_CHECK(countRuns(db.db(), "success") == 1);  // 渠道失败不改 run 的 success

    engine.tick(100 + 30);   // 尝试 2 (planned+30)
    engine.tick(100 + 300);  // 尝试 3 (planned+300) 仍失败 → 终态 failed
    auto rows = db.db().prepare(
                            "SELECT attempt, status, next_retry_at FROM schedule_delivery"
                            " ORDER BY attempt").value().fetchAll().value();
    DOCTEST_REQUIRE(rows.size() == 3);
    DOCTEST_CHECK(rows[0].getInt64("attempt").valueOr(-1) == 1);
    DOCTEST_CHECK(rows[0].getString("status").valueOr("") == "retry");
    DOCTEST_CHECK(rows[0].getInt64("next_retry_at").valueOr(-1) == 130);
    DOCTEST_CHECK(rows[1].getInt64("next_retry_at").valueOr(-1) == 400);
    DOCTEST_CHECK(rows[2].getString("status").valueOr("") == "failed");
    DOCTEST_CHECK(channels.fake->calls == 3);
    return true;
})

// 防：渠道并发满（httpInFlight≥2，§4.7）被当真实尝试虚耗配额——slotBusy 必须
// 置 retryWithoutCharge 且不落记账行，引擎 drain 侧据此跳过 attempt 递增；若
// 回归为计次，饱和期三次 slotBusy 烧光 3 次配额，渠道恢复后也无法再投。
// 在飞计数在 dispatchOnce 每个出口都回落、本层单线程无异步完成回调，入口计数
// 到 2 的唯一可达路径 = 渠道 deliver() 内经 hook 再入两层（未来真异步渠道
// 在飞跨 tick 时即等价此态）。
REGISTER_TEST(schedule_delivery_slot_busy_no_charge,
              "投递编排: 渠道并发满 → retryWithoutCharge 置位且不落 delivery 记账行", {
    auto db = openMigrated("slotbusy");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_busy");
    module::schedule::InternalTaskRegistry tasks;
    module::schedule::ActionRunner runner(store, channels.registry, tasks);

    module::schedule::Delivery d;
    d.title = "slotbusy-probe";
    d.body = "并发满探测";
    d.runId = 1;

    std::optional<module::schedule::RetryOutcome> slotBusy;
    bool inner1Ok = false;  // 断言宏内含 return false，只能用在 bool 测试体；
                            // hook（void）只记录结果，判定全部放回测试体
    channels.fake->hook = [&]() {
        if (channels.fake->calls == 1) {
            // 第 1 层再入：入口计数 1 < 2 放行，计数推到 2
            auto inner1 = runner.dispatchOnce(channels.channelRowId, d, 1, 1, 100, 500);
            inner1Ok = inner1.isOk() && inner1.value().success;
        } else if (channels.fake->calls == 2) {
            // 第 2 层再入：入口计数 2 ≥ 2 → 命中 slotBusy 门
            auto inner2 = runner.dispatchOnce(channels.channelRowId, d, 1, 1, 100, 500);
            if (inner2.isOk()) slotBusy = inner2.value();
        }
    };

    auto outer = runner.dispatchOnce(channels.channelRowId, d, 1, 1, 100, 500);
    DOCTEST_REQUIRE(outer.isOk());
    DOCTEST_REQUIRE(outer.value().success);
    DOCTEST_REQUIRE(inner1Ok);  // 第 1 层再入放行且成功

    DOCTEST_REQUIRE(slotBusy.has_value());
    DOCTEST_CHECK(slotBusy->retryWithoutCharge);
    DOCTEST_CHECK_FALSE(slotBusy->terminal);
    DOCTEST_CHECK(slotBusy->nextRetryAt.value_or(-1) == 500);  // 0 秒后重试 = now
    DOCTEST_CHECK_FALSE(slotBusy->success);
    DOCTEST_CHECK(slotBusy->error == "渠道并发已满，稍后重试");
    // 不记账：库中只有外层与第 1 层再入两行（均 success），slotBusy 那次零行——
    // 记账行数变 3 即回归（gate 失效或 slotBusy 误记账）。
    auto rows = db.db().prepare(
                            "SELECT status FROM schedule_delivery ORDER BY id")
                    .value().fetchAll().value();
    DOCTEST_REQUIRE(rows.size() == 2);
    DOCTEST_CHECK(rows[0].getString("status").valueOr("") == "success");
    DOCTEST_CHECK(rows[1].getString("status").valueOr("") == "success");
    return true;
})

// 防：永久失败被当可重试反复轰炸, 渠道不停用——93000 类错误按 §4.9 应立即
// 停用渠道不重试。
REGISTER_TEST(schedule_delivery_permanent_failure_disables_channel,
              "投递编排: 永久失败不重试并停用渠道", {
    auto db = openMigrated("perm");
    module::schedule::ScheduleStore store(db.db());

    // 真实注册表键 = kind 字符串（S5 对齐点）：TestChannels 建 kind="wecom" 配置行
    // 并注册同 id 假渠道，规则 channel_ids 用配置行 id 经引擎桥接命中。
    TestChannels channels(store, "wecom");
    channels.fake->script = {makeReceipt("permanent:errcode_93000")};
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(
        makeIntervalRule(100, 0, "remind", "once", {channels.channelRowId})).value();
    engine.tick(50);
    engine.tick(100);
    DOCTEST_CHECK(channels.fake->calls == 1);
    engine.tick(100 + 300);
    DOCTEST_CHECK(channels.fake->calls == 1);  // 不重试
    auto dRows = db.db().prepare(
                             "SELECT status FROM schedule_delivery").value().fetchAll().value();
    DOCTEST_REQUIRE(!dRows.empty());
    DOCTEST_CHECK(dRows[0].getString("status").valueOr("") == "dropped");
    // 渠道配置行已停用（§4.9 93000 类：不重试、enabled=0）
    auto enabled = queryOptInt(db.db(), "SELECT enabled AS v FROM schedule_channel");
    DOCTEST_CHECK(enabled.value().value_or(1) == 0);
    return true;
})

// 防：多规则同刻叠加触发把渠道外发打出官方限频（调研 §10.5：全局一桶 18 条/分钟，
// 防多 schedule_channel 行叠加超频）——超出部分按并发冲突同路径排队（pending 行
// 留痕不丢），令牌随 tick 回填后放行，节奏压回 18 条/分钟。
REGISTER_TEST(schedule_engine_global_bucket_paces_deliveries,
              "调度引擎: 全局投递桶把同刻 25 条触发压到 18 条/分钟, 余量排队回填后放行", {
    auto db = openMigrated("globalbucket");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    for (int i = 0; i < 25; ++i) {
        (void)store.createRule(makeOnceRule(100, "once", {channels.channelRowId})).value();
    }
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    engine.tick(50);    // 冷启动: 25 条 next=100
    engine.tick(100);   // 同刻到期: 桶容量 18 → 18 执行 + 7 排队
    DOCTEST_CHECK(countRuns(db.db(), "success") == 18);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 7);
    DOCTEST_CHECK(channels.fake->calls == 18);

    engine.tick(103);   // 3s 回填 0.9 个令牌 < 1: 不放行
    DOCTEST_CHECK(countRuns(db.db(), "success") == 18);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 7);

    engine.tick(110);   // 累计回填 3 个: 放行 3 条
    DOCTEST_CHECK(countRuns(db.db(), "success") == 21);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 4);

    engine.tick(200);   // 回填充足: 余量清空, 无丢失
    DOCTEST_CHECK(countRuns(db.db(), "success") == 25);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 0);
    DOCTEST_CHECK(channels.fake->calls == 25);
    return true;
})

// 防：全局桶误伤静默内部任务（run_task 不产生渠道外发）——桶耗尽时 run_task
// 仍必须立即执行，不允许被投递限流连坐排队。
REGISTER_TEST(schedule_engine_global_bucket_spares_internal_tasks,
              "调度引擎: 全局桶耗尽时 run_task 仍立即执行不连坐", {
    auto db = openMigrated("bucketspare");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    int taskCalls = 0;
    engine.taskRegistry().registerTask(
        "test:task", [&](const module::schedule::TaskInvoke&) {
            ++taskCalls;
            module::schedule::TaskOutcome o;
            o.status = "success";
            return core::Result<module::schedule::TaskOutcome>::ok(o);
        });

    for (int i = 0; i < 18; ++i) {  // 恰好耗尽全局桶（容量 18）
        (void)store.createRule(
            makeOnceRule(100, "once", {channels.channelRowId})).value();
    }
    (void)store.createRule(makeOnceRule(100, "once", {}, "run_task")).value();

    engine.tick(50);
    engine.tick(100);   // 18 条 remind 吃满桶; run_task 不占桶仍执行
    DOCTEST_CHECK(taskCalls == 1);
    DOCTEST_CHECK(countRuns(db.db(), "success") == 19);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 0);
    DOCTEST_CHECK(channels.fake->calls == 18);
    return true;
})

// 防：禁用规则不推进 last_eval_at——禁用 N 天再启用，once 策略补发陈旧动作
// （审计 P1-1：停用口径须与暂停一致，解除后从 now 干净续跑）。
REGISTER_TEST(schedule_engine_disabled_once_no_stale_catchup,
              "调度引擎: 禁用期间 last_eval_at 逐 tick 推进, 重启用不补发陈旧 once 动作", {
    auto db = openMigrated("disabledonce");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeOnceRule(100, "once", {channels.channelRowId})).value();
    engine.tick(50);   // 冷启动: last_eval=50, next=100
    // 启停翻转与 UI setRuleEnabled 同口径：getRule 新读当前行再翻 enabled，
    // 不用陈旧快照整行回写。
    auto disabled = store.getRule(rule.id).value();
    disabled.enabled = false;
    DOCTEST_REQUIRE(store.updateRule(disabled).isOk());
    engine.tick(60);
    engine.tick(5000);   // 禁用跨越触发点（模拟 N 天）
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                    std::to_string(rule.id)).value().value_or(-1) == 5000);
    auto reenabled = store.getRule(rule.id).value();
    reenabled.enabled = true;
    DOCTEST_REQUIRE(store.updateRule(reenabled).isOk());
    engine.tick(5001);
    DOCTEST_CHECK(countRuns(db.db()) == 0);
    DOCTEST_CHECK(channels.fake->calls == 0);
    return true;
})

// 防：all 策略把禁用期间的到期点当成停机错过补跑——重启用瞬间最多 100 连发。
REGISTER_TEST(schedule_engine_disabled_all_clean_resume,
              "调度引擎: 禁用 N 秒再启用, all 策略不补发禁用期间到期点, 从 now 干净续跑", {
    auto db = openMigrated("disabledall");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(100, 0, "remind", "all", {channels.channelRowId})).value();
    engine.tick(50);   // 冷启动: last_eval=50, next=100
    auto disabled = store.getRule(rule.id).value();
    disabled.enabled = false;
    DOCTEST_REQUIRE(store.updateRule(disabled).isOk());
    engine.tick(60);
    engine.tick(10000);   // 禁用期间错过约 95 个到期点
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                    std::to_string(rule.id)).value().value_or(-1) == 10000);
    auto reenabled = store.getRule(rule.id).value();
    reenabled.enabled = true;
    DOCTEST_REQUIRE(store.updateRule(reenabled).isOk());
    engine.tick(10001);
    DOCTEST_CHECK(countRuns(db.db()) == 0);
    DOCTEST_CHECK(countRuns(db.db(), "missed") == 0);
    DOCTEST_CHECK(channels.fake->calls == 0);
    engine.tick(10100);   // 禁用期间重算的 next_fire_at 正常触发一次
    DOCTEST_CHECK(countRuns(db.db(), "success") == 1);
    DOCTEST_CHECK(channels.fake->calls == 1);
    return true;
})

// 防：skip 策略把禁用期间逐点写 missed——重启用后历史被 100 条 missed 行刷满。
REGISTER_TEST(schedule_engine_disabled_skip_no_missed_spam,
              "调度引擎: 禁用跨越多个到期点再启用, skip 策略不写 missed 记录", {
    auto db = openMigrated("disabledskip");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    (void)store.createRule(
        makeIntervalRule(100, 0, "remind", "skip", {channels.channelRowId})).value();
    engine.tick(50);
    auto disabled = store.listRules().value().front();
    disabled.enabled = false;
    DOCTEST_REQUIRE(store.updateRule(disabled).isOk());
    engine.tick(60);
    engine.tick(5000);
    auto reenabled = store.getRule(disabled.id).value();
    reenabled.enabled = true;
    DOCTEST_REQUIRE(store.updateRule(reenabled).isOk());
    engine.tick(5001);
    DOCTEST_CHECK(countRuns(db.db()) == 0);
    DOCTEST_CHECK(channels.fake->calls == 0);
    return true;
})

// 防：排队窗口（桶等待/同规则串行）内规则被禁用仍执行——出队前必须复核规则行。
REGISTER_TEST(schedule_engine_drain_rechecks_disabled_rule,
              "调度引擎: 排队触发出队前复核规则行, 已禁用则丢弃不执行", {
    auto db = openMigrated("drainrecheck");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    int taskCalls = 0;
    engine.taskRegistry().registerTask(
        "test:task", [&](const module::schedule::TaskInvoke&) {
            ++taskCalls;
            module::schedule::TaskOutcome o;
            o.status = "async";
            return core::Result<module::schedule::TaskOutcome>::ok(o);
        });

    auto rule = store.createRule(makeIntervalRule(10, 0, "run_task", "all")).value();
    engine.tick(5);    // 冷启动: next=10
    engine.tick(10);   // 触发 1: async 占住 running
    engine.tick(20);   // 到期点 2: 同规则 running → 排队 pending
    DOCTEST_CHECK(taskCalls == 1);
    DOCTEST_CHECK(countRuns(db.db(), "pending") == 1);

    auto disabled = store.getRule(rule.id).value();
    disabled.enabled = false;
    DOCTEST_REQUIRE(store.updateRule(disabled).isOk());
    module::schedule::RunOutcome done;
    done.status = "success";
    DOCTEST_REQUIRE(engine.completeRun(
        queryOptInt(db.db(), "SELECT id AS v FROM schedule_run WHERE status='running'")
            .value().value_or(0), done, 21).isOk());
    engine.tick(21);   // drain: 出队复核 → 已禁用 → 丢弃
    DOCTEST_CHECK(taskCalls == 1);                      // 排队触发未执行
    DOCTEST_CHECK(countRuns(db.db(), "success") == 1);  // 只有触发 1
    DOCTEST_CHECK(countRuns(db.db(), "running") == 0);
    return true;
})

// 防：missing_secret（keychain 未配 key，配置问题）被判永久失败——未配 key 的
// 渠道第一次投递即被停用（§4.9 只允许 93000 类停用；wecom.h 状态表定义
// missing_secret 为配置问题）。
REGISTER_TEST(schedule_delivery_missing_secret_no_disable,
              "投递编排: missing_secret 只落 dropped, 不停用渠道不重试, 后续触发仍投递", {
    auto db = openMigrated("missingsecret");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "wecom");
    channels.fake->script = {makeReceipt("missing_secret")};
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    DOCTEST_CHECK(module::schedule::classifyReceipt(makeReceipt("missing_secret")) !=
                  module::schedule::ReceiptClass::kPermanent);
    DOCTEST_CHECK(module::schedule::classifyReceipt(makeReceipt("permanent:errcode_93000")) ==
                  module::schedule::ReceiptClass::kPermanent);

    // 两条一次性规则：规则 1 触发后归档，保证 +30s/+300s 重试阶梯窗口内无新触发，
    // 「calls 不变」才能单独归因于不重试；规则 2 证明渠道未被误停用、仍可投递。
    (void)store.createRule(
        makeOnceRule(100, "once", {channels.channelRowId})).value();
    (void)store.createRule(
        makeOnceRule(500, "once", {channels.channelRowId})).value();
    engine.tick(50);
    engine.tick(100);   // 第 1 次投递: missing_secret
    DOCTEST_CHECK(channels.fake->calls == 1);
    engine.tick(130);   // 重试阶梯 +30s: 若误入重试队列, 此刻会重试
    DOCTEST_CHECK(channels.fake->calls == 1);   // 不重试
    engine.tick(400);   // 阶梯 +300s 终点: 仍不重试
    DOCTEST_CHECK(channels.fake->calls == 1);
    auto dRows = db.db().prepare(
                           "SELECT status FROM schedule_delivery").value().fetchAll().value();
    DOCTEST_REQUIRE(!dRows.empty());
    DOCTEST_CHECK(dRows[0].getString("status").valueOr("") == "dropped");
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT enabled AS v FROM schedule_channel").value()
                  .value_or(0) == 1);   // 渠道保持启用
    engine.tick(500);   // 渠道未被误停用: 规则 2 触发仍正常尝试投递
    DOCTEST_CHECK(channels.fake->calls == 2);
    return true;
})

// 防：活跃规则每 tick 整行 updateRule 写放大——updated_at 每秒被刷（审计 P2-8）。
// 非触发 tick 零落库：last_eval_at 停在上一个边界（旧实现逐 tick 刷到 99）；
// 任何落库都会同时写 last_eval_at 与 updated_at，故 last_eval_at 不变 = 无写。
REGISTER_TEST(schedule_engine_active_rule_no_per_tick_persist,
              "调度引擎: 活跃规则非触发 tick 不落库, last_eval_at 停在边界, 触发边界才推进", {
    auto db = openMigrated("writeamp");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(100, 0, "remind", "once", {channels.channelRowId})).value();
    engine.tick(50);   // 冷启动: last_eval=50（边界落库）
    for (int64_t t = 51; t <= 99; ++t) engine.tick(t);
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                    std::to_string(rule.id)).value().value_or(-1) == 50);
    engine.tick(100);  // 到期触发（边界落库）
    DOCTEST_CHECK(countRuns(db.db(), "success") == 1);
    DOCTEST_CHECK(queryOptInt(db.db(), "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                    std::to_string(rule.id)).value().value_or(-1) == 100);
    DOCTEST_CHECK(nextFireOf(db.db(), rule.id) == 200);
    return true;
})

// 防：引擎接线回归——模块 onStart 后唯一 ticker 泵必须真驱动引擎（规则
// last_eval_at 推进 + run 落库），onStop 注销订阅（hasSubscribers 清零、
// pump 空转不崩、重复 onStop 幂等）。
REGISTER_TEST(schedule_engine_module_wiring_start_stop,
              "调度引擎接线: onStart 挂泵驱动引擎, onStop 注销订阅幂等", {
    test_support::TestEnv env(kDbPrefix, "wiring", /*registerAppDb=*/true);
    (void)env;
    core::ticker::resetForTest();
    module::schedule::ScheduleStore store(*core::appDatabase());
    auto rule = store.createRule(
        makeIntervalRule(10, 0, "remind", "once", {})).value();

    module::schedule::ScheduleModule mod;
    mod.onStart();
    DOCTEST_CHECK(core::ticker::hasSubscribers());
    core::ticker::pump(100);  // 首帧: 冷启动只刷 last_eval 不触发
    DOCTEST_CHECK(queryOptInt(*core::appDatabase(),
                              "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                  std::to_string(rule.id)).value().value_or(-1) == 100);

    mod.onStart();            // 双 onStart 幂等: 不重复订阅
    core::ticker::pump(110);  // nextFire 严格 > after: 冷启动 next=110, 此刻触发一次
    DOCTEST_CHECK(queryOptInt(*core::appDatabase(),
                              "SELECT last_eval_at AS v FROM schedule_rule WHERE id = " +
                                  std::to_string(rule.id)).value().value_or(-1) == 110);
    DOCTEST_CHECK(countRuns(*core::appDatabase()) == 1);

    mod.onStop();
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    core::ticker::pump(300);  // 注销后 pump 空转不崩
    mod.onStop();             // 双 onStop = no-op
    return true;
})

// 防：暂停分支未对齐禁用分支——暂停期间跨过的旧到期点（next_fire_at 缓存）
// 在解除当 tick 被当「当前到期」补发（R1 遗留：禁用分支已推走缓存，暂停没推）。
REGISTER_TEST(schedule_engine_paused_no_stale_refire_on_release,
              "调度引擎: 暂停跨越到期点, 解除后不补发旧到期点, 下次触发按 fresh 计算", {
    auto db = openMigrated("pausedstale");
    module::schedule::ScheduleStore store(db.db());
    TestChannels channels(store, "fake_remind");
    module::schedule::ScheduleEngine engine(db.db(), channels.registry);

    auto rule = store.createRule(
        makeIntervalRule(100, 0, "remind", "once", {channels.channelRowId})).value();
    engine.tick(50);   // 冷启动: last_eval=50, next=100
    auto paused = store.getRule(rule.id).value();
    paused.pausedUntil = 200;
    DOCTEST_REQUIRE(store.updateRule(paused).isOk());
    engine.tick(150);  // 暂停中跨过到期点 100: 缓存须推走到 200 (fresh)
    engine.tick(200);  // 解除当 tick: 只该触发 fresh 的 200, 旧点 100 不得补发
    auto rows = db.db().prepare("SELECT planned_at FROM schedule_run")
                    .value().fetchAll().value();
    DOCTEST_REQUIRE(rows.size() == 1);
    DOCTEST_CHECK(rows[0].getInt64("planned_at").valueOr(-1) == 200);
    DOCTEST_CHECK(channels.fake->calls == 1);
    return true;
})
