// 运行历史抽屉（波 S7b）：纯装配实现 + 渲染。
// 纯装配（summarizeDeliveries/assembleHistory/loadHistory）不依赖 EUI 运行时，
// 由 tests/test_modules/test_schedule_ui_history.cpp 无头直测；渲染只做展示。
#include "schedule_ui_history.h"

#include "core/component_theme.h"
#include "core/time.h"

#include <ui_kit/card.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/empty_state.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <utility>

namespace module::schedule::ui {

namespace {

std::string formatLocal(int64_t unixSec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(unixSec), &local);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                  local.tm_min, local.tm_sec);
    return buf;
}

// ---- 抽屉视觉常量（2026-11-01 UI 重整）----
// 全站栅格：弹窗内容列 = 左右各 kDialogInsetX，列宽 = 弹窗宽 − kDialogBodyInsetX。
// 所有右对齐元素（× 关闭钮 / 行内重试钮 / 底部关闭钮）一律以 contentRight
// 为右基准，旧版三者各写各的 x（重试钮右边界距卡右 16px、文本距卡左 12px、
// 底部钮却左对齐）——「按钮没对齐」的根因。
constexpr float kDrawerMinW = 520.0f;
constexpr float kDrawerMaxW = 720.0f;
constexpr float kDrawerMinH = 440.0f;
constexpr float kDrawerMaxH = 680.0f;

// 卡片内边距 / 卡片间距
constexpr float kCardPad = 14.0f;
constexpr float kCardGap = 10.0f;
// 卡片内各行 y（相对卡片顶）
constexpr float kRowStatusY = 12.0f;
constexpr float kRowPlannedY = 40.0f;
constexpr float kRowActualY = 59.0f;
constexpr float kRowChipY = 80.0f;
constexpr float kRowDetailY = 104.0f;
// 卡片高度（两档：有/无详情行）
constexpr float kCardH = 112.0f;
constexpr float kCardHWithDetail = 132.0f;
// 状态圆点直径、行高
constexpr float kDotSize = 8.0f;
constexpr float kStatusRowH = 22.0f;
constexpr float kBodyRowH = 16.0f;
constexpr float kChipRowH = 20.0f;
// 底部按钮带：按钮高 40 + 底边距 16
constexpr float kFooterBtnH = 40.0f;
constexpr float kFooterBtnW = 96.0f;
constexpr float kFooterBottomPad = 16.0f;
// 顶部 × 图标钮
constexpr float kTitleCloseSize = 32.0f;
// 行内重试钮
constexpr float kRetryBtnW = 64.0f;
constexpr float kRetryBtnH = 26.0f;
// 渠道 chip 内边距
constexpr float kChipPadX = 8.0f;
// 渠道 chip 最多展示数（其余折成「+N」）
constexpr size_t kMaxChannelChips = 3;

// run 状态 → 中文文案 + 圆点色 + 文字色
struct RunStatusStyle {
    const char* label;
    eui::Color dot;
    bool errorText = false;
};

RunStatusStyle runStatusStyle(const std::string& status,
                               const core::theme::Tokens& t) {
    if (status == "success") {
        return {"成功", t.colors.success, false};
    }
    if (status == "failed") {
        return {"失败", t.colors.error, true};
    }
    if (status == "missed") {
        return {"已错过", t.colors.warning, false};
    }
    if (status == "running") {
        return {"运行中", t.colors.info, false};
    }
    if (status == "pending") {
        return {"待执行", t.colors.textTertiary, false};
    }
    if (status == "skipped") {
        return {"已跳过", t.colors.textTertiary, false};
    }
    if (status == "suppressed") {
        return {"已抑制", t.colors.textTertiary, false};
    }
    return {"未知", t.colors.textTertiary, false};
}

// 投递状态 → 中文文案
const char* deliveryStatusLabel(const std::string& status) {
    if (status == "success") return "成功";
    if (status == "failed") return "失败";
    if (status == "retry") return "重试中";
    if (status == "pending") return "待投递";
    if (status == "dropped") return "已丢弃";
    return "未知";
}

// chip 宽度估算：ASCII 按 0.52em、中文按 0.85em 计（视觉够用，不追求像素级排版引擎精度）
float estimateTextWidth(const std::string& text, float fontSize) {
    float width = 0.0f;
    for (char ch : text) {
        const auto u = static_cast<unsigned char>(ch);
        width += (u < 0x80) ? fontSize * 0.52f : fontSize * 0.85f;
    }
    return width;
}

