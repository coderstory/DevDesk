// Todo 看板模块：Module 接口 + compose 骨架 + 拖拽状态机 + 数据 reload + 写操作 + 右键菜单分发。
// 看板绘制在 todo_ui_board.{h,cpp}，弹窗状态在 todo_dialogs.h，纯函数在 todo_ui_logic.h。
// 两条通道（BoardActions / DialogHost）构造期绑定一次，回调体读模块当下状态，UI 单元不持有本类
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/component_theme.h"
#include "core/event_bus.h"
#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>
#include "todo_dialogs.h"
#include "todo_service.h"
#include "todo_tools.h"
#include "todo_ui_board.h"
#include "todo_ui_logic.h"
#include "todo_view_state.h"

#include <eui/app.h>
#include <eui/dsl.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace module::todo {

class TodoModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Todo; }
    std::string displayName() const override { return "Todo 看板"; }
    std::string shortName() const override { return "待办"; }
    std::string iconGlyph() const override { return eui::utf8(0xF058); }  // fa-circle-check

    void onStart() override {
        // 重入幂等：double-onStart = no-op（不重跑 attachTools/reloadAll/视图恢复）
        if (started_) return;
        started_ = true;
        // GitHub Sync 拉取会 DROP 重建整个库：订阅导入成功事件，导入线程同步回调里
        // 只置原子脏标记（不得摸 UI 线程成员），compose 收割 reloadAll。
        // 闸 = 存活守卫：onStop 先关闸再退订，挡住在途 handler。
        dbImportHandlersEnabled_.store(true, std::memory_order_release);
        dbImportedSub_ = ::core::event::bus().subscribe(
            ::core::event::kDbImported, [this](const std::string&) {
                if (dbImportHandlersEnabled_.load(std::memory_order_acquire)) {
                    dbImportedDirty_.store(true, std::memory_order_release);
                }
            });
        auto* db = ::core::appDatabase();
        if (!db) {
            LOG_WARN("Todo: 数据库尚未注册，跳过数据加载");
            return;
        }
        // 服务复用而非重建（UAF 收口）：off→on 窗口重建会析构 agent run 线程正持有的旧 service；
        // 前提：db 进程级单例、运行期不换
        if (!service_) {
            service_ = std::make_unique<TodoService>(*db);
        }
        attachTools();   // off 后 todoTools_ 已被 detachTools 置空，此处正常重挂
        reloadAll();
        // 恢复列滚动 + 选中任务（须在 reloadAll 之后）
        viewStore_ = std::make_unique<TodoViewStateStore>(::core::appDatabase());
        restoreViewState();
        LOG_INFO("Todo 模块启动（" + std::to_string(columns_.size()) + " 列 / " +
                 std::to_string(tasks_.size()) + " 任务）");
    }

    void onStop() override {
        // 重入幂等：未启动的 off = no-op（此路径不该落盘/注销）
        if (!started_) return;
        // 先关原子闸再退订：闸挡在途 handler，退订挡后续 emit。
        dbImportHandlersEnabled_.store(false, std::memory_order_release);
        dbImportedSub_ = {};
        if (viewDirty_) saveViewState();
        // 只注销工具，不 reset service_：agent run 线程可能正执行 todo.* 工具（持 TodoService&），reset = UAF。
        // service_ 交给成员析构（声明序 todoTools_ 在前 → 先析构，引用不悬垂）
        detachTools();
        started_ = false;
        LOG_INFO("Todo 模块停止");
    }

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
        // 滚动按列 id 记忆；剪掉已不存在列的记录防膨胀（prune 有删除 → 状态待落盘）
        std::vector<int64_t> liveIds;
        liveIds.reserve(columns_.size());
        for (const auto& c : columns_) liveIds.push_back(c.id);
        if (TodoViewStateStore::pruneScrolls(columnScrolls_, liveIds) > 0) {
            viewDirty_ = true;
        }
        // 拖拽中任务被外部删除 → 锚点失效，复位防悬垂
        if (drag_.active && ui::findTask(tasks_, drag_.taskId) == nullptr) {
            drag_ = DragState{};
        }
        refreshOverdueCount();
        if (!searchQuery_.empty()) refreshSearch();
    }

    void refreshSearch() {
        searchMatchIds_.clear();
        if (searchQuery_.empty() || !service_) return;
        auto r = service_->search(searchQuery_);
        if (r.isFail()) {
            setStatus("搜索失败：" + r.errorMessage(), true);
            return;
        }
        // 命中表用 unordered_set（filterBySearch O(1) 查）
        for (const auto& t : r.value()) {
            searchMatchIds_.insert(t.id);
        }
    }

    void refreshOverdueCount() {
        if (!service_) return;
        auto r = service_->listOverdue();
        if (r.isFail()) {
            // 读取失败不降级成「0 条」：badge 保留上次值并把失败报出去
            LOG_ERROR("Todo: 逾期计数查询失败（badge 保留上次值）: " + r.errorMessage());
            setStatus("逾期任务查询失败：" + r.errorMessage(), true);
            return;
        }
        overdueCount_ = r.value().size();
    }

    void openOverdueDialog() {
        if (!service_) {
            setStatus("服务未就绪（TodoService 未注册）", true);
            return;
        }
        auto r = service_->listOverdue();
        if (r.isFail()) {
            // 查询失败不当空结果开弹窗（会让人以为数据丢了）：报错 + 不开，弹窗保持上次内容
            LOG_ERROR("Todo: 逾期弹窗查询失败: " + r.errorMessage());
            setStatus("逾期任务加载失败：" + r.errorMessage(), true);
            return;
        }
        dialogs_.overdue.tasks = std::move(r).value();
        overdueCount_ = dialogs_.overdue.tasks.size();
        dialogs_.popups.open(ui::Popup::Overdue);
    }

    // 错误态重试：数据库晚注册或读取失败后手动恢复
    void retryLoad() {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            setStatus("数据库尚未注册，无法重试", true);
            return;
        }
        if (!service_) {
            service_ = std::make_unique<TodoService>(*db);
            attachTools();
        }
        reloadAll();
        if (loadError_.empty()) {
            setStatus("已重新加载");
        } else {
            setStatus("重试仍失败：" + loadError_, true);
        }
    }

    // doCreate*/doRename* 返回错误信息（空 = 成功）：失败由弹窗写进内嵌红字 formError（弹窗不关）；成功走 toast

    std::string doCreateTask(int64_t columnId, const std::string& title) {
        if (!service_) return "服务未就绪（TodoService 未注册）";
        if (title.empty()) return "任务标题不能为空";
        auto r = service_->createTask(columnId, title);
        if (r.isFail()) {
            LOG_ERROR("Todo: createTask 失败: " + r.errorMessage());
            return "新建任务失败：" + r.errorMessage();
        }
        setStatus("已创建任务「" + title + "」");
        core::event::bus().emit(core::event::kTodoCreated);
        reloadAll();
        return "";
    }

    std::string doCreateColumn(const std::string& title) {
        if (!service_) return "服务未就绪（TodoService 未注册）";
        auto r = service_->createColumn(title);
        if (r.isFail()) {
            return "新建列失败：" + r.errorMessage();
        }
        setStatus("已创建列「" + title + "」");
        reloadAll();
        core::event::bus().emit(core::event::kTodoColumnChanged);
        return "";
    }

    // 返回是否成功（拖拽收尾据此给提示；其余调用方忽略）
    bool doMoveTask(int64_t taskId, int64_t toColumnId, int newPosition) {
        if (!service_) return false;
        auto r = service_->moveTask(taskId, toColumnId, newPosition);
        if (r.isFail()) {
            setStatus("移动失败：" + r.errorMessage(), true);
            return false;
        }
        reloadAll();
        core::event::bus().emit(core::event::kTodoMoved);
        return true;
    }

    void doCompleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->completeTask(taskId);
        if (r.isFail()) {
            setStatus("标记完成失败：" + r.errorMessage(), true);
            return;
        }
        // 完成后移到已完成列（kind='done'），追加列尾
        core::event::bus().emit(core::event::kTodoCompleted);
        finishCompletionMove(taskId, "done");
    }

    void doUncompleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->uncompleteTask(taskId);
        if (r.isFail()) {
            setStatus("取消完成失败：" + r.errorMessage(), true);
            return;
        }
        core::event::bus().emit(core::event::kTodoUncompleted);
        finishCompletionMove(taskId, "todo");
    }

    // 状态迁移 + 列迁移合起来才算一次操作：列迁移失败不得再提示成功；目标列按 kind 定位（标题/下标在重命名重排后不可靠）
    void finishCompletionMove(int64_t taskId, const std::string& kind) {
        const bool toDone = (kind == "done");
        const std::string successMsg = toDone ? "任务已完成" : "已取消完成";
        const int64_t targetCol = ui::findColumnByKind(columns_, kind);
        if (targetCol == 0) {
            // 明示而非静默：没有语义列时任务留在原列
            setStatus(successMsg + "（未找到 kind='" + kind + "' 的列，任务留在原列）");
            reloadAll();
            return;
        }
        auto mv = service_->moveTask(
            taskId, targetCol, ui::computeAppendPosition(tasks_, targetCol));
        if (mv.isFail()) {
            LOG_ERROR("Todo: 完成态流转的列迁移失败（kind=" + kind +
                      "）: " + mv.errorMessage());
            setStatus((toDone ? "任务已标记完成，但移入已完成列失败："
                              : "已取消完成，但移回待办列失败：") +
                          mv.errorMessage(),
                      true);
            reloadAll();
            return;
        }
        // toast 图标已带 fa-check，正文不再混排 ✓
        setStatus(successMsg);
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
        core::event::bus().emit(core::event::kTodoDeleted);
    }

    // 返回是否保存成功（详情弹窗先落表单再执行动作，失败要中止动作并保留弹窗）
    bool doUpdateTask(const Task& task) {
        if (!service_) return false;
        auto r = service_->updateTask(task);
        if (r.isFail()) {
            setStatus("保存失败：" + r.errorMessage(), true);
            return false;
        }
        setStatus("已保存");
        reloadAll();
        core::event::bus().emit(core::event::kTodoUpdated);
        return true;
    }

    std::string doRenameColumn(int64_t id, const std::string& title) {
        if (!service_) return "服务未就绪（TodoService 未注册）";
        if (title.empty()) return "列标题不能为空";
        if (ui::columnTitleTaken(columns_, title, id)) return "该列名已存在，请换一个";
        auto r = service_->renameColumn(id, title);
        if (r.isFail()) {
            return "重命名失败：" + r.errorMessage();
        }
        setStatus("已重命名列");
        reloadAll();
        core::event::bus().emit(core::event::kTodoColumnChanged);
        return "";
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
        core::event::bus().emit(core::event::kTodoColumnChanged);
    }

    void doSetColumnColor(int64_t id, const std::string& token) {
        if (!service_) return;
        auto r = service_->setColumnColor(id, token);
        if (r.isFail()) {
            setStatus("改色失败：" + r.errorMessage(), true);
            return;
        }
        setStatus("已更新列颜色");
        reloadAll();
        core::event::bus().emit(core::event::kTodoColumnChanged);
    }

    // dir = -1 左移 / +1 右移（交换相邻列后整体重排 position）
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
        core::event::bus().emit(core::event::kTodoColumnChanged);
    }

    // 智能体工具集：service 与 tools 生命周期成对（service_ 存在 ⇒ todoTools_ 存在且已注册）；
    // retryLoad 晚建 service 时同样要补注册，故收成两个小口
    void attachTools() {
        if (!service_ || todoTools_) return;
        todoTools_ = std::make_unique<TodoTools>(*service_);
        todoTools_->registerAll();
    }

    void detachTools() {
        if (!todoTools_) return;
        todoTools_->unregisterAll();
        todoTools_.reset();
    }

    void setColScrollOffset(size_t colIndex, float v) {
        if (colIndex >= columns_.size()) return;
        columnScrolls_[columns_[colIndex].id] = v;
        viewDirty_ = true;
    }

    // reloadAll 之后调用：恢复列滚动（列已不存在则丢弃）与选中任务（findTask 校验）
    void restoreViewState() {
        if (!viewStore_) return;
        const TodoViewState st = viewStore_->load();
        for (const auto& [colId, offset] : st.columnScrolls) {
            const bool live = std::any_of(columns_.begin(), columns_.end(),
                                          [&](const Column& c) { return c.id == colId; });
            if (live) columnScrolls_[colId] = offset;
        }
        if (st.selectedTaskId != 0 && ui::findTask(tasks_, st.selectedTaskId) != nullptr) {
            selectedTaskId_ = st.selectedTaskId;
            LOG_INFO("Todo: 已恢复上次选中任务 id=" + std::to_string(st.selectedTaskId));
        }
    }

    // 尽力而为：写失败只记日志，不影响看板操作
    void saveViewState() {
        if (!viewStore_) return;
        TodoViewState st;
        st.columnScrolls = columnScrolls_;
        st.selectedTaskId = selectedTaskId_;
        auto r = viewStore_->save(st);
        if (r.isFail()) {
            LOG_WARN("Todo: 视图状态落盘失败（不影响使用）: " + r.errorMessage());
        }
        viewDirty_ = false;
    }

    // 指针落点 → 目标列索引（列外/板外 → -1）。换算见 ui::dragDropTarget；命中输入统一走 makeDragHitInput
    ui::DragHitInput makeDragHitInput() const {
        ui::DragHitInput in;
        in.fromColIndex = drag_.fromColIndex;
        in.colCount = static_cast<int>(columns_.size());
        in.pressX = drag_.pressX;
        in.pressY = drag_.pressY;
        in.pressWidth = drag_.pressWidth;
        in.cardWidth = boardPlate_.cardW;
        in.colWidth = boardPlate_.colW;
        in.colGap = boardPlate_.colGap;
        in.boardHeight = boardPlate_.height;
        // 必须用按下时刻快照（与 pressY 同帧）：读"当下"值会在拖拽途中列被滚动时混帧错位
        in.scrollOffset = drag_.pressScrollOffset;
        in.cardIndex = drag_.cardIndex;
        // 尺度吃框架显式值 ::app::contentScale()（指针坐标是原始值，未被 runtime 除 dpi）；todo 渲染在主窗口故取主窗口值
        const ::core::Router* router = ::core::appRouter();
        in.pageOriginX = router ? router->pageOriginX() : 0.0f;
        in.dpiScale = ::app::contentScale();
        return in;
    }

    int hitDropColumn(double x, double y) const {
        const ui::DragHitInput in = makeDragHitInput();
        // 探针仅作断言：几何实测尺度与框架值不一致时告警一次（不刷屏）；换算本体不吃探针值
        const float probe = ui::dragAnchorScale(in);
        if (!dpiProbeWarned_ && ui::dragScaleMismatch(in.dpiScale, probe)) {
            dpiProbeWarned_ = true;
            LOG_WARN("Todo: 拖拽尺度口径不一致——框架 contentScale=" +
                     std::to_string(in.dpiScale) + "，锚点探针实测=" +
                     std::to_string(probe) + "（换算走框架值）");
        }
        return ui::dragDropTarget(in, x, y);
    }

    // 目标列悬停行（0 = 首卡前 / >= 列卡数 = 列尾追加；越界 -1）。任务数取全量
    int hitDropRow(int targetCol, double x, double y) const {
        if (targetCol < 0 || targetCol >= static_cast<int>(columns_.size())) {
            return -1;
        }
        const ui::DragHitInput in = makeDragHitInput();
        const auto colTasks = ui::tasksInColumn(tasks_, columns_[targetCol].id);
        return ui::dragDropRowIndex(
            in, targetCol, x, y,
            ui::columnScrollOffset(columns_, columnScrolls_,
                                   static_cast<size_t>(targetCol)),
            static_cast<int>(colTasks.size()));
    }

    // 松手收尾：列外 → 取消回原位；跨列 → 有悬停行按行插入否则追加列尾；同列 → 上下重排（computeReorderRow 换算），插回原位 no-op
    void finishDrag(int target) {
        const int64_t taskId = drag_.taskId;
        const int from = drag_.fromColIndex;
        const int hoverRow = drag_.hoverRow;
        drag_ = DragState{};  // 先复位（拖拽中间态结束，无论成败）
        if (target < 0 || target >= static_cast<int>(columns_.size())) return;
        const auto& dst = columns_[static_cast<size_t>(target)];
        if (target == from) {
            if (hoverRow < 0) return;  // 搜索态同列拖 = 取消（旧语义）
            // 归档列内拖拽排序无效化：显示顺序强制 archived_at DESC，不写 position
            const Column* srcCol =
                static_cast<size_t>(from) < columns_.size()
                    ? &columns_[static_cast<size_t>(from)]
                    : nullptr;
            if (srcCol != nullptr && srcCol->kind == "archive") return;
            // ownIdx 现算（列数据可能被外部改动，须与当下 tasks_ 一致）
            const auto colTasks = ui::tasksInColumn(tasks_, dst.id);
            int ownIdx = -1;
            for (size_t i = 0; i < colTasks.size(); ++i) {
                if (colTasks[i]->id == taskId) {
                    ownIdx = static_cast<int>(i);
                    break;
                }
            }
            if (ownIdx < 0) return;  // 拖拽卡已不在该列（数据变更边缘态）→ 取消
            const auto row = ui::computeReorderRow(ownIdx, hoverRow);
            if (!row.has_value()) return;  // 插回原位：顺序不变，不写库不提示
            const int newPos =
                ui::computeReorderInsertPosition(tasks_, dst.id, taskId, *row);
            if (doMoveTask(taskId, dst.id, newPos)) {
                setStatus("已调整顺序");
            }
            return;
        }
        const int newPos = (hoverRow >= 0)
            ? ui::computeInsertPosition(tasks_, dst.id, hoverRow)
            : ui::computeAppendPosition(tasks_, dst.id);
        // 成功不提示；失败时 doMoveTask 自己的错误 toast 保留
        doMoveTask(taskId, dst.id, newPos);
    }

    // 键盘/菜单共用的上移下移（position 语义见 todo_ui_logic.h）
    void moveTaskByKeyboard(bool up) {
        // 归档列内禁止 Ctrl+↑/↓ 移动（排序强制 archived_at DESC，位置写入无效）
        const Task* self = ui::findTask(tasks_, selectedTaskId_);
        if (self != nullptr) {
            const Column* taskCol = ui::findColumn(columns_, self->columnId);
            if (taskCol != nullptr && taskCol->kind == "archive") {
                setStatus("归档任务按归档时间排序，不支持手动移动", true);
                return;
            }
        }
        const auto mv = ui::computeKeyboardMove(tasks_, selectedTaskId_, up);
        if (!mv) {
            setStatus(up ? "已经在最上面了" : "已经在最下面了");
            return;
        }
        const std::string title = self != nullptr ? self->title : "";
        doMoveTask(selectedTaskId_, mv->columnId, mv->position);
        setStatus((up ? "已上移「" : "已下移「") + title + "」");
    }

    // 状态反馈走 toast（3.5s 超时自动消失）；subtitle 保持静态说明
    void setStatus(const std::string& msg, bool isError = false) {
        // 正文留空 = 不设正文（组件自带默认正文文案随之保留）
        toast_.show(msg, "", isError);
    }

    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        ui_kit::drawToast(ui, screen, "todo.toast", toast_, themeTokens);
    }

    // 消费挂在 Todo 页的跳转负载（consumePendingPayload 取出即清空，逐帧调用不重复）。
    // "action:new_task" → 新建任务弹窗
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
        dialogs_.newTask.reset(columnId);   // 打开清（含内嵌红字）
        dialogs_.popups.open(ui::Popup::NewTask);
    }

    // 「+ 列」不即时落库，先走命名弹窗
    void openNewColumnDialog() {
        dialogs_.newColumn.reset();        // 打开清（含内嵌红字）
        dialogs_.popups.open(ui::Popup::NewColumn);
    }

    // 打开列重命名弹窗（右键菜单与列头 ✎ 共用）
    void openRenameDialog(const Column& col) {
        dialogs_.rename.reset(col);        // 打开清（含内嵌红字）
        dialogs_.popups.open(ui::Popup::Rename);
    }

    void openDetailDialog(const Task& task) {
        selectedTaskId_ = task.id;  // 选中态：详情弹窗关闭后保留，供 Ctrl+↑/↓
        saveViewState();
        dialogs_.detail.reset(task);
        // 归档列卡片详情只读：判据 = 所在列 kind（运行期真相，不靠 archived_at 值）
        const Column* col = ui::findColumn(columns_, task.columnId);
        dialogs_.detail.readOnly = (col != nullptr && col->kind == "archive");
        dialogs_.popups.open(ui::Popup::Detail);
    }

    // 菜单定位走模块布局空间（ui::boardGeom），不用窗口坐标
    void openTaskContextMenu(const Task& task, int columnIndex, int cardIndex) {
        selectedTaskId_ = task.id;
        saveViewState();
        dialogs_.ctx.openTask(task, columnIndex, cardIndex);
        dialogs_.popups.open(ui::Popup::Ctx);
    }

    void openColumnContextMenu(const Column& col, int columnIndex) {
        dialogs_.ctx.openColumn(col, columnIndex);
        dialogs_.popups.open(ui::Popup::Ctx);
    }

    // Esc 关栈顶弹窗（与绘制 z 序同源）。返回 true = 键已消费；无弹窗返回 false
    bool closeTopmostPopup() {
        if (dialogs_.popups.empty()) return false;
        const ui::Popup top = dialogs_.popups.top();
        if (top == ui::Popup::Detail) {
            // 未存改动守卫：第一次只警告不关（栈保持不动，再按一次才真关）
            dialogs_.detail.requestClose(dialogs_.popups, dialogHost_);
            return true;
        }
        dialogs_.popups.close(top);
        return true;
    }

    // 菜单项点击分发（路径版，兼容「改色」子菜单）。列菜单 3=改色（path[1]=预设索引）
    void onContextSelectPath(const std::vector<int>& path) {
        if (dialogs_.ctx.taskId != 0) {
            if (!path.empty()) onContextSelect(path[0]);
            return;
        }
        if (path.size() == 2 && path[0] == 3) {
            const Column* col = ui::findColumn(columns_, dialogs_.ctx.columnId);
            if (col != nullptr && path[1] >= 0 && path[1] < ui::kColorPresetCount) {
                doSetColumnColor(col->id, ui::kColorPresets[path[1]].token);
            }
            return;
        }
        // 改色插在前面，删除列索引 3→4
        onContextSelect(!path.empty() && path[0] == 4 ? 3 : (path.empty() ? -1 : path[0]));
    }

    // 菜单项分发，索引与 drawContextMenu 构建的 items 对应。任务菜单：0=编辑
    // 1=上移 2=下移 3+i=移到第 i 个其他列 末项=删除；列菜单改色走 onContextSelectPath
    void onContextSelect(int index) {
        if (dialogs_.ctx.taskId != 0) {
            const Task* self = ui::findTask(tasks_, dialogs_.ctx.taskId);
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
                dialogs_.confirm.openForTask(dialogs_.ctx.taskId, self->title);
                dialogs_.popups.open(ui::Popup::Confirm);
                return;
            }
            int k = index - 3;
            for (const auto& c : columns_) {
                if (c.id == self->columnId) continue;
                if (k == 0) {
                    doMoveTask(dialogs_.ctx.taskId, c.id,
                               ui::computeAppendPosition(tasks_, c.id));
                    return;
                }
                --k;
            }
            return;
        }
        const Column* col = ui::findColumn(columns_, dialogs_.ctx.columnId);
        if (col == nullptr) return;
        switch (index) {
            case 0:
                openRenameDialog(*col);
                break;
            case 1: doShiftColumn(col->id, -1); break;
            case 2: doShiftColumn(col->id, +1); break;
            case 3:
                dialogs_.confirm.openForColumn(col->id);
                dialogs_.popups.open(ui::Popup::Confirm);
                break;
            default: break;
        }
    }

    void compose(eui::Ui& page, const eui::Screen& screen) override {
        // 库被 GitHub Sync 导入覆盖 → 列/任务缓存指向旧行，重查（导入线程只置标记）。
        if (dbImportedDirty_.exchange(false, std::memory_order_acq_rel)) {
            reloadAll();
            LOG_INFO("Todo: 检测到库导入，已重载看板数据");
        }

        consumeNavPayload();

        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = ui_kit::kHeaderH;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;

        // 页面底色 = 主窗口 clearColor（运行时跟随主题/效果档）——不自铺不透明底，半透/磨砂档才生效

        // 拖拽跟手副本锚点（页面 compose 空间）：换算因子复用 makeDragHitInput，与 hitDropColumn 同源，不另造尺度
        const ui::DragHitInput dragHit = makeDragHitInput();
        const ui::GhostAnchor ghostAnchor = ui::dragGhostAnchor(
            dragHit.pressX, dragHit.pressY, drag_.totalX, drag_.totalY,
            dragHit.pageOriginX, dragHit.dpiScale);

        // 看板绘制入参（容器是成员引用，不每帧拷贝）
        const ui::BoardView boardView{
            columns_, tasks_, searchMatchIds_, searchQuery_, columnScrolls_,
            selectedTaskId_, drag_.active, drag_.taskId, drag_.hoverColIndex,
            drag_.hoverRow, drag_.active && drag_.taskId != 0, ghostAnchor.x,
            ghostAnchor.y, overdueCount_, service_ != nullptr, loadError_,
            boardActions_};
        page.column("todo.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                ui::drawHeader(page, contentW, headerH, bodyH, t, boardView);
                ui::drawBoard(page, contentW, bodyH, t, boardView, boardPlate_);
            })
            .build();

        // 拖拽跟手副本：顶层 root（压在列之上、低于弹窗），plate 取本帧 drawBoard 刚写入的卡宽
        ui::drawDragGhost(page, t, boardView, boardPlate_);

        ui::drawNewTaskDialog(page, screen, t, dialogs_, dialogHost_);
        ui::drawNewColumnDialog(page, screen, t, dialogs_, dialogHost_);
        ui::drawDetailDialog(page, screen, t, dialogs_, dialogHost_);
        ui::drawOverdueDialog(page, screen, t, dialogs_, dialogHost_);
        ui::drawContextMenu(page, screen, t, dialogs_, dialogHost_, columnScrolls_);
        ui::drawDatePicker(page, screen, t, dialogs_, dialogHost_);
        ui::drawRenameDialog(page, screen, t, dialogs_, dialogHost_);
        ui::drawConfirmDeleteDialog(page, screen, t, dialogs_, dialogHost_);
        drawToast(page, screen, core::theme::componentTokens(t));
    }

    // Ctrl/Cmd+↑↓ 上移/下移选中任务（选中 = 最近点击的卡片）。无聚焦元素消费时经 Router 转发到本页
    bool onKeyEvent(const eui::KeyEvent& event) override {
        if (event.action != eui::KeyAction::Press &&
            event.action != eui::KeyAction::Repeat) {
            return false;
        }
        if (event.key == eui::InputKey::Escape) {
            return closeTopmostPopup();
        }
        if (event.key != eui::InputKey::Up && event.key != eui::InputKey::Down) {
            return false;
        }
        if (!event.modifiers.shortcut()) return false;
        if (service_ == nullptr) return false;
        if (selectedTaskId_ == 0 || ui::findTask(tasks_, selectedTaskId_) == nullptr) {
            setStatus("先点击卡片选中任务，再用 Ctrl+↑/↓ 移动", true);
            return false;
        }
        moveTaskByKeyboard(event.key == eui::InputKey::Up);
        return true;
    }

