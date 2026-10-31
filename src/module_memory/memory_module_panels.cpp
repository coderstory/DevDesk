// memory_module_panels.cpp
// 从 memory_module.cpp 拆出（2026-10-31）：记忆页主视图分区——动态流、主控台、
// 提案卡、已裁决卡、审核台、配置页。

#include "memory_module_internal.h"

namespace module::memory {

void MemoryModule::composeFeed(eui::Ui& ui, const core::theme::Tokens& t,
                               float x, float w, float y) {
    const auto snap = panel_.snapshot();
    constexpr float kCardH = 268.0f;
    ui.rect("memory.feed.card")
        .position(x, y)
        .size(w, kCardH)
        .color(ui_kit::cardSurface(t))
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.feed.title")
        .position(x + 16.0f, y + 12.0f)
        .size(300.0f, 22.0f)
        .text("最近动态")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(22.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.feed.sub")
        .position(x + 120.0f, y + 14.0f)
        .size(300.0f, 18.0f)
        .text("引擎替你做了什么，一目了然")
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(18.0f)
        .color(t.colors.textTertiary)
        .build();

    if (snap.feed.empty()) {
        ui.text("memory.feed.empty")
            .position(x + 16.0f, y + 48.0f)
            .size(w - 32.0f, 60.0f)
            .wrap()
            .text("还没有动态。引擎启动并产生采集 / 注入 / 提炼后，这里会滚动显示。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(20.0f)
            .color(t.colors.textTertiary)
            .build();
        return;
    }
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
    int i = 0;
    for (const auto& item : snap.feed) {
        const float ry = y + 44.0f + static_cast<float>(i) * 27.0f;
        ui.text("memory.feed.dot" + std::to_string(i))
            .position(x + 16.0f, ry)
            .size(14.0f, 24.0f)
            .text("●")
            .fontSize(core::theme::scaled(9.0f))
            .lineHeight(24.0f)
            .color(feedColor(item.kind))
            .build();
        ui.text("memory.feed.txt" + std::to_string(i))
            .position(x + 34.0f, ry)
            .size(w - 190.0f, 24.0f)
            .text(item.text)
            .fontSize(core::theme::scaled(12.5f))
            .lineHeight(24.0f)
            .color(t.colors.textSecondary)
            .build();
        ui.text("memory.feed.time" + std::to_string(i))
            .position(x + w - 150.0f, ry)
            .size(134.0f, 24.0f)
            .text(panel::formatFeedTime(item.ts))
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(24.0f)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textTertiary)
            .build();
        ++i;
    }
}

void MemoryModule::composeConsole(eui::Ui& ui, const core::theme::Tokens& t,
                                  float x, float w, float y, float viewH) {
    (void)viewH;
    const auto snap = panel_.snapshot();
    const float oy = y - consoleScroll_;  // 内容随滚动上移（settings 卡区同款）

    float cy = oy;
    const bool degraded = snap.engine.state == TelememEngine::State::Failed ||
                          snap.engine.state == TelememEngine::State::Stopped;
    if (degraded) {
        ui.rect("memory.degrade.banner")
            .position(x, cy)
            .size(w, 44.0f)
            .color(t.colors.active)
            .radius(t.sizes.radius)
            .build();
        ui.text("memory.degrade.text")
            .position(x + 14.0f, cy + 10.0f)
            .size(w - 220.0f, 24.0f)
            .text("引擎已停止——搜索与现有记忆仍可用，新增提炼暂停。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(24.0f)
            .fontWeight(600)
            .color(t.colors.warning)
            .build();
        primaryButton(ui, "memory.degrade.start", "启动引擎",
                      x + w - 180.0f, cy + 7.0f, 140.0f, 30.0f, t,
                      [] { TelememEngine::global().startAsync(); });
        cy += 44.0f + kCardGap;
    }

    // 面板库故障（可读错误，不留白）
    if (!snap.storesOk) {
        ui.text("memory.stores.error")
            .position(x, cy)
            .size(w, 22.0f)
            .text("面板数据通道异常：" + snap.storesError)
            .fontSize(core::theme::scaled(12.5f))
            .lineHeight(22.0f)
            .color(t.colors.errorText)
            .build();
        cy += 26.0f;
    }

    composeEngineCard(ui, t, x, w, cy);
    cy += 176.0f + kCardGap;
    composeHealthGrid(ui, t, x, w, cy);
    cy += 236.0f + kCardGap;

    // 消耗卡（今日/本月 token + 估算金额，ledger 聚合；月度预算软/硬档）
    {
        constexpr float kUsageH = 150.0f;
        ui.rect("memory.usage.card")
            .position(x, cy)
            .size(w, kUsageH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        ui.text("memory.usage.title")
            .position(x + 16.0f, cy + 12.0f)
            .size(320.0f, 22.0f)
            .text("token 消耗（今日 / 本月）")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(22.0f)
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
            .position(x + w - 240.0f, cy + 12.0f)
            .size(224.0f, 22.0f)
            .text(phase == engine::TokenUsageMeter::Phase::Hard
                      ? "已触发硬档 · 提炼暂停"
                      : phase == engine::TokenUsageMeter::Phase::Soft
                            ? "逼近预算 · 软档提醒"
                            : "预算充足")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(22.0f)
            .color(phase == engine::TokenUsageMeter::Phase::Normal
                       ? t.colors.textTertiary
                       : t.colors.warning)
            .build();
        const float barX = x + 16.0f;
        const float barW = w - 32.0f;
        ui.rect("memory.usage.bar")
            .position(barX, cy + 48.0f)
            .size(barW, 10.0f)
            .color(t.colors.active)
            .radius(t.sizes.radiusPill)
            .build();
        const float fillW = static_cast<float>(usage.monthRatio()) * barW;
        if (fillW > 1.0f) {
            ui.rect("memory.usage.fill")
                .position(barX, cy + 48.0f)
                .size(fillW, 10.0f)
                .color(phase == engine::TokenUsageMeter::Phase::Normal
                           ? t.colors.primary
                           : t.colors.warning)
                .radius(t.sizes.radiusPill)
                .build();
        }
        char numBuf[200];
        std::snprintf(
            numBuf, sizeof(numBuf),
            "今日 %lld tk · %s · 本月 %lld tk · %s（软档 %lld · 硬档 %lld）",
            snap.todayTokens, formatMilliYuan(snap.todayCostMilli).c_str(),
            snap.monthTokens, formatMilliYuan(snap.monthCostMilli).c_str(),
            usage.softBudgetTokens, usage.hardBudgetTokens);
        ui.text("memory.usage.nums")
            .position(barX, cy + 70.0f)
            .size(barW, 20.0f)
            .text(numBuf)
            .fontSize(core::theme::scaled(12.5f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        ui.text("memory.usage.note")
            .position(barX, cy + 96.0f)
            .size(barW, 40.0f)
            .wrap()
            .text("口径：码点数 ≈ CJK 1 字 1 token 的估算账面；单价按智谱计费页常量，"
                  "注入记账不计金额。")
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(16.0f)
            .color(t.colors.textTertiary)
            .build();
        cy += kUsageH + kCardGap;
    }

    // 最近调用记录表（消耗控制第 4 条：数字与内部账面一致——直接读 ledger 行）
    {
        constexpr float kRowH = 26.0f;
        const auto rows = snap.recentLedger;
        const float tableH = 40.0f + static_cast<float>(rows.size()) * kRowH + 14.0f;
        ui.rect("memory.recent.card")
            .position(x, cy)
            .size(w, tableH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        ui.text("memory.recent.title")
            .position(x + 16.0f, cy + 12.0f)
            .size(320.0f, 22.0f)
            .text("最近调用记录")
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(22.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        auto colHeader = [&](const char* text, float cx, float cw, bool right) {
            ui.text(std::string("memory.recent.h.") + text)
                .position(cx, cy + 40.0f)
                .size(cw, 18.0f)
                .text(text)
                .fontSize(core::theme::scaled(11.0f))
                .lineHeight(18.0f)
                .horizontalAlign(right ? eui::HorizontalAlign::Right
                                       : eui::HorizontalAlign::Left)
                .color(t.colors.textTertiary)
                .build();
        };
        const float cTime = x + 16.0f;
        const float cKindW = 160.0f;
        const float cKind = cTime + 140.0f;
        const float cCount = cKind + cKindW;
        const float cCountW = 120.0f;
        const float cTok = cCount + cCountW;
        const float cTokW = 120.0f;
        colHeader("时间", cTime, 130.0f, false);
        colHeader("类型", cKind, cKindW, false);
        colHeader("条数/字数", cCount, cCountW, true);
        colHeader("token · 金额", cTok, cTokW, true);
        auto kindDisplay = [](const std::string& kind) -> std::string {
            if (kind == "llm_input") return "提炼 · 输入";
            if (kind == "llm_output") return "提炼 · 产出";
            if (kind == "embed") return "向量化";
            if (kind == "inject_items") return "注入 · 条数";
            if (kind == "inject_chars") return "注入 · 字数";
            if (kind == "hook_search") return "hook 检索";
            return kind;
        };
        int i = 0;
        for (const auto& r : rows) {
            const float ry = cy + 62.0f + static_cast<float>(i) * kRowH;
            const std::string rid = std::to_string(i);
            ui.text("memory.recent.r" + rid + ".time")
                .position(cTime, ry)
                .size(130.0f, 20.0f)
                .text(panel::formatFeedTime(r.ts))
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(20.0f)
                .color(t.colors.textSecondary)
                .build();
            ui.text("memory.recent.r" + rid + ".kind")
                .position(cKind, ry)
                .size(cKindW, 20.0f)
                .text(kindDisplay(r.kind))
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(20.0f)
                .color(t.colors.textSecondary)
                .build();
            char countBuf[40];
            std::snprintf(countBuf, sizeof(countBuf), "%lld", r.tokens);
            ui.text("memory.recent.r" + rid + ".count")
                .position(cCount, ry)
                .size(cCountW, 20.0f)
                .text(countBuf)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textPrimary)
                .build();
            char tokBuf[64];
            std::snprintf(tokBuf, sizeof(tokBuf), "%lld · %s", r.tokens,
                          formatMilliYuan(r.costMilliYuan).c_str());
            ui.text("memory.recent.r" + rid + ".tok")
                .position(cTok, ry)
                .size(cTokW, 20.0f)
                .text(tokBuf)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textTertiary)
                .build();
            ++i;
        }
        if (rows.empty()) {
            ui.text("memory.recent.empty")
                .position(cTime, cy + 62.0f)
                .size(w - 32.0f, 20.0f)
                .text("还没有调用记录。")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(20.0f)
                .color(t.colors.textTertiary)
                .build();
        }
        cy += tableH + kCardGap;
    }

    composeFeed(ui, t, x, w, cy);
}

// ── 视图 2：记忆审核 ──

void MemoryModule::composeProposalCard(eui::Ui& ui,
                                       const core::theme::Tokens& t, float x,
                                       float w, float y,
                                       const panel::PanelSnapshot& snap,
                                       const Proposal& p, bool batchable) {
    (void)snap;
    constexpr float kCardH = 232.0f;
    ui.rect("memory.rv.card." + p.id)
        .position(x, y)
        .size(w, kCardH)
        .color(ui_kit::cardSurface(t))
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();

    // 批量勾选（仅批量模式；仅可批量类型）
    if (batchMode_) {
        const bool checked = batchSel_.count(p.id) > 0;
        ui.rect("memory.rv.pick." + p.id)
            .position(x + 12.0f, y + 14.0f)
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
                .position(x + 12.0f, y + 13.0f)
                .size(18.0f, 18.0f)
                .text("✓")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(18.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.onPrimary)
                .build();
        }
    }

    // 类型徽章 + 标题 + 置信度 + 来源
    const float hx = x + (batchMode_ ? 38.0f : 16.0f);
    const std::string kind = kindLabel(p.kind);
    const float kindW = 2.0f * 12.0f + 20.0f;
    ui.rect("memory.rv.kindchip." + p.id)
        .position(hx, y + 12.0f)
        .size(kindW, 22.0f)
        .color(t.colors.active)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text("memory.rv.kindtext." + p.id)
        .position(hx, y + 13.0f)
        .size(kindW, 20.0f)
        .text(kind)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(20.0f)
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(kindColor(t, p.kind))
        .build();

    std::string title = p.resultText;
    if (title.empty() && !p.evidence.empty()) title = p.evidence.front().quoted;
    if (title.size() > 32) title = title.substr(0, 32) + "…";
    if (title.empty()) title = "（无正文）";
    const std::string titleText =
        (p.kind == "add" ? "新增记忆：" + title
                         : (p.kind == "forget"
                                ? "遗忘过时记忆：" + title
                                : (p.kind == "invalidate"
                                       ? "标记过期记忆：" + title
                                       : "合并重复记忆：" + title)));
    ui.text("memory.rv.title." + p.id)
        .position(hx + kindW + 10.0f, y + 12.0f)
        .size(w - (hx - x) - kindW - 200.0f, 22.0f)
        .text(titleText)
        .fontSize(core::theme::scaled(13.5f))
        .lineHeight(22.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.rv.conf." + p.id)
        .position(x + w - 176.0f, y + 13.0f)
        .size(76.0f, 20.0f)
        .text("置信度 " + confidenceTier(p.confidence))
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(20.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    ui.text("memory.rv.src." + p.id)
        .position(x + w - 96.0f, y + 13.0f)
        .size(84.0f, 20.0f)
        .text("来自 " + srcLabel(p.source))
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(20.0f)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .color(t.colors.textTertiary)
        .build();

    // 正文（add/merge = 将生成/合并结果；forget/invalidate = 拟处理原文）
    std::string body;
    if (p.kind == "add") {
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
    ui.text("memory.rv.body." + p.id)
        .position(x + 16.0f, y + 42.0f)
        .size(w - 32.0f, 56.0f)
        .wrap()
        .text(body)
        .fontSize(core::theme::scaled(12.5f))
        .lineHeight(18.0f)
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
    const bool expanded = expanded_.count(p.id) > 0;
    ui.text("memory.rv.evtoggle." + p.id)
        .position(x + 16.0f, y + 106.0f)
        .size(w - 32.0f, 24.0f)
        .text(std::string(expanded ? "▾ " : "▸ ") + evText)
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(24.0f)
        .color(t.colors.info)
        .onClick([this, id = p.id, expanded] {
            if (expanded) {
                expanded_.erase(id);
            } else {
                expanded_.insert(id);
            }
        })
        .build();

    // 底部操作（拒绝 / 编辑后接受 / 接受）
    const float by = y + kCardH - 46.0f;
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
    ui.rect("memory.rv.done." + p.id)
        .position(x, y)
        .size(w, kCardH)
        .color(ui_kit::cardSurface(t))
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
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
        .position(x + 16.0f, y + 10.0f)
        .size(220.0f, 20.0f)
        .text(statusText)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(statusColor)
        .build();
    ui.text("memory.rv.done.kind." + p.id)
        .position(x + 16.0f, y + 34.0f)
        .size(w - 200.0f, 40.0f)
        .wrap()
        .text("「" + kindLabel(p.kind) + "」" +
              (p.resultText.empty() ? srcLabel(p.source) : p.resultText))
        .fontSize(core::theme::scaled(12.5f))
        .lineHeight(18.0f)
        .color(t.colors.textSecondary)
        .build();
    ui.text("memory.rv.done.time." + p.id)
        .position(x + w - 190.0f, y + 10.0f)
        .size(174.0f, 20.0f)
        .text(panel::formatFeedTime(p.decidedAt))
        .fontSize(core::theme::scaled(11.5f))
        .lineHeight(20.0f)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .color(t.colors.textTertiary)
        .build();
    if (p.status == "accepted" && !undone &&
        (p.kind == "add" || p.kind == "forget")) {
        ui_kit::button(ui, "memory.rv.undo." + p.id, "撤销",
                       ui_kit::ButtonLevel::Secondary, x + w - 100.0f,
                       y + 44.0f, 84.0f, 30.0f, t,
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
    const float oy = y - reviewScroll_;
    float cy = oy;

    ui.text("memory.rv.intro")
        .position(x, cy)
        .size(w, 22.0f)
        .text("Claude 整理记忆前先问你一句：合并、失效、遗忘、新增，都由你拍板，"
              "不满意随时撤销。")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(22.0f)
        .color(t.colors.textSecondary)
        .build();
    cy += 30.0f;

    // 工具条：类型 / 来源 / 状态 chips + 批量模式
    {
        // 窄窗换行：三组 chips 加间隔超过 (w-200) 会漫到右侧批量开关位，
        // 此时状态 chips 折到第二行、工具条加高；批量开关恒在第一行右缘。
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
        const float kBatchZone = 240.0f;  // 右侧「批量模式」标签+开关保留区（含估宽噪声余量）
        const std::vector<std::string> typeLabels = {"全部", "合并", "失效", "遗忘", "新增"};
        const std::vector<std::string> srcLabels = {"全部来源", "Claude 对话", "整理", "遗忘扫描"};
        const std::vector<std::string> stateLabels = {"待审", "已处理"};
        const float chipsTotal = 12.0f + chipRowW(typeLabels) + 14.0f + chipRowW(srcLabels) +
                                 14.0f + chipRowW(stateLabels);
        const bool wrapNarrow = chipsTotal > w - kBatchZone;
        const float kBarH = wrapNarrow ? 88.0f : 56.0f;
        const float stateRowY = wrapNarrow ? 42.0f : 10.0f;
        ui.rect("memory.rv.toolbar")
            .position(x, cy)
            .size(w, kBarH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .build();
        auto chipRow = [&](const std::string& prefix, float cx, float cyRow,
                           const std::vector<std::string>& labels, int active,
                           std::function<void(int)> onPick) {
            for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
                // chip 宽按显示字符估（全宽 12px / ASCII 6px + 24 内边距）：
                // 按字节数计会把 CJK 高估 3 倍，窄窗 chips 行溢出且压到批量开关
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
                    .size(chipW, 20.0f)
                    .text(labels[static_cast<std::size_t>(i)])
                    .fontSize(core::theme::scaled(11.5f))
                    .lineHeight(20.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(i == active ? t.colors.primaryText
                                       : t.colors.textSecondary)
                    .build();
                cx += chipW + 6.0f;
            }
            return cx;
        };
        float cx = x + 12.0f;
        cx = chipRow("memory.rv.chipT.", cx, cy + 10.0f,
                     typeLabels, filterType_,
                     [this](int v) { filterType_ = v; });
        cx += 14.0f;
        cx = chipRow("memory.rv.chipS.", cx, cy + 10.0f,
                     srcLabels, filterSrc_,
                     [this](int v) { filterSrc_ = v; });
        // 窄窗：状态 chips 折第二行；宽窗：延续第一行（原版接排）
        cx = wrapNarrow ? x + 12.0f : cx + 14.0f;
        cx = chipRow("memory.rv.chipSt.", cx, cy + stateRowY, stateLabels,
                     filterState_, [this](int v) { filterState_ = v; });
        // 批量模式开关（右侧；标签贴紧开关，窄窗不与 chips 行重叠）
        ui.text("memory.rv.batchlbl")
            .position(x + w - 180.0f, cy + 14.0f)
            .size(76.0f, 20.0f)
            .text("批量模式")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        ui_kit::wswitch(ui, "memory.rv.batchsw", batchMode_, x + w - 96.0f,
                        cy + 12.0f, 44.0f, 24.0f, t,
                        [this](bool on) {
                            batchMode_ = on;
                            batchSel_.clear();
                        });
        cy += kBarH + 10.0f;

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
                .position(x + 14.0f, cy + 12.0f)
                .size(w - 480.0f, 20.0f)
                .text("已选 " + std::to_string(batchSel_.size()) + " 项" +
                      (batchSel_.empty()
                           ? " · 在卡片左侧勾选后可批量处理"
                           : (allOk ? " · 所选提案均支持批量接受"
                                    : " · 批量接受仅对「遗忘 / 新增」开放")))
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(20.0f)
                .color(t.colors.primaryText)
                .build();
            ui_kit::button(ui, "memory.rv.batchclear", "取消选择",
                           ui_kit::ButtonLevel::Secondary, x + w - 260.0f,
                           cy + 7.0f, 108.0f, 30.0f, t,
                           {[this] { batchSel_.clear(); }});
            if (allOk) {
                primaryButton(ui, "memory.rv.batchaccept", "批量接受",
                              x + w - 140.0f, cy + 7.0f, 124.0f, 30.0f, t,
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
                               x + w - 140.0f, cy + 7.0f, 124.0f, 30.0f, t, {});
            }
            cy += 44.0f + 10.0f;
        }
    }

    // 卡片流（按过滤 + 状态）
    if (filterState_ == 0) {
        int shown = 0;
        for (const auto& p : snap.pending) {
            if (!kindMatches(filterType_, p.kind)) continue;
            if (!srcMatches(filterSrc_, p.source)) continue;
            composeProposalCard(ui, t, x, w, cy, snap, p,
                                batchableKind(p.kind));
            cy += 232.0f + 12.0f;
            ++shown;
        }
        if (shown == 0) {
            const bool totallyEmpty = snap.pending.empty();
            constexpr float kEmptyH = 150.0f;
            ui.rect("memory.rv.empty")
                .position(x, cy)
                .size(w, kEmptyH)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusOverlay)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.text("memory.rv.empty.title")
                .position(x, cy + 42.0f)
                .size(w, 28.0f)
                .text(totallyEmpty ? "都处理完啦！没有需要你拿主意的提案。"
                                   : "当前筛选下没有待审提案。")
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(28.0f)
                .fontWeight(700)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
            ui.text("memory.rv.empty.note")
                .position(x, cy + 84.0f)
                .size(w, 24.0f)
                .text("新提案会在 Claude 帮你整理记忆时自动出现在这里，不用盯着，不急。")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
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
            cy += 92.0f + 12.0f;
            ++idx;
        }
        if (idx == 0) {
            ui.text("memory.rv.done.empty")
                .position(x, cy + 12.0f)
                .size(w, 24.0f)
                .text("还没有处理记录。")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        }
    }
}

// ── 视图 3：配置 ──

void MemoryModule::composeConfig(eui::Ui& ui, const core::theme::Tokens& t,
                                 float x, float w, float y, float viewH) {
    (void)viewH;
    const auto snap = panel_.snapshot();
    const float oy = y - configScroll_;
    float cy = oy;
    const float innerW = w - 32.0f;

    auto cardHead = [&](const std::string& id, float cyCard,
                        const std::string& title) {
        ui.text(id)
            .position(x + 16.0f, cyCard + 12.0f)
            .size(400.0f, 24.0f)
            .text(title)
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(24.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
    };
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
            .size(innerW - 20.0f, 18.0f)
            .text(text)
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(18.0f)
            .color(t.colors.warning)
            .build();
    };

    // ── 大模型端点（从 DevDesk 网关 profile 一键导入；只读回显，无密钥）──
    {
        constexpr float kCardH = 176.0f;
        ui.rect("memory.cfg.llm")
            .position(x, cy)
            .size(w, kCardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        cardHead("memory.cfg.llm.title", cy, "大模型端点（提炼）");
        ui.text("memory.cfg.llm.hint")
            .position(x + 16.0f, cy + 38.0f)
            .size(innerW, 18.0f)
            .text("Claude 的「大脑」接在哪里——一般不用手动填，从 DevDesk 网关一键带入即可。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .color(t.colors.textSecondary)
            .build();
        const bool imported = snap.settings.llmImportedAtSec > 0;
        primaryButton(ui, "memory.cfg.llm.import",
                      "从 DevDesk 网关一键导入当前 profile",
                      x + 16.0f, cy + 62.0f,
                      320.0f, ui_kit::kButtonHeight, t,
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
                .position(x + 352.0f, cy + 72.0f)
                .size(120.0f, 22.0f)
                .text("已导入")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(22.0f)
                .fontWeight(600)
                .color(t.colors.success)
                .build();
        }
        // 只读回显（端点定案值 + 凭据通道；不回显任何密钥形态）
        ui.text("memory.cfg.llm.rows")
            .position(x + 16.0f, cy + 112.0f)
            .size(innerW, 36.0f)
            .wrap()
            .text(std::string("端点 ") + engine::kLlmBaseUrl + " · 模型 " +
                  engine::kLlmModel +
                  " · 凭据 Claude 设置（网关托管，运行时注入引擎，不写配置与日志）" +
                  (imported ? " · 来源 profile " + snap.settings.llmProfileName
                            : " · 尚未导入（点击上方按钮绑定）"))
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(16.0f)
            .color(t.colors.textTertiary)
            .build();
        warnBar("memory.cfg.llm.warn", cy + 140.0f,
                snap.gateEnabled ? "对话内容将发送至该端点（已过脱敏）"
                                 : "对话内容将发送至该端点（当前未经脱敏）");
        cy += kCardH + kCardGap;
    }

    // ── 向量化端点（本机 llama-server；零 API 费口径）──
    {
        constexpr float kCardH = 122.0f;
        ui.rect("memory.cfg.embed")
            .position(x, cy)
            .size(w, kCardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        cardHead("memory.cfg.embed.title", cy, "向量化端点（本机）");
        ui.text("memory.cfg.embed.rows")
            .position(x + 16.0f, cy + 42.0f)
            .size(innerW, 36.0f)
            .wrap()
            .text(std::string("llama-server · http://127.0.0.1:") +
                  std::to_string(engine::kEmbedServerPort) + " · 模型 " +
                  engine::kEmbedModel + " · " +
                  std::to_string(engine::kEmbedDims) + " 维（换档 = 全量重嵌）")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(17.0f)
            .color(t.colors.textSecondary)
            .build();
        warnBar("memory.cfg.embed.warn", cy + 84.0f,
                "记忆原文不出机：向量化全部落本机（零 API 费）。");
        cy += kCardH + kCardGap;
    }

    // ── 脱敏设置（真持久化 + 关闭走红色二次确认，设计稿逐字文案）──
    {
        constexpr float kCardH = 128.0f;
        ui.rect("memory.cfg.mask")
            .position(x, cy)
            .size(w, kCardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        cardHead("memory.cfg.mask.title", cy, "脱敏设置");
        ui.text("memory.cfg.mask.chip")
            .position(x + 120.0f, cy + 14.0f)
            .size(140.0f, 20.0f)
            .text(snap.gateEnabled ? "已开启" : "脱敏已关闭")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(snap.gateEnabled ? t.colors.primaryText : t.colors.errorText)
            .build();
        ui_kit::wswitch(ui, "memory.cfg.mask.switch", snap.gateEnabled,
                        x + 16.0f, cy + 44.0f, 44.0f, 24.0f, t,
                        [this](bool on) {
                            if (!on) {
                                maskModalOpen_ = true;  // 关闭须二次确认
                            } else {
                                panel_.setSanitizeEnabled(true);
                                showToast("脱敏已开启", "出机前自动清除敏感信息");
                            }
                        });
        ui.text("memory.cfg.mask.note")
            .position(x + 76.0f, cy + 46.0f)
            .size(innerW - 76.0f, 20.0f)
            .text(snap.gateEnabled
                      ? "密钥、手机号等敏感信息会在出机前被自动清除。"
                      : "已关闭：密钥、手机号等敏感信息将原样出机，风险自担。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        ui.text("memory.cfg.mask.rule")
            .position(x + 16.0f, cy + 84.0f)
            .size(innerW, 20.0f)
            .text("脱敏默认开启；关闭属高危操作需二次确认，关闭后内容将原样发送给云端。")
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(20.0f)
            .color(t.colors.textTertiary)
            .build();
        cy += kCardH + kCardGap;
    }

    // ── 触发线设置（阈值滑条持久化 + 基线磁贴）──
    {
        constexpr float kCardH = 218.0f;
        ui.rect("memory.cfg.trig")
            .position(x, cy)
            .size(w, kCardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        cardHead("memory.cfg.trig.title", cy, "触发线设置");
        ui.text("memory.cfg.trig.hint")
            .position(x + 16.0f, cy + 38.0f)
            .size(innerW, 18.0f)
            .text("决定「什么时候提醒你升级」和「什么样的词值得记」，默认值已经调好，可微调。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .color(t.colors.textSecondary)
            .build();
        // 滑条 1：升级阈值（60~95）
        {
            const int pct = snap.settings.recallThresholdPct;
            ui.text("memory.cfg.trig.s1.label")
                .position(x + 16.0f, cy + 66.0f)
                .size(90.0f, 18.0f)
                .text("升级阈值")
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(18.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text("memory.cfg.trig.s1.sub")
                .position(x + 96.0f, cy + 66.0f)
                .size(innerW - 96.0f, 18.0f)
                .text("Recall@5 低于此值时提醒（红线 85%）")
                .fontSize(core::theme::scaled(11.0f))
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();
            ui_kit::slider(ui, "memory.cfg.trig.s1", x + 16.0f, cy + 92.0f,
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
                .position(x + w - 92.0f, cy + 90.0f)
                .size(76.0f, 20.0f)
                .text(vbuf)
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textPrimary)
                .build();
        }
        // 滑条 2：两字词基线（2~10）
        {
            const int base = snap.settings.twoWordBaseline;
            ui.text("memory.cfg.trig.s2.label")
                .position(x + 16.0f, cy + 122.0f)
                .size(220.0f, 18.0f)
                .text("两字词提炼基线 · 命中次数")
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(18.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui_kit::slider(ui, "memory.cfg.trig.s2", x + 16.0f, cy + 148.0f,
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
                .position(x + w - 92.0f, cy + 146.0f)
                .size(76.0f, 20.0f)
                .text(std::to_string(base) + " 次")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textPrimary)
                .build();
        }
        // 基线磁贴（5 / 3 / 11，定案 7 自研基线）
        const char* tiles[3] = {"命中 5 次 → 候选", "复现 3 轮 → 确认",
                                "11 天窗口内有效"};
        const char* nums[3] = {"5", "3", "11"};
        for (int i = 0; i < 3; ++i) {
            const float tw = (innerW - 2.0f * 12.0f) / 3.0f;
            const float tx = x + 16.0f + static_cast<float>(i) * (tw + 12.0f);
            ui.rect("memory.cfg.trig.tile" + std::to_string(i))
                .position(tx, cy + 176.0f)
                .size(tw, 30.0f)
                .color(t.colors.active)
                .radius(t.sizes.radiusSmall)
                .build();
            ui.text("memory.cfg.trig.tilen" + std::to_string(i))
                .position(tx + 10.0f, cy + 182.0f)
                .size(20.0f, 18.0f)
                .text(nums[i])
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(18.0f)
                .fontWeight(700)
                .color(t.colors.primaryText)
                .build();
            ui.text("memory.cfg.trig.tilet" + std::to_string(i))
                .position(tx + 36.0f, cy + 182.0f)
                .size(tw - 44.0f, 18.0f)
                .text(tiles[i])
                .fontSize(core::theme::scaled(11.0f))
                .lineHeight(18.0f)
                .color(t.colors.textSecondary)
                .build();
        }
        cy += kCardH + kCardGap;
    }

    // ── 用量告警（需-9）：日/月两档阈值可调；月档软/硬 = 消耗卡预算同一套数 ──
    {
        constexpr float kCardH = 258.0f;
        ui.rect("memory.cfg.usage")
            .position(x, cy)
            .size(w, kCardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        cardHead("memory.cfg.usage.title", cy, "用量告警阈值（token 估算）");
        ui.text("memory.cfg.usage.hint")
            .position(x + 16.0f, cy + 38.0f)
            .size(innerW, 18.0f)
            .text("今日/本月 token 越线弹一条桌面提醒（每档每天最多一条）；月档软/硬即消耗卡预算。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .color(t.colors.textSecondary)
            .build();
        // 行绘制：标签 + 说明 + 滑条（量化步进）+ 右侧当前值（万）。
        // 去重在 PanelState::setUsageAlertThresholds（值未变 no-op），回调每 tick 直调。
        auto sliderRow = [&](const std::string& id, const std::string& label,
                             const std::string& sub, long long cur,
                             long long lo, long long step, long long hi,
                             float rowY,
                             std::function<void(long long)> apply) {
            ui.text(id + ".label")
                .position(x + 16.0f, rowY)
                .size(90.0f, 18.0f)
                .text(label)
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(18.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(id + ".sub")
                .position(x + 96.0f, rowY)
                .size(innerW - 220.0f, 18.0f)
                .text(sub)
                .fontSize(core::theme::scaled(11.0f))
                .lineHeight(18.0f)
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
            ui_kit::slider(ui, id, x + 16.0f, rowY + 24.0f, innerW - 120.0f,
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
                .position(x + w - 92.0f, rowY + 22.0f)
                .size(76.0f, 20.0f)
                .text(vbuf)
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textPrimary)
                .build();
        };
        sliderRow("memory.cfg.usage.day", "日档阈值",
                  "今日 token 越线提醒（默认 50 万）",
                  snap.settings.usageDayAlertTokens, 50000, 50000, 2000000,
                  cy + 62.0f, [this](long long q) {
                      const auto s = panel_.snapshot().settings;
                      panel_.setUsageAlertThresholds(
                          q, s.usageSoftBudgetTokens, s.usageHardBudgetTokens);
                  });
        sliderRow("memory.cfg.usage.soft", "月档 · 软档",
                  "进黄档区提醒 = 消耗卡预算软档",
                  snap.settings.usageSoftBudgetTokens, 500000, 100000, 5000000,
                  cy + 114.0f, [this](long long q) {
                      const auto s = panel_.snapshot().settings;
                      panel_.setUsageAlertThresholds(
                          s.usageDayAlertTokens, q, s.usageHardBudgetTokens);
                  });
        sliderRow("memory.cfg.usage.hard", "月档 · 硬档",
                  "严重级提醒 = 消耗卡预算硬档",
                  snap.settings.usageHardBudgetTokens, 1000000, 100000,
                  10000000, cy + 166.0f, [this](long long q) {
                      const auto s = panel_.snapshot().settings;
                      panel_.setUsageAlertThresholds(
                          s.usageDayAlertTokens, s.usageSoftBudgetTokens, q);
                  });
        warnBar("memory.cfg.usage.warn", cy + 216.0f,
                "口径：token 为码点估算（非引擎真值）；判定面 = 记忆消耗账本。");
        cy += kCardH + kCardGap;
    }

    // ── 数据与隐私（目录显示 + 导出 + 清除全部）──
    {
        constexpr float kCardH = 156.0f;
        ui.rect("memory.cfg.data")
            .position(x, cy)
            .size(w, kCardH)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
        cardHead("memory.cfg.data.title", cy, "数据与隐私");
        ui.text("memory.cfg.data.hint")
            .position(x + 16.0f, cy + 38.0f)
            .size(innerW, 18.0f)
            .text("你的记忆都在自己电脑的这个文件夹里，随时打包带走，或一键清空重来。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .color(t.colors.textSecondary)
            .build();
        ui.rect("memory.cfg.data.pathbox")
            .position(x + 16.0f, cy + 62.0f)
            .size(innerW, 28.0f)
            .color(t.colors.active)
            .radius(t.sizes.radiusSmall)
            .build();
        ui.text("memory.cfg.data.path")
            .position(x + 26.0f, cy + 66.0f)
            .size(innerW - 20.0f, 20.0f)
            .text(engine::memoryDataDir().generic_string() + "  ·  " +
                  formatBytes(snap.storageBytes))
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        ui_kit::button(ui, "memory.cfg.data.export", "导出全部记忆（JSON）",
                       ui_kit::ButtonLevel::Secondary, x + 16.0f, cy + 102.0f,
                       220.0f, ui_kit::kButtonHeight, t,
                       {[this] {
                           auto r = panel_.exportAllToJson();
                           if (r.isFail()) {
                               showToast("导出未完成", r.errorMessage(), true);
                           } else {
                               showToast("已导出", r.value());
                           }
                       }});
        ui_kit::button(ui, "memory.cfg.data.clear", "清除全部…",
                       ui_kit::ButtonLevel::Danger, x + 252.0f, cy + 102.0f,
                       150.0f, ui_kit::kButtonHeight, t,
                       {[this] {
                           clearInput_.clear();
                           clearModalOpen_ = true;
                       }});
        cy += kCardH + kCardGap;
    }

    ui.text("memory.cfg.foot")
        .position(x, cy + 4.0f)
        .size(w, 20.0f)
        .text("引擎侧记忆清空需引擎列举接口（A 阶段未暴露），清除全部只清本机面板库。")
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(20.0f)
        .color(t.colors.textDisabled)
        .build();
}

// ── 弹窗层 ──

} // namespace module::memory
