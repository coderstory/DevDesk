// 四表 CRUD 实现。约定：
//   - 每条语句独立 prepare（Statement 换绑须先 reset，逐次 prepare 免去 reset 步）；
//   - 「改动行数」与「最后插入 id」用 SQL 函数 changes() / last_insert_rowid() 读，
//     不碰裸 sqlite3* 句柄（免自持锁）；
//   - created_at / updated_at 由本层落 core::time::nowUnix()。
#include "schedule_store.h"

#include "core/time.h"

#include <charconv>
#include <system_error>

namespace module::schedule {

namespace {

int64_t nowUnix() { return static_cast<int64_t>(core::time::nowUnix()); }

// 上次语句（INSERT/UPDATE/DELETE）改动的行数；本连接随后只跑 SELECT，changes() 不被重置。
core::Result<int64_t> changesCount(core::data::SqliteDb& db) {
    auto prep = db.prepare("SELECT changes() AS v");
    if (prep.isFail()) return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return core::Result<int64_t>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) return core::Result<int64_t>::ok(0);
    return rows.value()[0].getInt64("v");
}

// 本次插入行的 rowid（须紧跟 INSERT 之后调用）。
core::Result<int64_t> lastInsertId(core::data::SqliteDb& db) {
    auto prep = db.prepare("SELECT last_insert_rowid() AS v");
    if (prep.isFail()) return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return core::Result<int64_t>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) return core::Result<int64_t>::ok(0);
    return rows.value()[0].getInt64("v");
}

// [1,2,3] ↔ vector<int64_t>（channel_ids 文本列）。空串与 [] 都当空表；格式非法 → JsonParseError。
// 用 std::from_chars 而非 stoll：-fno-exceptions 下 stoll 对非法输入直接 abort，错误必须走 Result。
core::Result<std::vector<int64_t>> decodeIds(const std::string& text) {
    std::vector<int64_t> out;
    if (text.empty() || text == "[]") return core::Result<std::vector<int64_t>>::ok(std::move(out));
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return core::Result<std::vector<int64_t>>::fail(
            core::ErrorCode::JsonParseError, "channel_ids 不是 JSON 数组: " + text);
    }
    const std::string body = text.substr(1, text.size() - 2);
    size_t pos = 0;
    while (pos <= body.size()) {
        const size_t comma = body.find(',', pos);
        const std::string item = body.substr(pos, comma - pos);
        if (item.empty()) {
            return core::Result<std::vector<int64_t>>::fail(
                core::ErrorCode::JsonParseError, "channel_ids 含空元素: " + text);
        }
        int64_t value = 0;
        const char* begin = item.data();
        const auto parsed = std::from_chars(begin, begin + item.size(), value);
        if (parsed.ec != std::errc() || parsed.ptr != begin + item.size()) {
            return core::Result<std::vector<int64_t>>::fail(
                core::ErrorCode::JsonParseError, "channel_ids 元素不是整数: " + item);
        }
        out.push_back(value);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return core::Result<std::vector<int64_t>>::ok(std::move(out));
}

std::string encodeIds(const std::vector<int64_t>& ids) {
    std::string out = "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) out += ",";
        out += std::to_string(ids[i]);
    }
    out += "]";
    return out;
}

// 可空整数列：nullopt ↔ NULL。
core::Result<void> bindOptInt(core::data::Statement& stmt, int index,
                              const std::optional<int64_t>& value) {
    if (value.has_value()) return stmt.bind(index, value.value());
    return stmt.bind(index, nullptr);
}

// 读可空整数列：NULL → nullopt。
core::Result<std::optional<int64_t>> readOptInt(const core::data::SqlRow& row,
                                                const std::string& col) {
    if (row.isNull(col)) return core::Result<std::optional<int64_t>>::ok(std::nullopt);
    auto v = row.getInt64(col);
    if (v.isFail())
        return core::Result<std::optional<int64_t>>::fail(v.errorCode(), v.errorMessage());
    return core::Result<std::optional<int64_t>>::ok(v.value());
}

// 读行 → 结构体（列名与 SELECT 列单一致；漏列/错名在此显式报错，不静默给默认值）。
#define READ_TEXT(target, col)                                                 \
    do {                                                                       \
        auto _r = row.getString(col);                                          \
        if (_r.isFail())                                                       \
            return core::Result<RuleRecord>::fail(_r.errorCode(), _r.errorMessage()); \
        target = _r.value();                                                   \
    } while (0)

