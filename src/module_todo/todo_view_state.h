// src/module_todo/todo_view_state.h
// 审计 [23]：todo 视图状态跨会话持久化 —— <appDataDir>/todo_view_state.json。
// 模块隔离：module_* 互不依赖，不进 module_settings 的 SettingsConfig（方案 B），
// 照抄 pet_config 模式：路径构造注入、缺失/损坏回退默认、save 走 Result。
// 滚动按列 id 记忆（审计 [23]：旧 columnScrolls_ 按列索引，重排/删列后串列）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

#include "core/result.h"

namespace module::todo {

struct TodoViewState {
    // 列 id → 列内滚动偏移（std::map 保序，json 里列 id 为对象键的十进制字符串）
    std::map<int64_t, float> columnScrolls;
    int64_t selectedTaskId = 0;  // 最近选中的任务（0 = 无）
};

// 默认文件位置（core::paths::appDataDir() 下；测试经构造函数注入临时文件）
std::filesystem::path todoViewStateFile();

// todo_view_state.json 读写（不抛异常；文件不存在/损坏一律回退默认值）
class TodoViewStateStore {
public:
    explicit TodoViewStateStore(std::filesystem::path file);

    TodoViewState load() const;
    core::Result<void> save(const TodoViewState& st) const;

    // 净化：滚动偏移负值/非有限值归 0；selectedTaskId 负值归 0
    static TodoViewState sanitized(const TodoViewState& st);

    // 剪掉已不存在列的滚动记录（reloadAll 收口用；返回删除条数）
    static size_t pruneScrolls(std::map<int64_t, float>& scrolls,
                               const std::vector<int64_t>& liveColumnIds);

private:
    std::filesystem::path file_;
};

} // namespace module::todo
