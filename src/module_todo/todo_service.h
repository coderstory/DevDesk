// Todo 看板业务逻辑层。所有可能失败的操作返回 core::Result<T>；复合操作（moveTask）走事务保证原子性；
// tags 以 TEXT 存 JSON 数组（["a","b",…]）。经 core::data::SqliteDb 引用访问数据库。
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

struct Column {
    int64_t id;
    std::string title;
    int position;
    std::optional<std::string> color;
    // 语义标记：'todo'/'doing'/'done'/'archive'（固定四列）；空 = 老库普通列。
    // 完成/归档流转按它定位目标列，不靠标题字符串；迁移后必有固定四列。
    std::string kind;
};

struct Task {
    // 原生字段带默认值：测试/调用方普遍「默认构造再逐字段赋值」，漏赋的字段
    // （如 priority）在 -O3 拷贝路径上触发 GCC16 -Werror=uninitialized，且未
    // 初始化的比较本身是 UB。类类型字段（string/optional/vector）自带默认。
    int64_t id = 0;
    int64_t columnId = 0;
    std::string title;
    std::optional<std::string> description;
    Priority priority = Priority::None;
    std::optional<int64_t> dueAt;       // Unix 秒；std::nullopt = 未设置
    std::vector<std::string> tags;       // 来自 JSON 数组
    int position = 0;
    std::optional<int64_t> completedAt;  // Unix 秒；std::nullopt = 未完成
    std::optional<int64_t> archivedAt;   // Unix 秒；std::nullopt = 未归档（进归档列时置当前时间戳，出归档列置 NULL）
};

// 新建任务时可一次给定的内容属性（std::nullopt = 不设置）。
// 为什么单独成型而不是给 createTask 堆三个默认形参：属性是一个整体（「带上属性建任务」
// 只有一种语义），成组传递让调用点自解释，也避免 future 加字段时再改签名。
// 只放内容字段：column/position 由 createTask 入参决定，completedAt/archivedAt 归
// completeTask/moveTask 管，tags 暂无入参（本票未要求，不要顺手加）。
struct NewTaskAttributes {
    std::optional<std::string> description;
    Priority priority = Priority::None;
    std::optional<int64_t> dueAt;  // Unix 秒
};

class TodoService {
public:
    explicit TodoService(core::data::SqliteDb& db);

    // 列出所有列（固定顺序：position ASC, id ASC；迁移后即 待办/进行中/已完成/已归档）
    core::Result<std::vector<Column>> listColumns();

    // 新建列（自动追加到末尾 position = max+1）
    core::Result<Column> createColumn(const std::string& title);

    // 重命名列（id 不存在 → NotFound）
    core::Result<void> renameColumn(int64_t id, const std::string& title);

    // 设置列颜色 token（schema 种子 / 改色预设的名称；nullopt = 清除）
    core::Result<void> setColumnColor(int64_t id, const std::optional<std::string>& color);

    // 删除列（外键 CASCADE 会自动删除其下所有任务）
    core::Result<void> deleteColumn(int64_t id);

    // 重排列顺序（ids 按期望顺序排列，重设为 0..n-1）
    core::Result<void> reorderColumns(const std::vector<int64_t>& ids);

    // 列出某列下所有任务（按 position ASC）
    core::Result<std::vector<Task>> listTasks(int64_t columnId);

    // 列出所有任务（按 column_id, position ASC）
    core::Result<std::vector<Task>> listAllTasks();

    // 新建任务（追加到列末尾）—— 只给标题的既有入口，签名与语义都不动
    core::Result<Task> createTask(int64_t columnId, const std::string& title);

    // 新建任务 + 内容属性：属性与标题走同一条 INSERT 一次落库，天然没有「任务建成了但
    // 属性还在半路」的中间态（二跳 update 的失败形态，见票面 01）
    core::Result<Task> createTask(int64_t columnId, const std::string& title,
                                  const NewTaskAttributes& attrs);

    // 整字段更新（只改内容字段；改列 / 改序一律走 moveTask）（id 必须已存在）
    core::Result<void> updateTask(const Task& task);

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

    // 获取当前 Unix 秒（std::time 不返回错误，无失败路径）
    static int64_t nowEpochSeconds();

    // 计算新列 position（MAX + 1）
    core::Result<int> nextColumnPosition();

    // 计算某列新任务 position（MAX + 1）
    core::Result<int> nextTaskPosition(int64_t columnId);

    static core::Result<Column> parseColumnRow(const core::data::SqlRow& row);

    core::Result<Task> parseTaskRow(const core::data::SqlRow& row);

    // tags 序列化（vector<string> → JSON 数组字符串）
    static std::string serializeTags(const std::vector<std::string>& tags);

    // tags 反序列化（JSON 数组字符串 → vector<string>）
    static core::Result<std::vector<std::string>> deserializeTags(const std::string& json);
};

} // namespace module::todo