// src/module_todo/todo_module.cpp
// Todo 看板模块 - 真实数据 + CRUD（TodoService）
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/time.h"
#include "todo_service.h"
#include "todo_ui_logic.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/contextmenu.h>
#include <components/datepicker.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/theme.h>
#include <components/toast.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace module::todo {

namespace {

// ---------- 卡片尺寸 ----------
// 常量定义在 todo_ui_logic.h（拖拽命中换算共用）
using module::todo::ui::kCardHeight;
using module::todo::ui::kCardGap;
using module::todo::ui::kColumnHeaderHeight;

// 优先级 → 中文标签
const char* priorityLabel(Priority p) {
    switch (p) {
        case Priority::High:   return "🔴高";
        case Priority::Medium: return "🟡中";
        case Priority::Low:    return "🔵低";
        default:               return "⚪无";
    }
}

eui::Color priorityColor(Priority p, const core::theme::Tokens& t) {
    switch (p) {
        case Priority::High:   return t.colors.error;
        case Priority::Medium: return t.colors.warning;
        case Priority::Low:    return t.colors.info;
        default:               return t.colors.textTertiary;
    }
}

// epoch 秒 → "MM-DD"；-1 表示无截止日期
std::string formatDue(int64_t epoch) {
    if (epoch <= 0) return {};
    std::time_t tt = static_cast<std::time_t>(epoch);
    std::tm tmv{};
    core::time::localTime(tt, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d-%02d", tmv.tm_mon + 1, tmv.tm_mday);
    return std::string(buf);
}

bool isOverdue(const Task& task) {
    if (!task.dueAt.has_value() || task.completedAt.has_value()) return false;
    return task.dueAt.value() < static_cast<int64_t>(std::time(nullptr));
}

// ---------- 看板几何 ----------
// drawBoard 与右键菜单定位共用（菜单坐标走模块自身布局空间，
// 不用指针事件的窗口坐标——页面在侧边栏右侧，两者相差一个侧边栏宽）
struct BoardGeom {
    float pad = 0.0f;
    float colW = 0.0f;
    float colGap = 0.0f;
    float boardTop = 0.0f;  // 看板区（列背景）顶边的模块空间 y
};

BoardGeom boardGeom(const eui::Screen& screen, size_t columnCount,
                    const core::theme::Tokens& t) {
    BoardGeom g;
    g.pad = t.sizes.spacingXXLarge;
    g.colGap = t.sizes.spacingLarge;
    const float contentW = screen.width - g.pad * 2.0f;
    const auto n = static_cast<float>(columnCount == 0 ? 1 : columnCount);
    g.colW = (contentW - g.colGap * (n - 1.0f)) / n;
    g.boardTop = g.pad + 64.0f /*headerH*/ + t.sizes.spacingLarge;
    return g;
}

float cardLeft(const BoardGeom& g, int columnIndex) {
    return g.pad + static_cast<float>(columnIndex) * (g.colW + g.colGap);
}

float cardTop(const BoardGeom& g, int cardIndex) {
    return g.boardTop + kColumnHeaderHeight +
           static_cast<float>(cardIndex) * (kCardHeight + kCardGap);
}

// 小标签（pill）
void drawChip(eui::Ui& ui,
              const std::string& id,
              float x,
              float y,
              float w,
              const core::theme::Tokens& t,
              const std::string& text,
              const eui::Color& fg,
              const eui::Color& bg,
              float opacity = 1.0f) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, 18.0f)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .opacity(opacity)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(w, 18.0f)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 3.0f)
        .lineHeight(18.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .opacity(opacity)
        .build();
}

} // namespace

class TodoModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Todo; }
    std::string displayName() const override { return "Todo 看板"; }
    std::string iconGlyph() const override { return eui::utf8(0xF058); }  // fa-circle-check

    // ============ 生命周期 ============

    void onStart() override {
        auto* db = ::core::appDatabase();
        if (!db) {
            LOG_WARN("Todo: 数据库尚未注册，跳过数据加载");
            return;
        }
        service_ = std::make_unique<TodoService>(*db);
        reloadAll();
        LOG_INFO("Todo 模块启动（" + std::to_string(columns_.size()) + " 列 / " +
                 std::to_string(tasks_.size()) + " 任务）");
    }

    void onStop() override {
        service_.reset();
        LOG_INFO("Todo 模块停止");
    }

    // ============ 数据加载 ============

    void reloadAll() {
        if (!service_) return;
        loadError_.clear();
        auto cols = service_->listColumns();
        if (cols.isOk()) {
            columns_ = std::move(cols).value();
        } else {
            loadError_ = "读取列失败: " + cols.errorMessage();
            LOG_ERROR("Todo: " + loadError_);
        }
        auto all = service_->listAllTasks();
        if (all.isOk()) {
            tasks_ = std::move(all).value();
        } else {
            loadError_ = "读取任务失败: " + all.errorMessage();
            LOG_ERROR("Todo: " + loadError_);
        }
        columnScrolls_.resize(columns_.size(), 0.0f);
        // 拖拽中任务被外部操作删除（如确认弹窗回调）→ 拖拽锚点失效，复位防悬垂
        if (drag_.active && findTask(drag_.taskId) == nullptr) {
            drag_ = DragState{};
        }
    }

    // 错误态重试：数据库晚注册（onStart 时为 null）或读取失败后手动恢复
    void retryLoad() {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            setStatus("数据库尚未注册，无法重试", true);
            return;
        }
        if (!service_) service_ = std::make_unique<TodoService>(*db);
        reloadAll();
        if (loadError_.empty()) {
            setStatus("已重新加载");
        } else {
            setStatus("重试仍失败：" + loadError_, true);
        }
    }

    std::vector<const Task*> tasksInColumn(int64_t columnId) const {
        std::vector<const Task*> out;
        for (const auto& t : tasks_) {
            if (t.columnId == columnId) out.push_back(&t);
        }
        std::sort(out.begin(), out.end(), [](const Task* a, const Task* b) {
            return a->position < b->position;
        });
        return out;
    }

    // 按语义标记找列 id（'done'=已完成列 / 'todo'=待办列；找不到返回 0）
    // 不按标题找：用户重命名列后标题不可靠（migration v4 起 kind 是权威）
    int64_t findColumnByKind(const std::string& kind) const {
        for (const auto& c : columns_) {
            if (c.kind == kind) return c.id;
        }
        return 0;
    }

    // ============ 写操作（都落到 SQLite） ============

    void doCreateTask(int64_t columnId, const std::string& title) {
        if (!service_) return;
        if (title.empty()) {
            setStatus("任务标题不能为空", true);
            return;
        }
        auto r = service_->createTask(columnId, title);
        if (r.isFail()) {
            setStatus("新建任务失败：" + r.errorMessage(), true);
            LOG_ERROR("Todo: createTask 失败: " + r.errorMessage());
            return;
        }
        setStatus("已创建任务「" + title + "」");
        reloadAll();
    }

    void doCreateColumn(const std::string& title) {
        if (!service_) return;
        auto r = service_->createColumn(title);
        if (r.isFail()) {
            setStatus("新建列失败：" + r.errorMessage(), true);
            return;
        }
        setStatus("已创建列「" + title + "」");
        reloadAll();
    }

    // 返回是否成功（拖拽收尾要区分成败给提示；其余调用方忽略返回值）
    bool doMoveTask(int64_t taskId, int64_t toColumnId, int newPosition) {
        if (!service_) return false;
        auto r = service_->moveTask(taskId, toColumnId, newPosition);
        if (r.isFail()) {
            setStatus("移动失败：" + r.errorMessage(), true);
            return false;
        }
        reloadAll();
        return true;
    }

    void doCompleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->completeTask(taskId);
        if (r.isFail()) {
            setStatus("标记完成失败：" + r.errorMessage(), true);
            return;
        }
        // 看板行为：完成后移到已完成列（kind='done'），追加到列尾
        const int64_t doneCol = findColumnByKind("done");
        if (doneCol != 0) {
            auto mv = service_->moveTask(
                taskId, doneCol, ui::computeAppendPosition(tasks_, doneCol));
            if (mv.isFail()) {
                LOG_WARN("Todo: 完成后移动到已完成列失败: " + mv.errorMessage());
            }
        } else {
            // 明示而非静默：没有 done 列时任务留在原列，用户需要知道为什么
            setStatus("任务已完成（未找到 kind='done' 的列，任务留在原列）");
            reloadAll();
            return;
        }
        setStatus("任务已完成 ✓");
        reloadAll();
    }

    void doUncompleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->uncompleteTask(taskId);
        if (r.isFail()) {
            setStatus("取消完成失败：" + r.errorMessage(), true);
            return;
        }
        // 撤回到待办列（kind='todo'），追加到列尾。
        // 此前假设 columns_.front() 是待办列——用户重排列后会把任务
        // 撤到错误的列，改为按语义标记定位。
        const int64_t todoCol = findColumnByKind("todo");
        if (todoCol != 0) {
            auto mv = service_->moveTask(
                taskId, todoCol, ui::computeAppendPosition(tasks_, todoCol));
            if (mv.isFail()) {
                LOG_WARN("Todo: 取消完成后移回待办列失败: " + mv.errorMessage());
            }
        } else {
            setStatus("已取消完成（未找到 kind='todo' 的列，任务留在原列）");
            reloadAll();
            return;
        }
        setStatus("已取消完成");
        reloadAll();
    }

    void doDeleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->deleteTask(taskId);
        if (r.isFail()) {
            setStatus("删除失败：" + r.errorMessage(), true);
            return;
        }
        setStatus("已删除任务");
        reloadAll();
    }

    void doUpdateTask(const Task& task) {
        if (!service_) return;
        auto r = service_->updateTask(task);
        if (r.isFail()) {
            setStatus("保存失败：" + r.errorMessage(), true);
            return;
        }
        setStatus("已保存");
        reloadAll();
    }

    // ============ 列管理（service 四 API 中的 rename/delete/reorder 接线） ============

    void doRenameColumn(int64_t id, const std::string& title) {
        if (!service_ || title.empty()) return;
        auto r = service_->renameColumn(id, title);
        if (r.isFail()) {
            setStatus("重命名失败：" + r.errorMessage(), true);
            return;
        }
        setStatus("已重命名列");
        reloadAll();
    }

    void doDeleteColumn(int64_t id) {
        if (!service_) return;
        auto r = service_->deleteColumn(id);
        if (r.isFail()) {
            setStatus("删除失败：" + r.errorMessage(), true);
            return;
        }
        setStatus("已删除列（含其下全部任务）");
        reloadAll();
    }

    // dir = -1 左移一位 / +1 右移一位（交换相邻列后整体重排 position）
    void doShiftColumn(int64_t id, int dir) {
        if (!service_) return;
        std::vector<int64_t> ids;
        ids.reserve(columns_.size());
        size_t idx = columns_.size();
        for (size_t i = 0; i < columns_.size(); ++i) {
            ids.push_back(columns_[i].id);
            if (columns_[i].id == id) idx = i;
        }
        const size_t j = (dir < 0) ? (idx == 0 ? idx : idx - 1)
                                   : (idx + 1 >= ids.size() ? idx : idx + 1);
        if (idx >= ids.size() || j == idx) {
            setStatus(dir < 0 ? "已经在最左边了" : "已经在最右边了");
            return;
        }
        std::swap(ids[idx], ids[j]);
        auto r = service_->reorderColumns(ids);
        if (r.isFail()) {
            setStatus("重排列失败：" + r.errorMessage(), true);
            return;
        }
        reloadAll();
    }

    // ============ 查找辅助 ============

    // 列内滚动位置（按列索引记忆；reloadAll 时 resize 保留旧值）
    float colScrollOffset(size_t colIndex) const {
        return colIndex < columnScrolls_.size() ? columnScrolls_[colIndex] : 0.0f;
    }
    void setColScrollOffset(size_t colIndex, float v) {
        if (colIndex < columnScrolls_.size()) columnScrolls_[colIndex] = v;
    }

    // ---- W8 拖拽（真 drop 语义）----

    // 指针落点 → 目标列索引（列外/板外 → -1）。换算细节见 ui::dragDropTarget。
    int hitDropColumn(double x, double y) const {
        ui::DragHitInput in;
        in.fromColIndex = drag_.fromColIndex;
        in.colCount = static_cast<int>(columns_.size());
        in.pressX = drag_.pressX;
        in.pressY = drag_.pressY;
        in.pressWidth = drag_.pressWidth;
        in.cardWidth = boardCardW_;
        in.colWidth = boardColW_;
        in.colGap = boardColGap_;
        in.boardHeight = boardH_;
        in.scrollOffset =
            drag_.fromColIndex >= 0 ? colScrollOffset(static_cast<size_t>(drag_.fromColIndex)) : 0.0f;
        in.cardIndex = drag_.cardIndex;
        return ui::dragDropTarget(in, x, y);
    }

    // 松手收尾：目标无效或等于源列 → 取消（卡片回原位）；
    // 否则按稳定 taskId 落到目标列尾（computeAppendPosition），消灭过期索引
    void finishDrag(int target) {
        const int64_t taskId = drag_.taskId;
        const int from = drag_.fromColIndex;
        drag_ = DragState{};  // 先复位（拖拽中间态结束，无论成败）
        if (target < 0 || target == from) return;
        if (target >= static_cast<int>(columns_.size())) return;
        const auto& dst = columns_[static_cast<size_t>(target)];
        const std::string title = dst.title;
        if (doMoveTask(taskId, dst.id, ui::computeAppendPosition(tasks_, dst.id))) {
            setStatus("已移动到「" + title + "」");  // 失败时保留 doMoveTask 的错误提示
        }
    }

    const Task* findTask(int64_t id) const {
        for (const auto& t : tasks_) {
            if (t.id == id) return &t;
        }
        return nullptr;
    }

    const Column* findColumn(int64_t id) const {
        for (const auto& c : columns_) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    // 键盘/菜单共用的上移下移（moveTask 的 position 语义见 todo_ui_logic.h）
    void moveTaskByKeyboard(bool up) {
        const auto mv = ui::computeKeyboardMove(tasks_, selectedTaskId_, up);
        if (!mv) {
            setStatus(up ? "已经在最上面了" : "已经在最下面了");
            return;
        }
        const std::string title =
            findTask(selectedTaskId_) != nullptr ? findTask(selectedTaskId_)->title : "";
        doMoveTask(selectedTaskId_, mv->columnId, mv->position);
        setStatus((up ? "已上移「" : "已下移「") + title + "」");
    }

    // ============ 状态提示 ============

    // 状态反馈走 toast（3.5s 超时自动消失，仿 markdown/claude 模块）。
    // 此前写进 header subtitle 永久驻留，静态操作说明被最后一条
    // 操作结果永久顶掉；现在 subtitle 始终保持静态说明。
    void setStatus(const std::string& msg, bool isError = false) {
        toastTitle_ = isError ? ("⚠ " + msg) : msg;
        toastOpen_ = true;
    }

    // ---------- Toast（画在弹窗之上，超时消失） ----------
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        components::toast(ui, "todo.toast")
            .theme(themeTokens)
            .screen(screen.width, screen.height)
            .visible(toastOpen_)
            .duration(3.5f)
            .title(toastTitle_)
            .onAutoDismiss([this] { toastOpen_ = false; })
            .onDismiss([this] { toastOpen_ = false; })
            .build();
    }

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        // 消费跨模块跳转负载（Router payload，一次性语义）
        consumeNavPayload();

        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = 64.0f;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;

        // 页面铺底色（仿 settings_module）：clearColor 只在启动时取主题快照，
        // 运行时切主题不跟随——页面必须自己铺 background，否则浅色主题下黑底
        ui.rect("todo.pagebg")
            .size(screen.width, screen.height)
            .color(t.colors.background)
            .build();

        ui.column("todo.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, headerH, bodyH, t);
                drawBoard(ui, contentW, bodyH, t);
            })
            .build();

        // 弹窗（画在最上层）
        drawNewTaskDialog(ui, screen, t);
        drawDetailDialog(ui, screen, t);
        drawContextMenu(ui, screen, t);
        drawDatePicker(ui, screen, t);
        drawRenameDialog(ui, screen, t);
        drawConfirmDeleteDialog(ui, screen, t);
        drawToast(ui, screen,
                  t.isDark ? components::theme::dark() : components::theme::light());
    }

    // P7a 键盘移动：Ctrl/Cmd+↑↓ 上移/下移当前选中任务（选中 = 最近
    // 点击过的卡片，点击详情弹窗关闭后选中态保留）。
    // 事件来源：EUI-NEO 无聚焦元素消费时经 main.cpp → Router 转发到本页。
    bool onKeyEvent(const eui::KeyEvent& event) override {
        if (event.action != eui::KeyAction::Press &&
            event.action != eui::KeyAction::Repeat) {
            return false;
        }
        if (event.key != eui::InputKey::Up && event.key != eui::InputKey::Down) {
            return false;
        }
        if (!event.modifiers.shortcut()) return false;
        if (service_ == nullptr) return false;
        if (selectedTaskId_ == 0 || findTask(selectedTaskId_) == nullptr) {
            setStatus("先点击卡片选中任务，再用 Ctrl+↑/↓ 移动", true);
            return false;
        }
        moveTaskByKeyboard(event.key == eui::InputKey::Up);
        return true;
    }

