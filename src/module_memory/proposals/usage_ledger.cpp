// src/module_memory/proposals/usage_ledger.cpp
#include "usage_ledger.h"
#include <utility>

#include <ctime>

#include "core/sqlite_db.h"
#include "core/time.h"

namespace module::memory::proposals {

namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS usage_ledger (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  kind TEXT NOT NULL,
  tokens INTEGER NOT NULL,
  cost_milli_yuan INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_usage_ts ON usage_ledger(ts);
)";

// 今日/本月 0 点（core::time 统一 localtime 口径，业务零平台宏）
std::int64_t dayStart(std::int64_t now) {
    std::tm tm{};
    core::time::localTime(static_cast<std::time_t>(now), &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm));
}

std::int64_t monthStart(std::int64_t now) {
    std::tm tm{};
    core::time::localTime(static_cast<std::time_t>(now), &tm);
    tm.tm_mday = 1;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm));
}

// 单值查询垫（同 collector 口径）：无行/失败回 0
std::int64_t queryInt64(core::data::SqliteDb* db, const std::string& sql,
                        std::int64_t arg) {
    auto stmt = db->prepare(sql);
    if (stmt.isFail()) return 0;
    if (!stmt.value().bind(1, arg).isOk()) return 0;
    auto step = stmt.value().step();
    if (!step.isOk() || !step.value()) return 0;
    const auto rowResult = stmt.value().currentRow();
    if (rowResult.isFail()) return 0;
    return rowResult.value().getInt64("v").valueOr(0);
}

}  // namespace