core::Result<RuleRecord> ruleFromRow(const core::data::SqlRow& row) {
    RuleRecord r;
    auto id = row.getInt64("id");
    if (id.isFail()) return core::Result<RuleRecord>::fail(id.errorCode(), id.errorMessage());
    r.id = id.value();
    READ_TEXT(r.name, "name");
    auto enabled = row.getInt64("enabled");
    if (enabled.isFail())
        return core::Result<RuleRecord>::fail(enabled.errorCode(), enabled.errorMessage());
    r.enabled = enabled.value() != 0;
    auto paused = readOptInt(row, "paused_until");
    if (paused.isFail()) return core::Result<RuleRecord>::fail(paused.errorCode(), paused.errorMessage());
    r.pausedUntil = paused.value();
    READ_TEXT(r.kind, "kind");
    READ_TEXT(r.specJson, "spec_json");
    READ_TEXT(r.tzId, "tz_id");
    READ_TEXT(r.actionKind, "action_kind");
    READ_TEXT(r.actionJson, "action_json");
    auto idsText = row.getString("channel_ids");
    if (idsText.isFail())
        return core::Result<RuleRecord>::fail(idsText.errorCode(), idsText.errorMessage());
    auto ids = decodeIds(idsText.value());
    if (ids.isFail()) return core::Result<RuleRecord>::fail(ids.errorCode(), ids.errorMessage());
    r.channelIds = std::move(ids.value());
    READ_TEXT(r.missedPolicy, "missed_policy");
    READ_TEXT(r.conflictPolicy, "conflict_policy");
    auto maxRuns = row.getInt64("max_runs");
    if (maxRuns.isFail())
        return core::Result<RuleRecord>::fail(maxRuns.errorCode(), maxRuns.errorMessage());
    r.maxRuns = maxRuns.value();
    auto next = readOptInt(row, "next_fire_at");
    if (next.isFail()) return core::Result<RuleRecord>::fail(next.errorCode(), next.errorMessage());
    r.nextFireAt = next.value();
    auto last = readOptInt(row, "last_fire_at");
    if (last.isFail()) return core::Result<RuleRecord>::fail(last.errorCode(), last.errorMessage());
    r.lastFireAt = last.value();
    auto eval = readOptInt(row, "last_eval_at");
    if (eval.isFail()) return core::Result<RuleRecord>::fail(eval.errorCode(), eval.errorMessage());
    r.lastEvalAt = eval.value();
    auto count = row.getInt64("run_count");
    if (count.isFail())
        return core::Result<RuleRecord>::fail(count.errorCode(), count.errorMessage());
    r.runCount = count.value();
    auto archived = readOptInt(row, "archived_at");
    if (archived.isFail()) return core::Result<RuleRecord>::fail(archived.errorCode(), archived.errorMessage());
    r.archivedAt = archived.value();
    auto deleted = readOptInt(row, "deleted_at");
    if (deleted.isFail()) return core::Result<RuleRecord>::fail(deleted.errorCode(), deleted.errorMessage());
    r.deletedAt = deleted.value();
    auto created = row.getInt64("created_at");
    if (created.isFail())
        return core::Result<RuleRecord>::fail(created.errorCode(), created.errorMessage());
    r.createdAt = created.value();
    auto updated = row.getInt64("updated_at");
    if (updated.isFail())
        return core::Result<RuleRecord>::fail(updated.errorCode(), updated.errorMessage());
    r.updatedAt = updated.value();
    return core::Result<RuleRecord>::ok(std::move(r));
}

#undef READ_TEXT

#define READ_TEXT(target, col)                                                 \
    do {                                                                       \
        auto _r = row.getString(col);                                          \
        if (_r.isFail())                                                       \
            return core::Result<RunRecord>::fail(_r.errorCode(), _r.errorMessage()); \
        target = _r.value();                                                   \
    } while (0)

