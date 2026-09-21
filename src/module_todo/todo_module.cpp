// src/module_todo/todo_module.cpp
// Todo 看板模块 - 真实数据 + CRUD（TodoService）
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/time.h"
#include "todo_service.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/theme.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace module::todo {

namespace {

// ---------- 卡片尺寸 ----------
constexpr float kCardHeight = 84.0f;
constexpr float kCardGap = 8.0f;
constexpr float kColumnHeaderHeight = 56.0f;

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

// 小标签（pill）
void drawChip(eui::Ui& ui,
              const std::string& id,
              float x,
              float y,
              float w,
              const core::theme::Tokens& t,
              const std::string& text,
              const eui::Color& fg,
              const eui::Color& bg) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, 18.0f)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(w, 18.0f)
        .text(text)
        .fontSize(10.0f)
        .lineHeight(18.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .build();
}

} // namespace

class TodoModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Todo; }
    std::string displayName() const override { return "Todo 看板"; }
    std::string iconGlyph() const override { return "✅"; }

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
        auto cols = service_->listColumns();
        if (cols.isOk()) {
            columns_ = std::move(cols).value();
        } else {
            LOG_ERROR("Todo: 读取列失败: " + cols.errorMessage());
        }
        auto all = service_->listAllTasks();
        if (all.isOk()) {
            tasks_ = std::move(all).value();
        } else {
            LOG_ERROR("Todo: 读取任务失败: " + all.errorMessage());
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

    // 按标题找列 id（找不到返回 0）
    int64_t findColumnByTitle(const std::string& title) const {
        for (const auto& c : columns_) {
            if (c.title == title) return c.id;
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

    void doMoveTask(int64_t taskId, int64_t toColumnId, int newPosition) {
        if (!service_) return;
        auto r = service_->moveTask(taskId, toColumnId, newPosition);
        if (r.isFail()) {
            setStatus("移动失败：" + r.errorMessage(), true);
            return;
        }
        reloadAll();
    }

    void doCompleteTask(int64_t taskId) {
        if (!service_) return;
        auto r = service_->completeTask(taskId);
        if (r.isFail()) {
            setStatus("标记完成失败：" + r.errorMessage(), true);
            return;
        }
        // 看板行为：完成后移到「已完成」列（如果该列存在）
        const int64_t doneCol = findColumnByTitle("已完成");
        if (doneCol != 0) {
            auto mv = service_->moveTask(taskId, doneCol, 0);
            if (mv.isFail()) {
                LOG_WARN("Todo: 完成后移动到已完成列失败: " + mv.errorMessage());
            }
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
        // 从「已完成」列撤回「待办」列（如果两列都存在）
        const int64_t doneCol = findColumnByTitle("已完成");
        const int64_t todoCol = columns_.empty() ? 0 : columns_.front().id;
        if (doneCol != 0 && todoCol != 0) {
            auto mv = service_->moveTask(taskId, todoCol, 0);
            if (mv.isFail()) {
                LOG_WARN("Todo: 取消完成后移回待办列失败: " + mv.errorMessage());
            }
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

    // ============ 状态提示 ============

    void setStatus(const std::string& msg, bool isError = false) {
        statusMessage_ = msg;
        statusIsError_ = isError;
    }

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = 64.0f;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;

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
                        const std::string sub = statusMessage_.empty()
                            ? "点击卡片查看详情 · 拖拽卡片跨列移动 · + 新建任务"
                            : statusMessage_;
                        ui.text("todo.subtitle")
                            .size(contentW - 480.0f, 18.0f)
                            .text(sub)
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(18.0f)
                            .color(statusIsError_ ? t.colors.error : t.colors.textSecondary)
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
                            .fontSize(13.0f)
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
                            .fontSize(13.0f)
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
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(width, height)
            .color(t.colors.surface)
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();

        auto colTasks = tasksInColumn(col.id);
        const bool isArchive = (col.title == "已归档");

        ui.text(id + ".title")
            .position(16.0f, 16.0f)
            .size(width - 60.0f, 20.0f)
            .text(col.title + " (" + std::to_string(colTasks.size()) + ")")
            .fontSize(15.0f)
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        // 列头 + 按钮
        components::button(ui, id + ".add")
            .position(width - 36.0f, 14.0f)
            .size(28.0f, 28.0f)
            .text("+")
            .fontSize(16.0f)
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
                .fontSize(12.0f)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            return;
        }

        const float cardY0 = kColumnHeaderHeight;
        for (size_t i = 0; i < colTasks.size(); ++i) {
            const float y = cardY0 + static_cast<float>(i) * (kCardHeight + kCardGap);
            if (y + kCardHeight > height) break;  // 超出列高就不画（简化：不做列内滚动）
            drawCard(ui, id + ".card." + std::to_string(i), y, width - 16.0f, t,
                     *colTasks[i], static_cast<int>(colIndex),
                     static_cast<int>(columns_.size()));
        }
    }

    void drawCard(eui::Ui& ui, const std::string& id, float y, float width,
                  const core::theme::Tokens& t, const Task& task,
                  int colIndex, int colCount) {
        const bool done = task.completedAt.has_value();

        ui.rect(id + ".bg")
            .position(0.0f, y)
            .size(width, kCardHeight)
            .states(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .radius(t.sizes.radius)
            .onClick([this, task] { openDetailDialog(task); })
            .onDrag([this, task, colIndex, colCount, width](const core::dsl::DragEvent& ev) {
                // 拖拽超过半列宽 → 移到相邻列（左 / 右）
                const double threshold = static_cast<double>(width) * 0.6;
                if (ev.totalX > threshold && colIndex + 1 < colCount) {
                    const int64_t toCol = columns_[static_cast<size_t>(colIndex + 1)].id;
                    doMoveTask(task.id, toCol, 0);
                    setStatus("已移动到「" + columns_[static_cast<size_t>(colIndex + 1)].title + "」");
                } else if (ev.totalX < -threshold && colIndex > 0) {
                    const int64_t toCol = columns_[static_cast<size_t>(colIndex - 1)].id;
                    doMoveTask(task.id, toCol, 0);
                    setStatus("已移动到「" + columns_[static_cast<size_t>(colIndex - 1)].title + "」");
                }
            })
            .build();

        // 优先级左边框
        ui.rect(id + ".bar")
            .position(0.0f, y)
            .size(4.0f, kCardHeight)
            .color(priorityColor(task.priority, t))
            .radius(2.0f)
            .build();

        // 标题
        ui.text(id + ".title")
            .position(16.0f, y + 14.0f)
            .size(width - 44.0f, 18.0f)
            .text(task.title)
            .fontSize(14.0f)
            .lineHeight(18.0f)
            .fontWeight(500)
            .color(t.colors.textPrimary)
            .opacity(done ? 0.6f : 1.0f)
            .build();

        // 优先级 chip
        drawChip(ui, id + ".prio", 16.0f, y + 44.0f, 48.0f, t,
                 priorityLabel(task.priority), priorityColor(task.priority, t),
                 t.colors.surface);

        // 截止 chip
        if (task.dueAt.has_value()) {
            const std::string dueText = "⏰ " + formatDue(task.dueAt.value());
            drawChip(ui, id + ".due", 70.0f, y + 44.0f, 66.0f, t, dueText,
                     isOverdue(task) ? t.colors.error : t.colors.textTertiary,
                     t.colors.surface);
        }

        // 完成打勾
        if (done) {
            ui.text(id + ".done")
                .position(width - 36.0f, y + 14.0f)
                .size(24.0f, 18.0f)
                .text("✓")
                .fontSize(14.0f)
                .lineHeight(18.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.success)
                .fontWeight(700)
                .build();
        }
    }

    // ---------- 新建任务弹窗 ----------
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
                    .fontSize(20.0f)
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
                    .fontSize(12.0f)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                components::input(ui, "todo.dialog.newtask.input")
                    .position(28.0f, 84.0f)
                    .size(w - 56.0f, 44.0f)
                    .value(newTaskTitle_)
                    .placeholder("任务标题…")
                    .fontSize(15.0f)
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
                    .fontSize(14.0f)
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
                    .fontSize(14.0f)
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

    // ---------- 任务详情 / 编辑弹窗 ----------
    void openDetailDialog(const Task& task) {
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
        const float h = 560.0f;

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
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // 标题
                ui.text("todo.dialog.detail.tlabel")
                    .position(28.0f, 58.0f)
                    .size(80.0f, 16.0f)
                    .text("标题")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "todo.dialog.detail.input.title")
                    .position(28.0f, 76.0f)
                    .size(w - 56.0f, 40.0f)
                    .value(detailTitle_)
                    .placeholder("任务标题…")
                    .fontSize(14.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { detailTitle_ = v; })
                    .build();

                // 描述
                ui.text("todo.dialog.detail.dlabel")
                    .position(28.0f, 126.0f)
                    .size(80.0f, 16.0f)
                    .text("描述")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "todo.dialog.detail.input.desc")
                    .position(28.0f, 144.0f)
                    .size(w - 56.0f, 88.0f)
                    .value(detailDescription_)
                    .placeholder("补充说明…（可多行）")
                    .fontSize(13.0f)
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
                    .fontSize(11.0f)
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
                        .fontSize(12.0f)
                        .colors(active ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                                t.colors.hover, t.colors.active)
                        .textColor(active ? t.colors.primary : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i, prios] { detailPriority_ = prios[i]; })
                        .build();
                }

                // 截止日期（快捷预设：无 / 今天 / 明天 / 一周后）
                ui.text("todo.dialog.detail.dlabel2")
                    .position(28.0f, 308.0f)
                    .size(120.0f, 16.0f)
                    .text("截止日期")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                const char* dueLabels[4] = {"无", "今天", "明天", "一周后"};
                for (int i = 0; i < 4; ++i) {
                    const bool hasDue = detailDueAt_.has_value();
                    const bool active = (i == 0) ? !hasDue : hasDue;
                    const float bw = 100.0f;
                    components::button(ui, "todo.dialog.detail.due." + std::to_string(i))
                        .position(28.0f + static_cast<float>(i) * (bw + 8.0f), 328.0f)
                        .size(bw, 32.0f)
                        .text(dueLabels[i])
                        .fontSize(12.0f)
                        .colors(active ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                                t.colors.hover, t.colors.active)
                        .textColor(active ? t.colors.primary : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i] {
                            const int64_t day = 86400;
                            const int64_t now = static_cast<int64_t>(std::time(nullptr));
                            switch (i) {
                                case 0: detailDueAt_ = std::nullopt; break;
                                case 1: detailDueAt_ = now + day; break;
                                case 2: detailDueAt_ = now + day * 2; break;
                                default: detailDueAt_ = now + day * 7; break;
                            }
                        })
                        .build();
                }

                // 移动到其他列
                ui.text("todo.dialog.detail.mlabel")
                    .position(28.0f, 372.0f)
                    .size(120.0f, 16.0f)
                    .text("移动到")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                for (size_t i = 0; i < columns_.size() && i < 4; ++i) {
                    const bool active = (columns_[i].id == detailColumnId_);
                    const float bw = 118.0f;
                    components::button(ui, "todo.dialog.detail.move." + std::to_string(i))
                        .position(28.0f + static_cast<float>(i) * (bw + 8.0f), 392.0f)
                        .size(bw, 32.0f)
                        .text(columns_[i].title)
                        .fontSize(12.0f)
                        .colors(active ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                                t.colors.hover, t.colors.active)
                        .textColor(active ? t.colors.primary : t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, active ? t.colors.primary : t.colors.borderDefault)
                        .onClick([this, i] {
                            const int64_t taskId = detailTaskId_;
                            const int64_t toCol = columns_[i].id;
                            detailColumnId_ = toCol;
                            detailOpen_ = false;
                            doMoveTask(taskId, toCol, 0);
                        })
                        .build();
                }

                // 底部操作区
                const float btnY = h - 58.0f;
                components::button(ui, "todo.dialog.detail.delete")
                    .position(28.0f, btnY)
                    .size(96.0f, 40.0f)
                    .text("删除")
                    .fontSize(13.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.error)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        const int64_t taskId = detailTaskId_;
                        detailOpen_ = false;
                        doDeleteTask(taskId);
                    })
                    .build();

                components::button(ui, "todo.dialog.detail.toggle")
                    .position(132.0f, btnY)
                    .size(132.0f, 40.0f)
                    .text(detailCompleted_ ? "↩ 取消完成" : "✓ 标记完成")
                    .fontSize(13.0f)
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
                    .fontSize(14.0f)
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
                        updated.position = 0;
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

    // 顶部提示
    std::string statusMessage_;
    bool statusIsError_ = false;
};

} // namespace module::todo

extern "C" ::core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}