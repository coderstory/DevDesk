// 「逾期」业务规则的唯一口径。core 不依赖 module，故只提供纯常量 / 纯函数，
// 完整 SQL 由调用方拼接。
// 口径：有截止时间 且 截止时间严格早于 now 且 未完成。
#pragma once

#include <cstdint>
#include <optional>

namespace core::overdue {

// SQL 谓词片段（列取自 todo_tasks；? = 当前 epoch 秒占位符，调用方前置 WHERE/AND 并绑定）
inline constexpr const char* kWhereSql =
    "due_at IS NOT NULL AND due_at < ? AND completed_at IS NULL";

// 与 kWhereSql 同口径的内存谓词（已取到行对象时用）；两形态语义必须同步。
// 边界：截止时刻当秒不算逾期，与 SQL 的 "<" 对齐。
inline bool isOverdue(std::optional<int64_t> dueAt,
                      std::optional<int64_t> completedAt,
                      int64_t now) {
    return dueAt.has_value() && dueAt.value() < now &&
           !completedAt.has_value();
}

} // namespace core::overdue
