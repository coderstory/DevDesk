// memory_module_panels.cpp
// 从 memory_module.cpp 拆出（2026-10-31）：记忆页主视图分区——动态流、主控台、
// 提案卡、已裁决卡、审核台、配置页。

#include "memory_module_internal.h"

namespace module::memory {

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
        cy += core::theme::scaled(26.0f);
    }

    composeEngineCard(ui, t, x, w, cy);
    // need:380 B-4：健康四格 / token 消耗带 / 运行明细折叠区（调用记录 + 最近动态）
    // 从主控台移除——观察面收敛为引擎卡（状态点 + 「详情」弹窗入口），
    // 「跑评测」并入引擎详情弹窗（memory_module_modals.cpp）。
}

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
    const float kMetaW = proposalMetaW(w);  // need298 M13：窄窗档 190→120（佐证先饿）
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
    if (p.source == "final_draft" && p.status == "pending") {
        // F-03 波 2 腿 A 兜底文案（OQ-2）：修饰链未达自动入库线（重试耗尽/
        // add 失败/低置信）——具体原因见动态流 note 与日志；人审 = 编辑后
        // 接受（addDirect 源文入库）或拒绝
        body = "终稿修饰未过自动入库线，待人审：";
        body += p.resultText;
    } else if (unrefinedAdd) {
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
        .size(decidedKindBoxW(w), core::theme::scaled(40.0f))
        .wrap()
        .text(truncateByAdvance(
            "「" + kindLabel(p.kind) + "」" +
                (p.resultText.empty() ? srcLabel(p.source) : p.resultText),
            2.0f * decidedKindBoxW(w) - 2.0f * core::theme::scaled(12.5f),
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
        cy += core::theme::scaled(26.0f);
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
                w += chipTextW(label, 30.0f) + 6.0f;  // M16：估宽单一来源
            }
            return w;
        };
        // 右端「批量处理」按钮保留区（104 + 估宽余量）；折行态 chips 独占第二行
        // 全宽，保留区不再参与竞争（M6）。常量单一来源 memory_module_internal.h
        const float chipsTotal = 12.0f + 120.0f + 10.0f + 120.0f + 14.0f +
                                 chipRowW(stateLabels);
        // need:181 §3.2：内容区 <860 时状态 chips 折第二行、高 88（设计自适应档）；
        // need298 M6：折行态「批量处理」恒第一行 y10 右锚，chips 第二行全宽
        // （原 chips y42 与按钮 y28 同带相触），保留区不参与第二行竞争。
        const bool wrapNarrow = chipsTotal > w - kBatchZone || w < 860.0f;
        const float kBarH = wrapNarrow ? 88.0f : 52.0f;
        const float stateRowY = batchToolbarStateRowY(wrapNarrow);
        ui_kit::cardPanel(ui, "memory.rv.toolbar", x, cy, w, kBarH, t.sizes.radius, t);
        auto chipRow = [&](const std::string& prefix, float cx, float cyRow,
                           const std::vector<std::string>& labels, int active,
                           std::function<void(int)> onPick) {
            for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
                // chip 宽按显示字符估（M16：chipTextW 单一来源，四字节双宽）
                const float chipW =
                    chipTextW(labels[static_cast<std::size_t>(i)], 24.0f);
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
        // 「批量处理」次级按钮（右缘锚；进出批量模式；M6：恒第一行 y10）
        ui_kit::button(ui, "memory.rv.batchbtn",
                       batchMode_ ? "退出批量" : "批量处理",
                       ui_kit::ButtonLevel::Secondary,
                       x + w - 16.0f - 104.0f, cy + 10.0f,
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
            // need298 M5：内容区 <740 两段式——说明独占第一行（超长省略），
            // 按钮行右锚第二行；原 w−480 在窄窗变负/极窄且与右锚钮叠印。
            const float barH = batchBarH(w);
            const float btnRowY = batchBtnRowY(w);
            const std::string infoText =
                "已选 " + std::to_string(batchSel_.size()) + " 项" +
                (batchSel_.empty()
                     ? " · 在卡片左侧勾选后可批量处理"
                     : (allOk ? " · 所选提案均支持批量接受"
                              : " · 批量接受仅对「遗忘 / 新增」开放"));
            ui.rect("memory.rv.batchbar")
                .position(x, cy)
                .size(w, barH)
                .color(t.colors.primarySoftBg)
                .radius(t.sizes.radius)
                .build();
            ui.text("memory.rv.batchinfo")
                .position(x + 14.0f, cy + core::theme::scaled(12.0f))
                .size(std::max(0.0f, batchInfoW(w)), core::theme::scaled(20.0f))
                .text(truncateByAdvance(infoText, batchInfoW(w),
                                        core::theme::scaled(12.5f)))
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(core::theme::scaled(20.0f))
                .color(t.colors.primaryText)
                .build();
            ui_kit::button(ui, "memory.rv.batchclear", "取消选择",
                           ui_kit::ButtonLevel::Secondary, x + w - 260.0f,
                           cy + btnRowY, 108.0f, 30.0f, t,
                           {[this] { batchSel_.clear(); }});
            if (allOk) {
                primaryButton(ui, "memory.rv.batchaccept", "批量接受",
                              x + w - 140.0f, cy + btnRowY, 124.0f, 30.0f, t,
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
                               x + w - 140.0f, cy + btnRowY, 124.0f, 30.0f, t, {});
            }
            cy += barH + 8.0f;  // need219 §4.3：卡间距 8px（批量条→列表）
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
                .position(x + 16.0f, cy + core::theme::scaled(6.0f))  // need298 M15：与右侧按钮同中心线（34 高钮 vs 22 高行）
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
            .states(t.colors.surface, t.colors.hover,
                    t.colors.hover)  // need298 M8：可点组头 hover 三态（#195 §4.4）
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
            .size(std::max(0.0f, groupSumW(w)), headH)
            .text(truncateByAdvance(sum, groupSumW(w),
                                    core::theme::scaled(12.0f)))  // need298 M11：超长省略，不越 chevron
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(headH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(sumWarn ? t.colors.warning : t.colors.textTertiary)
            .build();
        // need298 M14：FA chevron + 160ms OutCubic 旋转（同 memory.details.chev）
        ui.text("memory.cfg.grp" + gid + ".chev")
            .position(x + w - 40.0f, cy)
            .size(24.0f, headH)
            .icon(eui::utf8(0xF054u))  // fa-chevron-right，展开旋 90° 朝下
            .rotate(cfgOpen_[static_cast<std::size_t>(gi)] ? 1.5707963f : 0.0f)
            .transition(core::Transition::make(0.16f, core::Ease::OutCubic))
            .animate(core::AnimProperty::Transform)
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
            // need256：组 0 并入提炼 key 卡——体高随「已配置态展开输入行（修改）」
            // 派生两档；全部内容坐标经 scaled 累加（clue 134：禁定值偏移）
            const float bodyH = core::theme::scaled(
                (llmKeyEditing_ && !llmKeyMask_.empty()) ? 484.0f : 464.0f);
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
            // need256：提炼 API key 卡（app_secrets 通道）。两态——未配置=引导+输入行；
            // 已配置=掩码+修改/清除。密钥原文只存在于输入框瞬时值与数据库，
            // 模块态只存掩码（telemem_config.h 密钥红线）。
            ui.text("memory.cfg.key.subtitle")
                .position(x + 16.0f, by)
                .size(320.0f, core::theme::scaled(18.0f))
                .text("提炼 API key（本机数据库）")
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(core::theme::scaled(18.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            by += core::theme::scaled(24.0f);
            const auto ct = core::theme::componentTokensWeChat(t);
            if (llmKeyMask_.empty() || llmKeyEditing_) {
                ui.text("memory.cfg.key.hint")
                    .position(x + 16.0f, by)
                    .size(innerW, core::theme::scaled(18.0f))
                    .text(llmKeyMask_.empty()
                              ? "尚未配置——粘贴 API key，仅存本机数据库，不写配置与日志。"
                              : "粘贴新的 API key，保存后替换现有 key。")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .color(t.colors.textSecondary)
                    .build();
                by += core::theme::scaled(26.0f);
                components::input(ui, "memory.cfg.key.input")
                    .position(x + 16.0f, by)
                    .size(innerW - 104.0f, 40.0f)
                    .value(llmKeyInput_)
                    .placeholder("sk-…（智谱 API key）")
                    .fontSize(core::theme::scaled(13.0f))
                    .theme(ct)
                    .onChange([this](const std::string& v) { llmKeyInput_ = v; })
                    .onCancel([this] { llmKeyInput_.clear(); })
                    .build();
                primaryButton(ui, "memory.cfg.key.save", "保存",
                              x + 16.0f + innerW - 96.0f, by + 4.0f, 88.0f,
                              32.0f, t, {[this] { saveConfiguredLlmKey(); }});
                by += 52.0f;
                if (!llmKeyMask_.empty()) {
                    ui_kit::button(ui, "memory.cfg.key.canceledit", "取消修改",
                                   ui_kit::ButtonLevel::Text, x + 16.0f, by,
                                   92.0f, 28.0f, t,
                                   {[this] {
                                       llmKeyInput_.clear();
                                       llmKeyEditing_ = false;
                                   }});
                    by += core::theme::scaled(34.0f);
                }
            } else {
                ui.text("memory.cfg.key.mask")
                    .position(x + 16.0f, by)
                    .size(200.0f, core::theme::scaled(24.0f))
                    .text(llmKeyMask_)
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(core::theme::scaled(24.0f))
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui_kit::button(ui, "memory.cfg.key.edit", "修改",
                               ui_kit::ButtonLevel::Secondary, x + 224.0f, by,
                               68.0f, 28.0f, t,
                               {[this] {
                                   llmKeyInput_.clear();
                                   llmKeyEditing_ = true;
                               }});
                ui_kit::button(ui, "memory.cfg.key.clear", "清除",
                               ui_kit::ButtonLevel::Danger, x + 300.0f, by,
                               68.0f, 28.0f, t,
                               {[this] { clearConfiguredLlmKey(); }});
                by += core::theme::scaled(36.0f);
            }
            ui.text("memory.cfg.key.note")
                .position(x + 16.0f, by)
                .size(innerW, core::theme::scaled(16.0f))
                .text("key 仅存本机数据库，引擎启动时注入子进程环境；不出现在配置文件与日志。")
                .fontSize(core::theme::scaled(11.5f))
                .lineHeight(core::theme::scaled(16.0f))
                .color(t.colors.textTertiary)
                .build();
            by += core::theme::scaled(22.0f);
            ui.rect("memory.cfg.key.sep")
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
                    .text(truncateByAdvance(
                        tiles[i], tw - core::theme::scaled(48.0f),
                        core::theme::scaled(11.0f)))  // need298 M10：窄窗末字防裁
                    .fontSize(core::theme::scaled(11.0f))
                    .lineHeight(core::theme::scaled(18.0f))
                    .color(t.colors.textSecondary)
                    .build();
            }
            cy += bodyH + 8.0f;
        }
    }

    // ── 组 2：用量告警（need:380 B-5：4 滑条 → 单开关；阈值收敛为内部值——
    // 存量自定义值在 panel_settings.json 原键读入继续生效，默认取量程合理中位）──
    {
        groupHead(2, "用量告警",
                  snap.settings.usageAlertsEnabled ? "开" : "关", false);
        if (cfgOpen_[2]) {
            const float bodyH = core::theme::scaled(64.0f);
            groupBody(2, bodyH);
            float by = cy + 16.0f;
            ui_kit::wswitch(ui, "memory.cfg.usage.switch",
                            snap.settings.usageAlertsEnabled, x + 16.0f, by,
                            44.0f, 24.0f, t,
                            [this](bool on) {
                                panel_.setUsageAlertsEnabled(on);
                                showToast(on ? "用量告警已开启" : "用量告警已关闭",
                                          on ? "今日/本月 token 越线弹一条桌面提醒（每档每天最多一条）"
                                             : "阈值保留在内部，重新开启即恢复判定");
                            });
            ui.text("memory.cfg.usage.hint")
                .position(x + 76.0f, by)
                .size(innerW - 92.0f, core::theme::scaled(36.0f))
                .text("今日/本月 token 越线弹一条桌面提醒（每档每天最多一条）。\n"
                      "告警档位为内部默认值，不再单独调节。")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textSecondary)
                .build();
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
    // need:335：内容实占底 = foot 底（0 基）。scrollview 的 contentHeightProvider
    // 用它供滚动总高——EUI stack wrapContent 对绝对定位子元素测量偏小（实测
    // 1424 声明几何被解析为 1320，尾组 104px 不可达），弃整篇 scratch 测量。
    cfgContentBottom_ = (cy + core::theme::scaled(24.0f)) - oy;
}

// ── 弹窗层 ──

} // namespace module::memory
