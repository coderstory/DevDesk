// src/module_todo/todo_dialogs.h
// Todo 弹窗层（R5 / god-class spec §4.2）：6 个对话框 + 右键菜单 + datepicker 的
// 「打开状态 + 表单字段 + 渲染 + 提交」四者同居的状态与契约。
//
// 边界（同 todo_ui_board.h）：
//   - 状态是纯值类型（无模块引用、无 Ui 引用）；渲染与提交在 todo_ui_dialogs.cpp，
//     与各自 struct 紧邻定义；
//   - 要落库的动作走 DialogHost（模块构造期绑定）——弹窗层不捕获 TodoModule；
//   - 元素 id、文案、几何、控制流逐字沿用拆分前 todo_module.cpp 的对应函数
//     （无头 UI 用例按 id 定位，且按 id 触发回调，不得改）。
//
// 「谁在最上层」的唯一真相源是 PopupStack（审计 THS-05）：拆分成独立单元时
// **整段搬来这里**而不是拆成 per-dialog 的 open 标志——单一持有者的语义不变
//（打开即入栈 / 关栈顶 / z 序由栈内位置派生），新增一个弹窗仍只需改这一处。
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

// ============ 最上层弹窗的单一持有者（审计 THS-05）============
// 此前「谁在最上层」由三处各自维护：每个弹窗一个 bool、closeTopmostPopup
// 手写关闭顺序、绘制时手挑 zIndex——新增一个弹窗要同时改三处，漏改即静默
// 降级（Esc 关错层 / 层级错乱）。现在只有一个真相源 items：
//   · 谁最后打开谁在最上（= 栈顶）
//   · 绘制 zIndex = 栈内位置（不再手挑）
//   · Esc 关栈顶，因此天然与 z 序一致
// toast 不入栈：它不是互相竞争层级的弹窗，而是自动消失的状态提示
//（toast 状态位在 TodoModule，走 ui_kit::ToastState）。
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
    // 栈底 z / 每层增量：整栈上界须留在 toast 之下（EUI-NEO toast 默认
    // 1100）——8 个弹窗全叠满也只有 1070，状态提示不会被弹窗盖住
    static constexpr int kBaseZ = 1000;
    static constexpr int kZStep = 10;

    std::vector<Popup> items;

    bool isOpen(Popup p) const {
        return std::find(items.begin(), items.end(), p) != items.end();
    }
    void close(Popup p) {
        items.erase(std::remove(items.begin(), items.end(), p), items.end());
    }
    // 打开：已在栈中则提到栈顶（「最后打开的在最上」），否则入栈
    void open(Popup p) {
        close(p);
        items.push_back(p);
    }
    // 组件 onOpenChange 通道（弹窗遮罩点击 / 下拉字段点击）
    void setOpen(Popup p, bool on) {
        if (on) open(p);
        else close(p);
    }
    // 栈内位置 → 绘制 z（栈顶最高）；不在栈中 = 未显示，返回栈底值
    int zOf(Popup p) const {
        const auto it = std::find(items.begin(), items.end(), p);
        if (it == items.end()) return kBaseZ;
        return kBaseZ + kZStep * static_cast<int>(it - items.begin());
    }
    bool empty() const { return items.empty(); }
    Popup top() const { return items.back(); }
};

// 弹窗层 → 模块的通道：只读数据 + 数据层动作（模块构造期绑定一次）。
// 动作体写在模块侧，读的是模块**当下**状态——与拆分前的成员方法调用同款语义。
struct DialogHost {
    const std::vector<Column>* columns = nullptr;
    const std::vector<Task>* tasks = nullptr;
    // 落库动作：返回空串 = 成功，非空 = 弹窗内嵌红字要显示的错误文案
    //（与 TodoModule::doCreateTask / doCreateColumn / doRenameColumn 同语义）
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

// ---- 新建任务弹窗 ----
struct NewTaskDialog {
    int64_t columnId = 0;
    std::string title;
    std::string error;  // 审计 [19]：内嵌红字（claude formError_ 模式）

