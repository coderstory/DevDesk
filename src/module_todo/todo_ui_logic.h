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

// 卡片尺寸常量（drawCard 与拖拽命中换算共用）
constexpr float kCardHeight = 84.0f;
constexpr float kCardGap = 8.0f;
constexpr float kColumnHeaderHeight = 56.0f;

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

// W8 拖拽命中输入：拖拽锚点 = 按下时被拖卡片的 bounds（指针像素空间），
// 板面几何 = compose 空间（drawBoard 每帧快照）。两个空间相差统一的
// 缩放（dpi）与偏移（侧边栏等），锚定卡片自身即可同时消掉两者：
// dpi 用 pressWidth / cardWidth 探测，板面原点用卡片左缘回推。
struct DragHitInput {
    int fromColIndex = 0;
    int colCount = 0;
    float pressX = 0.0f;        // 按下时卡片 bounds（指针像素空间）
    float pressY = 0.0f;
    float pressWidth = 0.0f;
    float cardWidth = 0.0f;     // compose 空间卡宽（dpi 探针）
    float colWidth = 0.0f;      // compose 空间列宽
    float colGap = 0.0f;
    float boardHeight = 0.0f;   // compose 空间板高
    float scrollOffset = 0.0f;  // 源列当前滚动偏移（compose 空间）
    int cardIndex = 0;
};

// 指针位置 → 目标列索引。列间隙 / 板面外 / 输入退化 → -1（拖放取消）。
// 卡片 bounds.y 已含列内滚动偏移（滚动走 transform，命中框跟随），
// 先还原成"未滚动"的板内位置再推板面顶边。
inline int dragDropTarget(const DragHitInput& in, double pointerX, double pointerY) {
    if (in.colCount <= 0 || in.cardWidth <= 0.0f || in.pressWidth <= 0.0f ||
        in.colWidth <= 0.0f || in.fromColIndex < 0 ||
        in.fromColIndex >= in.colCount || in.cardIndex < 0) {
        return -1;
    }
    const float dpi = in.pressWidth / in.cardWidth;
    const float stride = (in.colWidth + in.colGap) * dpi;
    // 板面原点：卡片 bg 左缘 = 所在列左缘
    const float originX = in.pressX - static_cast<float>(in.fromColIndex) * stride;
    // 板面顶边：卡片在列内的未滚动 y = 列头高 + 序号 * (卡高+间距) - 滚动偏移
    const float cardTopInCol =
        kColumnHeaderHeight +
        static_cast<float>(in.cardIndex) * (kCardHeight + kCardGap) - in.scrollOffset;
    const float boardTop = in.pressY - cardTopInCol * dpi;
    const float boardBottom = boardTop + in.boardHeight * dpi;

    const float y = static_cast<float>(pointerY);
    if (y < boardTop || y > boardBottom) return -1;

    for (int j = 0; j < in.colCount; ++j) {
        const float left = originX + static_cast<float>(j) * stride;
        const float x = static_cast<float>(pointerX);
        if (x >= left && x <= left + in.colWidth * dpi) return j;
    }
    return -1;
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
