// memory_module_panels.cpp
// 从 memory_module.cpp 拆出（2026-10-31）：记忆页主视图分区——动态流、主控台、
// 提案卡、已裁决卡、审核台、配置页。

#include "memory_module_internal.h"

namespace module::memory {

namespace {
// 动态流行距（B6：随字号缩放，禁止字面量进行 y）
constexpr float kFeedRowH = 27.0f;
constexpr float kLedgerRowH = 26.0f;
}  // namespace

void MemoryModule::composeConsole(eui::Ui& ui, const core::theme::Tokens& t,
                                  float x, float w, float y, float viewH) {
    (void)viewH;
    const auto snap = panel_.snapshot();
    // bug158（2026-11-03）：滚动位移由 EUI scrollContentFrom 对内容列自动平移，
    // 内容里不再手动减 offset（双处相加 = 滚动时内容两倍速漂移）。need159
    // （2026-11-03）：顶部降级横条及其「启动引擎」按钮删除——启动入口只保留
    // 引擎卡一处，引擎停止态由引擎卡状态点与详情行承载。
    const float oy = y;

    float cy = oy;

    // 面板库故障（可读错误，不留白）
    if (!snap.storesOk) {
        ui.text("memory.stores.error")
            .position(x, cy)
            .size(w, core::theme::scaled(22.0f))
            .text("面板数据通道异常：" + snap.storesError)
            .fontSize(core::theme::scaled(12.5f))
            .lineHeight(core::theme::scaled(22.0f))
            .color(t.colors.errorText)
            .build();
        cy += 26.0f;
    }

    composeEngineCard(ui, t, x, w, cy);
    // need:181 分层高度链（设计 §1.5 预算）：状态带 64 + 健康格 132 + 消耗带
    // 104 + 折叠组头 44 = 392px（L1 一屏），展开运行明细后内滚
    cy += core::theme::scaled(64.0f) + kCardGap;
    composeHealthGrid(ui, t, x, w, cy);
    cy += core::theme::scaled(132.0f) + kCardGap;

    // 消耗带 104px（设计 §2.4）：标题行 + 12px 横带进度 + 数字行；
    // 软/硬档数字与口径 note → L2 hover 浮层
    {
        const float kUsageH = core::theme::scaled(104.0f);
        ui_kit::cardPanel(ui, "memory.usage.card", x, cy, w, kUsageH, t.sizes.radiusLarge, t);
        ui.text("memory.usage.title")
            .position(x + 16.0f, cy + core::theme::scaled(12.0f))
            .size(320.0f, core::theme::scaled(22.0f))
            .text("token 消耗（今日 / 本月）")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(core::theme::scaled(22.0f))
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        // 预算档随配置（需-9：软/硬阈值 = 用量告警月档同一套数，配置卡可调）
        engine::TokenUsageMeter usage;
        usage.monthTokens = snap.monthTokens;
        usage.softBudgetTokens = snap.settings.usageSoftBudgetTokens;
        usage.hardBudgetTokens = snap.settings.usageHardBudgetTokens;
        const auto phase = usage.phase();
        ui.text("memory.usage.phase")
            .position(x + w - 240.0f, cy + core::theme::scaled(13.0f))
            .size(224.0f, core::theme::scaled(20.0f))
            .text(phase == engine::TokenUsageMeter::Phase::Hard
                      ? "已触发硬档 · 提炼暂停"
                      : phase == engine::TokenUsageMeter::Phase::Soft
                            ? "逼近预算 · 软档提醒"
                            : "预算充足")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(phase == engine::TokenUsageMeter::Phase::Normal
                       ? t.colors.textTertiary
                       : t.colors.warning)
            .build();
        const float barX = x + 16.0f;
        const float barW = w - 32.0f;
        ui.rect("memory.usage.bar")
            .position(barX, cy + core::theme::scaled(40.0f))
            .size(barW, core::theme::scaled(12.0f))
            .color(t.colors.active)
            .radius(t.sizes.radiusPill)
            .build();
        const float fillW = static_cast<float>(usage.monthRatio()) * barW;
        if (fillW > 1.0f) {
            ui.rect("memory.usage.fill")
                .position(barX, cy + core::theme::scaled(40.0f))
                .size(fillW, core::theme::scaled(12.0f))
                .color(phase == engine::TokenUsageMeter::Phase::Normal
                           ? t.colors.primary
                           : t.colors.warning)
                .radius(t.sizes.radiusPill)
                .build();
        }
        char numBuf[200];
        std::snprintf(
            numBuf, sizeof(numBuf),
            "今日 %lld tk · %s · 本月 %lld tk · %s",
            snap.todayTokens, formatMilliYuan(snap.todayCostMilli).c_str(),
            snap.monthTokens, formatMilliYuan(snap.monthCostMilli).c_str());
        ui.text("memory.usage.nums")
            .position(barX, cy + core::theme::scaled(58.0f))
            .size(barW, core::theme::scaled(20.0f))
            .text(numBuf)
            .fontSize(core::theme::scaled(12.5f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textSecondary)
            .build();
        char tipBuf[280];
        std::snprintf(tipBuf, sizeof(tipBuf),
                      "软档 %lld · 硬档 %lld（配置页用量告警可调）\n口径：码点数 ≈ CJK 1 字 1 token "
                      "的估算账面；单价按智谱计费页常量，注入记账不计金额。",
                      usage.softBudgetTokens, usage.hardBudgetTokens);
        components::tooltip(ui, "memory.usage.tip")
            .theme(core::theme::componentTokensWeChat(t))
            .source("memory.usage.card")
            .value(tipBuf)
            .anchor(x + w * 0.5f, cy + kUsageH)
            .bounds(4096.0f, 4096.0f)
            .minWidth(320.0f)
            .maxWidth(420.0f)
            .build();
        cy += kUsageH + kCardGap;
    }

    // 运行明细折叠区（设计 §2.5）：调用记录表 + 动态 feed 两张常驻卡收进
    // L3 折叠（默认收起），组头摘要报最新一条 ledger 行
    {
        const float headH = core::theme::scaled(44.0f);
        const auto& rows = snap.recentLedger;
        auto kindDisplay = [](const std::string& kind) -> std::string {
            if (kind == "llm_input") return "提炼 · 输入";
            if (kind == "llm_output") return "提炼 · 产出";
            if (kind == "embed") return "向量化";
            if (kind == "inject_items") return "注入 · 条数";
            if (kind == "inject_chars") return "注入 · 字数";
            if (kind == "hook_search") return "hook 检索";
            return kind;
        };
        ui.rect("memory.details.head")
            .position(x, cy)
            .size(w, headH)
            .color(t.colors.surface)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { consoleDetailsOpen_ = !consoleDetailsOpen_; })
            .build();
        ui.text("memory.details.title")
            .position(x + 16.0f, cy)
            .size(120.0f, headH)
            .text("运行明细")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(headH)
            .fontWeight(600)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textPrimary)
            .build();
        const std::string sumText =
            rows.empty()
                ? std::string("引擎启动后这里记账")
                : ("最近 " + panel::formatFeedTime(rows.front().ts) + " " +
                   kindDisplay(rows.front().kind) + " · " +
                   std::to_string(rows.front().tokens) + " tk");
        ui.text("memory.details.sum")
            .position(x + 140.0f, cy)
            .size(w - 190.0f, headH)
            .text(sumText)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(headH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        ui.text("memory.details.chev")
            .position(x + w - 40.0f, cy)
            .size(24.0f, headH)
            .text(consoleDetailsOpen_ ? "⌄" : "›")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(headH)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        cy += headH;
        if (consoleDetailsOpen_) {
            const float innerX = x + 16.0f;
            const float innerW = w - 32.0f;
            float by = cy + 8.0f;
            // ① 最近调用记录（cap 10 行；列规格沿用原表 :241-308）
            ui.text("memory.recent.title")
                .position(innerX, by)
                .size(320.0f, core::theme::scaled(18.0f))
                .text("最近调用记录")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .fontWeight(600)
                .color(t.colors.textSecondary)
                .build();
            by += core::theme::scaled(24.0f);
            const float kRowH = core::theme::scaled(kLedgerRowH);
            const int showN = static_cast<int>(
                std::min<std::size_t>(rows.size(), 10));
            const float cTime = innerX;
            const float cKindW = 160.0f;
            const float cKind = cTime + 140.0f;
            const float cCount = cKind + cKindW;
            const float cCountW = 120.0f;
            const float cTok = cCount + cCountW;
            const float cTokW = 120.0f;
            if (!rows.empty()) {
                auto colHeader = [&](const char* text, float cx, float cw,
                                     bool right) {
                    ui.text(std::string("memory.recent.h.") + text)
                        .position(cx, by)
                        .size(cw, core::theme::scaled(18.0f))
                        .text(text)
                        .fontSize(core::theme::scaled(11.0f))
                        .lineHeight(core::theme::scaled(18.0f))
                        .horizontalAlign(right ? eui::HorizontalAlign::Right
                                               : eui::HorizontalAlign::Left)
                        .color(t.colors.textTertiary)
                        .build();
                };
                colHeader("时间", cTime, 130.0f, false);
                colHeader("类型", cKind, cKindW, false);
                colHeader("条数/字数", cCount, cCountW, true);
                colHeader("token · 金额", cTok, cTokW, true);
                by += core::theme::scaled(20.0f);
            }
            for (int i = 0; i < showN; ++i) {
                const auto& r = rows[static_cast<std::size_t>(i)];
                const float ry = by + static_cast<float>(i) * kRowH;
                const std::string rid = std::to_string(i);
                ui.text("memory.recent.r" + rid + ".time")
                    .position(cTime, ry)
                    .size(130.0f, core::theme::scaled(20.0f))
                    .text(panel::formatFeedTime(r.ts))
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .color(t.colors.textSecondary)
                    .build();
                ui.text("memory.recent.r" + rid + ".kind")
                    .position(cKind, ry)
                    .size(cKindW, core::theme::scaled(20.0f))
                    .text(kindDisplay(r.kind))
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .color(t.colors.textSecondary)
                    .build();
                char countBuf[40];
                std::snprintf(countBuf, sizeof(countBuf), "%lld", r.tokens);
                ui.text("memory.recent.r" + rid + ".count")
                    .position(cCount, ry)
                    .size(cCountW, core::theme::scaled(20.0f))
                    .text(countBuf)
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textPrimary)
                    .build();
                char tokBuf[64];
                std::snprintf(tokBuf, sizeof(tokBuf), "%lld · %s", r.tokens,
                              formatMilliYuan(r.costMilliYuan).c_str());
                ui.text("memory.recent.r" + rid + ".tok")
                    .position(cTok, ry)
                    .size(cTokW, core::theme::scaled(20.0f))
                    .text(tokBuf)
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textTertiary)
                    .build();
            }
            if (rows.empty()) {
                ui_kit::EmptyStateOptions opts;
                opts.message = "还没有调用记录";
                opts.guide =
                    "引擎启动后，提炼 / 向量化 / 注入的账目会逐条记在这里。";
                ui_kit::emptyState(ui, "memory.recent.empty", opts, innerX, by,
                                   innerW, core::theme::scaled(64.0f), t);
                by += core::theme::scaled(64.0f);
            } else {
                by += static_cast<float>(showN) * kRowH + core::theme::scaled(6.0f);
                ui.text("memory.recent.all")
                    .position(cTok - 60.0f, by)
                    .size(180.0f, core::theme::scaled(22.0f))
                    .text("查看全部（导出 JSON）")
                    .fontSize(core::theme::scaled(11.5f))
                    .lineHeight(core::theme::scaled(22.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.info)
                    .onClick([this] {
                        auto r = panel_.exportAllToJson();
                        if (r.isFail()) {
                            showToast("导出未完成", r.errorMessage(), true);
                        } else {
                            showToast("已导出", r.value());
                        }
                    })
                    .build();
                by += core::theme::scaled(26.0f);
            }
            // ② 1px 分隔 + 最近动态（cap 8 行）
            ui.rect("memory.details.sep")
                .position(x + 16.0f, by + 4.0f)
                .size(innerW, 1.0f)
                .color(t.colors.borderMuted)
                .build();
            by += 12.0f;
            ui.text("memory.feed.title")
                .position(innerX, by)
                .size(320.0f, core::theme::scaled(18.0f))
                .text("最近动态")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .fontWeight(600)
                .color(t.colors.textSecondary)
                .build();
            by += core::theme::scaled(24.0f);
            if (snap.feed.empty()) {
                ui_kit::EmptyStateOptions opts;
                opts.message = "还没有动态";
                opts.guide =
                    "引擎启动并产生采集 / 注入 / 提炼后，这里会滚动显示。";
                ui_kit::emptyState(ui, "memory.feed.empty", opts, innerX, by,
                                   innerW, core::theme::scaled(64.0f), t);
            } else {
                auto feedColor = [&](panel::FeedItem::Kind k) {
                    switch (k) {
                        case panel::FeedItem::Kind::Collect: return t.colors.success;
                        case panel::FeedItem::Kind::Blocked: return t.colors.warning;
                        case panel::FeedItem::Kind::Inject: return t.colors.info;
                        case panel::FeedItem::Kind::Merge: return t.colors.textSecondary;
                        case panel::FeedItem::Kind::Curate: return t.colors.primary;
                        default: return t.colors.textTertiary;
                    }
                };
                const int feedN = static_cast<int>(
                    std::min<std::size_t>(snap.feed.size(), 8));
                for (int i = 0; i < feedN; ++i) {
                    const auto& item =
                        snap.feed[static_cast<std::size_t>(i)];
                    const float ry =
                        by + static_cast<float>(i) * core::theme::scaled(kFeedRowH);
                    ui.text("memory.feed.dot" + std::to_string(i))
                        .position(innerX, ry)
                        .size(14.0f, core::theme::scaled(24.0f))
                        .text("●")
                        .fontSize(core::theme::scaled(9.0f))
                        .lineHeight(core::theme::scaled(24.0f))
                        .color(feedColor(item.kind))
                        .build();
                    ui.text("memory.feed.txt" + std::to_string(i))
                        .position(innerX + 18.0f, ry)
                        .size(innerW - 170.0f, core::theme::scaled(24.0f))
                        .text(item.text)
                        .fontSize(core::theme::scaled(12.5f))
                        .lineHeight(core::theme::scaled(24.0f))
                        .color(t.colors.textSecondary)
                        .build();
                    ui.text("memory.feed.time" + std::to_string(i))
                        .position(x + w - 16.0f - 134.0f, ry)
                        .size(134.0f, core::theme::scaled(24.0f))
                        .text(panel::formatFeedTime(item.ts))
                        .fontSize(core::theme::scaled(11.5f))
                        .lineHeight(core::theme::scaled(24.0f))
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .color(t.colors.textTertiary)
                        .build();
                }
            }
        }
    }
}

