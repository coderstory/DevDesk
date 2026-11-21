// Home 仪表盘数据服务实现
#include "home_service.h"

#include "core/call_rate.h"
#include "core/logging.h"
#include "core/overdue.h"
#include "core/time.h"

#include <cstdio>
#include <utility>

namespace module::home {

int64_t startOfDayEpoch(std::time_t t) {
    std::tm local{};
    core::time::localTime(t, &local);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    return static_cast<int64_t>(std::mktime(&local));
}

std::string formatRelativeTime(int64_t modifiedAt, std::time_t now) {
    const int64_t diff = static_cast<int64_t>(now) - modifiedAt;
    if (diff < 60) return "刚刚";  // 含未来时间（时钟偏差）兜底

    // 按本地日历日序号分桶（DST 切换日不会像「两个本地零点相减」那样偏差一天）。
    const int64_t dayGap =
        core::time::localDayNumber(now) -
        core::time::localDayNumber(static_cast<std::time_t>(modifiedAt));

    if (dayGap <= 0) {
        if (diff < 3600) {
            return std::to_string(diff / 60) + " 分钟前";
        }
        return std::to_string(diff / 3600) + " 小时前";
    }
    if (dayGap == 1) return "昨天";
    if (dayGap < 7) return std::to_string(dayGap) + " 天前";

    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(modifiedAt), &local);
    // buf 按最坏情况定尺（3 个 int 字段各 11 字节 + 分隔符 + '\0'），
    // 写小了会被 -Wformat-truncation 判红。
    char buf[36];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buf;
}

std::string formatTokenCount(int64_t tokens) {
    if (tokens < 1000) return std::to_string(tokens);
    char buf[32];
    if (tokens < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fk",
                      static_cast<double>(tokens) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1fM",
                      static_cast<double>(tokens) / 1000000.0);
    }
    return buf;
}

std::string formatRateTps(double tps) {
    if (tps <= 0.0) return "0";
    char buf[32];
    if (tps < 100.0) {
        std::snprintf(buf, sizeof(buf), "%.1f", tps);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", tps);
    }
    return buf;
}

HomeService::HomeService(core::data::SqliteDb& db) : db_(db) {}

core::Result<int64_t> HomeService::countBySql(
    const char* sql, std::optional<int64_t> boundValue) {
    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::propagate<int64_t>(prep);
    }
    auto& stmt = prep.value();
    if (boundValue.has_value()) {
        if (auto b = stmt.bind(1, *boundValue); b.isFail()) {
            return core::propagate<int64_t>(b);
        }
    }
    auto step = stmt.step();
    if (step.isFail()) {
        return core::propagate<int64_t>(step);
    }
    if (!step.value()) {
        return core::Result<int64_t>::fail(core::ErrorCode::Internal,
                                           "count 查询没有返回行");
    }
    auto row = stmt.currentRow();
    if (row.isFail()) {
        return core::propagate<int64_t>(row);
    }
    auto n = row.value().getInt64("n");
    if (n.isFail()) {
        return core::propagate<int64_t>(n);
    }
    return core::Result<int64_t>::ok(n.value());
}

core::Result<std::vector<RecentNote>> HomeService::queryRecentNotes(int limit) {
    auto prep = db_.prepare(
        "SELECT path, title, modified_at FROM markdown_index "
        "ORDER BY modified_at DESC, id DESC LIMIT ?");
    if (prep.isFail()) {
        return core::propagate<std::vector<RecentNote>>(prep);
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, static_cast<int64_t>(limit)); b.isFail()) {
        return core::propagate<std::vector<RecentNote>>(b);
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::propagate<std::vector<RecentNote>>(rows);
    }
    std::vector<RecentNote> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        RecentNote note;
        auto path = row.getString("path");
        if (path.isFail()) {
            return core::Result<std::vector<RecentNote>>::fail(
                path.errorCode(), path.errorMessage());
        }
        note.relativePath = path.value();
        auto title = row.getString("title");
        if (title.isOk()) note.title = title.value();
        auto mtime = row.getInt64("modified_at");
        if (mtime.isFail()) {
            return core::Result<std::vector<RecentNote>>::fail(
                mtime.errorCode(), mtime.errorMessage());
        }
        note.modifiedAt = mtime.value();
        out.push_back(std::move(note));
    }
    return core::Result<std::vector<RecentNote>>::ok(std::move(out));
}

