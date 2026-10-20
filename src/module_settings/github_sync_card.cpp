// src/module_settings/github_sync_card.cpp
// GitHub Sync 设置卡片实现（UAT5 方案 A 三段式）。
// 视觉规格 = D:/tmp-icon-candidates/sync-tab-redesign.html 的「方案 A」面板：
// 头部（状态点 + 标题/摘要 + 右侧纵排双开关）→ 仓库配置组 → 操作组（按钮 +
// 进度条）。旧「URL 行右挂三按钮」布局已废：按钮全部下移操作组，URL 输入
// 拉满卡内容右缘；错误摘要上收头部（点头部/错误行开既有 git_error_dialog）。
#include "github_sync_card.h"

#include "github_sync_config.h"
#include "settings_ui_logic.h"

#include <core/component_theme.h>
#include <core/paths.h>
#include <core/sql_dump.h>

#include <eui/app.h>
#include <eui/dsl.h>

#include <components/button.h>
#include <components/input.h>

#include <ui_kit/form_row.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace module::settings {

namespace {

// 行几何常量（100% 档基准，运行期随 roundFs 缩放）；E 批统一：label+control 行
// 迁 ui_kit::formRow（label 110、行高 36）。UAT5 方案 A 三段式：
//   头部 = 状态点 8 + 标题 22 + 摘要 16（左列）｜自动推送 44×24 + 启用主开关
//   52×28 纵排（右列，尺寸为字面值不随字号缩放，卡高兜底见下方 85% 档说明）；
//   配置组 = 组题 16 + URL 全宽行 + 分支|Token 双列；
//   操作组 = 三按钮行 + 右侧进度条（仅运行中）；操作组下方为条件信息行。
// 卡高最坏行序（85%/130% 档复算）见 settings_ui_logic.h 的 kGithubSyncCardHeight。
inline constexpr float kCardPaddingX = 28.0f;
inline constexpr float kCardPaddingY = 16.0f;   // 卡上下留白（字面值，不缩放）
inline constexpr float kTitleH = 22.0f;         // 头部标题行高
inline constexpr float kSummaryGapY = 2.0f;      // 标题与摘要行间距
inline constexpr float kMutedH = 16.0f;          // 摘要行 / 组题 / 开关标签行高
inline constexpr float kRowH = 36.0f;            // 表单行高（formRow 同口径）
inline constexpr float kRowGap = 8.0f;          // 行距（列间距同值）
inline constexpr float kSectionGapY = 12.0f;     // 分段线上下留白
inline constexpr float kBtnW = 120.0f;
inline constexpr float kBtnSmallW = 80.0f;
inline constexpr float kBtnTestW = 100.0f;
inline constexpr float kBtnGapX = 8.0f;
inline constexpr float kStatusH = 20.0f;         // 条件信息行高
// 头部状态点（方案 A .dot）：8px 圆点，已启用=wechat.primary / 错误=
// warningRed / 未启用=灰。
inline constexpr float kDotSize = 8.0f;
inline constexpr float kDotGapX = 8.0f;          // 状态点与标题左缘间距
// 右侧纵排双开关（方案 A 头部右列）：自动推送 44×24（与模块插拔卡开关同口径）
// + 启用主开关 52×28（大一号，管整个功能）。两开关尺寸是设计稿字面值，
// 不随字号缩放——85% 档它们撑住头部高度，卡高常量已按最坏档兜底。
inline constexpr float kAutoSwitchW = 44.0f;
inline constexpr float kAutoSwitchH = 24.0f;
inline constexpr float kMainSwitchW = 52.0f;
inline constexpr float kMainSwitchH = 28.0f;
inline constexpr float kSwitchRowGap = 6.0f;      // 两开关纵排间距
inline constexpr float kSwitchLabelGap = 8.0f;     // 标签右缘与开关左缘间距
inline constexpr float kAutoLabelW = 56.0f;        // 「自动推送」标签盒宽
inline constexpr float kEnableLabelW = 32.0f;     // 「启用」标签盒宽
// 操作组右侧进度（方案 A .bar）：4px 高轨道 + 主色填充 + 阶段文案，仅运行中渲染。
inline constexpr float kProgressBarH = 4.0f;
inline constexpr float kProgressTextW = 200.0f;
inline constexpr float kProgressTextGap = 10.0f;   // 轨道右缘与文案间距
inline constexpr float kProgressLeadGapX = 16.0f;  // 按钮组右缘与轨道左缘间距
inline constexpr float kInfoLeadGapY = 8.0f;       // 操作行与条件信息行的间距
inline constexpr float kInfoRowGap = 4.0f;
// 禁用态档位（UAT4 去门控沿用）：Input/Switch 组件没有自带 .disabled()，借 DSL
// element 级 disabled（子树不参与命中/聚焦）+ 子树 opacity 调暗实现置灰禁用。
// 0.55 对齐 Button 禁用态文字 40% 透明的既有先例，整树调暗取略轻档。
inline constexpr float kDisabledOpacity = 0.55f;

} // namespace

