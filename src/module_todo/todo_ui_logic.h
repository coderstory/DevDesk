// src/module_todo/todo_ui_logic.h
// Todo UI 可单测纯函数（P7a 键盘移动 + 截止日期换算）。
// 不持有状态、不碰 DB，输入输出均可离线断言。
#pragma once

#include "todo_service.h"

#include <eui/types.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <optional>
#include <unordered_set>
#include <vector>

#include "core/time.h"
#include "core/ui_geometry.h"

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

// W8 拖拽命中输入：拖拽锚点 = 按下时被拖卡片的 bounds（指针空间），
// 板面几何 = compose 空间（drawBoard 每帧快照）。
//
// 审计 M4：指针 → compose 的换算统一走 core::windowToCompose，尺度由调用点
// 显式声明（dpiScale）；板面原点由卡片左缘回推（卡片 bg 左缘 = 所在列左缘），
// 换算后全部几何在 compose 空间比较，不再把 colWidth / 板高逐个乘 dpi。
// onPress / onDrag 通道的指针值是**原始**值（runtime_input 对 onContextMenu /
// onMove 才做 /dpiScale），故 hitDropColumn 侧要喂"指针空间相对 compose 空间"
// 的尺度。
//
// U1 尺度契约（2026-09-27）：该尺度**取自框架，不再由锚点反推**。todo 是
// Router 页面模块、渲染在主窗口，调用点取 `app::contentScale()`（声明见
// include/eui/app.h：物理 = 逻辑 * scale，与 compose 收到的 Screen::scale 同源
// 同值）；若日后 todo 搬进独立子窗，改取该窗 compose 拿到的 Screen::scale。
// dragAnchorScale 探针保留，但只作断言（dragScaleMismatch）。
//
// ⚠ 同帧契约（tech-debt B）：pressY 与 scrollOffset 必须是"按下那一帧"的
// 成对值——bounds.y 里含的是按下时刻的列滚动，还原板面顶边只能减同一个值。
// 调用方（hitDropColumn）在 DragState 里快照，拖拽中途列被滚动也不会错位。
struct DragHitInput {
    int fromColIndex = 0;
    int colCount = 0;
    float pressX = 0.0f;        // 按下时卡片 bounds（指针空间）
    float pressY = 0.0f;
    float pressWidth = 0.0f;    // 按下时卡片 bounds 宽（dpi 探针的分子）
    float cardWidth = 0.0f;     // compose 空间卡宽（dpi 探针的分母）
    float colWidth = 0.0f;      // compose 空间列宽
    float colGap = 0.0f;
    float boardHeight = 0.0f;   // compose 空间板高
    float scrollOffset = 0.0f;  // 源列滚动偏移（compose 空间，按下时刻快照）
    float pageOriginX = 0.0f;   // 页面区原点 x（compose 空间；无头/未接入 = 0）
    float dpiScale = 1.0f;      // 指针空间 → compose 空间缩放（框架值；1.0 = 同尺度）
    int cardIndex = 0;
};

// dpi 探针（审计 M4：由"唯一手段"降级为断言）：按下时卡片 bounds 宽 /
// compose 空间卡宽 = 指针空间相对 compose 空间的**实测**尺度。换算本体不吃
// 本值（dpiScale 由调用点喂框架值），退化（无卡宽）返回 0。纯函数，无副作用。
inline float dragAnchorScale(const DragHitInput& in) {
    return in.cardWidth > 0.0f ? in.pressWidth / in.cardWidth : 0.0f;
}

// 探针断言（U1 尺度契约）：探针**实测**尺度与框架显式尺度不一致 → true，
// 调用点据此打一次性 WARN（可观测）。
// 判据 = 探针 > 0 **且** 探针 ≠ 1 **且** 探针 ≠ 显式值：
//   · 探针 == 0（无卡宽，退化）或 == 1 → 无信号（跳过断言，不告警；100% 档
//     探针本就 1）
//   · 探针 ≠ 1 但与框架值一致 → 正常（框架值被几何实测独立证实，不告警）
// 只在"探针自称一个非 1 的尺度、却对不上框架"时才是真异常（框架值口径与几何
// 实测漂移），此时告警留痕供点验比对。纯函数，无副作用。
inline bool dragScaleMismatch(float explicitScale, float probeScale) {
    return probeScale > 0.0f &&
           std::fabs(probeScale - 1.0f) > 0.001f &&
           std::fabs(probeScale - explicitScale) > 0.001f;
}

