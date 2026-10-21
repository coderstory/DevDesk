// 详情弹窗 UI 绘制——composeTaskDetailDialog 的完整实现。
#include "module_agent/task_draft_parse.h"
// 从 agent_module.cpp 迁出以降低 God-class 体积（原约 1289 行）。
//
// UAT6 方案 A 重设计（demo agent-detail-redesign.html 方案 A 面板，HTML/CSS 即设计稿）：
//   弹性宽 = max(720, 窗口/2)（与任务弹窗同口径）；高 660。
//   头部 = 真实任务名（不再恒「任务详情」）+ 状态行 + 右侧动作 chips
//  （停止红 chip · 编辑任务 · ✕）；摘要三卡常驻横排；顶部 segment 三档
//   （执行记录 | 运行详情 | 任务配置，选中绿底白字——与任务弹窗触发 segment 同语言，
//   旧 EUI-NEO segmented 暖橙选中块退役）；底部右对齐 关闭|编辑任务。
//   旧三套 IA 全部退役：宽屏双栏 / 窄屏 2-tab / 配置 scrim+popover 浮层——
//   统一为「单列三档 tab」一套几何走全尺寸，配置收进 tab 2（.config.body.<field>
//   字段 id 沿用旧浮层契约）。数据源/回调零变化，只动布局与样式。

#include "agent_module.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include <components/button.h>
#include <components/dialog.h>
#include <components/scrollview.h>
#include <core/window/window_backend.h>
#include <eui/dsl.h>
#include <ui_kit/glass_card.h>
#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include <core/window/window_backend.h>
#include "module_agent/agent_time_text.h"
#include "module_agent/message_blocks.h"
#include "module_agent/trigger_config.h"
#include "module_agent/agent_ui_shared.h"

namespace module::agent {

namespace {

// 详情弹窗独有常量（仅 composeTaskDetailDialog 消费）
constexpr float kDetailDialogH = 552.0f;   // UAT6 弹窗收口：原 660→552（紧凑档，余 309 内容高仍够三 tab）
constexpr float kDetailMinW = 720.0f;      // 弹性宽最小档（方案 A 720）
constexpr float kNavH = 32.0f;             // segment 高（任务弹窗触发 segment 同档）
constexpr float kStopChipW = 176.0f;
constexpr float kStopChipH = 32.0f;
constexpr float kEditChipW = 88.0f;        // 「编辑任务」chip（4 CJK @13 + 余量）
constexpr float kEditChipH = 32.0f;        // chip 高（actionChip 同档）
constexpr float kHeaderChipY = 12.0f;      // 头部动作 chip 带（UAT6.1: -4 收紧顶部留白）
constexpr float kMsgAreaMinH = 96.0f;
constexpr float kMsgAreaMaxH = 520.0f;

// 弹窗实际宽/高的单边复算——仅本 TU 使用（composeTaskDetailDialog）。
// 公式与 EUI-NEO components/dialog.h 的钳制逻辑对齐：
//   width  = min(width_,  screenWidth_  - metrics_.spacing.overlay)
//   height = min(height_, screenHeight_ - metrics_.spacing.overlay)
// overlayInset 取 themeTokens.metrics.spacing.overlay（默认 48.0f），DevDesk
// 不硬编码同一值——读上游 true source。内容坐标全部取钳后真值（面板被上游
// 再钳时内容不越界）。
float detailDialogActualExtent(float requested, float screenExtent,
                               float overlayInset) {
    return std::min(requested, std::max(0.0f, screenExtent - overlayInset));
}

// 配置 tab「定时」字段：schedule→cron 原文 / once→闹钟时间 / event→事件中文名列表；
// manual·resident·daily 不消费定时载荷，显「—」（不造假）。
std::string detailScheduleText(const AgentTaskRow& task) {
    if (task.triggerType == "schedule") {
        const std::string cron = cronTextOf(task);
        return cron.empty() ? std::string("（未设置）") : cron;
    }
    if (task.triggerType == "once") {
        const std::string onceText = onceTextOf(task);
        return onceText.empty() ? std::string("（未设置）") : onceText;
    }
    if (task.triggerType == "event") {
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        if (cfg.isOk() && !cfg.value().events.empty()) {
            std::string joined;
            for (const std::string& ev : cfg.value().events) {
                std::string label = ev;
                for (int i = 0; i < kEventCount; ++i) {
                    if (ev == kEventNames[i]) {
                        label = kEventLabels[i];
                        break;
                    }
                }
                if (!joined.empty()) joined += " · ";
                joined += label;
            }
            return joined;
        }
        return std::string("（未设置）");
    }
    return std::string("—");
}

} // namespace

void AgentModule::composeTaskDetailDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float overlayInset = themeTokens.metrics.spacing.overlay;
    // 弹性宽 = max(720, 窗口/2)（方案 A 最小档 720，与任务弹窗同口径）+ 高 660，
    // 两边都经上游 overlay 钳制复算（小窗口下内容坐标与面板真值一致）
    const float w = detailDialogActualExtent(
        std::max(kDetailMinW, screen.width * 0.5f), screen.width, overlayInset);
    const float h =
        detailDialogActualExtent(kDetailDialogH, screen.height, overlayInset);
    const float innerW = std::max(0.0f, w - ui_kit::kDialogBodyInsetX);
    const float x0 = ui_kit::kDialogInsetX;

