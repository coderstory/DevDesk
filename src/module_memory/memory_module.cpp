// src/module_memory/memory_module.cpp
// 记忆页三视图（S2 面板骨架）：主控台 / 记忆审核 / 配置。数据全部静态占位，
// 引擎启停按钮真接 TelememEngine（唯一活线）；提案人审与消耗账面在后续批次接入。
#include "memory_module.h"

#include "engine/token_usage.h"
#include "engine/telemem_config.h"
#include "engine/telemem_engine.h"
#include "core/logging.h"
#include "core/theme.h"
#include "ui_kit/button.h"
#include "ui_kit/wtab.h"

#include <eui/dsl.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace module::memory {

namespace {

using engine::TelememEngine;

constexpr float kTitleH = 32.0f;
constexpr float kTabH = 36.0f;
constexpr float kCardGap = 16.0f;

// 视图下标（与 tab 序一致）
constexpr int kViewConsole = 0;
constexpr int kViewReview = 1;
constexpr int kViewConfig = 2;

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

} // namespace

::core::PageId MemoryModule::pageId() const { return ::core::PageId::Memory; }

std::string MemoryModule::displayName() const { return "记忆"; }

std::string MemoryModule::shortName() const { return "记忆"; }

// fa-brain (U+F5DC) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string MemoryModule::iconGlyph() const { return "\xEF\x97\x9C"; }

void MemoryModule::onStart() {
    LOG_INFO("记忆模块已启用：引擎未启动时页面显引导态，零后台成本");
}

void MemoryModule::onStop() {
    // 页面关闭 ≠ 引擎停止：引擎是应用级常驻服务，生命周期归 TelememEngine，
    // 不随页面插拔终止（托盘/后台注入仍依赖它）。
    LOG_INFO("记忆页已关闭（引擎不受影响）");
}

void MemoryModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const float pagePad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pagePad * 2.0f);

    ui.text("memory.title")
        .position(pagePad, 0.0f)
        .size(contentW, kTitleH)
        .text("记忆")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(kTitleH)
        .fontWeight(700)
        .color(t.colors.textPrimary)
        .build();

    // 三视图分段 Tab（ui_kit::wtab 块面式；节点 id 见 wtab.h 契约）
    const float tabY = kTitleH + t.sizes.spacingLarge;
    const float tabW = 120.0f;
    const char* tabLabels[kViewConfig + 1] = {"主控台", "记忆审核", "配置"};
    for (int i = kViewConsole; i <= kViewConfig; ++i) {
        const std::string id = "memory.tabs." + std::to_string(i);
        const int view = i;  // lambda 捕获用
        ui_kit::wtab(ui, id, tabLabels[i], activeView_ == i,
                     pagePad + static_cast<float>(i) * (tabW + 4.0f), tabY,
                     tabW, kTabH, t, [this, view] { activeView_ = view; });
    }

    const float bodyY = tabY + kTabH + t.sizes.spacingLarge;
    if (activeView_ == kViewConsole) {
        composeConsole(ui, t, pagePad, contentW, bodyY);
    } else if (activeView_ == kViewReview) {
        composeReview(ui, t, pagePad, contentW, bodyY);
    } else {
        composeConfig(ui, t, pagePad, contentW, bodyY);
    }
}

// ── 视图 1：主控台 ──