// 渠道展示名：宿主注入可读名；未注入或查不到时回落「渠道 #id」
std::string channelName(const HistoryDrawerHost& host, int64_t channelId) {
    if (host.channelLabel) {
        const std::string label = host.channelLabel(channelId);
        if (!label.empty()) return label;
    }
    return "渠道 #" + std::to_string(channelId);
}

// 单条 run 卡片：状态点 + 状态文案 + 计划/实际 + 渠道 chip + 详情 + 重试钮
void drawRunCard(eui::Ui& ui, size_t index, const RunHistoryItem& item, float cardW,
                 float cardH, HistoryDrawerState& st, const HistoryDrawerHost& host,
                 const core::theme::Tokens& t,
                 const components::theme::ThemeColorTokens& themeTokens) {
    const auto& run = item.run;
    const std::string prefix = "schedule.history.row." + std::to_string(index);
    const auto style = runStatusStyle(run.status, t);
    const bool retryable =
        host.retryRun && (run.status == "failed" || run.status == "missed");

    // 卡片底板：描边/阴影全站统一；底色按主题分档——弹窗底是 surface（浅色=纯白），
    // 白玻璃卡压在白底上会「融」（描边 #E5E7EB 太淡），浅色取页面底淡绿白、
    // 深色取比弹窗底亮一档的 surfaceElevated。
    const eui::Color cardBg =
        t.isDark ? t.colors.surfaceElevated : t.colors.background;
    ui_kit::cardPanelColored(ui, prefix, 0.0f, 0.0f, cardW, cardH, t.sizes.radius, t,
                              cardBg,
                              [&st, id = run.id] { st.selectedRunId = id; });
    if (run.id == st.selectedRunId) {
        ui.rect(prefix + ".focus")
            .position(0.0f, 0.0f)
            .size(cardW, cardH)
            .color(eui::Color(0.0f, 0.0f, 0.0f, 0.0f))
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.primary)
            .build();
    }

    // 行 1：状态点 + 状态文案（重试钮右对齐内容列右基准）
    const float statusY = kRowStatusY;
    ui.rect(prefix + ".dot")
        .position(kCardPad, statusY + (kStatusRowH - kDotSize) * 0.5f)
        .size(kDotSize, kDotSize)
        .radius(kDotSize * 0.5f)
        .color(style.dot)
        .build();
    const float statusX = kCardPad + kDotSize + 8.0f;
    const float statusW = cardW - statusX - kCardPad -
                          (retryable ? kRetryBtnW + 10.0f : 0.0f);
    ui.text(prefix + ".status")
        .position(statusX, statusY)
        .size(std::max(36.0f, statusW), kStatusRowH)
        .text(style.label)
        .fontSize(t.sizes.fontSizeBody)
        .fontWeight(600)
        .lineHeight(kStatusRowH)
        .color(style.errorText ? t.colors.errorText : t.colors.textPrimary)
        .build();
    if (retryable) {
        components::button(ui, prefix + ".retry")
            .position(cardW - kCardPad - kRetryBtnW,
                      statusY + (kStatusRowH - kRetryBtnH) * 0.5f)
            .size(kRetryBtnW, kRetryBtnH)
            .text("重试")
            .fontSize(t.sizes.fontSizeSmall)
            .theme(themeTokens, false)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([&host, id = run.id] { host.retryRun(id); })
            .build();
    }

    // 行 2：计划时刻
    ui.text(prefix + ".planned")
        .position(kCardPad, kRowPlannedY)
        .size(cardW - 2.0f * kCardPad, kBodyRowH)
        .text("计划 " + formatLocal(run.plannedAt))
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(kBodyRowH)
        .color(t.colors.textSecondary)
        .build();

    // 行 3：实际时刻 / 耗时 / 尝试次数
    std::string actualLine;
    if (run.startedAt.has_value()) actualLine = "实际 " + formatLocal(*run.startedAt);
    if (run.durationMs.has_value()) {
        if (!actualLine.empty()) actualLine += " · ";
        actualLine += "耗时 " + std::to_string(*run.durationMs) + " ms";
    }
    if (run.attempt > 0) {
        if (!actualLine.empty()) actualLine += " · ";
        actualLine += "尝试 " + std::to_string(run.attempt);
    }
    if (actualLine.empty()) actualLine = "尚未开始";
    ui.text(prefix + ".actual")
        .position(kCardPad, kRowActualY)
        .size(cardW - 2.0f * kCardPad, kBodyRowH)
        .text(actualLine)
        .fontSize(t.sizes.fontSizeSmall)
        .lineHeight(kBodyRowH)
        .color(t.colors.textTertiary)
        .build();

    // 行 4：渠道投递 chip（药丸：淡绿底 + 描边，文字次要色）
    const float chipFont = t.sizes.fontSizeSmall - 1.0f;
    const float chipLimit = cardW - 2.0f * kCardPad;
    if (item.channelSummaries.empty()) {
        ui.text(prefix + ".channels")
            .position(kCardPad, kRowChipY)
            .size(cardW - 2.0f * kCardPad, kChipRowH)
            .text("无通知投递")
            .fontSize(chipFont)
            .lineHeight(kChipRowH)
            .color(t.colors.textTertiary)
            .build();
    } else {
        float chipX = kCardPad;
        size_t shown = 0;
        for (const auto& s : item.channelSummaries) {
            if (shown >= kMaxChannelChips) break;
            std::string label =
                channelName(host, s.channelId) + " " + deliveryStatusLabel(s.lastStatus);
            if (s.retries > 0) {
                label += " · 重试 " + std::to_string(s.retries);
            }
            const float chipW = estimateTextWidth(label, chipFont) + 2.0f * kChipPadX;
            if (chipX + chipW > kCardPad + chipLimit) break;
            const std::string chipId = prefix + ".chip." + std::to_string(shown);
            ui.rect(chipId)
                .position(chipX, kRowChipY)
                .size(chipW, kChipRowH)
                .radius(t.sizes.radiusPill)
                .color(eui::Color(0.0f, 0.0f, 0.0f, 0.0f))
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.text(chipId + ".text")
                .position(chipX, kRowChipY)
                .size(chipW, kChipRowH)
                .text(label)
                .fontSize(chipFont)
                .lineHeight(kChipRowH)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
            chipX += chipW + 6.0f;
            ++shown;
        }
        if (shown < item.channelSummaries.size()) {
            const std::string more = "+" + std::to_string(item.channelSummaries.size() - shown);
            const float moreW = estimateTextWidth(more, chipFont) + 2.0f * kChipPadX;
            if (chipX + moreW <= kCardPad + chipLimit) {
                const std::string moreId = prefix + ".chip.more";
                ui.rect(moreId)
                    .position(chipX, kRowChipY)
                    .size(moreW, kChipRowH)
                    .radius(t.sizes.radiusPill)
                    .color(eui::Color(0.0f, 0.0f, 0.0f, 0.0f))
                    .border(1.0f, t.colors.borderDefault)
                    .build();
                ui.text(moreId + ".text")
                    .position(chipX, kRowChipY)
                    .size(moreW, kChipRowH)
                    .text(more)
                    .fontSize(chipFont)
                    .lineHeight(kChipRowH)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            }
        }
    }

    // 行 5：错误 / 结果摘要（仅失败等有内容的行）
    const std::string detail = !run.error.empty() ? run.error : run.resultSummary;
    if (!detail.empty()) {
        ui.text(prefix + ".detail")
            .position(kCardPad, kRowDetailY)
            .size(cardW - 2.0f * kCardPad, kBodyRowH)
            .text(detail)
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(kBodyRowH)
            .color(run.status == "failed" ? t.colors.errorText : t.colors.textTertiary)
            .build();
    }
}

}  // namespace

