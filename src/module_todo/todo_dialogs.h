// Todo 弹窗层：6 个对话框 + 右键菜单 + datepicker 的「打开状态 + 表单字段 + 渲染 + 提交」状态与契约。
// 状态是纯值类型（无模块/Ui 引用）；渲染与提交在 todo_ui_dialogs.cpp；落库动作走 DialogHost（模块构造期绑定），
// 弹窗层不捕获 TodoModule。元素 id/文案/几何/控制流是无头 UI 用例契约（按 id 定位与触发，不得改）。
#pragma once

#include "todo_ui_logic.h"

#include <core/theme.h>
#include <eui/dsl.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace module::todo::ui {

// 最上层弹窗单一持有者：items 最后打开的在栈顶，绘制 zIndex、Esc 关栈顶与之天然一致；toast 不入栈
enum class Popup {
    NewTask,
    NewColumn,
    Detail,
    Overdue,
    Ctx,
    DatePick,
    Rename,
    Confirm,
};

struct PopupStack {
    // 栈底 z / 每层增量：整栈叠满也只有 1070，须留在 toast（默认 1100）之下
    static constexpr int kBaseZ = 1000;
    static constexpr int kZStep = 10;

    std::vector<Popup> items;

    bool isOpen(Popup p) const {
        return std::find(items.begin(), items.end(), p) != items.end();
    }
    void close(Popup p) {
        items.erase(std::remove(items.begin(), items.end(), p), items.end());
    }
    void open(Popup p) {
        close(p);
        items.push_back(p);
    }
    // 组件 onOpenChange 通道（遮罩点击 / 下拉字段点击）
    void setOpen(Popup p, bool on) {
        if (on) open(p);
        else close(p);
    }
    // 栈内位置 → 绘制 z（栈顶最高）；不在栈中返回栈底值
    int zOf(Popup p) const {
        const auto it = std::find(items.begin(), items.end(), p);
        if (it == items.end()) return kBaseZ;
        return kBaseZ + kZStep * static_cast<int>(it - items.begin());
    }
    bool empty() const { return items.empty(); }
    Popup top() const { return items.back(); }
};

// 弹窗层 → 模块的通道：只读数据 + 数据层动作（模块构造期绑定一次），动作体读模块当下状态
struct DialogHost {
    const std::vector<Column>* columns = nullptr;
    const std::vector<Task>* tasks = nullptr;
    // 落库动作：返回空串 = 成功，非空 = 弹窗内嵌红字要显示的错误文案
    std::function<std::string(int64_t, const std::string&)> createTask;
    std::function<std::string(const std::string&)> createColumn;
    std::function<std::string(int64_t, const std::string&)> renameColumn;
    std::function<void(int64_t)> deleteTask;
    std::function<void(int64_t)> deleteColumn;
    // 表单落库（false = 失败，弹窗保持打开）
    std::function<bool(const Task&)> updateTask;
    std::function<void(int64_t, int64_t, int)> moveTask;
    std::function<void(int64_t)> completeTask;
    std::function<void(int64_t)> uncompleteTask;
    // 打开任务详情（选中态 + 视图状态落盘在模块侧）
    std::function<void(const Task&)> openDetail;
    // 右键菜单分发（菜单项语义与模块写操作强耦合，留在模块）
    std::function<void(const std::vector<int>&)> contextSelectPath;
    // 警告提示（详情弹窗「未存改动」守卫：第一次只警告不关）
    std::function<void(const std::string&)> warn;
};

struct DialogSet;

struct NewTaskDialog {
    int64_t columnId = 0;
    std::string title;
    std::string error;  // 内嵌红字（claude formError_ 模式）

    // 打开时清字段（错误随下一次输入清除）
    void reset(int64_t column) {
        columnId = column;
        title.clear();
        error.clear();
    }
    // 提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；成功 → 关窗。
    void submit(PopupStack& popups, const DialogHost& host);
};

struct NewColumnDialog {
    std::string title;
    std::string error;  // 内嵌红字

    void reset() {
        title.clear();
        error.clear();
    }
    void submit(PopupStack& popups, const DialogHost& host);
};

struct RenameDialog {
    int64_t columnId = 0;
    std::string title;
    std::string error;  // 内嵌红字

    void reset(const Column& col) {
        columnId = col.id;
        title = col.title;
        error.clear();
    }
    void submit(PopupStack& popups, const DialogHost& host);
};

// 双模式：taskId > 0 = 删任务；否则 columnId > 0 = 删列（CASCADE 连带删列下任务）
struct ConfirmDeleteDialog {
    int64_t columnId = 0;
    int64_t taskId = 0;
    std::string taskTitle;

