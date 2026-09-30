// src/module_todo/todo_module.cpp
// Todo 看板模块 - 真实数据 + CRUD（TodoService）
//
// R5（god-class 拆分，spec 2026-09-27 §4.2）后的职责边界：
//   本文件    = Module 接口 + compose 骨架 + **拖拽状态机** + 数据 reload + 写操作
//               + 右键菜单分发 + 视图状态 + 键盘移动（Esc 关栈顶）
//   看板绘制  → todo_ui_board.{h,cpp}（表头/看板/列/列内容/卡片）
//   弹窗状态  → todo_dialogs.h（6 弹窗 + 右键菜单 + 打开栈 + 表单提交）
//   弹窗绘制  → todo_ui_dialogs.cpp
//   纯函数    → todo_ui_logic.h（几何/命中换算/标签配色/查找）
// 两条通道（BoardActions / DialogHost）在本类构造期绑定一次：回调体读的是模块
// 当下状态，与拆分前 `[this] { ... }` 同款语义；UI 单元不持有、也不捕获本类。
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/component_theme.h"
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

    // ============ 生命周期 ============

    void onStart() override {
        auto* db = ::core::appDatabase();
        if (!db) {
            LOG_WARN("Todo: 数据库尚未注册，跳过数据加载");
            return;
        }
        service_ = std::make_unique<TodoService>(*db);
        attachTools();
        reloadAll();
        // 审计 [23]：恢复列滚动（按列 id）+ 选中任务（reloadAll 之后，列/任务已就位）
        viewStore_ = std::make_unique<TodoViewStateStore>(todoViewStateFile());
        restoreViewState();
        LOG_INFO("Todo 模块启动（" + std::to_string(columns_.size()) + " 列 / " +
                 std::to_string(tasks_.size()) + " 任务）");
    }

    void onStop() override {
        if (viewDirty_) saveViewState();  // 审计 [23]：退出前落盘视图状态
        detachTools();                    // 先注销工具（工具引用 service）
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
        // 审计 [23]：滚动按列 id 记忆（旧按列索引 vector，重排/删列后串列）；
        // 剪掉已不存在列的记录，防文件膨胀。prune 有删除 → 状态待落盘
        std::vector<int64_t> liveIds;
        liveIds.reserve(columns_.size());
        for (const auto& c : columns_) liveIds.push_back(c.id);
        if (TodoViewStateStore::pruneScrolls(columnScrolls_, liveIds) > 0) {
            viewDirty_ = true;
        }
        // 拖拽中任务被外部操作删除（如确认弹窗回调）→ 拖拽锚点失效，复位防悬垂
        if (drag_.active && ui::findTask(tasks_, drag_.taskId) == nullptr) {
            drag_ = DragState{};
        }
        // 逾期 badge 数量 + 搜索命中集跟随数据刷新（写操作后 reloadAll 统一收口）
        refreshOverdueCount();
        if (!searchQuery_.empty()) refreshSearch();
    }

    // ============ 搜索 / 逾期（W 计划 P2 三件套）============

    // 搜索词变化 → 重新调用 service_->search（title/description LIKE）刷新命中集
    void refreshSearch() {
        searchMatchIds_.clear();
        if (searchQuery_.empty() || !service_) return;
        auto r = service_->search(searchQuery_);
        if (r.isFail()) {
            setStatus("搜索失败：" + r.errorMessage(), true);
            return;
        }
        // 审计 perf [40]：unordered_set 命中表（filterBySearch O(1) 查）
        for (const auto& t : r.value()) {
            searchMatchIds_.insert(t.id);
        }
    }

    void refreshOverdueCount() {
        if (!service_) return;
        auto r = service_->listOverdue();
        if (r.isFail()) {
            // 审计 THS-06：读取失败不得降级成「0 条」——badge 保留上次已知
            // 计数（不假装为空），并把失败报出去。此前无 else、无日志，
            // badge 恒 0 且用户无从察觉。
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
            // 审计 THS-06：查询失败不得当成空结果开弹窗——入口刚显示 N 条，
            // 弹「逾期任务（0）」会让用户以为数据丢了。改为报错 + 不开弹窗
            //（弹窗数据保持上次内容，不写入伪空）。
            LOG_ERROR("Todo: 逾期弹窗查询失败: " + r.errorMessage());
            setStatus("逾期任务加载失败：" + r.errorMessage(), true);
            return;
        }
        dialogs_.overdue.tasks = std::move(r).value();
        overdueCount_ = dialogs_.overdue.tasks.size();
        dialogs_.popups.open(ui::Popup::Overdue);
    }

    // 错误态重试：数据库晚注册（onStart 时为 null）或读取失败后手动恢复
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

    // ============ 写操作（都落到 SQLite） ============
    // 审计 [19]：doCreate*/doRename* 返回错误信息（空 = 成功），校验/创建
    // 失败由弹窗调用方写进内嵌红字 formError（照抄 claude formError_ 模式：
    // 弹窗不关、就地可见、下一次输入清除），不再走「关弹窗 + toast」。
    // 成功路径的确认反馈仍走 toast（操作结果 = toast 的全局分工）。

    std::string doCreateTask(int64_t columnId, const std::string& title) {
        if (!service_) return "服务未就绪（TodoService 未注册）";
        if (title.empty()) return "任务标题不能为空";
        auto r = service_->createTask(columnId, title);
        if (r.isFail()) {
            LOG_ERROR("Todo: createTask 失败: " + r.errorMessage());
            return "新建任务失败：" + r.errorMessage();
        }
        setStatus("已创建任务「" + title + "」");
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
        return "";
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
        finishCompletionMove(taskId, "done");
    }

    void doUncompleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->uncompleteTask(taskId);
        if (r.isFail()) {
            setStatus("取消完成失败：" + r.errorMessage(), true);
            return;
        }
        finishCompletionMove(taskId, "todo");
    }

    // 状态迁移成功后的列迁移收尾——「状态迁移 + 列迁移」合起来才算一次操作
    //（审计 THS-06）：列迁移失败时不得再提示成功，改报错并按真实状态刷新。
    // 此前只 LOG_WARN 就接着 setStatus("任务已完成")，用户看到成功提示而卡片
    // 留在原列，再点一次也看不出原因。
    // 目标列按语义标记 kind（'done' / 'todo'）定位，不按标题也不按
    // columns_.front()：用户重命名/重排列后标题与下标都不可靠。
    void finishCompletionMove(int64_t taskId, const std::string& kind) {
        const bool toDone = (kind == "done");
        const std::string successMsg = toDone ? "任务已完成" : "已取消完成";
        const int64_t targetCol = ui::findColumnByKind(columns_, kind);
        if (targetCol == 0) {
            // 明示而非静默：没有语义列时任务留在原列，用户需要知道为什么
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
        // toast 图标通道已带 fa-check（drawToast），正文不再混排 ✓ 字符
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
    }

    // 返回是否保存成功（详情弹窗动作按钮先落表单再执行动作，保存失败要中止
    // 动作并保留弹窗——产品审计 2026-09-23 [2]；其余调用方忽略返回值）
    bool doUpdateTask(const Task& task) {
        if (!service_) return false;
        auto r = service_->updateTask(task);
        if (r.isFail()) {
            setStatus("保存失败：" + r.errorMessage(), true);
            return false;
        }
        setStatus("已保存");
        reloadAll();
        return true;
    }

    // ============ 列管理（service 四 API 中的 rename/delete/reorder 接线） ============

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

    // ---- 智能体工具（agent M1 Task 8）----
    // TodoService 构造处旁挂工具集：service 与 tools 生命周期成对
    //（service_ 存在 ⇒ todoTools_ 存在且已注册；onStop 先注销再放 service）。
    // retryLoad 晚建 service 时同样要补注册，故收成两个小口。
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

    // ---- 视图状态持久化（审计 [23]）----

    // onStart 的 reloadAll 之后调用：恢复列滚动（列已不存在则丢弃该条）
    // 与选中任务（任务已删/移动走则不恢复，findTask 校验）
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

    // ---- W8 拖拽（真 drop 语义）----

    // 指针落点 → 目标列索引（列外/板外 → -1）。换算细节见 ui::dragDropTarget。
    // UI 整改 13：命中输入统一走 makeDragHitInput（hitDropColumn / hitDropRow
    // 同源同帧），行命中只喂占位条预览。
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
        // tech-debt B：必须用按下时刻的快照（与 pressY 同帧）。读"当下"的
        // colScrollOffset 会在拖拽途中列被滚动时两帧混值，板面顶边整体错位
        in.scrollOffset = drag_.pressScrollOffset;
        in.cardIndex = drag_.cardIndex;
        // U1 尺度契约：指针 → compose 的换算吃**框架显式**尺度，不再由锚点
        // 反推。onPress / onDrag / onRelease 通道的指针是**原始**值（runtime
        // 只对 onContextMenu / onMove 做 /dpiScale），故尺度 = 主窗口内容缩放
        // ::app::contentScale()（物理 = 逻辑 * scale，与 compose 收到的
        // Screen::scale 同源同值）。todo 是 Router 页面模块、渲染在主窗口，
        // 故取主窗口值（子窗才用自己 compose 的 Screen::scale）。
        const ::core::Router* router = ::core::appRouter();
        in.pageOriginX = router ? router->pageOriginX() : 0.0f;
        in.dpiScale = ::app::contentScale();
        return in;
    }

    int hitDropColumn(double x, double y) const {
        const ui::DragHitInput in = makeDragHitInput();
        // 探针降级为**断言**：几何实测尺度（按下卡片 bounds 宽 / compose 卡宽）
        // 只在"自称 ≠1 却对不上框架值"时告警一次（拖动中不刷屏），留痕供
        // 100%/125%/150% 三档点验比对（审计 V2）。换算本体不吃探针值。
        const float probe = ui::dragAnchorScale(in);
        if (!dpiProbeWarned_ && ui::dragScaleMismatch(in.dpiScale, probe)) {
            dpiProbeWarned_ = true;
            LOG_WARN("Todo: 拖拽尺度口径不一致——框架 contentScale=" +
                     std::to_string(in.dpiScale) + "，锚点探针实测=" +
                     std::to_string(probe) + "（换算走框架值）");
        }
        return ui::dragDropTarget(in, x, y);
    }

    // 指针在目标列的悬停行（0 = 首卡前 / >= 列卡数 = 列尾追加；越界 -1）。
    // 列内任务数取**全量**（搜索态由调用方置 -1 走追加兜底，行号语义不变）。
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

    // 松手收尾（2026-09-28 真机缺陷 3 后语义）：
    //   · 目标列外（target < 0，含指针拖出板面/落列间隙）→ 取消，卡片回原位；
    //   · 跨列（target != from）→ 按稳定 taskId 落库：有悬停行按行插入
    //     （computeInsertPosition），无（搜索态等）追加列尾（UI 整改 13 原语义）；
    //   · 同列（target == from）→ 列内上下重排：悬停行经 computeReorderRow
    //     换算成「摘出自身后」的行序，computeReorderInsertPosition 取落库位；
    //     插回原位（顺序不变）→ no-op 不落库；无悬停行（搜索态）→ 取消
    //     （保持"搜索态不重排"的旧行为）。真·原地松手/微小位移在框架侧已被
    //     dragThreshold 门控（不派发 onDrag → drag_.active 未置位），到不了这里。
    void finishDrag(int target) {
        const int64_t taskId = drag_.taskId;
        const int from = drag_.fromColIndex;
        const int hoverRow = drag_.hoverRow;
        drag_ = DragState{};  // 先复位（拖拽中间态结束，无论成败）
        if (target < 0 || target >= static_cast<int>(columns_.size())) return;
        const auto& dst = columns_[static_cast<size_t>(target)];
        if (target == from) {
            if (hoverRow < 0) return;  // 搜索态同列拖 = 取消（旧语义）
            // ownIdx 现算（不吃按下时的 cardIndex：拖拽途中列数据可能被
            // 外部操作改动，悬停行的坐标系须与当下 tasks_ 一致）
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
        const std::string title = dst.title;
        const int newPos = (hoverRow >= 0)
            ? ui::computeInsertPosition(tasks_, dst.id, hoverRow)
            : ui::computeAppendPosition(tasks_, dst.id);
        if (doMoveTask(taskId, dst.id, newPos)) {
            setStatus("已移动到「" + title + "」");  // 失败时保留 doMoveTask 的错误提示
        }
    }

    // 键盘/菜单共用的上移下移（moveTask 的 position 语义见 todo_ui_logic.h）
    void moveTaskByKeyboard(bool up) {
        const auto mv = ui::computeKeyboardMove(tasks_, selectedTaskId_, up);
        if (!mv) {
            setStatus(up ? "已经在最上面了" : "已经在最下面了");
            return;
        }
        const Task* self = ui::findTask(tasks_, selectedTaskId_);
        const std::string title = self != nullptr ? self->title : "";
        doMoveTask(selectedTaskId_, mv->columnId, mv->position);
        setStatus((up ? "已上移「" : "已下移「") + title + "」");
    }

    // ============ 状态提示 ============

    // 状态反馈走 toast（3.5s 超时自动消失，仿 markdown/claude 模块）。
    // 此前写进 header subtitle 永久驻留，静态操作说明被最后一条
    // 操作结果永久顶掉；现在 subtitle 始终保持静态说明。
    void setStatus(const std::string& msg, bool isError = false) {
        // 正文留空 = 不设正文（本模块 toast 现状即只有标题：组件自带默认正文
        // 文案随之保留，R1 零行为变化）
        toast_.show(msg, "", isError);
    }

    // ---------- Toast（画在弹窗之上，超时消失；状态位 + 绘制接线走 ui_kit） ----------
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        ui_kit::drawToast(ui, screen, "todo.toast", toast_, themeTokens);
    }

    // ============ 弹窗编排（状态与绘制在 todo_dialogs.h / todo_ui_dialogs.cpp） ============

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
        dialogs_.newTask.reset(columnId);   // 审计 [19]：打开清（含内嵌红字）
        dialogs_.popups.open(ui::Popup::NewTask);
    }

    // 审计 [24]：「+ 列」不再即时落库，先走命名弹窗
    void openNewColumnDialog() {
        dialogs_.newColumn.reset();        // 审计 [19]：打开清（含内嵌红字）
        dialogs_.popups.open(ui::Popup::NewColumn);
    }

    // 打开列重命名弹窗（右键菜单「重命名列」与列头 ✎ 按钮共用，审计 [24]）
    void openRenameDialog(const Column& col) {
        dialogs_.rename.reset(col);        // 审计 [19]：打开清（含内嵌红字）
        dialogs_.popups.open(ui::Popup::Rename);
    }

    // 打开任务详情（选中态与视图状态落盘留在模块；表单装载走 DetailDialog）
    void openDetailDialog(const Task& task) {
        selectedTaskId_ = task.id;  // 选中态：详情弹窗关闭后保留，供 Ctrl+↑/↓
        saveViewState();            // 审计 [23]：同上，即时落盘
        dialogs_.detail.reset(task);
        dialogs_.popups.open(ui::Popup::Detail);
    }

    // 菜单定位走模块自身布局空间（ui::boardGeom），不用窗口坐标（见 BoardGeom 注释）
    void openTaskContextMenu(const Task& task, int columnIndex, int cardIndex) {
        selectedTaskId_ = task.id;
        saveViewState();  // 审计 [23]：选中任务离散变更，即时落盘
        dialogs_.ctx.openTask(task, columnIndex, cardIndex);
        dialogs_.popups.open(ui::Popup::Ctx);
    }

    void openColumnContextMenu(const Column& col, int columnIndex) {
        dialogs_.ctx.openColumn(col, columnIndex);
        dialogs_.popups.open(ui::Popup::Ctx);
    }

    // 审计 M5：Esc 关「最上层」弹窗（= 栈顶，与绘制 z 序同源）。
    // 返回 true = 本键已消费；无弹窗返回 false（按键继续走既有路径）
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

    // 菜单项点击分发（路径版，兼容「改色」子菜单）。
    // 任务菜单无子级，path[0] 即原索引；列菜单：0=重命名 1=左移 2=右移
    //   3=改色（path[1] = 预设索引）4=删除
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
        // 删除列从原索引 3 挪到 4（改色插在前面）
        onContextSelect(!path.empty() && path[0] == 4 ? 3 : (path.empty() ? -1 : path[0]));
    }

    // 菜单项点击分发。索引含义与 drawContextMenu 构建的 items 一一对应：
    //   任务菜单：0=编辑 1=上移 2=下移 3+i=移动到第 i 个其他列 末项=删除（带确认）
    //   列菜单：  0=重命名 1=左移 2=右移 3=删除（改色走 onContextSelectPath，不进这里）
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
                    const std::string title = c.title;
                    doMoveTask(dialogs_.ctx.taskId, c.id,
                               ui::computeAppendPosition(tasks_, c.id));
                    setStatus("已移动到「" + title + "」");
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

    // ============ UI ============

    void compose(eui::Ui& page, const eui::Screen& screen) override {
        // 消费跨模块跳转负载（Router payload，一次性语义）
        consumeNavPayload();

        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = ui_kit::kHeaderH;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

        // 拖拽跟手副本锚点（页面 compose 空间）：按下时卡片位置 + 指针累计
        // 位移。换算因子复用 makeDragHitInput 的同一份（pressX/pressY 的指针
        // 空间 + pageOriginX + app::contentScale()）——与 hitDropColumn 同源，
        // 不另造一套尺度（U1 尺度契约）
        const ui::DragHitInput dragHit = makeDragHitInput();
        const ui::GhostAnchor ghostAnchor = ui::dragGhostAnchor(
            dragHit.pressX, dragHit.pressY, drag_.totalX, drag_.totalY,
            dragHit.pageOriginX, dragHit.dpiScale);

        // 看板绘制入参（R5：状态显式入参；容器是成员引用，不每帧拷贝）
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

        // 拖拽跟手副本：页面级顶层 root（在 todo.root 之后 = 压在所有列之上，
        // zIndex 压住看板子树、低于弹窗），plate 取本帧 drawBoard 刚写入的卡宽
        ui::drawDragGhost(page, t, boardView, boardPlate_);

        // 弹窗（画在最上层）
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

    // P7a 键盘移动：Ctrl/Cmd+↑↓ 上移/下移当前选中任务（选中 = 最近
    // 点击过的卡片，点击详情弹窗关闭后选中态保留）。
    // 事件来源：EUI-NEO 无聚焦元素消费时经 main.cpp → Router 转发到本页。
    bool onKeyEvent(const eui::KeyEvent& event) override {
        if (event.action != eui::KeyAction::Press &&
            event.action != eui::KeyAction::Repeat) {
            return false;
        }
        // 审计 M5：Esc 关最上层弹窗。键走得到本通道 = 焦点元素没消费（弹窗
        // 输入框聚焦时 Esc 已被 input 的 onCancel 收走 → 天然让位不重复）。
        // 审计 [24] 的新建列专用兜底并入 closeTopmostPopup。
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
    // ============ 拖拽状态机 ============
    // W8 拖拽（真 drop 语义）：onPress 记锚点 → onDrag 更新悬停列（视觉反馈）
    // → onRelease 按指针落点落库。dragging 半透明 + 目标列高亮见看板绘制单元。
    // 状态机留在本类（tech-debt B：pressX/pressY/pressScrollOffset 同帧快照，
    // 拖动途中不重算），绘制单元只读 active/taskId/hoverColIndex 三个快照值。
    // 声明位置在通道绑定之前：下面 cardPressed/cardReleased 的绑定体里要
    // `DragState{}` 复位，成员初始化器按声明顺序解析，早声明才能被看到。
    struct DragState {
        bool active = false;       // 超过 dragThreshold 才置位
        int64_t taskId = 0;        // 稳定任务 id（不用会过期的列/卡索引做落库依据）
        int fromColIndex = -1;
        int cardIndex = -1;
        float pressX = 0.0f;       // 按下时卡片 bounds（指针像素空间）
        float pressY = 0.0f;
        float pressWidth = 0.0f;
        // 与 pressX/pressY 同帧的源列滚动偏移快照（compose 空间）。
        // dragDropTarget 靠它把含滚动的 bounds.y 还原成板面顶边，
        // 取"当下"值会跨帧混用（tech-debt B，见 cardPressed 处注释）
        float pressScrollOffset = 0.0f;
        int hoverColIndex = -1;    // 拖动中指针所在列（-1 = 列外，松手取消）
        // 拖动中指针所在目标列行（-1 = 不预览；跨列与同列都置位，仅搜索态
        // 不置位——渲染行号与全量行号不同源）。驱动占位条视觉 + 落库插入位
        //（finishDrag 快照后复位；同列经 computeReorderRow 换算成摘出自身
        // 后的行序）
        int hoverRow = -1;
        // 跟手副本（2026-09-28）：按下点起的指针**累计**位移（指针像素空间，
        // DragEvent::totalX/totalY——框架按下时记锚点、每次移动给累计值）。
        // 只喂跟手副本锚点（ui::dragGhostAnchor），不参与命中换算（命中仍吃
        // onDrag 的"当下"指针坐标）。drag_ = DragState{} 复位即随之为 0 →
        // 松手/取消后副本偏移天然归零。
        double totalX = 0.0;
        double totalY = 0.0;
    };

    // ============ 通道绑定（构造期绑定一次） ============
    // 回调体读模块**当下**状态（与拆分前 `[this] { ... }` 同款语义）；
    // 拆出的 UI 单元只拿到这些通道与只读数据，不持有本类。

    ui::BoardActions boardActions_{
        .searchChanged = [this](const std::string& value) {
            searchQuery_ = value;
            refreshSearch();
        },
        .searchCancelled = [this] {
            // 审计 [10]：清除只挂 Esc（onCancel），Enter 无副作用
            if (!searchQuery_.empty()) {
                searchQuery_.clear();
                refreshSearch();
            }
        },
        .overdueClicked = [this] {
            if (overdueCount_ > 0) openOverdueDialog();
        },
        .addColumnClicked = [this] {
            // 审计 [24]：不再即时 doCreateColumn("新列 N") 落库，
            // 先走命名弹窗，与全应用创建范式统一
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
            // 记录拖拽锚点（卡片 bounds 为指针像素空间，含列内滚动偏移）
            drag_ = DragState{};
            drag_.taskId = task.id;
            drag_.fromColIndex = colIndex;
            drag_.cardIndex = cardIndex;
            drag_.pressX = bounds.x;
            drag_.pressY = bounds.y;
            drag_.pressWidth = bounds.width;
            // tech-debt B：bounds.y 里含的是"按下这一帧"的列滚动偏移，
            // 命中换算必须减同一个值才能还原出正确的板面顶边。拖拽途中
            // 源列被滚轮滚动时，若 onDrag 时刻再读 colScrollOffset（新值），
            // pressY（旧帧）× scrollOffset（新帧）混帧 → 命中框整体上偏/下偏。
            // 在按下回调内快照，保证两值同帧。
            drag_.pressScrollOffset =
                ui::columnScrollOffset(columns_, columnScrolls_,
                                       static_cast<size_t>(colIndex));
        },
        .cardDragged = [this](double x, double y, double totalX, double totalY) {
            // 运行时只在超过 dragThreshold 后派发 → 到这里即视为拖拽激活
            if (drag_.taskId == 0) return;
            drag_.active = true;
            // 跟手副本锚点 = 按下时卡片位置 + 这份累计位移（换算与命中同源，
            // 见 ui::dragGhostAnchor；此处只快照原始位移，换算在 compose）
            drag_.totalX = totalX;
            drag_.totalY = totalY;
            const int target = hitDropColumn(x, y);
            drag_.hoverColIndex = target;
            // 插入行预览：跨列与同列都置位（2026-09-28 真机缺陷 3——同列 =
            // 上下重排，旧「同列拖拽 = 取消」约定已废，条画在悬停缝上）；
            // 搜索态不置位（渲染行 ≠ 全量列行，行号不可用 → 跨列回落
            // 「追加列尾」、同列回落「取消」，与旧语义一致）
            drag_.hoverRow = (target >= 0 && searchQuery_.empty())
                ? hitDropRow(target, x, y)
                : -1;
        },
        .cardReleased = [this](double x, double y, bool released) {
            if (!drag_.active) {
                drag_ = DragState{};  // 普通点击：只清锚点（onClick 已处理）
                return;
            }
            // 跨模块剩余审计修（2026-09-28）：落位淡入不再设标志——列内容容器
            // opacity 直接门控 drag_.active（看板单元内），跟手结束（落库或
            // 取消）active 归 false 的那一帧起 0.6→1.0 平滑淡回
            // 取消分支：drag_ = DragState{} 天然清 active；同列重排（2026-09-28
            // 真机缺陷 3 起为受支持操作）也走 finishDrag 统一收口
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

    // ============ 状态 ============
    std::unique_ptr<TodoService> service_;
    // 智能体工具集（引用 service_，析构顺序：先 detachTools 再 reset service_）
    std::unique_ptr<TodoTools> todoTools_;
    std::vector<Column> columns_;
    std::vector<Task> tasks_;
    // 审计 [23]：列内滚动偏移按列 id 记忆（旧按列索引，重排/删列串列）+
    // 跨会话持久化（viewStore_；viewDirty_ = 有未落盘变更，onStop 收口）
    std::map<int64_t, float> columnScrolls_;
    std::unique_ptr<TodoViewStateStore> viewStore_;
    bool viewDirty_ = false;

    DragState drag_;

    // 板面几何快照（看板绘制单元每帧刷新，拖拽命中换算用）
    ui::BoardPlate boardPlate_;

    // 拖拽 dpi 探针告警只出一次（审计 M4：探针降级为断言；hitDropColumn 是
    // const 且每帧调用，故 mutable + 一次性）
    mutable bool dpiProbeWarned_ = false;

    // 弹窗层状态（打开栈 + 6 弹窗 + 右键菜单 + 逾期数据）
    ui::DialogSet dialogs_;

    // 键盘移动选中态（最近点击的卡片；详情弹窗关闭后保留）
    int64_t selectedTaskId_ = 0;

    // 状态反馈 toast（超时消失；subtitle 保持静态说明；状态位 + 绘制走 ui_kit）
    ui_kit::ToastState toast_;

    // 搜索（三件套 1）：query + service_->search 的命中任务 id 集
    std::string searchQuery_;
    std::unordered_set<int64_t> searchMatchIds_;

    // 逾期视图（三件套 2）：badge 数量（弹窗数据在 dialogs_.overdue）
    size_t overdueCount_ = 0;

    // 数据加载错误（空 = 正常；与空态区分，见看板错误态绘制）
    std::string loadError_;
};

} // namespace module::todo

extern "C" ::core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}
