// Todo 弹窗层：6 个对话框 + 右键菜单 + datepicker 的「打开状态 + 表单字段 + 渲染 + 提交」状态与契约。
// 状态是纯值类型（无模块/Ui 引用）；渲染与提交在 todo_ui_dialogs.cpp；落库动作走 DialogHost（模块构造期绑定），
// 弹窗层不捕获 TodoModule。元素 id/文案/几何/控制流是无头 UI 用例契约（按 id 定位与触发，不得改）。
#pragma once

#include "todo_ui_logic.h"

#include <core/markdown_notes_source.h>
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
    Detail,
    Overdue,
    Ctx,
    DatePick,
    Confirm,
    DocPick,   // UAT6.2 H31：详情内「关联文档」选择器（与 Detail 同栈共存，z 更高）
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
    // 落库动作：返回空串 = 成功，非空 = 弹窗内嵌红字要显示的错误文案。
    // UAT5 方案 A 富创建：attrs（描述/优先级/截止）与 createTask 一次 INSERT 落库；
    // tags 走建成后的 updateTask 补写（service 的 NewTaskAttributes 不收 tags）
    std::function<std::string(int64_t, const std::string&, const NewTaskAttributes&,
                              const std::vector<std::string>&)>
        createTask;
    std::function<void(int64_t)> deleteTask;
    // 表单落库（false = 失败，弹窗保持打开）
    std::function<bool(const Task&)> updateTask;
    std::function<void(int64_t, int64_t, int)> moveTask;
    std::function<void(int64_t)> completeTask;
    std::function<void(int64_t)> uncompleteTask;
    // UAT6.2 H31 关联文档三通道：打开选择器（模块侧取笔记清单）、跳转打开关联
    // 文档（模块侧导航 Markdown 页 + open: 负载）、存在性判定（文档灰态用）
    std::function<void()> openDocPicker;
    std::function<void(const std::string&)> openLinkedDoc;
    std::function<bool(const std::string&)> docExists;
    // 提供方可用性（H31）：Markdown 模块被禁用时与「文档不存在」区分灰态文案
    std::function<bool()> notesSourceAvailable;
    // 打开任务详情（选中态 + 视图状态落盘在模块侧）
    std::function<void(const Task&)> openDetail;
    // 右键菜单分发（菜单项语义与模块写操作强耦合，留在模块）
    std::function<void(const std::vector<int>&)> contextSelectPath;
    // 警告提示（详情弹窗「未存改动」守卫：第一次只警告不关）
    std::function<void(const std::string&)> warn;
};

struct DialogSet;

// 标签输入回车 → 追加（去空白/去重/空忽略）；NewTaskDialog 与 DetailDialog 共用同一语义
inline void appendTag(std::vector<std::string>& tags, std::string& input) {
    std::string tag;
    for (char c : input) {
        if (c != ' ' && c != '\t') tag += c;
    }
    input.clear();
    if (tag.empty()) return;
    for (const auto& existing : tags) {
        if (existing == tag) return;
    }
    tags.push_back(std::move(tag));
}

// 截止快选档位（方案 A 截止 segment）：0=今天 1=明天 2=自选（datepicker 写回）
enum class DueChoice { Today = 0, Tomorrow = 1, Custom = 2 };

// datepicker 当前归属弹窗——「选择…」入口在哪个弹窗里，滚轮锚点读谁的表单、
// 确定写回谁的 dueAt（详情与新建可同时在栈上，不能靠「谁开着」推断）
enum class DatePickOwner { Detail, NewTask };

// UAT5 方案 A 富创建（demo task-new-dialog-redesign.html）：一次给全
// 标题/描述/优先级/截止/标签，字段映射现有 Task 数据模型（只填提供的，其余默认）
struct NewTaskDialog {
    int64_t columnId = 0;
    std::string title;
    std::string description;                 // 描述（可选）
    Priority priority = Priority::None;
    std::optional<int64_t> dueAt;            // 已选截止；nullopt = 未选（segment 无选中）
    DueChoice dueChoice = DueChoice::Today;  // 仅当 dueAt 有值时代表选中档
    std::vector<std::string> tags;
    std::string tagInput;  // 标签输入框（回车追加，未落库）
    std::string error;      // 内嵌红字（claude formError_ 模式）

