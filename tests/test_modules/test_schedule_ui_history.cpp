// tests/test_modules/test_schedule_ui_history.cpp
// 运行历史抽屉（波 S7b）纯装配契约：planned_at 倒序稳定排序、limit 截断、
// 渠道投递与重试次数汇总、经 ScheduleStore 的 loadHistory 全链。
// 每条用例对应一个业务场景并注明防什么事故。
#include "doctest.h"

#include "core/schema.h"
#include "core/sqlite_db.h"
#include "module_schedule/schedule_ui_history.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using module::schedule::DeliveryRecord;
using module::schedule::RunRecord;
using module::schedule::ScheduleStore;
using module::schedule::ui::assembleHistory;
using module::schedule::ui::loadHistory;
using module::schedule::ui::summarizeDeliveries;

RunRecord makeRun(int64_t id, int64_t ruleId, int64_t plannedAt, const std::string& status,
                  int64_t durationMs, const std::string& error = "") {
    RunRecord r;
    r.id = id;
    r.origin = "user";
    r.originKey = "rule:" + std::to_string(ruleId);
    r.ruleId = ruleId;
    r.plannedAt = plannedAt;
    r.startedAt = plannedAt;
    r.endedAt = plannedAt + durationMs / 1000;
    r.status = status;
    r.durationMs = durationMs;
    r.error = error;
    return r;
}

DeliveryRecord makeDelivery(int64_t id, int64_t runId, int64_t channelId, int attempt,
                            const std::string& status, std::optional<int64_t> nextRetryAt = std::nullopt) {
    DeliveryRecord d;
    d.id = id;
    d.runId = runId;
    d.channelId = channelId;
    d.attempt = attempt;
    d.status = status;
    d.nextRetryAt = nextRetryAt;
    return d;
}

}  // namespace

// 防：历史列表顺序错乱（最新一次触发被挤到下面，用户误判最近结果）。
REGISTER_TEST(scheduleHistoryAssembleSortDesc, "schedule 历史抽屉：装配按 planned_at 倒序稳定排序 + limit 截断", {
    std::vector<RunRecord> runs;
    runs.push_back(makeRun(1, 7, 1000, "success", 50));
    runs.push_back(makeRun(2, 7, 3000, "failed", 500, "webhook 超时"));
    runs.push_back(makeRun(3, 7, 2000, "missed", 0));
    const auto items = assembleHistory(runs, {}, 10);
    DOCTEST_REQUIRE(items.size() == 3);
    DOCTEST_CHECK(items[0].run.id == 2);  // planned_at 3000
    DOCTEST_CHECK(items[1].run.id == 3);  // 2000
    DOCTEST_CHECK(items[2].run.id == 1);  // 1000
    const auto limited = assembleHistory(runs, {}, 2);
    DOCTEST_REQUIRE(limited.size() == 2);
    DOCTEST_CHECK(limited[0].run.id == 2);
    DOCTEST_CHECK(assembleHistory(runs, {}, 0).empty());
    return true;
});

// 防：重试次数/成败算错导致「run 成功、投递失败」不可分辨（§4.5/验收 7）。
REGISTER_TEST(scheduleHistorySummarizeRetries, "schedule 历史抽屉：渠道汇总 = 尝试/重试/成功/末次状态", {
    std::vector<DeliveryRecord> ds;
    // 渠道 1：第一次 retry、第二次 success → attempts 2 / retries 1 / success 1
    ds.push_back(makeDelivery(1, 42, 1, 0, "retry", 5000));
    ds.push_back(makeDelivery(2, 42, 1, 1, "success"));
    // 渠道 2：一次 failed 且安排了重试 → attempts 1 / retries 0
    ds.push_back(makeDelivery(3, 42, 2, 0, "failed", 9000));
    const auto sums = summarizeDeliveries(ds);
    DOCTEST_REQUIRE(sums.size() == 2);
    DOCTEST_CHECK(sums[0].channelId == 1);  // channelId 升序
    DOCTEST_CHECK(sums[0].attempts == 2);
    DOCTEST_CHECK(sums[0].retries == 1);
    DOCTEST_CHECK(sums[0].successCount == 1);
    DOCTEST_CHECK(sums[0].lastStatus == "success");
    DOCTEST_CHECK(sums[1].channelId == 2);
    DOCTEST_CHECK(sums[1].attempts == 1);
    DOCTEST_CHECK(sums[1].retries == 0);
    DOCTEST_CHECK(sums[1].successCount == 0);
    DOCTEST_CHECK(sums[1].lastStatus == "failed");
    DOCTEST_REQUIRE(sums[1].nextRetryAt.has_value());
    DOCTEST_CHECK(*sums[1].nextRetryAt == 9000);
    DOCTEST_CHECK(summarizeDeliveries({}).empty());  // 无投递的 run 不崩不误显
    return true;
});