// ── 视图 2：记忆审核 ──

void MemoryModule::composeProposalCard(eui::Ui& ui,
                                       const core::theme::Tokens& t, float x,
                                       float w, float y,
                                       const panel::PanelSnapshot& snap,
                                       const Proposal& p, bool batchable) {
    (void)snap;
    // need:181（设计 §3.3）：提案卡 232 → 176px；置信度+来源合并为右端摘要
    // （L2 hover）；need:183：证据链行改开「提案全文」只读弹窗（多行滚动可复制）
    const float kCardH = core::theme::scaled(176.0f);
    ui_kit::cardPanel(ui, "memory.rv.card." + p.id, x, y, w, kCardH, t.sizes.radiusLarge, t);

    // 批量勾选（仅批量模式；仅可批量类型）
    if (batchMode_) {
        const bool checked = batchSel_.count(p.id) > 0;
        ui.rect("memory.rv.pick." + p.id)
            .position(x + 12.0f, y + core::theme::scaled(14.0f))
            .size(18.0f, 18.0f)
            .color(checked ? t.colors.primary : t.colors.surface)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, checked ? t.colors.primary : t.colors.borderDefault)
            .onClick([this, id = p.id, ok = batchable, checked] {
                if (!ok) {
                    showToast("批量限制",
                              "批量接受仅对「遗忘 / 新增」开放，这张请逐张处理",
                              true);
                    return;
                }
                if (checked) {
                    batchSel_.erase(id);
                } else {
                    batchSel_.insert(id);
                }
            })
            .build();
        if (checked) {
            ui.text("memory.rv.pickmark." + p.id)
                .position(x + 12.0f, y + core::theme::scaled(13.0f))
                .size(18.0f, core::theme::scaled(18.0f))
                .text("✓")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.onPrimary)
                .build();
        }
    }

    // 类型徽章 + 标题 + 右端摘要（置信度档 + 来源短词合并，hover 出全口径）
    const float hx = x + (batchMode_ ? 38.0f : 16.0f);
    const std::string kind = kindLabel(p.kind);
    const float kindW = 2.0f * 12.0f + 20.0f;
    ui.rect("memory.rv.kindchip." + p.id)
        .position(hx, y + core::theme::scaled(10.0f))
        .size(kindW, 22.0f)
        .color(t.colors.active)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text("memory.rv.kindtext." + p.id)
        .position(hx, y + core::theme::scaled(11.0f))
        .size(kindW, core::theme::scaled(20.0f))
        .text(kind)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(core::theme::scaled(20.0f))
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(kindColor(t, p.kind))
        .build();

    std::string title = p.resultText;
    if (title.empty() && !p.evidence.empty()) title = p.evidence.front().quoted;
    if (title.size() > 32) {
        // bug217（2026-11-04）：32 字节硬切曾劈开 UTF-8 码点（标题尾部乱码 ??），
        // 回退到码点边界再截
        std::size_t cut = 32;
        while (cut > 0 &&
               (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        title = title.substr(0, cut) + "…";
    }
    if (title.empty()) title = "（无正文）";
    // bug184（2026-11-07）：诚实降级——采集路径未提炼的 add 卡（resultType 空，
    // 存量占位 "fact" 同判）不虚标「新增记忆：」，按原始片段标注保证可辨识
    const bool unrefinedAdd = p.kind == "add" &&
                              (p.resultType.empty() || p.resultType == "fact");
    const std::string titleText =
        unrefinedAdd
            ? "原始片段（未分类）：" + title
            : (p.kind == "add"
                   ? "新增记忆：" + title
                   : (p.kind == "forget"
                          ? "遗忘过时记忆：" + title
                          : (p.kind == "invalidate"
                                 ? "标记过期记忆：" + title
                                 : "合并重复记忆：" + title)));
    // bug217（2026-11-04）：行盒宽度预算截断（窄窗标题曾横向溢出压到右端摘要）
    const float kMetaW = 190.0f;
    const float kTitleBudget = w - (hx - x) - kindW - kMetaW - 20.0f;
    ui.text("memory.rv.title." + p.id)
        .position(hx + kindW + 10.0f, y + core::theme::scaled(10.0f))
        .size(kTitleBudget, 22.0f)
        .text(truncateByAdvance(titleText, kTitleBudget,
                                core::theme::scaled(13.5f)))
        .fontSize(core::theme::scaled(13.5f))
        .lineHeight(core::theme::scaled(22.0f))
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.rv.meta." + p.id)
        .position(x + w - 16.0f - kMetaW, y + core::theme::scaled(11.0f))
        .size(kMetaW, core::theme::scaled(20.0f))
        .text(truncateByAdvance(
            "置信度 " + confidenceTier(p.confidence) + " · 来自 " +
                srcLabel(p.source),
            kMetaW, core::theme::scaled(11.5f)))
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(core::theme::scaled(20.0f))
        .horizontalAlign(eui::HorizontalAlign::Right)
        .color(t.colors.textTertiary)
        .build();
    components::tooltip(ui, "memory.rv.meta.tip." + p.id)
        .theme(core::theme::componentTokensWeChat(t))
        .source("memory.rv.meta." + p.id)
        .value("置信度 " + confidenceTier(p.confidence) + " · 来源 " +
               srcLabel(p.source) + " · 观察于 " +
               panel::formatFeedTime(p.createdAt))
        .anchor(x + w - 16.0f - kMetaW * 0.5f, y + core::theme::scaled(34.0f))
        .bounds(4096.0f, 4096.0f)
        .minWidth(260.0f)
        .maxWidth(340.0f)
        .build();

    // 正文（add/merge = 将生成/合并结果；forget/invalidate = 拟处理原文）
    std::string body;
    if (unrefinedAdd) {
        // 价值摘要如实：未提炼无归纳，给处理建议代替虚假摘要
        body = "原始片段（未经 AI 提炼，建议「编辑后接受」整理成完整记忆）：";
        body += p.resultText;
    } else if (p.kind == "add") {
        body = "将新增：" + p.resultText;
    } else if (p.kind == "merge") {
        body = "合并结果（将生成）：" + p.resultText;
    } else if (!p.evidence.empty()) {
        body = p.kind == "forget"
                   ? std::string("拟遗忘（移出自动想起）：")
                   : std::string("拟标记为过期：");
        body += p.evidence.front().quoted;
    } else {
        body = "目标条目 #" +
               (p.targetIds.empty() ? std::string("0") : p.targetIds.front());
    }
    // bug217（2026-11-04）：正文盒 56px ≈ 3 行（lineHeight 18）——按 3 行像素
    // 预算截断（真库均值 ~1975 CJK 字符曾纵向溢出压住证据链/操作钮/下一卡）。
    // 全文出口 = 「编辑后接受」对话框（editText_ 载入未截断原文）。
    ui.text("memory.rv.body." + p.id)
        .position(x + 16.0f, y + core::theme::scaled(36.0f))
        .size(w - 32.0f, core::theme::scaled(52.0f))
        .wrap()
        .text(truncateByAdvance(body, 3.0f * (w - 32.0f) -
                                             3.0f * core::theme::scaled(12.5f),
                                core::theme::scaled(12.5f)))
        .fontSize(core::theme::scaled(12.5f))
        .lineHeight(core::theme::scaled(17.0f))
        .color(t.colors.textSecondary)
        .build();

    // 证据链（来源会话/时间/原文摘录；默认折叠）
    std::string evText;
    if (!p.evidence.empty()) {
        const auto& e = p.evidence.front();
        evText = "证据链：来自 " + panel::formatFeedTime(e.observedAt) + " · 会话 " +
                 (e.sessionId.empty() ? std::string("(未知)") : e.sessionId) +
                 (e.quoted.empty() ? std::string() : ("｜「" + e.quoted + "」"));
    } else {
        evText = "证据链：无（扫描类提案，无会话出处）";
    }
    // bug234（2026-11-04）：原 ▾/▸ 切态只翻箭头不展全文 = 死交互；改为单行截断 +
    // 「查看全文」入口。need:183：全文出口改为「提案全文」只读弹窗（多行滚动
    // 可复制，改动不落状态）；编辑通路仍走「编辑后接受」。
    const std::string evtoggleText =
        truncateByAdvance("查看全文 ▸ " + evText,
                          (w - 32.0f) - core::theme::scaled(11.5f) * 2.0f,
                          core::theme::scaled(11.5f));
    ui.text("memory.rv.evtoggle." + p.id)
        .position(x + 16.0f, y + core::theme::scaled(92.0f))
        .size(w - 32.0f, core::theme::scaled(24.0f))
        .text(evtoggleText)
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(core::theme::scaled(24.0f))
        .color(t.colors.info)
        .onClick([this, body = body, ev = evText] {
            textDialogTitle_ = "提案全文";
            textDialogBody_ = body + "\n\n" + ev;
            textDialogOpen_ = true;
        })
        .build();

    // 底部操作（拒绝 / 编辑后接受 / 接受）
    const float by = y + core::theme::scaled(136.0f);
    ui_kit::button(ui, "memory.rv.reject." + p.id, "拒绝",
                   ui_kit::ButtonLevel::Text, x + 16.0f, by, 76.0f, 32.0f, t,
                   {[this, id = p.id] {
                       auto r = panel_.rejectProposal(id);
                       if (r.isFail()) {
                           showToast("拒绝失败", r.errorMessage(), true);
                       } else {
                           showToast("已拒绝", "这条提案已留档，不再打扰你");
                       }
                   }});
    ui_kit::button(ui, "memory.rv.edit." + p.id, "编辑后接受",
                   ui_kit::ButtonLevel::Secondary, x + 100.0f, by, 118.0f,
                   32.0f, t,
                   {[this, id = p.id, text = p.resultText] {
                       editingId_ = id;
                       editText_ = text;
                   }});
    primaryButton(ui, "memory.rv.accept." + p.id, "接受", x + w - 100.0f, by,
                  84.0f, 32.0f, t, [this, id = p.id] {
                      auto r = panel_.acceptProposal(id);
                      if (r.isFail()) {
                          showToast("未能执行", r.errorMessage(), true);
                      } else {
                          showToast("已接受", "提案已执行，可在「已处理」里撤销");
                      }
                  });
}

void MemoryModule::composeDecidedCard(eui::Ui& ui,
                                      const core::theme::Tokens& t, float x,
                                      float w, float y,
                                      const panel::PanelSnapshot& snap,
                                      const Proposal& p, bool undone) {
    (void)snap;
    constexpr float kCardH = 92.0f;
    ui_kit::cardPanel(ui, "memory.rv.done." + p.id, x, y, w, kCardH, t.sizes.radiusLarge, t);
    std::string statusText;
    eui::Color statusColor = t.colors.textTertiary;
    if (p.status == "accepted") {
        if (undone) {
            statusText = "已撤销 · 记忆已恢复";
            statusColor = t.colors.warning;
        } else {
            statusText = p.appliedAt > 0 ? "已接受 · 可撤销"
                                         : "已接受 · 引擎执行待重试";
            statusColor = p.appliedAt > 0 ? t.colors.success : t.colors.warning;
        }
    } else if (p.status == "rejected") {
        statusText = "已拒绝";
        statusColor = t.colors.errorText;
    } else {
        statusText = "已过期（证据失效，可重新提案）";
        statusColor = t.colors.textTertiary;
    }
    ui.text("memory.rv.done.status." + p.id)
        .position(x + 16.0f, y + core::theme::scaled(10.0f))
        .size(220.0f, core::theme::scaled(20.0f))
        .text(statusText)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(core::theme::scaled(20.0f))
        .fontWeight(600)
        .color(statusColor)
        .build();
    // bug217（2026-11-04）：已裁决卡正文盒 40px ≈ 2 行——同口径像素预算截断
    // （resultText 长文曾纵向溢出压住撤销钮与下一卡）
    ui.text("memory.rv.done.kind." + p.id)
        .position(x + 16.0f, y + core::theme::scaled(34.0f))
        .size(w - 200.0f, core::theme::scaled(40.0f))
        .wrap()
        .text(truncateByAdvance(
            "「" + kindLabel(p.kind) + "」" +
                (p.resultText.empty() ? srcLabel(p.source) : p.resultText),
            2.0f * (w - 200.0f) - 2.0f * core::theme::scaled(12.5f),
            core::theme::scaled(12.5f)))
        .fontSize(core::theme::scaled(12.5f))
        .lineHeight(core::theme::scaled(18.0f))
        .color(t.colors.textSecondary)
        .build();
    ui.text("memory.rv.done.time." + p.id)
        .position(x + w - 190.0f, y + core::theme::scaled(10.0f))
        .size(174.0f, core::theme::scaled(20.0f))
        .text(panel::formatFeedTime(p.decidedAt))
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(core::theme::scaled(20.0f))
        .horizontalAlign(eui::HorizontalAlign::Right)
        .color(t.colors.textTertiary)
        .build();
    if (p.status == "accepted" && !undone &&
        (p.kind == "add" || p.kind == "forget")) {
        ui_kit::button(ui, "memory.rv.undo." + p.id, "撤销",
                       ui_kit::ButtonLevel::Secondary, x + w - 100.0f,
                       y + core::theme::scaled(44.0f), 84.0f, 30.0f, t,
                       {[this, id = p.id] {
                           auto r = panel_.createUndoCard(id);
                           if (r.isFail()) {
                               showToast("撤销失败", r.errorMessage(), true);
                           } else {
                               showToast("撤销卡已建",
                                         "撤销会再生一张提案，接受后记忆立即恢复");
                           }
                       }});
    }
}

void MemoryModule::composeReview(eui::Ui& ui, const core::theme::Tokens& t,
                                 float x, float w, float y, float viewH) {
    (void)viewH;
    const auto snap = panel_.snapshot();
    const float oy = y;  // 滚动位移交 EUI 自动平移（bug158）
    float cy = oy;

    // need:181 C7：审核 intro 说明行删除（极低频）——引导句已并入空态组件，
    // 口径说明走「?」语义时由空态引导句承载
    // bug184（2026-11-07）：引擎未运行 = 提炼链未跑，现存卡全是原始片段——
    // 如实降级口径（不虚标「已分类」），并说明出路
    if (TelememEngine::global().snapshot().state !=
        TelememEngine::State::Running) {
        ui.text("memory.rv.degraded")
            .position(x, cy)
            .size(w, core::theme::scaled(20.0f))
            .text("记忆引擎未运行：以下为采集的原始片段，未经 AI 提炼分类；"
                  "启动引擎后新提案将自动归纳类型。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.warning)
            .build();
        cy += 26.0f;
    }

    // 工具条（need:181 §3.2 定高 52 + need:183 筛选同类收进下拉）：类型/来源
    // 两个下拉（开合态显式托管），状态 chips（待审/已处理）保留 chips 形态；
    // 右端「批量处理」次级按钮进出批量模式（原 label+switch 240px 区退役）
    {
        const auto ct = core::theme::componentTokensWeChat(t);
        const std::vector<std::string> typeLabels = {"全部", "合并", "失效", "遗忘", "新增"};
        const std::vector<std::string> srcLabels = {"全部来源", "Claude 对话", "整理", "遗忘扫描"};
        const std::vector<std::string> stateLabels = {"待审", "已处理"};
        // 窄窗换行：状态 chips 折第二行、工具条加高 88（原分支保留）
        auto chipRowW = [](const std::vector<std::string>& labels) {
            float w = 0.0f;
            for (const auto& label : labels) {
                float chipW = 30.0f;
                for (unsigned char c : label) {
                    if (c >= 0x80 && c < 0xC0) continue;  // UTF-8 续字节不计宽
                    chipW += (c >= 0xC0) ? 12.0f : 6.0f;  // CJK 12 / ASCII 6，按首字节
                }
                w += chipW + 6.0f;
            }
            return w;
        };
        const float kBatchZone = 136.0f;  // 右端「批量处理」按钮保留区（104 + 估宽余量）
        const float chipsTotal = 12.0f + 120.0f + 10.0f + 120.0f + 14.0f +
                                 chipRowW(stateLabels);
        // need:181 §3.2：内容区 <860 时状态 chips 折第二行、高 88（设计自适应档）
        const bool wrapNarrow = chipsTotal > w - kBatchZone || w < 860.0f;
        const float kBarH = wrapNarrow ? 88.0f : 52.0f;
        const float stateRowY = wrapNarrow ? 42.0f : 10.0f;
        ui_kit::cardPanel(ui, "memory.rv.toolbar", x, cy, w, kBarH, t.sizes.radius, t);
        auto chipRow = [&](const std::string& prefix, float cx, float cyRow,
                           const std::vector<std::string>& labels, int active,
                           std::function<void(int)> onPick) {
            for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
                // chip 宽按显示字符估（全宽 12px / ASCII 6px + 24 内边距）
                float chipW = 24.0f;
                for (unsigned char c : labels[static_cast<std::size_t>(i)]) {
                    if (c >= 0xC0) chipW += 12.0f;  // 全宽字符首字节
                    else if (c < 0x80) chipW += 6.0f;
                }
                const std::string id = prefix + std::to_string(i);
                ui.rect(id)
                    .position(cx, cyRow)
                    .size(chipW, 24.0f)
                    .color(i == active ? t.colors.primarySoftBg
                                       : t.colors.surface)
                    .radius(t.sizes.radiusPill)
                    .border(1.0f, i == active ? t.colors.primary
                                              : t.colors.borderDefault)
                    .onClick([onPick, i] { onPick(i); })
                    .build();
                ui.text(id + ".t")
                    .position(cx, cyRow + 2.0f)
                    .size(chipW, core::theme::scaled(20.0f))
                    .text(labels[static_cast<std::size_t>(i)])
                    .fontSize(core::theme::scaled(11.5f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(i == active ? t.colors.primaryText
                                       : t.colors.textSecondary)
                    .build();
                cx += chipW + 6.0f;
            }
            return cx;
        };
        // 类型 / 来源下拉（同类筛选收进下拉，need:183）
        const float ddY = cy + (kBarH > 60.0f ? 10.0f : (kBarH - 32.0f) * 0.5f);
        ui.stack("memory.rv.typedd.wrap")
            .position(x + 12.0f, ddY)
            .size(120.0f, 32.0f)
            .content([&] {
                components::dropdown(ui, "memory.rv.typedd")
                    .size(120.0f, 32.0f)
                    .theme(ct)
                    .items(typeLabels)
                    .selected(filterType_)
                    .open(rvTypeDdOpen_)
                    .onOpenChange([this](bool v) { rvTypeDdOpen_ = v; })
                    .onChange([this](int v) {
                        filterType_ = v;
                        reviewShowLimit_ = kReviewShowLimit;  // bug190：筛选变化重置渐进加载
                    })
                    .build();
            })
            .build();
        ui.stack("memory.rv.srcdd.wrap")
            .position(x + 12.0f + 120.0f + 10.0f, ddY)
            .size(120.0f, 32.0f)
            .content([&] {
                components::dropdown(ui, "memory.rv.srcdd")
                    .size(120.0f, 32.0f)
                    .theme(ct)
                    .items(srcLabels)
                    .selected(filterSrc_)
                    .open(rvSrcDdOpen_)
                    .onOpenChange([this](bool v) { rvSrcDdOpen_ = v; })
                    .onChange([this](int v) {
                        filterSrc_ = v;
                        reviewShowLimit_ = kReviewShowLimit;
                    })
                    .build();
            })
            .build();
        // 状态 chips：宽窗延续第一行，窄窗折第二行
        float cx = wrapNarrow ? x + 12.0f : x + 12.0f + 120.0f + 10.0f + 120.0f + 14.0f;
        cx = chipRow("memory.rv.chipSt.", cx, cy + stateRowY, stateLabels,
                     filterState_, [this](int v) {
                         filterState_ = v;
                         reviewShowLimit_ = kReviewShowLimit;
                     });
        // 「批量处理」次级按钮（右缘锚；进出批量模式）
        ui_kit::button(ui, "memory.rv.batchbtn",
                       batchMode_ ? "退出批量" : "批量处理",
                       ui_kit::ButtonLevel::Secondary,
                       x + w - 16.0f - 104.0f, cy + (kBarH - 32.0f) * 0.5f,
                       104.0f, 32.0f, t,
                       {[this] {
                           batchMode_ = !batchMode_;
                           batchSel_.clear();
                       }});
        cy += kBarH + 8.0f;  // need219 §4.3：卡间距 8px（工具条卡→列表）

        // 批量操作条
        if (batchMode_) {
            const bool allOk =
                !batchSel_.empty() &&
                std::all_of(batchSel_.begin(), batchSel_.end(),
                            [&](const std::string& id) {
                                for (const auto& p : snap.pending) {
                                    if (p.id == id) return batchableKind(p.kind);
                                }
                                return false;
                            });
            ui.rect("memory.rv.batchbar")
                .position(x, cy)
                .size(w, 44.0f)
                .color(t.colors.primarySoftBg)
                .radius(t.sizes.radius)
                .build();
            ui.text("memory.rv.batchinfo")
                .position(x + 14.0f, cy + core::theme::scaled(12.0f))
                .size(w - 480.0f, core::theme::scaled(20.0f))
                .text("已选 " + std::to_string(batchSel_.size()) + " 项" +
                      (batchSel_.empty()
                           ? " · 在卡片左侧勾选后可批量处理"
                           : (allOk ? " · 所选提案均支持批量接受"
                                    : " · 批量接受仅对「遗忘 / 新增」开放")))
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(core::theme::scaled(20.0f))
                .color(t.colors.primaryText)
                .build();
            ui_kit::button(ui, "memory.rv.batchclear", "取消选择",
                           ui_kit::ButtonLevel::Secondary, x + w - 260.0f,
                           cy + core::theme::scaled(7.0f), 108.0f, 30.0f, t,
                           {[this] { batchSel_.clear(); }});
            if (allOk) {
                primaryButton(ui, "memory.rv.batchaccept", "批量接受",
                              x + w - 140.0f, cy + core::theme::scaled(7.0f), 124.0f, 30.0f, t,
                              [this] {
                                  std::vector<std::string> ids(
                                      batchSel_.begin(), batchSel_.end());
                                  auto r = panel_.batchAccept(ids);
                                  if (r.isFail()) {
                                      showToast("批量失败", r.errorMessage(),
                                                true);
                                  } else {
                                      showToast("批量接受完成",
                                                std::to_string(r.value()) + "/" +
                                                    std::to_string(ids.size()) +
                                                    " 条已执行（失败项保持待审）");
                                  }
                                  batchSel_.clear();
                              });
            } else {
                ui_kit::button(ui, "memory.rv.batchaccept", "批量接受",
                               ui_kit::ButtonLevel::Secondary,
                               x + w - 140.0f, cy + core::theme::scaled(7.0f), 124.0f, 30.0f, t, {});
            }
            cy += 44.0f + 8.0f;  // need219 §4.3：卡间距 8px（批量条→列表）
        }
    }

    // 卡片流（按过滤 + 状态）。bug190（2026-11-04）：渲染截断于
    // reviewShowLimit_（默认 50，筛选变化重置），matched 继续计数——上限外
    // 不 compose 卡（单帧元素数/文本度量随 pending 线性膨胀曾致主线程冻结），
    // 尾部「再显示 50 条」渐进加载。数据层 pending 不动（tab 徽章与筛选计数
    // 仍报全量真值）。
    if (filterState_ == 0) {
        int shown = 0;
        int matched = 0;
        for (const auto& p : snap.pending) {
            if (!kindMatches(filterType_, p.kind)) continue;
            if (!srcMatches(filterSrc_, p.source)) continue;
            ++matched;
            if (shown >= reviewShowLimit_) continue;
            composeProposalCard(ui, t, x, w, cy, snap, p,
                                batchableKind(p.kind));
            cy += core::theme::scaled(176.0f) + 8.0f;  // need219 §4.3：审核卡间距 8px
            ++shown;
        }
        if (matched > shown) {
            // 渐进加载尾行：如实报剩余量 + 按需增长（每批 kReviewShowLimit）
            ui.text("memory.rv.more.note")
                .position(x + 16.0f, cy + core::theme::scaled(12.0f))
                .size(w - 190.0f, core::theme::scaled(22.0f))
                .text("已显示前 " + std::to_string(shown) + " 条 · 还有 " +
                      std::to_string(matched - shown) +
                      " 条待审（也可用上方筛选缩小范围）")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(22.0f))
                .color(t.colors.textTertiary)
                .build();
            ui_kit::button(ui, "memory.rv.more", "再显示 50 条",
                           ui_kit::ButtonLevel::Secondary, x + w - 150.0f, cy,
                           134.0f, 34.0f, t,
                           {[this] { reviewShowLimit_ += kReviewShowLimit; }});
            cy += 44.0f + 12.0f;
        }
        if (shown == 0) {
            const bool totallyEmpty = snap.pending.empty();
            // 2026-11-01 R3：空态统一走 ui_kit::emptyState，不再外套大白卡
            // （原为 150px 高的卡包两行字，形态与其余页面不一致）。
            ui_kit::EmptyStateOptions opts;
            opts.message = totallyEmpty ? "没有需要你拿主意的提案"
                                        : "当前筛选下没有待审提案";
            opts.guide =
                "新提案会在 Claude 帮你整理记忆时自动出现在这里，不用盯着，不急。";
            opts.icon = 0xF0E7;  // fa-clipboard-check
            ui_kit::emptyState(ui, "memory.rv.empty", opts, x, cy, w, 150.0f, t);
        }
    } else {
        int idx = 0;
        for (std::size_t i = 0; i < snap.decided.size(); ++i) {
            const auto& p = snap.decided[i];
            if (!kindMatches(filterType_, p.kind)) continue;
            if (!srcMatches(filterSrc_, p.source)) continue;
            const bool undone =
                i < snap.decidedUndone.size() && snap.decidedUndone[i];
            composeDecidedCard(ui, t, x, w, cy, snap, p, undone);
            cy += 92.0f + 8.0f;  // need219 §4.3：已裁决卡间距 8px
            ++idx;
        }
        if (idx == 0) {
            // 2026-11-01 R3：已裁决空态同样走统一空态组件
            ui_kit::EmptyStateOptions opts;
            opts.message = "还没有处理记录";
            opts.guide = "你处理过的提案会按时间倒序留在这里，方便回溯。";
            ui_kit::emptyState(ui, "memory.rv.done.empty", opts, x, cy, w,
                               96.0f, t);
        }
    }
}

// ── 视图 3：配置 ──

void MemoryModule::composeConfig(eui::Ui& ui, const core::theme::Tokens& t,
                                 float x, float w, float y, float viewH) {
    (void)viewH;
    const auto snap = panel_.snapshot();
    const float oy = y;  // 滚动位移交 EUI 自动平移（bug158）
    float cy = oy;
    const float innerW = w - 32.0f;
    const float headH = core::theme::scaled(44.0f);

    auto warnBar = [&](const std::string& id, float cyCard,
                       const std::string& text) {
        ui.rect(id)
            .position(x + 16.0f, cyCard)
            .size(innerW, 30.0f)
            .color(t.colors.active)
            .radius(t.sizes.radiusSmall)
            .build();
        ui.text(id + ".t")
            .position(x + 26.0f, cyCard + 6.0f)
            .size(innerW - 20.0f, core::theme::scaled(18.0f))
            .text(text)
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(core::theme::scaled(18.0f))
            .color(t.colors.warning)
            .build();
    };

    // 折叠组头（need:181 §4.2）：标题 14/600 + 实时值摘要 12 + chevron；
    // 整行命中切展开态，hover 语义同行卡；组间距 8px（折叠组是轻量行不是卡）
    auto groupHead = [&](int gi, const std::string& title,
                         const std::string& sum, bool sumWarn) {
        const std::string gid = std::to_string(gi);
        ui.rect("memory.cfg.grp" + gid + ".head")
            .position(x, cy)
            .size(w, headH)
            .color(t.colors.surface)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this, gi] {
                cfgOpen_[static_cast<std::size_t>(gi)] =
                    !cfgOpen_[static_cast<std::size_t>(gi)];
            })
            .build();
        ui.text("memory.cfg.grp" + gid + ".title")
            .position(x + 16.0f, cy)
            .size(220.0f, headH)
            .text(title)
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(headH)
            .fontWeight(600)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textPrimary)
            .build();
        ui.text("memory.cfg.grp" + gid + ".sum")
            .position(x + 240.0f, cy)
            .size(w - 300.0f, headH)
            .text(sum)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(headH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(sumWarn ? t.colors.warning : t.colors.textTertiary)
            .build();
        ui.text("memory.cfg.grp" + gid + ".chev")
            .position(x + w - 40.0f, cy)
            .size(24.0f, headH)
            .text(cfgOpen_[static_cast<std::size_t>(gi)] ? "⌄" : "›")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(headH)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        cy += headH + 8.0f;
    };
    // 组体面板（内嵌）：surface 底 + 1px borderDefault + radius 10 + 内边距 16
    auto groupBody = [&](int gi, float bodyH) {
        ui.rect("memory.cfg.grp" + std::to_string(gi) + ".body")
            .position(x, cy)
            .size(w, bodyH)
            .color(t.colors.surface)
            .radius(10.0f)
            .border(1.0f, t.colors.borderDefault)
            .build();
    };

    // ── 组 0：提炼端点（LLM 一键导入 + 向量化端点并入同组；未导入 = warning
    // 摘要前置到组头，异常在 L1 可见）──
    {
        const bool imported = snap.settings.llmImportedAtSec > 0;
        groupHead(0, "提炼端点",
                  imported ? "已导入 · 智谱端点" : "未导入——提炼不可用",
                  !imported);
        if (cfgOpen_[0]) {
            const float bodyH = core::theme::scaled(300.0f);
            groupBody(0, bodyH);
            float by = cy + 16.0f;
            // LLM 端点（原端点卡内容原样迁入）
            ui.text("memory.cfg.llm.hint")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(18.0f))
                .text("Claude 的「大脑」接在哪里——一般不用手动填，从 DevDesk 网关一键带入即可。")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textSecondary)
                .build();
            by += core::theme::scaled(26.0f);
            primaryButton(ui, "memory.cfg.llm.import",
                          "从 DevDesk 网关一键导入当前 profile",
                          x + 16.0f, by, 320.0f, ui_kit::kButtonHeight, t,
                           {[this] {
                               // 模块隔离：激活 profile 经 core::LlmProfileSource
                               // 服务缝读取（装配层映射），不 include module_claude_proxy
                               auto src = ::core::ServiceLocator::requireService<
                                   ::core::LlmProfileSource>();
                               if (src.isFail()) {
                                   showToast("导入失败",
                                             "网关 profile 服务未装配（请先在 Claude 页配置并激活 profile）",
                                             true);
                                   return;
                               }
                               auto prof = src.value()->currentProfile();
                               if (prof.isFail()) {
                                   showToast("导入失败", prof.errorMessage(), true);
                                   return;
                               }
                               // 只回显端点/模型；密钥不落盘不显示（凭据通道 =
                               // settings.json → 引擎环境变量，见 telemem_config.h）
                               panel_.markLlmImported(prof.value().baseUrl);
                               showToast("已导入",
                                         "端点绑定已生效，引擎启动时写入 TELEMEM_CONFIG");
                           }});
            if (imported) {
                ui.text("memory.cfg.llm.imported")
                    .position(x + 352.0f, by + core::theme::scaled(10.0f))
                    .size(120.0f, core::theme::scaled(22.0f))
                    .text("已导入")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(22.0f))
                    .fontWeight(600)
                    .color(t.colors.success)
                    .build();
            }
            by += core::theme::scaled(50.0f);
            // 只读回显（端点定案值 + 凭据通道；不回显任何密钥形态）
            ui.text("memory.cfg.llm.rows")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(36.0f))
                .wrap()
                .text(std::string("端点 ") + engine::kLlmBaseUrl + " · 模型 " +
                      engine::kLlmModel +
                      " · 凭据 Claude 设置（网关托管，运行时注入引擎，不写配置与日志）" +
                      (imported ? " · 来源 profile " + snap.settings.llmProfileName
                                : " · 尚未导入（展开组后点击按钮绑定）"))
                .fontSize(core::theme::scaled(11.5f))
                .lineHeight(core::theme::scaled(16.0f))
                .color(t.colors.textTertiary)
                .build();
            by += core::theme::scaled(44.0f);
            warnBar("memory.cfg.llm.warn", by,
                    snap.gateEnabled ? "对话内容将发送至该端点（已过脱敏）"
                                     : "对话内容将发送至该端点（当前未经脱敏）");
            by += core::theme::scaled(38.0f);
            ui.rect("memory.cfg.grp0.sep")
                .position(x + 16.0f, by)
                .size(innerW, 1.0f)
                .color(t.colors.borderMuted)
                .build();
            by += core::theme::scaled(17.0f);
            // 向量化端点（并入同组；零 API 费口径）
            ui.text("memory.cfg.embed.subtitle")
                .position(x + 16.0f, by)
                .size(300.0f, core::theme::scaled(18.0f))
                .text("向量化端点（本机）")
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(core::theme::scaled(18.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            by += core::theme::scaled(24.0f);
            ui.text("memory.cfg.embed.rows")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(36.0f))
                .wrap()
                .text(std::string("llama-server · http://127.0.0.1:") +
                      std::to_string(engine::kEmbedServerPort) + " · 模型 " +
                      engine::kEmbedModel + " · " +
                      std::to_string(engine::kEmbedDims) + " 维（换档 = 全量重嵌）")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(17.0f))
                .color(t.colors.textSecondary)
                .build();
            by += core::theme::scaled(42.0f);
            warnBar("memory.cfg.embed.warn", by,
                    "记忆原文不出机：向量化全部落本机（零 API 费）。");
            cy += bodyH + 8.0f;
        }
    }

    // ── 组 1：触发线（摘要 = 升级线 % · 基线 n 次，不开组也能看当前值）──
    {
        groupHead(1, "触发线",
                  "升级线 " +
                      std::to_string(snap.settings.recallThresholdPct) + "% · 基线 " +
                      std::to_string(snap.settings.twoWordBaseline) + " 次",
                  false);
        if (cfgOpen_[1]) {
            const float bodyH = core::theme::scaled(220.0f);
            groupBody(1, bodyH);
            float by = cy + 16.0f;
            ui.text("memory.cfg.trig.hint")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(18.0f))
                .text("决定「什么时候提醒你升级」和「什么样的词值得记」，默认值已经调好，可微调。")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textSecondary)
                .build();
            by += core::theme::scaled(28.0f);
            // 滑条 1：升级阈值（60~95）
            {
                const int pct = snap.settings.recallThresholdPct;
                ui.text("memory.cfg.trig.s1.label")
                    .position(x + 16.0f, by)
                    .size(90.0f, core::theme::scaled(18.0f))
                    .text("升级阈值")
                    .fontSize(core::theme::scaled(12.5f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("memory.cfg.trig.s1.sub")
                    .position(x + 96.0f, by)
                    .size(innerW - 96.0f, core::theme::scaled(18.0f))
                    .text("Recall@5 低于此值时提醒（红线 85%）")
                    .fontSize(core::theme::scaled(11.0f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .color(t.colors.textTertiary)
                    .build();
                ui_kit::slider(ui, "memory.cfg.trig.s1", x + 16.0f,
                               by + core::theme::scaled(26.0f),
                               innerW - 120.0f, 20.0f,
                               static_cast<float>(pct - 60) / 35.0f, t,
                               [this](float v) {
                                   const int q =
                                       60 + static_cast<int>(v * 35.0f + 0.5f);
                                   static int last = -999;
                                   if (q != last) {
                                       last = q;
                                       panel_.setRecallThresholdPct(q);
                                   }
                               });
                char vbuf[16];
                std::snprintf(vbuf, sizeof(vbuf), "%d%%", pct);
                ui.text("memory.cfg.trig.s1.val")
                    .position(x + w - 92.0f, by + core::theme::scaled(24.0f))
                    .size(76.0f, core::theme::scaled(20.0f))
                    .text(vbuf)
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textPrimary)
                    .build();
            }
            by += core::theme::scaled(58.0f);
            // 滑条 2：两字词基线（2~10）
            {
                const int base = snap.settings.twoWordBaseline;
                ui.text("memory.cfg.trig.s2.label")
                    .position(x + 16.0f, by)
                    .size(220.0f, core::theme::scaled(18.0f))
                    .text("两字词提炼基线 · 命中次数")
                    .fontSize(core::theme::scaled(12.5f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui_kit::slider(ui, "memory.cfg.trig.s2", x + 16.0f,
                               by + core::theme::scaled(26.0f),
                               innerW - 120.0f, 20.0f,
                               static_cast<float>(base - 2) / 8.0f, t,
                               [this](float v) {
                                   const int q =
                                       2 + static_cast<int>(v * 8.0f + 0.5f);
                                   static int last = -999;
                                   if (q != last) {
                                       last = q;
                                       panel_.setTwoWordBaseline(q);
                                   }
                               });
                ui.text("memory.cfg.trig.s2.val")
                    .position(x + w - 92.0f, by + core::theme::scaled(24.0f))
                    .size(76.0f, core::theme::scaled(20.0f))
                    .text(std::to_string(base) + " 次")
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textPrimary)
                    .build();
            }
            by += core::theme::scaled(54.0f);
            // 基线磁贴（5 / 3 / 11；含义标注随组可读，bug187）
            ui.text("memory.cfg.trig.tiles-cap")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(16.0f))
                .text("记词基线 · 一个词过完这三步才会被长期记住：")
                .fontSize(core::theme::scaled(11.0f))
                .lineHeight(core::theme::scaled(16.0f))
                .color(t.colors.textTertiary)
                .build();
            by += core::theme::scaled(20.0f);
            const char* tiles[3] = {"命中 5 次 → 候选", "复现 3 轮 → 确认",
                                    "11 天窗口内有效"};
            const char* nums[3] = {"5", "3", "11"};
            for (int i = 0; i < 3; ++i) {
                const float tw = (innerW - 2.0f * 8.0f) / 3.0f;
                const float tx = x + 16.0f + static_cast<float>(i) * (tw + 8.0f);
                ui.rect("memory.cfg.trig.tile" + std::to_string(i))
                    .position(tx, by)
                    .size(tw, core::theme::scaled(30.0f))
                    .color(t.colors.active)
                    .radius(t.sizes.radiusSmall)
                    .build();
                ui.text("memory.cfg.trig.tilen" + std::to_string(i))
                    .position(tx + core::theme::scaled(10.0f),
                              by + core::theme::scaled(6.0f))
                    .size(core::theme::scaled(24.0f), core::theme::scaled(18.0f))
                    .text(nums[i])
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .fontWeight(600)
                    .color(t.colors.primaryText)
                    .build();
                ui.text("memory.cfg.trig.tilet" + std::to_string(i))
                    .position(tx + core::theme::scaled(40.0f),
                              by + core::theme::scaled(6.0f))
                    .size(tw - core::theme::scaled(48.0f),
                          core::theme::scaled(18.0f))
                    .text(tiles[i])
                    .fontSize(core::theme::scaled(11.0f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .color(t.colors.textSecondary)
                    .build();
            }
            cy += bodyH + 8.0f;
        }
    }

    // ── 组 2：用量告警（摘要 = 日/月档实时值；月档软/硬 = 消耗带预算同数）──
    {
        auto wanOf = [](long long v) { return std::to_string(v / 10000); };
        groupHead(2, "用量告警",
                  "日 " + wanOf(snap.settings.usageDayAlertTokens) +
                      " 万 · 月软 " + wanOf(snap.settings.usageSoftBudgetTokens) +
                      " 万 / 硬 " + wanOf(snap.settings.usageHardBudgetTokens) + " 万",
                  false);
        if (cfgOpen_[2]) {
            const float bodyH = core::theme::scaled(240.0f);
            groupBody(2, bodyH);
            float by = cy + 16.0f;
            ui.text("memory.cfg.usage.hint")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(18.0f))
                .text("今日/本月 token 越线弹一条桌面提醒（每档每天最多一条）；月档软/硬即消耗带预算。")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textSecondary)
                .build();
            by += core::theme::scaled(26.0f);
            // 行绘制：标签 + 说明 + 滑条（量化步进）+ 右侧当前值（万）。
            // 去重在 PanelState::setUsageAlertThresholds（值未变 no-op），回调每 tick 直调。
            auto sliderRow = [&](const std::string& id, const std::string& label,
                                 const std::string& sub, long long cur,
                                 long long lo, long long step, long long hi,
                                 float rowY,
                                 std::function<void(long long)> apply) {
                ui.text(id + ".label")
                    .position(x + 16.0f, rowY)
                    .size(90.0f, core::theme::scaled(18.0f))
                    .text(label)
                    .fontSize(core::theme::scaled(12.5f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".sub")
                    .position(x + 96.0f, rowY)
                    .size(innerW - 220.0f, core::theme::scaled(18.0f))
                    .text(sub)
                    .fontSize(core::theme::scaled(11.0f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .color(t.colors.textTertiary)
                    .build();
                const auto clampN = [](double v) {
                    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
                };
                const double norm =
                    clampN(static_cast<double>(cur - lo) /
                           static_cast<double>(hi - lo));
                const double steps = static_cast<double>(hi - lo) /
                                     static_cast<double>(step);
                ui_kit::slider(ui, id, x + 16.0f,
                               rowY + core::theme::scaled(24.0f), innerW - 120.0f,
                               20.0f, static_cast<float>(norm), t,
                               [apply, lo, step, steps](float v) {
                                   const long long q =
                                       lo + step * static_cast<long long>(
                                                 v * steps + 0.5f);
                                   apply(q);
                               });
                char vbuf[24];
                std::snprintf(vbuf, sizeof(vbuf), "%lld 万", cur / 10000);
                ui.text(id + ".val")
                    .position(x + w - 92.0f, rowY + core::theme::scaled(22.0f))
                    .size(76.0f, core::theme::scaled(20.0f))
                    .text(vbuf)
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textPrimary)
                    .build();
            };
            sliderRow("memory.cfg.usage.day", "日档阈值",
                      "今日 token 越线提醒（默认 50 万）",
                      snap.settings.usageDayAlertTokens, 50000, 50000, 2000000,
                      by, [this](long long q) {
                          const auto s = panel_.snapshot().settings;
                          panel_.setUsageAlertThresholds(
                              q, s.usageSoftBudgetTokens, s.usageHardBudgetTokens);
                      });
            by += core::theme::scaled(52.0f);
            sliderRow("memory.cfg.usage.soft", "月档 · 软档",
                      "进黄档区提醒 = 消耗带预算软档",
                      snap.settings.usageSoftBudgetTokens, 500000, 100000, 5000000,
                      by, [this](long long q) {
                          const auto s = panel_.snapshot().settings;
                          panel_.setUsageAlertThresholds(
                              s.usageDayAlertTokens, q, s.usageHardBudgetTokens);
                      });
            by += core::theme::scaled(52.0f);
            sliderRow("memory.cfg.usage.hard", "月档 · 硬档",
                      "严重级提醒 = 消耗带预算硬档",
                      snap.settings.usageHardBudgetTokens, 1000000, 100000,
                      10000000, by, [this](long long q) {
                          const auto s = panel_.snapshot().settings;
                          panel_.setUsageAlertThresholds(
                              s.usageDayAlertTokens, s.usageSoftBudgetTokens, q);
                      });
            by += core::theme::scaled(52.0f);
            warnBar("memory.cfg.usage.warn", by,
                    "口径：token 为码点估算（非引擎真值）；判定面 = 记忆消耗账本。");
            cy += bodyH + 8.0f;
        }
    }

    // ── 安全与隐私（常显组 ~200px，need:181 §4.3）：脱敏开关 + 数据目录 +
    // 导出/清除；脱敏关闭态 warn 条常驻组内（安全关键不进折叠）──
    {
        const bool gateOn = snap.gateEnabled;
        const float bodyH =
            core::theme::scaled(gateOn ? 184.0f : 218.0f);
        groupBody(3, bodyH);
        float by = cy + 16.0f;
        // 脱敏行：switch + 状态词 + 一句说明；关闭走红色二次确认（现有 maskModal）
        ui_kit::wswitch(ui, "memory.cfg.mask.switch", snap.gateEnabled,
                        x + 16.0f, by, 44.0f, 24.0f, t,
                        [this](bool on) {
                            if (!on) {
                                maskModalOpen_ = true;  // 关闭须二次确认
                            } else {
                                panel_.setSanitizeEnabled(true);
                                showToast("脱敏已开启", "出机前自动清除敏感信息");
                            }
                        });
        ui.text("memory.cfg.mask.chip")
            .position(x + 72.0f, by + core::theme::scaled(2.0f))
            .size(140.0f, core::theme::scaled(20.0f))
            .text(gateOn ? "已开启" : "脱敏已关闭")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .fontWeight(600)
            .color(gateOn ? t.colors.primaryText : t.colors.errorText)
            .build();
        ui.text("memory.cfg.mask.note")
            .position(x + 16.0f, by + core::theme::scaled(30.0f))
            .size(innerW, core::theme::scaled(18.0f))
            .text(gateOn
                      ? "密钥、手机号等敏感信息会在出机前被自动清除。"
                      : "已关闭：密钥、手机号等敏感信息将原样出机，风险自担。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(18.0f))
            .color(t.colors.textSecondary)
            .build();
        by += core::theme::scaled(52.0f);
        if (!gateOn) {
            warnBar("memory.cfg.mask.warn", by,
                    "脱敏默认开启；关闭属高危操作需二次确认，关闭后内容将原样发送给云端。");
            by += core::theme::scaled(34.0f);
        }
        ui.rect("memory.cfg.sec.sep")
            .position(x + 16.0f, by)
            .size(innerW, 1.0f)
            .color(t.colors.borderMuted)
            .build();
        by += core::theme::scaled(13.0f);
        // 数据行：路径盒（28px active 底）
        ui.rect("memory.cfg.data.pathbox")
            .position(x + 16.0f, by)
            .size(innerW, 28.0f)
            .color(t.colors.active)
            .radius(t.sizes.radiusSmall)
            .build();
        ui.text("memory.cfg.data.path")
            .position(x + 26.0f, by + core::theme::scaled(4.0f))
            .size(innerW - 20.0f, core::theme::scaled(20.0f))
            .text(engine::memoryDataDir().generic_string() + "  ·  " +
                  formatBytes(snap.storageBytes))
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textSecondary)
            .build();
        by += core::theme::scaled(38.0f);
        ui_kit::button(ui, "memory.cfg.data.export", "导出全部记忆（JSON）",
                       ui_kit::ButtonLevel::Secondary, x + 16.0f, by,
                       220.0f, 40.0f, t,
                       {[this] {
                           auto r = panel_.exportAllToJson();
                           if (r.isFail()) {
                               showToast("导出未完成", r.errorMessage(), true);
                           } else {
                               showToast("已导出", r.value());
                           }
                       }});
        ui_kit::button(ui, "memory.cfg.data.clear", "清除全部…",
                       ui_kit::ButtonLevel::Danger, x + 252.0f, by,
                       150.0f, 40.0f, t,
                       {[this] {
                           clearInput_.clear();
                           clearModalOpen_ = true;
                       }});
        cy += bodyH + 8.0f;
    }

    ui.text("memory.cfg.foot")
        .position(x, cy + core::theme::scaled(4.0f))
        .size(w, core::theme::scaled(20.0f))
        .text("引擎侧记忆清空需引擎列举接口（A 阶段未暴露），清除全部只清本机面板库。")
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(core::theme::scaled(20.0f))
        .color(t.colors.textDisabled)
        .build();
}

// ── 弹窗层 ──

} // namespace module::memory
