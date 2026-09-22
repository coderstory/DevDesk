// src/module_home/home_service.cpp
// Home 仪表盘数据服务实现（spec §8.4）
#include "home_service.h"

#include "core/logging.h"
#include "core/time.h"

#include <cstdio>

namespace module::home {

// ===== 纯函数 =====

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

    // 先按本地日界分桶（同一天内才显示 "N 小时前"）
    const int64_t todayStart = startOfDayEpoch(now);
    const int64_t modDayStart =
        startOfDayEpoch(static_cast<std::time_t>(modifiedAt));
    const int64_t dayGap = (todayStart - modDayStart) / 86400;

    if (dayGap <= 0) {
        if (diff < 3600) {
            return std::to_string(diff / 60) + " 分钟前";
        }
        return std::to_string(diff / 3600) + " 小时前";
    }
    if (dayGap == 1) return "昨天";
    if (dayGap < 7) return std::to_string(dayGap) + " 天前";

    // 7 天以上 → 具体日期
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(modifiedAt), &local);
    char buf[16];
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

// ===== HomeService =====

HomeService::HomeService(core::data::SqliteDb& db) : db_(db) {}

core::Result<int64_t> HomeService::countBySql(const char* sql) {
    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
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

core::Result<int64_t> HomeService::countBySql(const char* sql,
                                              int64_t boundValue) {
    auto prep = db_.prepare(sql);
    if (prep.isFail()) {
        return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, boundValue); b.isFail()) {
        return core::Result<int64_t>::fail(b.errorCode(), b.errorMessage());
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

    // 逾期口径与 TodoService::listOverdue 完全一致：
    // due_at IS NOT NULL AND due_at < now AND completed_at IS NULL
    auto todayNotes = countBySql(
        "SELECT COUNT(*) AS n FROM markdown_index WHERE modified_at >= ?",
        todayStart);
    if (todayNotes.isFail()) {
        return core::Result<DashboardStats>::fail(todayNotes.errorCode(),
                                                  todayNotes.errorMessage());
    }
    auto pendingTodos = countBySql(
        "SELECT COUNT(*) AS n FROM todo_tasks WHERE completed_at IS NULL");
    if (pendingTodos.isFail()) {
        return core::Result<DashboardStats>::fail(pendingTodos.errorCode(),
                                                  pendingTodos.errorMessage());
    }
    auto overdueTodos = countBySql(
        "SELECT COUNT(*) AS n FROM todo_tasks "
        "WHERE due_at IS NOT NULL AND due_at < ? AND completed_at IS NULL",
        nowSecs);
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

} // namespace module::home
