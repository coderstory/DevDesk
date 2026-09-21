// src/module_todo/todo_module.cpp
// Todo 看板模块 - 完整 UI 实现
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <string>

namespace module::todo {

namespace {

struct MockCard {
    const char* title;
    int priority;  // 0=高 1=中 2=低 3=无
    const char* due;
    bool done;
};

void drawPriorityChip(eui::Ui& ui,
                      const std::string& id,
                      float x,
                      float y,
                      const core::theme::Tokens& t,
                      int priority) {
    const char* labels[] = {"🔴高", "🟡中", "🔵低", "⚪无"};
    const eui::Color colors[] = {t.colors.error, t.colors.warning, t.colors.info, t.colors.textTertiary};
    const char* label = labels[priority];
    const eui::Color& color = colors[priority];
    const float w = 48.0f;
    const float h = 18.0f;
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.surfaceElevated)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(10.0f)
        .lineHeight(18.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(color)
        .fontWeight(600)
        .build();
}

void drawDueChip(eui::Ui& ui,
                 const std::string& id,
                 float x,
                 float y,
                 const core::theme::Tokens& t,
                 const std::string& text) {
    const float w = 64.0f;
    const float h = 18.0f;
    ui.text(id + ".text")
        .position(x, y)
        .size(w, h)
        .text("⏰ " + std::string(text))
        .fontSize(10.0f)
        .lineHeight(18.0f)
        .color(t.colors.textTertiary)
        .build();
}

void drawCard(eui::Ui& ui,
              const std::string& id,
              float y,
              float width,
              const core::theme::Tokens& t,
              const MockCard& card) {
    const float cardH = 84.0f;
    // 卡片背景
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, cardH)
        .color(t.colors.surfaceElevated)
        .radius(t.sizes.radius)
        .build();

    // 优先级左边框（4px）
    const eui::Color borderColors[] = {t.colors.error, t.colors.warning, t.colors.info, t.colors.borderDefault};
    ui.rect(id + ".priority.bar")
        .position(0.0f, y)
        .size(4.0f, cardH)
        .color(borderColors[card.priority])
        .build();

    // 标题
    ui.text(id + ".title")
        .position(16.0f, y + 14.0f)
        .size(width - 32.0f, 18.0f)
        .text(card.title)
        .fontSize(14.0f)
        .lineHeight(18.0f)
        .fontWeight(500)
        .color(t.colors.textPrimary)
        .opacity(card.done ? 0.6f : 1.0f)
        .build();

    // 优先级 chip + 截止 chip
    drawPriorityChip(ui, id + ".prio", 16.0f, y + 44.0f, t, card.priority);
    drawDueChip(ui, id + ".due", 70.0f, y + 44.0f, t, card.due);

