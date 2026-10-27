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

std::string formatTodayCn(std::time_t now) {
    static const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三",
                                      "星期四", "星期五", "星期六"};
    std::tm local{};
    core::time::localTime(now, &local);
    return std::to_string(local.tm_year + 1900) + " 年 " +
           std::to_string(local.tm_mon + 1) + " 月 " +
           std::to_string(local.tm_mday) + " 日 · " + kWeekdays[local.tm_wday];
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
        return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (boundValue.has_value()) {
        if (auto b = stmt.bind(1, *boundValue); b.isFail()) {
            return core::Result<int64_t>::fail(b.errorCode(), b.errorMessage());
        }
    }
    auto step = stmt.step();
    if (step.isFail()) {
        return core::Result<int64_t>::fail(step.errorCode(), step.errorMessage());
    }
    if (!step.value()) {
        return core::Result<int64_t>::fail(core::ErrorCode::Internal,
                                           "count 查询没有返回行");
    }
    auto row = stmt.currentRow();
    if (row.isFail()) {
        return core::Result<int64_t>::fail(row.errorCode(), row.errorMessage());
    }
    auto n = row.value().getInt64("n");
    if (n.isFail()) {
        return core::Result<int64_t>::fail(n.errorCode(), n.errorMessage());
    }
    return core::Result<int64_t>::ok(n.value());
}

