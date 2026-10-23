// 「逾期」业务规则的唯一口径。core 不依赖 module，故只提供纯常量 / 纯函数，
// 完整 SQL 由调用方拼接。
// 口径：有截止时间 且 截止时间严格早于 now 且 未完成，且任务在「待办 / 进行中」列
// （UAT6.2 H20 逾期收窄：归档、已完成、自定义列里的过期未完成任务不计逾期）。
#pragma once

#include <cstdint>
#include <optional>

namespace core::overdue {

// SQL 谓词片段（列取自 todo_tasks；? = 当前 epoch 秒占位符，调用方前置 WHERE/AND 并绑定）。
// 列限定用子查询而非 join：调用方都写成 `FROM todo_tasks WHERE <kWhereSql>`（无别名），
// 不加 join/别名即可直接拼。
inline constexpr const char* kWhereSql =
    "due_at IS NOT NULL AND due_at < ? AND completed_at IS NULL "
    "AND column_id IN (SELECT id FROM todo_columns WHERE kind IN ('todo','doing'))";

// 与 kWhereSql 同口径的内存谓词（已取到行对象时用）；两形态语义必须同步。
// 注意：这里不含列限定（行对象没有列 kind）——调用方必须由 SQL 先按
// `column_id IN (... kind IN ('todo','doing'))` 过滤，再调用本谓词做第三只眼校验。
// 边界：截止时刻当秒不算逾期，与 SQL 的 "<" 对齐。
inline bool isOverdue(std::optional<int64_t> dueAt,
                      std::optional<int64_t> completedAt,
                      int64_t now) {
    return dueAt.has_value() && dueAt.value() < now &&
           !completedAt.has_value();
}

} // namespace core::overdue
