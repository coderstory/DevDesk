// src/module_settings/github_sync_card.cpp
// GitHub Sync 设置卡片实现。
// 使用 EUI-NEO DSL 绘制表单：仓库 URL、分支、Token 输入、启用开关、同步/拉取按钮。
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
#include <components/switch.h>

#include <ui_kit/form_row.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace module::settings {

namespace {

// 行几何常量（100% 档基准，运行期随 roundFs 缩放）；E 批统一：标题 16/22、
// desc y=40/h=16、内容起 y=68，label+control 行迁 ui_kit::formRow（label 110）。
// UAT2 双列布局：5 行纵排 → 3 行——开关行（启用|自动推送）、URL 全宽行（输入 +
// 三操作按钮）、输入行（分支|Token）；卡高 460 → 372（见 settings_ui_logic.h）。
// UAT4 撑满 + 去门控：①输入宽不再设 kFieldW 320 上限——分支/Token 填满所在列
// 控制区、URL 拉满到按钮组（btnAreaX−12，窄窗自动让位）；②开关钉所在列右缘；
// ③未启用不再隐藏字段，改置灰禁用（启用开关本身始终可点）。
inline constexpr float kCardPaddingX = 28.0f;
inline constexpr float kTitleH = 22.0f;
inline constexpr float kDescH = 16.0f;
inline constexpr float kRowH = 36.0f;
inline constexpr float kRowGap = 8.0f;
inline constexpr float kBtnW = 120.0f;
inline constexpr float kBtnSmallW = 80.0f;
inline constexpr float kBtnTestW = 100.0f;
inline constexpr float kDetailBtnW = 64.0f;
inline constexpr float kSwitchW = 44.0f;
inline constexpr float kStatusH = 20.0f;
inline constexpr float kBtnGapX = 8.0f;
// 禁用态档位（UAT4 去门控用）：Input/Switch 组件没有自带 .disabled()，借 DSL
// element 级 disabled（子树不参与命中/聚焦，见 runtime_input.h 的 disabledTree
// 传递）+ 子树 opacity（Stack 的 opacity 级联整棵子树，见 runtime_instances.h
// renderTransform）实现置灰禁用。0.55 对齐 Button 禁用态文字 40% 透明的既有
// 先例（components/button.h disabledTextColor），整树调暗取略轻档。
inline constexpr float kDisabledOpacity = 0.55f;

} // namespace

// ── 同步内容披露（自由函数，便于单测；见 github_sync_card.h）──
// 背景：卡片描述行是用户唯一的知情点，而实际载荷是**整库 SQL dump**（唯一生产者
// github_sync_engine.cpp 调 exportToFile(..., defaultSkipTables())）。旧文案只承诺
// 「笔记和待办」，与实现口径不符 → 这里是「什么上去了、什么没上去」的披露。

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

void drawGithubSyncCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const float h = args.h;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;

    const auto compTheme = core::theme::componentTokensWeChat(t);

    ui.stack(id)
        .size(w, h)
        .content([&] {
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

    // 描述行：披露实际载荷（整库快照 + 不上传清单，由 skip 清单驱动），不再只承诺
    // 「笔记和待办」。单行固定高（kDescH），故文案靠压缩措辞控制长度、不改布局。
    ui.text(id + ".desc")
        .position(kCardPaddingX, y)
        .size(w - kCardPaddingX * 2, roundFs(kDescH))
        .text(githubSyncDisclosureText())
        .fontSize(fs(12.0f))
        .lineHeight(roundFs(kDescH))
        .color(t.colors.textTertiary)
        .build();
    y += roundFs(kDescH) + roundFs(12.0f);

    const float rowH = roundFs(kRowH);
    const float formW = w - kCardPaddingX * 2;
    // UAT2 双列几何：列间距 = 行距 8（8px 网格）；每列宽 = (formW − 列距)/2，右列
    // 外缘与全宽行同贴卡内容右缘。label 仍走 formRow 定宽 110，不在双列内单独收窄
    // ——E 批刚统一的标签列宽不再漂移。
    const float colGap = roundFs(kRowGap);
    const float colW = (formW - colGap) * 0.5f;
    const float col2X = kCardPaddingX + colW + colGap;   // 右列 x

    // 第一行（双列开关行）：启用（左列）| 自动推送（右列）。UAT4 撑满：两开关钉
    // 各自所在列右缘（列内右对齐，行内容横跨整列）。UAT4 去门控：两行开关均
    // 无条件渲染，不再按启用态隐藏右列；未启用时自动推送整行经 lock 包装层
    // 置灰禁用（disabled 子树不可命中/不可聚焦 + 子树 opacity 调暗）。启用
    // 开关本身不加 lock——始终可点，是重新启用的入口。
    ui_kit::formRow(ui, id + ".enabled", "启用", kCardPaddingX, y, colW, rowH,
        [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
            components::toggleSwitch(ui, id + ".enabled.sw")
                .position(cx + cw - kSwitchW, cy + (ch - 20.0f) * 0.5f)
                .size(kSwitchW, 20.0f)
                .checked(in.githubSyncEnabled)
                .theme(compTheme)
                .onChange([f = a.applyGithubSyncEnabled](bool v) { f(v); })
                .build();
        }, t);
    ui.stack(id + ".autopush.lock")
        .position(0.0f, 0.0f)
        .disabled(!in.githubSyncEnabled)
        .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
        .content([&] {
            ui_kit::formRow(ui, id + ".autopush", "自动推送", col2X, y, colW,
                           rowH,
                [&](eui::Ui& ui, float cx, float cy, float cw, float ch) {
                    components::toggleSwitch(ui, id + ".autopush.sw")
                        .position(cx + cw - kSwitchW,
                                  cy + (ch - 20.0f) * 0.5f)
                        .size(kSwitchW, 20.0f)
                        .checked(in.githubSyncAutoPush)
                        .theme(compTheme)
                        .onChange([f = a.applyGithubSyncAutoPush](bool v) { f(v); })
                        .build();
                }, t);
        })
        .build();
    y += rowH + roundFs(kRowGap);

    // （UAT4 去门控）旧版「未启用 → 早退不画后续表单」已废除：下面 URL/分支/
    // Token/按钮全部无条件渲染，未启用时各行经 lock 包装层、按钮经 disabled
    // 呈禁用态——字段一览可见，用户先看清配置再决定是否启用。

    // 操作按钮（右对齐，三个并排：立即同步 / 测试连接 / 拉取）；运行中或未启用
    // （UAT4 去门控后的禁用态）三个一律禁用。提前算 btnAreaX，供 URL 输入宽用。
    const float btnTotalW =
        kBtnW + kBtnTestW + kBtnSmallW + kBtnGapX * 2.0f;
    const float btnAreaX = w - kCardPaddingX - btnTotalW;

    // 第二行（URL 全宽行）：仓库 URL 输入 + 三个操作按钮（右缘对齐）。
    // UAT4 撑满：输入宽拉满到按钮组 = btnAreaX − 输入x − 12（12px 间距），
    // 不再受 320 上限；同一公式内禀承接 UAT3 窄窗让位——712 最小窗自动收窄
    // 到 210、不与按钮组重叠（H1 不回归），max(0,·) 兜负值为下限保护。
    // 未启用时整行经 lock 包装层置灰禁用（输入不可聚焦 = 不可编辑）。
    ui.stack(id + ".url.lock")
        .position(0.0f, 0.0f)
        .disabled(!in.githubSyncEnabled)
        .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
        .content([&] {
            ui_kit::formRow(ui, id + ".url", "仓库 URL", kCardPaddingX, y,
                           formW, rowH,
                [&](eui::Ui& ui, float cx, float cy, float /*cw*/, float ch) {
                    const float inputW =
                        std::max(0.0f, btnAreaX - cx - 12.0f);
                    components::input(ui, id + ".url.input")
                        .position(cx, cy)
                        .size(inputW, ch)
                        .placeholder("https://github.com/user/repo.git")
                        .value(in.githubSyncRepoUrl)
                        .fontSize(t.sizes.fontSizeSmall)
                        .theme(compTheme)
                        .onChange([f = a.applyGithubSyncRepoUrl](const std::string& v) { f(v); })
                        .build();
                }, t);
        })
        .build();

    // 按钮组（btnAreaX 已在上面算好）
    components::button(ui, id + ".btn.push")
        .position(btnAreaX, y)
        .size(kBtnW, rowH)
        .text("立即同步")
        .fontSize(fs(12.0f))
        .theme(compTheme, /*primary=*/true)
        .radius(t.sizes.radiusSmall)
        .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
        .onClick([f = a.githubSyncPush] { f(); })
        .build();
    components::button(ui, id + ".btn.test")
        .position(btnAreaX + kBtnW + kBtnGapX, y)
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
        .position(btnAreaX + kBtnW + kBtnGapX + kBtnTestW + kBtnGapX, y)
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
    y += rowH + roundFs(kRowGap);

    // 第三行（双列输入行）：分支（左列）| Token（右列）。UAT4 撑满：输入宽 =
    // 所在列控制区宽（列宽 − label 110 − 间距 8），不再设 320 上限——宽窗下
    // Token 值 40+ 字符全宽可见；编辑滚动仍由 EUI 单行输入的光标跟随滚动
    // （InputModel::syncScroll）承担。未启用时两行各经 lock 包装层置灰禁用。
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
            ui_kit::formRow(ui, id + ".token", "Token", col2X, y, colW, rowH,
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
    y += rowH + roundFs(kRowGap);

    // 进度行（运行中：阶段文案）
    if (in.githubSyncRunning && !in.githubSyncProgress.empty()) {
        ui.text(id + ".progress")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text(in.githubSyncProgress)
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.textSecondary)
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // 错误行（红字摘要 + 「详情」小按钮）；摘要宽度让出按钮位，避免压字。
    if (!in.githubSyncError.empty()) {
        ui.text(id + ".error")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2 - kDetailBtnW - kBtnGapX,
                  roundFs(kStatusH))
            .text(in.githubSyncError)
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.error)
            .build();
        components::button(ui, id + ".btn.detail")
            .position(w - kCardPaddingX - kDetailBtnW, y)
            .size(kDetailBtnW, roundFs(kStatusH))
            .text("详情")
            .fontSize(fs(11.0f))
            .theme(compTheme, /*primary=*/false)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.error)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([f = a.githubSyncShowErrorDetail] { f(); })
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // 状态行。错误行在显示时接管状态文案（两者内容相同，同显 = 同一句话画
    // 两遍），故 error 非空 → 状态行让位。
    if (!in.githubSyncStatus.empty() && in.githubSyncError.empty()) {
        ui.text(id + ".status")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text(in.githubSyncStatus)
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(in.githubSyncStatusError ? t.colors.error : t.colors.textTertiary)
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // 分支被自动创建提示
    if (!in.githubSyncBranchHint.empty()) {
        ui.text(id + ".branchhint")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text(in.githubSyncBranchHint)
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.warning)
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // public 仓黄字提醒（提示不硬拦）。口径必须与上方描述行同源：同步载荷是**整库快照**
    // 而不只是「笔记、待办」——旧口径把载荷窄化，用户按它做决策就会错判泄露面（sync-02）。
    if (in.githubSyncRepoPublic) {
        ui.text(id + ".publicwarn")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text("仓库为公开（public）：笔记、整库快照与其中的密钥将公开可见")
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.warning)
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // 本地 git 不可用（灰字，不影响同步）
    if (in.githubSyncLocalGitUnavailable) {
        ui.text(id + ".localgit")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text("本地版本历史：不可用")
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.textTertiary)
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // 退出推送未完成：上次退出时有未同步改动 → 黄字提示，同步成功即消失
    if (in.githubSyncUnsyncedNotice) {
        ui.text(id + ".unsynced")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text("有未同步改动（上次退出时未能推送，建议点「立即同步」）")
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.warning)
            .build();
        y += roundFs(kStatusH) + roundFs(4.0f);
    }

    // 删远端被安全栏跳过（本地仍在但读失败）→ 黄字提示
    if (!in.githubSyncDeleteSkipHint.empty()) {
        ui.text(id + ".deleteskip")
            .position(kCardPaddingX, y)
            .size(w - kCardPaddingX * 2, roundFs(kStatusH))
            .text(in.githubSyncDeleteSkipHint)
            .fontSize(fs(11.0f))
            .lineHeight(roundFs(kStatusH))
            .color(t.colors.warning)
            .build();
    }
        })
        .build();
}

} // namespace module::settings