core::Result<RunRecord> runFromRow(const core::data::SqlRow& row) {
    RunRecord r;
    auto id = row.getInt64("id");
    if (id.isFail()) return core::Result<RunRecord>::fail(id.errorCode(), id.errorMessage());
    r.id = id.value();
    READ_TEXT(r.origin, "origin");
    READ_TEXT(r.originKey, "origin_key");
    auto ruleId = readOptInt(row, "rule_id");
    if (ruleId.isFail()) return core::Result<RunRecord>::fail(ruleId.errorCode(), ruleId.errorMessage());
    r.ruleId = ruleId.value();
    auto planned = row.getInt64("planned_at");
    if (planned.isFail())
        return core::Result<RunRecord>::fail(planned.errorCode(), planned.errorMessage());
    r.plannedAt = planned.value();
    auto started = readOptInt(row, "started_at");
    if (started.isFail()) return core::Result<RunRecord>::fail(started.errorCode(), started.errorMessage());
    r.startedAt = started.value();
    auto ended = readOptInt(row, "ended_at");
    if (ended.isFail()) return core::Result<RunRecord>::fail(ended.errorCode(), ended.errorMessage());
    r.endedAt = ended.value();
    READ_TEXT(r.status, "status");
    auto attempt = row.getInt64("attempt");
    if (attempt.isFail())
        return core::Result<RunRecord>::fail(attempt.errorCode(), attempt.errorMessage());
    r.attempt = static_cast<int>(attempt.value());
    auto duration = readOptInt(row, "duration_ms");
    if (duration.isFail()) return core::Result<RunRecord>::fail(duration.errorCode(), duration.errorMessage());
    r.durationMs = duration.value();
    READ_TEXT(r.resultSummary, "result_summary");
    READ_TEXT(r.error, "error");
    return core::Result<RunRecord>::ok(std::move(r));
}

#undef READ_TEXT

#define READ_TEXT(target, col)                                                 \
    do {                                                                       \
        auto _r = row.getString(col);                                          \
        if (_r.isFail())                                                       \
            return core::Result<DeliveryRecord>::fail(_r.errorCode(), _r.errorMessage()); \
        target = _r.value();                                                   \
    } while (0)

core::Result<DeliveryRecord> deliveryFromRow(const core::data::SqlRow& row) {
    DeliveryRecord d;
    auto id = row.getInt64("id");
    if (id.isFail()) return core::Result<DeliveryRecord>::fail(id.errorCode(), id.errorMessage());
    d.id = id.value();
    auto runId = row.getInt64("run_id");
    if (runId.isFail())
        return core::Result<DeliveryRecord>::fail(runId.errorCode(), runId.errorMessage());
    d.runId = runId.value();
    auto channelId = row.getInt64("channel_id");
    if (channelId.isFail())
        return core::Result<DeliveryRecord>::fail(channelId.errorCode(), channelId.errorMessage());
    d.channelId = channelId.value();
    auto attempt = row.getInt64("attempt");
    if (attempt.isFail())
        return core::Result<DeliveryRecord>::fail(attempt.errorCode(), attempt.errorMessage());
    d.attempt = static_cast<int>(attempt.value());
    READ_TEXT(d.status, "status");
    auto code = readOptInt(row, "response_code");
    if (code.isFail()) return core::Result<DeliveryRecord>::fail(code.errorCode(), code.errorMessage());
    d.responseCode = code.value();
    auto errcode = readOptInt(row, "remote_errcode");
    if (errcode.isFail())
        return core::Result<DeliveryRecord>::fail(errcode.errorCode(), errcode.errorMessage());
    d.remoteErrcode = errcode.value();
    READ_TEXT(d.error, "error");
    auto retryAt = readOptInt(row, "next_retry_at");
    if (retryAt.isFail())
        return core::Result<DeliveryRecord>::fail(retryAt.errorCode(), retryAt.errorMessage());
    d.nextRetryAt = retryAt.value();
    auto created = row.getInt64("created_at");
    if (created.isFail())
        return core::Result<DeliveryRecord>::fail(created.errorCode(), created.errorMessage());
    d.createdAt = created.value();
    return core::Result<DeliveryRecord>::ok(std::move(d));
}

#undef READ_TEXT

