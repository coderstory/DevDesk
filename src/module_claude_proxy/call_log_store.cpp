// src/module_claude_proxy/call_log_store.cpp
// CallLogStore 实现（方法体自 ClaudeService 原实现搬移，语义零变化）
#include "call_log_store.h"

#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace module::claude {

namespace {

// 取当前 unix 秒
std::time_t nowUnix() {
    return std::time(nullptr);
}

// LIKE 模式转义（审计 [11]）：用户输入的 % / _ / \ 按字面量匹配
// （SQL 侧统一 ESCAPE '\'，SQLite 的 LIKE 对 ASCII 默认大小写不敏感）
std::string escapeLikePattern(const std::string& term) {
    std::string out;
    out.reserve(term.size() + 8);
    for (const char c : term) {
        if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// queryCalls / countCalls 共用 WHERE 片段（无命中返回空串 → 调用方省略 WHERE）。
// status_code 列可 NULL（早期直插行），COALESCE 归 0 → 落「错误」桶
std::string buildCallLogWhere(const CallLogStore::CallLogQuery& q) {
    std::string where;
    if (!q.search.empty()) {
        where = "(path LIKE ? ESCAPE '\\' OR method LIKE ? ESCAPE '\\')";
    }
    std::string status;
    switch (q.statusClass) {
        case CallLogStore::CallLogQuery::kStatus2xx:
            status = "COALESCE(status_code, 0) BETWEEN 200 AND 299";
            break;
        case CallLogStore::CallLogQuery::kStatus3xx:
            status = "COALESCE(status_code, 0) BETWEEN 300 AND 399";
            break;
        case CallLogStore::CallLogQuery::kStatus4xx:
            status = "COALESCE(status_code, 0) BETWEEN 400 AND 499";
            break;
        case CallLogStore::CallLogQuery::kStatus5xx:
            status = "COALESCE(status_code, 0) >= 500";
            break;
        case CallLogStore::CallLogQuery::kStatusErr:
            status = "COALESCE(status_code, 0) < 200";
            break;
        default:
            break;  // kStatusAll：不加状态条件
    }
    if (!status.empty()) {
        where = where.empty() ? status : (where + " AND " + status);
    }
    return where;
}

CallLogStore::CallLogEntry callLogFromRow(const core::data::SqlRow& row) {
    CallLogStore::CallLogEntry e;
    auto idV = row.getInt64("id");
    auto pidV = row.getInt64("profile_id");
    auto mV = row.getString("method");
    auto pV = row.getString("path");
    auto sV = row.getInt64("status_code");
    auto lV = row.getInt64("latency_ms");
    auto rbV = row.getInt64("request_bytes");
    auto rxbV = row.getInt64("response_bytes");
    auto errV = row.getString("error");
    auto saV = row.getInt64("started_at");
    if (idV.isOk()) e.id = idV.value();
    if (pidV.isOk()) e.profileId = pidV.value();
    if (mV.isOk()) e.method = mV.value();
    if (pV.isOk()) e.path = pV.value();
    if (sV.isOk()) e.statusCode = static_cast<int>(sV.value());
    if (lV.isOk()) e.latencyMs = lV.value();
    if (rbV.isOk()) e.requestBytes = rbV.value();
    if (rxbV.isOk()) e.responseBytes = rxbV.value();
    if (errV.isOk()) e.error = errV.value();
    if (saV.isOk()) e.startedAt = static_cast<std::time_t>(saV.value());
    return e;
}

} // namespace

CallLogStore::CallLogStore(core::data::SqliteDb& db) : db_(db) {}

core::Result<void> CallLogStore::logCall(const CallLogEntry& entry) {
    auto prep = db_.prepare(
        "INSERT INTO claude_call_logs "
        "(profile_id, method, path, status_code, latency_ms, "
        " request_bytes, response_bytes, error, started_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (prep.isFail()) {
        return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    stmt.bind(1, entry.profileId);
    stmt.bind(2, entry.method);
    stmt.bind(3, entry.path);
    stmt.bind(4, static_cast<int64_t>(entry.statusCode));
    stmt.bind(5, entry.latencyMs);
    stmt.bind(6, entry.requestBytes);
    stmt.bind(7, entry.responseBytes);
    if (entry.error.empty()) {
        stmt.bind(8, nullptr);
    } else {
        stmt.bind(8, entry.error);
    }
    stmt.bind(9, static_cast<int64_t>(entry.startedAt == 0 ? nowUnix() : entry.startedAt));

    auto stepR = stmt.step();
    if (stepR.isFail()) {
        return core::Result<void>::fail(stepR.errorCode(), stepR.errorMessage());
    }
    // 审计 perf [13]：落库成功即推进日志版本号，Claude 页 compose 据此
    // 只在有新调用时重查 recentCalls（proxy worker 线程写，UI 线程读）
    callLogVersion_.fetch_add(1, std::memory_order_release);
    return core::Result<void>::ok();
}

core::Result<std::vector<CallLogStore::CallLogEntry>>
CallLogStore::recentCalls(int limit) {
    CallLogQuery q;
    q.limit = limit;
    return queryCalls(q);
}

core::Result<std::vector<CallLogStore::CallLogEntry>>
CallLogStore::queryCalls(const CallLogQuery& q) {
    const int limit = q.limit > 0 ? q.limit : 20;
    const int offset = q.offset > 0 ? q.offset : 0;

    std::string sql =
        "SELECT id, profile_id, method, path, status_code, latency_ms, "
        "request_bytes, response_bytes, error, started_at "
        "FROM claude_call_logs";
    const std::string where = buildCallLogWhere(q);
    if (!where.empty()) sql += " WHERE " + where;
    sql += " ORDER BY started_at DESC, id DESC LIMIT ? OFFSET ?";

    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<std::vector<CallLogEntry>>::fail(
            prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    int bindIdx = 1;
    if (!q.search.empty()) {
        const std::string like = "%" + escapeLikePattern(q.search) + "%";
        stmt.bind(bindIdx++, like);
        stmt.bind(bindIdx++, like);
    }
    stmt.bind(bindIdx++, static_cast<int64_t>(limit));
    stmt.bind(bindIdx++, static_cast<int64_t>(offset));

    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<CallLogEntry>>::fail(
            rows.errorCode(), rows.errorMessage());
    }

    std::vector<CallLogEntry> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        out.push_back(callLogFromRow(row));
    }
    return core::Result<std::vector<CallLogEntry>>::ok(std::move(out));
}

core::Result<int> CallLogStore::countCalls(const CallLogQuery& q) {
    std::string sql = "SELECT COUNT(*) AS n FROM claude_call_logs";
    const std::string where = buildCallLogWhere(q);
    if (!where.empty()) sql += " WHERE " + where;

    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<int>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (!q.search.empty()) {
        const std::string like = "%" + escapeLikePattern(q.search) + "%";
        stmt.bind(1, like);
        stmt.bind(2, like);
    }

    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<int>::fail(rows.errorCode(), rows.errorMessage());
    }
    if (rows.value().empty()) return core::Result<int>::ok(0);
    auto nV = rows.value()[0].getInt64("n");
    if (nV.isFail()) {
        return core::Result<int>::fail(nV.errorCode(), nV.errorMessage());
    }
    return core::Result<int>::ok(static_cast<int>(nV.value()));
}

} // namespace module::claude