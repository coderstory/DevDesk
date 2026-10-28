// src/module_memory/memory_module.cpp
// 记忆页三视图（主控台 / 记忆审核 / 配置）。数据全部经 panel::PanelState
//（真账面：ledger / proposal store / collector 账本 / 评测库 / 设置 JSON），
// 引擎启停/健康接 TelememEngine。视觉与交互契约 = docs/mockups/memcpp-panel-demo.html；
// 偏差处（预算环→双横条、来源枚举对齐数据层）在控件旁注明口径。
#include "memory_module.h"
#include <utility>

#include <components/input.h>
#include <components/scrollview.h>
#include <eui/dsl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/component_theme.h"
#include "core/llm_profile_source.h"
#include "core/logging.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "engine/telemem_config.h"
#include "engine/telemem_engine.h"
#include "engine/token_usage.h"
#include "module_memory/proposals/usage_ledger.h"
#include "ui_kit/button.h"
#include "ui_kit/dialog_form.h"
#include "ui_kit/metrics.h"
#include "ui_kit/slider.h"
#include "ui_kit/wtab.h"
#include "ui_kit/wswitch.h"

namespace module::memory {

namespace {

using engine::TelememEngine;
using proposals::Proposal;

constexpr float kTitleH = 32.0f;
constexpr float kTabH = 36.0f;
constexpr float kCardGap = 16.0f;

constexpr int kViewConsole = 0;
constexpr int kViewReview = 1;
constexpr int kViewConfig = 2;

// ── 文案/取值映射 ──

const char* engineStateText(TelememEngine::State s) {
    switch (s) {
        case TelememEngine::State::Idle: return "未启动";
        case TelememEngine::State::Preparing: return "准备中";
        case TelememEngine::State::Running: return "运行中";
        case TelememEngine::State::Stopped: return "已停止";
        case TelememEngine::State::Failed: return "故障";
    }
    return "?";
}

eui::Color engineStateColor(const core::theme::Tokens& t,
                            TelememEngine::State s) {
    switch (s) {
        case TelememEngine::State::Running: return t.colors.success;
        case TelememEngine::State::Preparing: return t.colors.warning;
        case TelememEngine::State::Failed: return t.colors.errorText;
        default: return t.colors.textTertiary;
    }
}

std::string kindLabel(const std::string& kind) {
    if (kind == "add") return "新增";
    if (kind == "forget") return "遗忘";
    if (kind == "invalidate") return "失效";
    if (kind == "merge") return "合并";
    return kind;
}

eui::Color kindColor(const core::theme::Tokens& t, const std::string& kind) {
    if (kind == "add") return t.colors.success;
    if (kind == "forget") return t.colors.textTertiary;
    if (kind == "invalidate") return t.colors.warning;
    if (kind == "merge") return t.colors.info;
    return t.colors.textSecondary;
}

std::string confidenceTier(double c) {
    if (c >= 0.8) return "高";
    if (c >= 0.5) return "中";
    return "低";
}

std::string srcLabel(const std::string& source) {
    if (source == "collect") return "Claude 对话";
    if (source == "decay") return "遗忘扫描";
    if (source == "dedupe" || source == "conflict") return "整理";
    return source;
}

// 来源 chip 过滤键（数据层 source 枚举对齐口径）
bool srcMatches(int filter, const std::string& source) {
    switch (filter) {
        case 0: return true;
        case 1: return source == "collect";
        case 2: return source == "dedupe" || source == "conflict";
        case 3: return source == "decay";
        default: return true;
    }
}

bool kindMatches(int filter, const std::string& kind) {
    switch (filter) {
        case 0: return true;
        case 1: return kind == "merge";
        case 2: return kind == "invalidate";
        case 3: return kind == "forget";
        case 4: return kind == "add";
        default: return true;
    }
}

// 批量接受仅对「遗忘 / 新增」开放（设计稿契约；合并/失效须逐张处理）
bool batchableKind(const std::string& kind) {
    return kind == "forget" || kind == "add";
}

std::string formatInt(long long n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", n);
    std::string s = buf;
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

std::string formatBytes(std::int64_t bytes) {
    if (bytes < 0) return "统计中…";
    char buf[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    }
    return buf;
}

std::string formatMilliYuan(long long milli) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%03lld 元", milli / 1000,
                  milli % 1000);
    return buf;
}

// Primary 按钮在滚动画布内的文字补丁：EUI 渲染端对「stack 内 states 矩形 +
// 白字」的组合会把文字盖回底色下（定位实验见 2026-10-28 探针； Secondary /
// Danger 不触发）。文字改为画布层后置兄弟节点，保证浮于底色之上；渲染端
// 修复后应删掉本包装恢复直调 ui_kit::button。
void primaryButton(eui::Ui& ui, const std::string& id, const std::string& label,
                   float x, float y, float w, float h,
                   const core::theme::Tokens& t, std::function<void()> onClick) {
    ui_kit::button(ui, id, "", ui_kit::ButtonLevel::Primary, x, y, w, h, t,
                   {std::move(onClick)});
    ui.text(id + ".label")
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(core::theme::scaled(ui_kit::kButtonFontSize))
        .lineHeight(h)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.onPrimary)
        .build();
}

}  // namespace

