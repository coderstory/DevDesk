// src/module_home/home_service.h
// Home 仪表盘数据服务（spec §8.4）
//
// 依赖规则遵守：HomeService 只依赖 core::data::SqliteDb，
// 不依赖任何 module_*（markdown_index / todo_tasks / claude_call_logs
// 都是 core::migrations 管理的稳定 schema，可独立只读查询）。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace module::home {

// 最近编辑的笔记条目（markdown_index 行的投影）
struct RecentNote {
    std::string relativePath;  // POSIX 风格相对路径
    std::string title;         // 可能为空（UI 回退显示文件名）
    int64_t modifiedAt = 0;    // unix epoch seconds
};

// 仪表盘统计（spec §8.4 DashboardStats 的 P12 需要子集）
struct DashboardStats {
    int64_t todayNotes = 0;     // 今日笔记（markdown_index 当天有 mtime）
    int64_t pendingTodos = 0;   // 待办任务（todo_tasks 未完成总数）
    int64_t overdueTodos = 0;   // 逾期任务（due_at < now 且未完成）
    int64_t todayApiCalls = 0;  // API 调用（claude_call_logs 当天条数）
    std::vector<RecentNote> recentNotes;  // 最近编辑（mtime 倒序）
};

// ===== 纯函数（可单测，无 IO）=====

// 本地时区某时刻所在日的 00:00 epoch 秒（"今日"的统一起点）
int64_t startOfDayEpoch(std::time_t t);

// 相对时间描述："刚刚" / "N 分钟前" / "N 小时前" / "昨天" / "N 天前" / "YYYY-MM-DD"
std::string formatRelativeTime(int64_t modifiedAt, std::time_t now);

// 中文日期行："2026 年 9 月 22 日 · 星期二"
std::string formatTodayCn(std::time_t now);

// Token 速率卡片纯函数（设计 §4 方案 A）
// 累计缩写：0→"0"，12400→"12.4k"，1234567→"1.2M"
std::string formatTokenCount(int64_t tokens);
// 速率：0→"0"，<100 保留 1 位小数（47.25→"47.3"），>=100 整数（123.6→"124"）
std::string formatRateTps(double tps);

// Home 仪表盘只读聚合查询
class HomeService {
public:
    explicit HomeService(core::data::SqliteDb& db);

    // 聚合查询：任一子查询失败 → 整体 fail（UI 显示加载失败，spec §8.4）
    // recentNoteLimit <= 0 时按 5 处理
    core::Result<DashboardStats> getStats(int recentNoteLimit = 5);

    // UI 现代化 M3-1（2026-09-28）：当日 API 调用逐时分布（barChart 数据源）。
    // 纯新增查询不动既有字段：当日（本地零点起）按小时聚合 claude_call_logs，
    // 返回恰 hours 个点（默认 24），无调用的小时补 0——序列恒非空（防
    // barChart 对空 values 填演示数据，barchart.h build() 的 D-1..D-4 兜底）
    core::Result<std::vector<int64_t>> getTodayApiCallSeries(int hours = 24);

private:
    core::data::SqliteDb& db_;

    // 单值 COUNT 查询（sql 必须形如 "SELECT COUNT(*) AS n ..."）
    // boundValue 有值（THS-10）→ 绑定到第 1 个占位符；无 WHERE 绑定值的查询
    // 省略实参（默认 nullopt）。
    core::Result<int64_t> countBySql(
        const char* sql, std::optional<int64_t> boundValue = std::nullopt);

    // 最近编辑笔记（modified_at DESC）
    core::Result<std::vector<RecentNote>> queryRecentNotes(int limit);
};

} // namespace module::home