void MemoryModule::composeConsole(eui::Ui& ui, const core::theme::Tokens& t,
                                  float x, float w, float y) {
    const auto snap = TelememEngine::global().snapshot();
    const eui::Color stateColor = engineStateColor(t, snap.state);

    // 引擎状态卡
    constexpr float kCardH = 168.0f;
    ui.rect("memory.eng.card")
        .position(x, y)
        .size(w, kCardH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    // 状态圆点
    ui.rect("memory.eng.dot")
        .position(x + t.sizes.spacingLarge, y + 22.0f)
        .size(10.0f, 10.0f)
        .color(snap.state == TelememEngine::State::Running
                   ? t.colors.success
                   : t.colors.textDisabled)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text("memory.eng.title")
        .position(x + t.sizes.spacingLarge + 18.0f, y + 12.0f)
        .size(360.0f, t.sizes.fontSizeLarge + 8.0f)
        .text("telemem 记忆引擎")
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(t.sizes.fontSizeLarge + 8.0f)
        .fontWeight(700)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.eng.state")
        .position(x + w - 300.0f, y + 12.0f)
        .size(284.0f, t.sizes.fontSizeBody + 6.0f)
        .text(engineStateText(snap.state))
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(t.sizes.fontSizeBody + 6.0f)
        .color(stateColor)
        .build();
    // 状态明细（含失败原因/引导文案；面板不留白）
    ui.text("memory.eng.detail")
        .position(x + t.sizes.spacingLarge, y + 48.0f)
        .size(w - 2.0f * t.sizes.spacingLarge, 44.0f)
        .text(snap.detail.empty() ? std::string("它替 Claude 记住你的偏好和项目背景，"
                                                  "聊天时自动带上。")
                                  : snap.detail)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(22.0f)
        .color(snap.state == TelememEngine::State::Failed ? t.colors.errorText
                                                          : t.colors.textSecondary)
        .build();
    // Python 环境行（探测结果；未找到 = 引导文案）
    const std::string pythonLine =
        snap.pythonFound
            ? "Python " + snap.pythonVersion +
                  " 已检测（telemem 要求 3.10~3.12）"
            : "未检测到 Python 3.10~3.12——请安装后点「启动引擎」重试";
    ui.text("memory.eng.python")
        .position(x + t.sizes.spacingLarge, y + 96.0f)
        .size(w - 2.0f * t.sizes.spacingLarge, 20.0f)
        .text(pythonLine)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(20.0f)
        .color(t.colors.textTertiary)
        .build();
    // 启停按钮：真接引擎服务（engine 侧自带防重入；按钮恒渲染，语义由状态兜住）
    const bool canStart = snap.state != TelememEngine::State::Running &&
                          snap.state != TelememEngine::State::Preparing;
    const bool canStop = !canStart;
    ui_kit::button(ui, "memory.eng.start",
                   snap.state == TelememEngine::State::Preparing ? "启动中…"
                                                                 : "启动引擎",
                   ui_kit::ButtonLevel::Primary, x + t.sizes.spacingLarge,
                   y + kCardH - 48.0f, 158.0f, ui_kit::kButtonHeight, t,
                   {canStart ? [] { TelememEngine::global().startAsync(); }
                             : std::function<void()>()});
    ui_kit::button(ui, "memory.eng.stop", "停止引擎", ui_kit::ButtonLevel::Danger,
                   x + t.sizes.spacingLarge + 174.0f, y + kCardH - 48.0f, 158.0f,
                   ui_kit::kButtonHeight, t,
                   {canStop ? [] { TelememEngine::global().stopAsync(); }
                            : std::function<void()>()});

    // 健康四格（S2 静态占位：数值随对应链路接入换真账面）
    const float gridY = y + kCardH + kCardGap;
    const float cardW = (w - 3.0f * kCardGap) / 4.0f;
    constexpr float kStatH = 150.0f;
    const struct {
        const char* label;
        std::string value;
        const char* note;
    } stats[4] = {
        {"脱敏闸", "强制开启", "出机前自动清除敏感信息（S3 接入）"},
        {"触发线 · 记得牢不牢", "—", "Recall@5 低于 85% 提醒升级（S5 出数）"},
        {"注入预算", "—", "每次对话 ≤5 条 ≤500 字（S3 接入）"},
        {"存储概览", "—", "记忆存本机，可随时导出"},
    };
    for (int i = 0; i < 4; ++i) {
        const float cx = x + static_cast<float>(i) * (cardW + kCardGap);
        const std::string sid = "memory.health.card" + std::to_string(i);
        ui.rect(sid)
            .position(cx, gridY)
            .size(cardW, kStatH)
            .color(t.colors.surface)
            .radius(t.sizes.radiusOverlay)
            .border(1.0f, t.colors.borderDefault)
            .build();
        ui.text("memory.health.label" + std::to_string(i))
            .position(cx + 14.0f, gridY + 12.0f)
            .size(cardW - 28.0f, 18.0f)
            .text(stats[i].label)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(18.0f)
            .fontWeight(600)
            .color(t.colors.textSecondary)
            .build();
        ui.text("memory.health.value" + std::to_string(i))
            .position(cx + 14.0f, gridY + 36.0f)
            .size(cardW - 28.0f, 40.0f)
            .text(stats[i].value)
            .fontSize(t.sizes.fontSizeStat)
            .lineHeight(40.0f)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();
        ui.text("memory.health.note" + std::to_string(i))
            .position(cx + 14.0f, gridY + 84.0f)
            .size(cardW - 28.0f, 52.0f)
            .text(stats[i].note)
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(17.0f)
            .color(t.colors.textTertiary)
            .build();
    }

    // 消耗页骨架（任务书 #3：token 计量表结构 + UI 占位；账面后续批次接真数据）
    const engine::TokenUsageMeter usage;  // 骨架期恒 0 占位
    const float usageY = gridY + kStatH + kCardGap;
    constexpr float kUsageH = 120.0f;
    ui.rect("memory.usage.card")
        .position(x, usageY)
        .size(w, kUsageH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.usage.title")
        .position(x + 16.0f, usageY + 12.0f)
        .size(300.0f, 22.0f)
        .text("token 消耗（今日 / 本月）")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(22.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    const auto phase = usage.phase();
    ui.text("memory.usage.phase")
        .position(x + w - 200.0f, usageY + 12.0f)
        .size(184.0f, 22.0f)
        .text(phase == engine::TokenUsageMeter::Phase::Hard
                  ? "已触发硬档 · 提炼暂停（占位）"
                  : phase == engine::TokenUsageMeter::Phase::Soft
                        ? "逼近预算 · 软档提醒（占位）"
                        : "预算充足（占位）")
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(22.0f)
        .color(phase == engine::TokenUsageMeter::Phase::Normal
                   ? t.colors.textTertiary
                   : t.colors.warning)
        .build();
    // 仪表条：bg + fill（占比以硬档为满刻度）
    const float barX = x + 16.0f;
    const float barW = w - 32.0f;
    ui.rect("memory.usage.bar")
        .position(barX, usageY + 48.0f)
        .size(barW, 10.0f)
        .color(t.colors.active)
        .radius(t.sizes.radiusPill)
        .build();
    const float fillW =
        static_cast<float>(usage.monthRatio()) * barW;
    if (fillW > 1.0f) {
        ui.rect("memory.usage.fill")
            .position(barX, usageY + 48.0f)
            .size(fillW, 10.0f)
            .color(phase == engine::TokenUsageMeter::Phase::Normal
                       ? t.colors.primary
                       : t.colors.warning)
            .radius(t.sizes.radiusPill)
            .build();
    }
    char numBuf[128];
    std::snprintf(numBuf, sizeof(numBuf), "今日 %lld tk · 本月 %lld tk · 软档 %lld · 硬档 %lld",
                  usage.todayTokens, usage.monthTokens, usage.softBudgetTokens,
                  usage.hardBudgetTokens);
    ui.text("memory.usage.nums")
        .position(barX, usageY + 68.0f)
        .size(barW, 34.0f)
        .text(std::string(numBuf) + "（账面接入前恒为 0 占位）")
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(17.0f)
        .color(t.colors.textTertiary)
        .build();

    // 最近动态（静态示例文案；真动态流随采集/提炼批次接入）
    const float feedY = usageY + kUsageH + kCardGap;
    constexpr float kFeedH = 150.0f;
    ui.rect("memory.feed.card")
        .position(x, feedY)
        .size(w, kFeedH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.feed.title")
        .position(x + 16.0f, feedY + 12.0f)
        .size(300.0f, 22.0f)
        .text("最近动态")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(22.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    const char* feedLines[3] = {
        "采集 / 注入 / 提炼动态将在这里滚动显示（S3 接入）",
        "拦截与合并记录同样入流（脱敏闸与卡片代理批次接入）",
        "本卡片当前为静态占位，引擎动作不在此刷新",
    };
    for (int i = 0; i < 3; ++i) {
        ui.text("memory.feed.line" + std::to_string(i))
            .position(x + 16.0f, feedY + 44.0f + static_cast<float>(i) * 30.0f)
            .size(w - 32.0f, 26.0f)
            .text(feedLines[i])
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(26.0f)
            .color(t.colors.textSecondary)
            .build();
    }
}

// ── 视图 2：记忆审核 ──

void MemoryModule::composeReview(eui::Ui& ui, const core::theme::Tokens& t,
                                 float x, float w, float y) {
    ui.text("memory.rv.intro")
        .position(x, y)
        .size(w, 24.0f)
        .text("Claude 整理记忆前先问你一句：合并、失效、遗忘、新增，都由你拍板。")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(24.0f)
        .color(t.colors.textSecondary)
        .build();
    // 过滤 chips（静态占位：提案数据源 S4 接入后启用筛选）
    const float chipsY = y + 36.0f;
    const char* chips[] = {"全部", "合并", "失效", "遗忘", "新增"};
    float cx = x;
    for (int i = 0; i < 5; ++i) {
        const float chipW = 64.0f;
        const std::string id = "memory.rv.chip" + std::to_string(i);
        ui.rect(id)
            .position(cx, chipsY)
            .size(chipW, 28.0f)
            .color(i == 0 ? t.colors.primarySoftBg : t.colors.surface)
            .radius(t.sizes.radiusPill)
            .border(1.0f, t.colors.borderDefault)
            .build();
        ui.text("memory.rv.chiptext" + std::to_string(i))
            .position(cx, chipsY + 3.0f)
            .size(chipW, 22.0f)
            .text(chips[i])
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(22.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(i == 0 ? t.colors.primaryText : t.colors.textSecondary)
            .build();
        cx += chipW + 8.0f;
    }
    // 空态卡（S4 卡片代理接入前恒空态，不留白）
    const float emptyY = chipsY + 44.0f;
    constexpr float kEmptyH = 160.0f;
    ui.rect("memory.rv.empty")
        .position(x, emptyY)
        .size(w, kEmptyH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusOverlay)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.rv.empty.title")
        .position(x, emptyY + 40.0f)
        .size(w, 30.0f)
        .text("还没有需要你拿主意的提案")
        .fontSize(t.sizes.fontSizeLarge)
        .lineHeight(30.0f)
        .fontWeight(700)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.rv.empty.note")
        .position(x, emptyY + 82.0f)
        .size(w, 40.0f)
        .text("卡片人审（合并 / 失效 / 遗忘 / 新增）在 S4 批次接入；"
              "接入后提案会自动出现在这里")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
}

// ── 视图 3：配置 ──

void MemoryModule::composeConfig(eui::Ui& ui, const core::theme::Tokens& t,
                                 float x, float w, float y) {
    // 端点卡：LLM（anthropic provider · 智谱兼容端点）
    constexpr float kCardH = 150.0f;
    ui.rect("memory.cfg.llm")
        .position(x, y)
        .size(w, kCardH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.cfg.llm.title")
        .position(x + 16.0f, y + 12.0f)
        .size(400.0f, 24.0f)
        .text("大模型端点（提炼）")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(24.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.cfg.llm.rows")
        .position(x + 16.0f, y + 44.0f)
        .size(w - 32.0f, 62.0f)
        .text(std::string("提供方 ") + engine::kLlmProvider + " · 端点 " +
              engine::kLlmBaseUrl + " · 模型 " + engine::kLlmModel)
        .fontSize(core::theme::scaled(13.0f))
        .lineHeight(20.0f)
        .color(t.colors.textSecondary)
        .build();
    ui.text("memory.cfg.llm.note")
        .position(x + 16.0f, y + 108.0f)
        .size(w - 32.0f, 30.0f)
        .text("密钥运行时读自 Claude 设置（settings.json），只进引擎进程环境，"
              "绝不写入配置文件与日志")
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(15.0f)
        .color(t.colors.textTertiary)
        .build();

    // 端点卡：embedding（本机 llama-server）
    const float embedY = y + kCardH + kCardGap;
    ui.rect("memory.cfg.embed")
        .position(x, embedY)
        .size(w, kCardH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.cfg.embed.title")
        .position(x + 16.0f, embedY + 12.0f)
        .size(400.0f, 24.0f)
        .text("向量化端点（本机）")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(24.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.cfg.embed.rows")
        .position(x + 16.0f, embedY + 44.0f)
        .size(w - 32.0f, 62.0f)
        .text(std::string("llama-server · http://127.0.0.1:") +
              std::to_string(engine::kEmbedServerPort) + " · 模型 " +
              engine::kEmbedModel + " · 记忆原文不出机")
        .fontSize(core::theme::scaled(13.0f))
        .lineHeight(20.0f)
        .color(t.colors.textSecondary)
        .build();
    ui.text("memory.cfg.embed.note")
        .position(x + 16.0f, embedY + 108.0f)
        .size(w - 32.0f, 30.0f)
        .text("维度在建索引前定死，换模型档 = 全量重嵌；模型文件落数据目录")
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(15.0f)
        .color(t.colors.textTertiary)
        .build();

    // 数据与隐私卡：真实数据目录 + 预算占位
    const float dataY = embedY + kCardH + kCardGap;
    constexpr float kDataH = 130.0f;
    ui.rect("memory.cfg.data")
        .position(x, dataY)
        .size(w, kDataH)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
    ui.text("memory.cfg.data.title")
        .position(x + 16.0f, dataY + 12.0f)
        .size(400.0f, 24.0f)
        .text("数据与隐私")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(24.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.cfg.data.path")
        .position(x + 16.0f, dataY + 44.0f)
        .size(w - 32.0f, 24.0f)
        .text(engine::memoryDataDir().generic_string())
        .fontSize(core::theme::scaled(13.0f))
        .lineHeight(24.0f)
        .color(t.colors.textSecondary)
        .build();
    ui.text("memory.cfg.data.note")
        .position(x + 16.0f, dataY + 76.0f)
        .size(w - 32.0f, 44.0f)
        .text("venv / 向量库 / 历史库 / 模型文件全部收敛在该目录，随 DEVDESK_DATA_ROOT "
              "隔离；导出与清空入口随 S4 接入")
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(15.0f)
        .color(t.colors.textTertiary)
        .build();
}

} // namespace module::memory

// 默认开启的普通页面模块：工厂恒非空（无平台能力门槛）。
extern "C" ::core::Module* create_memory_module() {
    return new module::memory::MemoryModule();
}