private:
    // 拖拽状态机：onPress 记锚点 → onDrag 更新悬停列 → onRelease 按落点落库；
    // pressX/pressY/pressScrollOffset 同帧快照，拖动途中不重算，绘制单元只读快照值
    struct DragState {
        bool active = false;       // 超过 dragThreshold 才置位
        int64_t taskId = 0;        // 稳定任务 id（不用会过期的列/卡索引做落库依据）
        int fromColIndex = -1;
        int cardIndex = -1;
        float pressX = 0.0f;       // 按下时卡片 bounds（指针像素空间）
        float pressY = 0.0f;
        float pressWidth = 0.0f;
        // 与 pressX/pressY 同帧的源列滚动快照；dragDropTarget 靠它还原板面顶边
        float pressScrollOffset = 0.0f;
        int hoverColIndex = -1;    // 拖动中指针所在列（-1 = 列外，松手取消）
        // 目标列行（-1 = 不预览；仅搜索态不置位）。驱动占位条 + 落库插入位
        int hoverRow = -1;
        // 按下点起的指针累计位移（DragEvent::totalX/Y）：只喂副本锚点，不参与命中换算；复位 DragState{} 即归零
        double totalX = 0.0;
        double totalY = 0.0;
    };

    // 通道绑定（构造期绑定一次）：回调体读模块当下状态；UI 单元只拿通道与只读数据，不持有本类

    ui::BoardActions boardActions_{
        .searchChanged = [this](const std::string& value) {
            searchQuery_ = value;
            refreshSearch();
        },
        .searchCancelled = [this] {
            // 清除只挂 Esc（onCancel），Enter 无副作用
            if (!searchQuery_.empty()) {
                searchQuery_.clear();
                refreshSearch();
            }
        },
        .overdueClicked = [this] {
            if (overdueCount_ > 0) openOverdueDialog();
        },
        .addColumnClicked = [this] {
            openNewColumnDialog();
        },
        .addTaskClicked = [this] {
            if (columns_.empty()) {
                setStatus("请先创建至少一列", true);
                return;
            }
            openNewTaskDialog(columns_.front().id);
        },
        .retryClicked = [this] { retryLoad(); },
        .columnMenu = [this](const Column& col, int columnIndex) {
            openColumnContextMenu(col, columnIndex);
        },
        .renameColumn = [this](const Column& col) { openRenameDialog(col); },
        .addTaskToColumn = [this](int64_t columnId) { openNewTaskDialog(columnId); },
        .columnScrollChanged = [this](size_t colIndex, float value) {
            setColScrollOffset(colIndex, value);
        },
        .cardClicked = [this](const Task& task) { openDetailDialog(task); },
        .cardMenu = [this](const Task& task, int columnIndex, int cardIndex) {
            openTaskContextMenu(task, columnIndex, cardIndex);
        },
        .cardPressed = [this](const Task& task, int colIndex, int cardIndex,
                              const eui::Rect& bounds) {
            // 记录拖拽锚点（bounds 为指针像素空间，含列内滚动）
            drag_ = DragState{};
            drag_.taskId = task.id;
            drag_.fromColIndex = colIndex;
            drag_.cardIndex = cardIndex;
            drag_.pressX = bounds.x;
            drag_.pressY = bounds.y;
            drag_.pressWidth = bounds.width;
            // bounds.y 含「按下这一帧」的列滚动；在此快照与 pressY 同帧（onDrag 再读新值会混帧 → 命中框上下偏）
            drag_.pressScrollOffset =
                ui::columnScrollOffset(columns_, columnScrolls_,
                                       static_cast<size_t>(colIndex));
        },
        .cardDragged = [this](double x, double y, double totalX, double totalY) {
            // 超过 dragThreshold 才派发 → 到这里即视为拖拽激活
            if (drag_.taskId == 0) return;
            drag_.active = true;
            // 只快照原始位移，换算在 compose（与命中同源）
            drag_.totalX = totalX;
            drag_.totalY = totalY;
            const int target = hitDropColumn(x, y);
            drag_.hoverColIndex = target;
            // 跨列与同列都置位（同列 = 上下重排）；搜索态不置位（渲染行 ≠ 全量行号，跨列回落追加、同列回落取消）
            drag_.hoverRow = (target >= 0 && searchQuery_.empty())
                ? hitDropRow(target, x, y)
                : -1;
        },
        .cardReleased = [this](double x, double y, bool released) {
            if (!drag_.active) {
                drag_ = DragState{};  // 普通点击：只清锚点（onClick 已处理）
                return;
            }
            // 落位淡入不设标志：列容器 opacity 直接门控 drag_.active，归 false 那帧起 0.6→1.0 淡回
            const int target = released ? hitDropColumn(x, y) : -1;
            finishDrag(target);
        },
    };

    ui::DialogHost dialogHost_{
        .columns = &columns_,
        .tasks = &tasks_,
        .createTask = [this](int64_t columnId, const std::string& title) {
            return doCreateTask(columnId, title);
        },
        .createColumn = [this](const std::string& title) {
            return doCreateColumn(title);
        },
        .renameColumn = [this](int64_t id, const std::string& title) {
            return doRenameColumn(id, title);
        },
        .deleteTask = [this](int64_t taskId) { doDeleteTask(taskId); },
        .deleteColumn = [this](int64_t id) { doDeleteColumn(id); },
        .updateTask = [this](const Task& task) { return doUpdateTask(task); },
        .moveTask = [this](int64_t taskId, int64_t toColumnId, int newPosition) {
            doMoveTask(taskId, toColumnId, newPosition);
        },
        .completeTask = [this](int64_t taskId) { doCompleteTask(taskId); },
        .uncompleteTask = [this](int64_t taskId) { doUncompleteTask(taskId); },
        .openDetail = [this](const Task& task) { openDetailDialog(task); },
        .contextSelectPath = [this](const std::vector<int>& path) {
            onContextSelectPath(path);
        },
        .warn = [this](const std::string& msg) { setStatus(msg, true); },
    };

    std::unique_ptr<TodoService> service_;
    // 工具集（引用 service_；声明在前 = 先于 service_ 析构，引用不悬垂）
    std::unique_ptr<TodoTools> todoTools_;
    // GitHub Sync 导入成功 → 置脏，compose 收割 reloadAll（导入线程只碰原子，不碰 UI 成员）。
    // 闸 = 存活守卫，onStop 先关闸再退订（token 析构即退订）。
    std::atomic<bool> dbImportedDirty_{false};
    std::atomic<bool> dbImportHandlersEnabled_{false};
    ::core::event::EventBus::Subscription dbImportedSub_;
    bool started_ = false;
    std::vector<Column> columns_;
    std::vector<Task> tasks_;
    // 列内滚动偏移按列 id 记忆 + 跨会话持久化（viewDirty_ = 有未落盘变更，onStop 收口）
    std::map<int64_t, float> columnScrolls_;
    std::unique_ptr<TodoViewStateStore> viewStore_;
    bool viewDirty_ = false;

    DragState drag_;

    // 板面几何快照（每帧刷新，拖拽命中换算用）
    ui::BoardPlate boardPlate_;

    // dpi 探针告警只出一次（hitDropColumn 是 const 且每帧调用，故 mutable）
    mutable bool dpiProbeWarned_ = false;

    ui::DialogSet dialogs_;

    // 选中态（最近点击的卡片，详情弹窗关闭后保留）
    int64_t selectedTaskId_ = 0;

    ui_kit::ToastState toast_;

    std::string searchQuery_;
    std::unordered_set<int64_t> searchMatchIds_;

    size_t overdueCount_ = 0;

    // 数据加载错误（空 = 正常；与空态区分）
    std::string loadError_;
};

} // namespace module::todo

extern "C" ::core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}
