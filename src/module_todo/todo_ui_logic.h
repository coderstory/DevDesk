// src/module_todo/todo_ui_logic.h
// Todo UI 可单测纯函数（P7a 键盘移动 + 截止日期换算）。
// 不持有状态、不碰 DB，输入输出均可离线断言。
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

// 拖拽插入行（UI 整改 13：目标位占位条 + 按行落库）：指针在目标列内容区
// 的悬停行号（0 = 首卡前；>= taskCount = 列尾追加，与旧「追加列尾」语义
// 对齐）。几何口径与 dragDropTarget 同源（板面顶边由按下锚点回推）；
// targetScroll = 目标列**当下**滚动偏移——本值只喂**视觉预览**（占位条），
// 落库换算不吃它（tech-debt B 同帧契约只约束 pressY/scrollOffset 对），
// 拖拽途中目标列被滚轮滚动，预览条小幅错位可接受。
// 指针不在目标列 / 板外 / 输入退化 → -1（不预览）。
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
    // 与 dragDropTarget 同源：由按下锚点回推板面原点
    const float originX = ::core::windowToCompose(in.pressX, in.pageOriginX, dpi) -
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
    // 列内容坐标系（含目标列滚动）：卡 i 占据 [i*92, i*92+84]，卡间缝隙
    // 中心 = 插入位 → 行 = floor((relY-84)/92)+1（指针压着某张卡 = 插其前）
    const float relY = y - boardTop - kColumnHeaderHeight + targetScroll;
    int row = static_cast<int>(std::floor(
        (relY - kCardHeight) / (kCardHeight + kCardGap))) + 1;
    if (row < 0) row = 0;
    if (row > taskCount) row = taskCount;  // 尾部空白区 → 追加
    return row;
}

// 拖拽跟手副本的锚点（页面 compose 空间左上角）。
//
// 输入 = 按下时被拖卡片的 bounds（指针空间，DragState.pressX/pressY）+
// 按下点起的指针**累计**位移（DragEvent::totalX/totalY，物理像素——与
// onPress/onDrag 的指针坐标同空间，框架在按下时记 dragStart、每次移动给
// 累计值，见 core/input/input_types.h:266）。用累计位移而不是"当下指针 -
// 按下指针"：模块侧不必多存一份按下指针，且不受 onDrag 派发时机影响。
//
// 尺度因子与 dragDropTarget 同源（调用点喂框架值 app::contentScale()，见
// U1 尺度契约）：锚点走 core::windowToCompose、位移是**同一个** dpiScale 的
// 除法。两者不可各用一套因子——否则非 100% 缩放下跟手距离按缩放比走偏。
// 页面原点 x 在位移里同样对消（锚点已含，位移不含），故只在此处减一次。
//
// 纯函数，无副作用。位移为 0（刚按下 / 已被松手清零）→ 锚点 = 卡片原位置，
// 副本与原卡重合。退化 dpiScale（0/负/NaN）按同尺度处理（同 windowToCompose）。
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

// ==================== R5：任务/列的取值与筛选 ====================
// 以下三族原为 TodoModule 的成员方法（findTask / findColumn / tasksInColumn）与
// 匿名命名空间里的表驱动映射（优先级标签/配色、截止日期排版）。R5 把看板与
// 弹窗拆成两个绘制单元后，两处都要用同一份实现，故按「不碰 Ui、不碰 DB 的纯
// 逻辑住本头」的约定（god-class spec §5.2 第 4 条）收在这里，语义逐字未变。
// 传 vector 而不是模块成员：调用方（模块 / 看板单元 / 弹窗单元）各自持有或
// 引用同一份数据，纯函数只读。

// 优先级 → 中文标签（●/○ 是文本字形，随 chip 前景色调色；
// 旧 🔴🟡🔵⚪ 是系统彩色 emoji，风格不统一已换）
// 下标 = Priority 枚举值（None=0 … High=3）：新增枚举成员时 static_assert
// 编译期失败，不会像原 switch + default 那样静默吞掉漏改。越界（脏数据）走 [0]。
inline const char* priorityLabel(Priority p) {
    static constexpr const char* kLabels[] = {"无", "●低", "●中", "●高"};
    static_assert(std::size(kLabels) == static_cast<size_t>(Priority::High) + 1);
    const size_t idx = static_cast<size_t>(p);
    return kLabels[idx <= static_cast<size_t>(Priority::High) ? idx : 0];
}

