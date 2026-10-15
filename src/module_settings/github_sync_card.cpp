// src/module_settings/github_sync_card.cpp
// GitHub Sync 设置卡片实现。
// 使用 EUI-NEO DSL 绘制表单：仓库 URL、分支、Token 输入、启用开关、同步/拉取按钮。
#include "github_sync_card.h"

#include "github_sync_config.h"
#include "settings_ui_logic.h"

#include <core/component_theme.h>
#include <core/paths.h>

#include <eui/app.h>
#include <eui/dsl.h>

#include <components/button.h>
#include <components/input.h>
#include <components/switch.h>

#include <filesystem>
#include <string>

namespace module::settings {

namespace {

// 行几何常量（100% 档基准，运行期随 roundFs 缩放）
inline constexpr float kCardPaddingX = 28.0f;
inline constexpr float kTitleH = 24.0f;
inline constexpr float kDescH = 18.0f;
inline constexpr float kRowH = 36.0f;
inline constexpr float kRowGap = 8.0f;
inline constexpr float kLabelW = 64.0f;
inline constexpr float kFieldW = 320.0f;
inline constexpr float kBtnW = 120.0f;
inline constexpr float kBtnSmallW = 80.0f;
inline constexpr float kSwitchW = 44.0f;
inline constexpr float kStatusH = 20.0f;

} // namespace

void drawGithubSyncCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokens(t);

    // 各行的 y 坐标（从卡顶开始）
    float y = 16.0f;  // 内边距

    // 标题行
    ui.text(id + ".title")
        .position(kCardPaddingX, y)
        .size(w - kCardPaddingX * 2, roundFs(kTitleH))
        .text("GitHub 同步")
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(roundFs(kTitleH))
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    y += roundFs(kTitleH) + roundFs(2.0f);

    // 描述行
    ui.text(id + ".desc")
        .position(kCardPaddingX, y)
        .size(w - kCardPaddingX * 2, roundFs(kDescH))
        .text("将笔记和待办数据同步到 GitHub 私有仓库，实现多设备同步。")
        .fontSize(fs(12.0f))
        .lineHeight(roundFs(kDescH))
        .color(t.colors.textTertiary)
        .build();
    y += roundFs(kDescH) + roundFs(10.0f);

    const float fieldStartX = kCardPaddingX + kLabelW + 12.0f;
    const float rowH = roundFs(kRowH);
    const float labelH = roundFs(kDescH);

    // 启用开关（第一行）
    ui.text(id + ".enabled.label")
        .position(kCardPaddingX, y + (rowH - labelH) * 0.5f)
        .size(kLabelW, labelH)
        .text("启用")
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(labelH)
        .color(t.colors.textSecondary)
        .build();
    components::switch_(ui, id + ".enabled.sw")
        .position(fieldStartX, y + (rowH - 20.0f) * 0.5f)
        .size(kSwitchW, 20.0f)
        .checked(in.githubSyncEnabled)
        .theme(compTheme)
        .onChange([f = a.applyGithubSyncEnabled](bool v) { f(v); })
        .build();
    y += rowH + roundFs(kRowGap);

    if (!in.githubSyncEnabled) {
        // 未启用：不显示后续表单
        return;
    }

    // 仓库 URL 输入
    ui.text(id + ".url.label")
        .position(kCardPaddingX, y + (rowH - labelH) * 0.5f)
        .size(kLabelW, labelH)
        .text("仓库 URL")
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(labelH)
        .color(t.colors.textSecondary)
        .build();
    components::input(ui, id + ".url.input")
        .position(fieldStartX, y)
        .size(kFieldW, rowH)
        .placeholder("https://github.com/user/repo.git")
        .value(in.githubSyncRepoUrl)
        .fontSize(t.sizes.fontSizeSmall)
        .theme(compTheme)
        .onChange([f = a.applyGithubSyncRepoUrl](const std::string& v) { f(v); })
        .build();
    y += rowH + roundFs(kRowGap);

    // 分支输入
    ui.text(id + ".branch.label")
        .position(kCardPaddingX, y + (rowH - labelH) * 0.5f)
        .size(kLabelW, labelH)
        .text("分支")
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(labelH)
        .color(t.colors.textSecondary)
        .build();
    components::input(ui, id + ".branch.input")
        .position(fieldStartX, y)
        .size(kFieldW, rowH)
        .placeholder("main")
        .value(in.githubSyncBranch)
        .fontSize(t.sizes.fontSizeSmall)
        .theme(compTheme)
        .onChange([f = a.applyGithubSyncBranch](const std::string& v) { f(v); })
        .build();
    y += rowH + roundFs(kRowGap);

    // Token 输入
    ui.text(id + ".token.label")
        .position(kCardPaddingX, y + (rowH - labelH) * 0.5f)
        .size(kLabelW, labelH)
        .text("Token")
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(labelH)
        .color(t.colors.textSecondary)
        .build();
    components::input(ui, id + ".token.input")
        .position(fieldStartX, y)
        .size(kFieldW, rowH)
        .placeholder("ghp_xxxxxxxxxxxxxxxxxxxx")
        .value(in.githubSyncTokenDraft)
        .fontSize(t.sizes.fontSizeSmall)
        .theme(compTheme)
        .onChange([f = a.setGithubSyncTokenDraft](const std::string& v) { f(v); })
        .build();
    y += rowH + roundFs(kRowGap);

    // 自动 Push 开关 + 操作按钮行
    const float autoPushLabelY = y + (rowH - labelH) * 0.5f;
    ui.text(id + ".autopush.label")
        .position(kCardPaddingX, autoPushLabelY)
        .size(kLabelW, labelH)
        .text("自动推送")
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(labelH)
        .color(t.colors.textSecondary)
        .build();
    components::switch_(ui, id + ".autopush.sw")
        .position(fieldStartX, y + (rowH - 20.0f) * 0.5f)
        .size(kSwitchW, 20.0f)
        .checked(in.githubSyncAutoPush)
        .theme(compTheme)
        .onChange([f = a.applyGithubSyncAutoPush](bool v) { f(v); })
        .build();

    // 操作按钮（右对齐）
    const float btnAreaX = w - kCardPaddingX - kBtnW - 8.0f - kBtnSmallW;
    components::button(ui, id + ".btn.push")
        .position(btnAreaX, y)
        .size(kBtnW, rowH)
        .text("立即同步")
        .fontSize(fs(12.0f))
        .theme(compTheme, /*primary=*/true)
        .radius(t.sizes.radiusSmall)
        .onClick([f = a.githubSyncPush] { f(); })
        .build();
    components::button(ui, id + ".btn.pull")
        .position(btnAreaX + kBtnW + 8.0f, y)
        .size(kBtnSmallW, rowH)
        .text("拉取")
        .fontSize(fs(12.0f))
        .theme(compTheme, /*primary=*/false)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radiusSmall)
        .border(1.0f, t.colors.borderDefault)
        .onClick([f = a.githubSyncPull] { f(); })
        .build();
    y += rowH + roundFs(kRowGap);

    // 状态行
    if (!in.githubSyncStatus.empty()) {
        ui.text(id + ".status")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text(in.githubSyncStatus)
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(in.githubSyncStatusError ? t.colors.error : t.colors.textTertiary)
            .build();
    }
}

} // namespace module::settings