    // 文本高度全部由缩放后字号派生（fontScale 只缩 fontSize，行高按比例重算）
    const float fsS = t.sizes.fontSizeSmall;
    const float fsB = t.sizes.fontSizeBody;
    const float lhS = static_cast<float>(std::lround(fsS * 1.4f));
    const float lhB = static_cast<float>(std::lround(fsB * 1.4f));

    // —— 区域划分（面板内坐标；单列三档 tab 一套几何，无宽窄分叉）——
    const float summaryY = 48.0f;   // 头部之下（UAT6.1: 64→48 收紧顶部留白，与左右边距 28 协调）
    const float summaryH =
        std::max(81.0f, 2.0f * 10.0f + lhS + lhB + lhS + 4.0f);
    const float navY = summaryY + summaryH + 14.0f;   // segment（三档）
    const float contentY = navY + kNavH + 8.0f;
    const float footerBtnY = h - 48.0f;   // 底部按钮带（btn 高 36，底边距 12）
    const float contentH =
        std::max(56.0f, h - contentY - (h - footerBtnY + 12.0f));

    // 微信组件令牌（本弹窗局部）：module 级 themeTokens 仍是 core 暖橙（定义点的
    // 整体切换归「弹窗根治」批），本文件内 chip 全部传 wechat 版（UAT2 #11 延续）。
    // id 契约（<id>.bg 命中面 + 文本穿透可点）经 actionChip 原样保留。
    const auto wcTokens = core::theme::componentTokensWeChat(t);
    // 停止 chip = 危险语义（文案「停止 · 当前轮结束后生效」）：红底白字
    // wechat.warningRed #FA5151；hover/pressed/边框由 actionChip 从 base 派生
    auto stopTokens = wcTokens;
    stopTokens.primary = t.colors.error;

