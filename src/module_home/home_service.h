// src/module_home/home_service.h
// Home 仪表盘数据服务：只依赖 core::data::SqliteDb，不依赖任何 module_*（查询的表
// 都是 core::schema 定义的稳定 schema，可独立只读查询）。
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

// 最近一次成功调用的速率快照。hasValue=false 三种来源（UI 一律走空态 "--"）：
// 无成功调用 / latency_ms <= 0（无法算速率）/ 查询失败（错误经 Result 单独回传）。
// 速率算法与成功判定单源 core/call_rate.h。
struct LastCallRate {
    bool hasValue = false;
    double tps = 0.0;         // tok/s（仅 hasValue 时有效）
    int64_t outputTokens = 0; // 该次调用的输出 token
    int64_t latencyMs = 0;    // 该次调用的接口耗时
    std::time_t startedAt = 0;
};

// 去头去尾：逐时序列里「第一个有数据的小时」..「最后一个」闭区间。有数据 = 该
// 小时合计 > 0；全零 → hasData=false，调用方走空态不画空轴。
struct HourRange {
    bool hasData = false;
    size_t first = 0;  // 闭区间左端（小时下标）
    size_t last = 0;   // 闭区间右端
};

HourRange hourlyDataRange(const std::vector<int64_t>& hourly);

struct DashboardStats {
    int64_t todayNotes = 0;     // 今日笔记（markdown_index 当天有 mtime）
    int64_t pendingTodos = 0;   // 待办任务（todo_tasks 未完成总数）
    int64_t overdueTodos = 0;   // 逾期任务（due_at < now 且未完成）
    int64_t todayApiCalls = 0;  // API 调用（claude_call_logs 当天条数）
    std::vector<RecentNote> recentNotes;  // 最近编辑（mtime 倒序）
};

// 本地时区某时刻所在日的 00:00 epoch 秒（"今日"的统一起点）
int64_t startOfDayEpoch(std::time_t t);

// 相对时间描述："刚刚" / "N 分钟前" / "N 小时前" / "昨天" / "N 天前" / "YYYY-MM-DD"
std::string formatRelativeTime(int64_t modifiedAt, std::time_t now);

std::string formatTodayCn(std::time_t now);

// 累计缩写：0→"0"，12400→"12.4k"，1234567→"1.2M"
std::string formatTokenCount(int64_t tokens);
// 速率格式：0→"0"，<100 保留 1 位小数，>=100 整数。
// 卡片数字已改用单次调用口径（core/call_rate.h::formatTps），本函数只剩实时速率
//（2s 窗口，sparkline 数据源）的用例在用，勿当死代码删。
std::string formatRateTps(double tps);

class HomeService {
public:
    explicit HomeService(core::data::SqliteDb& db);

    // 聚合查询：任一子查询失败 → 整体 fail；recentNoteLimit <= 0 按 5 处理。
    core::Result<DashboardStats> getStats(int recentNoteLimit = 5);

    // 最近一次成功调用的速率：按 started_at DESC, id DESC 取首条「2xx 且
    // output_tokens > 0」；速率单源 core/call_rate.h。无此行 → hasValue=false（非错误）。
    core::Result<LastCallRate> getLastCallRate();

    // 当日（本地零点起）按小时聚合 output_tokens（未采集 = NULL 按 0），恒返回 hours
    // 个点。去头去尾在 hourlyDataRange 里做——裁剪是展示口径，不藏进数据层。
    core::Result<std::vector<int64_t>> getTodayHourlyTokenTotals(int hours = 24);

private:
    core::data::SqliteDb& db_;

    // 单值 COUNT 查询（sql 须形如 "SELECT COUNT(*) AS n ..."）；boundValue 有值则绑定第 1 个占位符。
    core::Result<int64_t> countBySql(
        const char* sql, std::optional<int64_t> boundValue = std::nullopt);

    core::Result<std::vector<RecentNote>> queryRecentNotes(int limit);
};

} // namespace module::home
