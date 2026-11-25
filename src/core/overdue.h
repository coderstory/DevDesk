// 「逾期」业务规则的唯一口径。core 不依赖 module，故只提供纯常量 / 纯函数，
// 完整 SQL 由调用方拼接。
// 口径（2026-11-16 老板令改自然日）：有截止日 且 截止【日】早于今天 且 未完成，
// 且任务在「待办 / 进行中」列（UAT6.2 H20 逾期收窄：归档、已完成、自定义列里的
// 过期未完成任务不计逾期）。
// 边界：按自然日比较——「截止 16 号、今天 16 号」不算逾期，到 17 号才算（旧口径
// 按秒比 now，16 号下午就会误判逾期，已被老板实测推翻）。
#pragma once

#include <cstdint>
#include <optional>

#include "core/time.h"

namespace core::overdue {

// SQL 谓词片段（列取自 todo_tasks；? = **今天本地零点** epoch 秒，调用方前置
// WHERE/AND 并绑定 core::time::localMidnightUnix(now)——不绑 now 本身，否则当天
// 截止的任务会在当天下午被误判逾期）。
// 列限定用子查询而非 join：调用方都写成 `FROM todo_tasks WHERE <kWhereSql>`（无别名），
// 不加 join/别名即可直接拼。
inline constexpr const char* kWhereSql =
    "due_at IS NOT NULL AND due_at < ? AND completed_at IS NULL "
    "AND column_id IN (SELECT id FROM todo_columns WHERE kind IN ('todo','doing'))";

// 「到期即提醒」谓词（need453 待办到期提醒）：秒级口径——dueAt 已过即命中，
// 与上方自然日「逾期」区分（逾期 = 过了截止日，本谓词 = 到了截止时刻）。
// 列范围与 kWhereSql 一致：待办/进行中列；完成列自动置 completed_at、归档列
// 已出范围，archived_at 显式排除属双保险。? = now（Unix 秒）。
inline constexpr const char* kDueNowWhereSql =
    "due_at IS NOT NULL AND due_at <= ? AND completed_at IS NULL "
    "AND archived_at IS NULL "
    "AND column_id IN (SELECT id FROM todo_columns WHERE kind IN ('todo','doing'))";

// 与 kWhereSql 同口径的内存谓词（已取到行对象时用）；两形态语义必须同步。
// 注意：这里不含列限定（行对象没有列 kind）——调用方必须由 SQL 先按
// `column_id IN (... kind IN ('todo','doing'))` 过滤，再调用本谓词做第三只眼校验。
// 边界：自然日——截止日 < 今天才逾期；当天截止不算（与 SQL 绑 todayStart 对齐）。
inline bool isOverdue(std::optional<int64_t> dueAt,
                      std::optional<int64_t> completedAt,
                      int64_t now) {
    if (!dueAt.has_value() || completedAt.has_value()) return false;
    if (dueAt.value() <= 0 || now <= 0) return false;
    return core::time::localDayNumber(static_cast<std::time_t>(dueAt.value())) <
           core::time::localDayNumber(static_cast<std::time_t>(now));
}

// 今日到期（未完成、且不在逾期口径内）：截止日 == 今天。UI 侧卡面橙黄警示用。
inline bool isDueToday(std::optional<int64_t> dueAt,
                       std::optional<int64_t> completedAt,
                       int64_t now) {
    if (!dueAt.has_value() || completedAt.has_value()) return false;
    if (dueAt.value() <= 0 || now <= 0) return false;
    return core::time::localDayNumber(static_cast<std::time_t>(dueAt.value())) ==
           core::time::localDayNumber(static_cast<std::time_t>(now));
}

} // namespace core::overdue