    components::dialog(ui, "agent.dialog.detail")
        .open(detailTaskId_.has_value())
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .onOpenChange([this](bool v) {
            if (!v) closeTaskDetail();
        })
        .content([&] {
            if (!detailTaskId_) return;

            const AgentTaskRow* taskRow =
                detailTaskRow_ ? &*detailTaskRow_ : nullptr;
            const auto runsIt = taskRuns_.find(*detailTaskId_);
            const std::vector<AgentRunRow>* runs =
                runsIt != taskRuns_.end() ? &runsIt->second : nullptr;
            const bool hasRuns = runs != nullptr && !runs->empty();
            const AgentRunRow* latestRun = hasRuns ? &runs->front() : nullptr;
            const bool runActive = isTaskRunActive(*detailTaskId_);

            const int64_t nowSec = core::time::nowUnix();
            // 「下一次」缓存：键判等早退，nextCronMatch 回扫不在逐帧路径
            if (detailTaskRow_) {
                updateNextRunCache(*detailTaskRow_, latestRun, nowSec);
            }

            // —— 头部：任务名 + 状态行 + 右侧动作 chips（方案 A）——
            // 右缘预算（从右往左）：✕ 28 | gap | 编辑任务 88 | gap | 停止 176（运行中）
            const float closeX = w - 56.0f;
            const float editX = closeX - kDialogGap - kEditChipW;
            const float stopX = editX - kDialogGap - kStopChipW;
            // 名字/状态行的右边界让位给在场的 chip 组（运行中含停止 chip 更挤）
            const float headerRightX =
                (runActive ? stopX : editX) - kDialogGap;
            ui.text("agent.dialog.detail.title")
                .position(x0, 10.0f)
                .size(std::max(0.0f, headerRightX - x0), 26.0f)
                .text(taskRow ? taskRow->name : std::string("任务详情"))
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(26.0f)
                .fontWeight(700)
                .wrap(false)
                .maxWidth(std::max(0.0f, headerRightX - x0))
                .color(t.colors.textPrimary)
                .build();
            // 状态行：任务详情 · ● <状态> · <触发摘要>（状态着色，其余灰）
            std::string statusText = "从未执行";
            eui::Color statusColor = t.colors.textTertiary;
            if (latestRun != nullptr) {
                statusText = runStatusText(latestRun->status);
                statusColor = statusDotColor(latestRun->status, t);
            }
            constexpr float kKickerW = 56.0f;   // 「任务详情」4 CJK @13
            constexpr float kStatusW = 64.0f;  // 「● 已取消」@13 + 余量
            ui.text("agent.dialog.detail.header.kicker")
                .position(x0, 34.0f)
                .size(kKickerW, lhS)
                .text("任务详情")
                .fontSize(fsS)
                .lineHeight(lhS)
                .color(t.colors.textTertiary)
                .build();
            ui.text("agent.dialog.detail.header.status")
                .position(x0 + kKickerW + 8.0f, 34.0f)
                .size(kStatusW, lhS)
                .text("● " + statusText)
                .fontSize(fsS)
                .lineHeight(lhS)
                .wrap(false)
                .maxWidth(kStatusW)
                .color(statusColor)
                .build();
            const float metaX = x0 + kKickerW + 8.0f + kStatusW + 8.0f;
            const float metaW = std::max(0.0f, headerRightX - metaX);
            std::string metaText;
            eui::Color metaColor = t.colors.textSecondary;
            if (taskRow != nullptr) {
                if (taskRow->triggerType == "schedule") {
                    const std::string cron = cronTextOf(*taskRow);
                    metaText = "定时 · cron " +
                               (cron.empty() ? std::string("（未设置）") : cron);
                } else if (taskRow->triggerType == "once") {
                    const std::string onceText = onceTextOf(*taskRow);
                    metaText = "闹钟 · " +
                               (onceText.empty() ? std::string("（未设置）") : onceText);
                } else {
                    metaText = triggerLabelOf(taskRow->triggerType);
                }
            } else {
                metaText = "任务信息读取失败";
                metaColor = t.colors.error;
            }
            ui.text("agent.dialog.detail.header.meta")
                .position(metaX, 34.0f)
                .size(metaW, lhS)
                .text(metaText)
                .fontSize(fsS)
                .lineHeight(lhS)
                .wrap(false)
                .maxWidth(metaW)
                .color(metaColor)
                .build();

            // ✕（与底部关闭 / 遮罩 / Esc 同一路径）
            ui.text("agent.dialog.detail.close")
                .position(closeX, 18.0f)
                .size(28.0f, 28.0f)
                .text("✕")
                .fontSize(14.0f)
                .lineHeight(28.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .onClick([this] { closeTaskDetail(); })
                .build();

            // 「编辑任务」chip：走既有编辑弹窗；详情弹窗后画会盖住任务弹窗
            //（compose 顺序 task→detail），必须先关详情再开编辑
            ui.stack("agent.dialog.detail.edit.wrap")
                .position(editX, kHeaderChipY)
                .size(kEditChipW, kEditChipH)
                .content([&] {
                    actionChip(ui, "agent.dialog.detail.edit", "编辑任务",
                               kEditChipW, /*primary=*/false, /*enabled=*/true,
                               wcTokens, [this, taskId = *detailTaskId_] {
                                   closeTaskDetail();
                                   openTaskEditor(taskId);
                               }, fsS);
                })
                .build();

            // 「停止」chip（C1）：只在有在跑 run 时渲染——无 ActiveRun 可置位的 run
            //（含崩溃遗留的 DB running 行）没有可停止对象，渲染入口等于骗点击。
            // 点击后切「停止中」禁用（cancel 位幂等，禁用只为给点击一个反馈）
            if (runActive) {
                const bool stopping = isStopRequested(*detailTaskId_);
                ui.stack("agent.dialog.detail.stop.wrap")
                    .position(stopX, kHeaderChipY)
                    .size(kStopChipW, kStopChipH)
                    .content([&] {
                        actionChip(ui, "agent.dialog.detail.stop",
                                   stopping ? "停止中 · 当前轮结束后生效"
                                            : "停止 · 当前轮结束后生效",
                                   kStopChipW, /*primary=*/true,
                                   /*enabled=*/!stopping, stopTokens,
                                   [this, taskId = *detailTaskId_] {
                                       requestStopRun(taskId);
                                   }, fsS);
                    })
                    .build();
            }

            // —— 摘要三卡（常驻横排；真实数据）——
            // 汇总查询失败必须显形（静默失败面）：last/stats 值转「读取失败」error 色
            const bool summaryFailed = !runSummariesError_.empty();
            // 最近一次：主值 = 状态 · 耗时（运行中 = now-startedAt；无 ended = 状态裸值）
            std::string lastValue = summaryFailed ? "读取失败" : "从未执行";
            std::string lastSub;
            eui::Color lastValueColor =
                summaryFailed ? t.colors.error : t.colors.textPrimary;
            if (!summaryFailed && latestRun != nullptr) {
                if (latestRun->status == "running") {
                    int64_t secs = nowSec - latestRun->startedAt;
                    if (secs < 0) secs = 0;
                    lastValue = "运行中 · " + formatRunDurationText(secs);
                    lastValueColor = t.colors.info;
                } else if (latestRun->endedAt) {
                    int64_t secs = *latestRun->endedAt - latestRun->startedAt;
                    if (secs < 0) secs = 0;   // 时钟回拨防御
                    lastValue = std::string(runStatusText(latestRun->status)) +
                                " · " + formatRunDurationText(secs);
                    lastValueColor = statusDotColor(latestRun->status, t);
                } else {
                    lastValue = runStatusText(latestRun->status);
                    lastValueColor = statusDotColor(latestRun->status, t);
                }
                lastSub = formatLocalShortDateTime(latestRun->startedAt) + " · " +
                          relativeTimeText(latestRun->startedAt, nowSec);
            }
            // 近 7 天：主值 = 成功 N · 失败 M；副值 = 其中受限 K · P%
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
            // 下一次：describeNextRun 的真话（手动/事件不造假时间）
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
            auto summaryCard = [&](const std::string& id, float cx, float cy,
                                   float cw, const char* label,
                                   const std::string& value,
                                   eui::Color valueColor, const std::string& sub,
                                   eui::Color subColor) {
                ui.stack(id)
                    .position(cx, cy)
                    .size(cw, summaryH)
                    .content([&] {
                        ui.rect(id + ".bg")
                            .size(cw, summaryH)
                            .color(components::theme::withOpacity(t.colors.surface, 0.62f))
                            .radius(t.sizes.radius)
                            .border(1.0f, components::theme::withOpacity(t.colors.surface, 0.55f))
                            .build();
                        ui_kit::drawGlassCardLayers(ui, id,
                            0.0f, 0.0f, cw, summaryH,
                            t.sizes.radius, t,
                            ui_kit::GlassCardStyle::subtle());
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
                    })
                    .build();
            };
            constexpr float kCardGap = 8.0f;
            const float cardW =
                std::max(0.0f, (innerW - 2.0f * kCardGap) / 3.0f);
            summaryCard("agent.dialog.detail.summary.last", x0, summaryY, cardW,
                        "最近一次", lastValue, lastValueColor, lastSub,
                        t.colors.textTertiary);
            summaryCard("agent.dialog.detail.summary.next",
                        x0 + cardW + kCardGap, summaryY, cardW, "下一次",
                        nextInfo.value,
                        toneColor(nextInfo.tone, t.colors.textPrimary),
                        nextInfo.detail,
                        toneColor(nextInfo.tone, t.colors.textTertiary));
            summaryCard("agent.dialog.detail.summary.stats",
                        x0 + 2.0f * (cardW + kCardGap), summaryY, cardW,
                        "近 7 天", statsValue,
                        summaryFailed ? t.colors.error
                                      : t.colors.textPrimary,
                        statsSub, t.colors.textTertiary);

            // —— 顶部 segment 三档（方案 A）：执行记录 | 运行详情 | 任务配置 ——
            // 选中 = 绿底白字（任务弹窗触发 segment 同语言；旧暖橙选中块退役）。
            // 旧 .nav（EUI-NEO segmented，.hit.<i>）id 让位给手搓 segment：
            // 可点面 = .nav.item.<i>.bg（button 组件契约）
            const int activeTab = std::clamp(detailTab_, 0, 2);
            constexpr float kSegGap = 6.0f;
            const float segW = std::max(0.0f, (innerW - 2.0f * kSegGap) / 3.0f);
            const char* kDetailTabLabels[3] = {"执行记录", "运行详情", "任务配置"};
            ui.stack("agent.dialog.detail.nav")
                .position(x0, navY)
                .size(innerW, kNavH)
                .content([&] {
                    for (int i = 0; i < 3; ++i) {
                        const bool on = activeTab == i;
                        components::button(
                            ui, "agent.dialog.detail.nav.item." +
                                    std::to_string(i))
                            .position(static_cast<float>(i) * (segW + kSegGap),
                                      0.0f)
                            .size(segW, kNavH)
                            .text(kDetailTabLabels[i])
                            .fontSize(12.0f)
                            .colors(on ? t.colors.primary
                                       : t.colors.background,
                                    on ? t.colors.primaryHover
                                       : t.colors.hover,
                                    on ? t.colors.primaryPressed
                                       : t.colors.primarySoftBg)
                            .textColor(on ? t.colors.onPrimary
                                          : t.colors.textPrimary)
                            .radius(8.0f)
                            .border(1.0f,
                                    on ? t.colors.primary
                                       : t.colors.borderDefault)
                            .onClick([this, i] { detailTab_ = i; })
                            .build();
                    }
                })
                .build();

            // —— 时间线（tab 0）：按天分组 + 行容器化 ——
            // 行 = <i> 容器（stack），子节点 .bg（命中面+选中底）/ .dot / .time /
            // .status / .duration；失败行内 .err（48 字节摘要）[.err.hint 仅 LLM 配置类]。
            // onClick 落 .bg；点行自动切「运行详情」tab（方案 A 要点 ③）
            auto composeRunlist = [&](float rx, float ry, float rw, float rh) {
                const float titleH = std::max(22.0f, lhB);
                // 行高 fs13 派生（err/hint 各 +16）
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
                            // key 含 size/选中/最新状态（运行中耗时文本变化
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
                                    // 日期分组头：今天 / 昨天 / YYYY-MM-DD
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
                                    // 耗时：结束 = ended-started（负差钳 0）；
                                    // 运行中 = now-started
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
                                            // .bg = 命中面 + 选中底（未选中透明，
                                            // hover 微亮）。alpha=0 = 透明底语义常量
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
                                                    // 消息展开态并收回消息区；
                                                    // 点行自动切「运行详情」tab
                                                    expandedMsgSeqs_.clear();
                                                    autoExpandedSeq_.reset();
                                                    messagesOpen_ = false;
                                                    detailTab_ = 1;
                                                    ++msgScrollVersion_;
                                                    tasksDirty_ = true;
                                                })
                                                .build();
                                            if (highlighted) {
                                                // 选中行左缘 2px primary
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
                                                .horizontalAlign(eui::HorizontalAlign::Right)
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
                                                // 「去 Claude 页」引导：仅「LLM 配置
                                                // 不可用」类错误渲染
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
                        // 20 条上限提示（不提供分页）
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

            // —— tab 1 运行详情：状态/元信息 2 列网格/计划对照/错误全文 + 消息区 ——
            // 元信息 6 行收进 2 列网格（方案 A：右栏不再挤——旧 6 行竖排浪费 400 宽）；
            // 余高充足 = 详情块固定 + .msgscroll 独立滚；余高不足 96 = .detail.scroll
            // 整栏滚（不建 .msgscroll，避免嵌套滚动）
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
                // 详情块高度预算：状态行 + 元信息 3 行（2 列网格）+ 计划对照
                // + [错误 label+滚动区]；错误块出现时预算变大，消息余高不足自动降级
                const float statusH = std::max(24.0f, lhB + 2.0f);
                const float metaH = 3.0f * lhS + 2.0f * 4.0f;
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
                    selRun != nullptr &&
                    (dh - detailTopH - kMsgsTitleH - 12.0f) >= kMsgAreaMinH;

                // 标题行 + 复制 chip + 区级开合 chip（flow 排）：固定态包在绝对定位
                // stack 里，滚动态直接进 .detail.scroll 内容列。copyfinal 与 toggle 都
                // 常驻标题行（折叠态 copyfinal 仍可见）
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
                            // 「复制回复」：一键复制 run 最终回复全文（最后一条
                            // assistant 的全部 text 块）。空串不渲染
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
                                            wcTokens, [this, text = finalText] {
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
                            // 区级开合（默认折叠）：展开时「最终 assistant 默认展开」
                            // 规则生效
                            inner.stack("agent.dialog.detail.msgs.toggle.wrap")
                                .size(kMsgsToggleW, kMsgsTitleH)
                                .content([&] {
                                    actionChip(
                                        inner, "agent.dialog.detail.msgs.toggle",
                                        messagesOpen_ ? "收起" : "展开",
                                        kMsgsToggleW,
                                        /*primary=*/false, /*enabled=*/true,
                                        wcTokens,
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

                // run 详情块：固定区与整栏滚动内容共用一套绘制（返回消耗高度）。
                // 布局必须流式：scrollView 内容里 .position 会被流式布局接管，
                // 区块统一走 column/row 流式排——外层列 gap 8（区块间），meta 网格
                // 行内 2 列（列距 20，行距 4）。
                // ID 契约：.detail.status[.dot/.text] / .detail.id /
                // .detail.meta.<field>[.label/.value] / .detail.schedule[.plan/
                // .actual/.verdict] / .detail.error[.label/.scroll/.text]
                auto drawDetailBlocks = [&](eui::Ui& inner, float cw, float y0) {
                    if (selRun == nullptr) return y0;
                    const std::string stText = runStatusText(selRun->status);
                    const eui::Color stColor = statusDotColor(selRun->status, t);
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
                    // 2 列网格列序（方案 A kv2）：开始|结束 / 耗时|迭代 / 输入|输出
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
                                    // pill 底/边 = 状态色 withAlpha 0.14/0.40
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
                            // 元信息 2 列网格：3 行 × 2 单元格；单元格 = stack 内
                            // label(64) + value（.meta.<field> 根 id 沿用旧竖排契约）
                            constexpr float kMetaGridGapX = 20.0f;
                            const float cellW =
                                std::max(0.0f, (cw - kMetaGridGapX) * 0.5f);
                            inner.column("agent.dialog.detail.detail.meta")
                                .size(cw, metaH)
                                .gap(4.0f)
                                .content([&] {
                                    for (int row = 0; row < 3; ++row) {
                                        inner.row("agent.dialog.detail.detail.meta.grid." +
                                                  std::to_string(row))
                                            .size(cw, lhS)
                                            .gap(kMetaGridGapX)
                                            .content([&] {
                                                for (int slot = 0; slot < 2; ++slot) {
                                                    const MetaRow& m =
                                                        metaRows[row * 2 + slot];
                                                    const std::string base =
                                                        std::string(
                                                            "agent.dialog.detail.detail.meta.") +
                                                        m.id;
                                                    inner.stack(base)
                                                        .size(cellW, lhS)
                                                        .content([&] {
                                                            inner.text(base + ".label")
                                                                .position(0.0f, 0.0f)
                                                                .size(64.0f, lhS)
                                                                .text(m.label)
                                                                .fontSize(fsS)
                                                                .lineHeight(lhS)
                                                                .color(t.colors.textTertiary)
                                                                .build();
                                                            inner.text(base + ".value")
                                                                .position(70.0f, 0.0f)
                                                                .size(std::max(
                                                                          0.0f,
                                                                          cellW - 70.0f),
                                                                      lhS)
                                                                .text(m.value)
                                                                .fontSize(fsS)
                                                                .lineHeight(lhS)
                                                                .wrap(false)
                                                                .maxWidth(std::max(
                                                                    0.0f, cellW - 70.0f))
                                                                .color(t.colors.textSecondary)
                                                                .build();
                                                        })
                                                        .build();
                                                }
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
                            // 无 run（或选中失效）：空态
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
                                // 折叠态：只给占位提示，不建 .msgscroll
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
                                // 用透明占位撑出标题行高）。alpha=0 = 透明占位语义常量
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

            // —— tab 2 任务配置：只读展示当前 TaskDef（方案 A：配置收进 tab，
            // scrim+popover 浮层机制退役；字段 id 沿用旧浮层 .config.body.<field>）——
            auto composeConfigTab = [&](float cx, float cy, float cw, float ch) {
                const AgentTaskRow* cfgTask =
                    detailTaskRow_ ? &*detailTaskRow_ : nullptr;
                auto taskValue = [&](const std::string& v) {
                    return cfgTask != nullptr ? v : std::string("—");
                };
                constexpr float kFieldLabelW = 64.0f;    // 配置标签宽（同旧浮层）
                constexpr float kMetaGridGapX = 20.0f;   // 2 列网格列距
                // 限次语义口径与列表徽章同源：失败也算跑过，承诺写「跑完自动停用」
                std::string limitV = "不限次数";
                if (cfgTask != nullptr) {
                    const auto limitIt = taskRunLimits_.find(cfgTask->id);
                    if (limitIt != taskRunLimits_.end()) {
                        limitV = runLimitText(limitIt->second) + "（跑完自动停用）";
                    }
                } else {
                    limitV = "—";
                }
                // 配置异常（problem 仅异常时渲染，error 色显形——零噪音）
                std::string problemV;
                if (cfgTask != nullptr) {
                    const auto problemIt = taskTriggerProblems_.find(cfgTask->id);
                    if (problemIt != taskTriggerProblems_.end()) {
                        problemV = problemIt->second;
                    }
                }
                const std::string cfgBase = "agent.dialog.detail.config";
                components::scrollView(ui, cfgBase + ".scroll")
                    .position(cx, cy)
                    .size(cw, ch)
                    .step(18.0f)
                    .theme(themeTokens)
                    .contentKey(cfgBase + ".scroll.k" +
                                std::to_string(cfgTask ? cfgTask->id : 0) +
                                "." +
                                std::to_string(cfgTask ? cfgTask->goal.size() : 0) +
                                "." +
                                std::to_string(
                                    cfgTask ? cfgTask->contextNote.size() : 0) +
                                "." +
                                std::to_string(
                                    cfgTask ? cfgTask->triggerConfig.size() : 0) +
                                "." +
                                std::to_string(
                                    cfgTask ? cfgTask->systemPrompt.size() : 0) +
                                ".p" + std::to_string(problemV.size()) +
                                ".f" + std::to_string(fsS))
                    .content([&](eui::Ui& f, float fw, float) {
                        // 单元格：stack 内 label(64) + value；网格列宽由调用方传入
                        auto cfgCell = [&](const std::string& field,
                                          const char* label,
                                          const std::string& value, float width,
                                          eui::Color valueColor) {
                            const std::string base =
                                cfgBase + ".body." + field;
                            f.stack(base)
                                .size(width, lhS)
                                .content([&] {
                                    f.text(base + ".label")
                                        .position(0.0f, 0.0f)
                                        .size(kFieldLabelW, lhS)
                                        .text(label)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .color(t.colors.textTertiary)
                                        .build();
                                    f.text(base + ".value")
                                        .position(kFieldLabelW + 6.0f, 0.0f)
                                        .size(std::max(0.0f, width - kFieldLabelW - 6.0f),
                                              lhS)
                                        .text(value)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .wrap(false)
                                        .maxWidth(std::max(
                                            0.0f, width - kFieldLabelW - 6.0f))
                                        .color(valueColor)
                                        .build();
                                })
                                .build();
                        };
                        // 长文本块（指令/执行记忆）：pageBg 灰底 + wrap 全文（demo .cfgtext）
                        auto cfgBlock = [&](const std::string& field,
                                            const char* label,
                                            const std::string& value) {
                            const std::string base =
                                cfgBase + ".body." + field;
                            constexpr float kBlockPad = 10.0f;
                            const float textW =
                                std::max(0.0f, fw - 2.0f * kBlockPad);
                            const float valueH =
                                estimateTextBlockHeight(value, textW, fsS, lhS);
                            const float blockH = 2.0f * 8.0f + lhS + 4.0f + valueH;
                            f.stack(base)
                                .size(fw, blockH)
                                .content([&] {
                                    f.rect(base + ".bg")
                                        .size(fw, blockH)
                                        .color(t.colors.background)
                                        .radius(t.sizes.radius)
                                        .build();
                                    f.text(base + ".label")
                                        .position(kBlockPad, 8.0f)
                                        .size(textW, lhS)
                                        .text(label)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .color(t.colors.textTertiary)
                                        .build();
                                    f.text(base + ".value")
                                        .position(kBlockPad, 8.0f + lhS + 4.0f)
                                        .size(textW, valueH)
                                        .text(value)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .wrap(true)
                                        .maxWidth(textW)
                                        .color(t.colors.textPrimary)
                                        .build();
                                })
                                .build();
                        };
                        auto groupTitle = [&](const char* id, const char* label) {
                            f.text(cfgBase + ".group." + id)
                                .size(fw, lhS)
                                .text(label)
                                .fontSize(fsS)
                                .lineHeight(lhS)
                                .fontWeight(600)
                                .color(t.colors.textTertiary)
                                .build();
                        };

                        // —— 触发与执行 ——
                        groupTitle("trigger", "触发与执行");
                        // 名称占整行（长名不截断成 2 列半宽）
                        cfgCell("name", "名称",
                                taskValue(cfgTask ? cfgTask->name
                                                  : std::string()),
                                fw, t.colors.textPrimary);
                        {
                            const float colW =
                                std::max(0.0f, (fw - kMetaGridGapX) * 0.5f);
                            struct CellSpec {
                                const char* field;
                                const char* label;
                                std::string value;
                            };
                            const CellSpec cells[6] = {
                                {"trigger", "触发",
                                 taskValue(cfgTask
                                               ? triggerLabelOf(cfgTask->triggerType)
                                               : std::string())},
                                {"schedule", "定时",
                                 cfgTask != nullptr
                                     ? detailScheduleText(*cfgTask)
                                     : std::string("—")},
                                {"model", "模型槽位",
                                 taskValue(cfgTask ? cfgTask->modelSlot
                                                   : std::string())},
                                {"iterations", "迭代上限",
                                 taskValue(cfgTask
                                               ? std::to_string(cfgTask->maxIterations)
                                               : std::string())},
                                {"limit", "次数上限", limitV},
                                {"autonote", "自动记录",
                                 cfgTask != nullptr
                                     ? std::string(cfgTask->autoNote != 0
                                                       ? "开启"
                                                       : "关闭")
                                     : std::string("—")},
                            };
                            for (int row = 0; row < 3; ++row) {
                                f.row(cfgBase + ".grid." + std::to_string(row))
                                    .size(fw, lhS)
                                    .gap(kMetaGridGapX)
                                    .content([&] {
                                        for (int slot = 0; slot < 2; ++slot) {
                                            const CellSpec& c = cells[row * 2 + slot];
                                            cfgCell(c.field, c.label, c.value,
                                                    colW,
                                                    t.colors.textPrimary);
                                        }
                                    })
                                    .build();
                            }
                        }
                        // 指令（目标全文，wrap 不截断）
                        cfgBlock("goal", "指令",
                                 taskValue(cfgTask ? cfgTask->goal
                                                   : std::string()));

                        // —— 安全与记忆 ——
                        groupTitle("safety", "安全与记忆");
                        // 破坏性：允许 = 红字（spec §8 item 5 高危语义）/ 禁止 = 绿
                        cfgCell("destructive", "破坏性",
                                taskValue(cfgTask
                                              ? std::string(cfgTask->allowDestructive
                                                                ? "允许"
                                                                : "禁止")
                                              : std::string()),
                                std::max(0.0f, (fw - kMetaGridGapX) * 0.5f),
                                cfgTask != nullptr && cfgTask->allowDestructive
                                    ? t.colors.error
                                    : t.colors.success);
                        if (!problemV.empty()) {
                            // 配置异常：error 色全文（wrap），与普通值不同色即显形
                            const float probW =
                                std::max(0.0f, fw - kFieldLabelW - 6.0f);
                            const float probH = std::max(
                                lhS, estimateTextBlockHeight(problemV, probW, fsS,
                                                             lhS));
                            f.stack(cfgBase + ".body.problem")
                                .size(fw, probH)
                                .content([&] {
                                    f.text(cfgBase + ".body.problem.label")
                                        .position(0.0f, 0.0f)
                                        .size(kFieldLabelW, lhS)
                                        .text("配置异常")
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .fontWeight(600)
                                        .color(t.colors.error)
                                        .build();
                                    f.text(cfgBase + ".body.problem.value")
                                        .position(kFieldLabelW + 6.0f, 0.0f)
                                        .size(probW, probH)
                                        .text(problemV)
                                        .fontSize(fsS)
                                        .lineHeight(lhS)
                                        .wrap(true)
                                        .maxWidth(probW)
                                        .color(t.colors.error)
                                        .build();
                                })
                                .build();
                        }
                        cfgBlock("context", "执行记忆",
                                 cfgTask != nullptr
                                     ? (cfgTask->contextNote.empty()
                                            ? std::string("（未设置）")
                                            : cfgTask->contextNote)
                                     : std::string("—"));
                        // 系统提示词：配置 tab 只读展示（编辑走任务弹窗）
                        cfgBlock("sysprompt", "系统提示词",
                                 cfgTask != nullptr
                                     ? (cfgTask->systemPrompt.empty()
                                            ? std::string("（未设置 · 将使用自动生成）")
                                            : cfgTask->systemPrompt)
                                     : std::string("—"));
                        // 只读口径：改配置走「编辑任务」（AI 起草也在编辑弹窗）
                        f.text(cfgBase + ".readonly.hint")
                            .size(fw, lhS)
                            .text("详情弹窗只读 · 改动配置走「编辑任务」")
                            .fontSize(fsS - 1.0f)
                            .lineHeight(lhS)
                            .color(t.colors.textTertiary)
                            .build();
                    })
                    .build();
            };

            // —— 三档 tab 内容（互斥渲染；存在性即档位，测试据此判档）——
            if (activeTab == 0) {
                composeRunlist(x0, contentY, innerW, contentH);
            } else if (activeTab == 1) {
                composeDetailColumn(x0, contentY, innerW, contentH);
            } else {
                composeConfigTab(x0, contentY, innerW, contentH);
            }

            // —— 底部按钮带：hint 左 + 关闭|编辑任务 右对齐（demo .dlgf）——
            const float footerCloseX =
                w - ui_kit::kDialogBodyInsetX - 2.0f * 104.0f - 8.0f;
            ui.text("agent.dialog.detail.footer.hint")
                .position(x0, footerBtnY + 12.0f)
                .size(std::max(0.0f, footerCloseX - kDialogGap - x0), 16.0f)
                .text("执行记录上限 20 条 · 详情只读，改动配置走「编辑任务」")
                .fontSize(fsS - 2.0f)
                .lineHeight(16.0f)
                .wrap(false)
                .color(t.colors.textTertiary)
                .build();
            {
                const float bw = 104.0f;
                const float bh = 36.0f;
                components::button(ui, "agent.dialog.detail.footer.close")
                    .position(footerCloseX, footerBtnY)
                    .size(bw, bh)
                    .text("关闭")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.background,
                            t.colors.hover,
                            t.colors.primarySoftBg)
                    .textColor(t.colors.textPrimary)
                    .radius(8.0f)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { closeTaskDetail(); })
                    .build();
                components::button(ui, "agent.dialog.detail.footer.edit")
                    .position(w - ui_kit::kDialogBodyInsetX - bw, footerBtnY)
                    .size(bw, bh)
                    .text("编辑任务")
                    .fontSize(t.sizes.fontSizeSmall)
                    .colors(t.colors.primary,
                            t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(8.0f)
                    .border(1.0f, t.colors.primary)
                    .onClick([this, taskId = *detailTaskId_] {
                        closeTaskDetail();
                        openTaskEditor(taskId);
                    })
                    .build();
            }
        })
        .build();
}

// Esc 关最上层弹窗（详情优先）。键走得到本通道 = 焦点元素没消费（输入框聚焦时已被 onCancel 收走）
} // namespace module::agent