::core::PageId MemoryModule::pageId() const { return ::core::PageId::Memory; }

std::string MemoryModule::displayName() const { return "记忆"; }

std::string MemoryModule::shortName() const { return "记忆"; }

// fa-brain (U+F5DC) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string MemoryModule::iconGlyph() const { return "\xEF\x97\x9C"; }

void MemoryModule::onStart() {
    LOG_INFO("记忆模块已启用：引擎未启动时页面显引导态，零后台成本");
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
        .onChange([this, offset](float v) { *offset = v; })
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
        note("memory.health.h1.note", cx, 180.0f,
             "冻结 15 查询转测检索，Recall@5 低于阈值提醒升级；本机 embedding 零 API 费。");
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

void MemoryModule::composeFeed(eui::Ui& ui, const core::theme::Tokens& t,
                               float x, float w, float y) {
    const auto snap = panel_.snapshot();
    constexpr float kCardH = 268.0f;
    ui.rect("memory.feed.card")
        .position(x, y)
        .size(w, kCardH)
        .color(t.colors.surface)
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
            .color(t.colors.surface)
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
        const engine::TokenUsageMeter usage;
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
            .color(t.colors.surface)
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
        .color(t.colors.surface)
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
               std::to_string(p.targetIds.empty() ? 0 : p.targetIds.front());
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
        .color(t.colors.surface)
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
        constexpr float kBarH = 56.0f;
        ui.rect("memory.rv.toolbar")
            .position(x, cy)
            .size(w, kBarH)
            .color(t.colors.surface)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .build();
        auto chipRow = [&](const std::string& prefix, float cx, float cyRow,
                           const std::vector<std::string>& labels, int active,
                           std::function<void(int)> onPick) {
            for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
                const float chipW =
                    static_cast<float>(
                        labels[static_cast<std::size_t>(i)].size()) *
                        12.0f +
                    24.0f;
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
                     {"全部", "合并", "失效", "遗忘", "新增"}, filterType_,
                     [this](int v) { filterType_ = v; });
        cx += 14.0f;
        cx = chipRow("memory.rv.chipS.", cx, cy + 10.0f,
                     {"全部来源", "Claude 对话", "整理", "遗忘扫描"}, filterSrc_,
                     [this](int v) { filterSrc_ = v; });
        cx += 14.0f;
        cx = chipRow("memory.rv.chipSt.", cx, cy + 10.0f, {"待审", "已处理"},
                     filterState_, [this](int v) { filterState_ = v; });
        // 批量模式开关（右侧）
        ui.text("memory.rv.batchlbl")
            .position(x + w - 220.0f, cy + 14.0f)
            .size(120.0f, 20.0f)
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
                .color(t.colors.surface)
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
            .color(t.colors.surface)
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
            .color(t.colors.surface)
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
            .color(t.colors.surface)
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
            .color(t.colors.surface)
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

    // ── 数据与隐私（目录显示 + 导出 + 清除全部）──
    {
        constexpr float kCardH = 156.0f;
        ui.rect("memory.cfg.data")
            .position(x, cy)
            .size(w, kCardH)
            .color(t.colors.surface)
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

void MemoryModule::composeEditDialog(eui::Ui& ui, const eui::Screen& screen,
                                     const core::theme::Tokens& t,
                                     const components::theme::ThemeColorTokens& ct) {
    if (editingId_.empty()) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.edit";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 240.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) editingId_.clear();
    };
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        ui_kit::drawDialogTitle(ui, "memory.dialog.edit.title", w, "编辑后接受",
                                20.0f, t.colors.textPrimary, {});
        ui.text("memory.dialog.edit.hint")
            .position(ui_kit::kDialogInsetX, 56.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 20.0f)
            .text("修改记忆正文后接受；引擎将按新正文执行。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "memory.dialog.edit.input")
            .position(ui_kit::kDialogInsetX, 84.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 40.0f)
            .value(editText_)
            .placeholder("记忆正文")
            .fontSize(core::theme::scaled(14.0f))
            .theme(ct)
            .onChange([this](const std::string& v) { editText_ = v; })
            .onCancel([this] { editingId_.clear(); })
            .build();
        ui_kit::button(ui, "memory.dialog.edit.cancel", "取消",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       150.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { editingId_.clear(); }});
        ui_kit::button(ui, "memory.dialog.edit.go", "接受",
                       ui_kit::ButtonLevel::Primary,
                       w - ui_kit::kDialogBodyInsetX - 110.0f, 150.0f, 110.0f,
                       ui_kit::kButtonHeight, t,
                       {[this] {
                           const std::string id = editingId_;
                           const std::string text = editText_;
                           editingId_.clear();
                           auto r = panel_.editThenAccept(id, text);
                           if (r.isFail()) {
                               showToast("未能执行", r.errorMessage(), true);
                           } else {
                               showToast("已接受", "编辑后的记忆已执行");
                           }
                       }});
    });
}