core::Result<ChannelRecord> channelFromRow(const core::data::SqlRow& row) {
    ChannelRecord c;
    auto id = row.getInt64("id");
    if (id.isFail()) return core::Result<ChannelRecord>::fail(id.errorCode(), id.errorMessage());
    c.id = id.value();
    auto kind = row.getString("kind");
    if (kind.isFail())
        return core::Result<ChannelRecord>::fail(kind.errorCode(), kind.errorMessage());
    c.kind = kind.value();
    auto name = row.getString("name");
    if (name.isFail())
        return core::Result<ChannelRecord>::fail(name.errorCode(), name.errorMessage());
    c.name = name.value();
    auto enabled = row.getInt64("enabled");
    if (enabled.isFail())
        return core::Result<ChannelRecord>::fail(enabled.errorCode(), enabled.errorMessage());
    c.enabled = enabled.value() != 0;
    auto config = row.getString("config_json");
    if (config.isFail())
        return core::Result<ChannelRecord>::fail(config.errorCode(), config.errorMessage());
    c.configJson = config.value();
    auto ref = row.getString("secret_ref");
    if (ref.isFail())
        return core::Result<ChannelRecord>::fail(ref.errorCode(), ref.errorMessage());
    c.secretRef = ref.value();
    auto created = row.getInt64("created_at");
    if (created.isFail())
        return core::Result<ChannelRecord>::fail(created.errorCode(), created.errorMessage());
    c.createdAt = created.value();
    auto updated = row.getInt64("updated_at");
    if (updated.isFail())
        return core::Result<ChannelRecord>::fail(updated.errorCode(), updated.errorMessage());
    c.updatedAt = updated.value();
    return core::Result<ChannelRecord>::ok(std::move(c));
}

// 规则 SELECT 列单（与 ruleFromRow 一一对应）
constexpr const char* kRuleCols =
    "id, name, enabled, paused_until, kind, spec_json, tz_id, action_kind, action_json, "
    "channel_ids, missed_policy, conflict_policy, max_runs, next_fire_at, last_fire_at, "
    "last_eval_at, run_count, archived_at, deleted_at, created_at, updated_at";

constexpr const char* kRunCols =
    "id, origin, origin_key, rule_id, planned_at, started_at, ended_at, status, attempt, "
    "duration_ms, result_summary, error";

constexpr const char* kDeliveryCols =
    "id, run_id, channel_id, attempt, status, response_code, remote_errcode, error, "
    "next_retry_at, created_at";

constexpr const char* kChannelCols =
    "id, kind, name, enabled, config_json, secret_ref, created_at, updated_at";

} // namespace

ScheduleStore::ScheduleStore(core::data::SqliteDb& db) : db_(db) {}

// ---- 规则 ----

core::Result<RuleRecord> ScheduleStore::createRule(const RuleRecord& rule) {
    const int64_t now = nowUnix();
    auto prep = db_.prepare(
        "INSERT INTO schedule_rule (name, enabled, paused_until, kind, spec_json, tz_id, "
        "action_kind, action_json, channel_ids, missed_policy, conflict_policy, max_runs, "
        "next_fire_at, last_fire_at, last_eval_at, run_count, archived_at, deleted_at, "
        "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (prep.isFail()) return core::Result<RuleRecord>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    int i = 1;
    if (auto r = stmt.bind(i++, rule.name); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.enabled ? 1 : 0); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, rule.pausedUntil); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.kind); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.specJson); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.tzId); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.actionKind); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.actionJson); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, encodeIds(rule.channelIds)); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.missedPolicy); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.conflictPolicy); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.maxRuns); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, rule.nextFireAt); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, rule.lastFireAt); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, rule.lastEvalAt); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, rule.runCount); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, rule.archivedAt); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, rule.deletedAt); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, now); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, now); r.isFail()) return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    auto step = stmt.step();
    if (step.isFail()) return core::Result<RuleRecord>::fail(step.errorCode(), step.errorMessage());
    auto newId = lastInsertId(db_);
    if (newId.isFail()) return core::Result<RuleRecord>::fail(newId.errorCode(), newId.errorMessage());
    auto created = getRule(newId.value());
    if (created.isFail()) return created;
    return created;
}

core::Result<RuleRecord> ScheduleStore::getRule(int64_t id) {
    auto prep = db_.prepare(std::string("SELECT ") + kRuleCols +
                            " FROM schedule_rule WHERE id = ? AND deleted_at IS NULL");
    if (prep.isFail()) return core::Result<RuleRecord>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, id); r.isFail())
        return core::Result<RuleRecord>::fail(r.errorCode(), r.errorMessage());
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return core::Result<RuleRecord>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) {
        return core::Result<RuleRecord>::fail(core::ErrorCode::NotFound,
                                              "规则不存在或已删除: " + std::to_string(id));
    }
    return ruleFromRow(rows.value()[0]);
}