private:
    // ---------- 顶部 Header ----------
    void drawHeader(eui::Ui& ui, float contentW, float headerH, float bodyH,
                    const core::theme::Tokens& t) {
        (void)bodyH;
        ui.row("todo.header")
            .size(contentW, headerH)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                ui.column("todo.title")
                    .size(contentW - 480.0f, headerH)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("todo.h1")
                            .size(contentW - 480.0f, 32.0f)
                            .text("Todo 看板")
                            .fontSize(t.sizes.fontSizeTitle)
                            .lineHeight(32.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        // subtitle 保持静态操作说明（动态反馈走 toast）
                        ui.text("todo.subtitle")
                            .size(contentW - 480.0f, 18.0f)
                            .text("点击卡片查看详情 · Ctrl+↑/↓ 移动选中任务 · 右键卡片/列头更多操作")
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(18.0f)
                            .color(t.colors.textSecondary)
                            .build();
                    })
                    .build();

                ui.row("todo.actions")
                    .size(480.0f, 40.0f)
                    .justifyContent(eui::Align::END)
                    .alignItems(eui::Align::CENTER)
                    .gap(t.sizes.spacingSmall)
                    .content([&] {
                        // + 列
                        components::button(ui, "todo.btn.addcol")
                            .size(72.0f, 40.0f)
                            .text("+ 列")
                            .fontSize(t.sizes.fontSizeSmall)
                            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                            .textColor(t.colors.textPrimary)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .onClick([this] {
                                doCreateColumn("新列 " +
                                    std::to_string(columns_.size() + 1));
                            })
                            .build();

                        // + 新建任务
                        components::button(ui, "todo.btn.add")
                            .size(120.0f, 40.0f)
                            .text("+ 新建任务")
                            .fontSize(t.sizes.fontSizeSmall)
                            .colors(t.colors.primary, t.colors.primaryHover,
                                    t.colors.primaryPressed)
                            .textColor(eui::Color("#FFFFFF"))
                            .radius(t.sizes.radius)
                            .onClick([this] {
                                if (columns_.empty()) {
                                    setStatus("请先创建至少一列", true);
                                    return;
                                }
                                openNewTaskDialog(columns_.front().id);
                            })
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 看板列 ----------
    void drawBoard(eui::Ui& ui, float contentW, float bodyH,
                   const core::theme::Tokens& t) {
        // 错误态（服务未就绪 / 读取失败）：与空态区分，给文案 + 重试按钮
        // （仿 Home statsError_ 三态：空 / 错误+重试 / 数据）
        if (!service_ || !loadError_.empty()) {
            const std::string msg = !service_
                ? "数据库尚未就绪，数据未加载"
                : "数据加载失败：" + loadError_;
            ui.stack("todo.error")
                .size(contentW, bodyH)
                .content([&] {
                    ui.text("todo.error.text")
                        .position(0.0f, bodyH / 2.0f - 36.0f)
                        .size(contentW, 24.0f)
                        .text(msg + "（点击重试）")
                        .fontSize(t.sizes.fontSizeBody)
                        .lineHeight(24.0f)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(t.colors.error)
                        .build();
                    components::button(ui, "todo.error.retry")
                        .position(contentW / 2.0f - 60.0f, bodyH / 2.0f)
                        .size(120.0f, 40.0f)
                        .text("重试")
                        .fontSize(t.sizes.fontSizeBody - 1.0f)
                        .colors(t.colors.primary, t.colors.primaryHover,
                                t.colors.primaryPressed)
                        .textColor(eui::Color("#FFFFFF"))
                        .radius(t.sizes.radius)
                        .onClick([this] { retryLoad(); })
                        .build();
                })
                .build();
            return;
        }

        if (columns_.empty()) {
            ui.stack("todo.empty")
                .size(contentW, bodyH)
                .content([&] {
                    ui.text("todo.empty.text")
                        .position(0.0f, bodyH / 2.0f - 12.0f)
                        .size(contentW, 24.0f)
                        .text("还没有任何列，点击右上角「+ 列」开始")
                        .fontSize(t.sizes.fontSizeBody)
                        .lineHeight(24.0f)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                })
                .build();
            return;
        }

        const float colGap = t.sizes.spacingLarge;
        const int n = static_cast<int>(columns_.size());
        const float colW = (contentW - colGap * static_cast<float>(n - 1)) /
                           static_cast<float>(n);

        // 板面几何快照（W8 拖拽命中换算用；compose 每帧刷新）
        boardColW_ = colW;
        boardColGap_ = colGap;
        boardH_ = bodyH;

        ui.row("todo.body")
            .size(contentW, bodyH)
            .gap(colGap)
            .content([&] {
                for (size_t ci = 0; ci < columns_.size(); ++ci) {
                    drawColumn(ui, "todo.col." + std::to_string(ci),
                               columns_[ci], colW, bodyH, t, ci);
                }
            })
            .build();
    }

    void drawColumn(eui::Ui& ui, const std::string& id, const Column& col,
                    float width, float height, const core::theme::Tokens& t,
                    size_t colIndex) {
        // 列内容整体包一层 stack：EUI-NEO 的 position() 只在 stack 内生效，
        // 直接作为 row("todo.body") 的子元素会被流式布局横向排开（列头/卡片错位）
        ui.stack(id)
            .size(width, height)
            .content([&] {
                drawColumnContent(ui, id, col, width, height, t, colIndex);
            })
            .build();
    }

    void drawColumnContent(eui::Ui& ui, const std::string& id, const Column& col,
                           float width, float height,
                           const core::theme::Tokens& t, size_t colIndex) {
        // 拖动中指针所在列高亮（drop 目标提示）
        const bool dropHover = drag_.active &&
            drag_.hoverColIndex == static_cast<int>(colIndex);

        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(width, height)
            .color(t.colors.surface)
            .radius(t.sizes.radiusLarge)
            .border(dropHover ? 2.0f : 1.0f,
                    dropHover ? t.colors.primary : t.colors.borderDefault)
            .build();

        auto colTasks = tasksInColumn(col.id);
        const bool isArchive = (col.title == "已归档");

        // 列头右键热区（重命名/左移/右移/删除列）。纯命中区：Rect 不设
        // color 时默认不透明白色（Element::color 默认 {1,1,1,1}），深色主题
        // 下会把列头整条刷白——必须显式给全透明
        ui.rect(id + ".head")
            .position(0.0f, 0.0f)
            .size(width, kColumnHeaderHeight)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .onContextMenu([this, col, colIndex](const eui::PointerEvent&,
                                                 const eui::Rect&) {
                openColumnContextMenu(col, static_cast<int>(colIndex));
            })
            .build();

        ui.text(id + ".title")
            .position(16.0f, 16.0f)
            .size(width - 60.0f, 20.0f)
            .text(col.title + " (" + std::to_string(colTasks.size()) + ")")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        // 列头 + 按钮
        components::button(ui, id + ".add")
            .position(width - 36.0f, 14.0f)
            .size(28.0f, 28.0f)
            .text("+")
            .fontSize(t.sizes.fontSizeBody + 1.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textSecondary)
            .radius(14.0f)
            .onClick([this, col] { openNewTaskDialog(col.id); })
            .build();

        if (colTasks.empty()) {
            ui.text(id + ".empty")
                .position(0.0f, height / 2.0f - 12.0f)
                .size(width, 24.0f)
                .text(isArchive ? "📦 归档的任务在这里" : "暂无任务")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            return;
        }

        // 列内滚动（scrollView）：此前直接裁掉超出列高的卡片（第 12 张起消失）。
        // 滚动位置按列索引记忆（reloadAll 时 vector resize 保留旧值）。
        const auto componentTheme = t.isDark ? components::theme::dark()
                                             : components::theme::light();
        components::scrollView(ui, id + ".scroll")
            .position(0.0f, kColumnHeaderHeight)
            .size(width, height - kColumnHeaderHeight)
            .gap(kCardGap)
            .step(60.0f)
            .theme(componentTheme)
            .contentKey(id + ".scroll." + std::to_string(colTasks.size()))
            .offset(colScrollOffset(colIndex))
            .onChange([this, colIndex](float v) { setColScrollOffset(colIndex, v); })
            .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                (void)viewportHeight;
                for (size_t i = 0; i < colTasks.size(); ++i) {
                    const std::string cardId = id + ".card." + std::to_string(i);
                    inner.stack(cardId + ".wrap")
                        .size(contentWidth, kCardHeight)
                        .content([&] {
                            drawCard(inner, cardId, 0.0f, contentWidth - 16.0f, t,
                                     *colTasks[i], static_cast<int>(colIndex),
                                     static_cast<int>(columns_.size()),
                                     static_cast<int>(i));
                        })
                        .build();
                }
            })
            .build();
    }

    void drawCard(eui::Ui& ui, const std::string& id, float y, float width,
                  const core::theme::Tokens& t, const Task& task,
                  int colIndex, int colCount, int cardIndex) {
        (void)colCount;
        const bool done = task.completedAt.has_value();
        const bool selected = (task.id == selectedTaskId_);
        // W8 拖拽中间态：被拖卡片半透明（拖动只是记录 + 视觉反馈，松手才落库）
        const bool dragging = drag_.active && drag_.taskId == task.id;
        const float cardOpacity = dragging ? 0.45f : 1.0f;
        boardCardW_ = width;  // compose 空间卡宽（拖拽命中 dpi 探针）

        ui.rect(id + ".bg")
            .position(0.0f, y)
            .size(width, kCardHeight)
            .states(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .radius(t.sizes.radius)
            .opacity(cardOpacity)
            .border(selected ? 2.0f : 0.0f,
                    selected ? t.colors.primary : t.colors.borderDefault)
            .onClick([this, task] {
                // 拖拽松手回落在本卡上时不当作点击（onClick 先于 onRelease 派发）
                if (drag_.active) return;
                openDetailDialog(task);
            })
            .onContextMenu([this, task, colIndex, cardIndex](const eui::PointerEvent&,
                                                             const eui::Rect&) {
                openTaskContextMenu(task, colIndex, cardIndex);
            })
            // ---- 真 drop 语义（W8）：拖动中无落库，松手按指针落点决定 ----
            .onPress([this, task, colIndex, cardIndex](const eui::PointerEvent&,
                                                       const eui::Rect& bounds) {
                // 记录拖拽锚点（卡片 bounds 为指针像素空间，含列内滚动偏移）
                drag_ = DragState{};
                drag_.taskId = task.id;
                drag_.fromColIndex = colIndex;
                drag_.cardIndex = cardIndex;
                drag_.pressX = bounds.x;
                drag_.pressY = bounds.y;
                drag_.pressWidth = bounds.width;
            })
            .onDrag([this](const core::dsl::DragEvent& ev) {
                // 运行时只在超过 dragThreshold 后派发 → 到这里即视为拖拽激活
                if (drag_.taskId == 0) return;
                drag_.active = true;
                drag_.hoverColIndex = hitDropColumn(ev.x, ev.y);
            })
            .onRelease([this](const eui::PointerEvent& ev, const eui::Rect&) {
                if (!drag_.active) {
                    drag_ = DragState{};  // 普通点击：只清锚点（onClick 已处理）
                    return;
                }
                // Cancel 或落点无效 → 取消，卡片回原位
                const int target = ev.action == eui::PointerAction::Release
                                       ? hitDropColumn(ev.x, ev.y)
                                       : -1;
                finishDrag(target);
            })
            .build();

        // 优先级左边框
        ui.rect(id + ".bar")
            .position(0.0f, y)
            .size(4.0f, kCardHeight)
            .color(priorityColor(task.priority, t))
            .radius(2.0f)
            .opacity(cardOpacity)
            .build();

        // 标题
        ui.text(id + ".title")
            .position(16.0f, y + 14.0f)
            .size(width - 44.0f, 18.0f)
            .text(task.title)
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .lineHeight(18.0f)
            .fontWeight(500)
            .color(t.colors.textPrimary)
            .opacity(done ? 0.6f : cardOpacity)
            .build();

        // 优先级 chip
        drawChip(ui, id + ".prio", 16.0f, y + 44.0f, 48.0f, t,
                 priorityLabel(task.priority), priorityColor(task.priority, t),
                 t.colors.surface, cardOpacity);

        // 截止 chip
        if (task.dueAt.has_value()) {
            const std::string dueText = "⏰ " + formatDue(task.dueAt.value());
            drawChip(ui, id + ".due", 70.0f, y + 44.0f, 66.0f, t, dueText,
                     isOverdue(task) ? t.colors.error : t.colors.textTertiary,
                     t.colors.surface, cardOpacity);
        }

        // 完成打勾
        if (done) {
            ui.text(id + ".done")
                .position(width - 36.0f, y + 14.0f)
                .size(24.0f, 18.0f)
                .text("✓")
                .fontSize(t.sizes.fontSizeBody - 1.0f)
                .lineHeight(18.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.success)
                .fontWeight(700)
                .opacity(cardOpacity)
                .build();
        }
    }

    // ---------- 新建任务弹窗 ----------

    // 消费挂在 Todo 页上的跳转负载（每帧调用；consumePendingPayload
    // 取出即清空，compose 逐帧重复调用不会重复触发）：
    //   "action:new_task" → 打开新建任务弹窗（等价页内「+ 新建任务」）
    void consumeNavPayload() {
        auto* router = ::core::appRouter();
        if (router == nullptr) return;
        const std::string payload = router->consumePendingPayload(pageId());
        if (payload.empty()) return;

        if (payload == "action:new_task") {
            if (columns_.empty()) {
                setStatus("请先创建至少一列", true);
                return;
            }
            openNewTaskDialog(columns_.front().id);
            LOG_INFO("跨模块负载：打开新建任务弹窗");
            return;
        }
        LOG_WARN("Todo: 未识别的跳转负载: " + payload);
    }

    void openNewTaskDialog(int64_t columnId) {
        newTaskColumnId_ = columnId;
        newTaskTitle_.clear();
        newTaskOpen_ = true;
    }

    void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t) {
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
        const float w = 460.0f;
        const float h = 230.0f;

        components::dialog(ui, "todo.dialog.newtask")
            .open(newTaskOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { newTaskOpen_ = v; })
            .content([&] {
                ui.text("todo.dialog.newtask.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("新建任务")
                    .fontSize(t.sizes.fontSizeLarge + 2.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // 目标列提示
                std::string colName = "?";
                for (const auto& c : columns_) {
                    if (c.id == newTaskColumnId_) colName = c.title;
                }
                ui.text("todo.dialog.newtask.col")
                    .position(28.0f, 56.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("将创建在「" + colName + "」列")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                components::input(ui, "todo.dialog.newtask.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newTaskTitle_)
                    .placeholder("任务标题…")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { newTaskTitle_ = v; })
                    .onEnter([this] {
                        const int64_t col = newTaskColumnId_;
                        const std::string title = newTaskTitle_;
                        newTaskOpen_ = false;
                        doCreateTask(col, title);
                    })
                    .build();

                components::button(ui, "todo.dialog.newtask.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { newTaskOpen_ = false; })
                    .build();

                components::button(ui, "todo.dialog.newtask.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("创建")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] {
                        const int64_t col = newTaskColumnId_;
                        const std::string title = newTaskTitle_;
                        newTaskOpen_ = false;
                        doCreateTask(col, title);
                    })
                    .build();
            })
            .build();
    }

    // ---------- 右键菜单 ----------
    // 菜单定位走模块自身布局空间（boardGeom），不用窗口坐标（见 BoardGeom 注释）
    void openTaskContextMenu(const Task& task, int columnIndex, int cardIndex) {
        selectedTaskId_ = task.id;
        ctxTaskId_ = task.id;
        ctxColumnId_ = 0;
        ctxColumnIndex_ = columnIndex;
        ctxCardIndex_ = cardIndex;
        ctxOpen_ = true;
    }

    void openColumnContextMenu(const Column& col, int columnIndex) {
        ctxTaskId_ = 0;
        ctxColumnId_ = col.id;
        ctxColumnIndex_ = columnIndex;
        ctxCardIndex_ = -1;
        ctxOpen_ = true;
    }

    void drawContextMenu(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t) {
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
        std::vector<components::ContextMenuItem> items;
        if (ctxTaskId_ != 0) {
            const Task* self = findTask(ctxTaskId_);
            if (self == nullptr) return;
            items.push_back({"编辑"});
            items.push_back({"上移"});
            items.push_back({"下移"});
            for (const auto& c : columns_) {
                if (c.id != self->columnId) {
                    items.push_back({"移动到「" + c.title + "」"});
                }
            }
            items.push_back({"删除"});
        } else if (ctxColumnId_ != 0) {
            if (findColumn(ctxColumnId_) == nullptr) return;
            items.push_back({"重命名列"});
            items.push_back({"左移列"});
            items.push_back({"右移列"});
            items.push_back({"删除列"});
        } else {
            return;
        }

        const BoardGeom g = boardGeom(screen, columns_.size(), t);
        const float menuW = 190.0f;
        const float x = cardLeft(g, ctxColumnIndex_) + g.colW * 0.5f - menuW * 0.5f;
        const float y = ctxCardIndex_ >= 0
            ? cardTop(g, ctxCardIndex_) + kCardHeight + 4.0f
            : g.boardTop + 4.0f;

        components::contextMenu(ui, "todo.ctx")
            .open(ctxOpen_)
            .screen(screen.width, screen.height)
            .position(x, y)
            .size(menuW, 36.0f)
            .items(std::move(items))
            .theme(themeTokens)
            .onSelect([this](int index) { onContextSelect(index); })
            .onOpenChange([this](bool v) { ctxOpen_ = v; })
            .build();
    }

    // 菜单项点击分发。索引含义与 drawContextMenu 构建的 items 一一对应：
    //   任务菜单：0=编辑 1=上移 2=下移 3+i=移动到第 i 个其他列 末项=删除（带确认）
    //   列菜单：  0=重命名 1=左移 2=右移 3=删除
    void onContextSelect(int index) {
        if (ctxTaskId_ != 0) {
            const Task* self = findTask(ctxTaskId_);
            if (self == nullptr) return;
            if (index == 0) {
                openDetailDialog(*self);
                return;
            }
            if (index == 1) { moveTaskByKeyboard(true); return; }
            if (index == 2) { moveTaskByKeyboard(false); return; }
            size_t otherColumns = 0;
            for (const auto& c : columns_) {
                if (c.id != self->columnId) ++otherColumns;
            }
            const int deleteIndex = 3 + static_cast<int>(otherColumns);
            if (index == deleteIndex) {
                confirmTaskId_ = ctxTaskId_;
                confirmTaskTitle_ = self->title;
                confirmColumnId_ = 0;
                confirmOpen_ = true;
                return;
            }
            int k = index - 3;
            for (const auto& c : columns_) {
                if (c.id == self->columnId) continue;
                if (k == 0) {
                    const std::string title = c.title;
                    doMoveTask(ctxTaskId_, c.id,
                               ui::computeAppendPosition(tasks_, c.id));
                    setStatus("已移动到「" + title + "」");
                    return;
                }
                --k;
            }
            return;
        }
        const Column* col = findColumn(ctxColumnId_);
        if (col == nullptr) return;
        switch (index) {
            case 0:
                renameColumnId_ = col->id;
                renameTitle_ = col->title;
                renameOpen_ = true;
                break;
            case 1: doShiftColumn(col->id, -1); break;
            case 2: doShiftColumn(col->id, +1); break;
            case 3:
                confirmColumnId_ = col->id;
                confirmTaskId_ = 0;
                confirmOpen_ = true;
                break;
            default: break;
        }
    }

    // ---------- 重命名列弹窗 ----------
    void drawRenameDialog(eui::Ui& ui, const eui::Screen& screen,
                          const core::theme::Tokens& t) {
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
        const float w = 460.0f;
        const float h = 210.0f;

        components::dialog(ui, "todo.dialog.rename")
            .open(renameOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { renameOpen_ = v; })
            .content([&] {
                ui.text("todo.dialog.rename.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("重命名列")
                    .fontSize(t.sizes.fontSizeLarge + 2.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                components::input(ui, "todo.dialog.rename.input")
                    .position(28.0f, 68.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(renameTitle_)
                    .placeholder("列标题…")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { renameTitle_ = v; })
                    .onEnter([this] {
                        const int64_t id = renameColumnId_;
                        const std::string title = renameTitle_;
                        renameOpen_ = false;
                        doRenameColumn(id, title);
                    })
                    .build();

                components::button(ui, "todo.dialog.rename.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { renameOpen_ = false; })
                    .build();

                components::button(ui, "todo.dialog.rename.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("保存")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] {
                        const int64_t id = renameColumnId_;
                        const std::string title = renameTitle_;
                        renameOpen_ = false;
                        doRenameColumn(id, title);
                    })
                    .build();
            })
            .build();
    }

    // ---------- 删除确认弹窗 ----------
    // 双模式：confirmTaskId_ > 0 = 删任务；否则 confirmColumnId_ > 0 = 删列
    // （外键 CASCADE 会连带删掉列下全部任务）
    void drawConfirmDeleteDialog(eui::Ui& ui, const eui::Screen& screen,
                                 const core::theme::Tokens& t) {
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
        const float w = 460.0f;
        const float h = 200.0f;
        const bool delTask = (confirmTaskId_ != 0);
        std::string colName = "?";
        int taskCount = 0;
        if (!delTask) {
            if (const Column* col = findColumn(confirmColumnId_)) {
                colName = col->title;
                taskCount = static_cast<int>(tasksInColumn(col->id).size());
            }
        }
        const std::string title = delTask ? "删除任务" : "删除列";
        const std::string body = delTask
            ? "将删除任务「" + confirmTaskTitle_ + "」，不可恢复。"
            : "将删除「" + colName + "」列及其中 " +
                  std::to_string(taskCount) + " 个任务，不可恢复。";

        components::dialog(ui, "todo.dialog.delcol")
            .open(confirmOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { confirmOpen_ = v; })
            .content([&] {
                ui.text("todo.dialog.delcol.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text(title)
                    .fontSize(t.sizes.fontSizeLarge + 2.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("todo.dialog.delcol.body")
                    .position(28.0f, 60.0f)
                    .size(w - 56.0f, 40.0f)
                    .text(body)
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();

                components::button(ui, "todo.dialog.delcol.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { confirmOpen_ = false; })
                    .build();

                components::button(ui, "todo.dialog.delcol.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("删除")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.error, t.colors.error, t.colors.error)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] {
                        if (confirmTaskId_ != 0) {
                            const int64_t taskId = confirmTaskId_;
                            confirmOpen_ = false;
                            confirmTaskId_ = 0;
                            doDeleteTask(taskId);
                            return;
                        }
                        const int64_t id = confirmColumnId_;
                        confirmOpen_ = false;
                        doDeleteColumn(id);
                    })
                    .build();
            })
            .build();
    }

    // ---------- 截止日期选择（EUI-NEO datepicker 滚轮，替代 4 个固定预设） ----------
    void drawDatePicker(eui::Ui& ui, const eui::Screen& screen,
                        const core::theme::Tokens& t) {
        if (!datePickOpen_) return;
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
        int y = 0, m = 0, d = 0;
        if (!ui::epochToYmd(detailDueAt_.value_or(0), y, m, d)) {
            ui::epochToYmd(static_cast<int64_t>(std::time(nullptr)), y, m, d);
        }
        components::datePicker(ui, "todo.datepicker")
            .open(true)
            .screen(screen.width, screen.height)
            .size(380.0f, 260.0f)
            .date(y, m, d)
            .theme(themeTokens)
            .zIndex(1100)  // 盖在详情弹窗（zIndex 1000）之上
            .onChange([this](int yy, int mm, int dd) {
                detailDueAt_ = ui::ymdToEpoch(yy, mm, dd);
            })
            .onOpenChange([this](bool v) { datePickOpen_ = v; })
            .build();
    }

    // ---------- 任务详情 / 编辑弹窗 ----------
    void openDetailDialog(const Task& task) {
        selectedTaskId_ = task.id;  // 选中态：详情弹窗关闭后保留，供 Ctrl+↑/↓
        detailTaskId_ = task.id;
        detailColumnId_ = task.columnId;
        detailTitle_ = task.title;
        detailDescription_ = task.description.value_or("");
        detailPriority_ = task.priority;
        detailDueAt_ = task.dueAt;
        detailCompleted_ = task.completedAt.has_value();
        detailTags_ = task.tags;
        detailOpen_ = true;
    }

    void drawDetailDialog(eui::Ui& ui, const eui::Screen& screen,
                          const core::theme::Tokens& t) {
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
        const float w = 560.0f;
        // 「移动到」全列遍历 + 可换行：每行 4 个按钮，列多时增高弹窗
        // （此前硬编码 i < 4，第 5 列起在详情弹窗里移不过去）
        const float moveBtnW = 118.0f;
        const float moveGap = 8.0f;
        const int perRow = std::max(
            1, static_cast<int>((w - 56.0f + moveGap) / (moveBtnW + moveGap)));
        const int moveRows = static_cast<int>(
            (columns_.size() + static_cast<size_t>(perRow) - 1) / perRow);
        const float h = 560.0f + 40.0f * static_cast<float>(std::max(0, moveRows - 1));

        components::dialog(ui, "todo.dialog.detail")
            .open(detailOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { detailOpen_ = v; })
            .content([&] {
                ui.text("todo.dialog.detail.title")
                    .position(28.0f, 22.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("任务详情")
                    .fontSize(t.sizes.fontSizeLarge + 2.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // 标题
                ui.text("todo.dialog.detail.tlabel")
                    .position(28.0f, 58.0f)
                    .size(80.0f, 16.0f)
                    .text("标题")
                    .fontSize(t.sizes.fontSizeSmall - 2.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "todo.dialog.detail.input.title")
                    .position(28.0f, 76.0f)
                    .size(w - 56.0f, 40.0f)
                    .value(detailTitle_)
                    .placeholder("任务标题…")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { detailTitle_ = v; })
                    .build();

                // 描述
                ui.text("todo.dialog.detail.dlabel")
                    .position(28.0f, 126.0f)
                    .size(80.0f, 16.0f)
                    .text("描述")
                    .fontSize(t.sizes.fontSizeSmall - 2.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "todo.dialog.detail.input.desc")
                    .position(28.0f, 144.0f)
                    .size(w - 56.0f, 88.0f)
                    .value(detailDescription_)
                    .placeholder("补充说明…（可多行）")
                    .fontSize(t.sizes.fontSizeSmall)
                    .multiline(true)
                    .scrollbar(true)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { detailDescription_ = v; })
                    .build();

                // 优先级
                ui.text("todo.dialog.detail.plabel")
                    .position(28.0f, 244.0f)
                    .size(80.0f, 16.0f)
                    .text("优先级")
                    .fontSize(t.sizes.fontSizeSmall - 2.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                const Priority prios[4] = {Priority::None, Priority::Low,
                                           Priority::Medium, Priority::High};
                for (int i = 0; i < 4; ++i) {
                    const bool active = (detailPriority_ == prios[i]);
                    const float bw = 100.0f;
                    components::button(ui, "todo.dialog.detail.prio." + std::to_string(i))
                        .position(28.0f + static_cast<float>(i) * (bw + 8.0f), 264.0f)
                        .size(bw, 32.0f)
                        .text(priorityLabel(prios[i]))
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .colors(active ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                                t.colors.hover, t.colors.active)
                        .textColor(active ? t.colors.primary : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i, prios] { detailPriority_ = prios[i]; })
                        .build();
                }

                // 截止日期：清除 + datepicker 滚轮选任意日期（原 4 个固定预设已替换）
                ui.text("todo.dialog.detail.dlabel2")
                    .position(28.0f, 308.0f)
                    .size(120.0f, 16.0f)
                    .text("截止日期")
                    .fontSize(t.sizes.fontSizeSmall - 2.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                const bool hasDue = detailDueAt_.has_value();
                components::button(ui, "todo.dialog.detail.due.clear")
                    .position(28.0f, 328.0f)
                    .size(100.0f, 32.0f)
                    .text(!hasDue ? "未设置" : "清除")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(!hasDue ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                            t.colors.hover, t.colors.active)
                    .textColor(!hasDue ? t.colors.primary : t.colors.textSecondary)
                    .radius(t.sizes.radius)
                    .border(1.0f, !hasDue ? t.colors.primary : t.colors.borderDefault)
                    .onClick([this] { detailDueAt_ = std::nullopt; })
                    .build();
                const std::string dueBtnText =
                    hasDue ? "📅 " + formatDue(detailDueAt_.value()) : "📅 选择日期…";
                components::button(ui, "todo.dialog.detail.due.pick")
                    .position(136.0f, 328.0f)
                    .size(180.0f, 32.0f)
                    .text(dueBtnText)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(hasDue ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                            t.colors.hover, t.colors.active)
                    .textColor(hasDue ? t.colors.primary : t.colors.textSecondary)
                    .radius(t.sizes.radius)
                    .border(1.0f, hasDue ? t.colors.primary : t.colors.borderDefault)
                    .onClick([this] { datePickOpen_ = true; })
                    .build();

                // 移动到其他列
                ui.text("todo.dialog.detail.mlabel")
                    .position(28.0f, 372.0f)
                    .size(120.0f, 16.0f)
                    .text("移动到")
                    .fontSize(t.sizes.fontSizeSmall - 2.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                for (size_t i = 0; i < columns_.size(); ++i) {
                    const bool active = (columns_[i].id == detailColumnId_);
                    const int slot = static_cast<int>(i);
                    const float bx = 28.0f +
                        static_cast<float>(slot % perRow) * (moveBtnW + moveGap);
                    const float by = 392.0f +
                        static_cast<float>(slot / perRow) * 40.0f;
                    components::button(ui, "todo.dialog.detail.move." + std::to_string(i))
                        .position(bx, by)
                        .size(moveBtnW, 32.0f)
                        .text(columns_[i].title)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .colors(active ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                                t.colors.hover, t.colors.active)
                        .textColor(active ? t.colors.primary : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i] {
                            const int64_t taskId = detailTaskId_;
                            const int64_t toCol = columns_[i].id;
                            if (toCol == detailColumnId_) return;  // 已在该列
                            detailColumnId_ = toCol;
                            detailOpen_ = false;
                            doMoveTask(taskId, toCol,
                                       ui::computeAppendPosition(tasks_, toCol));
                        })
                        .build();
                }

                // 底部操作区
                const float btnY = h - 58.0f;
                components::button(ui, "todo.dialog.detail.delete")
                    .position(28.0f, btnY)
                    .size(96.0f, 40.0f)
                    .text("删除")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.error)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    // 与右键菜单删除同路径：先弹确认（confirmOpen_），不再直接落库
                    .onClick([this] {
                        confirmTaskId_ = detailTaskId_;
                        confirmTaskTitle_ = detailTitle_;
                        confirmColumnId_ = 0;
                        detailOpen_ = false;
                        confirmOpen_ = true;
                    })
                    .build();

                components::button(ui, "todo.dialog.detail.toggle")
                    .position(132.0f, btnY)
                    .size(132.0f, 40.0f)
                    .text(detailCompleted_ ? "↩ 取消完成" : "✓ 标记完成")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(detailCompleted_ ? t.colors.textSecondary : t.colors.success)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        const int64_t taskId = detailTaskId_;
                        const bool wasDone = detailCompleted_;
                        detailOpen_ = false;
                        if (wasDone) doUncompleteTask(taskId);
                        else doCompleteTask(taskId);
                    })
                    .build();

                components::button(ui, "todo.dialog.detail.save")
                    .position(w - 28.0f - 160.0f, btnY)
                    .size(160.0f, 40.0f)
                    .text("保存")
                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                    .colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] {
                        Task updated;
                        updated.id = detailTaskId_;
                        updated.columnId = detailColumnId_;
                        updated.title = detailTitle_;
                        if (!detailDescription_.empty()) {
                            updated.description = detailDescription_;
                        }
                        updated.priority = detailPriority_;
                        updated.dueAt = detailDueAt_;
                        updated.completedAt = detailCompleted_
                            ? std::optional<int64_t>(static_cast<int64_t>(std::time(nullptr)))
                            : std::nullopt;
                        updated.tags = detailTags_;
                        // 保留原 position（编辑不改序；此前硬编码 0 会把任务顶到列首）
                        if (const Task* orig = findTask(detailTaskId_)) {
                            updated.position = orig->position;
                        } else {
                            updated.position = 0;
                        }
                        detailOpen_ = false;
                        doUpdateTask(updated);
                    })
                    .build();
            })
            .build();
    }

    // ---------- 状态 ----------
    std::unique_ptr<TodoService> service_;
    std::vector<Column> columns_;
    std::vector<Task> tasks_;
    std::vector<float> columnScrolls_;  // 列内滚动偏移（按列索引）

    // W8 拖拽（真 drop 语义）：onPress 记锚点 → onDrag 更新悬停列（视觉反馈）
    // → onRelease 按指针落点落库。dragging 半透明 + 目标列高亮见 drawCard/drawColumn
    struct DragState {
        bool active = false;       // 超过 dragThreshold 才置位
        int64_t taskId = 0;        // 稳定任务 id（不用会过期的列/卡索引做落库依据）
        int fromColIndex = -1;
        int cardIndex = -1;
        float pressX = 0.0f;       // 按下时卡片 bounds（指针像素空间）
        float pressY = 0.0f;
        float pressWidth = 0.0f;
        int hoverColIndex = -1;    // 拖动中指针所在列（-1 = 列外，松手取消）
    };
    DragState drag_;

    // 板面几何快照（drawBoard/drawCard 每帧刷新，拖拽命中换算用）
    float boardColW_ = 0.0f;
    float boardColGap_ = 0.0f;
    float boardH_ = 0.0f;
    float boardCardW_ = 0.0f;

    // 新建任务弹窗
    bool newTaskOpen_ = false;
    int64_t newTaskColumnId_ = 0;
    std::string newTaskTitle_;

    // 详情弹窗
    bool detailOpen_ = false;
    int64_t detailTaskId_ = 0;
    int64_t detailColumnId_ = 0;
    std::string detailTitle_;
    std::string detailDescription_;
    Priority detailPriority_ = Priority::None;
    std::optional<int64_t> detailDueAt_;
    bool detailCompleted_ = false;
    std::vector<std::string> detailTags_;

    // 键盘移动选中态（最近点击的卡片；详情弹窗关闭后保留）
    int64_t selectedTaskId_ = 0;

    // 右键菜单（ctxTaskId_ > 0 = 任务菜单；否则 ctxColumnId_ > 0 = 列菜单）
    bool ctxOpen_ = false;
    int64_t ctxTaskId_ = 0;
    int64_t ctxColumnId_ = 0;
    int ctxColumnIndex_ = 0;
    int ctxCardIndex_ = -1;

    // 截止日期 datepicker
    bool datePickOpen_ = false;

    // 重命名列 / 删除确认弹窗（确认弹窗双模式：任务 / 列）
    bool renameOpen_ = false;
    int64_t renameColumnId_ = 0;
    std::string renameTitle_;
    bool confirmOpen_ = false;
    int64_t confirmColumnId_ = 0;
    int64_t confirmTaskId_ = 0;
    std::string confirmTaskTitle_;

    // 状态反馈 toast（超时消失；subtitle 保持静态说明）
    bool toastOpen_ = false;
    std::string toastTitle_;

    // 数据加载错误（空 = 正常；与空态区分，见 drawBoard）
    std::string loadError_;
};

} // namespace module::todo

extern "C" ::core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}