    // 打开时清字段（审计 [19]：错误随下一次输入清除）
    void reset(int64_t column) {
        columnId = column;
        title.clear();
        error.clear();
    }
    // 审计 [19]：提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；
    // 成功 → 关窗 + toast 确认。此前「先关弹窗再报 toast」，反馈隔在窗外。
    void submit(PopupStack& popups, const DialogHost& host);
};

// ---- 新建列弹窗（审计 [24]）----
struct NewColumnDialog {
    std::string title;
    std::string error;  // 审计 [19]：内嵌红字

    void reset() {
        title.clear();
        error.clear();
    }
    // 审计 [19]：显式输入的列名撞名 / 创建失败 → 弹窗内嵌红字，弹窗不关。
    // 留空采用默认名「新列 N」的便利路径不查重（默认名是序号兜底，撞名
    // 也不该把用户困在弹窗里）。
    void submit(PopupStack& popups, const DialogHost& host);
};

// ---- 重命名列弹窗 ----
struct RenameDialog {
    int64_t columnId = 0;
    std::string title;
    std::string error;  // 审计 [19]：内嵌红字

    void reset(const Column& col) {
        columnId = col.id;
        title = col.title;
        error.clear();
    }
    // 审计 [19]：重命名提交——空名此前静默返回（弹窗已关、零反馈），
    // 撞名此前根本不校验；统一为弹窗内嵌红字 + 弹窗不关。
    void submit(PopupStack& popups, const DialogHost& host);
};

// ---- 删除确认弹窗 ----
// 双模式：taskId > 0 = 删任务；否则 columnId > 0 = 删列
//（外键 CASCADE 会连带删掉列下全部任务）
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
    // 确认删除。任务路径：确认后才关详情弹窗（取消路径不关，回到弹窗
    // 继续编辑——产品审计 2026-09-23 [2]）
    void confirm(DialogSet& d, const DialogHost& host);
};

// ---- 任务详情 / 编辑弹窗 ----
struct DetailDialog {
    // backdrop 关闭守卫（审计 2026-09-23 [2]）：有未存改动时第一次点遮罩
    // 只警告（置位），再点一次才真正关闭放弃修改。仅在弹窗打开时有效。
    bool discardArmed = false;
    int64_t taskId = 0;
    int64_t columnId = 0;
    std::string title;
    std::string description;
    Priority priority = Priority::None;
    std::optional<int64_t> dueAt;
    bool completed = false;
    std::vector<std::string> tags;
    std::string tagInput;  // 标签输入框（回车追加，未落库）

    // 打开时从任务装载全部字段（选中态 + 视图状态落盘在模块侧）
    void reset(const Task& task);
    // 标签输入回车 → 追加（去空白 / 去重 / 空忽略；保存时才落库）
    void addTag();
    // 表单与库中任务是否有差异（backdrop 关闭守卫用）。任务不在内存
    //（已被别的路径删掉）时无法比较，按不脏处理
    bool dirty(const std::vector<Task>& tasks) const;
    // 表单字段 → Task（含 position 保留；completedAt 取当前完成态）
    Task collect(const std::vector<Task>& tasks) const;
    // 表单原样落库；false = 保存失败（已 setStatus 报错），调用方应保持
    // 弹窗打开并放弃后续动作
    bool save(const DialogHost& host) const;
    // 详情弹窗关闭统一入口（审计 M5：Esc 兜底与 backdrop 点击共用同一份守卫，
    // 避免两处各写一遍「未存改动先警告」而语义漂移）。
    // 返回 true = 本次真的关了；false = 被未存改动守卫拦下（弹窗仍在栈顶）
    bool requestClose(PopupStack& popups, const DialogHost& host);
};

// ---- 右键菜单 ----
// 菜单定位走模块自身布局空间（ui::boardGeom），不用指针事件的窗口坐标
//（页面在侧边栏右侧，两者相差一个侧边栏宽）。
// 不变式：taskId 与 columnId 二选一（见 openTask / openColumn）。
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

// ---- 逾期任务弹窗（三件套 2）----
// 数据源 service_->listOverdue()（模块打开弹窗时刷新；行点击 = 关本弹窗并
// 打开该任务详情）。徽标计数在模块（看板表头显示用）。
struct OverdueDialog {
    std::vector<Task> tasks;
};

// ---- 弹窗层状态聚合（TodoModule 持有一个实例）----
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

// ---- 绘制（自由函数；状态显式入参，与各自的 struct 紧邻定义在 todo_ui_dialogs.cpp）----
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
                     const DialogHost& host);
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