core::Result<std::vector<RuleRecord>> ScheduleStore::listRules() {
    auto prep = db_.prepare(std::string("SELECT ") + kRuleCols +
                            " FROM schedule_rule WHERE deleted_at IS NULL ORDER BY id ASC");
    if (prep.isFail())
        return core::Result<std::vector<RuleRecord>>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail())
        return core::Result<std::vector<RuleRecord>>::fail(rows.errorCode(), rows.errorMessage());
    std::vector<RuleRecord> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto r = ruleFromRow(row);
        if (r.isFail())
            return core::Result<std::vector<RuleRecord>>::fail(r.errorCode(), r.errorMessage());
        out.push_back(std::move(r.value()));
    }
    return core::Result<std::vector<RuleRecord>>::ok(std::move(out));
}

core::Result<void> ScheduleStore::updateRule(const RuleRecord& rule) {
    const int64_t now = nowUnix();
    // archived_at / deleted_at / created_at 不在此列：归档/软删只有 archiveRule /
    // deleteRule 两个入口（防误写解归档），created_at 不可变。
    auto prep = db_.prepare(
        "UPDATE schedule_rule SET name = ?, enabled = ?, paused_until = ?, kind = ?, "
        "spec_json = ?, tz_id = ?, action_kind = ?, action_json = ?, channel_ids = ?, "
        "missed_policy = ?, conflict_policy = ?, max_runs = ?, next_fire_at = ?, "
        "last_fire_at = ?, last_eval_at = ?, run_count = ?, "
        "updated_at = ? WHERE id = ? AND deleted_at IS NULL");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    int i = 1;
    if (auto r = stmt.bind(i++, rule.name); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.enabled ? 1 : 0); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, rule.pausedUntil); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.kind); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.specJson); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.tzId); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.actionKind); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.actionJson); r.isFail()) return r;
    if (auto r = stmt.bind(i++, encodeIds(rule.channelIds)); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.missedPolicy); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.conflictPolicy); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.maxRuns); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, rule.nextFireAt); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, rule.lastFireAt); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, rule.lastEvalAt); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.runCount); r.isFail()) return r;
    if (auto r = stmt.bind(i++, now); r.isFail()) return r;
    if (auto r = stmt.bind(i++, rule.id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "规则不存在或已删除: " + std::to_string(rule.id));
    }
    return core::Result<void>::ok();
}

core::Result<void> ScheduleStore::deleteRule(int64_t id) {
    const int64_t now = nowUnix();
    auto prep = db_.prepare(
        "UPDATE schedule_rule SET deleted_at = ?, updated_at = ? "
        "WHERE id = ? AND deleted_at IS NULL");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, now); r.isFail()) return r;
    if (auto r = stmt.bind(2, now); r.isFail()) return r;
    if (auto r = stmt.bind(3, id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "规则不存在或已删除: " + std::to_string(id));
    }
    return core::Result<void>::ok();
}

core::Result<void> ScheduleStore::purgeRule(int64_t id) {
    auto prep = db_.prepare("DELETE FROM schedule_rule WHERE id = ?");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "规则不存在: " + std::to_string(id));
    }
    return core::Result<void>::ok();
}

core::Result<void> ScheduleStore::archiveRule(int64_t id) {
    const int64_t now = nowUnix();
    // 归档即停用（enabled=0）：重启后引擎不再排它（§4.4/验收 4）
    auto prep = db_.prepare(
        "UPDATE schedule_rule SET archived_at = ?, enabled = 0, updated_at = ? "
        "WHERE id = ? AND deleted_at IS NULL AND archived_at IS NULL");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, now); r.isFail()) return r;
    if (auto r = stmt.bind(2, now); r.isFail()) return r;
    if (auto r = stmt.bind(3, id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "规则不存在、已删除或已归档: " + std::to_string(id));
    }
    return core::Result<void>::ok();
}

// ---- 运行记录 ----

