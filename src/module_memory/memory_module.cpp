// memory_module.cpp
// 记忆页模块装配与主控台：生命周期、compose 骨架、引擎卡、健康格。
// 三拆（2026-10-31）：本文件 = 装配 + 主视图；记忆审核/配置 → memory_module_panels.cpp；
// 弹窗 → memory_module_modals.cpp。视觉与交互契约 = docs/mockups/memcpp-panel-demo.html，
// 偏差处（预算环→双横条、来源枚举对齐数据层）在控件旁注明口径。

#include "memory_module_internal.h"

namespace module::memory {

::core::PageId MemoryModule::pageId() const { return ::core::PageId::Memory; }

std::string MemoryModule::displayName() const { return "记忆"; }

std::string MemoryModule::shortName() const { return "记忆"; }

// fa-brain (U+F5DC) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。

std::string MemoryModule::iconGlyph() const { return "\xEF\x97\x9C"; }

void MemoryModule::pollUsageAlerts() { panel_.pollUsageAlerts(); }

void MemoryModule::onStart() {
    LOG_INFO("记忆模块已启用：引擎未启动时页面显引导态，零后台成本");
    // F1 产品接线：开页即同步采集循环——引擎已 Running 则立即起跑
    //（compose 每帧 refresh 也会同步；此处覆盖"启动引擎后未再切页"的首帧）
    panel_.refresh(true);
}

void MemoryModule::onStop() {
    // 页面关闭 ≠ 引擎停止：引擎是应用级常驻服务，生命周期归 TelememEngine。
    LOG_INFO("记忆页已关闭（引擎不受影响）");
}

void MemoryModule::showToast(const std::string& title, const std::string& body,
                             bool error) {
    toast_.show(title, body, error);
}

void MemoryModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const auto ct = core::theme::componentTokensWeChat(t);
    const float pagePad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pagePad * 2.0f);

    panel_.refresh();

    ui.text("memory.title")
        .position(pagePad, 0.0f)
        .size(contentW, kTitleH)
        .text("记忆")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(kTitleH)
        .fontWeight(700)
        .color(t.colors.textPrimary)
        .build();

    const float tabY = kTitleH + t.sizes.spacingLarge;
    const float tabW = 132.0f;
    const auto snap = panel_.snapshot();
    char reviewLabel[48];
    std::snprintf(reviewLabel, sizeof(reviewLabel), "记忆审核 (%d)",
                  static_cast<int>(snap.pending.size()));
    const char* tabLabels[kViewConfig + 1] = {"主控台", reviewLabel, "配置"};
    for (int i = kViewConsole; i <= kViewConfig; ++i) {
        const int view = i;
        ui_kit::wtab(ui, "memory.tabs." + std::to_string(i), tabLabels[i],
                     activeView_ == i,
                     pagePad + static_cast<float>(i) * (tabW + 4.0f), tabY,
                     tabW, kTabH, t, [this, view] { activeView_ = view; });
    }

    const float bodyY = tabY + kTabH + t.sizes.spacingLarge;

    // 三视图各套滚动容器（卡区总高可超小窗可视高；settings 卡区同款）
    const char* scrollIds[kViewConfig + 1] = {"memory.console.scroll",
                                              "memory.review.scroll",
                                              "memory.config.scroll"};
    const int view = activeView_;
    auto drawView = [&](eui::Ui& inner, float cw, float vh) {
        // 画布栈：滚动内容列的子元素会被列布局重排，绝对定位必须收进一层
        // stack（settings 卡区同款：卡根 stack 带页绝对坐标）
        inner.stack("memory.view.canvas")
            .width(cw)
            .height(core::SizeValue::wrapContent())
            .content([&] {
                if (view == kViewConsole) {
                    composeConsole(inner, t, pagePad, cw, bodyY, vh);
                } else if (view == kViewReview) {
                    composeReview(inner, t, pagePad, cw, bodyY, vh);
                } else {
                    composeConfig(inner, t, pagePad, cw, bodyY, vh);
                }
            })
            .build();
    };
    float* offset = view == kViewConsole
                        ? &consoleScroll_
                        : (view == kViewReview ? &reviewScroll_ : &configScroll_);
    // 滚动根铺满窗（settings 卡区同款：内容子元素自带页绝对坐标，
    // 滚动根不设 position——EUI 滚动状态坐标以 (0,0) 为基准）
    components::scrollView(ui, scrollIds[view])
        .size(contentW, std::max(120.0f, screen.height - bodyY - 12.0f))
        .step(60.0f)
        .theme(ct)
        .contentKey("memory.view" + std::to_string(view))
        .offset(*offset)
        .onChange([offset](float v) { *offset = v; })
        .content(drawView)
        .build();

    ui_kit::drawToast(ui, screen, "memory.toast", toast_, ct);

    // 弹窗层（最后画；open 由各自状态位控制）
    composeEditDialog(ui, screen, t, ct);
    composeMaskModal(ui, screen, t, ct);
    composeClearModal(ui, screen, t, ct);
    composeWizardModal(ui, screen, t, ct);
}