// ── 同步内容披露（自由函数，便于单测；见 github_sync_card.h）──
// 背景：卡内文案是用户唯一的知情点，而实际载荷是**整库 SQL dump**（唯一生产者
// github_sync_engine.cpp 调 exportToFile(..., defaultSkipTables())）。旧文案只承诺
// 「笔记和待办」，与实现口径不符 → 这里是「什么上去了、什么没上去」的披露。
// UAT5 方案 A：披露不再是常驻描述行，而是头部摘要行的**空闲兜底**——无错误且
// 无上次同步状态时（未同步过的初始态）显示，首次使用者仍能看清载荷。

std::string disclosureLabelFor(const std::string& tableName) {
    // 只翻译 defaultSkipTables() 会出现的表；未知表名原样返回，不编造标签。
    if (tableName == "claude_call_logs") return "调用日志";
    if (tableName == "claude_run_settings") return "运行设置";
    // token 就存在 github_sync_state（列 token）；该表在 skip 清单内 → 标签里把
    // 「Token」明写出来，即「token 不上云」这条口径与实现绑定（改 skip 清单会经
    // 防漂移用例提醒）。
    if (tableName == "github_sync_state") return "Token 与同步状态";
    return tableName;
}

std::string githubSyncDisclosureText() {
    // 「不上传」清单由 skip 清单**单一来源**驱动：core::sql_dump::defaultSkipTables()
    // 一变，本文案跟着变，杜绝文案与实现两张皮（防漂移用例是兜底闸）。
    std::string out = "同步笔记与整库数据库快照到 GitHub 私有仓库；不上传：";
    bool first = true;
    for (const auto& table : ::core::sql_dump::defaultSkipTables()) {
        if (!first) out += "、";
        out += disclosureLabelFor(table);
        first = false;
    }
    out += "。";
    return out;
}

float githubSyncProgressFraction(const std::string& stage) {
    // 阶段文案前缀「①..⑨」（U+2460..U+2468，UTF-8 = E2 91 A0..A8）→ 序号/6
    //（引擎阶段最深 ⑤ = 83%，留推进感）；认不出序号 → 起步 10%，轨道上始终
    // 可见活动。进度只有阶段粒度（引擎不发百分比），不伪造数字。
    if (stage.size() >= 3) {
        const auto b0 = static_cast<unsigned char>(stage[0]);
        const auto b1 = static_cast<unsigned char>(stage[1]);
        const auto b2 = static_cast<unsigned char>(stage[2]);
        if (b0 == 0xE2u && b1 == 0x91u && b2 >= 0xA0u && b2 <= 0xA8u) {
            return static_cast<float>(b2 - 0xA0u + 1) / 6.0f;
        }
    }
    return 0.1f;
}

void drawGithubSyncCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);
    const float titleH = roundFs(kTitleH);
    const float mutedH = roundFs(kMutedH);

    ui.stack(id)
        .size(w, h)
        .content([&] {
        // ── 头部：状态点 + 标题/摘要（左）｜纵排双开关（右）──
        const float y0 = kCardPaddingY;
        const bool hasError = !in.githubSyncError.empty();
        const float textL = kCardPaddingX + kDotSize + kDotGapX;   // 44

        // 状态点：已启用=wechat.primary、错误=warningRed、未启用=灰。
        ui.rect(id + ".dot")
            .position(kCardPaddingX, y0 + (titleH - kDotSize) * 0.5f)
            .size(kDotSize, kDotSize)
            .color(hasError
                       ? t.colors.wechat.warningRed
                       : (in.githubSyncEnabled ? t.colors.wechat.primary
                                              : t.colors.wechat.textTertiary))
            .radius(kDotSize * 0.5f)
            .build();

        // 标题行（~15px 加粗，状态后缀随启用态）。错误时点击头部 = 打开
        // 既有 git_error_dialog（弹窗本身不动）；无错误时不挂 onClick。
        {
            auto title = ui.text(id + ".title")
                .position(textL, y0)
                .size(w - textL - kCardPaddingX, titleH)
                .text(in.githubSyncEnabled ? "GitHub 同步 · 已启用"
                                           : "GitHub 同步 · 未启用")
                .fontSize(fs(15.0f))
                .lineHeight(titleH)
                .fontWeight(600)
                .color(t.colors.textPrimary);
            if (hasError) {
                title.onClick([f = a.githubSyncShowErrorDetail] { f(); });
            }
            title.build();
        }

        // 摘要行（muted）：错误一行红字优先（可点，同标题）；否则上次同步状态
        //（沿用既有 status 数据源）；都空 → 披露文案兜底（见上方披露注释）。
        const float summaryY = y0 + titleH + roundFs(kSummaryGapY);
        const float summaryW = w - textL - kCardPaddingX;
        if (hasError) {
            ui.text(id + ".error")
                .position(textL, summaryY)
                .size(summaryW, mutedH)
                .text(in.githubSyncError)
                .fontSize(fs(12.0f))
                .lineHeight(mutedH)
                .color(t.colors.error)
                .maxWidth(summaryW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .onClick([f = a.githubSyncShowErrorDetail] { f(); })
                .build();
        } else {
            std::string summary = in.githubSyncStatus.empty()
                ? githubSyncDisclosureText()
                : in.githubSyncStatus;
            ui.text(id + ".summary")
                .position(textL, summaryY)
                .size(summaryW, mutedH)
                .text(summary)
                .fontSize(fs(12.0f))
                .lineHeight(mutedH)
                .color(t.colors.textTertiary)
                .maxWidth(summaryW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();
        }

        // 右侧纵排双开关：自动推送（上；未启用时整组经 lock 包装层置灰禁用，
        // disabled 子树不可命中/不可聚焦 + opacity 调暗）；启用主开关（下，
        // 52×28 大一号）不加 lock——始终可点，是重新启用的入口。
        const float autoSwY = y0;
        const float enableSwY = y0 + kAutoSwitchH + roundFs(kSwitchRowGap);
        const float autoSwX = w - kCardPaddingX - kAutoSwitchW;
        const float enableSwX = w - kCardPaddingX - kMainSwitchW;
        ui.stack(id + ".autopush.lock")
            .position(0.0f, 0.0f)
            .disabled(!in.githubSyncEnabled)
            .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
            .content([&] {
                ui.text(id + ".autopush.label")
                    .position(autoSwX - kSwitchLabelGap - kAutoLabelW,
                              autoSwY + (kAutoSwitchH - mutedH) * 0.5f)
                    .size(kAutoLabelW, mutedH)
                    .text("自动推送")
                    .fontSize(fs(12.0f))
                    .lineHeight(mutedH)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textTertiary)
                    .build();
                ui_kit::wswitch(ui, id + ".autopush.sw", in.githubSyncAutoPush,
                                autoSwX, autoSwY, kAutoSwitchW, kAutoSwitchH, t,
                                [f = a.applyGithubSyncAutoPush](bool v) {
                                    f(v);
                                });
            })
            .build();
        ui.text(id + ".enabled.label")
            .position(enableSwX - kSwitchLabelGap - kEnableLabelW,
                      enableSwY + (kMainSwitchH - mutedH) * 0.5f)
            .size(kEnableLabelW, mutedH)
            .text("启用")
            .fontSize(fs(12.0f))
            .lineHeight(mutedH)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textTertiary)
            .build();
        ui_kit::wswitch(ui, id + ".enabled.sw", in.githubSyncEnabled,
                       enableSwX, enableSwY, kMainSwitchW, kMainSwitchH, t,
                       [f = a.applyGithubSyncEnabled](bool v) { f(v); });

        // 纵向游标：头部内容底 = max(左列标题+摘要, 右侧开关纵排)——
        // 85% 档开关纵排（24+5+28，字面值）高于缩放后的左列，取 max 保住。
        float y = std::max(y0 + titleH + roundFs(kSummaryGapY) + mutedH,
                           enableSwY + kMainSwitchH);
        const float formW = w - kCardPaddingX * 2.0f;
        const float rowH = roundFs(kRowH);
        // 双列几何沿用：列间距 = 行距 8（8px 网格）；每列宽 = (formW − 列距)/2，
        // 右列外缘与全宽行同贴卡内容右缘；label 走 formRow 定宽 110。
        const float colGap = roundFs(kRowGap);
        const float colW = (formW - colGap) * 0.5f;
        const float col2X = kCardPaddingX + colW + colGap;

        // 分段线（方案 A 三段间的 1px 分隔）：上下各留 kSectionGapY。
        const auto sectionDivider = [&](const char* suffix) {
            ui.rect(id + suffix)
                .position(kCardPaddingX, y + roundFs(kSectionGapY))
                .size(formW, 1.0f)
                .color(t.colors.wechat.dividerPrimary)
                .build();
            y += roundFs(kSectionGapY) + 1.0f + roundFs(kSectionGapY);
        };

        // ── 仓库配置组：组题 + URL 全宽行 + 分支|Token 双列 ──
        sectionDivider(".div.head");
        ui.text(id + ".group.config")
            .position(kCardPaddingX, y)
            .size(formW, mutedH)
            .text("仓库配置")
            .fontSize(fs(12.0f))
            .lineHeight(mutedH)
            .color(t.colors.textTertiary)
            .build();
        y += mutedH + roundFs(kRowGap);

        // URL 全宽行：按钮已下移操作组（UAT5），输入拉满卡内容右缘（w−28），
        // 无按钮组让位（H1 让位断言随旧布局一并废除）。未启用时经 lock 置灰。
        ui.stack(id + ".url.lock")
            .position(0.0f, 0.0f)
            .disabled(!in.githubSyncEnabled)
            .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
            .content([&] {
                ui_kit::formRow(ui, id + ".url", "仓库 URL", kCardPaddingX, y,
                                formW, rowH,
                    [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                        components::input(ui, id + ".url.input")
                            .position(cx, cy)
                            .size(cw, ch)
                            .placeholder("https://github.com/user/repo.git")
                            .value(in.githubSyncRepoUrl)
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(compTheme)
                            .onChange([f = a.applyGithubSyncRepoUrl](const std::string& v) { f(v); })
                            .build();
                    }, t);
            })
            .build();
        y += rowH + roundFs(kRowGap);

        // 分支 | Token 双列：输入宽 = 所在列控制区宽（列宽 − label 110 − 间距 8），
        // 撑满语义沿用 UAT4（无 320 上限，宽窗 Token 值全宽可见）。各经 lock 置灰。
        ui.stack(id + ".branch.lock")
            .position(0.0f, 0.0f)
            .disabled(!in.githubSyncEnabled)
            .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
            .content([&] {
                ui_kit::formRow(ui, id + ".branch", "分支", kCardPaddingX, y,
                                colW, rowH,
                    [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                        components::input(ui, id + ".branch.input")
                            .position(cx, cy)
                            .size(cw, ch)
                            .placeholder("main")
                            .value(in.githubSyncBranch)
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(compTheme)
                            .onChange([f = a.applyGithubSyncBranch](const std::string& v) { f(v); })
                            .build();
                    }, t);
            })
            .build();
        ui.stack(id + ".token.lock")
            .position(0.0f, 0.0f)
            .disabled(!in.githubSyncEnabled)
            .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
            .content([&] {
                ui_kit::formRow(ui, id + ".token", "Token", col2X, y,
                                colW, rowH,
                    [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                        components::input(ui, id + ".token.input")
                            .position(cx, cy)
                            .size(cw, ch)
                            .placeholder("ghp_xxxxxxxxxxxxxxxxxxxx")
                            .value(in.githubSyncTokenDraft)
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(compTheme)
                            .onChange([f = a.setGithubSyncTokenDraft](const std::string& v) { f(v); })
                            .build();
                    }, t);
            })
            .build();
        y += rowH;

        // ── 操作组：三按钮（左）+ 进度（右，仅运行中）──
        sectionDivider(".div.actions");
        components::button(ui, id + ".btn.push")
            .position(kCardPaddingX, y)
            .size(kBtnW, rowH)
            .text("立即同步")
            .fontSize(fs(12.0f))
            .theme(compTheme, /*primary=*/true)
            .radius(t.sizes.radiusSmall)
            .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
            .onClick([f = a.githubSyncPush] { f(); })
            .build();
        components::button(ui, id + ".btn.test")
            .position(kCardPaddingX + kBtnW + kBtnGapX, y)
            .size(kBtnTestW, rowH)
            .text("测试连接")
            .fontSize(fs(12.0f))
            .theme(compTheme, /*primary=*/false)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
            .onClick([f = a.githubSyncTestConnection] { f(); })
            .build();
        components::button(ui, id + ".btn.pull")
            .position(kCardPaddingX + kBtnW + kBtnGapX + kBtnTestW + kBtnGapX, y)
            .size(kBtnSmallW, rowH)
            .text("拉取")
            .fontSize(fs(12.0f))
            .theme(compTheme, /*primary=*/false)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
            .onClick([f = a.githubSyncPull] { f(); })
            .build();

        // 进度条（右）：4px 轨道 + 主色填充（填充比 = 阶段序号/6）+「同步中 ·
        // <阶段>」文案；空闲时右侧留空（整组不渲染）。
        if (in.githubSyncRunning) {
            const float btnRight = kCardPaddingX + kBtnW + kBtnTestW +
                                   kBtnSmallW + kBtnGapX * 2.0f;
            const float textX = w - kCardPaddingX - kProgressTextW;
            const float barX = btnRight + roundFs(kProgressLeadGapX);
            const float barW =
                std::max(0.0f, textX - kProgressTextGap - barX);
            const float barY = y + (rowH - kProgressBarH) * 0.5f;
            ui.rect(id + ".progress.bar")
                .position(barX, barY)
                .size(barW, kProgressBarH)
                .color(t.colors.wechat.dividerSecondary)
                .radius(kProgressBarH * 0.5f)
                .build();
            ui.rect(id + ".progress.fill")
                .position(barX, barY)
                .size(barW * githubSyncProgressFraction(in.githubSyncProgress),
                      kProgressBarH)
                .color(t.colors.wechat.primary)
                .radius(kProgressBarH * 0.5f)
                .build();
            ui.text(id + ".progress")
                .position(textX, y + (rowH - mutedH) * 0.5f)
                .size(kProgressTextW, mutedH)
                .text(in.githubSyncProgress.empty()
                          ? std::string("同步中…")
                          : "同步中 · " + in.githubSyncProgress)
                .fontSize(fs(11.0f))
                .lineHeight(mutedH)
                .color(t.colors.textSecondary)
                .maxWidth(kProgressTextW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();
        }
        y += rowH;

        // ── 条件信息行（操作组下方纵排；id 沿用 UAT4）──
        y += roundFs(kInfoLeadGapY);
        const float infoH = roundFs(kStatusH);
        const auto infoRow = [&](const char* suffix, const std::string& text,
                                  const eui::Color& color) {
            ui.text(id + suffix)
                .position(kCardPaddingX, y)
                .size(formW, infoH)
                .text(text)
                .fontSize(fs(11.0f))
                .lineHeight(infoH)
                .color(color)
                .build();
            y += infoH + roundFs(kInfoRowGap);
        };
        // 分支被自动创建提示
        if (!in.githubSyncBranchHint.empty()) {
            infoRow(".branchhint", in.githubSyncBranchHint, t.colors.warning);
        }
        // public 仓黄字提醒（提示不硬拦）。口径必须与披露文案同源：同步载荷是
        // **整库快照**而不只是「笔记、待办」（sync-02）。
        if (in.githubSyncRepoPublic) {
            infoRow(".publicwarn",
                    "仓库为公开（public）：笔记、整库快照与其中的密钥将公开可见",
                    t.colors.warning);
        }
        // 本地 git 不可用（灰字，不影响同步）
        if (in.githubSyncLocalGitUnavailable) {
            infoRow(".localgit", "本地版本历史：不可用", t.colors.textTertiary);
        }
        // 退出推送未完成：上次退出时有未同步改动 → 黄字提示，同步成功即消失
        if (in.githubSyncUnsyncedNotice) {
            infoRow(".unsynced",
                    "有未同步改动（上次退出时未能推送，建议点「立即同步」）",
                    t.colors.warning);
        }
        // 删远端被安全栏跳过（本地仍在但读失败）→ 黄字提示
        if (!in.githubSyncDeleteSkipHint.empty()) {
            infoRow(".deleteskip", in.githubSyncDeleteSkipHint, t.colors.warning);
        }
        })
        .build();
}

} // namespace module::settings