core::Result<std::optional<RunRecord>> ScheduleStore::findRun(const std::string& origin,
                                                              const std::string& originKey,
                                                              int64_t plannedAt) {
    auto prep = db_.prepare(std::string("SELECT ") + kRunCols +
                            " FROM schedule_run WHERE origin = ? AND origin_key = ? "
                            "AND planned_at = ? LIMIT 1");
    if (prep.isFail())
        return core::Result<std::optional<RunRecord>>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, origin); r.isFail())
        return core::Result<std::optional<RunRecord>>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(2, originKey); r.isFail())
        return core::Result<std::optional<RunRecord>>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(3, plannedAt); r.isFail())
        return core::Result<std::optional<RunRecord>>::fail(r.errorCode(), r.errorMessage());
    auto rows = stmt.fetchAll();
    if (rows.isFail())
        return core::Result<std::optional<RunRecord>>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) return core::Result<std::optional<RunRecord>>::ok(std::nullopt);
    auto r = runFromRow(rows.value()[0]);
    if (r.isFail())
        return core::Result<std::optional<RunRecord>>::fail(r.errorCode(), r.errorMessage());
    return core::Result<std::optional<RunRecord>>::ok(std::move(r.value()));
}

core::Result<RunRecord> ScheduleStore::insertRun(const RunRecord& run) {
    auto prep = db_.prepare(
        "INSERT INTO schedule_run (origin, origin_key, rule_id, planned_at, started_at, "
        "ended_at, status, attempt, duration_ms, result_summary, error) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(origin, origin_key, planned_at) DO NOTHING");
    if (prep.isFail()) return core::Result<RunRecord>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    int i = 1;
    if (auto r = stmt.bind(i++, run.origin); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, run.originKey); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, run.ruleId); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, run.plannedAt); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, run.startedAt); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, run.endedAt); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, run.status); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, static_cast<int64_t>(run.attempt)); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, run.durationMs); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, run.resultSummary); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, run.error); r.isFail()) return core::Result<RunRecord>::fail(r.errorCode(), r.errorMessage());
    auto step = stmt.step();
    if (step.isFail()) return core::Result<RunRecord>::fail(step.errorCode(), step.errorMessage());
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<RunRecord>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        // 唯一索引命中：已有同键行（重启重复补跑 / 并发重复入队）。返回错误让
        // 调用方走「已处理过」分支，绝不产生第二行。
        auto existing = findRun(run.origin, run.originKey, run.plannedAt);
        const int64_t existingId =
            (existing.isOk() && existing.value().has_value()) ? existing.value().value().id : 0;
        return core::Result<RunRecord>::fail(
            core::ErrorCode::AlreadyExists,
            "同 (origin, origin_key, planned_at) 运行记录已存在，已有行 id=" +
                std::to_string(existingId));
    }
    auto created = findRun(run.origin, run.originKey, run.plannedAt);
    if (created.isFail() || !created.value().has_value()) {
        return core::Result<RunRecord>::fail(core::ErrorCode::Internal,
                                             "插入成功但回读失败");
    }
    return core::Result<RunRecord>::ok(std::move(created.value().value()));
}

core::Result<void> ScheduleStore::updateRun(const RunRecord& run) {
    auto prep = db_.prepare(
        "UPDATE schedule_run SET origin = ?, origin_key = ?, rule_id = ?, planned_at = ?, "
        "started_at = ?, ended_at = ?, status = ?, attempt = ?, duration_ms = ?, "
        "result_summary = ?, error = ? WHERE id = ?");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    int i = 1;
    if (auto r = stmt.bind(i++, run.origin); r.isFail()) return r;
    if (auto r = stmt.bind(i++, run.originKey); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, run.ruleId); r.isFail()) return r;
    if (auto r = stmt.bind(i++, run.plannedAt); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, run.startedAt); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, run.endedAt); r.isFail()) return r;
    if (auto r = stmt.bind(i++, run.status); r.isFail()) return r;
    if (auto r = stmt.bind(i++, static_cast<int64_t>(run.attempt)); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, run.durationMs); r.isFail()) return r;
    if (auto r = stmt.bind(i++, run.resultSummary); r.isFail()) return r;
    if (auto r = stmt.bind(i++, run.error); r.isFail()) return r;
    if (auto r = stmt.bind(i++, run.id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "运行记录不存在: " + std::to_string(run.id));
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<RunRecord>> ScheduleStore::listRunsByRule(int64_t ruleId, int limit) {
    auto prep = db_.prepare(std::string("SELECT ") + kRunCols +
                            " FROM schedule_run WHERE rule_id = ? "
                            "ORDER BY planned_at DESC, id DESC LIMIT ?");
    if (prep.isFail())
        return core::Result<std::vector<RunRecord>>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, ruleId); r.isFail())
        return core::Result<std::vector<RunRecord>>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(2, static_cast<int64_t>(limit)); r.isFail())
        return core::Result<std::vector<RunRecord>>::fail(r.errorCode(), r.errorMessage());
    auto rows = stmt.fetchAll();
    if (rows.isFail())
        return core::Result<std::vector<RunRecord>>::fail(rows.errorCode(), rows.errorMessage());
    std::vector<RunRecord> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto r = runFromRow(row);
        if (r.isFail())
            return core::Result<std::vector<RunRecord>>::fail(r.errorCode(), r.errorMessage());
        out.push_back(std::move(r.value()));
    }
    return core::Result<std::vector<RunRecord>>::ok(std::move(out));
}