// 指针位置 → 目标列索引。列间隙 / 板面外 / 输入退化 → -1（拖放取消）。
// 卡片 bounds.y 已含列内滚动偏移（滚动走 transform，命中框跟随），
// 先还原成"未滚动"的板内位置再推板面顶边。
inline int dragDropTarget(const DragHitInput& in, double pointerX, double pointerY) {
    if (in.colCount <= 0 || in.cardWidth <= 0.0f || in.pressWidth <= 0.0f ||
        in.colWidth <= 0.0f || in.fromColIndex < 0 ||
        in.fromColIndex >= in.colCount || in.cardIndex < 0) {
        return -1;
    }
    const float dpi = in.dpiScale;
    const float stride = in.colWidth + in.colGap;
    // 板面原点：按下锚点（→ compose）左移若干列步长 = 源列左缘
    const float originX = ::core::windowToCompose(in.pressX, in.pageOriginX, dpi) -
                          static_cast<float>(in.fromColIndex) * stride;
    // 板面顶边：卡片在列内的未滚动 y = 列头高 + 序号 * (卡高+间距) - 滚动偏移
    const float cardTopInCol =
        kColumnHeaderHeight +
        static_cast<float>(in.cardIndex) * (kCardHeight + kCardGap) - in.scrollOffset;
    const float boardTop = ::core::windowToCompose(in.pressY, 0.0f, dpi) - cardTopInCol;
    const float boardBottom = boardTop + in.boardHeight;

    const float y = ::core::windowToCompose(static_cast<float>(pointerY), 0.0f, dpi);
    if (y < boardTop || y > boardBottom) return -1;

    const float x = ::core::windowToCompose(static_cast<float>(pointerX),
                                            in.pageOriginX, dpi);
    for (int j = 0; j < in.colCount; ++j) {
        const float left = originX + static_cast<float>(j) * stride;
        if (x >= left && x <= left + in.colWidth) return j;
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

// ---- 三件套（W 计划 P2 遗留）纯函数 ----

// 列颜色 token → 色点 hex（migrations 种子 + 改色预设共用一张表）。
// 空 / 未知 token → nullopt（列头不渲染色点）。
inline std::optional<eui::Color> columnDotColor(const std::optional<std::string>& token) {
    if (!token.has_value()) return std::nullopt;
    if (*token == "claude-clay") return eui::Color("#D97757");
    if (*token == "warm-amber")  return eui::Color("#C29243");
    if (*token == "muted-sage")  return eui::Color("#788C5D");
    if (*token == "warm-gray")   return eui::Color("#9A948A");
    if (*token == "dusty-blue")  return eui::Color("#6A9BCC");
    if (*token == "rose-red")    return eui::Color("#CF2D56");
    return std::nullopt;
}

// 逾期天数（本地日历日差：昨天到期今天看 = 1 天；未过期/当天到期 → 0。
// 终验复审：原秒差向上取整在"23:59 到期、次日 00:01 查看"时虚报 2 天）
// 日历日序号统一走 core::time::localDayNumber（days_from_civil，
// 免 tm_yday 跨年/闰年坑）。
inline int overdueDays(int64_t dueAt, int64_t now) {
    if (now <= dueAt) return 0;
    const int64_t diff =
        core::time::localDayNumber(static_cast<std::time_t>(now)) -
        core::time::localDayNumber(static_cast<std::time_t>(dueAt));
    return diff <= 0 ? 0 : static_cast<int>(diff);
}

// 搜索过滤：搜索激活时只保留命中 id 集合内的任务（列内排序不变）。
// matchedIds 来自 TodoService::search（title/description LIKE）。
// 审计 perf [40]：命中表用 unordered_set——旧版对每任务线性 std::find，
// 搜索激活时每帧 compose O(任务数×命中数)；集合查找 O(1)（过滤结果只依赖
// 「是否命中」，与命中表原有顺序无关，语义不变）。
inline std::vector<const Task*> filterBySearch(const std::vector<const Task*>& tasks,
                                               const std::unordered_set<int64_t>& matchedIds,
                                               bool searching) {
    if (!searching) return tasks;
    std::vector<const Task*> out;
    for (const Task* t : tasks) {
        if (matchedIds.count(t->id) != 0) {
            out.push_back(t);
        }
    }
    return out;
}

} // namespace module::todo::ui
