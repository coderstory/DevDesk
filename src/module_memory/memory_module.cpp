// memory_module.cpp
// 记忆页模块装配与主控台：生命周期、compose 骨架、引擎卡、健康格。
// 三拆（2026-10-31）：本文件 = 装配 + 主视图；记忆审核/配置 → memory_module_panels.cpp；
// 弹窗 → memory_module_modals.cpp。视觉与交互契约 = docs/mockups/memcpp-panel-demo.html，
// 偏差处（预算环→双横条、来源枚举对齐数据层）在控件旁注明口径。

#include "memory_module_internal.h"

#include "core/tool.h"
#include "module_memory/agent_tools.h"
#include "module_memory/engine/telemem_config.h"

namespace module::memory {

::core::PageId MemoryModule::pageId() const { return ::core::PageId::Memory; }

std::string MemoryModule::displayName() const { return "记忆"; }

std::string MemoryModule::shortName() const { return "记忆"; }

// fa-brain (U+F5DC) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。

std::string MemoryModule::iconGlyph() const { return "\xEF\x97\x9C"; }

void MemoryModule::pollUsageAlerts() { panel_.pollUsageAlerts(); }

void MemoryModule::onStart() {
    LOG_INFO("记忆模块已启用：引擎未启动时页面显引导态，零后台成本");
    // memory.stats / memory.health：只读诊断工具，注册跟随模块启用（need 219
    // 拍板 3）。路径与 panel_.openStores 同源（memoryDataDir），不依赖页面开过。
    if (!agentToolsRegistered_) {
        memStatsTool_ = makeMemoryStatsTool(
            engine::memoryDataDir() / "proposals.db",
            engine::memoryDataDir() / "usage_ledger.db");
        memHealthTool_ = makeMemoryHealthTool();
        auto regStats =
            ::core::ToolRegistry::instance().registerTool(memStatsTool_);
        auto regHealth =
            ::core::ToolRegistry::instance().registerTool(memHealthTool_);
        if (regStats.isFail()) {
            LOG_WARN("记忆: memory.stats 注册失败: " + regStats.errorMessage());
        }
        if (regHealth.isFail()) {
            LOG_WARN("记忆: memory.health 注册失败: " + regHealth.errorMessage());
        }
        agentToolsRegistered_ = regStats.isOk() && regHealth.isOk();
    }
    // F1 产品接线：开页即同步采集循环——引擎已 Running 则立即起跑
    //（compose 每帧 refresh 也会同步；此处覆盖"启动引擎后未再切页"的首帧）
    panel_.refresh(true);
}

void MemoryModule::onStop() {
    // 工具随模块停用注销（先注销再放实例，避免在途调用悬垂）
    if (agentToolsRegistered_) {
        ::core::ToolRegistry::instance().unregisterTool("memory.stats");
        ::core::ToolRegistry::instance().unregisterTool("memory.health");
        agentToolsRegistered_ = false;
    }
    memStatsTool_.reset();
    memHealthTool_.reset();
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
        .fontWeight(600)  // need219 列表统一规范 §2.1：页头两档制 600，无 700
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

    // 三视图各套滚动容器（卡区总高可超小窗可视高；settings 卡区同款）。
    // bug158/160（2026-11-03）：滚动根必须 .position(pagePad, bodyY)——不设
    // position 时根从页顶 (0,0) 铺满，scrollState 置 interactive 使整面可命中、
    // 后建兄弟先命中，盖住 TAB 行吞点击；内容高测量也含 bodyY 偏移，滚动条
    // 轨道从页顶起步、比例失真。内容子元素相应改 0 基相对坐标，滚动位移全交
    // EUI scrollContentFrom 自动平移（内容里再手动减 offset = 双重平移）。
    const char* scrollIds[kViewConfig + 1] = {"memory.console.scroll",
                                              "memory.review.scroll",
                                              "memory.config.scroll"};
    const int view = activeView_;
    auto drawView = [&](eui::Ui& inner, float cw, float vh) {
        // 画布栈：滚动内容列的子元素会被列布局重排，绝对定位必须收进一层 stack
        inner.stack("memory.view.canvas")
            .width(cw)
            .height(core::SizeValue::wrapContent())
            .content([&] {
                if (view == kViewConsole) {
                    composeConsole(inner, t, 0.0f, cw, 0.0f, vh);
                } else if (view == kViewReview) {
                    composeReview(inner, t, 0.0f, cw, 0.0f, vh);
                } else {
                    composeConfig(inner, t, 0.0f, cw, 0.0f, vh);
                }
            })
            .build();
    };
    float* offset = view == kViewConsole
                        ? &consoleScroll_
                        : (view == kViewReview ? &reviewScroll_ : &configScroll_);
    // 滚动根定位于 TAB 行下方（页头/TAB 不滚；调度页同款），内容 0 基相对坐标
    components::scrollView(ui, scrollIds[view])
        .position(pagePad, bodyY)
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
    composeEngineDetailModal(ui, screen, t, ct);
    composeFullTextDialog(ui, screen, t, ct);
}

// ── 视图 1：主控台 ──

void MemoryModule::composeEngineCard(eui::Ui& ui, const core::theme::Tokens& t,
                                     float x, float w, float y) {
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    const eui::Color stateColor = engineStateColor(t, es.state);

    // need:181 三 TAB 分层（设计 §2.2）：引擎卡 196 → 64px 状态带。低频内容
    // （说明文案 / python 检测 / 语义档 / 版本号）移入「详情」弹窗（L3）；
    // 异常态前置警示短语到带上（L1 可见，细节进弹窗）。
    const float bandH = core::theme::scaled(64.0f);
    ui.stack("memory.eng.root")
        .position(x, y)
        .size(w, bandH)
        .content([&] {
    ui_kit::cardPanel(ui, "memory.eng.card", 0.0f, 0.0f, w, bandH, t.sizes.radiusLarge, t);
    ui.rect("memory.eng.dot")
        .position(t.sizes.spacingLarge, (bandH - 10.0f) * 0.5f)
        .size(10.0f, 10.0f)
        .color(stateColor)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text("memory.eng.title")
        .position(t.sizes.spacingLarge + 18.0f, 0.0f)
        .size(170.0f, bandH)
        .text("telemem 记忆引擎")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(bandH)
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.eng.state")
        .position(212.0f, 0.0f)
        .size(90.0f, bandH)
        .text(engineStateText(es.state))
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(bandH)
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(stateColor)
        .build();
    // 异常前置：Failed / 本机模型缺失时警示短语直接上带（细节进详情弹窗）
    const bool failed = es.state == TelememEngine::State::Failed;
    if (failed || es.llamaMissing) {
        ui.text("memory.eng.warn")
            .position(306.0f, 0.0f)
            .size(320.0f, bandH)
            .text(failed ? "引擎故障 · 点「详情」看原因" : "本机模型文件缺失")
            .fontSize(core::theme::scaled(11.0f))
            .lineHeight(bandH)
            .fontWeight(600)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning)
            .build();
    }

    // 右端按钮组（右锚 gap 8，高 36 垂直居中）：详情常驻；启停随状态；
    // 向导仅未安装引导态（Idle + 未检出 Python）直出
    const bool busy = es.state == TelememEngine::State::Preparing;
    const bool running = es.state == TelememEngine::State::Running;
    const bool wizardInline =
        es.state == TelememEngine::State::Idle && !es.pythonFound;
    const float btnH = 36.0f;
    const float btnY = (bandH - btnH) * 0.5f;
    float rightX = w - 16.0f - 120.0f;  // 主操作（启停）右缘对齐
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
                       ui_kit::ButtonLevel::Danger, x + rightX, y + btnY,
                       120.0f, btnH, t, {toggle});
    } else {
        primaryButton(ui, "memory.eng.toggle", busy ? "启动中…" : "启动引擎",
                      x + rightX, y + btnY, 120.0f, btnH, t,
                      busy ? nullptr : toggle);
    }
    if (wizardInline) {
        rightX -= 8.0f + 150.0f;
        ui_kit::button(ui, "memory.eng.wizard", "首次安装向导",
                       ui_kit::ButtonLevel::Secondary, x + rightX, y + btnY,
                       150.0f, btnH, t,
                       {[this] {
                           wizardOpen_ = true;
                           // 向导③步即时检查（need:202 F-04）：本地文件探测一次，
                           // idle 态下刷新 embed* 快照字段（管线态不受影响）
                           TelememEngine::global().refreshEmbedProbe();
                       }});
    }
    rightX -= 8.0f + 64.0f;
    ui_kit::button(ui, "memory.eng.detail", "详情",
                   ui_kit::ButtonLevel::Text, x + rightX, y + btnY, 64.0f,
                   btnH, t, {[this] { engineDetailOpen_ = true; }});
    });
}