// ── 视图 1：主控台 ──

void MemoryModule::composeEngineCard(eui::Ui& ui, const core::theme::Tokens& t,
                                     float x, float w, float y) {
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    const eui::Color stateColor = engineStateColor(t, es.state);

    ui.stack("memory.eng.root")
        .position(x, y)
        .size(w, 176.0f)
        .content([&] {
    ui.rect("memory.eng.card")
        .position(0.0f, 0.0f)
        .size(w, 176.0f)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.rect("memory.eng.dot")
        .position(t.sizes.spacingLarge, 22.0f)
        .size(10.0f, 10.0f)
        .color(es.state == TelememEngine::State::Running ? t.colors.success
                                                         : t.colors.textDisabled)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text("memory.eng.title")
        .position( t.sizes.spacingLarge + 18.0f, 12.0f)
        .size(300.0f, 28.0f)
        .text("telemem 记忆引擎")
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(28.0f)
        .fontWeight(700)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.eng.version")
        .position( 320.0f, 16.0f)
        .size(90.0f, 20.0f)
        .text("v1.10.1")
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(20.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    ui.text("memory.eng.state")
        .position( 416.0f, 14.0f)
        .size(120.0f, 24.0f)
        .text(engineStateText(es.state))
        .fontSize(core::theme::scaled(13.0f))
        .lineHeight(24.0f)
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(stateColor)
        .build();
    ui.text("memory.eng.detail")
        .position( t.sizes.spacingLarge, 46.0f)
        .size(w - 2.0f * t.sizes.spacingLarge, 40.0f)
        .wrap()
        .text(es.detail.empty()
                  ? std::string("它替 Claude 记住你的偏好和项目背景，聊天时自动带上，"
                                "不用每次重复交代。")
                  : es.detail)
        .fontSize(core::theme::scaled(13.0f))
        .lineHeight(19.0f)
        .color(es.state == TelememEngine::State::Failed ? t.colors.errorText
                                                        : t.colors.textSecondary)
        .build();
    const std::string pythonLine =
        es.pythonFound
            ? "Python " + es.pythonVersion + " 已检测（telemem 要求 3.10~3.12）"
            : (es.llamaMissing
                   ? "本机模型文件缺失——embedding 档不可用，先跑首次安装向导"
                   : "未检测到 Python 3.10~3.12——点「首次安装向导」引导安装");
    ui.text("memory.eng.python")
        .position( t.sizes.spacingLarge, 92.0f)
        .size(w - 2.0f * t.sizes.spacingLarge, 20.0f)
        .text(pythonLine)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(20.0f)
        .color(t.colors.textTertiary)
        .build();

    const bool busy = es.state == TelememEngine::State::Preparing;
    const bool running = es.state == TelememEngine::State::Running;
    ui_kit::button(ui, "memory.eng.wizard", "首次安装向导",
                   ui_kit::ButtonLevel::Secondary, x + t.sizes.spacingLarge,
                   122.0f, 158.0f, ui_kit::kButtonHeight, t,
                   {[this] { wizardOpen_ = true; }});
    std::function<void()> toggle;
    if (!busy) {
        if (running) {
            toggle = [] { TelememEngine::global().stopAsync(); };
        } else {
            toggle = [] { TelememEngine::global().startAsync(); };
        }
    }
    if (running) {
        ui_kit::button(ui, "memory.eng.toggle", "停止引擎",
                       ui_kit::ButtonLevel::Danger,
                       x + t.sizes.spacingLarge + 174.0f, 122.0f, 158.0f,
                       ui_kit::kButtonHeight, t, {toggle});
    } else {
        primaryButton(ui, "memory.eng.toggle",
                      busy ? "启动中…" : "启动引擎",
                      x + t.sizes.spacingLarge + 174.0f, 122.0f, 158.0f,
                      ui_kit::kButtonHeight, t, toggle);
    }
    });
}

void MemoryModule::composeHealthGrid(eui::Ui& ui,
                                     const core::theme::Tokens& t, float x,
                                     float w, float y) {
    const auto snap = panel_.snapshot();
    const float gap = kCardGap;
    const float cardW = (w - 3.0f * gap) / 4.0f;
    constexpr float kCardH = 236.0f;

    auto cardBase = [&](int i) {
        const float cx = x + static_cast<float>(i) * (cardW + gap);
        ui.rect("memory.health.card" + std::to_string(i))
            .position(cx, y)
            .size(cardW, kCardH)
            .color(t.colors.surface)
            .radius(t.sizes.radiusOverlay)
            .border(1.0f, t.colors.borderDefault)
            .build();
        return cx;
    };
    auto label = [&](const std::string& id, float cx, const char* text) {
        ui.text(id)
            .position(cx + 14.0f, y + 12.0f)
            .size(cardW - 28.0f, 18.0f)
            .text(text)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .fontWeight(600)
            .color(t.colors.textSecondary)
            .build();
    };
    auto note = [&](const std::string& id, float cx, float ny,
                    const std::string& text) {
        ui.text(id)
            .position(cx + 14.0f, y + ny)
            .size(cardW - 28.0f, 46.0f)
            .wrap()
            .text(text)
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(16.0f)
            .color(t.colors.textTertiary)
            .build();
    };

    // ① 脱敏闸（真开关态 + 旁路审计提示；红队 0/110 口径来自 S3 红队夹具）
    {
        const float cx = cardBase(0);
        label("memory.health.h0.label", cx, "脱敏闸");
        const bool on = snap.gateEnabled;
        ui.rect("memory.health.h0.chip")
            .position(cx + 14.0f, y + 36.0f)
            .size(cardW - 28.0f, 24.0f)
            .color(on ? t.colors.primarySoftBg : t.colors.active)
            .radius(t.sizes.radiusPill)
            .build();
        ui.text("memory.health.h0.chiptext")
            .position(cx + 14.0f, y + 38.0f)
            .size(cardW - 28.0f, 20.0f)
            .text(on ? "强制开启 · 已锁定" : "未启用（已二次确认）")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .fontWeight(600)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(on ? t.colors.primaryText : t.colors.warning)
            .build();
        ui.text("memory.health.h0.body")
            .position(cx + 14.0f, y + 68.0f)
            .size(cardW - 28.0f, 34.0f)
            .wrap()
            .text("记忆出机前自动清除密钥/手机号等敏感信息。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(17.0f)
            .color(t.colors.textSecondary)
            .build();
        ui.text("memory.health.h0.redteam")
            .position(cx + 14.0f, y + 112.0f)
            .size(cardW - 28.0f, 22.0f)
            .text(on ? "红队测试 泄漏 0/110 样本" : "验证资产仍在，当前未启用")
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(22.0f)
            .fontWeight(600)
            .color(on ? t.colors.success : t.colors.textTertiary)
            .build();
        note("memory.health.h0.note", cx, 140.0f,
             on ? "110 条攻击样本全部被拦下；旁路会记审计日志（只含计数）。"
                : "已关闭：内容将原样出机，旁路已记入审计（只含计数，无原文）。");
    }

    // ② 触发线（评测结果驱动；阈值刻度仪表 + 黄标；评测零 API 费）
    {
        const float cx = cardBase(1);
        label("memory.health.h1.label", cx, "触发线 · 记得牢不牢");
        const int pct = snap.lastEval.recallPct;
        const int threshold = snap.settings.recallThresholdPct;
        const bool hasEval = pct >= 0;
        const bool below = hasEval && pct < threshold;
        ui.text("memory.health.h1.value")
            .position(cx + 14.0f, y + 34.0f)
            .size(cardW - 28.0f, 40.0f)
            .text(hasEval ? std::to_string(pct) : "—")
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(40.0f)
            .fontWeight(700)
            .color(hasEval ? (below ? t.colors.warning : t.colors.success)
                           : t.colors.textDisabled)
            .build();
        const float barX = cx + 14.0f;
        const float barW = cardW - 28.0f;
        ui.rect("memory.health.h1.gauge")
            .position(barX, y + 82.0f)
            .size(barW, 8.0f)
            .color(t.colors.active)
            .radius(t.sizes.radiusPill)
            .build();
        const float fillW =
            hasEval ? barW * static_cast<float>(
                                  std::min(100, std::max(0, pct))) / 100.0f
                    : 0.0f;
        if (fillW > 1.0f) {
            ui.rect("memory.health.h1.fill")
                .position(barX, y + 82.0f)
                .size(fillW, 8.0f)
                .color(below ? t.colors.warning : t.colors.success)
                .radius(t.sizes.radiusPill)
                .build();
        }
        const float tickX =
            barX + barW * static_cast<float>(threshold) / 100.0f;
        ui.rect("memory.health.h1.tick")
            .position(tickX - 1.0f, y + 78.0f)
            .size(2.0f, 16.0f)
            .color(t.colors.errorText)
            .build();
        ui.text("memory.health.h1.scale")
            .position(barX, y + 96.0f)
            .size(barW, 16.0f)
            .text("0                        升级线 " +
                  std::to_string(threshold) + "%")
            .fontSize(core::theme::scaled(10.5f))
            .lineHeight(16.0f)
            .color(t.colors.textTertiary)
            .build();
        ui.text("memory.health.h1.chip")
            .position(barX, y + 116.0f)
            .size(barW, 20.0f)
            .text(!hasEval
                      ? "未评测 · 跑一轮出数"
                      : (below ? ("当前 " + std::to_string(pct) + "% · 低于升级线")
                               : ("当前 " + std::to_string(pct) + "% · 达标")))
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(!hasEval ? t.colors.textTertiary
                            : (below ? t.colors.warning : t.colors.success))
            .build();
        const bool running = panel_.evalRunning();
        ui_kit::button(ui, "memory.health.h1.run", running ? "评测中…" : "跑评测",
                       ui_kit::ButtonLevel::Secondary, barX, y + 142.0f, 96.0f,
                       30.0f, t,
                       {[this, running] {
                           if (running) return;
                           auto r = panel_.startEvalAsync();
                           if (r.isFail()) {
                               showToast("评测未启动", r.errorMessage(), true);
                           } else {
                               showToast("评测已启动",
                                         "冻结 15 查询转测引擎检索（本机 embedding，零 API 费）");
                           }
                       }});
        // F5：评测失败与清理缺口必须有 UI 出口（不再只进日志）
        note("memory.health.h1.note", cx, 180.0f,
             !snap.lastEvalError.empty()
                 ? "上次评测失败：" + snap.lastEvalError
                 : (snap.lastEval.cleanupFailures > 0
                        ? ("评测清理缺口 " +
                           std::to_string(snap.lastEval.cleanupFailures) +
                           " 条：残留可能进注入通道，重跑评测可覆盖清理")
                        : "冻结 15 查询转测检索，Recall@5 低于阈值提醒升级；"
                          "本机 embedding 零 API 费。"));
    }

    // ③ 注入预算（ledger 注入账：今日累计 vs 单次预算 ≤5 条 ≤500 字；
    // 设计稿的环在 EUI 无弧形基元，以双横条同口径呈现）
    {
        const float cx = cardBase(2);
        label("memory.health.h2.label", cx, "注入预算 · 今日");
        const long long items = snap.injectItems;
        const long long chars = snap.injectChars;
        ui.text("memory.health.h2.value")
            .position(cx + 14.0f, y + 34.0f)
            .size(cardW - 28.0f, 40.0f)
            .text(std::to_string(items) + "/5 条")
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(40.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();
        const float barX = cx + 14.0f;
        const float barW = cardW - 28.0f;
        auto budgetBar = [&](const std::string& id, float by, long long used,
                             long long cap) {
            ui.rect(id)
                .position(barX, by)
                .size(barW, 6.0f)
                .color(t.colors.active)
                .radius(t.sizes.radiusPill)
                .build();
            const float fw =
                cap > 0 ? barW * static_cast<float>(std::min(used, cap)) /
                              static_cast<float>(cap)
                        : 0.0f;
            if (fw > 1.0f) {
                ui.rect(id + ".fill")
                    .position(barX, by)
                    .size(fw, 6.0f)
                    .color(used >= cap ? t.colors.warning : t.colors.primary)
                    .radius(t.sizes.radiusPill)
                    .build();
            }
        };
        budgetBar("memory.health.h2.baritems", y + 86.0f, items, 5);
        budgetBar("memory.health.h2.barchars", y + 112.0f, chars, 500);
        ui.text("memory.health.h2.chars")
            .position(barX, y + 124.0f)
            .size(barW, 18.0f)
            .text(std::to_string(chars) + "/500 字")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .color(t.colors.textSecondary)
            .build();
        note("memory.health.h2.note", cx, 152.0f,
             "每次对话最多带 5 条、合计 500 字记忆给 Claude，防止喧宾夺主。");
    }

    // ④ 存储概览（collector 水位 + proposals 计数 + 目录体积；
    // 引擎侧记忆数无列举缝，不可查——如实不显示该行）
    {
        const float cx = cardBase(3);
        label("memory.health.h3.label", cx, "存储概览");
        ui.text("memory.health.h3.value")
            .position(cx + 14.0f, y + 34.0f)
            .size(cardW - 28.0f, 40.0f)
            .text(formatInt(snap.collectorEvents))
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(40.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();
        ui.text("memory.health.h3.unit")
            .position(cx + 14.0f, y + 70.0f)
            .size(cardW - 28.0f, 18.0f)
            .text("条已采集事件（collector 水位）")
            .fontSize(core::theme::scaled(11.5f))
            .lineHeight(18.0f)
            .color(t.colors.textTertiary)
            .build();
        auto kv = [&](const std::string& id, float ky, const std::string& k,
                      const std::string& v) {
            ui.text(id + ".k")
                .position(cx + 14.0f, y + ky)
                .size(110.0f, 18.0f)
                .text(k)
                .fontSize(core::theme::scaled(11.5f))
                .lineHeight(18.0f)
                .color(t.colors.textTertiary)
                .build();
            ui.text(id + ".v")
                .position(cx + 128.0f, y + ky)
                .size(cardW - 142.0f, 18.0f)
                .text(v)
                .fontSize(core::theme::scaled(11.5f))
                .lineHeight(18.0f)
                .color(t.colors.textSecondary)
                .build();
        };
        kv("memory.health.h3.size", 96.0f, "占用体积",
           formatBytes(snap.storageBytes));
        kv("memory.health.h3.prop", 116.0f, "提案",
           std::to_string(snap.pending.size()) + " 待审 / " +
               std::to_string(snap.decided.size()) + " 已处理");
        kv("memory.health.h3.sess", 136.0f, "采集会话",
           std::to_string(snap.collectorSessions) + " 个");
        note("memory.health.h3.note", cx, 162.0f,
             "记忆都存在你自己的电脑里，配置页可随时导出。");
    }
}

} // namespace module::memory

// 默认开启的普通页面模块：工厂恒非空（无平台能力门槛）。
extern "C" ::core::Module* create_memory_module() {
    return new module::memory::MemoryModule();
}
