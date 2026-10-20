// src/module_settings/git_error_dialog.cpp — 错误详情弹窗实现（边界见 .h）。
#include "git_error_dialog.h"

#include "settings_ui_logic.h"  // fs / roundFs（字号缩放随全局档位）

#include <core/component_theme.h>

#include <components/button.h>
#include <components/scrollview.h>

#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>

#include <algorithm>
#include <string>

namespace module::settings {

namespace {

constexpr const char* kDialogId = "settings.github.err";
constexpr float kDialogH = 560.0f;
// 纵向布局（100% 档基准；行盒走 roundFs 派生）。内容底沿 = kDetailY + kDetailH
// = 484，按钮行 y = h - 60 = 500，互不重叠。
constexpr float kSummaryY = 58.0f;
constexpr float kSummaryH = 46.0f;
constexpr float kHintY = 110.0f;
constexpr float kLogsY = 132.0f;
constexpr float kLogsH = 248.0f;
constexpr float kLogRowH = 30.0f;
// 覆盖清单最多列出的行数：本地未推改动无上界（首次拉取可上万条），而 scrollView 的
// content 每帧重建 —— 无上限就是无界每帧成本。截断处另画一行「其余 N 个」（见渲染处）。
constexpr size_t kMaxOverwriteRows = 50;
constexpr float kDetailY = 408.0f;
constexpr float kDetailH = 76.0f;
constexpr float kBtnRowOffsetY = 60.0f;  // 按钮行底距弹窗底（与 dialog_form 同口径）
constexpr float kBtnH = 40.0f;
constexpr float kBtnGap = 12.0f;

// 选中条目详情（越界 = 空串）
std::string selectedDetail(const std::vector<platform::github::ApiLogEntry>& logs,
                           int index) {
    if (index < 0 || index >= static_cast<int>(logs.size())) return {};
    return formatGitLogDetail(logs[static_cast<size_t>(index)]);
}

} // namespace

std::string formatGitLogRowLine(const platform::github::ApiLogEntry& e) {
    // `方法 URL 状态码 耗时`；无状态码（网络错误）= 明确写「网络错误」而不是 0。
    const std::string status =
        e.statusCode > 0 ? std::to_string(e.statusCode) : std::string("网络错误");
    return e.method + " " + e.urlMasked + " " + status + " " +
           std::to_string(e.durationMs) + "ms";
}

std::string formatGitLogDetail(const platform::github::ApiLogEntry& e) {
    if (!e.bodyTail.empty()) return e.bodyTail;
    if (!e.errorMessage.empty()) return e.errorMessage;
    return {};
}

GitErrorDialogVariant variantForSyncFailure(
    const std::vector<platform::github::ApiLogEntry>& logs, bool conflict) {
    if (conflict) return GitErrorDialogVariant::Conflict409;
    for (const auto& e : logs) {
        if (e.statusCode == 401) return GitErrorDialogVariant::AuthFailed;
    }
    return GitErrorDialogVariant::Error;
}

std::string formatGitErrorDetail(
    const std::string& summary,
    const std::vector<platform::github::ApiLogEntry>& logs) {
    std::string out;
    out += "同步失败详情\n";
    out += "摘要：" + summary + "\n";
    out += "请求日志（" + std::to_string(logs.size()) + " 条）：\n";
    for (size_t i = 0; i < logs.size(); ++i) {
        out += "  [" + std::to_string(i) + "] " + formatGitLogRowLine(logs[i]) + "\n";
        const std::string detail = formatGitLogDetail(logs[i]);
        if (!detail.empty()) {
            out += "      " + detail + "\n";
        }
    }
    return out;
}

void drawGitErrorDialog(eui::Ui& ui, const eui::Screen& screen,
                        const core::theme::Tokens& t, GitErrorDialogState& st,
                        const GitErrorDialogIn& in,
                        const GitErrorDialogActions& a) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::ExtraWide);
    const float bodyW = w - ui_kit::kDialogInsetX * 2.0f;

    // 每帧取一次日志快照：弹窗不缓存，日志源即唯一真源。
    const std::vector<platform::github::ApiLogEntry> logs =
        in.logs ? in.logs() : std::vector<platform::github::ApiLogEntry>{};

    ui_kit::DialogFrame frame;
    frame.id = kDialogId;
    frame.width = ui_kit::DialogWidth::ExtraWide;
    frame.height = kDialogH;
    frame.open = st.open;
    // 状态归模块所有（st 是模块成员引用）→ 可安全按地址捕获；弹窗外点关闭也走这条。
    frame.onOpenChange = [sp = &st](bool v) { sp->open = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        const bool conflict =
            st.variant == GitErrorDialogVariant::Conflict409;
        const bool authFailed =
            st.variant == GitErrorDialogVariant::AuthFailed;
        ui_kit::drawDialogTitle(
            ui, std::string(kDialogId) + ".title", w,
            conflict ? "同步冲突（409）"
                     : (authFailed ? "认证失败（Token 无效或已过期）"
                                   : "同步失败详情"),
            t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);

        ui_kit::drawDialogText(
            ui, std::string(kDialogId) + ".summary", w, in.summary,
            t.sizes.fontSizeSmall, t.colors.error,
            {.y = kSummaryY, .height = kSummaryH, .lineHeight = roundFs(20.0f),
             .wrap = true});

        // 409 二次确认武装后，此行渲染覆盖警告（同一行位，不撑高弹窗）。
        const bool armed = conflict && st.conflictConfirmArmed;
        ui_kit::drawDialogText(
            ui, std::string(kDialogId) + ".hint", w,
            armed ? "再次点击将用远端覆盖本地数据库与笔记（本地库已自动备份）"
                  : "请求日志（点击查看详情）",
            fs(11.0f), armed ? t.colors.warning : t.colors.textTertiary,
            {.y = kHintY, .height = roundFs(16.0f), .lineHeight = roundFs(16.0f)});

        components::scrollView(ui, std::string(kDialogId) + ".logs")
            .position(ui_kit::kDialogInsetX, kLogsY)
            .size(bodyW, kLogsH)
            .gap(0.0f)
            .step(roundFs(60.0f))
            .theme(themeTokens)
            .offset(st.logScrollOffset)
            .onChange([f = a.logScrolled](float v) {
                if (f) f(v);
            })
            .content([&](eui::Ui& inner, float contentWidth, float) {
                const float rowH = roundFs(kLogRowH);
                // 409 且清单非空 → 列表头渲染「将被覆盖的本地未推文件」，
                // 用户二次确认前看得见具体会丢什么（清单随日志区一起滚动）。
                if (conflict && !in.overwriteFiles.empty()) {
                    inner.text(std::string(kDialogId) + ".logs.overwrite.header")
                        .size(contentWidth, rowH)
                        .text("拉取将覆盖以下本地未推改动：")
                        .fontSize(fs(12.0f))
                        .lineHeight(rowH)
                        .fontWeight(600)
                        .color(t.colors.warning)
                        .build();
                    const size_t shown =
                        std::min(in.overwriteFiles.size(), kMaxOverwriteRows);
                    for (size_t i = 0; i < shown; ++i) {
                        inner.text(std::string(kDialogId) + ".logs.overwrite." +
                                   std::to_string(i) + ".text")
                            .size(contentWidth, rowH)
                            .text("  " + in.overwriteFiles[i])
                            .fontSize(fs(12.0f))
                            .lineHeight(rowH)
                            .color(t.colors.textSecondary)
                            .build();
                    }
                    if (in.overwriteFiles.size() > shown) {
                        const size_t rest = in.overwriteFiles.size() - shown;
                        inner.text(std::string(kDialogId) + ".logs.overwrite.more")
                            .size(contentWidth, rowH)
                            .text("  …以及其余 " + std::to_string(rest) +
                                  " 个文件（上面只列前 " + std::to_string(shown) +
                                  " 个）")
                            .fontSize(fs(12.0f))
                            .lineHeight(rowH)
                            .color(t.colors.textTertiary)
                            .build();
                    }
                }
                if (logs.empty()) {
                    inner.text(std::string(kDialogId) + ".logs.empty")
                        .size(contentWidth, rowH)
                        .text("（无请求日志）")
                        .fontSize(fs(12.0f))
                        .lineHeight(rowH)
                        .color(t.colors.textTertiary)
                        .build();
                    return;
                }
                for (size_t i = 0; i < logs.size(); ++i) {
                    const std::string rowId = std::string(kDialogId) +
                                              ".logs.row." + std::to_string(i);
                    const bool sel =
                        static_cast<int>(i) == st.selectedLogIndex;
                    // 行 = 背景 rect（命中区）+ 文本；同尺寸叠在 stack 里，
                    // 不拆成两个流式兄弟（那会让行高翻倍）。
                    inner.stack(rowId)
                        .size(contentWidth, rowH)
                        .content([&, i, sel, rowH, contentWidth] {
                            inner.rect(rowId + ".bg")
                                .size(contentWidth, rowH)
                                .color(t.colors.hover)
                                .opacity(sel ? 1.0f : 0.0f)
                                .radius(t.sizes.radiusSmall)
                                .onClick([f = a.selectLog, i] {
                                    if (f) f(static_cast<int>(i));
                                })
                                .build();
                            inner.text(rowId + ".text")
                                .position(8.0f, 0.0f)
                                .size(contentWidth - 16.0f, rowH)
                                .text(formatGitLogRowLine(logs[i]))
                                .fontSize(fs(12.0f))
                                .lineHeight(rowH)
                                .color(logs[i].networkError
                                           ? t.colors.error
                                           : t.colors.textSecondary)
                                .build();
                        })
                        .build();
                }
            })
            .build();

        ui_kit::drawDialogText(
            ui, std::string(kDialogId) + ".detail", w,
            selectedDetail(logs, st.selectedLogIndex), fs(11.0f),
            t.colors.textTertiary,
            {.y = kDetailY, .height = kDetailH, .lineHeight = roundFs(16.0f),
             .wrap = true});

        // 三按钮：主（重试 / 拉取远端后再推）/ 复制详情 / 关闭。
        // 节点 id 恒为 `.btn.retry`（409 变体下语义是「拉取后再推」，id 不随变体改，
        // 否则测试与调用方都要按变体分支找节点）。
        const float btnW = (bodyW - kBtnGap * 2.0f) / 3.0f;
        const float btnY = kDialogH - kBtnRowOffsetY;
        const float labelFont = t.sizes.fontSizeBody - 1.0f;
        const std::string primaryLabel =
            conflict
                ? (st.conflictConfirmArmed ? "确认：用远端覆盖并推送"
                                           : "拉取远端后再推")
                : (authFailed ? "去更新 Token" : "重试");

        components::button(ui, std::string(kDialogId) + ".btn.retry")
            .position(ui_kit::kDialogInsetX, btnY)
            .size(btnW, kBtnH)
            .text(primaryLabel)
            .fontSize(labelFont)
            .theme(themeTokens, /*primary=*/true)
            .radius(t.sizes.radius)
            // 409 = 二次确认：第一下只武装，第二下才真拉取覆盖并推送。
            .onClick([sp = &st, fRetry = a.retry, fPull = a.pullThenPush,
                      fUpdateToken = a.updateToken] {
                if (sp->variant == GitErrorDialogVariant::Conflict409) {
                    if (!sp->conflictConfirmArmed) {
                        sp->conflictConfirmArmed = true;
                        return;
                    }
                    sp->conflictConfirmArmed = false;
                    if (fPull) fPull();
                    return;
                }
                // 401 定向引导：不做通用重试（重试只会再吃一次 401），
                // 直接领用户去更新 Token。
                if (sp->variant == GitErrorDialogVariant::AuthFailed) {
                    if (fUpdateToken) fUpdateToken();
                    return;
                }
                if (fRetry) fRetry();
            })
            .build();
        components::button(ui, std::string(kDialogId) + ".btn.copy")
            .position(ui_kit::kDialogInsetX + btnW + kBtnGap, btnY)
            .size(btnW, kBtnH)
            .text("复制详情")
            .fontSize(labelFont)
            .theme(themeTokens, /*primary=*/false)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            // 复制详情：日志源与摘要按值捕获（元素树比本帧活得久，捕引用必悬垂）。
            .onClick([f = a.copyDetail, logsSrc = in.logs, summary = in.summary] {
                if (!f) return;
                const auto cur = logsSrc
                                     ? logsSrc()
                                     : std::vector<platform::github::ApiLogEntry>{};
                f(formatGitErrorDetail(summary, cur));
            })
            .build();
        components::button(ui, std::string(kDialogId) + ".btn.close")
            .position(ui_kit::kDialogInsetX + (btnW + kBtnGap) * 2.0f, btnY)
            .size(btnW, kBtnH)
            .text("关闭")
            .fontSize(labelFont)
            .theme(themeTokens, /*primary=*/false)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick(a.close)
            .build();
    });
}

} // namespace module::settings
