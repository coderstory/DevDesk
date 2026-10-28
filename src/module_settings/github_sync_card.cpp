// src/module_settings/github_sync_card.cpp
// GitHub Sync 设置卡片实现（UAT5 方案 A 三段式）。
// 视觉规格 = D:/tmp-icon-candidates/sync-tab-redesign.html 的「方案 A」面板：
// 头部（状态点 + 标题/摘要）→ 启用/自动推送两整行开关行 → 仓库配置组 → 操作组
//（按钮 + 进度条）。旧「URL 行右挂三按钮」布局已废：按钮全部下移操作组，URL 输入
// 拉满卡内容右缘；错误摘要上收头部（点头部/错误行开既有 git_error_dialog）。
// UAT6：头部右侧纵排双开关（真机反馈太挤）拆成各自一整行（模块插拔卡开关行
// 先例：label 左块、开关钉卡内容右缘、行宽=卡内宽弹性契约）。
// UAT6 最小窗零滚动收口（用户：「同步 tab 出滚动条完全没必要」）：两开关行的
// 说明文案行删除（label 自承担语义）、「仓库配置」组题行删除（分段线已分节、
// 三个表单 label 自明）、行距/分段留白/信息行/卡上下留白按档收紧——卡高常量
// 496 → 372 → 362。
// H28（2026-10-28 两全拍板）：UAT6 收口把间距压到设计系统令牌以下（行距 6/
// 分段 5/信息行间 2/标题摘要距 1），真机反馈「过于紧凑」。按设计系统 v1.0
// 间距令牌恢复（4/8/12/16，见行几何常量注释），披露折叠链接并入摘要行右端
//（省独立行、消除与启用行的重叠），卡高常量 420 = S4.2 后最小窗可用高恰好值
// ——100%/85% 档零滚动契约不变（复算见 settings_ui_logic.h kGithubSyncCardHeight）。
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

#include <ui_kit/card.h>
#include <ui_kit/form_row.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace module::settings {

