// 运行历史抽屉（波 S7b）：纯装配实现 + 渲染。
// 纯装配（summarizeDeliveries/assembleHistory/loadHistory）不依赖 EUI 运行时，
// 由 tests/test_modules/test_schedule_ui_history.cpp 无头直测；渲染只做展示。
#include "schedule_ui_history.h"

#include "core/component_theme.h"
#include "core/time.h"

#include <ui_kit/dialog_form.h>

#include <components/button.h>
#include <components/dialog.h>

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
    const float w = std::max(520.0f, screen.width * 0.46f);
    const float h = 560.0f;
    const float x0 = ui_kit::kDialogInsetX;
    const float innerW = w - ui_kit::kDialogBodyInsetX;

    components::dialog(ui, "schedule.history.drawer")
        .open(st.open)
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .zIndex(990)
        .onOpenChange([&st](bool v) { st.open = v; })
        .content([&] {
            ui_kit::drawDialogTitle(ui, "schedule.history.title", w, "运行历史", 20.0f,
                                    t.colors.textPrimary,
                                    {.y = 16.0f, .height = 28.0f, .lineHeight = 28.0f});

            float y = 56.0f;
            for (size_t i = 0; i < host.items.size() && y < h - 70.0f; ++i) {
                const auto& item = host.items[i];
                const auto& run = item.run;
                const bool selected = run.id == st.selectedRunId;
                // 行容器（选中 = 高亮底色）
                ui.rect("schedule.history.row." + std::to_string(i))
                    .position(x0, y)
                    .size(innerW, 96.0f)
                    .radius(8.0f)
                    .color(selected ? t.colors.primarySoftBg : t.colors.background)
                    .border(1.0f, selected ? t.colors.primary : t.colors.borderDefault)
                    .onClick([&st, id = run.id] { st.selectedRunId = id; })
                    .build();

                std::string line1 = "计划 " + formatLocal(run.plannedAt);
                if (run.startedAt.has_value()) line1 += "   实际 " + formatLocal(*run.startedAt);
                ui.text("schedule.history.row." + std::to_string(i) + ".planned")
                    .position(x0 + 12.0f, y + 8.0f)
                    .size(innerW - 24.0f, 18.0f)
                    .text(line1)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(t.colors.textPrimary)
                    .build();

                std::string line2 = "状态 " + run.status;
                if (run.durationMs.has_value()) line2 += "   耗时 " + std::to_string(*run.durationMs) + " ms";
                if (run.attempt > 0) line2 += "   尝试 " + std::to_string(run.attempt);
                ui.text("schedule.history.row." + std::to_string(i) + ".status")
                    .position(x0 + 12.0f, y + 30.0f)
                    .size(innerW - 24.0f, 18.0f)
                    .text(line2)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(run.status == "failed" ? t.colors.error
                                                  : t.colors.textSecondary)
                    .build();

                // 渠道投递与重试次数（run 成功 + 投递失败必须可分辨：状态行与渠道行分列）
                std::string line3 = "渠道 ";
                for (size_t c = 0; c < item.channelSummaries.size(); ++c) {
                    const auto& s = item.channelSummaries[c];
                    if (c) line3 += "  ";
                    line3 += "#" + std::to_string(s.channelId) + " " + s.lastStatus +
                             "(" + std::to_string(s.attempts) + " 次/重试 " +
                             std::to_string(s.retries) + ")";
                }
                if (item.channelSummaries.empty()) line3 += "无投递";
                ui.text("schedule.history.row." + std::to_string(i) + ".channels")
                    .position(x0 + 12.0f, y + 52.0f)
                    .size(innerW - 24.0f, 18.0f)
                    .text(line3)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                std::string line4 = run.error.empty() ? run.resultSummary : run.error;
                if (!line4.empty()) {
                    ui.text("schedule.history.row." + std::to_string(i) + ".detail")
                        .position(x0 + 12.0f, y + 72.0f)
                        .size(innerW - 24.0f, 18.0f)
                        .text(line4)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(18.0f)
                        .color(run.status == "failed" ? t.colors.errorText
                                                      : t.colors.textTertiary)
                        .build();
                }

                // 手动重试（仅失败/错过且宿主提供回调时显示）
                if (host.retryRun && (run.status == "failed" || run.status == "missed")) {
                    components::button(ui, "schedule.history.row." + std::to_string(i) + ".retry")
                        .position(x0 + innerW - 76.0f, y + 8.0f)
                        .size(60.0f, 24.0f)
                        .text("重试")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .radius(6.0f)
                        .border(1.0f, t.colors.borderDefault)
                        .onClick([&host, id = run.id] { host.retryRun(id); })
                        .build();
                }
                y += 104.0f;
            }

            components::button(ui, "schedule.history.close")
                .position(x0, h - 56.0f)
                .size(120.0f, 36.0f)
                .text("关闭")
                .radius(8.0f)
                .border(1.0f, t.colors.borderDefault)
                .onClick([&host] {
                    if (host.close) host.close();
                })
                .build();
        })
        .build();
}

}  // namespace module::schedule::ui