std::vector<ChannelDeliverySummary> summarizeDeliveries(
    const std::vector<DeliveryRecord>& deliveries) {
    // run_id 归组顺序按首个出现次序；组内行已按 attempt 升序（store 契约）
    std::vector<ChannelDeliverySummary> out;
    std::map<int64_t, size_t> indexOf;
    for (const auto& d : deliveries) {
        auto it = indexOf.find(d.channelId);
        if (it == indexOf.end()) {
            ChannelDeliverySummary s;
            s.channelId = d.channelId;
            indexOf[d.channelId] = out.size();
            out.push_back(std::move(s));
            it = indexOf.find(d.channelId);
        }
        ChannelDeliverySummary& s = out[it->second];
        ++s.attempts;
        if (d.status == "success") ++s.successCount;
        s.lastStatus = d.status;
        s.nextRetryAt = d.nextRetryAt;
    }
    for (auto& s : out) {
        s.retries = s.attempts > 0 ? s.attempts - 1 : 0;
    }
    std::sort(out.begin(), out.end(),
              [](const ChannelDeliverySummary& a, const ChannelDeliverySummary& b) {
                  return a.channelId < b.channelId;
              });
    return out;
}

std::vector<RunHistoryItem> assembleHistory(const std::vector<RunRecord>& runs,
                                            const std::vector<DeliveryRecord>& deliveries,
                                            int limit) {
    if (limit <= 0) return {};
    std::map<int64_t, std::vector<DeliveryRecord>> byRun;
    for (const auto& d : deliveries) {
        byRun[d.runId].push_back(d);
    }
    std::vector<RunHistoryItem> items;
    items.reserve(runs.size());
    for (const auto& r : runs) {
        RunHistoryItem item;
        item.run = r;
        const auto it = byRun.find(r.id);
        if (it != byRun.end()) {
            item.deliveries = it->second;  // attempt 升序
            item.channelSummaries = summarizeDeliveries(item.deliveries);
        }
        items.push_back(std::move(item));
    }
    // planned_at 倒序稳定排序（同 planned_at 保持入序 = id 升序的 store 序）
    std::stable_sort(items.begin(), items.end(),
                     [](const RunHistoryItem& a, const RunHistoryItem& b) {
                         return a.run.plannedAt > b.run.plannedAt;
                     });
    if (static_cast<int>(items.size()) > limit) {
        items.resize(static_cast<size_t>(limit));
    }
    return items;
}

