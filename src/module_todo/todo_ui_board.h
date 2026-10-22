// Todo 看板区绘制单元：表头、看板、列、列内容、卡片。
// 不持有模块状态、不捕获 this：画图数据经 BoardView 显式入参，回写动作走 BoardActions。
// 元素 id/文案/几何/控制流是无头 UI 用例契约（按 id 定位，不得改）。拖拽状态机留在 TodoModule，
// 本单元只画「拖动中」视觉（源列调暗 + 目标列高亮 + 占位条 + 跟手副本），原样转交指针事件。
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

// 板面几何快照（drawBoard / drawCard 每帧刷新）：写权在本单元、读权在模块（hitDropColumn）
struct BoardPlate {
    float colW = 0.0f;
    float colGap = 0.0f;
    float height = 0.0f;
    float cardW = 0.0f;
    int64_t archiveColumnId = 0;
};

// 看板 → 模块的回写通道（模块构造期绑定一次）；回调体写在模块侧，读模块当下状态，不是当帧快照
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
    // 四参：指针 x/y（命中换算，物理像素）+ 按下点起累计位移 totalX/totalY（跟手副本锚点用，框架算）
    std::function<void(double, double, double, double)> cardDragged;
    std::function<void(double, double, bool)> cardReleased;
    std::function<void()> archiveToggle;
};

// 看板绘制的只读输入（模块每帧组装一次）：容器是模块成员的引用（不拷贝），拖拽中间态与标量取当帧快照
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
    // 拖拽插入行预览（-1 = 不预览，0 = 首卡前，>= 列卡数 = 列尾）
    int dragHoverRow = -1;
    // 拖拽跟手副本在页面 compose 空间的左上角（模块每帧换算，见 ui::dragGhostAnchor）。
    // 给的是绝对锚点不是「卡内偏移」——副本画在页面顶层 root（列内 translate 会被 scrollView 的 clip 剪掉）
    bool dragGhostOn = false;
    float dragGhostX = 0.0f;
    float dragGhostY = 0.0f;
    size_t overdueCount = 0;
    bool hasService = false;
    const std::string& loadError;
    const BoardActions& act;
    // 落位淡入按 dragActive 门控列内容容器 opacity：拖拽中 0.6，松手 0.6→1.0 淡回（0.12s OutQuad）
    bool archiveExpanded = false;
};

// 表头（标题 + 搜索框 + 逾期入口 + 建列/建任务）
void drawHeader(eui::Ui& ui, float contentW, float headerH, float bodyH,
                const core::theme::Tokens& t, const BoardView& v);

// 看板主体（错误态 / 空态 / 列行）。plate 为出参：每帧刷新几何快照。
void drawBoard(eui::Ui& ui, float contentW, float bodyH,
               const core::theme::Tokens& t, const BoardView& v, BoardPlate& plate);

// 拖拽跟手副本（页面级顶层 root：画在 todo.root 之后、弹窗之前）；v.dragGhostOn = false 时零元素产出
void drawDragGhost(eui::Ui& ui, const core::theme::Tokens& t,
                   const BoardView& v, const BoardPlate& plate);

} // namespace module::todo::ui
