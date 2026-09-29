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

// 最近一次成功调用的速率快照（真机反馈 ①：「Token 速率」卡改成最近一次调用的
// 速率）。hasValue=false 有三种来源，UI 一律走空态（number "--"）：
//   · 一条成功调用都没有（status 2xx 且 output_tokens > 0 的行不存在）
//   · 该行的 latency_ms <= 0（耗时未采集 → 无法算速率）
//   · 查询失败（错误经 Result 单独回传，UI 显示「加载失败」）
// 速率算法与成功判定见 core/call_rate.h（与 ② 明细列同一口径）。
struct LastCallRate {
    bool hasValue = false;
    double tps = 0.0;         // tok/s（仅 hasValue 时有效）
    int64_t outputTokens = 0; // 该次调用的输出 token
    int64_t latencyMs = 0;    // 该次调用的接口耗时
    std::time_t startedAt = 0;
};

// 去头去尾（真机反馈 ③）：逐时序列里「第一个有数据的小时」..「最后一个有
// 数据的小时」**闭区间**。有数据 = 该小时合计 > 0（与图表纵轴同口径：0 等于
// 没数据，不画）——注意不是「该小时有调用」：只有调用的小时若没采集到 token
// 合计为 0，与没调用一样是空小时。
// 全零（整天无数据）→ hasData=false，调用方走空态、不画空轴。
struct HourRange {
    bool hasData = false;
    size_t first = 0;  // 闭区间左端（小时下标）
    size_t last = 0;   // 闭区间右端
};

// 逐时序列 → 去头去尾区间（纯函数，可单测）
HourRange hourlyDataRange(const std::vector<int64_t>& hourly);

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
// 注（① 2026-09-29 改口径后）：卡片数字已改用单次调用口径
//（core/call_rate.h::formatTps，恒 1 位小数），本函数只剩实时速率的纯函数
// 用例在用 —— 保留是因为实时速率（2s 窗口）仍是 sparkline 的数据源，
// 量级档位口径随它一起留着，别当死代码删。
std::string formatRateTps(double tps);

// Home 仪表盘只读聚合查询
class HomeService {
public:
    explicit HomeService(core::data::SqliteDb& db);

    // 聚合查询：任一子查询失败 → 整体 fail（UI 显示加载失败，spec §8.4）
    // recentNoteLimit <= 0 时按 5 处理
    core::Result<DashboardStats> getStats(int recentNoteLimit = 5);

    // 最近一次成功调用的速率（真机反馈 ①）：按 started_at DESC, id DESC 取
    // 第一条「2xx 且 output_tokens > 0」的调用，速率 = output_tokens /
    // (latency_ms / 1000)（口径单源 core/call_rate.h）。
    // 没有这样的行 → hasValue=false（**不是**错误）。
    // 旧口径（2s 窗口实时速率）仍是 live 快照 core::proxy_stats 的事，
    // 本查询只回答「最近一次调用的速率是多少」。
    core::Result<LastCallRate> getLastCallRate();

    // 当日逐时 token 合计（真机反馈 ③：分布图的纵轴从「归一百分比」改成
    // 「每小时 output_tokens 合计」）。当日（本地零点起）按小时聚合
    // claude_call_logs 的 output_tokens（未采集 = NULL 按 0），返回恰 hours
    // 个点（默认 24），无调用 / 无 token 的小时补 0。去头去尾在
    // hourlyDataRange（纯函数）里做，本查询只负责逐时合计，不裁剪——裁剪是
    // 展示口径，不该藏进数据层。
    core::Result<std::vector<int64_t>> getTodayHourlyTokenTotals(int hours = 24);

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
