// src/core/overdue.h
// 「逾期」业务规则的唯一口径（THS-04）
//
// 背景：这条规则此前在仓内各自维护——首页 HomeService::getStats 的聚合 SQL 与
// TodoService::listOverdue 的列表 SQL 各写了一遍同样的 WHERE 片段。任一处改了
// 列名或比较口径，首页统计卡与 Todo 页逾期徽标就会给出不同数字，且无从判断
// 谁对。口径收进 core 单源：core 不依赖任何 module，只能提供纯常量 / 纯函数
// （完整 SQL 由调用方与自身 SELECT 前缀拼接）。
//
// 口径：有截止时间 且 截止时间已过（严格早于 now） 且 未完成。
#pragma once

#include <cstdint>
#include <optional>

namespace core::overdue {

// SQL 谓词片段（列名取自 todo_tasks；? = 当前 epoch 秒的绑定占位符）。
// 调用方负责前置 "WHERE "（或 "AND "）并绑定该占位符。
inline constexpr const char* kWhereSql =
    "due_at IS NOT NULL AND due_at < ? AND completed_at IS NULL";

// 与 kWhereSql 同口径的内存谓词（已取到行对象时用，不必回 SQL）。
// 两形态语义必须一致，改一处要同步另一处（跨模块一致性用例把二者钉在一起）。
// 边界是严格小于：截止时刻当秒不算逾期，与 SQL 的 "<" 对齐。
// 注：Todo 视图行内仍有一份等价实现（todo_module.cpp 的 isOverdue），待迁移。
inline bool isOverdue(std::optional<int64_t> dueAt,
                      std::optional<int64_t> completedAt,
                      int64_t now) {
    return dueAt.has_value() && dueAt.value() < now &&
           !completedAt.has_value();
}

} // namespace core::overdue
