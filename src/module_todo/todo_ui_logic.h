// src/module_todo/todo_ui_logic.h
// Todo UI 可单测纯函数（P7a 键盘移动 + 截止日期换算）。
// 不持有状态、不碰 DB，输入输出均可离线断言。
#pragma once

#include "todo_service.h"

#include <algorithm>
#include <ctime>
#include <optional>
#include <vector>

#include "core/time.h"

namespace module::todo::ui {

// 键盘移动的目标（columnId + moveTask 的 newPosition 语义）
struct MoveTarget {
    int64_t columnId;
    int position;
};

// 计算 Ctrl/Cmd+↑↓ 的移动目标。
// moveTask 的 newPosition 语义 = "插到 position == newPosition 的任务之前"：
//   上移 → 挪到紧邻上方任务的位置（它会被挤下去）
//   下移 → 挪到紧邻下方任务的位置 + 1（它保持原位，本任务落到其后）
// 已在顶部/底部、任务不存在 → nullopt。
// 直接用 DB 里的真实 position 计算（moveTask 会留下空洞，rank 不可靠）。
inline std::optional<MoveTarget> computeKeyboardMove(
    const std::vector<Task>& tasks, int64_t taskId, bool up) {
    const Task* self = nullptr;
    for (const auto& t : tasks) {
        if (t.id == taskId) {
            self = &t;
            break;
        }
    }
    if (self == nullptr) return std::nullopt;

    std::vector<const Task*> colTasks;
    for (const auto& t : tasks) {
        if (t.columnId == self->columnId) colTasks.push_back(&t);
    }
    std::sort(colTasks.begin(), colTasks.end(),
              [](const Task* a, const Task* b) { return a->position < b->position; });

    size_t idx = 0;
    bool found = false;
    for (size_t i = 0; i < colTasks.size(); ++i) {
        if (colTasks[i]->id == taskId) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) return std::nullopt;

    if (up) {
        if (idx == 0) return std::nullopt;
        return MoveTarget{self->columnId, colTasks[idx - 1]->position};
    }
    if (idx + 1 >= colTasks.size()) return std::nullopt;
    return MoveTarget{self->columnId, colTasks[idx + 1]->position + 1};
}

// 目标列末尾追加位置（该列 max position + 1；空列 = 0）。
// 拖拽 / 详情移动 / 完成迁移等"无目标索引"的移动统一追加到列尾；
// 用 DB 真实 position 计算（moveTask 留空洞，rank 不可靠，同 computeKeyboardMove）。
inline int computeAppendPosition(const std::vector<Task>& tasks, int64_t columnId) {
    int maxPos = -1;
    for (const auto& t : tasks) {
        if (t.columnId == columnId && t.position > maxPos) maxPos = t.position;
    }
    return maxPos + 1;
}

// 年月日（本地时区零点）→ Unix 秒
inline int64_t ymdToEpoch(int year, int month, int day) {
    std::tm tmv{};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = 12;  // 正午，避开 DST 切换日的边界
    tmv.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tmv)) - 12 * 3600;
}

// Unix 秒 → 年月日（本地时区）。epoch <= 0 → false（无截止日期）
inline bool epochToYmd(int64_t epoch, int& year, int& month, int& day) {
    if (epoch <= 0) return false;
    std::tm tmv{};
    core::time::localTime(static_cast<std::time_t>(epoch), &tmv);
    year = tmv.tm_year + 1900;
    month = tmv.tm_mon + 1;
    day = tmv.tm_mday;
    return true;
}

} // namespace module::todo::ui
