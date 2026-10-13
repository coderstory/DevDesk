// Todo UI 可单测纯函数（键盘移动 / 拖拽几何 / 截止日期 / 优先级配色）。无状态、不碰 DB
#pragma once

#include "todo_service.h"

#include <eui/types.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/theme.h"
#include "core/time.h"
#include "core/ui_geometry.h"
#include "ui_kit/metrics.h"

namespace module::todo::ui {

// 卡片尺寸常量（drawCard 与拖拽命中换算共用）
constexpr float kCardHeight = 84.0f;
constexpr float kCardGap = 8.0f;
constexpr float kColumnHeaderHeight = 56.0f;

// 序号徽章（每列独立 1..n，纯渲染期计算）。正圆 20px，卡左缘 12px 内缩、8px gap 接标题
constexpr float kCardOrdinalBadgeSize = 20.0f;
constexpr float kCardOrdinalBadgeGap = 8.0f;
// 徽章 + 间距占用的水平宽度（标题等右侧内容的减量）
constexpr float kCardOrdinalBadgeHoriz = kCardOrdinalBadgeSize + kCardOrdinalBadgeGap;

// 列内容区水平内边距（16px 网格）。卡左缘 = 列内容左缘 + 本常量，卡宽/占位条宽同源
constexpr float kColumnContentInset = 16.0f;

// 卡片 / 占位条共用的可用宽（内容宽 - 双侧 inset），窄列钳到 0
inline float contentCardWidth(float contentWidth) {
    return std::max(0.0f, contentWidth - kColumnContentInset * 2.0f);
}

// 键盘移动的目标（columnId + moveTask 的 newPosition 语义）
struct MoveTarget {
    int64_t columnId;
    int position;
};

// Ctrl/Cmd+↑↓ 的移动目标。moveTask 语义 = "插到 position == newPosition 的任务之前"：
// 上移取紧邻上方 position，下移取紧邻下方 position+1；到顶/底 → nullopt。用 DB 真实 position（rank 有空洞不可靠）
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

// 目标列末尾追加位置（max position + 1；空列 = 0）。用 DB 真实 position（同 computeKeyboardMove）
inline int computeAppendPosition(const std::vector<Task>& tasks, int64_t columnId) {
    int maxPos = -1;
    for (const auto& t : tasks) {
        if (t.columnId == columnId && t.position > maxPos) maxPos = t.position;
    }
    return maxPos + 1;
}

// 拖拽命中输入：拖拽锚点 = 按下时被拖卡片的 bounds（指针空间），板面几何 = compose 空间。
// 指针 → compose 统一走 core::windowToCompose，尺度由调用点显式声明。
// ⚠ 同帧契约：pressY 与 scrollOffset 必须是按下那一帧的成对值——bounds.y 含按下时刻的列滚动，
// 还原板面顶边只能减同一个值（调用方在 DragState 里快照）
struct DragHitInput {
    int fromColIndex = 0;
    int colCount = 0;
    // 按下时卡片 bounds（指针空间）。卡片左缘 = 列左缘 + kColumnContentInset，回推原点时先减掉
    float pressX = 0.0f;
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

// dpi 探针（断言用）：pressWidth / cardWidth = 实测尺度。换算本体不吃本值，退化返回 0
inline float dragAnchorScale(const DragHitInput& in) {
    return in.cardWidth > 0.0f ? in.pressWidth / in.cardWidth : 0.0f;
}

// 探针与框架显式尺度不一致 → true（调用点打一次性 WARN）。判据 = 探针 > 0 且 ≠ 1 且 ≠ 显式值
inline bool dragScaleMismatch(float explicitScale, float probeScale) {
    return probeScale > 0.0f &&
           std::fabs(probeScale - 1.0f) > 0.001f &&
           std::fabs(probeScale - explicitScale) > 0.001f;
}

// 指针 → 目标列索引；列间隙 / 板外 / 输入退化 → -1。bounds.y 含列滚动，先还原成未滚动位置再推板面顶边
inline int dragDropTarget(const DragHitInput& in, double pointerX, double pointerY) {
    if (in.colCount <= 0 || in.cardWidth <= 0.0f || in.pressWidth <= 0.0f ||
        in.colWidth <= 0.0f || in.fromColIndex < 0 ||
        in.fromColIndex >= in.colCount || in.cardIndex < 0) {
        return -1;
    }
    const float dpi = in.dpiScale;
    const float stride = in.colWidth + in.colGap;
    // 板面原点：按下锚点（卡片左缘 → compose）减 kColumnContentInset 还原列左缘，再左移若干列步长
    const float originX = ::core::windowToCompose(in.pressX, in.pageOriginX, dpi) -
                          kColumnContentInset -
                          static_cast<float>(in.fromColIndex) * stride;
    // 板面顶边：卡片未滚动 y = 列头高 + 序号*(卡高+间距) - 滚动偏移
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

// 拖拽插入行号（0 = 首卡前；>= taskCount = 列尾追加），几何口径与 dragDropTarget 同源；不在目标列/板外 → -1
inline int dragDropRowIndex(const DragHitInput& in, int targetCol, double pointerX,
                            double pointerY, float targetScroll, int taskCount) {
    if (in.colCount <= 0 || in.cardWidth <= 0.0f || in.pressWidth <= 0.0f ||
        in.colWidth <= 0.0f || in.fromColIndex < 0 ||
        in.fromColIndex >= in.colCount || in.cardIndex < 0 ||
        targetCol < 0 || targetCol >= in.colCount || taskCount < 0) {
        return -1;
    }
    const float dpi = in.dpiScale;
    const float stride = in.colWidth + in.colGap;
    // 同 dragDropTarget：由按下锚点回推板面原点
    const float originX = ::core::windowToCompose(in.pressX, in.pageOriginX, dpi) -
                          kColumnContentInset -
                          static_cast<float>(in.fromColIndex) * stride;
    const float x = ::core::windowToCompose(static_cast<float>(pointerX),
                                            in.pageOriginX, dpi);
    const float left = originX + static_cast<float>(targetCol) * stride;
    if (x < left || x > left + in.colWidth) return -1;  // 不在目标列
    const float cardTopInCol =
        kColumnHeaderHeight +
        static_cast<float>(in.cardIndex) * (kCardHeight + kCardGap) - in.scrollOffset;
    const float boardTop = ::core::windowToCompose(in.pressY, 0.0f, dpi) - cardTopInCol;
    const float y = ::core::windowToCompose(static_cast<float>(pointerY), 0.0f, dpi);
    if (y < boardTop || y > boardTop + in.boardHeight) return -1;  // 板外
    // 行 = floor((relY-卡高)/92)+1（指针压着某张卡 = 插其前）
    const float relY = y - boardTop - kColumnHeaderHeight + targetScroll;
    int row = static_cast<int>(std::floor(
        (relY - kCardHeight) / (kCardHeight + kCardGap))) + 1;
    if (row < 0) row = 0;
    if (row > taskCount) row = taskCount;  // 尾部空白区 → 追加
    return row;
}

// 拖拽跟手副本锚点（页面 compose 空间左上角）。输入 = 按下卡片 bounds + 按下起累计位移（DragEvent::totalX/Y）。
// 尺度因子与 dragDropTarget 同源（锚点走 windowToCompose、位移除同一个 dpiScale）——
// 两者不可各用一套因子，否则非 100% 缩放下跟手距离走偏。退化 dpiScale 按同尺度处理
struct GhostAnchor {
    float x = 0.0f;
    float y = 0.0f;
};

inline GhostAnchor dragGhostAnchor(float pressX, float pressY, double totalX,
                                   double totalY, float pageOriginX,
                                   float dpiScale) {
    if (!(dpiScale > 0.0f)) dpiScale = 1.0f;
    return {::core::windowToCompose(pressX, pageOriginX, dpiScale) +
                static_cast<float>(totalX) / dpiScale,
            ::core::windowToCompose(pressY, 0.0f, dpiScale) +
                static_cast<float>(totalY) / dpiScale};
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

// 列颜色 token → 色点 hex（与 kColorPresets 对应）；空/未知 → nullopt
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

// 逾期天数（本地日历日差）：昨天到期今天看 = 1 天；未过期/当天到期 → 0。走 localDayNumber 免 tm_yday 跨年/闰年坑
inline int overdueDays(int64_t dueAt, int64_t now) {
    if (now <= dueAt) return 0;
    const int64_t diff =
        core::time::localDayNumber(static_cast<std::time_t>(now)) -
        core::time::localDayNumber(static_cast<std::time_t>(dueAt));
    return diff <= 0 ? 0 : static_cast<int>(diff);
}

// 搜索过滤：搜索激活时只保留命中集合内的任务（列内排序不变）；每帧 compose，unordered_set 查找 O(1)
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

// 优先级 → 中文标签。下标 = Priority 值域，static_assert 保证新增枚举时编译期失败；越界走 [0]
inline const char* priorityLabel(Priority p) {
    static constexpr const char* kLabels[] = {"无", "●低", "●中", "●高"};
    static_assert(std::size(kLabels) == static_cast<size_t>(Priority::High) + 1);
    const size_t idx = static_cast<size_t>(p);
    return kLabels[idx <= static_cast<size_t>(Priority::High) ? idx : 0];
}

inline eui::Color priorityColor(Priority p, const core::theme::Tokens& t) {
    // 存 ColorTokens 成员指针而非色值：色值随主题运行期解析，不缓存首帧主题色。下标 = Priority 值域，越界走 [0]
    static constexpr eui::Color core::theme::ColorTokens::*kColorFields[] = {
        &core::theme::ColorTokens::textTertiary,  // None
        &core::theme::ColorTokens::info,          // Low
        &core::theme::ColorTokens::warning,       // Medium
        &core::theme::ColorTokens::error,         // High
    };
    static_assert(std::size(kColorFields) == static_cast<size_t>(Priority::High) + 1);
    const size_t idx = static_cast<size_t>(p);
    return t.colors.*kColorFields[idx <= static_cast<size_t>(Priority::High) ? idx : 0];
}

// 优先级色底：卡底与详情弹窗优先级按钮底的唯一色源（priorityColor 色表 + 强度表 + priorityTintBg 算式）
inline constexpr float kPriorityTintStrength[] = {0.12f, 0.24f, 0.32f, 0.30f};
static_assert(std::size(kPriorityTintStrength) ==
              static_cast<size_t>(Priority::High) + 1);

// 越界走 [0] 档，同 priorityLabel / priorityColor
inline float priorityTintStrength(Priority p) {
    const size_t idx = static_cast<size_t>(p);
    return kPriorityTintStrength[idx <= static_cast<size_t>(Priority::High) ? idx
                                                                           : 0];
}

// 底色算式：基色向优先级色混 strength；strength=0 原样返回（None 档 = 无 tint）
inline eui::Color priorityTintBg(Priority p, const core::theme::Tokens& t,
                                 const eui::Color& base, float strength) {
    if (strength <= 0.0f) return base;
    return eui::mixColor(base, priorityColor(p, t), strength);
}

// ① 看板卡底：None 不加 tint，低/中/高按强度表混；优先级色相与 hover/active 反馈叠加而非互相覆盖
inline eui::Color priorityCardBg(Priority p, const core::theme::Tokens& t,
                                 const eui::Color& base) {
    return priorityTintBg(p, t, base,
                          p == Priority::None ? 0.0f : priorityTintStrength(p));
}

// ② 按钮选中档强度：不取 1.0 实色——实色底配单一前景 token 总有一档掉到 4.5:1 以下；0.55 深 tint 四档均 ≥ 6.0:1
inline constexpr float kPriorityButtonPickedStrength = 0.55f;

// ② 按钮底：「无」也是可选项须有自己的底色，None 档取 0.12（近中性灰）；base 由调用方给三态基色
inline eui::Color priorityButtonBg(Priority p, const core::theme::Tokens& t,
                                   bool picked, const eui::Color& base) {
    return priorityTintBg(p, t, base,
                          picked ? kPriorityButtonPickedStrength
                                 : priorityTintStrength(p));
}

// epoch 秒 → "MM-DD"；<=0 表示无截止日期
inline std::string formatDue(int64_t epoch) {
    if (epoch <= 0) return {};
    std::time_t tt = static_cast<std::time_t>(epoch);
    std::tm tmv{};
    core::time::localTime(tt, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d-%02d", tmv.tm_mon + 1, tmv.tm_mday);
    return std::string(buf);
}

inline bool isOverdue(const Task& task) {
    if (!task.dueAt.has_value() || task.completedAt.has_value()) return false;
    return task.dueAt.value() < static_cast<int64_t>(std::time(nullptr));
}

inline const Task* findTask(const std::vector<Task>& tasks, int64_t id) {
    for (const auto& t : tasks) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

inline const Column* findColumn(const std::vector<Column>& columns, int64_t id) {
    for (const auto& c : columns) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

// epoch 秒 → "YYYY-MM-DD HH:MM:SS"（24h 制，本地时区）；<=0 返回空串
inline std::string formatArchivedAt(int64_t epoch) {
    if (epoch <= 0) return {};
    std::time_t tt = static_cast<std::time_t>(epoch);
    std::tm tmv{};
    core::time::localTime(tt, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

// 列内任务（按 position 升序）；看板列内容与删除确认弹窗共用
inline std::vector<const Task*> tasksInColumn(const std::vector<Task>& tasks,
                                              int64_t columnId) {
    std::vector<const Task*> out;
    for (const auto& t : tasks) {
        if (t.columnId == columnId) out.push_back(&t);
    }
    std::sort(out.begin(), out.end(), [](const Task* a, const Task* b) {
        return a->position < b->position;
    });
    return out;
}

// 归档列任务（按 archived_at 倒序，最近归档的在最上面；同刻并列时 position 兜底）
// 非归档列用 tasksInColumn（按 position 升序）。手动 position 写入对归档列仍可发生但显示忽略。
inline std::vector<const Task*> tasksInArchiveColumn(const std::vector<Task>& tasks,
                                                      int64_t columnId) {
    std::vector<const Task*> out;
    for (const auto& t : tasks) {
        if (t.columnId == columnId) out.push_back(&t);
    }
    std::sort(out.begin(), out.end(), [](const Task* a, const Task* b) {
        const auto aVal = a->archivedAt.value_or(0);
        const auto bVal = b->archivedAt.value_or(0);
        if (aVal != bVal) return aVal > bVal;  // 最近的在最上面 → DESC
        return a->position < b->position;      // 同刻并列靠 position 兜底
    });
    return out;
}

// 拖拽按行落库位置：插到「行 == rowIndex 的任务」之前（用 DB 真实 position）；rowIndex 越界 / 空列 → 列尾追加
inline int computeInsertPosition(const std::vector<Task>& tasks,
                                 int64_t columnId, int rowIndex) {
    const auto colTasks = tasksInColumn(tasks, columnId);
    if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= colTasks.size()) {
        return computeAppendPosition(tasks, columnId);
    }
    return colTasks[static_cast<size_t>(rowIndex)]->position;
}

// ① 同列重排行号换算：行号含被拖卡自身（预览条即所见），落库要摘出自身——row > ownIndex 时减 1；插回原位 → nullopt
inline std::optional<int> computeReorderRow(int ownIndex, int rowIndex) {
    if (ownIndex < 0 || rowIndex < 0) return std::nullopt;
    const int adjusted = rowIndex > ownIndex ? rowIndex - 1 : rowIndex;
    if (adjusted == ownIndex) return std::nullopt;
    return adjusted;
}

// ② 落库位置：语义同 computeInsertPosition，但行序排除被拖卡自身（否则插入位错一格）；rowIndex 越界 → 排除自身后 max+1
inline int computeReorderInsertPosition(const std::vector<Task>& tasks,
                                        int64_t columnId, int64_t draggedTaskId,
                                        int rowIndex) {
    std::vector<const Task*> others;
    for (const auto& t : tasks) {
        if (t.columnId == columnId && t.id != draggedTaskId) others.push_back(&t);
    }
    std::sort(others.begin(), others.end(), [](const Task* a, const Task* b) {
        return a->position < b->position;
    });
    if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= others.size()) {
        int maxPos = -1;
        for (const Task* t : others) {
            if (t->position > maxPos) maxPos = t->position;
        }
        return maxPos + 1;
    }
    return others[static_cast<size_t>(rowIndex)]->position;
}

// 按 kind 找列 id（'done'/'todo'；找不到 0）。不按标题找——重命名后不可靠，kind 权威
inline int64_t findColumnByKind(const std::vector<Column>& columns,
                                const std::string& kind) {
    for (const auto& c : columns) {
        if (c.kind == kind) return c.id;
    }
    return 0;
}

// 列标题是否已占用（exceptId = 改名列自身 id，0 = 不排除）。撞名经弹窗红字呈现
inline bool columnTitleTaken(const std::vector<Column>& columns,
                             const std::string& title, int64_t exceptId) {
    for (const auto& c : columns) {
        if (c.id != exceptId && c.title == title) return true;
    }
    return false;
}

// 留空默认列名「新列 N」（N = 现有列数 + 1）
inline std::string defaultColumnTitle(size_t columnCount) {
    return "新列 " + std::to_string(columnCount + 1);
}

// 列内滚动位置（按列 id 记忆，列重排/删列不串列）
inline float columnScrollOffset(const std::vector<Column>& columns,
                                const std::map<int64_t, float>& scrolls,
                                size_t colIndex) {
    if (colIndex >= columns.size()) return 0.0f;
    auto it = scrolls.find(columns[colIndex].id);
    return it == scrolls.end() ? 0.0f : it->second;
}

// 看板几何：drawBoard 与右键菜单定位共用（菜单走模块布局空间，非指针窗口坐标）
struct BoardGeom {
    float pad = 0.0f;
    float colW = 0.0f;
    float colGap = 0.0f;
    float boardTop = 0.0f;  // 看板区（列背景）顶边的模块空间 y
};

// screenWidth = 页面逻辑宽（调用点传 Screen::width）；本头不碰 Ui，故只收 float
inline BoardGeom boardGeom(float screenWidth, size_t columnCount,
                           const core::theme::Tokens& t) {
    BoardGeom g;
    g.pad = t.sizes.spacingXXLarge;
    g.colGap = t.sizes.spacingLarge;
    const float contentW = screenWidth - g.pad * 2.0f;
    const auto n = static_cast<float>(columnCount == 0 ? 1 : columnCount);
    g.colW = (contentW - g.colGap * (n - 1.0f)) / n;
    g.boardTop = g.pad + ui_kit::kHeaderH + t.sizes.spacingLarge;
    return g;
}

inline float cardLeft(const BoardGeom& g, int columnIndex) {
    return g.pad + static_cast<float>(columnIndex) * (g.colW + g.colGap);
}

inline float cardTop(const BoardGeom& g, int cardIndex) {
    return g.boardTop + kColumnHeaderHeight +
           static_cast<float>(cardIndex) * (kCardHeight + kCardGap);
}

// chip 几何与字号（标签 / 截止 pill）：drawChip 与 chipWidth 估宽共用同一份常量——调字号必须同步调估宽
constexpr float kChipHeight = 22.0f;
constexpr float kChipFontInset = 1.0f;  // 字号 = fontSizeSmall - inset（13-1=12）

// chip 宽度估算（估计值略偏大，只多留白、不裁字；不发 FontRegistry 实测）
inline float chipWidth(const std::string& text) {
    float w = 20.0f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? 4.8f : 6.6f;
    }
    return w;
}

// 卡片内容组几何：内容组在 kCardHeight 内垂直居中；水平只居中主文本。常量供 drawCard / drawDragGhost / 无头断言同源
constexpr float kCardTitleLineH = 18.0f;      // 标题行高（= lineHeight）
constexpr float kCardTitleToChipGap = 12.0f;  // 标题行底 → chip 行顶
// 标题框左右对称内缩 40（总留白 80），右缘 W-40 让开完成角标区间 [W-36, W-12]
constexpr float kCardTitleBoxInset = 40.0f;

// 内容组高度：有 chip 行 = 18+12+22；无 = 仅标题 18
inline float cardContentGroupH(bool hasChipRow) {
    return kCardTitleLineH +
           (hasChipRow ? kCardTitleToChipGap + kChipHeight : 0.0f);
}

inline float cardContentGroupTop(bool hasChipRow) {
    return (kCardHeight - cardContentGroupH(hasChipRow)) / 2.0f;
}

// 改色预设（与 columnDotColor 映射一一对应）；标签给 UI，token 给 service
struct ColorPreset {
    const char* token;
    const char* label;
};
inline constexpr ColorPreset kColorPresets[] = {
    {"claude-clay", "Claude 橘"},
    {"warm-amber",  "暖琥珀"},
    {"muted-sage",  "鼠尾草绿"},
    {"warm-gray",   "暖灰"},
    {"dusty-blue",  "雾蓝"},
    {"rose-red",    "玫瑰红"},
};
inline constexpr int kColorPresetCount =
    static_cast<int>(sizeof(kColorPresets) / sizeof(kColorPresets[0]));

} // namespace module::todo::ui