void MemoryModule::composeHealthGrid(eui::Ui& ui,
                                     const core::theme::Tokens& t, float x,
                                     float w, float y) {
    const auto snap = panel_.snapshot();
    const float gap = kCardGap;
    const float cardW = (w - 3.0f * gap) / 4.0f;
    // need:181 三 TAB 分层（设计 §2.3）：健康四格 236 → 132px。格内节奏 =
    // 标签 y10 → 数值 y30 → 状态行 y72 → 12px 横带胶囊 y98；口径/说明类
    // note 全部降 L2 hover 浮层（components::tooltip，挂整卡透明命中）。
    const float kCardH = core::theme::scaled(132.0f);

    auto cardBase = [&](int i) {
        const float cx = x + static_cast<float>(i) * (cardW + gap);
        ui_kit::cardPanel(ui, "memory.health.card" + std::to_string(i), cx, y, cardW, kCardH, t.sizes.radiusOverlay, t);
        return cx;
    };
    auto label = [&](const std::string& id, float cx, const char* text) {
        ui.text(id)
            .position(cx + 14.0f, y + core::theme::scaled(10.0f))
            .size(cardW - 28.0f, core::theme::scaled(18.0f))
            .text(text)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(18.0f))
            .fontWeight(600)
            .color(t.colors.textSecondary)
            .build();
    };
    auto statusRow = [&](const std::string& id, float cx, const std::string& text,
                         eui::Color color) {
        ui.text(id)
            .position(cx + 14.0f, y + core::theme::scaled(72.0f))
            .size(cardW - 28.0f, core::theme::scaled(20.0f))
            .text(text)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .fontWeight(600)
            .color(color)
            .build();
    };
    // 12px 横带胶囊（禁发丝条，卡片基准 R6②）：底 active + 语义色填充
    auto miniBand = [&](const std::string& id, float cx, float ratio,
                        eui::Color fill) {
        const float bandX = cx + 14.0f;
        const float bandW = cardW - 28.0f;
        ui.rect(id)
            .position(bandX, y + core::theme::scaled(98.0f))
            .size(bandW, core::theme::scaled(12.0f))
            .color(t.colors.active)
            .radius(t.sizes.radiusPill)
            .build();
        const float fw = bandW * std::clamp(ratio, 0.0f, 1.0f);
        if (fw > 1.0f) {
            ui.rect(id + ".fill")
                .position(bandX, y + core::theme::scaled(98.0f))
                .size(fw, core::theme::scaled(12.0f))
                .color(fill)
                .radius(t.sizes.radiusPill)
                .build();
        }
    };
    // L2 hover 浮层：透明命中区 = 整格（source 挂卡面 id），长句出口
    auto tip = [&](int i, const std::string& text, float cx) {
        components::tooltip(ui, "memory.health.h" + std::to_string(i) + ".tip")
            .theme(core::theme::componentTokensWeChat(t))
            .source("memory.health.card" + std::to_string(i))
            .value(text)
            .anchor(cx + cardW * 0.5f, y + kCardH)
            .bounds(4096.0f, 4096.0f)
            .minWidth(240.0f)
            .maxWidth(cardW * 1.4f < 240.0f ? 240.0f : cardW * 1.4f)
            .build();
    };

    // ① 脱敏闸（红队 0/110 口径来自 S3 红队夹具）
    {
        const float cx = cardBase(0);
        label("memory.health.h0.label", cx, "脱敏闸");
        const bool on = snap.gateEnabled;
        ui.text("memory.health.h0.value")
            .position(cx + 14.0f, y + core::theme::scaled(30.0f))
            .size(cardW - 28.0f, core::theme::scaled(36.0f))
            .text("0/110")
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(core::theme::scaled(36.0f))
            .fontWeight(600)
            .color(on ? t.colors.success : t.colors.textDisabled)
            .build();
        statusRow("memory.health.h0.chiptext", cx,
                  on ? "强制开启 · 已锁定" : "未启用（已二次确认）",
                  on ? t.colors.primaryText : t.colors.warning);
        miniBand("memory.health.h0.band", cx, on ? 1.0f : 0.0f,
                 on ? t.colors.success : t.colors.warning);
        tip(0, on ? "110 条攻击样本全部被拦下；旁路会记审计日志（只含计数）。"
                  : "已关闭：内容将原样出机，旁路已记入审计（只含计数，无原文）。",
            cx);
    }

    // ② 触发线（评测结果驱动；阈值刻度仪表 + 黄标；「跑评测」按钮留格内）
    {
        const float cx = cardBase(1);
        label("memory.health.h1.label", cx, "触发线 · 记得牢不牢");
        const int pct = snap.lastEval.recallPct;
        const int threshold = snap.settings.recallThresholdPct;
        const bool hasEval = pct >= 0;
        const bool below = hasEval && pct < threshold;
        ui.text("memory.health.h1.value")
            .position(cx + 14.0f, y + core::theme::scaled(30.0f))
            .size(cardW - 28.0f, core::theme::scaled(36.0f))
            .text(hasEval ? std::to_string(pct) : "—")
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(core::theme::scaled(36.0f))
            .fontWeight(600)
            .color(hasEval ? (below ? t.colors.warning : t.colors.success)
                           : t.colors.textDisabled)
            .build();
        // 迷你仪表：12px 横带 + 阈值刻度（3px 豁免档，卡片基准 R6）
        const float bandX = cx + 14.0f;
        const float bandW = cardW - 28.0f;
        const float bandY = y + core::theme::scaled(98.0f);
        ui.rect("memory.health.h1.gauge")
            .position(bandX, bandY)
            .size(bandW, core::theme::scaled(12.0f))
            .color(t.colors.active)
            .radius(t.sizes.radiusPill)
            .build();
        const float fillW =
            hasEval ? bandW * static_cast<float>(
                                  std::min(100, std::max(0, pct))) / 100.0f
                    : 0.0f;
        if (fillW > 1.0f) {
            ui.rect("memory.health.h1.fill")
                .position(bandX, bandY)
                .size(fillW, core::theme::scaled(12.0f))
                .color(below ? t.colors.warning : t.colors.success)
                .radius(t.sizes.radiusPill)
                .build();
        }
        const float tickX =
            bandX + bandW * static_cast<float>(threshold) / 100.0f;
        ui.rect("memory.health.h1.tick")
            .position(tickX - 1.5f, bandY - core::theme::scaled(2.0f))
            .size(3.0f, core::theme::scaled(16.0f))
            .color(t.colors.errorText)
            .build();
        statusRow("memory.health.h1.chip", cx,
                  !hasEval
                      ? "未评测 · 跑一轮出数"
                      : (below ? ("当前 " + std::to_string(pct) + "% · 低于升级线")
                               : ("当前 " + std::to_string(pct) + "% · 达标")),
                  !hasEval ? t.colors.textTertiary
                           : (below ? t.colors.warning : t.colors.success));
        // 「跑评测」96×28 移至状态行右侧；引擎非 Running 或评测中禁用（IMP-114-5）
        const bool running = panel_.evalRunning();
        const bool clickable = !running && snap.engine.state ==
                                                   TelememEngine::State::Running;
        ui_kit::ButtonOptions runOpts;
        if (clickable) {
            runOpts.onClick = [this] {
                auto r = panel_.startEvalAsync();
                if (r.isFail()) {
                    showToast("评测未启动", r.errorMessage(), true);
                } else {
                    showToast("评测已启动",
                              "冻结 15 查询转测引擎检索（本机 embedding，零 API 费）");
                }
            };
        } else {
            runOpts.hasTextColorOverride = true;
            runOpts.textColorOverride = t.colors.textDisabled;
        }
        ui_kit::button(ui, "memory.health.h1.run", running ? "评测中…" : "跑评测",
                       ui_kit::ButtonLevel::Secondary,
                       x + cardW - 14.0f - 96.0f, y + core::theme::scaled(66.0f),
                       96.0f, 28.0f, t, runOpts);
        std::string h1tip =
            "冻结 15 查询转测检索，Recall@5 低于阈值提醒升级；本机 embedding 零 API 费。";
        if (!snap.lastEvalError.empty()) h1tip = "上次评测失败：" + snap.lastEvalError;
        else if (snap.lastEval.cleanupFailures > 0)
            h1tip = "评测清理缺口 " + std::to_string(snap.lastEval.cleanupFailures) +
                    " 条：残留可能进注入通道，重跑评测可覆盖清理";
        tip(1, h1tip, cx);
    }

    // ③ 注入预算（单横带 = 条数占比；字符账进状态行，说明 → hover）
    {
        const float cx = cardBase(2);
        label("memory.health.h2.label", cx, "注入预算 · 今日");
        const long long items = snap.injectItems;
        const long long chars = snap.injectChars;
        ui.text("memory.health.h2.value")
            .position(cx + 14.0f, y + core::theme::scaled(30.0f))
            .size(cardW - 28.0f, core::theme::scaled(36.0f))
            .text(std::to_string(items) + "/5 条")
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(core::theme::scaled(36.0f))
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        statusRow("memory.health.h2.chars", cx,
                  std::to_string(chars) + "/500 字", t.colors.textSecondary);
        miniBand("memory.health.h2.baritems", cx,
                 static_cast<float>(std::min(items, 5LL)) / 5.0f,
                 items >= 5 ? t.colors.warning : t.colors.primary);
        tip(2, "每次对话最多带 5 条、合计 500 字记忆给 Claude，防止喧宾夺主。",
            cx);
    }

    // ④ 存储概览（数值+状态行常显；提案/体积明细 → hover）
    {
        const float cx = cardBase(3);
        label("memory.health.h3.label", cx, "存储概览");
        ui.text("memory.health.h3.value")
            .position(cx + 14.0f, y + core::theme::scaled(30.0f))
            .size(cardW - 28.0f, core::theme::scaled(36.0f))
            .text(formatInt(snap.collectorEvents))
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(core::theme::scaled(36.0f))
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        statusRow("memory.health.h3.unit", cx,
                  formatBytes(snap.storageBytes) + " · " +
                      std::to_string(snap.collectorSessions) + " 个会话",
                  t.colors.textSecondary);
        miniBand("memory.health.h3.band", cx, 0.0f, t.colors.primary);
        tip(3, "提案 " + std::to_string(snap.pending.size()) + " 待审 / " +
                   std::to_string(snap.decided.size()) +
                   " 已处理 · 记忆都存在你自己的电脑里，配置页可随时导出。",
            cx);
    }
}

} // namespace module::memory

// 默认开启的普通页面模块：工厂恒非空（无平台能力门槛）。
extern "C" ::core::Module* create_memory_module() {
    return new module::memory::MemoryModule();
}