    // 完成态打勾
    if (card.done) {
        ui.text(id + ".done")
            .position(width - 32.0f, y + 14.0f)
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

void drawColumn(eui::Ui& ui,
                const std::string& id,
                float y,
                float width,
                float height,
                const core::theme::Tokens& t,
                const std::string& title,
                int count,
                const std::vector<MockCard>& cards,
                bool archived = false) {
    // 列容器
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, height)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();

    // 列头
    ui.text(id + ".title")
        .position(16.0f, y + 16.0f)
        .size(width - 60.0f, 20.0f)
        .text(title + " (" + std::to_string(count) + ")")
        .fontSize(15.0f)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    // + 按钮
    ui.rect(id + ".add.bg")
        .position(width - 36.0f, y + 14.0f)
        .size(28.0f, 28.0f)
        .color(t.colors.surfaceElevated)
        .radius(14.0f)
        .build();
    ui.text(id + ".add.text")
        .position(width - 36.0f, y + 14.0f)
        .size(28.0f, 28.0f)
        .text("+")
        .fontSize(18.0f)
        .lineHeight(28.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textSecondary)
        .build();

    // 空列提示（归档列）
    if (archived) {
        ui.text(id + ".empty.icon")
            .position(width / 2.0f - 16.0f, y + height / 2.0f - 24.0f)
            .size(32.0f, 32.0f)
            .text("📦")
            .fontSize(28.0f)
            .lineHeight(32.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .build();
        ui.text(id + ".empty.text")
            .position(0.0f, y + height / 2.0f + 16.0f)
            .size(width, 18.0f)
            .text("归档的任务在这里")
            .fontSize(12.0f)
            .lineHeight(18.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
    } else {
        // 卡片列表
        const float cardY0 = y + 56.0f;
        const float cardGap = 8.0f;
        for (size_t i = 0; i < cards.size(); ++i) {
            drawCard(ui, id + ".card." + std::to_string(i),
                     cardY0 + static_cast<float>(i) * (84.0f + cardGap),
                     width - 16.0f, t, cards[i]);
        }
    }
}

} // namespace

class TodoModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Todo; }
    std::string displayName() const override { return "Todo 看板"; }
    std::string iconGlyph() const override { return "✅"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = 64.0f;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;
        const float colGap = t.sizes.spacingLarge;        // 16
        const float colW = (contentW - colGap * 3.0f) / 4.0f;

        ui.column("todo.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                // ========== 顶部 Header ==========
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
                                ui.text("todo.subtitle")
                                    .size(contentW - 480.0f, 18.0f)
                                    .text("⌘N 新建任务 · 拖拽卡片跨列移动（也可右键「移动到...」）")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧操作
                        ui.row("todo.actions")
                            .size(480.0f, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                // 搜索框占位
                                ui.rect("todo.search.bg")
                                    .size(160.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("todo.search.text")
                                    .size(160.0f, 40.0f)
                                    .text("🔍 搜索任务")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // + 列 按钮
                                ui.rect("todo.btn.addcol.bg")
                                    .size(72.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("todo.btn.addcol.text")
                                    .size(72.0f, 40.0f)
                                    .text("+ 列")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // + 新建任务
                                ui.rect("todo.btn.add.bg")
                                    .size(120.0f, 40.0f)
                                    .color(t.colors.primary)
                                    .radius(t.sizes.radius)
                                    .build();
                                ui.text("todo.btn.add.text")
                                    .size(120.0f, 40.0f)
                                    .text("+ 新建任务")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(eui::Color("#FFFFFF"))
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 4 列卡片墙 ==========
                ui.row("todo.body")
                    .size(contentW, bodyH)
                    .gap(colGap)
                    .content([&] {
                        // 列 1: 待办 (3)
                        std::vector<MockCard> todoCards = {
                            {"写文档", 0, "明天", false},
                            {"会议纪要", 1, "今天", false},
                            {"环境配置", 2, "本周", false},
                        };
                        drawColumn(ui, "todo.col.todo", 0, colW, bodyH, t, "待办", 3, todoCards);

                        // 列 2: 进行中 (1)
                        std::vector<MockCard> doingCards = {
                            {"修复 bug #42", 1, "", false},
                        };
                        drawColumn(ui, "todo.col.doing", 0, colW, bodyH, t, "进行中", 1, doingCards);

                        // 列 3: 已完成 (2)
                        std::vector<MockCard> doneCards = {
                            {"初次部署", 2, "今天", true},
                            {"环境配置", 3, "昨天", true},
                        };
                        drawColumn(ui, "todo.col.done", 0, colW, bodyH, t, "已完成", 2, doneCards);

                        // 列 4: 已归档
                        std::vector<MockCard> empty;
                        drawColumn(ui, "todo.col.archive", 0, colW, bodyH, t, "已归档", 0, empty, true);
                    })
                    .build();
            })
            .build();

        LOG_DEBUG("Todo 模块 compose 完成");
    }

    void onStart() override { LOG_INFO("Todo 模块启动"); }
    void onStop() override { LOG_INFO("Todo 模块停止"); }
};

} // namespace module::todo

extern "C" ::core::Module* create_todo_module() {
    return new module::todo::TodoModule();
}