core::Result<std::vector<RunHistoryItem>> loadHistory(ScheduleStore& store, int64_t ruleId,
                                                      int limit) {
    const auto runs = store.listRunsByRule(ruleId, limit);
    if (runs.isFail()) {
        return core::Result<std::vector<RunHistoryItem>>::fail(runs.errorCode(),
                                                               runs.errorMessage());
    }
    std::vector<DeliveryRecord> deliveries;
    for (const auto& r : runs.value()) {
        const auto ds = store.listDeliveriesByRun(r.id);
        if (ds.isFail()) {
            return core::Result<std::vector<RunHistoryItem>>::fail(ds.errorCode(),
                                                                   ds.errorMessage());
        }
        deliveries.insert(deliveries.end(), ds.value().begin(), ds.value().end());
    }
    return core::Result<std::vector<RunHistoryItem>>::ok(
        assembleHistory(runs.value(), deliveries, limit));
}

// ---- 渲染 ----

void drawHistoryDrawer(eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
                       HistoryDrawerState& st, const HistoryDrawerHost& host) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);

    // 内容列：左起 x0 = kDialogInsetX，右基准 contentRight（所有右对齐元素共用）
    const float x0 = ui_kit::kDialogInsetX;

    // 头部：标题 28 + 副标题 18 + 分隔线 1
    const float titleY = 16.0f;
    const float titleH = 28.0f;
    const float subtitleY = titleY + titleH + 6.0f;
    const float subtitleH = 18.0f;
    const float dividerY = subtitleY + subtitleH + 10.0f;
    const float listTop = dividerY + 12.0f;

    // 底部结构（自上而下）：列表 → 12 缝 → 分隔线 1 → 16 缝 → 按钮带 40 → 底边距 16
    const float listBottomGap = 12.0f;
    const float dividerGap = 16.0f;
    const float footerBandH = dividerGap + 1.0f + dividerGap + kFooterBtnH + kFooterBottomPad;

    // 列表内容高（卡片两档高度 + 间距）→ 抽屉高度随内容自适应：
    // 固定高度时 3 条记录下方空一大片，看着像没做完。
    float listContentH = 0.0f;
    for (size_t i = 0; i < host.items.size(); ++i) {
        const auto& run = host.items[i].run;
        const bool hasDetail = !run.error.empty() || !run.resultSummary.empty();
        listContentH += hasDetail ? kCardHWithDetail : kCardH;
        if (i > 0) listContentH += kCardGap;
    }
    const float neededH = listTop + std::max(listContentH, 120.0f) + footerBandH;
    const float h = std::clamp(std::max(neededH, kDrawerMinH), kDrawerMinH, kDrawerMaxH);
    const float w = std::clamp(screen.width * 0.46f, kDrawerMinW, kDrawerMaxW);
    const float contentW = w - ui_kit::kDialogBodyInsetX;
    const float contentRight = x0 + contentW;
    const float footerBtnY = h - kFooterBottomPad - kFooterBtnH;
    const float footerDividerY = footerBtnY - dividerGap - 1.0f;
    const float listH = std::max(96.0f, footerDividerY - listBottomGap - listTop);

    // 统计条：总数 + 成功 / 失败 / 错过
    size_t okCount = 0;
    size_t failCount = 0;
    size_t missedCount = 0;
    for (const auto& item : host.items) {
        if (item.run.status == "success") {
            ++okCount;
        } else if (item.run.status == "failed") {
            ++failCount;
        } else if (item.run.status == "missed") {
            ++missedCount;
        }
    }
    std::string statText = "共 " + std::to_string(host.items.size()) + " 条";
    if (okCount > 0) statText += " · 成功 " + std::to_string(okCount);
    if (failCount > 0) statText += " · 失败 " + std::to_string(failCount);
    if (missedCount > 0) statText += " · 错过 " + std::to_string(missedCount);

    components::dialog(ui, "schedule.history.drawer")
        .open(st.open)
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .zIndex(990)
        .onOpenChange([&st](bool v) { st.open = v; })
        .content([&] {
            // 标题：宽度让出右上角 × 钮（避免文字压按钮）
            const float titleW = contentRight - kTitleCloseSize - 12.0f - x0;
            ui_kit::drawDialogTitle(ui, "schedule.history.title", w, "运行历史",
                                    ui_kit::kDialogTitleFontSize, t.colors.textPrimary,
                                    {.x = x0, .y = titleY, .inset = w - titleW,
                                     .height = titleH, .lineHeight = titleH});

            // 右上角关闭：图标钮，右边缘 = contentRight（与底部钮同一基准）
            components::button(ui, "schedule.history.titleClose")
                .position(contentRight - kTitleCloseSize,
                          titleY + (titleH - kTitleCloseSize) * 0.5f)
                .size(kTitleCloseSize, kTitleCloseSize)
                .text("")
                .icon(0xF00D)  // fa-xmark
                .iconSize(12.0f)
                .theme(themeTokens, false)
                .radius(t.sizes.radiusSmall)
                .onClick([&host] {
                    if (host.close) host.close();
                })
                .build();

            // 副标题：统计条
            ui_kit::drawDialogText(ui, "schedule.history.summary", w, statText,
                                    t.sizes.fontSizeSmall, t.colors.textSecondary,
                                    {.x = x0, .y = subtitleY, .inset = w - contentW,
                                     .height = subtitleH, .lineHeight = subtitleH});

            // 分隔线（头部 / 列表）
            ui.rect("schedule.history.divider")
                .position(x0, dividerY)
                .size(contentW, 1.0f)
                .color(t.colors.borderDefault)
                .build();

            // 列表区：空态 or 滚动列表
            if (host.items.empty()) {
                ui_kit::emptyState(
                    ui, "schedule.history.empty",
                    ui_kit::EmptyStateOptions{
                        .message = "暂无运行记录",
                        .guide = "规则触发后会在此处留下计划与实际执行记录",
                        .icon = 0xF1DA,  // fa-history
                    },
                    x0, listTop, contentW, listH, t);
            } else {
                components::scrollView(ui, "schedule.history.scroll")
                    .position(0.0f, listTop)
                    .size(w, listH)
                    .theme(themeTokens)
                    .scrollbarOverlay(true)
                    .gap(kCardGap)
                    .contentKey("sched.hist." + std::to_string(host.items.size()))
                    .content([&](eui::Ui& inner, float cw, float) {
                        const float cardW = std::max(0.0f, cw - ui_kit::kDialogBodyInsetX);
                        float cardY = 0.0f;
                        for (size_t i = 0; i < host.items.size(); ++i) {
                            const auto& item = host.items[i];
                            const bool hasDetail =
                                !item.run.error.empty() || !item.run.resultSummary.empty();
                            const float cardH = hasDetail ? kCardHWithDetail : kCardH;
                            inner.stack("schedule.history.row." + std::to_string(i))
                                .position(x0, cardY)
                                .size(cardW, cardH)
                                .content([&] {
                                    drawRunCard(inner, i, item, cardW, cardH, st, host, t,
                                                themeTokens);
                                })
                                .build();
                            cardY += cardH + kCardGap;
                        }
                    })
                    .build();
            }

            // 底部：分隔线 + 关闭钮右对齐（与右上 × 钮同一右基准）
            ui.rect("schedule.history.footerDivider")
                .position(x0, footerDividerY)
                .size(contentW, 1.0f)
                .color(t.colors.borderDefault)
                .build();
            components::button(ui, "schedule.history.close")
                .position(contentRight - kFooterBtnW, footerBtnY)
                .size(kFooterBtnW, kFooterBtnH)
                .text("关闭")
                .theme(themeTokens, false)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([&host] {
                    if (host.close) host.close();
                })
                .build();
        })
        .build();
}

}  // namespace module::schedule::ui