namespace {

// 行几何常量（100% 档基准，运行期随 roundFs 缩放）；E 批统一：label+control 行
// 迁 ui_kit::formRow（label 110、行高 36）。UAT5 方案 A 三段式 + UAT6 开关行 +
// H28 间距令牌恢复（设计系统 v1.0：行距 8 / 分段 12 / 信息行间 4 / 卡上下留白
// 16 / 标题摘要距 4——UAT6 收口压到 6/5/2/1/8，真机反馈过于紧凑）：
//   头部 = 状态点 8 + 标题 20 + 摘要 16（左列，折叠链接并入摘要行右端）+
//   启用/自动推送两整行开关行（行盒 24 = 主开关高，label 垂直居中于行盒；
//   开关钉卡内容右缘，尺寸 44×24 字面值不随字号缩放）；
//   配置组 = URL 全宽行 + 分支|Token 双列（组题行已删，分段线已分节）；
//   操作组 = 左侧进度（条+文案，仅运行中）+ 右侧三按钮组（按钮组右锚）；
//   操作组下方为条件信息行。
// 卡高最坏行序（85%/115%/130% 档复算）见 settings_ui_logic.h 的 kGithubSyncCardHeight。
inline constexpr float kCardPaddingX = 28.0f;
inline constexpr float kCardPaddingY = 16.0f;   // 卡上下留白（字面值不缩放；常规间距令牌 16，零滚动收口 16→8，H28 恢复）
inline constexpr float kTitleH = 20.0f;         // 头部标题行高（零滚动收口 22→20）
inline constexpr float kSummaryGapY = 4.0f;      // 标题与摘要行间距（超小间距令牌 4；零滚动收口 2→1，H28 取 4）
inline constexpr float kMutedH = 16.0f;          // 摘要行高
inline constexpr float kSummaryToggleGap = 8.0f; // 摘要文本与折叠链接间距（小间距令牌 8）
inline constexpr float kSummaryToggleW = 72.0f;  // 折叠链接盒宽（「查看/收起详细说明」11px × 6 字 + 余量）
inline constexpr float kRowH = 36.0f;            // 表单行高（formRow 同口径）
inline constexpr float kToggleRowH = 24.0f;     // 开关行高（N 项: 28→24, = 主开关 24）
inline constexpr float kRowGap = 8.0f;          // 行距（小间距令牌 8；列间距同值；零滚动收口 8→6，H28 恢复）
inline constexpr float kSectionGapY = 12.0f;    // 分段线上下留白（中间距令牌 12；零滚动收口 12→5，H28 恢复）
// H18 横向弹性锚（S4.1：1000×640 为锚点）：三按钮定宽不伸，按钮组整体右锚
// 卡内容右缘（组右缘 = w − 28）；宽窗增量全部让给左侧进度文案列（flex）。
inline constexpr float kBtnW = 120.0f;
inline constexpr float kBtnSmallW = 80.0f;
inline constexpr float kBtnTestW = 100.0f;
inline constexpr float kBtnGapX = 8.0f;
inline constexpr float kStatusH = 16.0f;         // 条件信息行高（零滚动收口 20→16）
// 头部状态点（方案 A .dot）：8px 圆点，已启用=wechat.primary / 错误=
// warningRed / 未启用=灰。
inline constexpr float kDotSize = 8.0f;
inline constexpr float kDotGapX = 8.0f;          // 状态点与标题左缘间距
// 头部两整行开关行（UAT6，模块插拔卡开关行先例）：「启用」与「自动推送」
// 均 44×24 微信绿 wswitch（与 settings 其他 tab 开关同口径，Module 开关行同
// 尺寸先例：settings_ui_cards.cpp toggleSwitch 44×24）。N 项统一大小——不再有大
// 一圈的 52×28。开关尺寸是设计稿字面值，不随字号缩放。
inline constexpr float kAutoSwitchW = 44.0f;
inline constexpr float kAutoSwitchH = 24.0f;
inline constexpr float kMainSwitchW = 44.0f;   // N 项: 52→44, 与自动推送同尺寸
inline constexpr float kMainSwitchH = 24.0f;   // N 项: 28→24, 对齐标准 wswitch
inline constexpr float kSwitchLabelGap = 8.0f;     // 行文本块右缘与开关左缘间距
inline constexpr float kToggleLabelH = 18.0f;      // 开关行 label 高（模块卡 name 同口径）
// 操作组左侧进度（方案 A .bar，H18 按钮组右锚后移到左侧剩余区）：4px 高定宽
// 轨道 + 主色填充 + 阶段文案，仅运行中渲染。轨道定宽 130 = 1000×640 最小档
//（页宽 792 = 1000 − 208 侧栏 → 卡宽 728）旧布局的轨道宽，文案列 200 与之同宽
// ——最小档进度分界与改前逐值一致；文案列改 flex 占剩余宽，宽窗增量全进文案。
inline constexpr float kProgressBarH = 4.0f;
inline constexpr float kProgressBarW = 130.0f;
inline constexpr float kProgressTextGap = 10.0f;   // 轨道右缘与文案间距
inline constexpr float kProgressLeadGapX = 16.0f;  // 进度区右缘与按钮组左缘间距
inline constexpr float kInfoLeadGapY = 8.0f;       // 操作行与条件信息行间距（小间距令牌 8；零滚动收口 8→6，H28 恢复）
inline constexpr float kInfoRowGap = 4.0f;         // 信息行行间（超小间距令牌 4；零滚动收口 4→2，H28 恢复；仅行间无行尾距）
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
        // S6.1 玻璃退役→白卡 + Sm 阴影 + H10 描边 + H15 轻微半透（kCardGlassAlpha）
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f).size(w, h)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f, 0.02f))
            .border(1.0f, t.colors.borderDefault)
            .build();

        // ── 头部：状态点 + 标题/摘要（左列，宽度全展开）→ 启用/自动推送两整行 ──
        const float y0 = kCardPaddingY;
        const bool hasError = !in.githubSyncError.empty();
        const float textL = kCardPaddingX + kDotSize + kDotGapX;   // 44

        // 状态点：已启用=wechat.primary、错误=warningRed、未启用=灰。
        ui.rect(id + ".dot")
            .position(kCardPaddingX, y0 + (titleH - kDotSize) * 0.5f)
            .size(kDotSize, kDotSize)
            .color(hasError
                       ? t.colors.error
                       : (in.githubSyncEnabled ? t.colors.primary
                                              : t.colors.textTertiary))
            .radius(kDotSize * 0.5f)
            .build();

        // 标题行（~15px 加粗，状态后缀随启用态）。不挂 onClick——
        // H1: 错误详情入口已移入摘要行的「查看详情」，标题不再双击即弹。
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
            title.build();
        }

        // 摘要行（muted）：错误一行优先——Warning 色+图标警示条 +「查看详情」可点；
        // 否则上次同步状态（沿用既有 status 数据源）；都空 → 披露文案兜底。
        const float summaryY = y0 + titleH + roundFs(kSummaryGapY);
        const float summaryW = w - textL - kCardPaddingX;
        if (hasError) {
            // H1: 页内警示条——警告图标 + 黄字错误摘要 +「查看详情」文字链接
            const float detailW = roundFs(56.0f);     // "查看详情" 宽度
            const float iconW = roundFs(14.0f);       // 警告图标宽
            const float iconGap = 3.0f;                // 图标与文本间距
            const float errTextW = std::max(0.0f, summaryW - iconW - iconGap - detailW - 4.0f);
            // 警告图标（fa-triangle-exclamation，Warning 色 #F59E0B）
            ui.text(id + ".error.icon")
                .position(textL, summaryY + (mutedH - 12.0f) * 0.5f)
                .size(iconW, 12.0f)
                .icon(0xF071)
                .fontSize(fs(12.0f))
                .color(t.colors.warning)
                .build();
            // 错误摘要文本（Warning 色）
            ui.text(id + ".error")
                .position(textL + iconW + iconGap, summaryY)
                .size(errTextW, mutedH)
                .text(in.githubSyncError)
                .fontSize(fs(12.0f))
                .lineHeight(mutedH)
                .color(t.colors.warning)
                .maxWidth(errTextW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();
            //「查看详情」——唯一弹窗入口
            ui.text(id + ".error.detail")
                .position(textL + iconW + iconGap + errTextW + roundFs(4.0f), summaryY)
                .size(detailW, mutedH)
                .text("查看详情")
                .fontSize(fs(12.0f))
                .lineHeight(mutedH)
                .fontWeight(500)
                .color(t.colors.primary)
                .onClick([f = a.githubSyncShowErrorDetail] { f(); })
                .build();
        } else {
            // S3.7#4: 同步说明可折叠——默认紧凑摘要，展开后显示完整披露。
            // H28: 折叠链接并入摘要行右端（与错误行「查看详情」同模，右缘 = 卡
            // 内容右缘）。原 S3.7#4 把链接放摘要下方独立行，但行序公式未记账该
            // 行（enableRowY 只加 summaryY+mutedH+rowGap），独立行与启用行重叠
            // ——并入同行使披露区恒 1 行：重叠消除、零滚动预算不再为它加码。
            if (in.githubSyncStatus.empty()) {
                const char* compactSummary = "同步笔记与整库数据库快照到 GitHub 私有仓库";
                std::string summaryText = in.githubSyncDetailExpanded
                    ? githubSyncDisclosureText()
                    : compactSummary;
                const float toggleW = roundFs(kSummaryToggleW);
                const float textW = std::max(
                    0.0f, summaryW - toggleW - roundFs(kSummaryToggleGap));
                ui.text(id + ".summary")
                    .position(textL, summaryY)
                    .size(textW, mutedH)
                    .text(summaryText)
                    .fontSize(fs(12.0f))
                    .lineHeight(mutedH)
                    .color(t.colors.textTertiary)
                    .maxWidth(textW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
                // 折叠/展开链接（Ghost 风格文字链接，摘要行右端）
                auto toggle = ui.text(id + ".summary.toggle")
                    .position(textL + summaryW - toggleW, summaryY)
                    .size(toggleW, mutedH)
                    .text(in.githubSyncDetailExpanded ? "收起详细说明" : "查看详细说明")
                    .fontSize(fs(11.0f))
                    .lineHeight(mutedH)
                    .color(t.colors.primary);
                if (a.toggleGithubSyncDetail) {
                    toggle.onClick([f = a.toggleGithubSyncDetail] { f(); });
                }
                toggle.build();
            } else {
                ui.text(id + ".summary")
                    .position(textL, summaryY)
                    .size(summaryW, mutedH)
                    .text(in.githubSyncStatus)
                    .fontSize(fs(12.0f))
                    .lineHeight(mutedH)
                    .color(t.colors.textTertiary)
                    .maxWidth(summaryW)
                    .overflow(core::TextOverflow::Ellipsis)
                    .clip()
                    .build();
            }
        }

        // ── 启用 / 自动推送：各成一整行（UAT6，模块插拔卡开关行先例）──
        // 行宽 = 卡内宽（弹性契约，无固定帽）：label 左块贴卡左缘、右缘让
        // 位开关（间距 8），开关钉卡内容右缘、行盒内垂直居中。启用行在上（总
        // 开关，不加 lock——始终可点，是重新启用的入口）；自动推送行在下，整行
        // 经 lock 包装层置灰禁用（disabled 子树不可命中/不可聚焦 + opacity 调暗）。
        // UAT6 零滚动收口：说明文案行删除（label 自承担语义，开关行语义不变），
        // 行盒 36→28 贴主开关高。行底仍取 max(行盒, 文本块, 开关)——防小字号档
        // roundFs 舍入把 28 高主开关挤出缩放后的行盒。
        const float rowH = roundFs(kRowH);
        const float toggleRowH = roundFs(kToggleRowH);
        const float rowGap = roundFs(kRowGap);
        const float toggleLabelH = roundFs(kToggleLabelH);
        const auto toggleRowText = [&](const char* suffix, const char* label,
                                       float swW, float rowY) {
            const float textW =
                std::max(0.0f, w - kCardPaddingX * 2.0f - swW -
                                   roundFs(kSwitchLabelGap));
            ui.text(id + suffix + ".label")
                .position(kCardPaddingX,
                          rowY + (toggleRowH - toggleLabelH) * 0.5f)
                .size(textW, toggleLabelH)
                .text(label)
                .fontSize(fs(13.0f))
                .lineHeight(toggleLabelH)
                .fontWeight(500)
                .color(t.colors.textPrimary)
                .maxWidth(textW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();
            return rowY + (toggleRowH + toggleLabelH) * 0.5f;
        };
        const float enableRowY = summaryY + mutedH + rowGap;
        const float enableSwY = enableRowY + (toggleRowH - kMainSwitchH) * 0.5f;
        const float enableTextBottom = toggleRowText(
            ".enabled", "启用", kMainSwitchW, enableRowY);
        ui_kit::wswitch(ui, id + ".enabled.sw", in.githubSyncEnabled,
                        w - kCardPaddingX - kMainSwitchW, enableSwY,
                        kMainSwitchW, kMainSwitchH, t,
                        [f = a.applyGithubSyncEnabled](bool v) { f(v); });
        const float autoRowY =
            std::max(enableRowY + toggleRowH,
                     std::max(enableTextBottom, enableSwY + kMainSwitchH)) +
            rowGap;
        const float autoSwY = autoRowY + (toggleRowH - kAutoSwitchH) * 0.5f;
        const float autoSwX = w - kCardPaddingX - kAutoSwitchW;
        float autoTextBottom = 0.0f;
        ui.stack(id + ".autopush.lock")
            .position(0.0f, 0.0f)
            .disabled(!in.githubSyncEnabled)
            .opacity(in.githubSyncEnabled ? 1.0f : kDisabledOpacity)
            .content([&] {
                autoTextBottom = toggleRowText(
                    ".autopush", "自动推送", kAutoSwitchW, autoRowY);
                ui_kit::wswitch(ui, id + ".autopush.sw", in.githubSyncAutoPush,
                                autoSwX, autoSwY, kAutoSwitchW, kAutoSwitchH, t,
                                [f = a.applyGithubSyncAutoPush](bool v) {
                                    f(v);
                                });
            })
            .build();

        // 纵向游标：头部内容底 = 自动推送行底（已含 max(行盒/文本块/开关)）。
        float y = std::max(autoRowY + toggleRowH,
                           std::max(autoTextBottom, autoSwY + kAutoSwitchH));
        const float formW = w - kCardPaddingX * 2.0f;
        // 双列几何沿用：列间距 = 行距（kRowGap = 小间距令牌 8，H28 恢复）；每列宽 =
        // (formW − 列距)/2，右列外缘与全宽行同贴卡内容右缘；label 走 formRow 定宽 110。
        const float colGap = roundFs(kRowGap);
        const float colW = (formW - colGap) * 0.5f;
        const float col2X = kCardPaddingX + colW + colGap;

        // 分段线（方案 A 三段间的 1px 分隔）：上下各留 kSectionGapY。
        const auto sectionDivider = [&](const char* suffix) {
            ui.rect(id + suffix)
                .position(kCardPaddingX, y + roundFs(kSectionGapY))
                .size(formW, 1.0f)
                .color(t.colors.borderDefault)
                .build();
            y += roundFs(kSectionGapY) + 1.0f + roundFs(kSectionGapY);
        };

        // ── 仓库配置组：URL 全宽行 + 分支|Token 双列 ──
        // UAT6 零滚动收口：「仓库配置」组题行删除（−22px）——分段线已分节、三个
        // 表单 label（仓库 URL/分支/Token）自明，不损失信息量。
        sectionDivider(".div.head");

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
                        // S3.7#6: 输入右给眼睛图标让位
                        const float eyeW = 20.0f;
                        const float inputW = std::max(0.0f, cw - eyeW - 4.0f);
                        components::input(ui, id + ".token.input")
                            .position(cx, cy)
                            .size(inputW, ch)
                            .placeholder("ghp_xxxxxxxxxxxxxxxxxxxx")
                            .value(in.githubSyncTokenVisible
                                       ? in.githubSyncTokenDraft
                                       : std::string(in.githubSyncTokenDraft.size(),
                                                     '*'))
                            .fontSize(t.sizes.fontSizeSmall)
                            .theme(compTheme)
                            .onChange([f = a.setGithubSyncTokenDraft](const std::string& v) { f(v); })
                            .build();
                        // 眼睛显隐图标
                        auto eye = ui.text(id + ".token.eye")
                            .position(cx + inputW + 4.0f, cy + (ch - 16.0f) * 0.5f)
                            .size(eyeW, 16.0f)
                            .icon(in.githubSyncTokenVisible ? 0xF070 : 0xF06E)
                            .fontSize(fs(14.0f))
                            .color(t.colors.textTertiary);
                        if (a.toggleGithubSyncTokenVisibility) {
                            eye.onClick([f = a.toggleGithubSyncTokenVisibility] { f(); });
                        }
                        eye.build();
                    }, t);
            })
            .build();
        y += rowH;

        // ── 操作组：左侧进度（仅运行中）+ 右侧三按钮组（右锚）──
        // H18 横向弹性（S4.1）：按钮定宽不伸，组右缘钉卡内容右缘 w − 28；
        // 组左缘 = w − 28 − (120 + 8 + 100 + 8 + 80) = w − 344
        //（1000×640 最小档：页宽 792 − 64 → 卡 728 → 组左缘 384）。
        sectionDivider(".div.actions");
        const float btnPullX = w - kCardPaddingX - kBtnSmallW;
        const float btnTestX = btnPullX - kBtnGapX - kBtnTestW;
        const float btnPushX = btnTestX - kBtnGapX - kBtnW;
        components::button(ui, id + ".btn.push")
            .position(btnPushX, y)
            .size(kBtnW, rowH)
            .text("立即同步")
            .fontSize(t.sizes.fontSizeBody)
            .theme(compTheme, /*primary=*/true)
            .radius(t.sizes.radius)
            .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
            .onClick([f = a.githubSyncPush] { f(); })
            .build();
        components::button(ui, id + ".btn.test")
            .position(btnTestX, y)
            .size(kBtnTestW, rowH)
            .text("测试连接")
            .fontSize(t.sizes.fontSizeBody)
            .theme(compTheme, /*primary=*/false)
            .colors(t.colors.surface, t.colors.primarySoftBg,
                    components::theme::withOpacity(t.colors.primaryPressed, 0.15f))
            .textColor(t.colors.primary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.primary)
            .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
            .onClick([f = a.githubSyncTestConnection] { f(); })
            .build();
        components::button(ui, id + ".btn.pull")
            .position(btnPullX, y)
            .size(kBtnSmallW, rowH)
            .text("拉取")
            .fontSize(t.sizes.fontSizeBody)
            .theme(compTheme, /*primary=*/false)
            .colors(t.colors.surface, t.colors.primarySoftBg,
                    components::theme::withOpacity(t.colors.primaryPressed, 0.15f))
            .textColor(t.colors.primary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.primary)
            .disabled(!in.githubSyncEnabled || in.githubSyncRunning)
            .onClick([f = a.githubSyncPull] { f(); })
            .build();

        // 进度（按钮组左侧剩余区）：轨道定宽 130 + 主色填充（填充比 = 阶段序号/6）
        // +「同步中 · <阶段>」文案列 flex（宽 = 进度区宽 − 轨道 − 10）；空闲整组
        // 不渲染。进度区右缘 = 按钮组左缘 − 16。
        if (in.githubSyncRunning) {
            const float progressRight = btnPushX - roundFs(kProgressLeadGapX);
            const float barX = kCardPaddingX;
            const float barW = kProgressBarW;
            const float textX = barX + barW + kProgressTextGap;
            const float textW = std::max(0.0f, progressRight - textX);
            const float barY = y + (rowH - kProgressBarH) * 0.5f;
            ui.rect(id + ".progress.bar")
                .position(barX, barY)
                .size(barW, kProgressBarH)
                .color(t.colors.hover)
                .radius(kProgressBarH * 0.5f)
                .build();
            ui.rect(id + ".progress.fill")
                .position(barX, barY)
                .size(barW * githubSyncProgressFraction(in.githubSyncProgress),
                      kProgressBarH)
                .color(t.colors.primary)
                .radius(kProgressBarH * 0.5f)
                .build();
            ui.text(id + ".progress")
                .position(textX, y + (rowH - mutedH) * 0.5f)
                .size(textW, mutedH)
                .text(in.githubSyncProgress.empty()
                          ? std::string("同步中…")
                          : "同步中 · " + in.githubSyncProgress)
                .fontSize(fs(11.0f))
                .lineHeight(mutedH)
                .color(t.colors.textSecondary)
                .maxWidth(textW)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();
        }
        y += rowH;

        // ── 条件信息行（操作组下方纵排；id 沿用 UAT4）──
        // 行高 20→16（零滚动收口）；行距 4（超小间距令牌，H28 恢复）且**仅行间**
        //（旧行尾距 +4 是纯浪费的记账高度，五条全显的最坏组合直接吃进卡高常量）。
        y += roundFs(kInfoLeadGapY);
        const float infoH = roundFs(kStatusH);
        const float infoGap = roundFs(kInfoRowGap);
        bool infoFirst = true;
        const auto infoRow = [&](const char* suffix, const std::string& text,
                                  const eui::Color& color, bool showIcon = false) {
            if (!infoFirst) y += infoGap;
            infoFirst = false;
            if (showIcon) {
                // S3.7#4: warning 信息行前加 fa-triangle-exclamation 图标
                ui.text(id + std::string(suffix) + ".icon")
                    .position(kCardPaddingX, y)
                    .size(14.0f, infoH)
                    .icon(0xF071)  // fa-triangle-exclamation
                    .fontSize(fs(11.0f))
                    .lineHeight(infoH)
                    .color(color)
                    .build();
                ui.text(id + suffix)
                    .position(kCardPaddingX + 16.0f, y)
                    .size(formW - 16.0f, infoH)
                    .text(text)
                    .fontSize(fs(11.0f))
                    .lineHeight(infoH)
                    .color(color)
                    .build();
            } else {
                ui.text(id + suffix)
                    .position(kCardPaddingX, y)
                    .size(formW, infoH)
                    .text(text)
                    .fontSize(fs(11.0f))
                    .lineHeight(infoH)
                    .color(color)
                    .build();
            }
            y += infoH;
        };
        // 分支被自动创建提示
        if (!in.githubSyncBranchHint.empty()) {
            infoRow(".branchhint", in.githubSyncBranchHint, t.colors.warning, true);
        }
        // public 仓黄字提醒（提示不硬拦）。口径必须与披露文案同源：同步载荷是
        // **整库快照**而不只是「笔记、待办」（sync-02）。
        if (in.githubSyncRepoPublic) {
            infoRow(".publicwarn",
                    "仓库为公开（public）：笔记、整库快照与其中的密钥将公开可见",
                    t.colors.warning, true);
        }
        // 本地 git 不可用（灰字，不影响同步）
        if (in.githubSyncLocalGitUnavailable) {
            infoRow(".localgit", "本地版本历史：不可用", t.colors.textTertiary);
        }
        // 退出推送未完成：上次退出时有未同步改动 → 黄字提示，同步成功即消失
        if (in.githubSyncUnsyncedNotice) {
            infoRow(".unsynced",
                    "有未同步改动（上次退出时未能推送，建议点「立即同步」）",
                    t.colors.warning, true);
        }
        // 删远端被安全栏跳过（本地仍在但读失败）→ 黄字提示
        if (!in.githubSyncDeleteSkipHint.empty()) {
            infoRow(".deleteskip", in.githubSyncDeleteSkipHint, t.colors.warning, true);
        }
        })
        .build();
}

} // namespace module::settings