void MemoryModule::composeMaskModal(eui::Ui& ui, const eui::Screen& screen,
                                    const core::theme::Tokens& t,
                                    const components::theme::ThemeColorTokens& ct) {
    if (!maskModalOpen_) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.mask";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 220.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) maskModalOpen_ = false;
    };
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        // 设计稿逐字文案（mask-modal）
        ui_kit::drawDialogTitle(ui, "memory.dialog.mask.title", w,
                                "确认关闭脱敏？", 20.0f, t.colors.errorText, {});
        ui.text("memory.dialog.mask.body")
            .position(ui_kit::kDialogInsetX, 56.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 64.0f)
            .wrap()
            .text("关闭后，密钥/手机号等敏感信息将原样发送给云端大模型。"
                  "仅建议临时排查时使用，用完请及时重新开启。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        ui_kit::button(ui, "memory.dialog.mask.cancel", "取消",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       150.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { maskModalOpen_ = false; }});
        ui_kit::button(ui, "memory.dialog.mask.confirm", "仍要关闭",
                       ui_kit::ButtonLevel::Danger,
                       w - ui_kit::kDialogBodyInsetX - 130.0f, 150.0f, 130.0f,
                       ui_kit::kButtonHeight, t,
                       {[this] {
                           maskModalOpen_ = false;
                           panel_.setSanitizeEnabled(false);
                           showToast("脱敏已关闭",
                                     "内容将原样出机，用完请尽快重新开启", true);
                       }});
    });
}

void MemoryModule::composeClearModal(eui::Ui& ui, const eui::Screen& screen,
                                     const core::theme::Tokens& t,
                                     const components::theme::ThemeColorTokens& ct) {
    if (!clearModalOpen_) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.clear";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 260.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) clearModalOpen_ = false;
    };
    const auto snap = panel_.snapshot();
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        ui_kit::drawDialogTitle(ui, "memory.dialog.clear.title", w,
                                "清除全部记忆？", 20.0f, t.colors.errorText, {});
        ui.text("memory.dialog.clear.body")
            .position(ui_kit::kDialogInsetX, 56.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 64.0f)
            .wrap()
            .text("将删除本机面板库全部 " +
                  std::to_string(snap.proposalTotal) +
                  " 条提案记录与全部消耗账面，操作不可恢复。"
                  "如确定，请在下方输入「清除」二字。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "memory.dialog.clear.input")
            .position(ui_kit::kDialogInsetX, 128.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 40.0f)
            .value(clearInput_)
            .placeholder("输入：清除")
            .fontSize(core::theme::scaled(14.0f))
            .theme(ct)
            .onChange([this](const std::string& v) { clearInput_ = v; })
            .onCancel([this] { clearModalOpen_ = false; })
            .build();
        ui_kit::button(ui, "memory.dialog.clear.cancel", "取消",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       190.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { clearModalOpen_ = false; }});
        const bool armed = clearInput_ == "清除";
        ui_kit::button(ui, "memory.dialog.clear.go", "确认清除",
                       armed ? ui_kit::ButtonLevel::Danger
                             : ui_kit::ButtonLevel::Secondary,
                       w - ui_kit::kDialogBodyInsetX - 130.0f, 190.0f, 130.0f,
                       ui_kit::kButtonHeight, t,
                       {[this, armed] {
                           if (!armed) return;
                           clearModalOpen_ = false;
                           auto r = panel_.clearAllOwnStores();
                           if (r.isFail()) {
                               showToast("清除失败", r.errorMessage(), true);
                           } else {
                               showToast("已清除",
                                         "面板库已清空（引擎侧记忆不在本操作范围）",
                                         true);
                           }
                       }});
    });
}

