// src/module_todo/todo_ui_board.h
// Todo 看板区的绘制单元（R5 / god-class spec §4.2）：表头、看板、列、列内容、卡片。
//
// 边界（与 md_* 单元、ui_kit 同一约定）：
//   - 不持有模块状态、不捕获 this：凡是画图要用的数据都经 BoardView 显式入参传入
//     （成员引用 + 少量当帧标量快照），要回写模块的动作走 BoardActions；
//   - 元素 id、文案、几何、控制流逐字沿用拆分前 todo_module.cpp 的对应函数——
//     无头 UI 用例按 id 定位，不得改；
//   - 拖拽状态机（DragState 的 pressX/pressY/pressScrollOffset 同帧快照，tech-debt B）
//     不属于本单元：它留在 TodoModule，本单元只画「拖动中」的视觉（半透明卡片 +
//     目标列高亮），并把 onPress/onDrag/onRelease 原样转交出去。
#pragma once

#include "todo_ui_logic.h"

#include <core/theme.h>
#include <eui/dsl.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace module::todo::ui {

// 板面几何快照（drawBoard / drawCard 每帧刷新，拖拽命中换算用）。
// 写权在本单元（「上一帧画出来的几何」的唯一来源），读权在模块（hitDropColumn）
// 与命中换算是同一份数据——故类型随本单元走，模块持有一个实例。
struct BoardPlate {
    float colW = 0.0f;
    float colGap = 0.0f;
    float height = 0.0f;
    float cardW = 0.0f;
};

// 看板 → 模块的回写通道（模块构造期绑定一次；回调体写在模块侧，读的是模块
// 的**当下**状态——与拆分前 `[this] { ... }` 同款语义，不是当帧快照）。
struct BoardActions {
    std::function<void(const std::string&)> searchChanged;
    std::function<void()> searchCancelled;
    std::function<void()> overdueClicked;
    std::function<void()> addColumnClicked;
    std::function<void()> addTaskClicked;
    std::function<void()> retryClicked;
    std::function<void(const Column&, int)> columnMenu;
    std::function<void(const Column&)> renameColumn;
    std::function<void(int64_t)> addTaskToColumn;
    std::function<void(size_t, float)> columnScrollChanged;
    std::function<void(const Task&)> cardClicked;
    std::function<void(const Task&, int, int)> cardMenu;
    std::function<void(const Task&, int, int, const eui::Rect&)> cardPressed;
    std::function<void(double, double)> cardDragged;
    std::function<void(double, double, bool)> cardReleased;
};

// 看板绘制的只读输入（模块每帧组装一次）。容器是模块成员的引用（不拷贝，
// 免每帧分配），拖拽中间态与两个标量取当帧值快照。
struct BoardView {
    const std::vector<Column>& columns;
    const std::vector<Task>& tasks;
    const std::unordered_set<int64_t>& searchMatchIds;
    const std::string& searchQuery;
    const std::map<int64_t, float>& columnScrolls;
    int64_t selectedTaskId = 0;
    bool dragActive = false;
    int64_t dragTaskId = 0;
    int dragHoverColumn = -1;
    // UI 整改 13：拖拽插入行预览（-1 = 不预览；模块只在跨列时置位，
    // 0 = 首卡前，>= 列卡数 = 列尾）。看板单元据此画目标位占位条。
    int dragHoverRow = -1;
    size_t overdueCount = 0;
    bool hasService = false;
    const std::string& loadError;
    const BoardActions& act;
    // M2-3（2026-09-28，UI 现代化）：卡片落位淡入标志（模块状态当帧快照）。
    // 拖拽结束后模块置 false → 列内容容器 0.6 → 1.0 淡回（0.12s OutQuad）；
    // 跟手期间恒 true（onDrag 通道不触碰，动画不与拖拽打架）
    bool columnFadeIn = true;
};

// 表头（标题 + 搜索框 + 逾期入口 + 建列/建任务）
void drawHeader(eui::Ui& ui, float contentW, float headerH, float bodyH,
                const core::theme::Tokens& t, const BoardView& v);

// 看板主体（错误态 / 空态 / 列行）。plate 为出参：每帧刷新几何快照。
void drawBoard(eui::Ui& ui, float contentW, float bodyH,
               const core::theme::Tokens& t, const BoardView& v, BoardPlate& plate);

} // namespace module::todo::ui