    // 打开时清全部字段（错误随下一次输入清除）
    void reset(int64_t column) {
        columnId = column;
        title.clear();
        description.clear();
        priority = Priority::None;
        dueAt = std::nullopt;
        dueChoice = DueChoice::Today;
        tags.clear();
        tagInput.clear();
        error.clear();
    }
    void addTag() { appendTag(tags, tagInput); }
    // 提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；成功 → 关窗。
    void submit(PopupStack& popups, const DialogHost& host);
};

// 删除任务确认弹窗（「删除」按钮 onClick 即 confirm；取消路径不落库）
struct ConfirmDeleteDialog {
    int64_t taskId = 0;
    std::string taskTitle;

    void openForTask(int64_t id, std::string title) {
        taskId = id;
        taskTitle = std::move(title);
    }
    // 确认后才关详情弹窗（取消路径不关，回到弹窗继续编辑）
    void confirm(DialogSet& d, const DialogHost& host);
};

// 任务详情 / 编辑弹窗
struct DetailDialog {
    // backdrop 关闭守卫：有未存改动时第一次点遮罩只警告，再点一次才真正关闭。
    bool discardArmed = false;
    // 归档列任务只读：隐藏保存/删除/切换完成按钮，输入框不可编辑。判据 = 所在列 kind=='archive'（运行期），
    // 不靠 archived_at 值——拖出归档列后 archived_at 被清，不应残留只读态。
    bool readOnly = false;
    int64_t taskId = 0;
    // 列身份以库中为准（collect 从 findTask 读回，dirty 不比列）
    std::string title;
    std::string description;
    Priority priority = Priority::None;
    std::optional<int64_t> dueAt;
    bool completed = false;
    std::vector<std::string> tags;
    std::string tagInput;  // 标签输入框（回车追加，未落库）
    // UAT6.2 H31 关联文档：docPath = 笔记相对路径（空串 = 未关联）；docTitle =
    // 选择/打开时解析的显示名（不落库，跳转与灰态判定只用 docPath）
    std::string docPath;
    std::string docTitle;

    // 打开时从任务装载全部字段（选中态 + 视图状态落盘在模块侧）
    void reset(const Task& task);
    // 标签输入回车 → 追加（去空白/去重/空忽略；保存时才落库）
    void addTag() { appendTag(tags, tagInput); }
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
    float anchorX = 0.0f;

    void openTask(const Task& task, int colIndex, int cardIndex, float anchorX) {
        taskId = task.id;
        columnId = 0;
        columnIndex = colIndex;
        this->cardIndex = cardIndex;
        this->anchorX = anchorX;
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
    ConfirmDeleteDialog confirm;
    DetailDialog detail;
    ContextMenuState ctx;
    OverdueDialog overdue;
    // datepicker 归属弹窗（详情「选择日期…」/ 新建「选择…」两个入口都写它）：
    // 打开时由入口设置；锚点读取与确定写回都按它路由
    DatePickOwner datePickOwner = DatePickOwner::Detail;
    // UAT6.2 H31：文档选择器清单（模块在 openDocPicker 时刷新填充）
    std::vector<::core::NoteRef> docPickNotes;
    // datepicker 本次打开已绘制的帧数（UAT5「默认当天」机制，详见 drawDatePicker）：
    // 首帧喂锚点日期初始化滚轮 + 放掉上一会话草稿，后续帧喂哨兵提交值——组件的
    // 「确定」只在「滚轮草稿 ≠ 提交值」时回调 onChange，不喂哨兵则「打开不滚直接
    // 确定」永远不回调。关闭态每帧归零（Esc/确定/遮罩全路径覆盖）。
    int datePickDrawn = 0;
};

// 绘制（自由函数；状态显式入参，定义在 todo_ui_dialogs.cpp）
void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
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
void drawConfirmDeleteDialog(eui::Ui& ui, const eui::Screen& screen,
                             const core::theme::Tokens& t, DialogSet& d,
                             const DialogHost& host);
// UAT6.2 H31：关联文档选择器（清单来自 DialogSet::docPickNotes，模块打开时填充）
void drawDocPickerDialog(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t, DialogSet& d,
                         const DialogHost& host);

} // namespace module::todo::ui
