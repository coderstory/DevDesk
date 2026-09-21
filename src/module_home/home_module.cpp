// src/module_home/home_module.cpp
// Home 仪表盘模块 - 完整 UI 实现
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <ctime>
#include <string>

namespace module::home {

namespace {

// 拼装中文日期字符串（mock — MVP 不读系统时间）
std::string todayDate() {
    return "2026 年 9 月 21 日 · 星期一";
}

// 渲染单个统计卡片
void drawStatCard(eui::Ui& ui,
                  const std::string& id,
                  float x,
                  float y,
                  float width,
                  float height,
                  const core::theme::Tokens& tokens,
                  const std::string& label,
                  const std::string& number,
                  const std::string& badge,
                  bool accent) {
    // 卡片背景
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, height)
        .color(tokens.colors.surface)
        .radius(tokens.sizes.radiusLarge)
        .border(1.0f, tokens.colors.borderDefault)
        .build();

    // 顶部 label
    ui.text(id + ".label")
        .position(x + 28.0f, y + 22.0f)
        .size(width - 56.0f, 16.0f)
        .text(label)
        .fontSize(tokens.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
        .color(tokens.colors.textSecondary)
        .build();

    // 中间大数字 (48px display)
    ui.text(id + ".number")
        .position(x + 28.0f, y + 44.0f)
        .size(width - 56.0f, 56.0f)
        .text(number)
        .fontSize(tokens.sizes.fontSizeDisplay)
        .lineHeight(56.0f)
        .fontWeight(700)
        .color(accent ? tokens.colors.error : tokens.colors.textPrimary)
        .build();

    // 底部 badge
    ui.text(id + ".badge")
        .position(x + 28.0f, y + 100.0f)
        .size(width - 56.0f, 14.0f)
        .text(badge)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(accent ? tokens.colors.error : tokens.colors.textTertiary)
        .build();
}

} // namespace

class HomeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Home; }
    std::string displayName() const override { return "首页"; }
    std::string iconGlyph() const override { return "🏠"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;       // 40
        const float gap = t.sizes.spacingLarge * 1.25f; // 20
        const float contentW = screen.width - pad * 2.0f;
        const float cardGap = 20.0f;
        const float statW = (contentW - cardGap * 3.0f) / 4.0f;
        const float statH = 120.0f;

        ui.column("home.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(gap)
            .content([&] {
                // ========== 顶部欢迎区 ==========
                ui.row("home.header")
                    .size(contentW, 72.0f)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        ui.column("home.greeting")
                            .size(std::max(280.0f, contentW - 220.0f), 72.0f)
                            .gap(6.0f)
                            .content([&] {
                                ui.text("home.h1")
                                    .size(std::max(280.0f, contentW - 220.0f), 40.0f)
                                    .text("欢迎回到")
                                    .fontSize(t.sizes.fontSizePage)
                                    .lineHeight(40.0f)
                                    .fontWeight(700)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("home.date")
                                    .size(std::max(280.0f, contentW - 220.0f), 18.0f)
                                    .text(todayDate())
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧按钮组（占位 — 用 ghost 风格 rect）
                        ui.row("home.header.actions")
                            .size(200.0f, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                ui.rect("home.btn.refresh")
                                    .size(96.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("home.btn.refresh.text")
                                    .size(96.0f, 40.0f)
                                    .text("↻ 刷新")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textSecondary)
                                    .build();

                                ui.rect("home.btn.settings")
                                    .size(96.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("home.btn.settings.text")
                                    .size(96.0f, 40.0f)
                                    .text("⚙ 设置")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 4 个统计卡片 ==========
                ui.row("home.stats")
                    .size(contentW, statH)
                    .gap(cardGap)
                    .content([&] {
                        drawStatCard(ui, "home.stat.notes", 0, 0, statW, statH, t,
                                     "今日笔记", "3", "新建 +1", false);
                        drawStatCard(ui, "home.stat.todo", 0, 0, statW, statH, t,
                                     "待办任务", "7", "进行中 2", false);
                        drawStatCard(ui, "home.stat.overdue", 0, 0, statW, statH, t,
                                     "逾期任务", "1", "今日截止", true);
                        drawStatCard(ui, "home.stat.api", 0, 0, statW, statH, t,
                                     "API 调用", "142", "本周 +24%", false);
                    })
                    .build();

                // ========== 最近编辑卡片 ==========
                ui.rect("home.recent.bg")
                    .size(contentW, 232.0f)
                    .color(t.colors.surface)
                    .radius(t.sizes.radiusLarge)
                    .border(1.0f, t.colors.borderDefault)
                    .build();

                ui.text("home.recent.title")
                    .position(pad + 28.0f, pad + (72.0f + gap) + (statH + gap) + 22.0f)
                    .size(contentW - 56.0f, 24.0f)
                    .text("最近编辑")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(24.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                // 三条 mock 数据
                const float itemY = pad + (72.0f + gap) + (statH + gap) + 64.0f;
                const char* titles[]   = {"设计稿v2.md", "周会纪要.md", "bug 分析.md"};
                const char* times[]    = {"2 小时前",    "昨天",        "3 天前"};
                for (int i = 0; i < 3; ++i) {
                    const float iy = itemY + static_cast<float>(i) * 48.0f;
                    // icon
                    ui.rect("home.recent.icon." + std::to_string(i))
                        .position(pad + 28.0f, iy)
                        .size(36.0f, 36.0f)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radiusSmall)
                        .build();
                    // 文件名
                    ui.text("home.recent.name." + std::to_string(i))
                        .position(pad + 76.0f, iy + 2.0f)
                        .size(contentW - 220.0f, 18.0f)
                        .text(titles[i])
                        .fontSize(t.sizes.fontSizeBody - 1.0f)
                        .lineHeight(18.0f)
                        .fontWeight(500)
                        .color(t.colors.textPrimary)
                        .build();
                    // 时间
                    ui.text("home.recent.time." + std::to_string(i))
                        .position(pad + 76.0f, iy + 20.0f)
                        .size(contentW - 220.0f, 14.0f)
                        .text(times[i])
                        .fontSize(11.0f)
                        .lineHeight(14.0f)
                        .color(t.colors.textTertiary)
                        .build();
                    // tag chip 右侧
                    ui.rect("home.recent.tag." + std::to_string(i))
                        .position(pad + contentW - 92.0f, iy + 8.0f)
                        .size(64.0f, 20.0f)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radiusPill)
                        .build();
                }

                // ========== 快捷操作按钮组 ==========
                const float actGap = t.sizes.spacingLarge; // 16
                const float actW = (contentW - actGap * 2.0f) / 3.0f;
                ui.row("home.actions")
                    .size(contentW, 44.0f)
                    .gap(actGap)
                    .content([&] {
                        // 主按钮 + 新建笔记 (accent 背景)
                        ui.rect("home.act.primary.bg")
                            .size(actW, 44.0f)
                            .color(t.colors.primary)
                            .radius(t.sizes.radius)
                            .build();
                        ui.text("home.act.primary.text")
                            .size(actW, 44.0f)
                            .text("+ 新建笔记")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(44.0f)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(eui::Color("#FFFFFF"))
                            .build();

                        ui.rect("home.act.todo.bg")
                            .size(actW, 44.0f)
                            .color(t.colors.surfaceElevated)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .build();
                        ui.text("home.act.todo.text")
                            .size(actW, 44.0f)
                            .text("+ 新建任务")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(44.0f)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textPrimary)
                            .build();

                        ui.rect("home.act.claude.bg")
                            .size(actW, 44.0f)
                            .color(t.colors.surfaceElevated)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .build();
                        ui.text("home.act.claude.text")
                            .size(actW, 44.0f)
                            .text("📋 切换 Claude Profile")
                            .fontSize(t.sizes.fontSizeBody)
                            .lineHeight(44.0f)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .color(t.colors.textPrimary)
                            .build();
                    })
                    .build();
            })
            .build();

        LOG_DEBUG("Home 模块 compose 完成");
    }

    void onStart() override { LOG_INFO("Home 模块启动"); }
    void onStop() override { LOG_INFO("Home 模块停止"); }
};

} // namespace module::home

extern "C" ::core::Module* create_home_module() {
    return new module::home::HomeModule();
}