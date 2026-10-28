// 定时调度存储层（S3）测试：迁移幂等 / 唯一键守恒 / CRUD 往返 / 软删隔离。
// 每条用例对应一个业务场景并注明防的具体事故（场景 = 什么状态、什么环境、做什么）。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/result.h"
#include "module_schedule/schedule_store.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_schedstore_";

// 建一个已 migrate 的临时库（open 即应用 schema，再显式 migrate 一次走幂等路径）。
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

// 单表行数（结果列别名 v）；失败返回 -1
int64_t countRows(core::data::SqliteDb& db, const std::string& table) {
    auto prep = db.prepare("SELECT COUNT(*) AS v FROM " + table);
    if (prep.isFail()) return -1;
    auto rows = prep.value().fetchAll();
    if (rows.isFail() || rows.value().empty()) return -1;
    return rows.value()[0].getInt64("v").valueOr(-1);
}

// 单值查询（结果列别名 v）；失败返回空 optional
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

module::schedule::RuleRecord makeRule(const std::string& name) {
    module::schedule::RuleRecord r;
    r.name = name;
    r.kind = "calendar";
    r.specJson = R"({"unit":"week","interval":2,"weekdays":[2],"time":"09:00","anchor_date":"2026-10-27"})";
    r.actionKind = "remind";
    r.actionJson = R"({"title":"站立会"})";
    r.channelIds = {1, 2};
    r.missedPolicy = "once";
    r.conflictPolicy = "priority";
    r.nextFireAt = 1760100000;
    return r;
}

} // namespace

REGISTER_TEST(schedule_store_migrate_idempotent,
              "调度四表迁移幂等：老库反复打开、数据与索引原样（防：二次启动迁移炸/丢数据/重复建表建索引）", {
    auto path = test_support::tempDbPath(kDbPrefix, "migrate");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        auto opened = core::Database::open(path);
        DOCTEST_REQUIRE(opened.isOk());
        auto& db = opened.value().db();
        module::schedule::ScheduleStore store(db);

        auto rule = store.createRule(makeRule("每两周周二提醒"));
        DOCTEST_REQUIRE(rule.isOk());
        module::schedule::RunRecord run;
        run.origin = "user";
        run.originKey = "rule:" + std::to_string(rule.value().id) + ":sched";
        run.ruleId = rule.value().id;
        run.plannedAt = 1760100000;
        run.status = "success";
        DOCTEST_REQUIRE(store.insertRun(run).isOk());
        module::schedule::ChannelRecord ch;
        ch.kind = "wecom";
        ch.name = "群机器人";
        ch.secretRef = "wecom.ch1";
        DOCTEST_REQUIRE(store.createChannel(ch).isOk());

        // 同一连接再 migrate 一次：幂等，行数不变
        DOCTEST_REQUIRE(opened.value().migrate().isOk());
        DOCTEST_CHECK_EQ(countRows(db, "schedule_rule"), 1);
        DOCTEST_CHECK_EQ(countRows(db, "schedule_run"), 1);
        DOCTEST_CHECK_EQ(countRows(db, "schedule_channel"), 1);
    } // 关库（模拟进程退出）
    {
        // 模拟重启：重开老库再 migrate，数据与唯一索引都在
        auto reopened = core::Database::open(path);
        DOCTEST_REQUIRE(reopened.isOk());
        DOCTEST_REQUIRE(reopened.value().migrate().isOk());
        auto& db = reopened.value().db();
        DOCTEST_CHECK_EQ(countRows(db, "schedule_rule"), 1);
        DOCTEST_CHECK_EQ(countRows(db, "schedule_run"), 1);
        DOCTEST_CHECK_EQ(countRows(db, "schedule_channel"), 1);
        auto idx = queryOptInt(
            db, "SELECT COUNT(*) AS v FROM sqlite_master WHERE type='index' "
                "AND name='idx_schedule_run_unique'");
        DOCTEST_REQUIRE(idx.isOk());
        DOCTEST_CHECK_EQ(idx.value().value(), 1);
    }
    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schedule_store_run_insert_no_side_effect,
              "同 (origin,origin_key,planned_at) 二次插入被拒且零副作用、不覆盖已有行（防：重启重复补跑产生重复行/重复动作）", {
    auto opened = openMigrated("dup");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    module::schedule::ScheduleStore store(db);

    module::schedule::ChannelRecord ch;
    ch.kind = "os";
    ch.name = "系统通知";
    DOCTEST_REQUIRE(store.createChannel(ch).isOk());

    module::schedule::RunRecord run;
    run.origin = "system:release";
    run.originKey = "task:7:precheck";
    run.plannedAt = 1770000000;
    run.status = "running";
    auto first = store.insertRun(run);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().id > 0);

    auto second = store.insertRun(run);
    DOCTEST_CHECK(second.isFail());
    DOCTEST_CHECK(second.errorCode() == core::ErrorCode::AlreadyExists);

    // 行守恒：run 仍 1 行、无关表不受影响
    DOCTEST_CHECK_EQ(countRows(db, "schedule_run"), 1);
    DOCTEST_CHECK_EQ(countRows(db, "schedule_channel"), 1);

    // 无副作用：已有行内容未被第二次插入覆盖（状态仍是 running）
    auto found = store.findRun("system:release", "task:7:precheck", 1770000000);
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK(found.value().has_value());
    if (found.value().has_value()) {
        DOCTEST_CHECK_EQ(found.value().value().id, first.value().id);
        DOCTEST_CHECK_EQ(found.value().value().status, std::string("running"));
    }
    return true;
})