std::size_t countCodePoints(const std::string& utf8) {
    std::size_t n = 0;
    for (char c : utf8) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

long long estimateCostMilliYuan(const std::string& kind, long long tokens) {
    if (tokens <= 0) return 0;
    double yuanPerTok = 0.0;
    if (kind == "llm_input") {
        yuanPerTok = kLlmInputYuanPerMTok / 1000000.0;
    } else if (kind == "llm_output") {
        yuanPerTok = kLlmOutputYuanPerMTok / 1000000.0;
    } else if (kind == "embed") {
        yuanPerTok = kEmbedYuanPerMTok / 1000000.0;
    }
    const double milli = yuanPerTok * static_cast<double>(tokens) * 1000.0;
    return static_cast<long long>(milli + 0.5);  // 四舍五入：浮点 24.999…截断成 24
}

core::Result<UsageLedger> UsageLedger::open(
    const std::filesystem::path& dbFile) {
    auto db = core::data::SqliteDb::open(dbFile);
    if (db.isFail()) {
        return core::Result<UsageLedger>::fail(db.errorCode(),
                                               db.errorMessage());
    }
    auto applied = db.value().applySchema(kSchema);
    if (applied.isFail()) {
        return core::Result<UsageLedger>::fail(applied.errorCode(),
                                               applied.errorMessage());
    }
    UsageLedger ledger;
    ledger.db_ = std::make_shared<core::data::SqliteDb>(std::move(db.value()));
    return core::Result<UsageLedger>::ok(std::move(ledger));
}

core::Result<long long> UsageLedger::record(const std::string& kind,
                                            long long tokens,
                                            std::int64_t nowSec) {
    if (tokens < 0) tokens = 0;
    const long long cost = estimateCostMilliYuan(kind, tokens);
    auto stmt = db_->prepare(
        "INSERT INTO usage_ledger(ts, kind, tokens, cost_milli_yuan) "
        "VALUES(?, ?, ?, ?)");
    if (stmt.isFail()) {
        return core::Result<long long>::fail(stmt.errorCode(),
                                             stmt.errorMessage());
    }
    if (!stmt.value().bind(1, nowSec).isOk() ||
        !stmt.value().bind(2, kind).isOk() ||
        !stmt.value().bind(3, tokens).isOk() ||
        !stmt.value().bind(4, cost).isOk()) {
        return core::Result<long long>::fail(core::ErrorCode::Internal,
                                             "计量入账绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<long long>::fail(step.errorCode(),
                                             step.errorMessage());
    }
    return core::Result<long long>::ok(cost);
}

// 成本口径排除注入账：inject_* 行是 hook 预算记账，不是模型 token（头文件契约）
constexpr const char* kCostTokenSql =
    "SELECT COALESCE(SUM(tokens),0) AS v FROM usage_ledger "
    "WHERE ts >= ? AND kind NOT LIKE 'inject%'";
constexpr const char* kCostMilliSql =
    "SELECT COALESCE(SUM(cost_milli_yuan),0) AS v FROM usage_ledger "
    "WHERE ts >= ? AND kind NOT LIKE 'inject%'";
constexpr const char* kInjectSql =
    "SELECT COALESCE(SUM(tokens),0) AS v FROM usage_ledger "
    "WHERE ts >= ? AND kind = ?";

core::Result<long long> UsageLedger::todayTokens(std::int64_t nowSec) const {
    return core::Result<long long>::ok(
        queryInt64(db_.get(), kCostTokenSql, dayStart(nowSec)));
}

core::Result<long long> UsageLedger::monthTokens(std::int64_t nowSec) const {
    return core::Result<long long>::ok(
        queryInt64(db_.get(), kCostTokenSql, monthStart(nowSec)));
}

core::Result<long long> UsageLedger::todayCostMilliYuan(
    std::int64_t nowSec) const {
    return core::Result<long long>::ok(
        queryInt64(db_.get(), kCostMilliSql, dayStart(nowSec)));
}

core::Result<long long> UsageLedger::monthCostMilliYuan(
    std::int64_t nowSec) const {
    return core::Result<long long>::ok(
        queryInt64(db_.get(), kCostMilliSql, monthStart(nowSec)));
}

core::Result<void> UsageLedger::recordInjection(int items, int chars,
                                                std::int64_t nowSec) {
    auto itemsRow = record(kKindInjectItems, items, nowSec);
    if (itemsRow.isFail()) {
        return core::Result<void>::fail(itemsRow.errorCode(),
                                        itemsRow.errorMessage());
    }
    auto charsRow = record(kKindInjectChars, chars, nowSec);
    if (charsRow.isFail()) {
        return core::Result<void>::fail(charsRow.errorCode(),
                                        charsRow.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<long long> UsageLedger::todayInjectItems(
    std::int64_t nowSec) const {
    auto stmt = db_->prepare(kInjectSql);
    if (stmt.isFail()) {
        return core::Result<long long>::fail(stmt.errorCode(),
                                             stmt.errorMessage());
    }
    if (!stmt.value().bind(1, dayStart(nowSec)).isOk() ||
        !stmt.value().bind(2, std::string(kKindInjectItems)).isOk()) {
        return core::Result<long long>::fail(core::ErrorCode::Internal,
                                             "注入账查询绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail() || !step.value()) {
        return core::Result<long long>::ok(0);
    }
    auto row = stmt.value().currentRow();
    if (row.isFail()) return core::Result<long long>::ok(0);
    return core::Result<long long>::ok(row.value().getInt64("v").valueOr(0));
}

core::Result<long long> UsageLedger::todayInjectChars(
    std::int64_t nowSec) const {
    auto stmt = db_->prepare(kInjectSql);
    if (stmt.isFail()) {
        return core::Result<long long>::fail(stmt.errorCode(),
                                             stmt.errorMessage());
    }
    if (!stmt.value().bind(1, dayStart(nowSec)).isOk() ||
        !stmt.value().bind(2, std::string(kKindInjectChars)).isOk()) {
        return core::Result<long long>::fail(core::ErrorCode::Internal,
                                             "注入账查询绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail() || !step.value()) {
        return core::Result<long long>::ok(0);
    }
    auto row = stmt.value().currentRow();
    if (row.isFail()) return core::Result<long long>::ok(0);
    return core::Result<long long>::ok(row.value().getInt64("v").valueOr(0));
}

core::Result<std::vector<LedgerRow>> UsageLedger::recentRows(int limit) const {
    auto stmt = db_->prepare(
        "SELECT ts, kind, tokens, cost_milli_yuan FROM usage_ledger "
        "ORDER BY ts DESC, id DESC LIMIT ?");
    if (stmt.isFail()) {
        return core::Result<std::vector<LedgerRow>>::fail(stmt.errorCode(),
                                                          stmt.errorMessage());
    }
    if (!stmt.value().bind(1, static_cast<std::int64_t>(limit)).isOk()) {
        return core::Result<std::vector<LedgerRow>>::fail(
            core::ErrorCode::Internal, "最近账面绑定失败");
    }
    std::vector<LedgerRow> rows;
    for (int i = 0; i < limit; ++i) {
        auto step = stmt.value().step();
        if (step.isFail()) break;
        if (!step.value()) break;
        auto row = stmt.value().currentRow();
        if (row.isFail()) break;
        LedgerRow r;
        r.ts = row.value().getInt64("ts").valueOr(0);
        r.kind = row.value().getString("kind").valueOr("");
        r.tokens = row.value().getInt64("tokens").valueOr(0);
        r.costMilliYuan = row.value().getInt64("cost_milli_yuan").valueOr(0);
        rows.push_back(std::move(r));
    }
    return core::Result<std::vector<LedgerRow>>::ok(std::move(rows));
}

}  // namespace module::memory::proposals
