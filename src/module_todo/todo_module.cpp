// src/module_todo/todo_module.cpp
// Todo 看板模块 - 真实数据 + CRUD（TodoService）
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/component_theme.h"
#include "core/time.h"
#include <ui_kit/dialog_form.h>
#include <ui_kit/toast.h>
#include "todo_service.h"
#include "todo_ui_logic.h"
#include "todo_view_state.h"

#include <eui/app.h>
#include <eui/dsl.h>

#include <components/button.h>
#include <components/contextmenu.h>
#include <components/datepicker.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace module::todo {

namespace {

// ---------- 卡片尺寸 ----------
// 常量定义在 todo_ui_logic.h（拖拽命中换算共用）
using module::todo::ui::kCardHeight;
using module::todo::ui::kCardGap;
using module::todo::ui::kColumnHeaderHeight;

// 优先级 → 中文标签（●/○ 是文本字形，随 chip 前景色调色；
// 旧 🔴🟡🔵⚪ 是系统彩色 emoji，风格不统一已换）
// 下标 = Priority 枚举值（None=0 … High=3）：新增枚举成员时 static_assert
// 编译期失败，不会像原 switch + default 那样静默吞掉漏改。越界（脏数据）走 [0]。
const char* priorityLabel(Priority p) {
    static constexpr const char* kLabels[] = {"无", "●低", "●中", "●高"};
    static_assert(std::size(kLabels) == static_cast<size_t>(Priority::High) + 1);
    const size_t idx = static_cast<size_t>(p);
    return kLabels[idx <= static_cast<size_t>(Priority::High) ? idx : 0];
}

eui::Color priorityColor(Priority p, const core::theme::Tokens& t) {
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

// chip 宽度估算（fontSizeSmall-3 ≈ 11px：CJK ≈ 11px/字（3 字节），ASCII ≈ 5.5px/字节）
// ponytail: 估宽不发 FontRegistry 实测；偏差只影响 chip 间留白，可接受
float chipWidth(const std::string& text) {
    float w = 20.0f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? 4.0f : 5.5f;
    }
    return w;
}

// 改色预设（token 与 ui::columnDotColor 的映射表一一对应；spec §8.2 承诺）
struct ColorPreset {
    const char* token;
    const char* label;
};
const ColorPreset kColorPresets[] = {
    {"claude-clay", "Claude 橘"},
    {"warm-amber",  "暖琥珀"},
    {"muted-sage",  "鼠尾草绿"},
    {"warm-gray",   "暖灰"},
    {"dusty-blue",  "雾蓝"},
    {"rose-red",    "玫瑰红"},
};
constexpr int kColorPresetCount =
    static_cast<int>(sizeof(kColorPresets) / sizeof(kColorPresets[0]));

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
        // 审计 [23]：恢复列滚动（按列 id）+ 选中任务（reloadAll 之后，列/任务已就位）
        viewStore_ = std::make_unique<TodoViewStateStore>(todoViewStateFile());
        restoreViewState();
        LOG_INFO("Todo 模块启动（" + std::to_string(columns_.size()) + " 列 / " +
                 std::to_string(tasks_.size()) + " 任务）");
    }

    void onStop() override {
        if (viewDirty_) saveViewState();  // 审计 [23]：退出前落盘视图状态
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
        if (drag_.active && findTask(drag_.taskId) == nullptr) {
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
            //（overdueTasks_ 保持上次内容，不写入伪空）。
            LOG_ERROR("Todo: 逾期弹窗查询失败: " + r.errorMessage());
            setStatus("逾期任务加载失败：" + r.errorMessage(), true);
            return;
        }
        overdueTasks_ = std::move(r).value();
        overdueCount_ = overdueTasks_.size();
        openPopup(Popup::Overdue);
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
        const int64_t targetCol = findColumnByKind(kind);
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

    // 列标题是否已被占用（exceptId = 改名列自身 id，重命名为自己的
    // 当前名不算撞名；0 = 不排除）。审计 [19]：撞名校验错误经弹窗
    // 内嵌红字呈现（此前重命名空名静默返回、无任何反馈）。
    bool columnTitleTaken(const std::string& title, int64_t exceptId) const {
        for (const auto& c : columns_) {
            if (c.id != exceptId && c.title == title) return true;
        }
        return false;
    }

    std::string doRenameColumn(int64_t id, const std::string& title) {
        if (!service_) return "服务未就绪（TodoService 未注册）";
        if (title.empty()) return "列标题不能为空";
        if (columnTitleTaken(title, id)) return "该列名已存在，请换一个";
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

    // 列内滚动位置（审计 [23]：按列 id 记忆——列重排/删列不再串列；
    // 变更只标脏，onStop 统一落盘，避免每滚轮一格写一次文件）
    float colScrollOffset(size_t colIndex) const {
        if (colIndex >= columns_.size()) return 0.0f;
        auto it = columnScrolls_.find(columns_[colIndex].id);
        return it == columnScrolls_.end() ? 0.0f : it->second;
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
        if (st.selectedTaskId != 0 && findTask(st.selectedTaskId) != nullptr) {
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
        // 正文留空 = 不设正文（本模块 toast 现状即只有标题：组件自带默认正文
        // 文案随之保留，R1 零行为变化）
        toast_.show(msg, "", isError);
    }

    // ---------- Toast（画在弹窗之上，超时消失；状态位 + 绘制接线走 ui_kit） ----------
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        ui_kit::drawToast(ui, screen, "todo.toast", toast_, themeTokens);
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

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

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
        drawNewColumnDialog(ui, screen, t);
        drawDetailDialog(ui, screen, t);
        drawOverdueDialog(ui, screen, t);
        drawContextMenu(ui, screen, t);
        drawDatePicker(ui, screen, t);
        drawRenameDialog(ui, screen, t);
        drawConfirmDeleteDialog(ui, screen, t);
        drawToast(ui, screen,
                  core::theme::componentTokens(t));
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
        if (selectedTaskId_ == 0 || findTask(selectedTaskId_) == nullptr) {
            setStatus("先点击卡片选中任务，再用 Ctrl+↑/↓ 移动", true);
            return false;
        }
        moveTaskByKeyboard(event.key == eui::InputKey::Up);
        return true;
    }

private:
    // ============ 最上层弹窗的单一持有者（审计 THS-05） ============
    // 此前「谁在最上层」由三处各自维护：每个弹窗一个 bool、closeTopmostPopup
    // 手写关闭顺序、绘制时手挑 zIndex——新增一个弹窗要同时改三处，漏改即静默
    // 降级（Esc 关错层 / 层级错乱）。现在只有一个真相源 openStack_：
    //   · 谁最后打开谁在最上（= 栈顶）
    //   · 绘制 zIndex = 栈内位置（不再手挑）
    //   · Esc 关栈顶，因此天然与 z 序一致
    // toast 不入栈：它不是互相竞争层级的弹窗，而是自动消失的状态提示
    //（toast_.open 独立保留）。
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
    // 栈底 z / 每层增量：整栈上界须留在 toast 之下（EUI-NEO toast 默认
    // 1100）——8 个弹窗全叠满也只有 1070，状态提示不会被弹窗盖住
    static constexpr int kPopupBaseZ = 1000;
    static constexpr int kPopupZStep = 10;

    bool isPopupOpen(Popup p) const {
        return std::find(openStack_.begin(), openStack_.end(), p) !=
               openStack_.end();
    }
    void closePopup(Popup p) {
        openStack_.erase(std::remove(openStack_.begin(), openStack_.end(), p),
                         openStack_.end());
    }
    // 打开：已在栈中则提到栈顶（「最后打开的在最上」），否则入栈
    void openPopup(Popup p) {
        closePopup(p);
        openStack_.push_back(p);
    }
    // 组件 onOpenChange 通道（弹窗遮罩点击 / 下拉字段点击）
    void setPopupOpen(Popup p, bool open) {
        if (open) openPopup(p);
        else closePopup(p);
    }
    // 栈内位置 → 绘制 z（栈顶最高）；不在栈中 = 未显示，返回栈底值
    int popupZ(Popup p) const {
        const auto it = std::find(openStack_.begin(), openStack_.end(), p);
        if (it == openStack_.end()) return kPopupBaseZ;
        return kPopupBaseZ +
               kPopupZStep * static_cast<int>(it - openStack_.begin());
    }

    // 审计 M5：Esc 关「最上层」弹窗（= 栈顶，与绘制 z 序同源）。
    // 返回 true = 本键已消费；无弹窗返回 false（按键继续走既有路径）
    bool closeTopmostPopup() {
        if (openStack_.empty()) return false;
        const Popup top = openStack_.back();
        if (top == Popup::Detail) {
            // 未存改动守卫：第一次只警告不关（栈保持不动，再按一次才真关）
            requestCloseDetailDialog();
            return true;
        }
        closePopup(top);
        return true;
    }

    // 详情弹窗关闭统一入口（审计 M5：Esc 兜底与 backdrop 点击共用同一份守卫，
    // 避免两处各写一遍「未存改动先警告」而语义漂移）。
    // 返回 true = 本次真的关了；false = 被未存改动守卫拦下（弹窗仍在栈顶）
    bool requestCloseDetailDialog() {
        // 产品审计 2026-09-23 [2]：有未存改动时第一次只警告不关，再触发一次
        // = 明确放弃修改才关。EUI-NEO dialog 无 dismissable 开关（中期上游补），
        // 两击语义是当前组件能力下的守卫
        if (detailFormDirty() && !detailDiscardArmed_) {
            detailDiscardArmed_ = true;
            setStatus("有未保存修改：点「保存」落库，或再点一次遮罩放弃", true);
            return false;
        }
        detailDiscardArmed_ = false;
        closePopup(Popup::Detail);
        return true;
    }

    // ---------- 顶部 Header ----------
    // 右侧动作区 560px：搜索框(240) + 逾期入口(104) + 「+ 列」(72) + 「+ 新建任务」(120) + 间距
    void drawHeader(eui::Ui& ui, float contentW, float headerH, float bodyH,
                    const core::theme::Tokens& t) {
        (void)bodyH;
        const auto componentTheme = core::theme::componentTokens(t);
        ui.row("todo.header")
            .size(contentW, headerH)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                ui.column("todo.title")
                    .size(contentW - 560.0f, headerH)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("todo.h1")
                            .size(contentW - 560.0f, 32.0f)
                            .text("Todo 看板")
                            .fontSize(t.sizes.fontSizeTitle)
                            .lineHeight(32.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        // subtitle 保持静态操作说明（动态反馈走 toast）；
                        // 包 clip 栈：41 字文案实宽 ≈545px，窄窗（≲1400）会压到
                        // 右侧搜索框/按钮（同 md.subtitle.clip 写法）
                        ui.stack("todo.subtitle.clip")
                            .size(contentW - 560.0f, 18.0f)
                            .clip()
                            .content([&] {
                                ui.text("todo.subtitle")
                                    .size(contentW - 560.0f, 18.0f)
                                    .text("点击卡片查看详情 · Ctrl+↑/↓ 移动选中任务 · 右键卡片/列头更多操作 · 拖动卡片跨列移动")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();
                    })
                    .build();

                ui.row("todo.actions")
                    .size(560.0f, 40.0f)
                    .justifyContent(eui::Align::END)
                    .alignItems(eui::Align::CENTER)
                    .gap(t.sizes.spacingSmall)
                    .content([&] {
                        // 搜索框（真实调用 service_->search；仿 markdown 模块 md.search）
                        // 审计 [10]：Enter 改无副作用（不绑 onEnter，与 md.search 一致——
                        // 搜索已即时生效，回车不该清空）；Esc 清除走 onCancel（[21] 路径，
                        // 不再依赖 Escape→onEnter 旧映射）。
                        // 🔍 emoji 换 FA：包一层 stack 才能在流式 row 里叠 icon 浮层
                        ui.stack("todo.search.wrap")
                            .size(240.0f, 40.0f)
                            .content([&] {
                                components::input(ui, "todo.search")
                                    .size(240.0f, 40.0f)
                                    .value(searchQuery_)
                                    .placeholder("搜索任务…")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .inset(30.0f)
                                    .theme(componentTheme)
                                    .onChange([this](const std::string& v) {
                                        searchQuery_ = v;
                                        refreshSearch();
                                    })
                                    .onCancel([this] {
                                        // 审计 [10]：清除只挂 Esc（onCancel），Enter 无副作用
                                        if (!searchQuery_.empty()) {
                                            searchQuery_.clear();
                                            refreshSearch();
                                        }
                                    })
                                    .build();
                                ui.text("todo.search.icon")
                                    .position(14.0f, 12.0f)
                                    .size(16.0f, 16.0f)
                                    .icon(0xF002)  // fa-magnifying-glass
                                    .fontSize(12.0f)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                            })
                            .build();

                        // 逾期入口（badge = listOverdue().size()，0 时置灰不可点；
                        // ⚠ emoji 换 fa-triangle-exclamation，0 时随 disabled 自动置灰）
                        const bool hasOverdue = overdueCount_ > 0;
                        components::button(ui, "todo.btn.overdue")
                            .size(104.0f, 40.0f)
                            .text("逾期 " + std::to_string(overdueCount_))
                            .icon(0xF071)
                            .fontSize(t.sizes.fontSizeSmall)
                            .colors(hasOverdue ? t.colors.error : t.colors.surfaceElevated,
                                    hasOverdue ? t.colors.error : t.colors.hover,
                                    hasOverdue ? t.colors.error : t.colors.active)
                            .textColor(hasOverdue ? t.colors.onPrimary
                                                  : t.colors.textDisabled)
                            .iconColor(hasOverdue ? t.colors.onPrimary
                                                  : t.colors.textDisabled)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .onClick([this] {
                                if (overdueCount_ > 0) openOverdueDialog();
                            })
                            .build();

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
                                // 审计 [24]：不再即时 doCreateColumn("新列 N") 落库，
                                // 先走命名弹窗，与全应用创建范式统一
                                openNewColumnDialog();
                            })
                            .build();

                        // + 新建任务
                        components::button(ui, "todo.btn.add")
                            .size(120.0f, 40.0f)
                            .text("+ 新建任务")
                            .fontSize(t.sizes.fontSizeSmall)
                            .colors(t.colors.primary, t.colors.primaryHover,
                                    t.colors.primaryPressed)
                            .textColor(t.colors.onPrimary)
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
                        .textColor(t.colors.onPrimary)
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
            // 看板列容器是页面结构 → background（米色/暖墨），卡片白只给
            // 列内浮动卡片与按钮（主题层级修正，同侧边栏根因）
            .color(t.colors.background)
            .radius(t.sizes.radiusLarge)
            .border(dropHover ? 2.0f : 1.0f,
                    dropHover ? t.colors.primary : t.colors.borderDefault)
            .build();

        auto colTasks = tasksInColumn(col.id);
        // 搜索激活时只显示命中卡（命中列照常渲染；命中集来自 service_->search）
        colTasks = ui::filterBySearch(colTasks, searchMatchIds_,
                                      !searchQuery_.empty());
        // 归档列语义走 kind（migration v8 起回填 'archive'）：按标题判会在
        // 用户重命名后静默失效
        const bool isArchive = (col.kind == "archive");

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

        // 列颜色点（Column::color 入库后首次渲染；10px 圆点，token→hex 见 ui::columnDotColor）
        const auto colDot = ui::columnDotColor(col.color);
        if (colDot.has_value()) {
            ui.rect(id + ".dot")
                .position(16.0f, 21.0f)
                .size(10.0f, 10.0f)
                .color(*colDot)
                .radius(5.0f)
                .build();
        }

        ui.text(id + ".title")
            .position(colDot.has_value() ? 34.0f : 16.0f, 16.0f)
            // 右侧留到 width-88：给 ✎ 重命名（width-70 起）和 + 加任务（width-36 起）两个按钮
            .size(width - 88.0f - (colDot.has_value() ? 18.0f : 0.0f), 20.0f)
            .text(col.title + " (" + std::to_string(colTasks.size()) + ")")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        // 列重命名入口显性化（审计 [24]）：列头可见 ✎ 按钮直达改名弹窗，
        // 右键菜单「重命名列」入口保留
        components::button(ui, id + ".rename")
            .position(width - 70.0f, 14.0f)
            .size(28.0f, 28.0f)
            .icon(0xF303)  // fa-pencil-alt（对称铅笔；原 fa-edit 0xf044 字形右重偏右，换之）
            .iconSize(t.sizes.fontSizeSmall)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .iconColor(t.colors.textSecondary)
            .radius(14.0f)
            .onClick([this, col] { openRenameDialog(col); })
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
                .text(searchQuery_.empty()
                          ? (isArchive ? "归档的任务在这里" : "暂无任务")
                          : "无命中任务")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            return;
        }

        // 列内滚动（scrollView）：此前直接裁掉超出列高的卡片（第 12 张起消失）。
        // 滚动位置按列索引记忆（reloadAll 时 vector resize 保留旧值）。
        const auto componentTheme = core::theme::componentTokens(t);
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
            // 卡片 = surface 层（浅色白 / 深色 #232120，spec §6.5.1 卡片色），
            // 浮在列容器（background）之上；曾用 surfaceElevated 抬高了一级
            .states(t.colors.surface, t.colors.hover, t.colors.active)
            .radius(t.sizes.radius)
            .opacity(cardOpacity)
            .border(selected ? 2.0f : 0.0f,
                    selected ? t.colors.primary : t.colors.borderDefault)
            .onClick([this, task] {
                // 拖拽后松手不触发 click 由框架 InteractionState.dragMoved 语义
                // 保障（挂了 onDrag 的元素超阈值拖动后不再派发 click），本地
                // 抑制逻辑已删（上游能力采用）
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
                // tech-debt B：bounds.y 里含的是"按下这一帧"的列滚动偏移，
                // 命中换算必须减同一个值才能还原出正确的板面顶边。拖拽途中
                // 源列被滚轮滚动时，若 onDrag 时刻再读 colScrollOffset（新值），
                // pressY（旧帧）× scrollOffset（新帧）混帧 → 命中框整体上偏/下偏。
                // 在按下回调内快照，保证两值同帧。
                drag_.pressScrollOffset =
                    colScrollOffset(static_cast<size_t>(colIndex));
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

        // 截止 chip（⏰ emoji 已去掉：逾期时 chip 前景直接用 error 色示意）
        if (task.dueAt.has_value()) {
            const std::string dueText = formatDue(task.dueAt.value());
            drawChip(ui, id + ".due", 70.0f, y + 44.0f, 66.0f, t, dueText,
                     isOverdue(task) ? t.colors.error : t.colors.textTertiary,
                     t.colors.surface, cardOpacity);
        }

        // tags chip（前 2 个 + 超出 +N；完整编辑在详情弹窗）
        if (!task.tags.empty()) {
            float tagX = task.dueAt.has_value() ? 146.0f : 70.0f;
            size_t shown = 0;
            for (size_t ti = 0; ti < task.tags.size() && shown < 2; ++ti) {
                const float tw = chipWidth(task.tags[ti]);
                if (tagX + tw > width - 12.0f) break;
                drawChip(ui, id + ".tag." + std::to_string(ti), tagX, y + 44.0f,
                         tw, t, task.tags[ti], t.colors.info, t.colors.surface,
                         cardOpacity);
                tagX += tw + 6.0f;
                ++shown;
            }
            if (task.tags.size() > shown) {
                const std::string more = "+" + std::to_string(task.tags.size() - shown);
                const float moreW = chipWidth(more);
                // 放不下就不画（旧代码无条件画，chip 右缘可越出卡片 ~19px）
                if (tagX + moreW <= width - 12.0f) {
                    drawChip(ui, id + ".tag.more", tagX, y + 44.0f, moreW, t,
                             more, t.colors.textTertiary, t.colors.surface, cardOpacity);
                }
            }
        }

        // 完成打勾
        if (done) {
            ui.text(id + ".done")
                .position(width - 36.0f, y + 14.0f)
                .size(24.0f, 18.0f)
                .icon(0xF00C)  // fa-check（✓ 字符混排正文不可行，走 icon 通道）
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
        newTaskError_.clear();   // 审计 [19]：打开清（claude formError_ 模式）
        openPopup(Popup::NewTask);
    }

    // 审计 [19]：提交失败（空标题/服务错误）→ 弹窗内嵌红字、弹窗不关；
    // 成功 → 关窗 + toast 确认。此前「先关弹窗再报 toast」，反馈隔在窗外。
    void submitNewTask() {
        const int64_t col = newTaskColumnId_;
        const std::string title = newTaskTitle_;
        const std::string err = doCreateTask(col, title);
        if (!err.empty()) {
            newTaskError_ = err;
            return;
        }
        closePopup(Popup::NewTask);
    }

    void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 230.0f;

        ui_kit::DialogFrame frame;
        frame.id = "todo.dialog.newtask";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = isPopupOpen(Popup::NewTask);
        frame.zIndex = popupZ(Popup::NewTask);
        frame.onOpenChange = [this](bool v) { setPopupOpen(Popup::NewTask, v); };

        ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
                ui_kit::drawDialogTitle(ui, "todo.dialog.newtask.title", w, "新建任务",
                                        t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

                // 目标列提示
                std::string colName = "?";
                for (const auto& c : columns_) {
                    if (c.id == newTaskColumnId_) colName = c.title;
                }
                ui_kit::drawDialogText(ui, "todo.dialog.newtask.col", w,
                                       "将创建在「" + colName + "」列",
                                       t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                                       {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

                components::input(ui, "todo.dialog.newtask.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newTaskTitle_)
                    .placeholder("任务标题…")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) {
                        newTaskTitle_ = v;
                        newTaskError_.clear();  // 审计 [19]：错误随下一次输入清除
                    })
                    .onEnter([this] { submitNewTask(); })
                    // 审计 [21]：Esc=取消（关弹窗不落库，同「取消」按钮）
                    .onCancel([this] { closePopup(Popup::NewTask); })
                    .build();

                // 审计 [19]：内嵌红字错误行（照抄 claude formError_ 渲染——
                // 校验/创建失败就地可见，弹窗不关）
                ui_kit::drawDialogText(ui, "todo.dialog.newtask.error", w, newTaskError_,
                                       t.sizes.fontSizeSmall, t.colors.error,
                                       {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                        .skipIfEmpty = true});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
                cancel.style = ui_kit::DialogButtonStyle::Secondary;
                cancel.onClick = [this] { closePopup(Popup::NewTask); };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "创建";
                confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
                confirm.style = ui_kit::DialogButtonStyle::Primary;
                confirm.onClick = [this] { submitNewTask(); };
                ui_kit::drawDialogButtons(ui, "todo.dialog.newtask", w, h, cancel, confirm,
                                          t, themeTokens);
        });
    }

    // ---------- 新建列弹窗（审计 [24]） ----------
    // 「+ 列」不再即时落库，先走命名弹窗，与任务/笔记/供应商 Profile 的
    // 创建范式统一。审计建议「预填 新列 N 并全选」——EUI-NEO Input 无
    // 程序化 focus/selectAll 接口，全选预填产品层做不了；改用与新建任务
    // 弹窗同款等价实现：空值 + placeholder 显示默认名，确认时留空=采用
    // 「新列 N」、输入名字=按名字建，两条路径用户都无需额外编辑动作。

    void openNewColumnDialog() {
        newColumnTitle_.clear();
        newColumnError_.clear();   // 审计 [19]：打开清（claude formError_ 模式）
        openPopup(Popup::NewColumn);
    }

    std::string defaultColumnTitle() const {
        return "新列 " + std::to_string(columns_.size() + 1);
    }

    // 审计 [19]：显式输入的列名撞名 / 创建失败 → 弹窗内嵌红字，弹窗不关。
    // 留空采用默认名「新列 N」的便利路径不查重（默认名是序号兜底，撞名
    // 也不该把用户困在弹窗里）。失败仍关闭的话与「关弹窗+toast」旧通道同罪。
    void confirmNewColumn() {
        const bool defaulted = newColumnTitle_.empty();
        const std::string title = defaulted ? defaultColumnTitle() : newColumnTitle_;
        if (!defaulted && columnTitleTaken(title, 0)) {
            newColumnError_ = "该列名已存在，请换一个";
            return;
        }
        const std::string err = doCreateColumn(title);
        if (!err.empty()) {
            newColumnError_ = err;
            return;
        }
        closePopup(Popup::NewColumn);
    }

    void drawNewColumnDialog(eui::Ui& ui, const eui::Screen& screen,
                             const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 230.0f;

        ui_kit::DialogFrame frame;
        frame.id = "todo.dialog.newcol";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = isPopupOpen(Popup::NewColumn);
        frame.zIndex = popupZ(Popup::NewColumn);
        frame.onOpenChange = [this](bool v) { setPopupOpen(Popup::NewColumn, v); };

        ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
                ui_kit::drawDialogTitle(ui, "todo.dialog.newcol.title", w, "新建列",
                                        t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

                ui_kit::drawDialogText(ui, "todo.dialog.newcol.hint", w,
                                       "输入列标题，留空回车则用「" + defaultColumnTitle() + "」",
                                       t.sizes.fontSizeSmall - 1.0f, t.colors.textTertiary,
                                       {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

                components::input(ui, "todo.dialog.newcol.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newColumnTitle_)
                    .placeholder(defaultColumnTitle())
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) {
                        newColumnTitle_ = v;
                        newColumnError_.clear();  // 审计 [19]：错误随下一次输入清除
                    })
                    .onEnter([this] { confirmNewColumn(); })
                    // 审计 [21]：Esc=取消走 onCancel（原 per-dialog onKeyEvent
                    // passthrough 拦截 hack 已由上游 onCancel 取代移除）。
                    // 输入框未聚焦的兜底走 app 级 Module::onKeyEvent（见下）。
                    .onCancel([this] { closePopup(Popup::NewColumn); })
                    .build();

                // 审计 [19]：内嵌红字错误行（claude formError_ 同款渲染）
                ui_kit::drawDialogText(ui, "todo.dialog.newcol.error", w, newColumnError_,
                                       t.sizes.fontSizeSmall, t.colors.error,
                                       {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                        .skipIfEmpty = true});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
                cancel.style = ui_kit::DialogButtonStyle::Secondary;
                cancel.onClick = [this] { closePopup(Popup::NewColumn); };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "创建";
                confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
                confirm.style = ui_kit::DialogButtonStyle::Primary;
                confirm.onClick = [this] { confirmNewColumn(); };
                ui_kit::drawDialogButtons(ui, "todo.dialog.newcol", w, h, cancel, confirm,
                                          t, themeTokens);
        });
    }

    // ---------- 右键菜单 ----------
    // 菜单定位走模块自身布局空间（boardGeom），不用窗口坐标（见 BoardGeom 注释）
    void openTaskContextMenu(const Task& task, int columnIndex, int cardIndex) {
        selectedTaskId_ = task.id;
        saveViewState();  // 审计 [23]：选中任务离散变更，即时落盘
        ctxTaskId_ = task.id;
        ctxColumnId_ = 0;
        ctxColumnIndex_ = columnIndex;
        ctxCardIndex_ = cardIndex;
        openPopup(Popup::Ctx);
    }

    void openColumnContextMenu(const Column& col, int columnIndex) {
        ctxTaskId_ = 0;
        ctxColumnId_ = col.id;
        ctxColumnIndex_ = columnIndex;
        ctxCardIndex_ = -1;
        openPopup(Popup::Ctx);
    }

    void drawContextMenu(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
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
            // 改色子选（spec §8.2 承诺；预设与 ui::columnDotColor 映射表对应）
            std::vector<components::ContextMenuItem> colorItems;
            for (int i = 0; i < kColorPresetCount; ++i) {
                colorItems.push_back({kColorPresets[i].label});
            }
            items.push_back({"改色", std::move(colorItems)});
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
            .open(isPopupOpen(Popup::Ctx))
            .screen(screen.width, screen.height)
            .position(x, y)
            .size(menuW, 36.0f)
            .items(std::move(items))
            .theme(themeTokens)
            .zIndex(popupZ(Popup::Ctx))
            .onSelectPath([this](const std::vector<int>& path) { onContextSelectPath(path); })
            .onOpenChange([this](bool v) { setPopupOpen(Popup::Ctx, v); })
            .build();
    }

    // 菜单项点击分发（路径版，兼容「改色」子菜单）。
    // 任务菜单无子级，path[0] 即原索引；列菜单：0=重命名 1=左移 2=右移
    //   3=改色（path[1] = 预设索引）4=删除
    void onContextSelectPath(const std::vector<int>& path) {
        if (ctxTaskId_ != 0) {
            if (!path.empty()) onContextSelect(path[0]);
            return;
        }
        if (path.size() == 2 && path[0] == 3) {
            const Column* col = findColumn(ctxColumnId_);
            if (col != nullptr && path[1] >= 0 && path[1] < kColorPresetCount) {
                doSetColumnColor(col->id, kColorPresets[path[1]].token);
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
                openPopup(Popup::Confirm);
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
                openRenameDialog(*col);
                break;
            case 1: doShiftColumn(col->id, -1); break;
            case 2: doShiftColumn(col->id, +1); break;
            case 3:
                confirmColumnId_ = col->id;
                confirmTaskId_ = 0;
                openPopup(Popup::Confirm);
                break;
            default: break;
        }
    }

    // ---------- 重命名列弹窗 ----------
    // 打开列重命名弹窗（右键菜单「重命名列」与列头 ✎ 按钮共用，审计 [24]）
    void openRenameDialog(const Column& col) {
        renameColumnId_ = col.id;
        renameTitle_ = col.title;
        renameError_.clear();   // 审计 [19]：打开清（claude formError_ 模式）
        openPopup(Popup::Rename);
    }

    // 审计 [19]：重命名提交——空名此前静默返回（弹窗已关、零反馈），
    // 撞名此前根本不校验；统一为弹窗内嵌红字 + 弹窗不关。
    void submitRenameColumn() {
        const int64_t id = renameColumnId_;
        const std::string title = renameTitle_;
        const std::string err = doRenameColumn(id, title);
        if (!err.empty()) {
            renameError_ = err;
            return;
        }
        closePopup(Popup::Rename);
    }

    void drawRenameDialog(eui::Ui& ui, const eui::Screen& screen,
                          const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
        const float h = 210.0f;

        ui_kit::DialogFrame frame;
        frame.id = "todo.dialog.rename";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = isPopupOpen(Popup::Rename);
        frame.zIndex = popupZ(Popup::Rename);
        frame.onOpenChange = [this](bool v) { setPopupOpen(Popup::Rename, v); };

        ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
                ui_kit::drawDialogTitle(ui, "todo.dialog.rename.title", w, "重命名列",
                                        t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

                components::input(ui, "todo.dialog.rename.input")
                    .position(28.0f, 68.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(renameTitle_)
                    .placeholder("列标题…")
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) {
                        renameTitle_ = v;
                        renameError_.clear();  // 审计 [19]：错误随下一次输入清除
                    })
                    .onEnter([this] { submitRenameColumn(); })
                    // 审计 [21]：Esc=取消（关弹窗不改名，同「取消」按钮）
                    .onCancel([this] { closePopup(Popup::Rename); })
                    .build();

                // 审计 [19]：内嵌红字错误行（claude formError_ 同款渲染）
                ui_kit::drawDialogText(ui, "todo.dialog.rename.error", w, renameError_,
                                       t.sizes.fontSizeSmall, t.colors.error,
                                       {.y = 118.0f, .height = 18.0f, .lineHeight = 18.0f,
                                        .skipIfEmpty = true});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
                cancel.style = ui_kit::DialogButtonStyle::Secondary;
                cancel.onClick = [this] { closePopup(Popup::Rename); };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "保存";
                confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
                confirm.style = ui_kit::DialogButtonStyle::Primary;
                confirm.onClick = [this] { submitRenameColumn(); };
                ui_kit::drawDialogButtons(ui, "todo.dialog.rename", w, h, cancel, confirm,
                                          t, themeTokens);
        });
    }

    // ---------- 删除确认弹窗 ----------
    // 双模式：confirmTaskId_ > 0 = 删任务；否则 confirmColumnId_ > 0 = 删列
    // （外键 CASCADE 会连带删掉列下全部任务）
    void drawConfirmDeleteDialog(eui::Ui& ui, const eui::Screen& screen,
                                 const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
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

        ui_kit::DialogFrame frame;
        frame.id = "todo.dialog.delcol";
        frame.width = ui_kit::DialogWidth::Standard;
        frame.height = h;
        frame.open = isPopupOpen(Popup::Confirm);
        // 审计 2026-09-23 [2]：从详情弹窗发起的删除确认叠在详情之上
        //（z 由打开顺序给出，见 popupZ）
        frame.zIndex = popupZ(Popup::Confirm);
        frame.onOpenChange = [this](bool v) { setPopupOpen(Popup::Confirm, v); };

        ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
                ui_kit::drawDialogTitle(ui, "todo.dialog.delcol.title", w, title,
                                        t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);
                ui_kit::drawDialogText(ui, "todo.dialog.delcol.body", w, body,
                                       t.sizes.fontSizeBody - 1.0f, t.colors.textSecondary,
                                       {.y = 60.0f, .height = 40.0f, .lineHeight = 20.0f});

                ui_kit::DialogButtonSpec cancel;
                cancel.label = "取消";
                cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
                cancel.style = ui_kit::DialogButtonStyle::Secondary;
                cancel.onClick = [this] { closePopup(Popup::Confirm); };
                ui_kit::DialogButtonSpec confirm;
                confirm.label = "删除";
                confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
                confirm.style = ui_kit::DialogButtonStyle::Danger;
                confirm.onClick = [this] {
                        if (confirmTaskId_ != 0) {
                            const int64_t taskId = confirmTaskId_;
                            closePopup(Popup::Confirm);
                            confirmTaskId_ = 0;
                            // 详情弹窗发起的删除：确认后才关详情（取消路径
                            // 不关，回到弹窗——审计 2026-09-23 [2]）
                            closePopup(Popup::Detail);
                            detailDiscardArmed_ = false;
                            doDeleteTask(taskId);
                            return;
                        }
                        const int64_t id = confirmColumnId_;
                        closePopup(Popup::Confirm);
                        doDeleteColumn(id);
                    };
                ui_kit::drawDialogButtons(ui, "todo.dialog.delcol", w, h, cancel, confirm,
                                          t, themeTokens);
        });
    }

    // ---------- 截止日期选择（EUI-NEO datepicker 滚轮，替代 4 个固定预设） ----------
    void drawDatePicker(eui::Ui& ui, const eui::Screen& screen,
                        const core::theme::Tokens& t) {
        if (!isPopupOpen(Popup::DatePick)) return;
        const auto themeTokens = core::theme::componentTokens(t);
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
            .zIndex(popupZ(Popup::DatePick))  // 由打开顺序给出（详情之上）
            .onChange([this](int yy, int mm, int dd) {
                detailDueAt_ = ui::ymdToEpoch(yy, mm, dd);
            })
            .onOpenChange([this](bool v) { setPopupOpen(Popup::DatePick, v); })
            .build();
    }

    // ---------- 逾期任务弹窗（三件套 2） ----------
    // 数据源 service_->listOverdue()（openOverdueDialog 时刷新）；
    // 行点击 = 关闭本弹窗并打开该任务详情
    void drawOverdueDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);
        const float rowH = 44.0f;
        constexpr float kMaxDialogH = 640.0f;
        // 行数按封顶高度截断（终验复审：≥13 条时高度封顶 640 但行仍全量渲染，
        // 第 13 条起画到弹窗外；标题保留总数，提示行标注截断）
        const size_t maxRows =
            static_cast<size_t>((kMaxDialogH - 96.0f) / rowH);  // = 12
        const size_t shown = std::min(overdueTasks_.size(), maxRows);
        const float h =
            std::min(kMaxDialogH, 96.0f + rowH * static_cast<float>(shown));

        ui_kit::DialogFrame frame;
        frame.id = "todo.dialog.overdue";
        frame.width = ui_kit::DialogWidth::Wide;
        frame.height = h;
        frame.open = isPopupOpen(Popup::Overdue);
        frame.zIndex = popupZ(Popup::Overdue);
        frame.onOpenChange = [this](bool v) { setPopupOpen(Popup::Overdue, v); };

        ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
                ui.text("todo.dialog.overdue.title.icon")
                    .position(28.0f, 20.0f)
                    .size(18.0f, 28.0f)
                    .icon(0xF071)  // fa-triangle-exclamation（⚠ 字符换 FA）
                    .fontSize(t.sizes.fontSizeLarge + 2.0f)
                    .lineHeight(28.0f)
                    .color(t.colors.error)
                    .build();
                ui_kit::drawDialogTitle(
                    ui, "todo.dialog.overdue.title", w,
                    "逾期任务（" + std::to_string(overdueTasks_.size()) + "）",
                    t.sizes.fontSizeLarge + 2.0f, t.colors.error,
                    {.x = 50.0f, .y = 20.0f, .inset = 78.0f});
                std::string hint = "任务名 · 所属列 · 截止 · 逾期天数，点击行跳转详情";
                if (overdueTasks_.size() > shown) {
                    hint += "（仅显示前 " + std::to_string(shown) + " 条）";
                }
                ui_kit::drawDialogText(ui, "todo.dialog.overdue.hint", w, hint,
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 50.0f, .height = 16.0f, .lineHeight = 16.0f});

                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                for (size_t i = 0; i < shown; ++i) {
                    const Task& task = overdueTasks_[i];
                    const float y = 74.0f + static_cast<float>(i) * rowH;
                    const std::string rowId =
                        "todo.dialog.overdue.row." + std::to_string(i);
                    const Column* col = findColumn(task.columnId);
                    ui.rect(rowId)
                        .position(20.0f, y)
                        .size(w - 40.0f, rowH - 6.0f)
                        .states(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                        .radius(t.sizes.radius)
                        .onClick([this, taskId = task.id] {
                            closePopup(Popup::Overdue);
                            if (const Task* task = findTask(taskId)) {
                                openDetailDialog(*task);
                            }
                        })
                        .build();
                    ui.text(rowId + ".title")
                        .position(32.0f, y + 10.0f)
                        .size(w - 300.0f, 20.0f)
                        .text(task.title)
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .fontWeight(500)
                        .color(t.colors.textPrimary)
                        .build();
                    ui.text(rowId + ".col")
                        .position(w - 262.0f, y + 10.0f)
                        .size(120.0f, 20.0f)
                        .text(col != nullptr ? col->title : "?")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.textSecondary)
                        .build();
                    ui.text(rowId + ".due")
                        .position(w - 136.0f, y + 10.0f)
                        .size(60.0f, 20.0f)
                        .text(task.dueAt.has_value() ? formatDue(task.dueAt.value())
                                                     : "-")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.textSecondary)
                        .build();
                    ui.text(rowId + ".days")
                        .position(w - 68.0f, y + 10.0f)
                        .size(48.0f, 20.0f)
                        // 入选按秒级（listOverdue 的 due_at < now），日历日差却
                        // 可能为 0（1 秒前到期）→ 行文案至少 1 天，与「已逾期」一致
                        .text("逾期" + std::to_string(
                                   std::max(1, ui::overdueDays(task.dueAt.value_or(0), now))) + "天")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.error)
                        .build();
                }
        });
    }

    // ---------- 任务详情 / 编辑弹窗 ----------
    void openDetailDialog(const Task& task) {
        selectedTaskId_ = task.id;  // 选中态：详情弹窗关闭后保留，供 Ctrl+↑/↓
        saveViewState();            // 审计 [23]：同上，即时落盘
        detailTaskId_ = task.id;
        detailColumnId_ = task.columnId;
        detailTitle_ = task.title;
        detailDescription_ = task.description.value_or("");
        detailPriority_ = task.priority;
        detailDueAt_ = task.dueAt;
        detailCompleted_ = task.completedAt.has_value();
        detailTags_ = task.tags;
        detailTagInput_.clear();
        detailDiscardArmed_ = false;
        openPopup(Popup::Detail);
    }

    // 标签输入回车 → 追加（去空白 / 去重 / 空忽略；保存时才落库）
    void addDetailTag() {
        std::string tag;
        for (char c : detailTagInput_) {
            if (c != ' ' && c != '\t') tag += c;
        }
        detailTagInput_.clear();
        if (tag.empty()) return;
        for (const auto& existing : detailTags_) {
            if (existing == tag) return;
        }
        detailTags_.push_back(std::move(tag));
    }

    // ---------- 详情弹窗表单落库辅助（产品审计 2026-09-23 [2]） ----------
    // 「保存」「移动到」「标记完成」「取消完成」共用同一份表单组装，
    // 动作按钮先落表单再执行动作，编辑中的改动不再被静默丢弃。

    // 表单字段 → Task（含 position 保留；completedAt 取当前完成态）
    Task collectDetailFormTask() const {
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
        return updated;
    }

    // 表单原样落库；false = 保存失败（已 setStatus 报错），调用方应保持
    // 弹窗打开并放弃后续动作
    bool saveDetailForm() {
        return doUpdateTask(collectDetailFormTask());
    }

    // 表单与库中任务是否有差异（backdrop 关闭守卫用）。任务不在内存
    //（已被别的路径删掉）时无法比较，按不脏处理
    bool detailFormDirty() const {
        const Task* orig = findTask(detailTaskId_);
        if (orig == nullptr) return false;
        return orig->title != detailTitle_ ||
               orig->description.value_or("") != detailDescription_ ||
               orig->priority != detailPriority_ ||
               orig->dueAt != detailDueAt_ ||
               orig->columnId != detailColumnId_ ||
               orig->completedAt.has_value() != detailCompleted_ ||
               orig->tags != detailTags_;
    }

    void drawDetailDialog(eui::Ui& ui, const eui::Screen& screen,
                          const core::theme::Tokens& t) {
        const auto themeTokens = core::theme::componentTokens(t);
        const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);
        // 「移动到」全列遍历 + 可换行：每行 4 个按钮，列多时增高弹窗
        // （此前硬编码 i < 4，第 5 列起在详情弹窗里移不过去）
        const float moveBtnW = 118.0f;
        const float moveGap = 8.0f;
        const int perRow = std::max(
            1, static_cast<int>((w - 56.0f + moveGap) / (moveBtnW + moveGap)));
        const int moveRows = static_cast<int>(
            (columns_.size() + static_cast<size_t>(perRow) - 1) / perRow);
        // 640 = 原布局 + 标签编辑区（chips 行 + 输入框，见下方「标签」段）
        const float h = 640.0f + 40.0f * static_cast<float>(std::max(0, moveRows - 1));

        ui_kit::DialogFrame frame;
        frame.id = "todo.dialog.detail";
        frame.width = ui_kit::DialogWidth::Wide;
        frame.height = h;
        frame.open = isPopupOpen(Popup::Detail);
        frame.zIndex = popupZ(Popup::Detail);
        // 产品审计 2026-09-23 [2]：backdrop 点击（dialog 唯一外部关闭
        // 通道）→ 与 Esc 兜底共用同一份守卫（requestCloseDetailDialog）
        frame.onOpenChange = [this](bool v) {
            if (!v) requestCloseDetailDialog();
        };

        ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
                ui_kit::drawDialogTitle(ui, "todo.dialog.detail.title", w, "任务详情",
                                        t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary,
                                        {.y = 22.0f});

                // 标题
                ui_kit::drawDialogText(ui, "todo.dialog.detail.tlabel", w, "标题",
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 58.0f, .width = 80.0f, .height = 16.0f,
                                        .lineHeight = 16.0f});
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
                ui_kit::drawDialogText(ui, "todo.dialog.detail.dlabel", w, "描述",
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 126.0f, .width = 80.0f, .height = 16.0f,
                                        .lineHeight = 16.0f});
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
                ui_kit::drawDialogText(ui, "todo.dialog.detail.plabel", w, "优先级",
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 244.0f, .width = 80.0f, .height = 16.0f,
                                        .lineHeight = 16.0f});
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
                        .textColor(active ? t.colors.primaryText : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i, prios] { detailPriority_ = prios[i]; })
                        .build();
                }

                // 截止日期：清除 + datepicker 滚轮选任意日期（原 4 个固定预设已替换）
                ui_kit::drawDialogText(ui, "todo.dialog.detail.dlabel2", w, "截止日期",
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 308.0f, .width = 120.0f, .height = 16.0f,
                                        .lineHeight = 16.0f});
                const bool hasDue = detailDueAt_.has_value();
                components::button(ui, "todo.dialog.detail.due.clear")
                    .position(28.0f, 328.0f)
                    .size(100.0f, 32.0f)
                    .text(!hasDue ? "未设置" : "清除")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(!hasDue ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                            t.colors.hover, t.colors.active)
                    .textColor(!hasDue ? t.colors.primaryText : t.colors.textSecondary)
                    .radius(t.sizes.radius)
                    .border(1.0f, !hasDue ? t.colors.primary : t.colors.borderDefault)
                    .onClick([this] { detailDueAt_ = std::nullopt; })
                    .build();
                // 📅 emoji 换 fa-calendar（码点已对 EUI-NEO FA7 cmap 离线核对）
                components::button(ui, "todo.dialog.detail.due.pick")
                    .position(136.0f, 328.0f)
                    .size(180.0f, 32.0f)
                    .text(hasDue ? formatDue(detailDueAt_.value()) : "选择日期…")
                    .icon(0xF133)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(hasDue ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                            t.colors.hover, t.colors.active)
                    .textColor(hasDue ? t.colors.primaryText : t.colors.textSecondary)
                    .radius(t.sizes.radius)
                    .border(1.0f, hasDue ? t.colors.primary : t.colors.borderDefault)
                    .onClick([this] { openPopup(Popup::DatePick); })
                    .build();

                // 标签（三件套 3）：chip × 删除 + 输入回车添加；保存时随
                // detailTags_ 走 updateTask 的 JSON tags 序列化（W9 已确认往返）
                ui_kit::drawDialogText(ui, "todo.dialog.detail.taglabel", w, "标签",
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 372.0f, .width = 120.0f, .height = 16.0f,
                                        .lineHeight = 16.0f});
                {
                    float chipX = 28.0f;
                    for (size_t ti = 0; ti < detailTags_.size(); ++ti) {
                        const float tw = chipWidth(detailTags_[ti]);
                        // ponytail: 单行不换行，放不下即截断（+N 省略）；标签很多时再改多行
                        if (chipX + tw + 22.0f > w - 28.0f) break;
                        const std::string chipId =
                            "todo.dialog.detail.tag." + std::to_string(ti);
                        drawChip(ui, chipId, chipX, 392.0f, tw, t, detailTags_[ti],
                                 t.colors.info, t.colors.surface);
                        components::button(ui, chipId + ".x")
                            .position(chipX + tw + 1.0f, 390.0f)
                            .size(20.0f, 22.0f)
                            .text("×")
                            .fontSize(t.sizes.fontSizeSmall)
                            .colors(t.colors.surface, t.colors.hover, t.colors.active)
                            .textColor(t.colors.textSecondary)
                            .radius(11.0f)
                            .onClick([this, ti] {
                                if (ti < detailTags_.size()) {
                                    detailTags_.erase(detailTags_.begin() +
                                                      static_cast<long>(ti));
                                }
                            })
                            .build();
                        chipX += tw + 26.0f;
                    }
                }
                components::input(ui, "todo.dialog.detail.tag.input")
                    .position(28.0f, 418.0f)
                    .size(w - 56.0f, 36.0f)
                    .value(detailTagInput_)
                    .placeholder("输入标签，回车添加…")
                    .fontSize(t.sizes.fontSizeSmall)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { detailTagInput_ = v; })
                    .onEnter([this] { addDetailTag(); })
                    // 审计 [21]：详情弹窗内加标签输入框，Esc=丢弃草稿（不清标签
                    // 草稿、不关详情弹窗——整窗关断由「关闭」按钮负责）
                    .onCancel([this] { detailTagInput_.clear(); })
                    .build();

                // 移动到其他列
                ui_kit::drawDialogText(ui, "todo.dialog.detail.mlabel", w, "移动到",
                                       t.sizes.fontSizeSmall - 2.0f, t.colors.textTertiary,
                                       {.y = 470.0f, .width = 120.0f, .height = 16.0f,
                                        .lineHeight = 16.0f});
                for (size_t i = 0; i < columns_.size(); ++i) {
                    const bool active = (columns_[i].id == detailColumnId_);
                    const int slot = static_cast<int>(i);
                    const float bx = 28.0f +
                        static_cast<float>(slot % perRow) * (moveBtnW + moveGap);
                    const float by = 490.0f +
                        static_cast<float>(slot / perRow) * 40.0f;
                    components::button(ui, "todo.dialog.detail.move." + std::to_string(i))
                        .position(bx, by)
                        .size(moveBtnW, 32.0f)
                        .text(columns_[i].title)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .colors(active ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                                t.colors.hover, t.colors.active)
                        .textColor(active ? t.colors.primaryText : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i] {
                            const int64_t taskId = detailTaskId_;
                            const int64_t toCol = columns_[i].id;
                            if (toCol == detailColumnId_) return;  // 已在该列
                            // 产品审计 2026-09-23 [2]：先落表单再移动，
                            // 编辑中的改动不再被静默丢弃；保存失败保持弹窗
                            if (!saveDetailForm()) return;
                            detailColumnId_ = toCol;
                            closePopup(Popup::Detail);
                            detailDiscardArmed_ = false;
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
                    // 与右键菜单删除同路径：先弹确认（Popup::Confirm），不再直接落库。
                    // 产品审计 2026-09-23 [2]：不关详情弹窗——确认取消后回到
                    // 弹窗继续编辑（确认弹窗按打开顺序压在详情之上）
                    .onClick([this] {
                        confirmTaskId_ = detailTaskId_;
                        confirmTaskTitle_ = detailTitle_;
                        confirmColumnId_ = 0;
                        openPopup(Popup::Confirm);
                    })
                    .build();

                components::button(ui, "todo.dialog.detail.toggle")
                    .position(132.0f, btnY)
                    .size(132.0f, 40.0f)
                    .icon(detailCompleted_ ? 0xF021 : 0xF00C)  // fa-arrows-rotate / fa-check
                    .text(detailCompleted_ ? "取消完成" : "标记完成")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(detailCompleted_ ? t.colors.textSecondary : t.colors.success)
                    .iconColor(detailCompleted_ ? t.colors.textSecondary
                                                : t.colors.success)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        const int64_t taskId = detailTaskId_;
                        const bool wasDone = detailCompleted_;
                        // 产品审计 2026-09-23 [2]：先落表单再切完成态，
                        // 编辑中的改动不再被静默丢弃；保存失败保持弹窗
                        if (!saveDetailForm()) return;
                        closePopup(Popup::Detail);
                        detailDiscardArmed_ = false;
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
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this] {
                        // 表单组装 + 落库统一走 collectDetailFormTask/
                        // saveDetailForm（与动作按钮同一份语义）；
                        // 保存失败保持弹窗打开（错误已 setStatus 提示）
                        if (!saveDetailForm()) return;
                        closePopup(Popup::Detail);
                        detailDiscardArmed_ = false;
                    })
                    .build();
        });
    }

    // ---------- 状态 ----------
    std::unique_ptr<TodoService> service_;
    std::vector<Column> columns_;
    std::vector<Task> tasks_;
    // 审计 [23]：列内滚动偏移按列 id 记忆（旧按列索引，重排/删列串列）+
    // 跨会话持久化（viewStore_；viewDirty_ = 有未落盘变更，onStop 收口）
    std::map<int64_t, float> columnScrolls_;
    std::unique_ptr<TodoViewStateStore> viewStore_;
    bool viewDirty_ = false;

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
        // 与 pressX/pressY 同帧的源列滚动偏移快照（compose 空间）。
        // dragDropTarget 靠它把含滚动的 bounds.y 还原成板面顶边，
        // 取"当下"值会跨帧混用（tech-debt B，见 onPress 处注释）
        float pressScrollOffset = 0.0f;
        int hoverColIndex = -1;    // 拖动中指针所在列（-1 = 列外，松手取消）
    };
    DragState drag_;

    // 板面几何快照（drawBoard/drawCard 每帧刷新，拖拽命中换算用）
    float boardColW_ = 0.0f;
    float boardColGap_ = 0.0f;
    float boardH_ = 0.0f;
    float boardCardW_ = 0.0f;

    // 拖拽 dpi 探针告警只出一次（审计 M4：探针降级为断言；hitDropColumn 是
    // const 且每帧调用，故 mutable + 一次性）
    mutable bool dpiProbeWarned_ = false;

    // 弹窗打开栈（审计 THS-05）：唯一的「谁在最上层」真相源——
    // 开关/判读/z 序都走它（见 Popup / openPopup / popupZ）
    std::vector<Popup> openStack_;

    // 新建任务弹窗
    int64_t newTaskColumnId_ = 0;
    std::string newTaskTitle_;
    std::string newTaskError_;  // 审计 [19]：内嵌红字（claude formError_ 模式）

    // 详情弹窗
    // backdrop 关闭守卫（审计 2026-09-23 [2]）：有未存改动时第一次点遮罩
    // 只警告（置位），再点一次才真正关闭放弃修改
    bool detailDiscardArmed_ = false;
    int64_t detailTaskId_ = 0;
    int64_t detailColumnId_ = 0;
    std::string detailTitle_;
    std::string detailDescription_;
    Priority detailPriority_ = Priority::None;
    std::optional<int64_t> detailDueAt_;
    bool detailCompleted_ = false;
    std::vector<std::string> detailTags_;
    std::string detailTagInput_;  // 标签输入框（回车追加，未落库）

    // 键盘移动选中态（最近点击的卡片；详情弹窗关闭后保留）
    int64_t selectedTaskId_ = 0;

    // 右键菜单（ctxTaskId_ > 0 = 任务菜单；否则 ctxColumnId_ > 0 = 列菜单）
    int64_t ctxTaskId_ = 0;
    int64_t ctxColumnId_ = 0;
    int ctxColumnIndex_ = 0;
    int ctxCardIndex_ = -1;

    // 重命名列 / 删除确认弹窗（确认弹窗双模式：任务 / 列）
    int64_t renameColumnId_ = 0;
    std::string renameTitle_;
    // 审计 [19]：弹窗内嵌红字错误行（照抄 claude formError_ 模式）——
    // 校验/创建失败不再静默、不再「关弹窗 + toast」，就地可见
    std::string renameError_;
    // 新建列弹窗（审计 [24]）：「+ 列」先命名后落库
    std::string newColumnTitle_;
    std::string newColumnError_;  // 审计 [19]：同款内嵌错误行
    int64_t confirmColumnId_ = 0;
    int64_t confirmTaskId_ = 0;
    std::string confirmTaskTitle_;

    // 状态反馈 toast（超时消失；subtitle 保持静态说明；状态位 + 绘制走 ui_kit）
    ui_kit::ToastState toast_;

    // 搜索（三件套 1）：query + service_->search 的命中任务 id 集
    std::string searchQuery_;
    std::unordered_set<int64_t> searchMatchIds_;

    // 逾期视图（三件套 2）：badge 数量 + 弹窗数据（打开时刷新）
    size_t overdueCount_ = 0;
    std::vector<Task> overdueTasks_;

    // 数据加载错误（空 = 正常；与空态区分，见 drawBoard）
    std::string loadError_;
};

} // namespace module::todo

extern "C" ::core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}