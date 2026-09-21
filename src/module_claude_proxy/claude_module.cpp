// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - 完整 UI 实现
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <string>

namespace module::claude {

namespace {

struct MockProfile {
    const char* name;
    const char* baseUrl;
    const char* model;
    bool active;
};

void drawProfileCard(eui::Ui& ui,
                     const std::string& id,
                     float y,
                     float width,
                     const core::theme::Tokens& t,
                     const MockProfile& profile) {
    const float h = 96.0f;
    const eui::Color borderColor = profile.active ? t.colors.primary : t.colors.borderDefault;
    const float borderW = profile.active ? 2.0f : 1.0f;
    const eui::Color bg = profile.active ? t.colors.primarySoftBg : t.colors.surface;

    // 卡片背景
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, h)
        .color(bg)
        .radius(t.sizes.radiusLarge)
        .border(borderW, borderColor)
        .build();

    // 头像（圆形 - 用 rect 模拟）
    ui.rect(id + ".avatar")
        .position(16.0f, y + 16.0f)
        .size(40.0f, 40.0f)
        .color(t.colors.surfaceElevated)
        .radius(20.0f)
        .border(1.0f, t.colors.borderDefault)
        .build();
    // 头像首字
    const std::string initial = std::string(1, profile.name[0]);
    ui.text(id + ".avatar.text")
        .position(16.0f, y + 16.0f)
        .size(40.0f, 40.0f)
        .text(initial)
        .fontSize(16.0f)
        .lineHeight(40.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textPrimary)
        .fontWeight(600)
        .build();

    // 名称 + ACTIVE chip
    ui.text(id + ".name")
        .position(64.0f, y + 18.0f)
        .size(width - 130.0f, 20.0f)
        .text(profile.name)
        .fontSize(15.0f)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    if (profile.active) {
        ui.rect(id + ".active.bg")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .color(t.colors.primary)
            .radius(t.sizes.radiusPill)
            .build();
        ui.text(id + ".active.text")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .text("ACTIVE")
            .fontSize(9.0f)
            .lineHeight(20.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(eui::Color("#FFFFFF"))
            .fontWeight(700)
            .build();
    }

    // base-url (mono, 12px, tertiary)
    ui.text(id + ".url")
        .position(64.0f, y + 44.0f)
        .size(width - 80.0f, 14.0f)
        .text(profile.baseUrl)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // model (mono, 12px)
    ui.text(id + ".model")
        .position(64.0f, y + 62.0f)
        .size(width - 80.0f, 14.0f)
        .text(std::string("model: ") + profile.model)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textSecondary)
        .build();
}

} // namespace

class ClaudeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ClaudeProxy; }
    std::string displayName() const override { return "Claude 切换器"; }
    std::string iconGlyph() const override { return "🔄"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = 64.0f;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;
        const float gap = 16.0f;
        const float leftW = 400.0f;
        const float rightW = contentW - leftW - gap;

        ui.column("claude.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                // ========== 顶部 Header ==========
                ui.row("claude.header")
                    .size(contentW, headerH)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        // 左侧标题 + 状态
                        ui.column("claude.title.col")
                            .size(contentW - 360.0f, headerH)
                            .gap(6.0f)
                            .content([&] {
                                ui.row("claude.title.row")
                                    .size(contentW - 360.0f, 28.0f)
                                    .gap(12.0f)
                                    .alignItems(eui::Align::CENTER)
                                    .content([&] {
                                        ui.text("claude.h1")
                                            .size(220.0f, 32.0f)
                                            .text("Claude 切换器")
                                            .fontSize(t.sizes.fontSizeTitle)
                                            .lineHeight(32.0f)
                                            .fontWeight(700)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        // 状态指示器
                                        ui.rect("claude.status.dot")
                                            .size(10.0f, 10.0f)
                                            .color(t.colors.success)
                                            .radius(5.0f)
                                            .build();
                                        ui.text("claude.status.text")
                                            .size(140.0f, 18.0f)
                                            .text("🟢 运行中 :8888")
                                            .fontSize(13.0f)
                                            .lineHeight(18.0f)
                                            .color(t.colors.success)
                                            .fontWeight(500)
                                            .build();
                                    })
                                    .build();

                                ui.text("claude.subtitle")
                                    .size(contentW - 360.0f, 16.0f)
                                    .text("管理多套 Claude API 配置 · 本地代理 http://127.0.0.1:8888")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧操作
                        ui.row("claude.actions")
                            .size(360.0f, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                // 停止代理
                                ui.rect("claude.btn.stop.bg")
                                    .size(96.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("claude.btn.stop.text")
                                    .size(96.0f, 40.0f)
                                    .text("⏸ 停止代理")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textSecondary)
                                    .build();

                                // 新建 Profile
                                ui.rect("claude.btn.new.bg")
                                    .size(140.0f, 40.0f)
                                    .color(t.colors.primary)
                                    .radius(t.sizes.radius)
                                    .build();
                                ui.text("claude.btn.new.text")
                                    .size(140.0f, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(eui::Color("#FFFFFF"))
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 主区: Profile 列表 + 详情 ==========
                ui.row("claude.body")
                    .size(contentW, bodyH)
                    .gap(gap)
                    .content([&] {
                        // ---- 左栏：Profile 列表 ----
                        ui.column("claude.profiles")
                            .size(leftW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("claude.profiles.bg")
                                    .size(leftW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("claude.profiles.overlay")
                            .size(leftW, bodyH)
                            .content([&] {
                                // 顶部栏
                                ui.text("claude.profiles.title")
                                    .position(20.0f, 20.0f)
                                    .size(leftW - 40.0f, 20.0f)
                                    .text("Profile 列表")
                                    .fontSize(15.0f)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 卡片列表
                                std::vector<MockProfile> profiles = {
                                    {"官方", "api.anthropic.com", "claude-sonnet-4-5", true},
                                    {"第三方 A", "api.3rd-party.com", "claude-opus-4-5", false},
                                    {"测试账号", "api.test.com", "claude-haiku-4-5", false},
                                };
                                const float cardY0 = 56.0f;
                                const float cardGap = 12.0f;
                                for (size_t i = 0; i < profiles.size(); ++i) {
                                    drawProfileCard(ui, "claude.profile." + std::to_string(i),
                                                    cardY0 + static_cast<float>(i) * (96.0f + cardGap),
                                                    leftW - 24.0f, t, profiles[i]);
                                }

                                // 底部新建按钮
                                const float newBtnY = bodyH - 64.0f;
                                ui.rect("claude.profiles.new.bg")
                                    .position(20.0f, newBtnY)
                                    .size(leftW - 40.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("claude.profiles.new.text")
                                    .position(20.0f, newBtnY)
                                    .size(leftW - 40.0f, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textPrimary)
                                    .build();
                            })
                            .build();

                        // ---- 右栏：详情面板 + 最近调用 ----
                        ui.column("claude.detail")
                            .size(rightW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("claude.detail.bg")
                                    .size(rightW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("claude.detail.overlay")
                            .size(rightW, bodyH)
                            .content([&] {
                                const float x0 = 28.0f;
                                const float y0 = 24.0f;
                                const float w0 = rightW - 56.0f;

                                // 标题
                                ui.text("claude.detail.title")
                                    .position(x0, y0)
                                    .size(w0, 24.0f)
                                    .text("当前 Profile 详情")
                                    .fontSize(18.0f)
                                    .lineHeight(24.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 字段网格（2 列）
                                const float fy0 = y0 + 44.0f;
                                const float fieldH = 56.0f;

                                // 名称（左）
                                ui.text("claude.field.name.label")
                                    .position(x0, fy0)
                                    .size((w0 - 16.0f) / 2.0f, 14.0f)
                                    .text("名称")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.name.bg")
                                    .position(x0, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f, fieldH - 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.name.value")
                                    .position(x0 + 12.0f, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f - 24.0f, fieldH - 32.0f)
                                    .text("官方")
                                    .fontSize(14.0f)
                                    .lineHeight(fieldH - 32.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 模型（右）
                                const float col2x = x0 + (w0 + 16.0f) / 2.0f;
                                ui.text("claude.field.model.label")
                                    .position(col2x, fy0)
                                    .size((w0 - 16.0f) / 2.0f, 14.0f)
                                    .text("模型")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.model.bg")
                                    .position(col2x, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f, fieldH - 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.model.value")
                                    .position(col2x + 12.0f, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f - 24.0f, fieldH - 32.0f)
                                    .text("claude-sonnet-4-5")
                                    .fontSize(14.0f)
                                    .lineHeight(fieldH - 32.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // Base URL（全宽）
                                ui.text("claude.field.url.label")
                                    .position(x0, fy0 + fieldH)
                                    .size(w0, 14.0f)
                                    .text("Base URL")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.url.bg")
                                    .position(x0, fy0 + fieldH + 18.0f)
                                    .size(w0, 28.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.url.value")
                                    .position(x0 + 12.0f, fy0 + fieldH + 18.0f)
                                    .size(w0 - 24.0f, 28.0f)
                                    .text("https://api.anthropic.com")
                                    .fontSize(13.0f)
                                    .lineHeight(28.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // Token（脱敏）
                                ui.text("claude.field.token.label")
                                    .position(x0, fy0 + fieldH + 56.0f)
                                    .size(w0, 14.0f)
                                    .text("API Key")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.token.bg")
                                    .position(x0, fy0 + fieldH + 74.0f)
                                    .size(w0, 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.token.value")
                                    .position(x0 + 12.0f, fy0 + fieldH + 74.0f)
                                    .size(w0 - 24.0f, 32.0f)
                                    .text("sk-ant-•••••••••xyz [Keychain]")
                                    .fontSize(13.0f)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textSecondary)
                                    .build();

                                // 操作按钮组
                                const float actY = fy0 + fieldH + 130.0f;
                                const float actGap = 12.0f;
                                const float actW = (w0 - actGap * 3.0f) / 4.0f;
                                ui.row("claude.detail.actions")
                                    .size(w0, 36.0f)
                                    .gap(actGap)
                                    .content([&] {
                                        // 启用
                                        ui.rect("claude.act.enable.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.primary)
                                            .radius(t.sizes.radius)
                                            .build();
                                        ui.text("claude.act.enable.text")
                                            .size(actW, 36.0f)
                                            .text("启用")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(eui::Color("#FFFFFF"))
                                            .build();
                                        // 测试连通
                                        ui.rect("claude.act.test.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .build();
                                        ui.text("claude.act.test.text")
                                            .size(actW, 36.0f)
                                            .text("测试连通")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.textPrimary)
                                            .build();
                                        // 编辑
                                        ui.rect("claude.act.edit.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .build();
                                        ui.text("claude.act.edit.text")
                                            .size(actW, 36.0f)
                                            .text("编辑")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.textPrimary)
                                            .build();
                                        // 删除
                                        ui.rect("claude.act.del.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .build();
                                        ui.text("claude.act.del.text")
                                            .size(actW, 36.0f)
                                            .text("删除")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.error)
                                            .build();
                                    })
                                    .build();

                                // 分隔线
                                ui.rect("claude.divider")
                                    .position(x0, actY + 60.0f)
                                    .size(w0, 1.0f)
                                    .color(t.colors.borderMuted)
                                    .build();

                                // 最近调用
                                ui.text("claude.calls.title")
                                    .position(x0, actY + 76.0f)
                                    .size(w0, 20.0f)
                                    .text("最近调用")
                                    .fontSize(15.0f)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 调用表头
                                const float tableY = actY + 108.0f;
                                const float colTimeX = x0;
                                const float colPathX = x0 + 80.0f;
                                const float colStatusX = x0 + w0 - 200.0f;
                                const float colDurX = x0 + w0 - 100.0f;
                                ui.text("claude.calls.h.time")
                                    .position(colTimeX, tableY)
                                    .size(72.0f, 14.0f)
                                    .text("时间")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.path")
                                    .position(colPathX, tableY)
                                    .size(w0 - 280.0f, 14.0f)
                                    .text("方法 / 路径")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.status")
                                    .position(colStatusX, tableY)
                                    .size(60.0f, 14.0f)
                                    .text("状态")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.dur")
                                    .position(colDurX, tableY)
                                    .size(60.0f, 14.0f)
                                    .text("耗时")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 3 行 mock 数据
                                struct Call { const char* t; const char* p; int s; const char* d; };
                                const Call calls[] = {
                                    {"14:32", "POST /v1/messages", 200, "1247ms"},
                                    {"14:30", "POST /v1/messages", 200, "832ms"},
                                    {"14:25", "POST /v1/messages", 429, "312ms"},
                                };
                                for (int i = 0; i < 3; ++i) {
                                    const float rowY = tableY + 24.0f + static_cast<float>(i) * 24.0f;
                                    const Call& c = calls[i];
                                    ui.text("claude.calls.t." + std::to_string(i))
                                        .position(colTimeX, rowY)
                                        .size(72.0f, 16.0f)
                                        .text(c.t)
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.textPrimary)
                                        .build();
                                    ui.text("claude.calls.p." + std::to_string(i))
                                        .position(colPathX, rowY)
                                        .size(w0 - 280.0f, 16.0f)
                                        .text(c.p)
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.textPrimary)
                                        .build();
                                    const eui::Color statusColor =
                                        c.s == 200 ? t.colors.success :
                                        c.s == 429 ? t.colors.warning :
                                                     t.colors.error;
                                    ui.text("claude.calls.s." + std::to_string(i))
                                        .position(colStatusX, rowY)
                                        .size(60.0f, 16.0f)
                                        .text(std::to_string(c.s))
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(statusColor)
                                        .build();
                                    ui.text("claude.calls.d." + std::to_string(i))
                                        .position(colDurX, rowY)
                                        .size(60.0f, 16.0f)
                                        .text(c.d)
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.textSecondary)
                                        .build();
                                }

                                // 查看全部 →
                                ui.text("claude.calls.viewall")
                                    .position(x0, tableY + 108.0f)
                                    .size(w0, 18.0f)
                                    .text("查看全部 →")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.primary)
                                    .build();
                            })
                            .build();
                    })
                    .build();
            })
            .build();

        LOG_DEBUG("Claude 模块 compose 完成");
    }

    void onStart() override { LOG_INFO("Claude 模块启动"); }
    void onStop() override { LOG_INFO("Claude 模块停止"); }
};

} // namespace module::claude

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}