// ---- 投递记录 ----

core::Result<DeliveryRecord> ScheduleStore::insertDelivery(const DeliveryRecord& delivery) {
    const int64_t now = nowUnix();
    auto prep = db_.prepare(
        "INSERT INTO schedule_delivery (run_id, channel_id, attempt, status, response_code, "
        "remote_errcode, error, next_retry_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (prep.isFail())
        return core::Result<DeliveryRecord>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    int i = 1;
    if (auto r = stmt.bind(i++, delivery.runId); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, delivery.channelId); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, static_cast<int64_t>(delivery.attempt)); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, delivery.status); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, delivery.responseCode); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, delivery.remoteErrcode); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, delivery.error); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = bindOptInt(stmt, i++, delivery.nextRetryAt); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(i++, now); r.isFail()) return core::Result<DeliveryRecord>::fail(r.errorCode(), r.errorMessage());
    auto step = stmt.step();
    if (step.isFail())
        return core::Result<DeliveryRecord>::fail(step.errorCode(), step.errorMessage());
    auto newId = lastInsertId(db_);
    if (newId.isFail())
        return core::Result<DeliveryRecord>::fail(newId.errorCode(), newId.errorMessage());
    DeliveryRecord out = delivery;
    out.id = newId.value();
    out.createdAt = now;
    return core::Result<DeliveryRecord>::ok(std::move(out));
}

core::Result<void> ScheduleStore::updateDelivery(const DeliveryRecord& delivery) {
    auto prep = db_.prepare(
        "UPDATE schedule_delivery SET run_id = ?, channel_id = ?, attempt = ?, status = ?, "
        "response_code = ?, remote_errcode = ?, error = ?, next_retry_at = ? WHERE id = ?");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    int i = 1;
    if (auto r = stmt.bind(i++, delivery.runId); r.isFail()) return r;
    if (auto r = stmt.bind(i++, delivery.channelId); r.isFail()) return r;
    if (auto r = stmt.bind(i++, static_cast<int64_t>(delivery.attempt)); r.isFail()) return r;
    if (auto r = stmt.bind(i++, delivery.status); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, delivery.responseCode); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, delivery.remoteErrcode); r.isFail()) return r;
    if (auto r = stmt.bind(i++, delivery.error); r.isFail()) return r;
    if (auto r = bindOptInt(stmt, i++, delivery.nextRetryAt); r.isFail()) return r;
    if (auto r = stmt.bind(i++, delivery.id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "投递记录不存在: " + std::to_string(delivery.id));
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<DeliveryRecord>> ScheduleStore::listDeliveriesByRun(int64_t runId) {
    auto prep = db_.prepare(std::string("SELECT ") + kDeliveryCols +
                            " FROM schedule_delivery WHERE run_id = ? "
                            "ORDER BY attempt ASC, id ASC");
    if (prep.isFail())
        return core::Result<std::vector<DeliveryRecord>>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, runId); r.isFail())
        return core::Result<std::vector<DeliveryRecord>>::fail(r.errorCode(), r.errorMessage());
    auto rows = stmt.fetchAll();
    if (rows.isFail())
        return core::Result<std::vector<DeliveryRecord>>::fail(rows.errorCode(), rows.errorMessage());
    std::vector<DeliveryRecord> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto d = deliveryFromRow(row);
        if (d.isFail())
            return core::Result<std::vector<DeliveryRecord>>::fail(d.errorCode(), d.errorMessage());
        out.push_back(std::move(d.value()));
    }
    return core::Result<std::vector<DeliveryRecord>>::ok(std::move(out));
}

// ---- 渠道配置 ----

