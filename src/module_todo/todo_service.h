// src/module_todo/todo_service.h
// Todo 看板业务逻辑层（详见 spec §7.2 / §8.2）
//
// 设计原则：
//   - 所有可能失败的操作返回 core::Result<T>
//   - 通过 core::data::SqliteDb 引用访问底层数据库
//   - 复合操作（moveTask）使用事务保证原子性
//   - tags 字段以 TEXT 存储，序列化为 ["a", "b", ...] 形式的 JSON
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace module::todo {

// 优先级（与 todo_tasks.priority INTEGER 字段映射）
enum class Priority { None = 0, Low = 1, Medium = 2, High = 3 };

// 列（来自 todo_columns 表）
struct Column {
    int64_t id;
    std::string title;
    int position;
    std::optional<std::string> color;
    // 语义标记（migration v3，v8 起含 'archive'）：'done' = 已完成列，
    // 'todo' = 默认待办列，'archive' = 归档列，空 = 普通列。完成态流转与
    // 归档空态文案按它定位目标列，不靠标题字符串。
    std::string kind;
};

// 任务（来自 todo_tasks 表）
struct Task {
    int64_t id;
    int64_t columnId;
    std::string title;
    std::optional<std::string> description;
    Priority priority;
    std::optional<int64_t> dueAt;       // Unix 秒；std::nullopt = 未设置
    std::vector<std::string> tags;       // 来自 JSON 数组
    int position;
    std::optional<int64_t> completedAt;  // Unix 秒；std::nullopt = 未完成
};

class TodoService {
public:
    explicit TodoService(core::data::SqliteDb& db);

    // ============ 列操作 ============
    // 列出所有列（按 position ASC）
    core::Result<std::vector<Column>> listColumns();

    // 新建列（自动追加到末尾 position = max+1）
    core::Result<Column> createColumn(const std::string& title);

    // 重命名列（id 不存在 → NotFound）
    core::Result<void> renameColumn(int64_t id, const std::string& title);

    // 设置列颜色 token（migrations 种子 / 改色预设的名称；nullopt = 清除）
    core::Result<void> setColumnColor(int64_t id, const std::optional<std::string>& color);

    // 删除列（外键 CASCADE 会自动删除其下所有任务）
    core::Result<void> deleteColumn(int64_t id);

    // 重排列顺序（ids 按期望顺序排列，重设为 0..n-1）
    core::Result<void> reorderColumns(const std::vector<int64_t>& ids);

    // ============ 任务操作 ============
    // 列出某列下所有任务（按 position ASC）
    core::Result<std::vector<Task>> listTasks(int64_t columnId);

    // 列出所有任务（按 column_id, position ASC）
    core::Result<std::vector<Task>> listAllTasks();

    // 新建任务（追加到列末尾）
    core::Result<Task> createTask(int64_t columnId, const std::string& title);

    // 整字段更新（只改内容字段；改列 / 改序一律走 moveTask）（id 必须已存在）
    core::Result<void> updateTask(const Task& task);

    // 删除任务
    core::Result<void> deleteTask(int64_t id);

    // 移动任务到新列 + 位置（在事务里重排双侧 position）
    core::Result<void> moveTask(int64_t taskId, int64_t toColumnId, int newPosition);

    // 标记完成（completed_at = now()）
    core::Result<void> completeTask(int64_t id);

    // 取消完成（completed_at = NULL）
    core::Result<void> uncompleteTask(int64_t id);

    // 搜索 title/description 中包含 query 的任务
    core::Result<std::vector<Task>> search(const std::string& query);

    // 列出过期任务（due_at < now 且未完成）
    core::Result<std::vector<Task>> listOverdue();

private:
    core::data::SqliteDb& db_;

    // ============ 内部辅助 ============
    // 获取当前 Unix 秒（无失败路径：std::time 不返回错误）
    static int64_t nowEpochSeconds();

    // 计算新列 position（MAX + 1）
    core::Result<int> nextColumnPosition();

    // 计算某列新任务 position（MAX + 1）
    core::Result<int> nextTaskPosition(int64_t columnId);

    // 把 SqlRow 解析成 Column
    static core::Result<Column> parseColumnRow(const core::data::SqlRow& row);

    // 把 SqlRow 解析成 Task
    core::Result<Task> parseTaskRow(const core::data::SqlRow& row);

    // tags 序列化（vector<string> → JSON 数组字符串）
    static std::string serializeTags(const std::vector<std::string>& tags);

    // tags 反序列化（JSON 数组字符串 → vector<string>）
    static core::Result<std::vector<std::string>> deserializeTags(const std::string& json);
};

} // namespace module::todo