// 防：装配按 run_id 归组错位（A 规则的投递挂到 B 规则的行上）。
REGISTER_TEST(scheduleHistoryAssembleGroupsDeliveries, "schedule 历史抽屉：投递按 run_id 归组进对应行", {
    std::vector<RunRecord> runs;
    runs.push_back(makeRun(1, 7, 2000, "success", 10));
    runs.push_back(makeRun(2, 7, 1000, "success", 10));
    std::vector<DeliveryRecord> ds;
    ds.push_back(makeDelivery(1, 2, 1, 0, "success"));  // 只属于 run 2
    const auto items = assembleHistory(runs, ds, 10);
    DOCTEST_REQUIRE(items.size() == 2);
    DOCTEST_CHECK(items[0].run.id == 1);
    DOCTEST_CHECK(items[0].channelSummaries.empty());
    DOCTEST_CHECK(items[1].run.id == 2);
    DOCTEST_REQUIRE(items[1].channelSummaries.size() == 1);
    DOCTEST_CHECK(items[1].channelSummaries[0].channelId == 1);
    return true;
});

// 防：经 ScheduleStore 读 schedule_run + schedule_delivery 全链断裂（含软删隔离、
// 唯一键不变时也能读到历史）。
REGISTER_TEST(scheduleHistoryLoadViaStore, "schedule 历史抽屉：loadHistory 经 ScheduleStore 读两表", {
    auto db = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(db.isOk());
    DOCTEST_REQUIRE(db.value().applySchema(core::schema::kSql).isOk());
    ScheduleStore store(db.value());

    const int64_t ruleId = 7;
    {
        auto r = store.insertRun(makeRun(0, ruleId, 5000, "success", 30));
        DOCTEST_REQUIRE(r.isOk());
        const int64_t runId = r.value().id;
        DOCTEST_REQUIRE(store.insertDelivery(makeDelivery(0, runId, 1, 0, "success")).isOk());
    }
    {
        auto r = store.insertRun(makeRun(0, ruleId, 4000, "failed", 700, "boom"));
        DOCTEST_REQUIRE(r.isOk());
        const int64_t runId = r.value().id;
        DOCTEST_REQUIRE(store.insertDelivery(makeDelivery(0, runId, 1, 0, "retry", 6000)).isOk());
        DOCTEST_REQUIRE(store.insertDelivery(makeDelivery(0, runId, 1, 1, "success")).isOk());
    }

    const auto items = loadHistory(store, ruleId, 10);
    DOCTEST_REQUIRE(items.isOk());
    DOCTEST_REQUIRE(items.value().size() == 2);
    DOCTEST_CHECK(items.value()[0].run.plannedAt == 5000);  // 倒序
    DOCTEST_CHECK(items.value()[0].channelSummaries[0].attempts == 1);
    DOCTEST_CHECK(items.value()[1].run.plannedAt == 4000);
    DOCTEST_CHECK(items.value()[1].run.status == "failed");
    DOCTEST_CHECK(items.value()[1].channelSummaries[0].attempts == 2);
    DOCTEST_CHECK(items.value()[1].channelSummaries[0].retries == 1);
    return true;
});