core::Result<std::vector<RecentNote>> HomeService::queryRecentNotes(int limit) {
    auto prep = db_.prepare(
        "SELECT path, title, modified_at FROM markdown_index "
        "ORDER BY modified_at DESC, id DESC LIMIT ?");
    if (prep.isFail()) {
        return core::Result<std::vector<RecentNote>>::fail(prep.errorCode(),
                                                           prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, static_cast<int64_t>(limit)); b.isFail()) {
        return core::Result<std::vector<RecentNote>>::fail(b.errorCode(),
                                                           b.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<RecentNote>>::fail(rows.errorCode(),
                                                           rows.errorMessage());
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
    const int64_t nowSecs = static_cast<int64_t>(now);

    // 逾期口径单源 core/overdue.h 的 kWhereSql（与 TodoService::listOverdue 共用）。
    auto todayNotes = countBySql(
        "SELECT COUNT(*) AS n FROM markdown_index WHERE modified_at >= ?",
        todayStart);
    if (todayNotes.isFail()) {
        return core::Result<DashboardStats>::fail(todayNotes.errorCode(),
                                                  todayNotes.errorMessage());
    }
    // 待办任务口径 = 「待办列 + 进行中列」里未完成的任务数，不是全部未完成任务。
    // 只认列 kind（'todo'/'doing'）：schema.h 契约禁止按标题匹配（改名即静默失效），
    // H20 迁移已把历史「进行中」列补成 kind='doing'；done/archive/kind NULL 列不计。
    auto pendingTodos = countBySql(
        "SELECT COUNT(*) AS n FROM todo_tasks t JOIN todo_columns c "
        "ON c.id = t.column_id WHERE t.completed_at IS NULL "
        "AND c.kind IN ('todo','doing')");
    if (pendingTodos.isFail()) {
        return core::Result<DashboardStats>::fail(pendingTodos.errorCode(),
                                                  pendingTodos.errorMessage());
    }
    const std::string overdueCountSql =
        std::string("SELECT COUNT(*) AS n FROM todo_tasks WHERE ") +
        core::overdue::kWhereSql;
    auto overdueTodos = countBySql(overdueCountSql.c_str(), nowSecs);
    if (overdueTodos.isFail()) {
        return core::Result<DashboardStats>::fail(overdueTodos.errorCode(),
                                                  overdueTodos.errorMessage());
    }
    auto todayApiCalls = countBySql(
        "SELECT COUNT(*) AS n FROM claude_call_logs WHERE started_at >= ?",
        todayStart);
    if (todayApiCalls.isFail()) {
        return core::Result<DashboardStats>::fail(todayApiCalls.errorCode(),
                                                  todayApiCalls.errorMessage());
    }
    auto recentNotes = queryRecentNotes(recentNoteLimit);
    if (recentNotes.isFail()) {
        return core::Result<DashboardStats>::fail(recentNotes.errorCode(),
                                                  recentNotes.errorMessage());
    }

    DashboardStats stats;
    stats.todayNotes = todayNotes.value();
    stats.pendingTodos = pendingTodos.value();
    stats.overdueTodos = overdueTodos.value();
    stats.todayApiCalls = todayApiCalls.value();
    stats.recentNotes = std::move(recentNotes).value();
    return core::Result<DashboardStats>::ok(std::move(stats));
}

core::Result<std::vector<int64_t>> HomeService::getTodayHourlyTokenTotals(
    int hours) {
    if (hours <= 0) hours = 24;
    const std::time_t now = std::time(nullptr);
    const int64_t dayStart = startOfDayEpoch(now);
    const int64_t dayEnd = dayStart + 86400;
    // 小时下标 = (started_at - 本地零点) / 3600，越界落点按 [0, hours) 过滤；
    // COALESCE：全未采集行 SUM 返回 NULL，与「0 token」同义。
    auto prep = db_.prepare(
        "SELECT (started_at - ?) / 3600 AS h, "
        "COALESCE(SUM(output_tokens), 0) AS n "
        "FROM claude_call_logs "
        "WHERE started_at >= ? AND started_at < ? GROUP BY h");
    if (prep.isFail()) {
        return core::Result<std::vector<int64_t>>::fail(prep.errorCode(),
                                                        prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, dayStart); b.isFail()) {
        return core::Result<std::vector<int64_t>>::fail(b.errorCode(),
                                                        b.errorMessage());
    }
    if (auto b = stmt.bind(2, dayStart); b.isFail()) {
        return core::Result<std::vector<int64_t>>::fail(b.errorCode(),
                                                        b.errorMessage());
    }
    if (auto b = stmt.bind(3, dayEnd); b.isFail()) {
        return core::Result<std::vector<int64_t>>::fail(b.errorCode(),
                                                        b.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) {
        return core::Result<std::vector<int64_t>>::fail(rows.errorCode(),
                                                        rows.errorMessage());
    }
    // 恒返回恰 hours 个点（无调用的小时补 0），序列恒非空。
    std::vector<int64_t> series(static_cast<size_t>(hours), 0);
    for (const auto& row : rows.value()) {
        auto hRaw = row.getInt64("h");
        auto nRaw = row.getInt64("n");
        if (hRaw.isFail() || nRaw.isFail()) {
            return core::Result<std::vector<int64_t>>::fail(
                hRaw.isFail() ? hRaw.errorCode() : nRaw.errorCode(),
                hRaw.isFail() ? hRaw.errorMessage() : nRaw.errorMessage());
        }
        const int64_t idx = hRaw.value();
        if (idx >= 0 && idx < hours) {
            series[static_cast<size_t>(idx)] = nRaw.value();
        }
    }
    return core::Result<std::vector<int64_t>>::ok(std::move(series));
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
        return core::Result<LastCallRate>::fail(prep.errorCode(),
                                                prep.errorMessage());
    }
    auto step = prep.value().step();
    if (step.isFail()) {
        return core::Result<LastCallRate>::fail(step.errorCode(),
                                                step.errorMessage());
    }
    LastCallRate out;
    if (!step.value()) {
        return core::Result<LastCallRate>::ok(out);  // 没有成功调用：空态
    }
    auto& stmt = prep.value();
    auto row = stmt.currentRow();
    if (row.isFail()) {
        return core::Result<LastCallRate>::fail(row.errorCode(),
                                                row.errorMessage());
    }
    auto sc = row.value().getInt64("status_code");
    auto ot = row.value().getInt64("output_tokens");
    auto lm = row.value().getInt64("latency_ms");
    auto sa = row.value().getInt64("started_at");
    if (sc.isFail() || ot.isFail() || lm.isFail()) {
        const auto& e = sc.isFail() ? sc : (ot.isFail() ? ot : lm);
        return core::Result<LastCallRate>::fail(e.errorCode(),
                                                e.errorMessage());
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

HourRange hourlyDataRange(const std::vector<int64_t>& hourly) {
    HourRange r;
    for (size_t i = 0; i < hourly.size(); ++i) {
        if (hourly[i] > 0) {
            if (!r.hasData) {
                r.hasData = true;
                r.first = i;
            }
            r.last = i;
        }
    }
    return r;
}

} // namespace module::home