void MemoryModule::composeWizardModal(eui::Ui& ui, const eui::Screen& screen,
                                      const core::theme::Tokens& t,
                                      const components::theme::ThemeColorTokens& ct) {
    if (!wizardOpen_) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.wizard";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 320.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) wizardOpen_ = false;
    };
    panel_.refresh(true);  // 向导打开期间快步刷新，步骤文案跟着引擎状态走
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        ui_kit::drawDialogTitle(ui, "memory.dialog.wizard.title", w,
                                "首次安装向导", 20.0f, t.colors.textPrimary, {});
        // 步骤清单：检测 → venv+pip → 拉起 → 完成/失败（状态真接 TelememEngine）
        auto stepRow = [&](const char* id, float ry, const char* name,
                           const std::string& status, eui::Color color) {
            ui.text(std::string(id) + ".name")
                .position(ui_kit::kDialogInsetX, ry)
                .size(180.0f, 22.0f)
                .text(name)
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(22.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(std::string(id) + ".status")
                .position(ui_kit::kDialogInsetX + 190.0f, ry)
                .size(w - ui_kit::kDialogBodyInsetX - 190.0f, 22.0f)
                .text(status)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(22.0f)
                .color(color)
                .build();
        };
        const auto state = es.state;
        const auto pendingColor = t.colors.textTertiary;
        const auto okColor = t.colors.success;
        const auto doingColor = t.colors.warning;
        const auto failColor = t.colors.errorText;
        const bool idleLike = state == TelememEngine::State::Idle ||
                              state == TelememEngine::State::Stopped ||
                              state == TelememEngine::State::Failed;
        stepRow("memory.dialog.wizard.s1", 56.0f, "① Python 3.10~3.12",
                es.pythonFound
                    ? ("已检测（" + es.pythonVersion + "）")
                    : std::string(idleLike ? "未检测到——安装后重试" : "检测中…"),
                es.pythonFound
                    ? okColor
                    : (state == TelememEngine::State::Preparing ? doingColor
                                                                : failColor));
        stepRow("memory.dialog.wizard.s2", 84.0f, "② venv + pip 安装",
                state == TelememEngine::State::Preparing
                    ? (es.detail.empty() ? std::string("进行中…") : es.detail)
                    : (state == TelememEngine::State::Running
                           ? std::string("完成")
                           : std::string(idleLike ? "待开始" : "进行中…")),
                state == TelememEngine::State::Preparing
                    ? doingColor
                    : (state == TelememEngine::State::Running ? okColor
                                                              : pendingColor));
        stepRow("memory.dialog.wizard.s3", 112.0f, "③ 引擎拉起与健康检查",
                state == TelememEngine::State::Running
                    ? "运行中（127.0.0.1:8421）"
                    : std::string(idleLike ? "待开始" : "进行中…"),
                state == TelememEngine::State::Running ? okColor : doingColor);
        // 完成态 / 失败态 / 引导态
        if (state == TelememEngine::State::Running) {
            ui.text("memory.dialog.wizard.done")
                .position(ui_kit::kDialogInsetX, 148.0f)
                .size(w - ui_kit::kDialogBodyInsetX, 44.0f)
                .wrap()
                .text("安装完成，记忆引擎已在后台运行。关掉本向导即可正常使用。")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .color(okColor)
                .build();
        } else if (state == TelememEngine::State::Failed) {
            ui.text("memory.dialog.wizard.fail")
                .position(ui_kit::kDialogInsetX, 148.0f)
                .size(w - ui_kit::kDialogBodyInsetX, 60.0f)
                .wrap()
                .text("安装失败：" + es.detail + "（可点「重试安装」再跑一轮）")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .color(failColor)
                .build();
        } else {
            ui.text("memory.dialog.wizard.intro")
                .position(ui_kit::kDialogInsetX, 148.0f)
                .size(w - ui_kit::kDialogBodyInsetX, 44.0f)
                .wrap()
                .text("本向导将检测 Python、创建虚拟环境并 pip 安装 telemem，"
                      "全程只写数据目录，不捆绑解释器。")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .color(t.colors.textSecondary)
                .build();
        }
        ui_kit::button(ui, "memory.dialog.wizard.close", "关闭",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       250.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { wizardOpen_ = false; }});
        const bool installing = state == TelememEngine::State::Preparing;
        const bool canStart =
            !installing && state != TelememEngine::State::Running;
        ui_kit::button(ui, "memory.dialog.wizard.go",
                       installing
                           ? "安装中…"
                           : (state == TelememEngine::State::Running
                                  ? "重新运行安装检查"
                                  : "开始安装 / 重试"),
                       canStart ? ui_kit::ButtonLevel::Primary
                                : ui_kit::ButtonLevel::Secondary,
                       w - ui_kit::kDialogBodyInsetX - 190.0f, 250.0f, 190.0f,
                       ui_kit::kButtonHeight, t,
                       {canStart ? std::function<void()>([] {
                            TelememEngine::global().startAsync();
                        })
                                 : std::function<void()>()});
    });
}

} // namespace module::memory

// 默认开启的普通页面模块：工厂恒非空（无平台能力门槛）。
extern "C" ::core::Module* create_memory_module() {
    return new module::memory::MemoryModule();
}