core::Result<ChannelRecord> ScheduleStore::createChannel(const ChannelRecord& channel) {
    const int64_t now = nowUnix();
    auto prep = db_.prepare(
        "INSERT INTO schedule_channel (kind, name, enabled, config_json, secret_ref, "
        "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)");
    if (prep.isFail())
        return core::Result<ChannelRecord>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, channel.kind); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(2, channel.name); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(3, channel.enabled ? 1 : 0); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(4, channel.configJson); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(5, channel.secretRef); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(6, now); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    if (auto r = stmt.bind(7, now); r.isFail()) return core::Result<ChannelRecord>::fail(r.errorCode(), r.errorMessage());
    auto step = stmt.step();
    if (step.isFail())
        return core::Result<ChannelRecord>::fail(step.errorCode(), step.errorMessage());
    auto newId = lastInsertId(db_);
    if (newId.isFail())
        return core::Result<ChannelRecord>::fail(newId.errorCode(), newId.errorMessage());
    ChannelRecord out = channel;
    out.id = newId.value();
    out.createdAt = now;
    out.updatedAt = now;
    return core::Result<ChannelRecord>::ok(std::move(out));
}

core::Result<std::optional<ChannelRecord>> ScheduleStore::getChannel(int64_t id) {
    auto prep = db_.prepare(std::string("SELECT ") + kChannelCols +
                            " FROM schedule_channel WHERE id = ?");
    if (prep.isFail())
        return core::Result<std::optional<ChannelRecord>>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, id); r.isFail())
        return core::Result<std::optional<ChannelRecord>>::fail(r.errorCode(), r.errorMessage());
    auto rows = stmt.fetchAll();
    if (rows.isFail())
        return core::Result<std::optional<ChannelRecord>>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) return core::Result<std::optional<ChannelRecord>>::ok(std::nullopt);
    auto c = channelFromRow(rows.value()[0]);
    if (c.isFail())
        return core::Result<std::optional<ChannelRecord>>::fail(c.errorCode(), c.errorMessage());
    return core::Result<std::optional<ChannelRecord>>::ok(std::move(c.value()));
}

core::Result<std::vector<ChannelRecord>> ScheduleStore::listChannels() {
    auto prep = db_.prepare(std::string("SELECT ") + kChannelCols +
                            " FROM schedule_channel ORDER BY id ASC");
    if (prep.isFail())
        return core::Result<std::vector<ChannelRecord>>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail())
        return core::Result<std::vector<ChannelRecord>>::fail(rows.errorCode(), rows.errorMessage());
    std::vector<ChannelRecord> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto c = channelFromRow(row);
        if (c.isFail())
            return core::Result<std::vector<ChannelRecord>>::fail(c.errorCode(), c.errorMessage());
        out.push_back(std::move(c.value()));
    }
    return core::Result<std::vector<ChannelRecord>>::ok(std::move(out));
}

core::Result<void> ScheduleStore::updateChannel(const ChannelRecord& channel) {
    const int64_t now = nowUnix();
    auto prep = db_.prepare(
        "UPDATE schedule_channel SET kind = ?, name = ?, enabled = ?, config_json = ?, "
        "secret_ref = ?, updated_at = ? WHERE id = ?");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, channel.kind); r.isFail()) return r;
    if (auto r = stmt.bind(2, channel.name); r.isFail()) return r;
    if (auto r = stmt.bind(3, channel.enabled ? 1 : 0); r.isFail()) return r;
    if (auto r = stmt.bind(4, channel.configJson); r.isFail()) return r;
    if (auto r = stmt.bind(5, channel.secretRef); r.isFail()) return r;
    if (auto r = stmt.bind(6, now); r.isFail()) return r;
    if (auto r = stmt.bind(7, channel.id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "渠道不存在: " + std::to_string(channel.id));
    }
    return core::Result<void>::ok();
}

core::Result<void> ScheduleStore::deleteChannel(int64_t id) {
    auto prep = db_.prepare("DELETE FROM schedule_channel WHERE id = ?");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, id); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return step;
    auto changed = changesCount(db_);
    if (changed.isFail()) return core::Result<void>::fail(changed.errorCode(), changed.errorMessage());
    if (changed.value() == 0) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "渠道不存在: " + std::to_string(id));
    }
    return core::Result<void>::ok();
}

} // namespace module::schedule
