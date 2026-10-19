// 详情弹窗 UI 绘制——composeTaskDetailDialog 的完整实现。
#include "module_agent/task_draft_parse.h"
// 从 agent_module.cpp 迁出以降低 God-class 体积（原约 1289 行）。
// 行为零变化——仅搬迁，不改逻辑/文案/样式/间距/ui id。

#include "agent_module.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/segmented.h>
#include <components/switch.h>
#include <eui/dsl.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_agent/agent_time_text.h"
#include "module_agent/message_blocks.h"
#include "module_agent/trigger_config.h"
#include "module_agent/agent_ui_shared.h"

namespace module::agent {

namespace {

// 详情弹窗独有常量（停止 chip 几何 / 消息区高钳制——仅 composeTaskDetailDialog 消费）
constexpr float kDetailDialogH = 660.0f;
constexpr float kStopChipW = 176.0f;
constexpr float kStopChipH = 32.0f;
constexpr float kStopChipY = 20.0f;
constexpr float kMsgAreaMinH = 96.0f;
constexpr float kMsgAreaMaxH = 520.0f;

// 弹窗实际宽/高的单边复算——仅本 TU 使用（composeTaskDetailDialog）
float detailDialogActualExtent(float requested, float screenExtent,
                               float overlayInset) {
    return std::min(requested, std::max(0.0f, screenExtent - overlayInset));
}

} // namespace

void AgentModule::composeTaskDetailDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float overlayInset = themeTokens.metrics.spacing.overlay;
    const float w = detailDialogActualExtent(
        ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form), screen.width,
        overlayInset);
    const float h =
        detailDialogActualExtent(kDetailDialogH, screen.height, overlayInset);
    const float innerW = std::max(0.0f, w - ui_kit::kDialogBodyInsetX);
    const float x0 = ui_kit::kDialogInsetX;

    // 文本高度全部由缩放后字号派生（fontScale 只缩 fontSize，行高按比例重算）
    const float fsS = t.sizes.fontSizeSmall;
    const float fsB = t.sizes.fontSizeBody;
    const float lhS = static_cast<float>(std::lround(fsS * 1.4f));
    const float lhB = static_cast<float>(std::lround(fsB * 1.4f));

    // 响应式档位：宽 ≥680 且高 ≥520 分栏；否则单列 tab。矮宽屏（h<600）右栏整栏滚动
    const bool wide = w >= 680.0f && h >= 520.0f;

    // 区域划分（面板内坐标）
    const float summaryY = 60.0f;
    const float summaryH = wide
                               ? std::max(81.0f, 2.0f * 10.0f + lhS + lhB + lhS + 4.0f)
                               : (h < 600.0f ? 116.0f : 122.0f);
    const float configY = summaryY + summaryH + 8.0f;
    const float configH = std::max(40.0f, lhS + 20.0f);
    const float mainY = configY + configH + 12.0f;
    const float mainH = std::max(56.0f, h - mainY - 24.0f);
    const float leftW = static_cast<float>(
        std::clamp(std::lround(innerW * 0.40f), 240L, 282L));
    const float rightX = x0 + leftW + 20.0f;
    const float rightW = std::max(0.0f, innerW - leftW - 20.0f);
    const float navH = h < 600.0f ? std::max(32.0f, lhS + 8.0f)
                                  : std::max(36.0f, lhS + 12.0f);
    const float navY = configY + configH + 8.0f;
    const float contentY = navY + navH + 8.0f;
    const float contentH = std::max(56.0f, h - contentY - 24.0f);

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.detail";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = h;
    frame.open = detailTaskId_.has_value();
    frame.onOpenChange = [this](bool v) {
        if (!v) closeTaskDetail();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!detailTaskId_) return;
        ui_kit::drawDialogTitle(ui, "agent.dialog.detail.title", w, "任务详情",
                                t.sizes.fontSizeLarge + 2.0f, t.colors.textPrimary);
        ui.text("agent.dialog.detail.close")
            .position(w - 56.0f, 22.0f)
            .size(28.0f, 28.0f)
            .text("✕")
            .fontSize(14.0f)
            .lineHeight(28.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .onClick([this] { closeTaskDetail(); })
            .build();

        // 「停止」chip（C1）：只在有在跑 run 时渲染——无 ActiveRun 可置位的 run（含崩溃
        // 遗留的 DB running 行）没有可停止对象，渲染入口等于骗点击。点击后切「停止中」
        // 禁用（cancel 位幂等，禁用只为给点击一个反馈，防 A5 型「点了没反应反复点」）
        if (isTaskRunActive(*detailTaskId_)) {
            const bool stopping = isStopRequested(*detailTaskId_);
            ui.stack("agent.dialog.detail.stop.wrap")
                .position(w - 56.0f - kDialogGap - kStopChipW, kStopChipY)
                .size(kStopChipW, kStopChipH)
                .content([&] {
                    actionChip(ui, "agent.dialog.detail.stop",
                               stopping ? "停止中 · 当前轮结束后生效"
                                        : "停止 · 当前轮结束后生效",
                               kStopChipW, /*primary=*/false, /*enabled=*/!stopping,
                               themeTokens, [this, taskId = *detailTaskId_] {
                                   requestStopRun(taskId);
                               }, fsS);
                })
                .build();
        }

        const auto runsIt = taskRuns_.find(*detailTaskId_);
        const std::vector<AgentRunRow>* runs =
            runsIt != taskRuns_.end() ? &runsIt->second : nullptr;
        const bool hasRuns = runs != nullptr && !runs->empty();
        const AgentRunRow* latestRun = hasRuns ? &runs->front() : nullptr;

        const int64_t nowSec = core::time::nowUnix();
        // 「下一次」缓存：键判等早退，nextCronMatch 回扫不在逐帧路径（spec §8.5）
        if (detailTaskRow_) {
            updateNextRunCache(*detailTaskRow_, latestRun, nowSec);
        }

        // —— 摘要三卡（常驻；真实数据，spec §8.2/§8.5）——
        // 汇总查询失败必须显形（静默失败面）：last/stats 值转「读取失败」error 色
        const bool summaryFailed = !runSummariesError_.empty();
        // 最近一次：主值 = 状态 · 耗时（运行中 = now-startedAt；无 ended = 状态裸值）
        std::string lastValue = summaryFailed ? "读取失败" : "从未执行";
        std::string lastSub;
        eui::Color lastValueColor = summaryFailed ? t.colors.error : t.colors.textPrimary;
        if (!summaryFailed && latestRun != nullptr) {
            if (latestRun->status == "running") {
                int64_t secs = nowSec - latestRun->startedAt;
                if (secs < 0) secs = 0;
                lastValue = "运行中 · " + formatRunDurationText(secs);
                lastValueColor = t.colors.info;
            } else if (latestRun->endedAt) {
                int64_t secs = *latestRun->endedAt - latestRun->startedAt;
                if (secs < 0) secs = 0;   // 时钟回拨防御
                lastValue = std::string(runStatusText(latestRun->status)) + " · " +
                            formatRunDurationText(secs);
                lastValueColor = statusDotColor(latestRun->status, t);
            } else {
                lastValue = runStatusText(latestRun->status);
                lastValueColor = statusDotColor(latestRun->status, t);
            }
            lastSub = formatLocalShortDateTime(latestRun->startedAt) + " · " +
                      relativeTimeText(latestRun->startedAt, nowSec);
        }
        // 近 7 天：主值 = 成功 N · 失败 M；副值 = 其中受限 K · P%（分母 = 成功+失败）
        std::string statsValue = summaryFailed ? "读取失败" : "近 7 天暂无执行";
        std::string statsSub;
        const auto sumIt = runSummaries_.find(*detailTaskId_);
        if (!summaryFailed && sumIt != runSummaries_.end()) {
            const AgentTaskRunSummary& s = sumIt->second;
            statsValue = "成功 " + std::to_string(s.okRuns7d) + " · 失败 " +
                         std::to_string(s.failedRuns7d);
            const int64_t denom = s.okRuns7d + s.failedRuns7d;
            const int64_t pct = denom > 0 ? (s.okRuns7d * 100) / denom : 0;
            statsSub = "其中受限 " + std::to_string(s.limitedRuns7d) + " · " +
                       std::to_string(pct) + "%";
        }
        // 下一次：describeNextRun 的真话（手动/事件不造假时间，spec D10/A6）
        const NextRunInfo nextInfo =
            (detailTaskRow_ && nextRunText_) ? *nextRunText_ : NextRunInfo{};
        auto toneColor = [&t](TextTone tone, eui::Color normal) -> eui::Color {
            switch (tone) {
                case TextTone::Muted: return t.colors.textTertiary;
                case TextTone::Warning: return t.colors.warning;
                case TextTone::Error: return t.colors.error;
                case TextTone::Normal: break;
            }
            return normal;
        };
        constexpr float kCardPad = 10.0f;
        // 副值行按卡高自适应：装得下（宽屏 81 ≥ pad*2+lhS+lhB+lhS）才画，窄屏两行卡不画
        auto hasSubRow = [&](float ch) {
            return ch >= 2.0f * kCardPad + lhS + lhB + lhS;
        };
        auto summaryCard = [&](const std::string& id, float cx, float cy, float cw,
                               float ch, const char* label,
                               const std::string& value, eui::Color valueColor,
                               const std::string& sub, eui::Color subColor) {
            ui.stack(id)
                .position(cx, cy)
                .size(cw, ch)
                .content([&] {
                    ui.rect(id + ".bg")
                        .size(cw, ch)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .build();
                    ui.text(id + ".label")
                        .position(kCardPad, kCardPad)
                        .size(std::max(0.0f, cw - 2.0f * kCardPad), lhS)
                        .text(label)
                        .fontSize(fsS)
                        .lineHeight(lhS)
                        .color(t.colors.textTertiary)
                        .build();
                    ui.text(id + ".value")
                        .position(kCardPad, kCardPad + lhS)
                        .size(std::max(0.0f, cw - 2.0f * kCardPad), lhB)
                        .text(value)
                        .fontSize(fsB)
                        .lineHeight(lhB)
                        .wrap(false)
                        .maxWidth(std::max(0.0f, cw - 2.0f * kCardPad))
                        .color(valueColor)
                        .build();
                    // 副值只在卡高装得下时画：窄屏两行卡高装不下第三行（卡高与 §6 的
                    // 122/116 耦合）
                    if (hasSubRow(ch)) {
                        ui.text(id + ".sub")
                            .position(kCardPad, kCardPad + lhS + lhB)
                            .size(std::max(0.0f, cw - 2.0f * kCardPad), lhS)
                            .text(sub)
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .wrap(false)
                            .maxWidth(std::max(0.0f, cw - 2.0f * kCardPad))
                            .color(subColor)
                            .build();
                    }
                })
                .build();
        };
        constexpr float kCardGap = 8.0f;
        if (wide) {
            const float cardW = std::max(0.0f, (innerW - 2.0f * kCardGap) / 3.0f);
            summaryCard("agent.dialog.detail.summary.last", x0, summaryY, cardW,
                        summaryH, "最近一次", lastValue, lastValueColor, lastSub,
                        t.colors.textTertiary);
            summaryCard("agent.dialog.detail.summary.next", x0 + cardW + kCardGap,
                        summaryY, cardW, summaryH, "下一次", nextInfo.value,
                        toneColor(nextInfo.tone, t.colors.textPrimary),
                        nextInfo.detail,
                        toneColor(nextInfo.tone, t.colors.textTertiary));
            summaryCard("agent.dialog.detail.summary.stats",
                        x0 + 2.0f * (cardW + kCardGap), summaryY, cardW, summaryH,
                        "近 7 天", statsValue,
                        summaryFailed ? t.colors.error : t.colors.textPrimary,
                        statsSub, t.colors.textTertiary);
        } else {
            const float rowH = (summaryH - kCardGap) / 2.0f;
            const float halfW = std::max(0.0f, (innerW - kCardGap) / 2.0f);
            summaryCard("agent.dialog.detail.summary.last", x0, summaryY, halfW,
                        rowH, "最近一次", lastValue, lastValueColor, lastSub,
                        t.colors.textTertiary);
            summaryCard("agent.dialog.detail.summary.next", x0 + halfW + kCardGap,
                        summaryY, halfW, rowH, "下一次", nextInfo.value,
                        toneColor(nextInfo.tone, t.colors.textPrimary),
                        nextInfo.detail,
                        toneColor(nextInfo.tone, t.colors.textTertiary));
            summaryCard("agent.dialog.detail.summary.stats", x0,
                        summaryY + rowH + kCardGap, innerW, rowH, "近 7 天",
                        statsValue,
                        summaryFailed ? t.colors.error : t.colors.textPrimary,
                        statsSub, t.colors.textTertiary);
        }

        // —— 配置头（常驻）+ 覆盖式浮层（spec §6.4/§7.2，P3 落地）——
        // hint 是折叠态摘要：触发 · cron（闹钟任务为闹钟时间）· 模型 · 迭代；
        // 目标/限次/破坏性/异常/执行记忆全在浮层字段（点开看全量，信息不丢）
        std::string configHint;
        if (detailTaskRow_) {
            const AgentTaskRow& task = *detailTaskRow_;
            configHint = "触发：" + triggerLabelOf(task.triggerType);
            if (task.triggerType == "schedule") {
                const std::string cron = cronTextOf(task);
                configHint += " · cron：" + (cron.empty() ? "（未设置）" : cron);
            } else if (task.triggerType == "once") {
                // 闹钟时间 = once 型的「定时」等价物，留在折叠摘要
                const std::string onceText = onceTextOf(task);
                configHint += " · 闹钟时间：" +
                              (onceText.empty() ? "（未设置）" : onceText);
            }
            configHint += " · 模型：" + task.modelSlot;
            configHint += " · 迭代：" + std::to_string(task.maxIterations);
        } else {
            configHint = "任务信息读取失败";
        }
        constexpr float kConfigTitleW = 64.0f;   // 「任务配置」4 CJK @15
        constexpr float kConfigToggleW = 40.0f;  // 「展开/收起」2 CJK @13 + 余量
        ui.stack("agent.dialog.detail.config.header")
            .position(x0, configY)
            .size(innerW, configH)
            .content([&] {
                ui.rect("agent.dialog.detail.config.header.bg")
                    .size(innerW, configH)
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .build();
                ui.text("agent.dialog.detail.config.header.title")
                    .position(kCardPad, (configH - lhB) * 0.5f)
                    .size(kConfigTitleW, lhB)
                    .text("任务配置")
                    .fontSize(fsB)
                    .lineHeight(lhB)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("agent.dialog.detail.config.header.hint")
                    .position(kCardPad + kConfigTitleW + kDialogGap,
                              (configH - lhS) * 0.5f)
                    .size(std::max(0.0f, innerW - kCardPad - kConfigTitleW -
                                        kConfigToggleW - 2.0f * kDialogGap),
                          lhS)
                    .text(configHint)
                    .fontSize(fsS)
                    .lineHeight(lhS)
                    .wrap(false)
                    .maxWidth(std::max(
                        0.0f, innerW - kCardPad - kConfigTitleW -
                                  kConfigToggleW - 2.0f * kDialogGap))
                    .color(t.colors.textSecondary)
                    .build();
                ui.text("agent.dialog.detail.config.header.toggle")
                    .position(innerW - kCardPad - kConfigToggleW,
                              (configH - lhS) * 0.5f)
                    .size(kConfigToggleW, lhS)
                    .text(configOpen_ ? "收起" : "展开")
                    .fontSize(fsS)
                    .lineHeight(lhS)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.primary)
                    .build();
                // 整行命中面最后画（盖在文本上收点击）；P1 只切状态位，浮层 P3 渲染
                ui.rect("agent.dialog.detail.config.header.hit")
                    .size(innerW, configH)
                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([this] { configOpen_ = !configOpen_; })
                    .build();
            })
            .build();

        // —— 时间线（宽屏左栏 / 窄屏 tab 0）：按天分组 + 行容器化（spec §7.2）——
        // 行 = <i> 容器（stack），子节点 .bg（命中面+选中底）/ .dot / .time / .status /
        // .duration；失败行内 .err（48 字节摘要）[.err.hint 仅 LLM 配置类]。
        // onClick 落 .bg——文件头「chip 手搓两层」契约同款
        auto composeRunlist = [&](float rx, float ry, float rw, float rh) {
            const float titleH = std::max(22.0f, lhB);
            // 行高 fs13 派生（§9.3 的 52/68/84 是 1.0 档实测值；err/hint 各 +16）
            const float rowMainH = std::max(52.0f, lhS + 34.0f);
            const float rowErrH = std::max(16.0f, lhS - 2.0f);
            constexpr float kRowInnerX = 12.0f;
            constexpr float kDotSize = 8.0f;
            const float timeX = kRowInnerX + kDotSize + 8.0f;
            constexpr float kTimeW = 100.0f;   // MM-DD HH:MM:SS @13 ≈ 96
            const float statusX = timeX + kTimeW + 4.0f;
            constexpr float kStatusW = 48.0f;  // 3 CJK @13 = 39
            const float durationX = statusX + kStatusW + 4.0f;
            const bool capped =
                hasRuns && runs->size() >= static_cast<size_t>(kDetailRunListLimit);
            ui.stack("agent.dialog.detail.runlist")
                .position(rx, ry)
                .size(rw, rh)
                .content([&] {
                    ui.text("agent.dialog.detail.runlist.title")
                        .position(0.0f, 0.0f)
                        .size(std::max(0.0f, rw - 90.0f), titleH)
                        .text("执行记录")
                        .fontSize(fsB)
                        .lineHeight(lhB)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    ui.text("agent.dialog.detail.runlist.count")
                        .position(0.0f, 0.0f)
                        .size(rw, titleH)
                        .text(hasRuns ? "最近 " + std::to_string(runs->size()) +
                                            " 次"
                                      : std::string())
                        .fontSize(fsS)
                        .lineHeight(lhB)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .color(t.colors.textTertiary)
                        .build();
                    if (!hasRuns) {
                        ui.text("agent.dialog.detail.runlist.empty")
                            .position(0.0f, titleH + 8.0f)
                            .size(rw, std::max(lhS, rh - titleH - 8.0f))
                            .text("从未执行")
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    const float noteH = capped ? lhS + 4.0f : 0.0f;
                    components::scrollView(ui,
                                           "agent.dialog.detail.runlist.scroll")
                        .position(0.0f, titleH + 8.0f)
                        .size(rw, std::max(0.0f, rh - titleH - 8.0f - noteH))
                        .gap(4.0f)
                        .step(18.0f)
                        .theme(themeTokens)
                        // key 含 size/选中/最新状态（spec §8.6：运行中耗时文本变化
                        // 不改行高，不需每帧换 key）
                        .contentKey("agent.dialog.detail.runlist.scroll.v" +
                                    std::to_string(msgScrollVersion_) + ".n" +
                                    std::to_string(runs->size()) + ".s" +
                                    std::to_string(
                                        selectedRunId_.value_or(0)) +
                                    ".w" +
                                    std::to_string(static_cast<int>(rw)))
                        .content([&](eui::Ui& inner, float cw, float) {
                            std::string lastDayLabel;
                            int dayIdx = -1;
                            for (size_t i = 0; i < runs->size(); ++i) {
                                const AgentRunRow& run = (*runs)[i];
                                // 日期分组头：今天 / 昨天 / YYYY-MM-DD（跨组插一行）
                                const std::string dayText =
                                    localDayLabel(run.startedAt, nowSec);
                                if (dayText != lastDayLabel) {
                                    lastDayLabel = dayText;
                                    ++dayIdx;
                                    inner.text("agent.dialog.detail.runlist.day." +
                                               std::to_string(dayIdx))
                                        .size(cw, lhS)
                                        .text(dayText)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .fontWeight(600)
                                        .color(t.colors.textTertiary)
                                        .build();
                                }
                                const std::string runRowId =
                                    "agent.dialog.detail.runlist." +
                                    std::to_string(i);
                                const bool highlighted =
                                    selectedRunId_ && *selectedRunId_ == run.id;
                                const bool hasErr =
                                    run.error && !run.error->empty();
                                const bool hasHint = hasErr &&
                                                     needsClaudePageHint(*run.error);
                                const float rowH =
                                    rowMainH +
                                    (hasErr ? rowErrH : 0.0f) +
                                    (hasHint ? rowErrH : 0.0f);
                                const std::string statusText =
                                    runStatusText(run.status);
                                // 耗时：结束 = ended-started（负差钳 0）；运行中 =
                                // now-started（spec §10）
                                std::string durationText = "—";
                                if (run.status == "running") {
                                    int64_t secs = nowSec - run.startedAt;
                                    if (secs < 0) secs = 0;
                                    durationText = formatRunDurationText(secs);
                                } else if (run.endedAt) {
                                    int64_t secs = *run.endedAt - run.startedAt;
                                    if (secs < 0) secs = 0;
                                    durationText = formatRunDurationText(secs);
                                }
                                inner.stack(runRowId)
                                    .size(cw, rowH)
                                    .content([&] {
                                        // .bg = 命中面 + 选中底（未选中透明，hover 微亮）
                                        inner.rect(runRowId + ".bg")
                                            .size(cw, rowH)
                                            .radius(t.sizes.radiusSmall)
                                            .states(highlighted
                                                        ? t.colors.primarySoftBg
                                                        : eui::Color{0.0f, 0.0f,
                                                                     0.0f, 0.0f},
                                                    t.colors.hover,
                                                    t.colors.hover)
                                            .onClick([this, runId = run.id] {
                                                selectedRunId_ = runId;
                                                // seq 只在 run 内唯一：切 run 清
                                                // 消息展开态并收回消息区（§6.2）；
                                                // 窄屏点行自动切「运行详情」tab、
                                                // 收起配置头
                                                expandedMsgSeqs_.clear();
                                                autoExpandedSeq_.reset();
                                                messagesOpen_ = false;
                                                detailTab_ = 1;
                                                configOpen_ = false;
                                                ++msgScrollVersion_;
                                                tasksDirty_ = true;
                                            })
                                            .build();
                                        if (highlighted) {
                                            // 选中行左缘 2px primary（spec §9.1）
                                            inner.rect(runRowId + ".edge")
                                                .size(2.0f, rowH)
                                                .color(t.colors.primary)
                                                .build();
                                        }
                                        inner.rect(runRowId + ".dot")
                                            .position(kRowInnerX,
                                                      (rowMainH - kDotSize) * 0.5f)
                                            .size(kDotSize, kDotSize)
                                            .radius(t.sizes.radiusPill)
                                            .color(statusDotColor(run.status, t))
                                            .build();
                                        inner.text(runRowId + ".time")
                                            .position(timeX,
                                                      (rowMainH - lhS) * 0.5f)
                                            .size(kTimeW, lhS)
                                            .text(formatLocalShortDateTime(
                                                run.startedAt))
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(false)
                                            .maxWidth(kTimeW)
                                            .color(t.colors.textSecondary)
                                            .build();
                                        inner.text(runRowId + ".status")
                                            .position(statusX,
                                                      (rowMainH - lhS) * 0.5f)
                                            .size(kStatusW, lhS)
                                            .text(statusText)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(false)
                                            .maxWidth(kStatusW)
                                            .color(statusDotColor(run.status, t))
                                            .build();
                                        inner.text(runRowId + ".duration")
                                            .position(durationX,
                                                      (rowMainH - lhS) * 0.5f)
                                            .size(std::max(
                                                0.0f, cw - durationX - kRowInnerX),
                                                  lhS)
                                            .text(durationText)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(false)
                                            .maxWidth(std::max(
                                                0.0f, cw - durationX - kRowInnerX))
                                            .color(t.colors.textTertiary)
                                            .build();
                                        if (hasErr) {
                                            inner.text(runRowId + ".err")
                                                .position(timeX, rowMainH)
                                                .size(std::max(
                                                          0.0f,
                                                          cw - timeX -
                                                              kRowInnerX),
                                                      rowErrH)
                                                .text("错误：" +
                                                      utf8Truncate(*run.error, 48))
                                                .fontSize(fsS)
                                                .lineHeight(rowErrH)
                                                .wrap(false)
                                                .maxWidth(std::max(
                                                    0.0f, cw - timeX - kRowInnerX))
                                                .color(t.colors.error)
                                                .build();
                                            // 「去 Claude 页」引导：仅「LLM 配置不可
                                            // 用」类错误渲染（needsClaudePageHint
                                            // 前缀钉死）
                                            if (hasHint) {
                                                inner.text(runRowId + ".err.hint")
                                                    .position(timeX,
                                                              rowMainH + rowErrH)
                                                    .size(std::max(
                                                              0.0f,
                                                              cw - timeX -
                                                                  kRowInnerX),
                                                          rowErrH)
                                                    .text("去 Claude 页 →")
                                                    .fontSize(fsS)
                                                    .lineHeight(rowErrH)
                                                    .wrap(false)
                                                    .color(t.colors.primary)
                                                    .onClick(
                                                        [] {
                                                            navigateToClaudePage();
                                                        })
                                                    .build();
                                            }
                                        }
                                    })
                                    .build();
                            }
                        })
                        .build();
                    // 20 条上限提示（spec §7.2/§10：不提供分页）
                    if (capped) {
                        ui.text("agent.dialog.detail.runlist.note")
                            .position(0.0f, rh - lhS)
                            .size(rw, lhS)
                            .text("仅显示最近 " +
                                  std::to_string(kDetailRunListLimit) + " 次")
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .color(t.colors.textTertiary)
                            .build();
                    }
                })
                .build();
        };

        // —— 右栏 / 窄屏 tab 1：run 详情块（状态/元信息/计划对照/错误全文）+ 消息区 ——
        // 宽屏正常高：详情块固定 + 消息标题固定 + .msgscroll 独立滚；矮宽屏 / 窄屏 /
        // 余高不足 96：.detail.scroll 整栏滚（不建 .msgscroll，避免嵌套滚动）
        constexpr float kMsgsTitleH = 32.0f;
        constexpr float kCopyFinalChipW = 76.0f;
        // 选中 run（无 → .detail.empty；默认选中 = 最近一次，refreshData 已回落）
        const AgentRunRow* selRun = nullptr;
        if (hasRuns && selectedRunId_) {
            for (const auto& r : *runs) {
                if (r.id == *selectedRunId_) {
                    selRun = &r;
                    break;
                }
            }
        }
        auto composeDetailColumn = [&](float dx, float dy, float dw, float dh) {
            const std::string finalText = finalAssistantText(detailMessages_);
            const bool hasCopyFinal = !finalText.empty();
            // 详情块高度预算（spec §6.3 detailTopH）：状态行 + 元信息 6 行 + 计划对照
            // + [错误 label+滚动区]；错误块出现时预算变大，消息余高不足自动降级整栏滚
            const float statusH = std::max(24.0f, lhB + 2.0f);
            const float metaPitch = lhS + 4.0f;
            const float metaH = 6.0f * metaPitch - 4.0f;
            const float scheduleH = lhS;
            float errScrollH = 0.0f;
            float errBlockH = 0.0f;
            const bool hasErr = selRun != nullptr && selRun->error &&
                                !selRun->error->empty();
            if (hasErr) {
                errScrollH = std::clamp(
                    estimateTextBlockHeight(*selRun->error, dw - 16.0f, fsS, lhS),
                    lhS, 96.0f);
                errBlockH = lhS + 8.0f + errScrollH;   // label + 列 gap + scroll
            }
            // 列内容高：status + gap8 + meta + gap8 + schedule [+ gap8 + 错误块]
            //（与 .blocks 列的 gap 8 逐项对应，供 fixed 态 msgs head 定位与降级判定）
            const float detailTopH =
                selRun != nullptr
                    ? statusH + 8.0f + metaH + 8.0f + scheduleH +
                          (hasErr ? 8.0f + errBlockH : 0.0f)
                    : 0.0f;
            const bool fixedMsgArea =
                selRun != nullptr && wide && h >= 600.0f &&
                (dh - detailTopH - kMsgsTitleH - 12.0f) >= 96.0f;

            // 标题行 + 复制 chip + 区级开合 chip（flow 排）：固定态包在绝对定位
            // stack 里，滚动态直接进 .detail.scroll 内容列。copyfinal 与 toggle 都
            // 常驻标题行（折叠态 copyfinal 仍可见，spec §12.1）
            constexpr float kMsgsToggleW = 48.0f;
            auto msgsHeaderRow = [&](eui::Ui& inner, float cw) {
                const float chipsW =
                    (hasCopyFinal ? kCopyFinalChipW + 8.0f : 0.0f) +
                    kMsgsToggleW;
                inner.row("agent.dialog.detail.msgs.head")
                    .size(cw, kMsgsTitleH)
                    .content([&] {
                        inner.text("agent.dialog.detail.msgs.title")
                            .size(std::max(0.0f, cw - chipsW), kMsgsTitleH)
                            .text("消息（" +
                                  std::to_string(detailMessages_.size()) + "）")
                            .fontSize(fsB)
                            .lineHeight(lhB)
                            .fontWeight(600)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textPrimary)
                            .build();
                        // 「复制回复」：一键复制 run 最终回复全文（最后一条 assistant
                        // 的全部 text 块，见 finalAssistantText）。空串不渲染
                        if (hasCopyFinal) {
                            inner.stack(
                                "agent.dialog.detail.msgs.copyfinal.wrap")
                                .size(kCopyFinalChipW, kMsgsTitleH)
                                .content([&] {
                                    actionChip(
                                        inner,
                                        "agent.dialog.detail.msgs.copyfinal",
                                        "复制回复", kCopyFinalChipW,
                                        /*primary=*/true, /*enabled=*/true,
                                        themeTokens, [this, text = finalText] {
                                            // setClipboardText 返回 void 无法校验
                                            // → LOG 留痕；反馈走页级 toast（D13）
                                            core::window::setClipboardText(text);
                                            LOG_INFO("Agent: 已复制最终回复 " +
                                                     std::to_string(text.size()) +
                                                     " 字节到剪贴板");
                                            toast_.show("已复制",
                                                        "最终回复已复制到剪贴板");
                                        }, fsS);
                                })
                                .build();
                        }
                        // 区级开合（D6 默认折叠）：展开时「最终 assistant 默认展开」
                        // 规则生效（applyDefaultMessageExpansion 降级为展开态内规则）
                        inner.stack("agent.dialog.detail.msgs.toggle.wrap")
                            .size(kMsgsToggleW, kMsgsTitleH)
                            .content([&] {
                                actionChip(
                                    inner, "agent.dialog.detail.msgs.toggle",
                                    messagesOpen_ ? "收起" : "展开",
                                    kMsgsToggleW,
                                    /*primary=*/false, /*enabled=*/true,
                                    themeTokens,
                                    [this] {
                                        messagesOpen_ = !messagesOpen_;
                                        if (messagesOpen_) {
                                            applyDefaultMessageExpansion();
                                        }
                                        ++msgScrollVersion_;   // 内容高变了 → scrollView 重测
                                    }, fsS);
                            })
                            .build();
                    })
                    .build();
            };

            // run 详情块：宽屏固定区与整栏滚动内容共用一套绘制（返回消耗高度）。
            // 布局必须流式：scrollView 内容里 .position 会被流式布局接管（实测钉死），
            // 区块统一走 column/row 流式排——外层列 gap 8（区块间 §9.3），meta 行内 4。
            // ID 契约（spec §7.2）：.detail.status[.dot/.text] / .detail.id /
            // .detail.meta.<field>[.label/.value] / .detail.schedule[.plan/.actual/
            // .verdict] / .detail.error[.label/.scroll/.text]
            auto drawDetailBlocks = [&](eui::Ui& inner, float cw, float y0) {
                if (selRun == nullptr) return y0;
                const std::string stText = runStatusText(selRun->status);
                const eui::Color stColor = statusDotColor(selRun->status, t);
                // 元信息 6 行单列（spec D：保证完整时间戳不截断）
                std::string durV = "—";
                if (selRun->status == "running") {
                    int64_t secs = nowSec - selRun->startedAt;
                    if (secs < 0) secs = 0;
                    durV = "进行中 · " + formatRunDurationText(secs);
                } else if (selRun->endedAt) {
                    int64_t secs = *selRun->endedAt - selRun->startedAt;
                    if (secs < 0) secs = 0;
                    durV = formatRunDurationText(secs);
                }
                struct MetaRow {
                    const char* id;
                    const char* label;
                    std::string value;
                };
                const MetaRow metaRows[6] = {
                    {"started", "开始", formatLocalDateTime(selRun->startedAt)},
                    {"ended", "结束",
                     selRun->endedAt ? formatLocalDateTime(*selRun->endedAt)
                                     : std::string("—")},
                    {"duration", "耗时", durV},
                    {"iterations", "迭代", std::to_string(selRun->iterations)},
                    {"input", "输入",
                     selRun->inputTokens ? formatTokenCount(*selRun->inputTokens)
                                         : std::string("—")},
                    {"output", "输出",
                     selRun->outputTokens
                         ? formatTokenCount(*selRun->outputTokens)
                         : std::string("—")},
                };
                // 计划对照（describeRunPlan 缓存产物；无计划不造假）
                const RunPlanInfo plan =
                    detailPlan_.has_value() ? *detailPlan_ : RunPlanInfo{};
                inner.column("agent.dialog.detail.detail.blocks")
                    .size(cw, detailTopH)
                    .gap(8.0f)
                    .content([&] {
                        // 状态 pill + #id（plain stack 内绝对定位合法）
                        inner.stack("agent.dialog.detail.detail.status")
                            .size(cw, statusH)
                            .content([&] {
                                // pill 底/边 = 状态色 withAlpha 0.14/0.40（spec §9.1）
                                inner.rect(
                                          "agent.dialog.detail.detail.status.pill")
                                    .size(88.0f, statusH)
                                    .color(components::theme::withOpacity(
                                        stColor, 0.14f))
                                    .radius(t.sizes.radiusPill)
                                    .border(1.0f,
                                            components::theme::withOpacity(
                                                stColor, 0.40f))
                                    .build();
                                inner.rect("agent.dialog.detail.detail.status.dot")
                                    .position(8.0f, (statusH - 8.0f) * 0.5f)
                                    .size(8.0f, 8.0f)
                                    .radius(t.sizes.radiusPill)
                                    .color(stColor)
                                    .build();
                                inner.text(
                                          "agent.dialog.detail.detail.status.text")
                                    .position(24.0f, 0.0f)
                                    .size(60.0f, statusH)
                                    .text(stText)
                                    .fontSize(fsB)
                                    .lineHeight(statusH)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(stColor)
                                    .build();
                                inner.text("agent.dialog.detail.detail.id")
                                    .position(std::max(0.0f, cw - 72.0f), 0.0f)
                                    .size(72.0f, statusH)
                                    .text("#" + std::to_string(selRun->id))
                                    .fontSize(fsS)
                                    .lineHeight(statusH)
                                    .horizontalAlign(eui::HorizontalAlign::Right)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();
                            })
                            .build();
                        // 元信息 6 行（行盒 flow，行内 label 64 + value）
                        inner.column("agent.dialog.detail.detail.meta")
                            .size(cw, metaH)
                            .gap(4.0f)
                            .content([&] {
                                for (const auto& row : metaRows) {
                                    const std::string base =
                                        std::string(
                                            "agent.dialog.detail.detail.meta.") +
                                        row.id;
                                    inner.row(base)
                                        .size(cw, lhS)
                                        .content([&] {
                                            inner.text(base + ".label")
                                                .size(64.0f, lhS)
                                                .text(row.label)
                                                .fontSize(fsS)
                                                .lineHeight(lhS)
                                                .color(t.colors.textTertiary)
                                                .build();
                                            inner.text(base + ".value")
                                                .size(std::max(0.0f, cw - 72.0f),
                                                      lhS)
                                                .text(row.value)
                                                .fontSize(fsS)
                                                .lineHeight(lhS)
                                                .wrap(false)
                                                .maxWidth(std::max(
                                                    0.0f, cw - 72.0f))
                                                .color(t.colors.textSecondary)
                                                .build();
                                        })
                                        .build();
                                }
                            })
                            .build();
                        // 计划对照一行三列（flow row）
                        inner.row("agent.dialog.detail.detail.schedule")
                            .size(cw, lhS)
                            .content([&] {
                                inner.text(
                                          "agent.dialog.detail.detail.schedule.plan")
                                    .size(cw * 0.44f, lhS)
                                    .text(std::string("计划 ") +
                                          (plan.hasPlan ? plan.plan : "—"))
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .wrap(false)
                                    .color(t.colors.textSecondary)
                                    .build();
                                inner.text(
                                          "agent.dialog.detail.detail.schedule.actual")
                                    .size(cw * 0.34f, lhS)
                                    .text("实际 " + plan.actual)
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .wrap(false)
                                    .color(t.colors.textSecondary)
                                    .build();
                                inner.text(
                                          "agent.dialog.detail.detail.schedule.verdict")
                                    .size(std::max(0.0f, cw * 0.22f), lhS)
                                    .text(plan.verdict)
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .horizontalAlign(eui::HorizontalAlign::Right)
                                    // verdict 色：Normal=按时→success；其余各归 token
                                    .color(plan.tone == TextTone::Normal
                                               ? t.colors.success
                                           : plan.tone == TextTone::Warning
                                               ? t.colors.warning
                                           : plan.tone == TextTone::Error
                                               ? t.colors.error
                                               : t.colors.textTertiary)
                                    .build();
                            })
                            .build();
                        if (hasErr) {
                            inner.text("agent.dialog.detail.detail.error.label")
                                .size(cw, lhS)
                                .text("错误详情")
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .fontWeight(600)
                                .color(t.colors.error)
                                .build();
                            components::scrollView(
                                inner, "agent.dialog.detail.detail.error.scroll")
                                .size(cw, errScrollH)
                                .step(18.0f)
                                .theme(themeTokens)
                                .contentKey(
                                    "agent.dialog.detail.detail.error.scroll.v" +
                                    std::to_string(
                                        hasErr ? selRun->error->size() : 0) +
                                    ".w" + std::to_string(static_cast<int>(cw)))
                                .content([&](eui::Ui& errInner, float ew, float) {
                                    errInner.text(
                                              "agent.dialog.detail.detail.error.text")
                                        .size(ew,
                                              estimateTextBlockHeight(
                                                  *selRun->error, ew, fsS, lhS))
                                        .text(*selRun->error)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .wrap(true)
                                        .maxWidth(ew)
                                        .color(t.colors.error)
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
                return y0 + detailTopH;
            };

            ui.stack("agent.dialog.detail.detail")
                .position(dx, dy)
                .size(dw, dh)
                .content([&] {
                    if (selRun == nullptr) {
                        // 无 run（或选中失效）：右栏空态（spec §10）
                        ui.text("agent.dialog.detail.detail.empty")
                            .size(dw, dh)
                            .text("选择一次执行查看详情")
                            .fontSize(fsS)
                            .lineHeight(lhS)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    if (fixedMsgArea) {
                        const float blocksH = drawDetailBlocks(ui, dw, 0.0f);
                        ui.stack("agent.dialog.detail.msgs.head.wrap")
                            .position(0.0f, blocksH)
                            .size(dw, kMsgsTitleH)
                            .content([&] { msgsHeaderRow(ui, dw); })
                            .build();
                        const float yMsg = blocksH + kMsgsTitleH;
                        if (!detailMessagesError_.empty()) {
                            ui.text("agent.dialog.detail.msgs.error")
                                .position(0.0f, yMsg)
                                .size(dw, std::max(0.0f, dh - yMsg))
                                .text("消息读取失败：" + detailMessagesError_)
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .wrap(true)
                                .maxWidth(dw)
                                .color(t.colors.error)
                                .build();
                            return;
                        }
                        if (detailMessages_.empty()) {
                            ui.text("agent.dialog.detail.empty")
                                .position(0.0f, yMsg)
                                .size(dw, std::max(0.0f, dh - yMsg))
                                .text("（选中的 run 还没有消息）")
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                // 盒高 ≠ 行高，不给 verticalAlign 会顶在上缘
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                            return;
                        }
                        if (!messagesOpen_) {
                            // 折叠态（D6）：只给占位提示，不建 .msgscroll（spec
                            // §12.3 反向断言钉「默认无 msgscroll」）
                            ui.text("agent.dialog.detail.msgs.hint")
                                .position(0.0f, yMsg)
                                .size(dw, std::max(0.0f, dh - yMsg))
                                .text("展开查看完整链路")
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                            return;
                        }
                        components::scrollView(
                            ui, "agent.dialog.detail.msgscroll")
                            .position(0.0f, yMsg)
                            .size(dw, std::max(0.0f, dh - yMsg))
                            .gap(kMsgRowGap)
                            .step(18.0f)
                            .theme(themeTokens)
                            .contentKey("agent.dialog.detail.msgscroll.v" +
                                        std::to_string(msgScrollVersion_) +
                                        ".n" +
                                        std::to_string(
                                            detailMessages_.size()) +
                                        ".o" +
                                        std::to_string(messagesOpen_ ? 1 : 0) +
                                        ".x" +
                                        std::to_string(
                                            expandedMsgSeqs_.size()) +
                                        ".e" +
                                        std::to_string(
                                            detailMessagesError_.size()) +
                                        ".w" +
                                        std::to_string(static_cast<int>(dw)))
                            .content([&](eui::Ui& inner, float cw, float) {
                                for (size_t i = 0;
                                     i < detailMessages_.size(); ++i) {
                                    renderMessageRow(inner, cw,
                                                     "agent.dialog.detail",
                                                     i, detailMessages_[i],
                                                     selRun->startedAt, t,
                                                     themeTokens);
                                }
                            })
                            .build();
                        return;
                    }
                    components::scrollView(ui,
                                           "agent.dialog.detail.detail.scroll")
                        .size(dw, dh)
                        .gap(kMsgRowGap)
                        .step(18.0f)
                        .theme(themeTokens)
                        .contentKey("agent.dialog.detail.detail.scroll.v" +
                                    std::to_string(msgScrollVersion_) + ".n" +
                                    std::to_string(detailMessages_.size()) +
                                    ".o" +
                                    std::to_string(messagesOpen_ ? 1 : 0) +
                                    ".x" +
                                    std::to_string(expandedMsgSeqs_.size()) +
                                    ".e" +
                                    std::to_string(
                                        detailMessagesError_.size()) +
                                    ".d" +
                                    std::to_string(static_cast<int>(detailTopH)) +
                                    ".w" + std::to_string(static_cast<int>(dw)))
                        .content([&](eui::Ui& inner, float cw, float) {
                            drawDetailBlocks(inner, cw, 0.0f);
                            // 详情块 → 消息标题的定位间隔（flow 布局无绝对 y，
                            // 用透明占位撑出标题行高）
                            inner.rect("agent.dialog.detail.detail.scroll.gap")
                                .size(cw, kMsgsTitleH)
                                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                .build();
                            msgsHeaderRow(inner, cw);
                            if (!detailMessagesError_.empty()) {
                                inner.text("agent.dialog.detail.msgs.error")
                                    .size(cw, lhS)
                                    .text("消息读取失败：" + detailMessagesError_)
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .wrap(true)
                                    .maxWidth(cw)
                                    .color(t.colors.error)
                                    .build();
                                return;
                            }
                            if (detailMessages_.empty()) {
                                inner.text("agent.dialog.detail.empty")
                                    .size(cw,
                                          std::clamp(dh - kMsgsTitleH,
                                                     kMsgAreaMinH,
                                                     kMsgAreaMaxH))
                                    .text("（选中的 run 还没有消息）")
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();
                                return;
                            }
                            if (!messagesOpen_) {
                                inner.text("agent.dialog.detail.msgs.hint")
                                    .size(cw, lhS)
                                    .text("展开查看完整链路")
                                    .fontSize(fsS)
                                    .lineHeight(lhS)
                                    .color(t.colors.textTertiary)
                                    .build();
                                return;
                            }
                            for (size_t i = 0; i < detailMessages_.size(); ++i) {
                                renderMessageRow(inner, cw, "agent.dialog.detail",
                                                 i, detailMessages_[i],
                                                 selRun->startedAt, t,
                                                 themeTokens);
                            }
                        })
                        .build();
                })
                .build();
        };

        if (wide) {
            composeRunlist(x0, mainY, leftW, mainH);
            composeDetailColumn(rightX, mainY, rightW, mainH);
        } else {
            // 窄屏单列：摘要 → 配置头 → segmented（两 tab）→ 当前 tab 内容。
            // segmented 组件不暴露 position，包一层 wrap stack 落位
            ui.stack("agent.dialog.detail.nav.wrap")
                .position(x0, navY)
                .size(innerW, navH)
                .content([&] {
                    components::segmented(ui, "agent.dialog.detail.nav")
                        .size(innerW, navH)
                        .items({"执行记录", "运行详情"})
                        .selected(detailTab_)
                        .fontSize(fsS)
                        .theme(themeTokens)
                        .onChange([this](int index) { detailTab_ = index; })
                        .build();
                })
                .build();
            if (detailTab_ == 0) {
                composeRunlist(x0, contentY, innerW, contentH);
            } else {
                composeDetailColumn(x0, contentY, innerW, contentH);
            }
        }

        // —— 配置浮层（spec §6.4，P3）：先画 scrim 再画 popover，盖在主区上。
        // 两者是弹窗内容内的相对层（z19/z20），不改变 dialog 自身 z=1000；
        // 浮层不改变主区几何——关闭后主区原位（测试钉「开合前后 runlist y 不变」）
        if (configOpen_) {
            constexpr float kFieldLabelW = 64.0f;   // §9.3 配置标签宽
            constexpr float kFieldGap = 10.0f;      // §9.3 配置字段行距
            const float scrimY = configY + configH;
            const float popoverY = scrimY + 4.0f;
            const float popoverMaxH = std::max(0.0f, h - popoverY - 24.0f);
            // 字段值宽 = 浮层内宽 − 左右 pad − 标签 − gap（wrap 字段按它估高）
            const float valueX = kCardPad + kFieldLabelW + 6.0f;
            const float valueW =
                std::max(0.0f, innerW - 2.0f * kCardPad - kFieldLabelW - 6.0f);
            const AgentTaskRow* cfgTask =
                detailTaskRow_ ? &*detailTaskRow_ : nullptr;
            // 字段表（§7.2）：读取失败/缺数据显「—」，不造假；problem 无异常不渲染
            struct CfgField {
                const char* id;
                const char* label;
                std::string value;
                bool wrap;
                bool error;
            };
            std::vector<CfgField> fields;
            auto taskValue = [&](const std::string& v) {
                return cfgTask != nullptr ? v : std::string("—");
            };
            fields.push_back({"goal", "目标",
                              taskValue(cfgTask ? cfgTask->goal : std::string()),
                              true, false});
            fields.push_back({"trigger", "触发",
                              taskValue(cfgTask
                                            ? triggerLabelOf(cfgTask->triggerType)
                                            : std::string()),
                              false, false});
            std::string scheduleV = "—";
            if (cfgTask != nullptr) {
                if (cfgTask->triggerType == "schedule") {
                    const std::string cron = cronTextOf(*cfgTask);
                    scheduleV = cron.empty() ? "（未设置）" : cron;
                } else if (cfgTask->triggerType == "once") {
                    const std::string onceText = onceTextOf(*cfgTask);
                    scheduleV = onceText.empty() ? "（未设置）" : onceText;
                }
            }
            fields.push_back({"schedule", "定时", scheduleV, false, false});
            fields.push_back({"model", "模型槽位",
                              taskValue(cfgTask ? cfgTask->modelSlot
                                                : std::string()),
                              false, false});
            fields.push_back({"iterations", "迭代上限",
                              taskValue(cfgTask
                                            ? std::to_string(cfgTask->maxIterations)
                                            : std::string()),
                              false, false});
            fields.push_back({"destructive", "破坏性",
                              taskValue(cfgTask
                                            ? std::string(cfgTask->allowDestructive
                                                              ? "允许"
                                                              : "禁止")
                                            : std::string()),
                              false, false});
            std::string limitV = "不限次数";
            if (cfgTask != nullptr) {
                // 限次语义口径与列表徽章同源：失败也算跑过，承诺写「跑完自动停用」
                const auto limitIt = taskRunLimits_.find(cfgTask->id);
                if (limitIt != taskRunLimits_.end()) {
                    limitV = runLimitText(limitIt->second) + "（跑完自动停用）";
                }
            } else {
                limitV = "—";
            }
            fields.push_back({"limit", "次数上限", limitV, false, false});
            if (cfgTask != nullptr) {
                const auto problemIt = taskTriggerProblems_.find(cfgTask->id);
                if (problemIt != taskTriggerProblems_.end()) {
                    fields.push_back({"problem", "配置异常", problemIt->second,
                                      true, true});
                }
            }
            fields.push_back({"context", "执行记忆",
                              taskValue(cfgTask && !cfgTask->contextNote.empty()
                                            ? cfgTask->contextNote
                                            : std::string()),
                              true, false});
            // 内容高估算（§6.4 min(内容高, popoverMaxH)；真实超高由 scrollView 内滚）
            float contentH = 0.0f;
            for (const auto& f : fields) {
                contentH +=
                    (f.wrap ? estimateTextBlockHeight(f.value, valueW, fsS, lhS)
                            : lhS) +
                    kFieldGap;
            }
            contentH -= kFieldGap;
            const float kPopoverPad = 12.0f;   // §9.3 卡片内边距
            const float scrollH =
                std::min(std::max(0.0f, contentH),
                         std::max(0.0f, popoverMaxH - 2.0f * kPopoverPad));
            const float popoverH = scrollH + 2.0f * kPopoverPad;
            const std::string cfgBase = "agent.dialog.detail.config";
            ui.rect(cfgBase + ".scrim")
                .position(x0, scrimY)
                .size(innerW, std::max(0.0f, h - scrimY))
                .color(components::theme::withOpacity(t.colors.textPrimary,
                                                      0.08f))
                .zIndex(19)
                .onClick([this] { configOpen_ = false; })
                .build();
            ui.stack(cfgBase + ".popover")
                .position(x0, popoverY)
                .size(innerW, popoverH)
                .zIndex(20)
                .content([&] {
                    ui.rect(cfgBase + ".popover.bg")
                        .size(innerW, popoverH)
                        .color(t.colors.surfaceElevated)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .build();
                    components::scrollView(ui, cfgBase + ".popover.scroll")
                        .position(kPopoverPad, kPopoverPad)
                        .size(std::max(0.0f, innerW - 2.0f * kPopoverPad),
                              scrollH)
                        .step(18.0f)
                        .theme(themeTokens)
                        .contentKey(
                            cfgBase + ".popover.scroll.v" +
                            std::to_string(cfgTask ? cfgTask->goal.size() : 0) +
                            "." +
                            std::to_string(cfgTask ? cfgTask->contextNote.size()
                                                   : 0) +
                            "." +
                            (cfgTask ? cfgTask->triggerType : std::string()) +
                            "." +
                            (cfgTask ? cfgTask->triggerConfig : std::string()) +
                            ".p" +
                            std::to_string([&] {
                                if (cfgTask == nullptr) return size_t{0};
                                const auto it =
                                    taskTriggerProblems_.find(cfgTask->id);
                                return it != taskTriggerProblems_.end()
                                           ? it->second.size()
                                           : size_t{0};
                            }()) +
                            ".f" + std::to_string(fsS))
                        .content([&](eui::Ui& f, float fw, float) {
                            for (const auto& fld : fields) {
                                const std::string base =
                                    cfgBase + ".body." + fld.id;
                                const float valueH =
                                    fld.wrap
                                        ? estimateTextBlockHeight(
                                              fld.value, valueW, fsS, lhS)
                                        : lhS;
                                const float valW =
                                    std::max(0.0f, fw - (valueX - kPopoverPad));
                                f.row(base)
                                    .size(fw, valueH)
                                    .content([&] {
                                        f.text(base + ".label")
                                            .size(kFieldLabelW, lhS)
                                            .text(fld.label)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .color(t.colors.textTertiary)
                                            .build();
                                        f.text(base + ".value")
                                            .size(valW, valueH)
                                            .text(fld.value)
                                            .fontSize(fsS)
                                            .lineHeight(lhS)
                                            .wrap(fld.wrap)
                                            .maxWidth(valW)
                                            .color(fld.error
                                                       ? t.colors.error
                                                       : t.colors.textPrimary)
                                            .build();
                                    })
                                    .build();
                            }
                        })
                        .build();
                })
                .build();
        }
    });
}

// Esc 关最上层弹窗（详情优先）。键走得到本通道 = 焦点元素没消费（输入框聚焦时已被 onCancel 收走）
} // namespace module::agent