REGISTER_TEST(schedule_store_unique_index_blocks_duplicate,
              "唯一索引在数据库层硬挡同键重复行：绕过 store 的直接 SQL 也被拒（防：索引缺失时库层不设防）", {
    auto opened = openMigrated("uniq");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();

    auto insert = db.prepare(
        "INSERT INTO schedule_run (origin, origin_key, planned_at, status) "
        "VALUES ('user', 'k1', 100, 'pending')");
    DOCTEST_REQUIRE(insert.isOk());
    DOCTEST_REQUIRE(insert.value().step().isOk());

    auto dup = db.prepare(
        "INSERT INTO schedule_run (origin, origin_key, planned_at, status) "
        "VALUES ('user', 'k1', 100, 'pending')");
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK(dup.value().step().isFail());  // SQLITE_CONSTRAINT（唯一索引）
    DOCTEST_CHECK_EQ(countRows(db, "schedule_run"), 1);
    return true;
})

REGISTER_TEST(schedule_store_rule_crud_roundtrip,
              "规则 CRUD 往返：全字段落库/回读一致、更新生效、列表可见（防：字段映射错位/可空列丢失）", {
    auto opened = openMigrated("crud");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    module::schedule::ScheduleStore store(db);

    auto created = store.createRule(makeRule("每日巡检"));
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;
    DOCTEST_CHECK(id > 0);

    auto got = store.getRule(id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("每日巡检"));
    DOCTEST_CHECK(got.value().enabled);
    DOCTEST_CHECK(!got.value().pausedUntil.has_value());      // 可空列 NULL 往返
    DOCTEST_CHECK_EQ(got.value().kind, std::string("calendar"));
    DOCTEST_CHECK_EQ(got.value().tzId, std::string(""));      // 默认空串 = 本地时区
    DOCTEST_CHECK_EQ(got.value().actionKind, std::string("remind"));
    DOCTEST_CHECK_EQ(got.value().channelIds.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(got.value().channelIds[0], 1);
    DOCTEST_CHECK_EQ(got.value().channelIds[1], 2);
    DOCTEST_CHECK_EQ(got.value().missedPolicy, std::string("once"));
    DOCTEST_CHECK(got.value().nextFireAt.has_value());
    DOCTEST_CHECK_EQ(got.value().nextFireAt.value(), 1760100000);
    DOCTEST_CHECK(got.value().deletedAt.has_value() == false);
    DOCTEST_CHECK(got.value().updatedAt >= got.value().createdAt);

    // 更新：全字段覆盖生效
    module::schedule::RuleRecord updated = got.value();
    updated.name = "每日巡检（改）";
    updated.enabled = false;
    updated.pausedUntil = 1780000000;
    updated.channelIds = {3};
    updated.missedPolicy = "skip";
    DOCTEST_REQUIRE(store.updateRule(updated).isOk());
    auto after = store.getRule(id);
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(after.value().name, std::string("每日巡检（改）"));
    DOCTEST_CHECK(!after.value().enabled);
    DOCTEST_CHECK(after.value().pausedUntil.has_value());
    DOCTEST_CHECK_EQ(after.value().pausedUntil.value(), 1780000000);
    DOCTEST_CHECK_EQ(after.value().channelIds.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(after.value().channelIds[0], 3);
    DOCTEST_CHECK_EQ(after.value().missedPolicy, std::string("skip"));

    // 列表可见且有序
    DOCTEST_REQUIRE(store.createRule(makeRule("第二条")).isOk());
    auto list = store.listRules();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(list.value()[0].id, id);
    return true;
})

REGISTER_TEST(schedule_store_soft_delete_isolates,
              "软删后查询隔离：列表/读/改全部不可见，行仍在库（防：软删被实现成硬删丢历史）", {
    auto opened = openMigrated("softdel");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    module::schedule::ScheduleStore store(db);

    auto r1 = store.createRule(makeRule("A"));
    DOCTEST_REQUIRE(r1.isOk());
    auto r2 = store.createRule(makeRule("B"));
    DOCTEST_REQUIRE(r2.isOk());

    DOCTEST_REQUIRE(store.deleteRule(r1.value().id).isOk());

    // 隔离：list/get/update 都看不见软删行
    auto list = store.listRules();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(list.value()[0].id, r2.value().id);
    auto got = store.getRule(r1.value().id);
    DOCTEST_CHECK(got.isFail());
    DOCTEST_CHECK(got.errorCode() == core::ErrorCode::NotFound);
    module::schedule::RuleRecord upd = r1.value();
    upd.name = "A 改名";
    auto updResult = store.updateRule(upd);
    DOCTEST_CHECK(updResult.isFail());
    DOCTEST_CHECK(updResult.errorCode() == core::ErrorCode::NotFound);

    // 行仍在库（deleted_at 非空），B 行未被波及
    DOCTEST_CHECK_EQ(countRows(db, "schedule_rule"), 2);
    auto deletedAt = queryOptInt(db, "SELECT deleted_at AS v FROM schedule_rule WHERE id = " +
                                         std::to_string(r1.value().id));
    DOCTEST_REQUIRE(deletedAt.isOk());
    DOCTEST_CHECK(deletedAt.value().has_value());
    auto bDeleted = queryOptInt(db, "SELECT deleted_at AS v FROM schedule_rule WHERE id = " +
                                        std::to_string(r2.value().id));
    DOCTEST_REQUIRE(bDeleted.isOk());
    DOCTEST_CHECK(!bDeleted.value().has_value());

    // 重复软删幂等失败（NotFound），不会二次改行
    auto again = store.deleteRule(r1.value().id);
    DOCTEST_CHECK(again.isFail());

    // 彻底删除：行消失，另一行仍在
    DOCTEST_REQUIRE(store.purgeRule(r1.value().id).isOk());
    DOCTEST_CHECK_EQ(countRows(db, "schedule_rule"), 1);
    return true;
})

REGISTER_TEST(schedule_store_archive_once_rule,
              "一次性规则归档：archived_at 非空 + enabled=0（防：重启后一次性规则再次触发）", {
    auto opened = openMigrated("archive");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    module::schedule::ScheduleStore store(db);

    module::schedule::RuleRecord once = makeRule("单次闹钟");
    once.kind = "once";
    auto created = store.createRule(once);
    DOCTEST_REQUIRE(created.isOk());

    DOCTEST_REQUIRE(store.archiveRule(created.value().id).isOk());
    auto got = store.getRule(created.value().id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK(got.value().archivedAt.has_value());
    DOCTEST_CHECK(!got.value().enabled);

    // 已归档再归档 → NotFound（不重复写）
    auto again = store.archiveRule(created.value().id);
    DOCTEST_CHECK(again.isFail());
    DOCTEST_CHECK(again.errorCode() == core::ErrorCode::NotFound);

    // 归档行仍在 listRules（折叠是 UI 职责），raw 行 enabled=0
    auto list = store.listRules();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), static_cast<size_t>(1));
    auto rawEnabled = queryOptInt(db, "SELECT enabled AS v FROM schedule_rule WHERE id = " +
                                          std::to_string(created.value().id));
    DOCTEST_REQUIRE(rawEnabled.isOk());
    DOCTEST_CHECK_EQ(rawEnabled.value().value(), 0);
    return true;
})

REGISTER_TEST(schedule_store_channel_secret_ref,
              "渠道 CRUD：secret_ref 只存 keychain account 名、config_json 往返（防：key 明文落库/配置丢字段）", {
    auto opened = openMigrated("channel");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    module::schedule::ScheduleStore store(db);

    module::schedule::ChannelRecord ch;
    ch.kind = "wecom";
    ch.name = "排障群";
    ch.configJson = R"({"endpoint":"https://qyapi.weixin.qq.com/cgi-bin/webhook/send","msgtype":"text"})";
    ch.secretRef = "wecom.ch1";
    auto created = store.createChannel(ch);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;
    DOCTEST_CHECK(id > 0);
    DOCTEST_CHECK(created.value().createdAt > 0);
    DOCTEST_CHECK_EQ(created.value().secretRef, std::string("wecom.ch1"));

    auto got = store.getChannel(id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK(got.value().has_value());
    DOCTEST_CHECK_EQ(got.value().value().name, std::string("排障群"));
    DOCTEST_CHECK_EQ(got.value().value().configJson, ch.configJson);
    DOCTEST_CHECK(got.value().value().enabled);

    // 行内 secret_ref 恰好是 account 名（库里没有别的 key 载体）
    auto ref = db.prepare("SELECT secret_ref FROM schedule_channel WHERE id = ?");
    DOCTEST_REQUIRE(ref.isOk());
    DOCTEST_REQUIRE(ref.value().bind(1, id).isOk());
    auto refRows = ref.value().fetchAll();
    DOCTEST_REQUIRE(refRows.isOk());
    DOCTEST_CHECK_EQ(refRows.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(refRows.value()[0].getString("secret_ref").valueOr(""),
                     std::string("wecom.ch1"));

    // 更新 + 列表 + 删除
    module::schedule::ChannelRecord upd = got.value().value();
    upd.enabled = false;
    upd.secretRef = "wecom.ch1.rotated";
    DOCTEST_REQUIRE(store.updateChannel(upd).isOk());
    auto list = store.listChannels();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK(!list.value()[0].enabled);
    DOCTEST_CHECK_EQ(list.value()[0].secretRef, std::string("wecom.ch1.rotated"));

    DOCTEST_REQUIRE(store.deleteChannel(id).isOk());
    auto gone = store.getChannel(id);
    DOCTEST_REQUIRE(gone.isOk());
    DOCTEST_CHECK(!gone.value().has_value());
    return true;
})

REGISTER_TEST(schedule_store_delivery_roundtrip,
              "投递记录往返：每尝试一行、重试推进、不改变 run 的 success（防：run 成功与投递失败不可分辨）", {
    auto opened = openMigrated("delivery");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    module::schedule::ScheduleStore store(db);

    module::schedule::RunRecord run;
    run.origin = "user";
    run.originKey = "rule:1:d";
    run.ruleId = 1;
    run.plannedAt = 1790000000;
    run.status = "success";
    auto inserted = store.insertRun(run);
    DOCTEST_REQUIRE(inserted.isOk());
    const int64_t runId = inserted.value().id;

    module::schedule::DeliveryRecord d1;
    d1.runId = runId;
    d1.channelId = 9;
    d1.attempt = 0;
    d1.status = "failed";
    d1.responseCode = 500;
    d1.error = "upstream timeout";
    d1.nextRetryAt = 1790000030;
    auto ins1 = store.insertDelivery(d1);
    DOCTEST_REQUIRE(ins1.isOk());
    DOCTEST_CHECK(ins1.value().id > 0);

    module::schedule::DeliveryRecord d2;
    d2.runId = runId;
    d2.channelId = 9;
    d2.attempt = 1;
    d2.status = "pending";
    DOCTEST_REQUIRE(store.insertDelivery(d2).isOk());

    auto byRun = store.listDeliveriesByRun(runId);
    DOCTEST_REQUIRE(byRun.isOk());
    DOCTEST_CHECK_EQ(byRun.value().size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(byRun.value()[0].attempt, 0);
    DOCTEST_CHECK_EQ(byRun.value()[0].status, std::string("failed"));
    DOCTEST_CHECK(byRun.value()[0].responseCode.has_value());
    DOCTEST_CHECK_EQ(byRun.value()[0].responseCode.value(), 500);
    DOCTEST_CHECK_EQ(byRun.value()[0].nextRetryAt.value(), 1790000030);
    DOCTEST_CHECK_EQ(byRun.value()[1].attempt, 1);

    // 重试推进：第二行改 success
    module::schedule::DeliveryRecord upd2 = byRun.value()[1];
    upd2.status = "success";
    upd2.responseCode = 200;
    DOCTEST_REQUIRE(store.updateDelivery(upd2).isOk());
    auto after = store.listDeliveriesByRun(runId);
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(after.value()[1].status, std::string("success"));
    DOCTEST_CHECK_EQ(after.value()[1].responseCode.value(), 200);

    // 投递失败/重试不改变 run 的 success
    auto found = store.findRun("user", "rule:1:d", 1790000000);
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK(found.value().has_value());
    if (found.value().has_value()) {
        DOCTEST_CHECK_EQ(found.value().value().status, std::string("success"));
    }
    // 运行历史按规则可取
    auto runs = store.listRunsByRule(1, 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(runs.value()[0].id, runId);
    return true;
})