core::Result<DashboardStats> HomeService::getStats(int recentNoteLimit) {
    if (recentNoteLimit <= 0) recentNoteLimit = 5;

    const std::time_t now = std::time(nullptr);
    const int64_t todayStart = startOfDayEpoch(now);

    // 逾期口径单源 core/overdue.h 的 kWhereSql（与 TodoService::listOverdue 共用）。
    auto todayNotes = countBySql(
        "SELECT COUNT(*) AS n FROM markdown_index WHERE modified_at >= ?",
        todayStart);
    if (todayNotes.isFail()) {
        return core::propagate<DashboardStats>(todayNotes);
    }
    // 待办任务口径 = 「待办列 + 进行中列」里未完成的任务数，不是全部未完成任务。
    // 只认列 kind（'todo'/'doing'）：schema.h 契约禁止按标题匹配（改名即静默失效），
    // H20 迁移已把历史「进行中」列补成 kind='doing'；done/archive/kind NULL 列不计。
    auto pendingTodos = countBySql(
        "SELECT COUNT(*) AS n FROM todo_tasks t JOIN todo_columns c "
        "ON c.id = t.column_id WHERE t.completed_at IS NULL "
        "AND c.kind IN ('todo','doing')");
    if (pendingTodos.isFail()) {
        return core::propagate<DashboardStats>(pendingTodos);
    }
    const std::string overdueCountSql =
        std::string("SELECT COUNT(*) AS n FROM todo_tasks WHERE ") +
        core::overdue::kWhereSql;
    // 自然日口径（2026-11-16 老板令，与 TodoService::listOverdue 同源）：绑今天零点
    auto overdueTodos = countBySql(overdueCountSql.c_str(), todayStart);
    if (overdueTodos.isFail()) {
        return core::propagate<DashboardStats>(overdueTodos);
    }
    auto todayApiCalls = countBySql(
        "SELECT COUNT(*) AS n FROM claude_call_logs WHERE started_at >= ?",
        todayStart);
    if (todayApiCalls.isFail()) {
        return core::propagate<DashboardStats>(todayApiCalls);
    }
    auto recentNotes = queryRecentNotes(recentNoteLimit);
    if (recentNotes.isFail()) {
        return core::propagate<DashboardStats>(recentNotes);
    }

    DashboardStats stats;
    stats.todayNotes = todayNotes.value();
    stats.pendingTodos = pendingTodos.value();
    stats.overdueTodos = overdueTodos.value();
    stats.todayApiCalls = todayApiCalls.value();
    stats.recentNotes = std::move(recentNotes).value();
    return core::Result<DashboardStats>::ok(std::move(stats));
}

core::Result<LastCallRate> HomeService::getLastCallRate() {
    // 「成功」谓词与 core/call_rate.h 的 isSuccessful 同口径（2xx 且 output_tokens > 0），
    // 两处必须一起改；按 started_at 倒序。
    auto prep = db_.prepare(
        "SELECT status_code, output_tokens, latency_ms, started_at "
        "FROM claude_call_logs "
        "WHERE COALESCE(status_code, 0) BETWEEN 200 AND 299 "
        "AND COALESCE(output_tokens, 0) > 0 "
        "ORDER BY started_at DESC, id DESC LIMIT 1");
    if (prep.isFail()) {
        return core::propagate<LastCallRate>(prep);
    }
    auto step = prep.value().step();
    if (step.isFail()) {
        return core::propagate<LastCallRate>(step);
    }
    LastCallRate out;
    if (!step.value()) {
        return core::Result<LastCallRate>::ok(out);  // 没有成功调用：空态
    }
    auto& stmt = prep.value();
    auto row = stmt.currentRow();
    if (row.isFail()) {
        return core::propagate<LastCallRate>(row);
    }
    auto sc = row.value().getInt64("status_code");
    auto ot = row.value().getInt64("output_tokens");
    auto lm = row.value().getInt64("latency_ms");
    auto sa = row.value().getInt64("started_at");
    if (sc.isFail() || ot.isFail() || lm.isFail()) {
        const auto& e = sc.isFail() ? sc : (ot.isFail() ? ot : lm);
        return core::propagate<LastCallRate>(e);
    }
    out.outputTokens = ot.value();
    out.latencyMs = lm.value();
    if (sa.isOk()) out.startedAt = static_cast<std::time_t>(sa.value());
    // 速率算法（含 latency_ms <= 0 的兜底）单源在 core/call_rate.h
    out.hasValue = core::call_rate::tps(static_cast<int>(sc.value()),
                                        out.outputTokens, out.latencyMs,
                                        out.tps);
    return core::Result<LastCallRate>::ok(out);
}

} // namespace module::home