    bool forTask() const { return taskId != 0; }
    void openForTask(int64_t id, std::string title) {
        taskId = id;
        taskTitle = std::move(title);
        columnId = 0;
    }
    void openForColumn(int64_t id) {
        columnId = id;
        taskId = 0;
        taskTitle.clear();
    }
    // 确认删除。任务路径：确认后才关详情弹窗（取消路径不关，回到弹窗继续编辑）
    void confirm(DialogSet& d, const DialogHost& host);
};

// 任务详情 / 编辑弹窗
struct DetailDialog {
    // backdrop 关闭守卫：有未存改动时第一次点遮罩只警告，再点一次才真正关闭。
    bool discardArmed = false;
    int64_t taskId = 0;
    // 列身份以库中为准（collect 从 findTask 读回，dirty 不比列）
    std::string title;
    std::string description;
    Priority priority = Priority::None;
    std::optional<int64_t> dueAt;
    bool completed = false;
    std::vector<std::string> tags;
    std::string tagInput;  // 标签输入框（回车追加，未落库）

    // 打开时从任务装载全部字段（选中态 + 视图状态落盘在模块侧）
    void reset(const Task& task);
    // 标签输入回车 → 追加（去空白/去重/空忽略；保存时才落库）
    void addTag();
    // 表单与库中任务是否有差异（backdrop 守卫用）；任务不在内存时按不脏处理
    bool dirty(const std::vector<Task>& tasks) const;
    // 表单字段 → Task（含 position 保留；completedAt 取当前完成态）
    Task collect(const std::vector<Task>& tasks) const;
    // 表单原样落库；false = 保存失败（已 setStatus 报错），调用方保持弹窗打开
    bool save(const DialogHost& host) const;
    // 详情弹窗关闭统一入口（Esc 兜底与 backdrop 点击共用同一份守卫）；false = 被未存改动守卫拦下
    bool requestClose(PopupStack& popups, const DialogHost& host);
};

// 右键菜单。菜单定位走模块自身布局空间（ui::boardGeom），不用指针窗口坐标（相差一个侧边栏宽），
// 锚点经 menuAnchorForPointer 统一换算。不变式：taskId 与 columnId 二选一
struct ContextMenuState {
    int64_t taskId = 0;
    int64_t columnId = 0;
    int columnIndex = 0;
    int cardIndex = -1;

    void openTask(const Task& task, int colIndex, int cardIndex) {
        taskId = task.id;
        columnId = 0;
        columnIndex = colIndex;
        this->cardIndex = cardIndex;
    }
    void openColumn(const Column& col, int colIndex) {
        taskId = 0;
        columnId = col.id;
        columnIndex = colIndex;
        cardIndex = -1;
    }
};

// 数据源 listOverdue()（模块打开弹窗时刷新；行点击 = 关本弹窗并打开该任务详情）
struct OverdueDialog {
    std::vector<Task> tasks;
};

// 弹窗层状态聚合（TodoModule 持有一个实例）
struct DialogSet {
    PopupStack popups;
    NewTaskDialog newTask;
    NewColumnDialog newColumn;
    RenameDialog rename;
    ConfirmDeleteDialog confirm;
    DetailDialog detail;
    ContextMenuState ctx;
    OverdueDialog overdue;
};

// 绘制（自由函数；状态显式入参，定义在 todo_ui_dialogs.cpp）
void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t, DialogSet& d,
                       const DialogHost& host);
void drawNewColumnDialog(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t, DialogSet& d,
                         const DialogHost& host);
void drawDetailDialog(eui::Ui& ui, const eui::Screen& screen,
                      const core::theme::Tokens& t, DialogSet& d,
                      const DialogHost& host);
void drawOverdueDialog(eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t, DialogSet& d,
                       const DialogHost& host);
void drawContextMenu(eui::Ui& ui, const eui::Screen& screen,
                     const core::theme::Tokens& t, DialogSet& d,
                     const DialogHost& host,
                     const std::map<int64_t, float>& columnScrolls);
void drawDatePicker(eui::Ui& ui, const eui::Screen& screen,
                    const core::theme::Tokens& t, DialogSet& d,
                    const DialogHost& host);
void drawRenameDialog(eui::Ui& ui, const eui::Screen& screen,
                      const core::theme::Tokens& t, DialogSet& d,
                      const DialogHost& host);
void drawConfirmDeleteDialog(eui::Ui& ui, const eui::Screen& screen,
                             const core::theme::Tokens& t, DialogSet& d,
                             const DialogHost& host);

} // namespace module::todo::ui
