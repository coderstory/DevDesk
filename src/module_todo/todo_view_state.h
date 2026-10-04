// todo 视图状态跨会话持久化，落在中立的 app_config 表（键 todo_view_state），不进任何其他模块。
// 滚动按列 id 记忆（重排/删列不串列）。
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::todo {

// 默认 app_config 键名（供测试与工具层拼键名；实现与 save/load 同一来源）
inline constexpr const char* kTodoViewStateKey = "todo_view_state";

struct TodoViewState {
    // 列 id → 列内滚动偏移（std::map 保序，json 里列 id 为对象键的十进制字符串）
    std::map<int64_t, float> columnScrolls;
    int64_t selectedTaskId = 0;  // 最近选中的任务（0 = 无）
};

// todo 视图状态读写（不抛异常；键缺失 / 值损坏一律回退默认值）。
// db 允许为 nullptr，但那不是「没配置」而是「数据库没就绪」：save() 返回 fail Result，load() 回退默认并 LOG_ERROR
class TodoViewStateStore {
public:
    explicit TodoViewStateStore(core::data::SqliteDb* db);

    TodoViewState load() const;

    // 与 load() 同源，把「库未就绪 / 表不存在 / 值损坏」如实报出来
    core::Result<TodoViewState> loadChecked() const;

    core::Result<void> save(const TodoViewState& st) const;

    // 净化：滚动偏移负值/非有限值归 0；selectedTaskId 负值归 0
    static TodoViewState sanitized(const TodoViewState& st);

    // 剪掉已不存在列的滚动记录（reloadAll 收口用；返回删除条数）
    static size_t pruneScrolls(std::map<int64_t, float>& scrolls,
                               const std::vector<int64_t>& liveColumnIds);

private:
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace module::todo
