// Home 仪表盘模块：只读聚合 markdown_index / todo_tasks / claude_call_logs，
// 经 core::appDatabase() 拿共享连接。导航经 core::appRouter()，module_* 互不依赖；
// 跨模块动作走 Router payload（"action:*" / "open:*"），由目标页消费。
#include "core/call_rate.h"
#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/theme.h"
#include "home_service.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/card.h>
#include <components/linechart.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace module::home {

namespace {

// 首页「今日 token 图表卡」的固定标称高度。首页各块高度全部固定（欢迎区 40 /
// 统计网格 statH / 图表卡本值 / 快捷按钮行 44），页面装不下时不做任何压缩——
// 溢出由整页外层 home.scroll（scrollView）出滚动条吸收，最小窗（1000×640）也
// 不丢内容。
constexpr float kHomeChartH = 260.0f;

// 渲染单个统计卡片（card 组件，点击整卡跳转）。numberColor/badgeColor 非空覆盖默认配色；
// timerSeconds > 0 挂 0 尺寸 onTimer 定期重绘；chart 非空叠加迷你图表；onTick 为该 timer 回调。
// 卡内容四周统一 16px 内边距（显式 .padding 替代组件默认 20）
void drawStatCard(eui::Ui& ui,
                  const std::string& id,
                  float width,
                  float height,
                  const core::theme::Tokens& t,
                  const std::string& label,
                  const std::string& number,
                  const std::string& badge,
                  bool accent,
                  std::function<void()> onClick,
                  const eui::Color* numberColor = nullptr,
                  const eui::Color* badgeColor = nullptr,
                  float timerSeconds = 0.0f,
                  std::function<void(eui::Ui&, float, float)> chart =
                      nullptr,
                  std::function<void()> onTick = nullptr) {
    const auto ct = core::theme::componentTokensWeChat(t);  // W9 微信白卡
    const float s = core::theme::fontScale();
    const float inset = t.sizes.spacingLarge; // 16：卡内容统一内边距（16px 网格）
    const float cw = width - inset * 2.0f;
    const float ch = height - inset * 2.0f;
    components::card(ui, id)
        .size(width, height) // panelShadow 由 CardStyle 默认注入
        .theme(ct)           // 先注入主题再覆盖 radius（theme 会重置 CardStyle）
        .radius(t.sizes.radiusWeChat)  // W9 微信统一圆角 6px（D-12 §4.5）
        .padding(inset)      // 显式 16：不用组件默认 20（与旧坐标叠加致顶留空/底溢出）
        .content([&] {
            // 交互层（点击 + hover 过渡）：画整卡矩形垫底，内容画其上
            ui.rect(id + ".press")
                .position(-inset, -inset)
                .size(width, height)
                .states(components::theme::color(0.0f, 0.0f, 0.0f,
                                                 0.0f),
                        t.colors.wechat.dividerSecondary,
                        t.colors.wechat.selectedBg)
                .radius(t.sizes.radiusWeChat)
                .transition(core::Transition::make(0.1f,
                                                   core::Ease::OutQuad))
                .animate(core::AnimProperty::Color)
                .onClick(std::move(onClick))
                .build();
            ui.stack(id + ".inner")
                .size(cw, ch)
                .content([&] {
                    // 卡内文字：坐标相对 card content 原点（已含 16 inset）
                    // L-4（UAT2 一审）：label 盒高 18→16，底缘 32 恰接 number 盒顶
                    //（旧 18 高 → 底缘 34 与 number 顶 32 重叠 2px；探针组 A 记载的
                    // 期望几何即「.label y16..32」）。lineHeight 仍 18：EUI 文本字形
                    // 按 lineHeight 布局、不按盒高裁字，改盒高视觉零变化。
                    ui.text(id + ".label")
                        .position(0.0f, 16.0f * s)
                        .size(cw, 16.0f * s)
                        .text(label)
                        .fontSize(t.sizes.fontSizeSmall + 1.0f)
                        .lineHeight(18.0f * s)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(t.colors.wechat.textTertiary)  // W9 三级灰 #999999
                        .build();
                    // UAT2 覆盖 W9：数字 22px（fontSizeLarge+4）深灰 #1A1A1A，更醒目
                    const float numberSize = t.sizes.fontSizeLarge + 4.0f;
                    ui.text(id + ".number")
                        .position(0.0f, 32.0f * s)
                        .size(cw, 56.0f * s)
                        .text(number)
                        .fontSize(numberSize)
                        .lineHeight(56.0f * s)
                        .fontWeight(700)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(numberColor ? *numberColor
                                           : (accent ? t.colors.wechat.warningRed
                                                     : t.colors.wechat.textPrimary))
                        .build();
                    ui.text(id + ".badge")
                        // 底对齐 height-48（= 16 inset + 16 badge + 16 底边距），任何字号档底部不贴卡边
                        .position(0.0f, height - 48.0f)
                        .size(cw, 16.0f)
                        .text(badge)
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(16.0f)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(badgeColor ? *badgeColor
                                          : (accent ? t.colors.wechat.warningRed
                                                    : t.colors.wechat.textTertiary))
                        .build();
                    if (timerSeconds > 0.0f) {
                        // 周期整卡重绘（compose 不是每帧重跑，靠 onTimer 触发）；onTick 非空时用它
                        ui.stack(id + ".timer")
                            .size(0.0f, 0.0f)
                            .onTimer(timerSeconds,
                                     onTick ? std::move(onTick)
                                            : std::function<void()>([] {}))
                            .build();
                    }
                    if (chart) {
                        // sparkline 固定进顶带（内 y0..16 = label 16 之上），整盒高 16
                        const float sparkW = cw - 56.0f * s;
                        ui.stack(id + ".sparkwrap")
                            .position(0.0f, 0.0f)
                            .size(sparkW, 16.0f)
                            .content([&] { chart(ui, sparkW, 16.0f); })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

// 模块注册感知：按 Router 注册状态隐藏对应卡片/按钮，不显示陈旧数据；appRouter() 为 null（无头测试）视为全启用
bool moduleRegistered(::core::PageId page) {
    auto* router = ::core::appRouter();
    return router == nullptr || router->isRegistered(page);
}

// 统计卡 / 快捷按钮元数据表：static 纯数据表，不带 lambda/可调用列（static 表带可调用列
// 首次初始化只求值一次，捕获引用会冻结甚至悬垂）。行序 = 渲染序；数值/副标题文案在 compose 逐行查表外取。

// 统计卡：kind = 行身份，module = 归属模块（注册态过滤），target = 点击跳转页。
enum class StatCardKind { Tokens, Notes, TodoPending, TodoOverdue, ApiCalls };
struct HomeStatCard {
    StatCardKind kind;
    ::core::PageId module;
    const char* title;
    ::core::PageId target;
};
constexpr HomeStatCard kStatCards[] = {
    {StatCardKind::Tokens,      ::core::PageId::ClaudeProxy, "Token 速率 (tok/s)", ::core::PageId::ClaudeProxy},
    {StatCardKind::Notes,       ::core::PageId::Markdown,    "今日笔记",           ::core::PageId::Markdown},
    {StatCardKind::TodoPending, ::core::PageId::Todo,        "待办任务",           ::core::PageId::Todo},
    {StatCardKind::TodoOverdue, ::core::PageId::Todo,        "逾期任务",           ::core::PageId::Todo},
    {StatCardKind::ApiCalls,    ::core::PageId::ClaudeProxy, "API 调用",           ::core::PageId::ClaudeProxy},
};

// 快捷按钮：kind = 行身份，module = 归属模块，target + navPayload = 点击负载。
enum class ActionKind { NewNote, NewTodo, SwitchClaude };
struct HomeActionButton {
    ActionKind kind;
    ::core::PageId module;
    const char* label;
    unsigned int iconCodepoint;
    ::core::PageId target;
    const char* navPayload;
};
constexpr HomeActionButton kActionButtons[] = {
    {ActionKind::NewNote,    ::core::PageId::Markdown,    "+ 新建笔记",         0,     ::core::PageId::Markdown,    "action:new_note"},
    {ActionKind::NewTodo,    ::core::PageId::Todo,        "+ 新建任务",         0,     ::core::PageId::Todo,        "action:new_task"},
    {ActionKind::SwitchClaude, ::core::PageId::ClaudeProxy, "切换 Claude Profile", 0xF46D, ::core::PageId::ClaudeProxy, ""},
};

// 元素 id 由 kind 唯一决定（单测锁定这些 id，行序重排不飘）。
const char* statCardElementId(StatCardKind kind) {
    switch (kind) {
        case StatCardKind::Tokens: return "home.stat.tokens";
        case StatCardKind::Notes: return "home.stat.notes";
        case StatCardKind::TodoPending: return "home.stat.todo";
        case StatCardKind::TodoOverdue: return "home.stat.overdue";
        case StatCardKind::ApiCalls: return "home.stat.api";
    }
    return "home.stat.unknown";
}

const char* actionButtonElementId(ActionKind kind) {
    switch (kind) {
        case ActionKind::NewNote: return "home.act.newnote";
        case ActionKind::NewTodo: return "home.act.newtodo";
        case ActionKind::SwitchClaude: return "home.act.claude";
    }
    return "home.act.unknown";
}

} // namespace

class HomeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Home; }
    std::string displayName() const override { return "首页"; }
    std::string shortName() const override { return "首页"; }
    std::string iconGlyph() const override { return eui::utf8(0xF015); }  // fa-house

    void onStart() override {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            LOG_WARN("Home 模块启动：appDatabase() 为空，仪表盘显示空态");
            // 补齐错误文案，否则 compose 拼出「数据加载失败：（空串）」
            statsError_ = "数据库未初始化（appDatabase 为空）";
            return;
        }
        service_ = std::make_unique<HomeService>(*db);
        refreshStats();
    }

    void onStop() override {
        service_.reset();
        LOG_INFO("Home 模块停止");
    }

    // 进页重查：数据只在启动查一次，从别的页切回来数字不更新。
    void onPageEnter() override { refreshStats(); }

    void refreshStats() {
        if (!service_) return;
        auto r = service_->getStats(kRecentNoteLimit);
        if (r.isOk()) {
            stats_ = std::move(r.value());
            statsLoaded_ = true;
            statsError_.clear();
            LOG_INFO("Home 仪表盘数据：今日笔记 " +
                     std::to_string(stats_.todayNotes) + " / 待办 " +
                     std::to_string(stats_.pendingTodos) + " / 逾期 " +
                     std::to_string(stats_.overdueTodos) + " / 今日 API 调用 " +
                     std::to_string(stats_.todayApiCalls));
        } else {
            statsLoaded_ = false;
            statsError_ = r.errorMessage();
            LOG_ERROR("Home 仪表盘数据加载失败: " + statsError_);
        }
        refreshLastRate();
    }

    // 最近一次成功调用的速率（独立于 getStats）：失败不拖垮整页也不静默，单独记 lastRateError_
    void refreshLastRate() {
        if (!service_) return;
        auto r = service_->getLastCallRate();
        if (r.isOk()) {
            lastRate_ = r.value();
            lastRateError_.clear();
            LOG_INFO(std::string("Home 最近一次成功调用速率：") +
                     (lastRate_.hasValue
                          ? core::call_rate::formatTps(lastRate_.tps) + " tok/s（输出 " +
                                std::to_string(lastRate_.outputTokens) +
                                " tok / " + std::to_string(lastRate_.latencyMs) +
                                " ms）"
                          : std::string("无（暂无成功调用）")));
        } else {
            lastRate_ = LastCallRate{};
            lastRateError_ = r.errorMessage();
            LOG_ERROR("Home 最近一次调用速率查询失败: " + lastRateError_);
        }
    }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;       // 40
        const float gap = t.sizes.spacingLarge * 1.25f; // 20
        // ⚠ contentW 不在这里定义：整页挂在 home.scroll 下，内容宽只有 scrollView 的 content
        // 回调拿到的 innerW 说了算（见下方同名遮蔽定义）。kHeaderActionsW + 标题列 = contentW。
        constexpr float kHeaderActionsW = 100.0f;  // W9 仅保留刷新按钮（设置按钮已移除）
        const float cardGap = 20.0f;
        // 卡高 144 × fontScale 派生（144 = 16+16+8+56+16+16，见 drawStatCard）。
        const float statH = 144.0f * core::theme::fontScale();
        const std::time_t now = std::time(nullptr);

        // 模块注册感知：统计卡可见数从元数据表按注册态过滤派生（改表即改计数），全关整行隐藏。
        int statCount = 0;
        for (const auto& card : kStatCards) {
            if (moduleRegistered(card.module)) ++statCount;
        }

        // 页面不自铺底色，用主窗口 clearColor 透出（否则半透/磨砂档页面 alpha 恒 1 看不见效果）。

        // 首页外层滚动容器：整页包一层 scrollView，矮窗超出由滚动条吸收，装得下时几何不变。
        // ⚠ 上游坑：layoutColumn() 算子元素只看流式游标，position() 挂在 column 的流式
        // 子元素上会被静默忽略——home.root 的直接子块一律只用 flow + size()。形参 ui 刻意与外层同名。
        components::scrollView(ui, "home.scroll")
            .size(screen.width, screen.height)
            .theme(core::theme::componentTokensWeChat(t))  // W9 微信页面底 #F7F7F7
            .content([&](eui::Ui& ui, float innerW, float) {
            // ⚠ contentW 遮蔽外层：出滚动条时内容列收窄，必须跟 innerW 走，否则横向溢出。
            const float contentW = innerW - pad * 2.0f;
        ui.column("home.root")
            // 高度用 wrapContent：固定高度会让 scrollView 的测量 pass 永远量到「正好等于视口」→ 永不判可滚
            .width(innerW)
            .height(core::SizeValue::wrapContent())
            .padding(pad)
            .gap(gap)
            .content([&] {
                // UAT5 全局顶部收紧：页头行 72→40（与其他页 kHeaderH=40 同档）——
                // 标题 24px 加粗（与 todo/markdown/agent 页题同字号）+ 日期 14px 两行叠满。
                ui.row("home.header")
                    .size(contentW, 40.0f)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        // 标题列宽 = contentW − 右侧按钮组宽（两段和 = contentW，header 行 gap=0）。
                        ui.column("home.greeting")
                            .size(std::max(280.0f, contentW - kHeaderActionsW), 40.0f)
                            .gap(2.0f)
                            .content([&] {
                                ui.text("home.h1")
                                    .size(std::max(280.0f, contentW - kHeaderActionsW), 24.0f)
                                    .text("工作台")
                                    .fontSize(t.sizes.fontSizeTitle)
                                    .lineHeight(24.0f)
                                    .fontWeight(700)
                                    .color(t.colors.wechat.textPrimary)
                                    .build();
                                ui.text("home.date")
                                    .size(std::max(280.0f, contentW - kHeaderActionsW), 14.0f)
                                    .text("今天是 " + formatTodayCn(now))
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(14.0f)
                                    .color(t.colors.wechat.textSecondary)
                                    .build();
                            })
                            .build();

                        ui.row("home.header.actions")
                            .size(kHeaderActionsW, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                // UAT2 覆盖 W9：刷新按钮对齐底部次按钮——
                                // secondaryButtonBg 底 + textPrimary 字（不再透明底浮空）
                                components::button(ui, "home.btn.refresh")
                                    .size(100.0f, 40.0f)
                                    .text("刷新")
                                    .icon(0xF021)  // fa-arrows-rotate
                                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                                    .colors(t.colors.wechat.secondaryButtonBg,
                                            t.colors.wechat.dividerSecondary,
                                            t.colors.wechat.dividerSecondary)
                                    .textColor(t.colors.wechat.textPrimary)
                                    .iconColor(t.colors.wechat.textPrimary)
                                    .radius(t.sizes.radiusWeChat)
                                    .onClick([this] { refreshStats(); })
                                    .build();
                            })
                            .build();
                    })
                    .build();

                const bool ok = statsLoaded_;
                auto num = [&ok](int64_t v) {
                    return ok ? std::to_string(v) : std::string("--");
                };

                // Token 速率卡：数字 = 最近一次成功调用的速率（口径单源 core/call_rate.h）；
                // 无成功调用 → 空态「--」。流式实时值只喂 sparkline；流式中 primary 数字 + success 绿 badge。
                const core::proxy_stats::Snapshot snap =
                    core::proxy_stats::snapshot();
                const bool streaming = snap.activeStreams > 0;
                // 「加载失败」= 整页统计没加载成功或最近速率查询自己失败——不把「未加载」冒充「暂无成功调用」
                const bool rateFailed =
                    !statsLoaded_ || !lastRateError_.empty();
                const bool rateOk = !rateFailed && lastRate_.hasValue;
                const std::string tokenNumber =
                    rateOk ? core::call_rate::formatTps(lastRate_.tps)
                           : std::string("--");
                // badge 文案不带「会话」二字（11px 全角在最小窗会溢出卡片）。分档：流式中「● N 路生成中」；
                // 空闲「最近一次成功调用」/「暂无成功调用」/「加载失败」。
                const std::string tokenBadge =
                    streaming
                        ? "● " + std::to_string(snap.activeStreams) +
                              " 路生成中" + (rateOk ? " · 最近一次" : "")
                        : (rateFailed
                               ? "加载失败"
                               : (rateOk ? "最近一次成功调用"
                                         : "暂无成功调用"));

                if (statCount == 0) {
                    // 三模块全关的空态兜底：标题区保留 + 一句极简引导。
                    ui.text("home.stats.alloff")
                        .size(contentW, 32.0f)
                        .text("所有工具模块已关闭")
                        .fontSize(t.sizes.fontSizeBody)
                        .lineHeight(32.0f)
                        .color(t.colors.wechat.textTertiary)
                        .build();
                } else {
                const float statW =
                    (contentW - cardGap * static_cast<float>(statCount - 1)) /
                    static_cast<float>(statCount);
                // sparkline 序列只取一次（live 判定与图表绘制共用；空序列兜底 {0.0f}）。
                const std::vector<float> sparkSeries =
                    core::proxy_stats::rateSeries(24);
                ui.row("home.stats")
                    .size(contentW, statH)
                    .gap(cardGap)
                    .content([&] {
                        // 逐表行渲染：行序 = 渲染序，元素 id 由 kind 决定；数值/配色/图表按行身份在表外取。
                        for (const auto& card : kStatCards) {
                            if (!moduleRegistered(card.module)) continue;
                            switch (card.kind) {
                                case StatCardKind::Tokens: {
                                    // 周期重绘仅「有速率活动」时挂载：常挂 onTimer 会整页 2Hz 重排并打断 hover 过渡
                                    bool seriesLive = false;
                                    for (float v : sparkSeries) {
                                        if (v > 0.05f) {
                                            seriesLive = true;
                                            break;
                                        }
                                    }
                                    const bool tokenLive =
                                        snap.activeStreams > 0 ||
                                        snap.rateTps > 0.05 || seriesLive;
                                    const eui::Color* tokenNumberColor =
                                        streaming ? &t.colors.wechat.primary
                                        : rateOk  ? &t.colors.wechat.textPrimary
                                                  : &t.colors.wechat.textTertiary;
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 tokenNumber,
                                                 tokenBadge, false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 },
                                                 tokenNumberColor,
                                                 streaming ? &t.colors.wechat.primary
                                                           : nullptr,
                                                 tokenLive ? 1.0f : 0.0f,
                                                 [&](eui::Ui& inner, float chartW,
                                                     float chartH) {
                                                     // sparkline：sparkSeries 已兜底 {0.0f}（rateSeries 空序列语义，防 demo 泄漏）。
                                                     components::lineChart(
                                                         inner,
                                                         "home.stat.tokens.spark")
                                                         .size(chartW, chartH)
                                                         .values(sparkSeries)
                                                         .chromeless()
                                                         .style(components::
                                                                    LineStyle::
                                                                        Curve)
                                                         .theme(core::theme::
                                                                    componentTokensWeChat(
                                                                        t))
                                                         .build();
                                                 },
                                                 // 流末重查：数字来自落库行，用量在流末才回填；只在 1→0 那一拍查一次
                                                 [this] {
                                                     const int active =
                                                         core::proxy_stats::
                                                             snapshot()
                                                                 .activeStreams;
                                                     if (prevActiveStreams_ >
                                                             0 &&
                                                         active == 0) {
                                                         refreshLastRate();
                                                     }
                                                     prevActiveStreams_ =
                                                         active;
                                                 });
                                    break;
                                }
                                case StatCardKind::Notes:
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 num(stats_.todayNotes),
                                                 ok ? "今日修改过的笔记"
                                                    : "加载失败",
                                                 false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                                case StatCardKind::TodoPending:
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 num(stats_.pendingTodos),
                                                 ok ? "未完成总数" : "加载失败",
                                                 false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                                case StatCardKind::TodoOverdue:
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 num(stats_.overdueTodos),
                                                 ok ? "已过截止时间" : "加载失败",
                                                 stats_.overdueTodos > 0,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                                case StatCardKind::ApiCalls:
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 num(stats_.todayApiCalls),
                                                 ok ? "今日代理调用" : "加载失败",
                                                 false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                            }
                        }
                    })
                    .build();
                }

                // 今日 Token 用量分布：纵轴 = 该小时 output_tokens 合计；x 轴只画「第一个有数据的小时」
                // ..「最后一个」闭区间；整天无数据 → 空态。
                // 不用 components::barChart：它把纵值硬编码成 [0,1] 百分比语义，喂 token 合计会被夹成满格，
                // 故模块内自绘柱体（元素 id 沿用 home.api.chart.bars.*，probe/单测不失效）；无悬停 tooltip，用柱顶数值标签。
                if (moduleRegistered(::core::PageId::ClaudeProxy)) {
                    const auto tokenSeries =
                        service_ ? service_->getTodayHourlyTokenTotals(24)
                                 : core::Result<std::vector<int64_t>>::fail(
                                       core::ErrorCode::Internal,
                                       "服务未就绪");
                    std::vector<int64_t> hourly;
                    if (ok && tokenSeries.isOk()) {
                        hourly = tokenSeries.value();
                    }
                    if (hourly.empty()) hourly.assign(24, 0);
                    // 去头去尾（纯函数）：闭区间两端都是「有 token 的小时」；全零 → 空态。
                    const module::home::HourRange range =
                        module::home::hourlyDataRange(hourly);
                    const size_t barCount =
                        range.hasData ? range.last - range.first + 1 : 0;
                    int64_t peak = 0;
                    for (size_t i = 0; i < barCount; ++i) {
                        peak = std::max(peak, hourly[range.first + i]);
                    }
                    // 图表卡高度：固定标称高；矮窗装不下时整页由外层 home.scroll 吸收溢出。
                    const float chartCardH = kHomeChartH;
                    const float chartWrapH = chartCardH - 48.0f - 16.0f;  // 196
                    components::card(ui, "home.api.chart")
                        .size(contentW, chartCardH)
                        .theme(core::theme::componentTokensWeChat(t))  // W9 微信白卡
                        .radius(t.sizes.radiusWeChat)
                        .padding(t.sizes.spacingLarge)  // 16 统一内边距（替代默认 20）
                        .content([&] {
                            ui.text("home.api.chart.title")
                                .position(0.0f, 16.0f)
                                .size(contentW - 32.0f, 22.0f)
                                .text("今日 Token 用量分布（每小时输出 token 合计）")
                                .fontSize(t.sizes.fontSizeLarge - 1.0f)
                                .lineHeight(22.0f)
                                .fontWeight(600)
                                .color(t.colors.wechat.textPrimary)
                                .build();
                            // 图表区：自绘柱体包一层定位 stack（沿用 wrap id，probe 按它量左右对称）。
                            ui.stack("home.api.chart.wrap")
                                .position(0.0f, 48.0f)
                                .size(contentW - 32.0f, chartWrapH)
                                .content([&] {
                                    const float wrapW = contentW - 32.0f;
                                    if (!range.hasData) {
                                        // 空态：整天没数据不画空轴也不画假条，居中一句引导。
                                        ui.text("home.api.chart.empty")
                                            .position(
                                                0.0f,
                                                (chartWrapH - 40.0f) * 0.5f)
                                            .size(contentW - 32.0f, 40.0f)
                                            .text("今日暂无 API 调用")
                                            .fontSize(t.sizes.fontSizeBody)
                                            .lineHeight(40.0f)
                                            .horizontalAlign(
                                                eui::HorizontalAlign::Center)
                                            .color(t.colors.wechat.textTertiary)
                                            .build();
                                    } else {
                                        // 柱体容器（id 沿用组件根 id：probe 与单测按 home.api.chart.bars.* 定位）。
                                        ui.stack("home.api.chart.bars")
                                            .position(0.0f, 0.0f)
                                            .size(wrapW, chartWrapH)
                                            .content([&] {
                                        // 自绘几何：常量逐项取自 barchart.h（改这里 = 改图表观感）。
                                        const float plotX = 32.0f;
                                        const float plotY = 70.0f;
                                        const float plotW =
                                            std::max(1.0f, wrapW - 64.0f);
                                        const float plotH =
                                            std::max(1.0f, chartWrapH - 112.0f);
                                        const float bottomY = plotY + plotH;
                                        const float slotW =
                                            plotW / static_cast<float>(barCount);
                                        const float barW = std::min(
                                            32.0f,
                                            std::max(18.0f, slotW * 0.54f));
                                        // chromeless：网格线用主题描边色的低透明版，无第二底、无边框。
                                        components::theme::ThemeColorTokens
                                            chartTokens =
                                                core::theme::componentTokensWeChat(t);
                                        for (int line = 0; line < 4; ++line) {
                                            ui.rect("home.api.chart.bars.grid." +
                                                    std::to_string(line))
                                                .x(plotX)
                                                .y(plotY + static_cast<float>(
                                                               line) *
                                                               plotH / 3.0f)
                                                .size(plotW, 1.0f)
                                                .color(components::theme::
                                                           withOpacity(
                                                               chartTokens.border,
                                                               0.36f))
                                                .build();
                                        }
                                        for (size_t i = 0; i < barCount; ++i) {
                                            const int64_t v =
                                                hourly[range.first + i];
                                            // 柱高按区间峰值归一；0 token 的小时只剩 1px 底线，不画假柱。
                                            const float ratio =
                                                peak > 0
                                                    ? static_cast<float>(v) /
                                                          static_cast<float>(
                                                              peak)
                                                    : 0.0f;
                                            const float barH =
                                                std::max(1.0f, ratio * plotH);
                                            const float bx =
                                                plotX +
                                                static_cast<float>(i) * slotW +
                                                (slotW - barW) * 0.5f;
                                            const float by = bottomY - barH;
                                            ui.rect(
                                                "home.api.chart.bars.bar." +
                                                std::to_string(i))
                                                .x(bx)
                                                .y(by)
                                                .size(barW, barH)
                                                // 色取 barchart.h 默认 palette[0]
                                                .color(components::theme::color(
                                                    0.22f, 0.50f, 0.88f))
                                                .radius(std::min(
                                                    t.sizes.radius,
                                                    barW * 0.34f))
                                                .build();
                                            // 柱顶数值 = 该小时 token 合计
                                            ui.text(
                                                "home.api.chart.bars.val." +
                                                std::to_string(i))
                                                .x(bx - 12.0f)
                                                .y(by - 14.0f)
                                                .size(barW + 24.0f, 14.0f)
                                                .text(formatTokenCount(v))
                                                .fontSize(10.0f)
                                                .lineHeight(14.0f)
                                                .horizontalAlign(
                                                    eui::HorizontalAlign::Center)
                                                .color(t.colors.wechat.textSecondary)
                                                .build();
                                            // x 轴小时标签：去头去尾后第 i 根对应真实小时 range.first + i。
                                            ui.text(
                                                "home.api.chart.bars.label." +
                                                std::to_string(i))
                                                .x(bx - 12.0f)
                                                .y(chartWrapH - 22.0f)
                                                .size(barW + 24.0f, 16.0f)
                                                .text(std::to_string(
                                                          range.first + i) +
                                                      "时")
                                                .fontSize(10.0f)
                                                .lineHeight(16.0f)
                                                .horizontalAlign(
                                                    eui::HorizontalAlign::Center)
                                                .color(t.colors.wechat.textTertiary)
                                                .build();
                                        }
                                            })
                                            .build();
                                    }
                                })
                                .build();
                        })
                        .build();
                }

                // 快捷操作按钮组：目标模块关闭 → 按钮隐藏；UAT2 覆盖改等宽。
                int actCount = 0;
                for (const auto& btn : kActionButtons) {
                    if (moduleRegistered(btn.module)) ++actCount;
                }
                if (actCount > 0) {
                const float actGap = t.sizes.spacingLarge; // 16
                // UAT2 覆盖 W9 主次不等宽：三按钮权重全 1.0（actWeightSum=actCount）
                ui.row("home.actions")
                    .size(contentW, 44.0f)
                    .gap(actGap)
                    .content([&] {
                        // W9 微信风格：新建笔记绿主按钮，其余浅灰次按钮（无边框）；UAT2 起等宽
                        for (const auto& btn : kActionButtons) {
                            if (!moduleRegistered(btn.module)) continue;
                            const float actW =
                                (contentW -
                                 actGap * static_cast<float>(actCount - 1)) /
                                static_cast<float>(actCount);
                            auto builder = components::button(
                                ui, actionButtonElementId(btn.kind));
                            builder.size(actW, 44.0f)
                                .text(btn.label)
                                .fontSize(t.sizes.fontSizeBody)
                                .radius(t.sizes.radiusWeChat);
                            if (btn.kind == ActionKind::NewNote) {
                                builder.colors(t.colors.wechat.primary,
                                               t.colors.wechat.primaryHover,
                                               t.colors.wechat.primaryPressed)
                                    .textColor(t.colors.wechat.onPrimary);
                            } else {
                                builder.colors(
                                       t.colors.wechat.secondaryButtonBg,
                                       t.colors.wechat.dividerSecondary,
                                       t.colors.wechat.dividerSecondary)
                                    .textColor(t.colors.wechat.textPrimary);
                            }
                            if (btn.iconCodepoint != 0) {
                                builder.icon(btn.iconCodepoint)  // 0xF46D fa-clipboard-list
                                    .iconColor(t.colors.wechat.textPrimary);
                            }
                            // 点击负载按值捕获（btn.navPayload 空串 = 无负载），闭包只活在 compose 调用期。
                            const std::string payload = btn.navPayload;
                            builder
                                .onClick([this, payload,
                                          target = btn.target] {
                                    navigateTo(target, payload);
                                })
                                .build();
                        }
                    })
                    .build();
                }  // if (actCount > 0)
            })   // home.root 的 content
            .build();  // home.root
    })  // home.scroll 的 content（收尾多这一层）
        .build();  // home.scroll
    }

private:
    static constexpr int kRecentNoteLimit = 5;

    // ① 的取数结果（空态/失败态靠 hasValue + lastRateError_ 区分）。
    using LastCallRate = module::home::LastCallRate;

    void navigateTo(::core::PageId page, const std::string& payload = "") {
        auto* router = ::core::appRouter();
        if (router == nullptr) {
            LOG_WARN("Home: Router 未注册，无法跳转到 " +
                     std::string(::core::pageIdToString(page)));
            return;
        }
        router->navigateTo(page, payload);
    }

    std::unique_ptr<HomeService> service_;
    DashboardStats stats_;
    bool statsLoaded_ = false;
    std::string statsError_;
    LastCallRate lastRate_;
    std::string lastRateError_;   // 非空 = 最近速率查询失败（卡显示「加载失败」）
    // 上一拍看到的活跃流数（只有 Token 卡的 timer 回调写它）：1→0 那一拍 = 流式刚结束 → 重查
    int prevActiveStreams_ = 0;
};

} // namespace module::home

extern "C" ::core::Module* create_home_module() {
    return new module::home::HomeModule();
}