inline eui::Color priorityColor(Priority p, const core::theme::Tokens& t) {
    // 表里存的是 ColorTokens 成员指针（不是色值）：色值随主题 t 在运行期解析，
    // 编译期常量表因此不会把首帧主题的颜色缓存住。下标 = Priority 值域，
    // static_assert 保证枚举新增时编译期失败；越界走 [0]（None 色）。
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

// 优先级底色（UI 整改 12：底色区分，去「高/中/低」文本）。
// 高=警示色底 / 中=中性底 / 低=弱底（各向 surface 掺 token 的 tint，
// 深浅主题下都保持「底」的弱对比语义）；None=无标记（不画 pill）。
// eui::mixColor 是 header-only 值函数（与 eui::Color 同层），模块层可直用。
inline std::optional<eui::Color> priorityPillBg(Priority p,
                                                const core::theme::Tokens& t) {
    switch (p) {
        case Priority::Low:
            return eui::mixColor(t.colors.surface, t.colors.info, 0.18f);     // 弱底
        case Priority::Medium:
            return eui::mixColor(t.colors.surface, t.colors.textTertiary, 0.40f);  // 中性底
        case Priority::High:
            return eui::mixColor(t.colors.surface, t.colors.error, 0.45f);    // 警示色底
        default:
            return std::nullopt;
    }
}

// epoch 秒 → "MM-DD"；-1 表示无截止日期
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

// 列内任务（按 position 升序）——看板列内容与删除确认弹窗的任务计数共用
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

// 拖拽按行落库的目标位置（UI 整改 13）：插入语义同 computeKeyboardMove——
// 插到「行 == rowIndex 的任务」之前（用 DB 真实 position，moveTask 留洞
// 后 rank 不可靠）；rowIndex 越界 / 空列 → 列尾追加（同旧语义）。
inline int computeInsertPosition(const std::vector<Task>& tasks,
                                 int64_t columnId, int rowIndex) {
    const auto colTasks = tasksInColumn(tasks, columnId);
    if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= colTasks.size()) {
        return computeAppendPosition(tasks, columnId);
    }
    return colTasks[static_cast<size_t>(rowIndex)]->position;
}

// 按语义标记找列 id（'done'=已完成列 / 'todo'=待办列；找不到返回 0）
// 不按标题找：用户重命名列后标题不可靠（migration v4 起 kind 是权威）
inline int64_t findColumnByKind(const std::vector<Column>& columns,
                                const std::string& kind) {
    for (const auto& c : columns) {
        if (c.kind == kind) return c.id;
    }
    return 0;
}

// 列标题是否已被占用（exceptId = 改名列自身 id，重命名为自己的
// 当前名不算撞名；0 = 不排除）。审计 [19]：撞名校验错误经弹窗
// 内嵌红字呈现（此前重命名空名静默返回、无任何反馈）。
inline bool columnTitleTaken(const std::vector<Column>& columns,
                             const std::string& title, int64_t exceptId) {
    for (const auto& c : columns) {
        if (c.id != exceptId && c.title == title) return true;
    }
    return false;
}

// 留空采用默认列名「新列 N」（N = 现有列数 + 1）
inline std::string defaultColumnTitle(size_t columnCount) {
    return "新列 " + std::to_string(columnCount + 1);
}

// 列内滚动位置（审计 [23]：按列 id 记忆——列重排/删列不再串列）
inline float columnScrollOffset(const std::vector<Column>& columns,
                                const std::map<int64_t, float>& scrolls,
                                size_t colIndex) {
    if (colIndex >= columns.size()) return 0.0f;
    auto it = scrolls.find(columns[colIndex].id);
    return it == scrolls.end() ? 0.0f : it->second;
}

// ==================== R5：看板几何 ====================
// drawBoard 与右键菜单定位共用（菜单坐标走模块自身布局空间，
// 不用指针事件的窗口坐标——页面在侧边栏右侧，两者相差一个侧边栏宽）
struct BoardGeom {
    float pad = 0.0f;
    float colW = 0.0f;
    float colGap = 0.0f;
    float boardTop = 0.0f;  // 看板区（列背景）顶边的模块空间 y
};

// screenWidth = 页面逻辑宽（调用点传 Screen::width）。收宽度而不是收
// eui::Screen：Screen 在 eui/dsl.h 里，本头是不碰 Ui 的纯函数层，
// 引 DSL 应用头只为拿一个 float 不值当（eui/types.h 的值类型桥接足够）。
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

// chip 宽度估算（fontSizeSmall-3 ≈ 11px：CJK ≈ 11px/字（3 字节），ASCII ≈ 5.5px/字节）
// ponytail: 估宽不发 FontRegistry 实测；偏差只影响 chip 间留白，可接受
inline float chipWidth(const std::string& text) {
    float w = 20.0f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? 4.0f : 5.5f;
    }
    return w;
}

// 改色预设（token 与 columnDotColor 的映射表一一对应；spec §8.2 承诺）
// 右键菜单「改色」子项与菜单分发共用：标签给 UI，token 给 service。
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
