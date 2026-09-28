// src/module_home/home_module.cpp
// Home 仪表盘模块 - 真实数据（HomeService，spec §8.4）+ 快捷按钮接线
//
// 数据：markdown_index / todo_tasks / claude_call_logs 只读聚合，
// 经 core::appDatabase() 拿共享连接（与 Markdown/Todo 模块同一装配模式）。
// 导航：core::appRouter()（main.cpp 注册），module_* 之间互不依赖；
// 跨模块动作经 Router payload 下发（"action:*" / "open:*"），由目标页消费。
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
#include <components/barchart.h>
#include <components/card.h>
#include <components/linechart.h>

#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace module::home {

namespace {

// 数字字号按位数收缩（2026-09-28 审计 #4 text-overflow）：700 字重数字
// ≈0.60em/位；可用宽不足时降号（下限 24 fontScale 档），上限 base（已含
// fontScale）。只缩字号，行高/垂直节奏不动。
float statNumberFont(float base, int digits, float availWidth, float scale) {
    if (digits <= 0) return base;
    const float want =
        std::floor(availWidth / (0.60f * static_cast<float>(digits)) / scale);
    return std::clamp(want * scale, 24.0f * scale, base);
}

int digitCount(const std::string& text) {
    int n = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') ++n;
    }
    return n;
}

// 渲染单个统计卡片（card 组件，点击整卡跳转）
// numberColor/badgeColor 非空时覆盖默认配色（accent ? error : 常规）
// timerSeconds > 0 时挂一个 0 尺寸 onTimer 元素驱动本卡定期重绘
// chart 非空时在卡内叠加迷你图表（sparkline 等），由调用点提供渲染 lambda
//
// M3（2026-09-28）：手绘卡换 card 组件。保留：accent/numberColor/badgeColor
// 语义色与 timerSeconds 重绘驱动。card 组件无 hover states（CardStyle 无
// states 字段）→ hover 反馈由卡内交互层 .press 补（states 三态 + 0.1s
// OutQuad 过渡）；.press 位 content 首层——card 的 bg 不可交互，非交互
// 文本点击会穿透命中 .press（与旧 .bg onClick 同命中语义）。
// 内边距口径（2026-09-28 系统性修复）：卡内容四周统一 16px（16px 网格）
// ——显式 .padding(inset) 替代组件默认 inset 20。旧写法吃默认 20 又与
// 手绘残留坐标（label 20/42/98）叠加成「顶 40 留空、badge 底 134 > 卡高
// 120 溢出贴边」；当前 y 节奏 = 16 顶内边距 + label 16 + 8 间距 + number
// 56 + badge 16 + 16 底内边距（badge 底对齐 height-48 恒保 16 底边距）。
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
                      nullptr) {
    const auto ct = core::theme::componentTokens(t);
    const float s = core::theme::fontScale();
    const float inset = t.sizes.spacingLarge; // 16：卡内容统一内边距（16px 网格）
    const float cw = width - inset * 2.0f;
    const float ch = height - inset * 2.0f;
    components::card(ui, id)
        .size(width, height) // panelShadow 由 CardStyle 默认注入（§M3-4 复查）
        .theme(ct)           // 先注入主题再覆盖 radius（theme 会重置 CardStyle）
        .radius(t.sizes.radiusLarge)
        .padding(inset)      // 显式 16：不用组件默认 20（与旧坐标叠加致顶留空/底溢出）
        .content([&] {
            ui.stack(id + ".inner")
                .size(cw, ch)
                .content([&] {
                    // 交互层（点击 + hover 过渡；非交互文本穿透命中）
                    ui.rect(id + ".press")
                        .size(cw, ch)
                        .states(components::theme::color(0.0f, 0.0f, 0.0f,
                                                         0.0f),
                                t.colors.hover, t.colors.active)
                        .transition(core::Transition::make(0.1f,
                                                           core::Ease::OutQuad))
                        .animate(core::AnimProperty::Color)
                        .onClick(std::move(onClick))
                        .build();
                    // 卡内文字：坐标相对 card content 原点（已含 16 inset），
                    // 四周统一 16px 留白（label 16 / number 40 / badge 底对齐）
                    ui.text(id + ".label")
                        .position(0.0f, 16.0f * s)
                        .size(cw, 16.0f * s)
                        .text(label)
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(16.0f * s)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(t.colors.textSecondary)
                        .build();
                    // 数字字号随位数收缩（6 位以上按可用宽降号，最小窗
                    // statW 208 → cw 176 时 6 位亦不溢卡）
                    const float numberSize = statNumberFont(
                        t.sizes.fontSizeDisplay, digitCount(number), cw, s);
                    ui.text(id + ".number")
                        .position(0.0f, 32.0f * s)
                        .size(cw, 56.0f * s)
                        .text(number)
                        .fontSize(numberSize)
                        .lineHeight(56.0f * s)
                        .fontWeight(700)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(numberColor ? *numberColor
                                           : (accent ? t.colors.error
                                                     : t.colors.textPrimary))
                        .build();
                    ui.text(id + ".badge")
                        // 底对齐 height-48（= 16 inset + 16 badge + 16 底边距），
                        // 任何字号档底部都不贴卡边
                        .position(0.0f, height - 48.0f)
                        .size(cw, 16.0f)
                        .text(badge)
                        .fontSize(t.sizes.fontSizeSmall - 2.0f)
                        .lineHeight(16.0f)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(badgeColor ? *badgeColor
                                          : (accent ? t.colors.error
                                                    : t.colors.textTertiary))
                        .build();
                    if (timerSeconds > 0.0f) {
                        // 保留：0.5s 周期整卡重绘（流式速率实时刷新，设计
                        // §2.6——compose 不是每帧重跑，靠 onTimer 触发）
                        ui.stack(id + ".timer")
                            .size(0.0f, 0.0f)
                            .onTimer(timerSeconds, [] {})
                            .build();
                    }
                    if (chart) {
                        // 2026-09-28 审计 #1 z-collision 修：sparkline 固定进
                        // 顶带（内 y0..16 = label 16 之上），整盒高 16——旧 32
                        // 盒叠 (0,0) 压住 label 文字带，图线画在文字上层
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

// 模块注册感知（插拔方案 A §5.5）：按 Router 注册状态隐藏对应卡片/按钮，
// 不显示陈旧数据、不显示故障态。appRouter() 为 null（无头测试）视为全启用
// ——与模块既有 null-guard 行为一致
bool moduleRegistered(::core::PageId page) {
    auto* router = ::core::appRouter();
    return router == nullptr || router->isRegistered(page);
}

// ===== THS-08：统计卡 / 快捷按钮元数据表 =====
// 两张 static 纯元数据表（审计 #9：表不落 lambda/可调用列——static 表带可调用
// 列在首次初始化只求值一次，捕获引用会冻结甚至悬垂（UB））。数值/副标题
// 文案在 compose 逐行查表外取（随 stats_ 实时，不落表）；行序 = 渲染序；
// statCount/actCount 由表行数按归属模块注册态过滤派生——新增/关闭模块改
// 表即改计数，宽度（statW/actW）不再靠手算（宽度公式本身不变）。

// 统计卡（现 5 卡，claude 2 卡 = 2 行）：kind = 行身份（compose 侧按它取
// 数值/副标题/配色/图表），module = 归属模块（注册态过滤 = 旧
// claudeOn/mdOn/todoOn 判定的一次化），target = 点击跳转页
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

// 快捷按钮（现 3 钮）：kind = 行身份，module = 归属模块，label = 文案，
// iconCodepoint = FA7 码点（0 = 无图标），target + navPayload = 点击负载
//（Router navigateTo(page, payload)；payload 空串 = 无负载）
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

// 元素 id 由 kind 唯一决定（test_home / test_module_gating / test_module_icons
// 锁定这些 id，行序重排不飘）
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

    // ==================== 生命周期 ====================

    void onStart() override {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            LOG_WARN("Home 模块启动：appDatabase() 为空，仪表盘显示空态");
            // 补齐错误文案：否则 compose 拼出「数据加载失败：（空串）」
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

    // 进页重查（W7）：数据只在启动查一次，从 Todo/Markdown 切回来数字
    // 不更新——Router::navigateTo 进页通知驱动重查
    void onPageEnter() override { refreshStats(); }

    // ==================== 数据 ====================

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
    }

    // ==================== 渲染 ====================

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;       // 40
        const float gap = t.sizes.spacingLarge * 1.25f; // 20
        const float contentW = screen.width - pad * 2.0f;
        // 审计 M6：header 右侧按钮组宽（标题列 = contentW - 本值）——header 行
        // gap=0，两段必须相加等于 contentW；字面量写死两处改一处忘一处就右缘错位
        constexpr float kHeaderActionsW = 212.0f;
        const float cardGap = 20.0f;
        // 卡高随字号缩放派生（卡内 y 布局同源缩放，大字号不裁切）。
        // 144 = 16 顶内边距 + 16 标签 + 8 间距 + 56 数字 + 16 badge + 16 底内边距
        //（badge 底对齐 height-48，见 drawStatCard；旧 120 无底边距致 badge 贴边/溢出）
        const float statH = 144.0f * core::theme::fontScale();
        const std::time_t now = std::time(nullptr);

        // 模块注册感知（插拔方案 A §5.5）：卡片/按钮按目标模块注册状态显隐。
        // THS-08：统计卡/快捷按钮的归属模块判定已表驱动（逐行
        // moduleRegistered(card.module)）；最近编辑卡已删除（UI 整改）——
        // 本文件不再有模块布尔判定点
        // 统计卡可见数从元数据表派生（按归属模块注册态过滤）——新增/
        // 关闭模块改表即改计数，宽度（statW）均分不再靠手算；全关整行隐藏
        int statCount = 0;
        for (const auto& card : kStatCards) {
            if (moduleRegistered(card.module)) ++statCount;
        }

        // 页面底色 = 主窗口 clearColor（applyWindowVisuals 随主题/窗口效果档
        // 运行时跟随，磨砂 Phase B/D）——不再自铺不透明底，否则半透/磨砂档
        // 下页面 alpha 恒为 1，主体看不见效果（用户实测主窗主体无效果的根因）

        ui.column("home.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(gap)
            .content([&] {
                // ========== 顶部欢迎区 ==========
                ui.row("home.header")
                    .size(contentW, 72.0f)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        // 标题列宽 = contentW - 右侧按钮组宽 212（对偶写法，同
                        // Todo 560/560、Claude 360/360）：旧 240 让整行比下方满宽
                        // 卡内缩 28px，右缘不齐（home.header 行 gap=0，两段和=contentW）
                        // UI 整改（2026-09-28）：欢迎区不再垫渐变底——浅色主题
                        // surface==surfaceElevated==#FFFFFF，白→白渐变 = 纯白通栏卡，
                        // 即「首页外层白底卡」；文案直接铺页面背景
                        ui.column("home.greeting")
                            .size(std::max(280.0f, contentW - kHeaderActionsW), 72.0f)
                            .gap(6.0f)
                            .content([&] {
                                ui.text("home.h1")
                                    .size(std::max(280.0f, contentW - kHeaderActionsW), 40.0f)
                                    .text("欢迎回到 DevDesk")
                                    .fontSize(t.sizes.fontSizePage)
                                    .lineHeight(40.0f)
                                    .fontWeight(700)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("home.date")
                                    .size(std::max(280.0f, contentW - kHeaderActionsW), 18.0f)
                                    .text("今天是 " + formatTodayCn(now))
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧按钮组
                        ui.row("home.header.actions")
                            .size(kHeaderActionsW, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                components::button(ui, "home.btn.refresh")
                                    .size(100.0f, 40.0f)
                                    .text("刷新")
                                    .icon(0xF021)  // fa-arrows-rotate
                                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                                    .colors(t.colors.surfaceElevated,
                                            t.colors.hover, t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .iconColor(t.colors.textSecondary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { refreshStats(); })
                                    .build();

                                // 设置页已落地（P13/T11），直接导航
                                components::button(ui, "home.btn.settings")
                                    .size(100.0f, 40.0f)
                                    .text("设置")
                                    .icon(0xF013)  // fa-gear
                                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                                    .colors(t.colors.surfaceElevated,
                                            t.colors.hover, t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .iconColor(t.colors.textSecondary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] {
                                        navigateTo(::core::PageId::Settings);
                                    })
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 5 个统计卡片（真实数据 + Token 速率实时卡） ==========
                const bool ok = statsLoaded_;
                auto num = [&ok](int64_t v) {
                    return ok ? std::to_string(v) : std::string("--");
                };

                // Token 速率卡三态（设计 §4 方案 A）：
                //   流式中 = primary 色 + success 绿 badge；空闲/刚结束 = 灰，
                //   速率随 2s 窗口自然衰减到 0，累计定格保留
                const core::proxy_stats::Snapshot snap =
                    core::proxy_stats::snapshot();
                const bool streaming = snap.activeStreams > 0;
                const std::string sessionCn =
                    formatTokenCount(snap.sessionTokens);
                // W9：badge 文案不带「会话」二字——em 修复后 11px 全角文案
                // 在最小窗口（statW 208）下会溢出卡片（实测 154.85 > 152 可用宽）
                // 2026-09-28 审计 #2：并发措辞「路并发」→「路」（省 2 个全角，
                // 最长档「● 999 路 · 累计 999.9M」实测 ~111px ≤ 144 可用宽）
                const std::string tokenBadge =
                    streaming
                        ? "● " + std::to_string(snap.activeStreams) +
                              " 路 · 累计 " + sessionCn
                        : "空闲 · 累计 " + sessionCn;

                if (statCount == 0) {
                    // 三模块全关的空态兜底（§5.5）：欢迎区保留 + 一句引导
                    ui.text("home.stats.alloff")
                        .size(contentW, 32.0f)
                        .text("所有工具模块已关闭，可在设置中开启")
                        .fontSize(t.sizes.fontSizeBody)
                        .lineHeight(32.0f)
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                const float statW =
                    (contentW - cardGap * static_cast<float>(statCount - 1)) /
                    static_cast<float>(statCount);
                // sparkline 序列只取一次（live 判定与图表绘制共用；
                // rateSeries 空序列兜底 {0.0f}，D-6 护栏）
                const std::vector<float> sparkSeries =
                    core::proxy_stats::rateSeries(24);
                ui.row("home.stats")
                    .size(contentW, statH)
                    .gap(cardGap)
                    .content([&] {
                        // THS-08：逐表行渲染（行序 = 渲染序；元素 id 由 kind
                        // 决定，test_home/gating/icons 锁定）；数值/副标题/配色/
                        // 图表按行身份在表外取，不落表
                        for (const auto& card : kStatCards) {
                            if (!moduleRegistered(card.module)) continue;
                            switch (card.kind) {
                                case StatCardKind::Tokens: {
                                    // 周期重绘仅「有速率活动」时挂载
                                    // （2026-09-28 根因修：旧 0.5s 常挂
                                    // onTimer → 整页 2Hz 重排重绘 + 常驻
                                    // animating_ → 5 卡 hover 0.1s 过渡被
                                    // 反复打断闪烁；空闲静默后过渡完整跑完、
                                    // hover 命中缓存恢复有效）
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
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 formatRateTps(snap.rateTps),
                                                 tokenBadge, false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 },
                                                 streaming ? &t.colors.primaryText
                                                           : &t.colors.textTertiary,
                                                 streaming ? &t.colors.success
                                                           : nullptr,
                                                 tokenLive ? 1.0f : 0.0f,
                                                 [&](eui::Ui& inner, float chartW,
                                                     float chartH) {
                                                     // sparkline：sparkSeries
                                                     // 已兜底 {0.0f}（rateSeries
                                                     // 空序列语义，D-6 护栏防
                                                     // lineChart demo 泄漏）
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
                                                                    componentTokens(
                                                                        t))
                                                         .build();
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

                // ========== 今日 API 调用分布（UI 现代化 M3-1 剩余：API barChart） ==========
                // barChart 组件按整图硬几何设计（plot 区 ≥ ~112px、barchart.h
                // build 内 plotY=70/title 区），统计卡内 32px 迷你槽放不下 →
                // 独立整卡呈现（plan M3-1 Step 3 的适配口径：「以 barchart.h
                // 为准适配，不新建组件」）。数据 = HomeService::getTodayApiCallSeries
                // （24 点当日逐时分布；恒 24 点 → 绝不空，barChart 空 values
                // 会填演示数据）；barChart 值域 [0,1]（percent 语义）→ 按当日
                // 峰值归一化。加载失败（!ok）→ 全 0（统计卡 badge 已示失败）。
                // 2026-09-28 审计 #3/#4/#5：barChart 内嵌化（透明底/边框/阴影
                // 样式消第二底，不动 barchart.h）；卡高 208→260（wrap 128→196，
                // plot 16px→84px，不再出 8px 地板假条）；全零画空态不画假条；
                // title/wrap 去二次 16（content 坐标已含 inset，左 32 右 0 不对称）
                if (moduleRegistered(::core::PageId::ClaudeProxy)) {
                    const auto apiSeries =
                        service_ ? service_->getTodayApiCallSeries(24)
                                 : core::Result<std::vector<int64_t>>::fail(
                                       core::ErrorCode::Internal,
                                       "服务未就绪");
                    std::vector<float> bars;
                    if (ok && apiSeries.isOk()) {
                        bars.reserve(apiSeries.value().size());
                        for (const int64_t v : apiSeries.value()) {
                            bars.push_back(static_cast<float>(v));
                        }
                    }
                    if (bars.empty()) bars.assign(24, 0.0f);
                    float maxV = 1.0f;
                    bool allZero = true;
                    for (const float bv : bars) {
                        maxV = std::max(maxV, bv);
                        if (bv > 0.0f) allZero = false;
                    }
                    std::vector<float> norm;
                    norm.reserve(bars.size());
                    for (const float bv : bars) norm.push_back(bv / maxV);
                    std::vector<std::string> labels;
                    labels.reserve(bars.size());
                    for (size_t h = 0; h < bars.size(); ++h) {
                        labels.push_back(std::to_string(h) + "时");
                    }
                    const float chartCardH = 260.0f;
                    const float chartWrapH = chartCardH - 48.0f - 16.0f;  // 196
                    components::card(ui, "home.api.chart")
                        .size(contentW, chartCardH)
                        .theme(core::theme::componentTokens(t))
                        .padding(t.sizes.spacingLarge)  // 16 统一内边距（替代默认 20）
                        .content([&] {
                            ui.text("home.api.chart.title")
                                .position(0.0f, 16.0f)
                                .size(contentW - 32.0f, 22.0f)
                                .text("今日 API 调用分布（按当日峰值归一）")
                                .fontSize(t.sizes.fontSizeLarge - 1.0f)
                                .lineHeight(22.0f)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            // barChart 无 position 接口 → 包定位 stack
                            // （title 置空：本卡自带标题，避免组件内默认
                            // 「BarChart」字样）
                            ui.stack("home.api.chart.wrap")
                                .position(0.0f, 48.0f)
                                .size(contentW - 32.0f, chartWrapH)
                                .content([&] {
                                    if (allZero) {
                                        // 空态（审计 #4）：全零不画 8px 地板
                                        // 假条，居中一句引导
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
                                            .color(t.colors.textTertiary)
                                            .build();
                                    } else {
                                        // 审计 #3：内嵌 chromeless 样式——
                                        // 透明底/透明边框/禁阴影，消除与外层
                                        // 卡同色但带边框阴影的「第二底」
                                        components::theme::ThemeColorTokens
                                            chartTokens =
                                                core::theme::componentTokens(t);
                                        components::BarChartStyle chartStyle(
                                            chartTokens);
                                        chartStyle.background =
                                            components::theme::color(0.0f, 0.0f,
                                                                     0.0f, 0.0f);
                                        chartStyle.border =
                                            components::theme::color(0.0f, 0.0f,
                                                                     0.0f, 0.0f);
                                        chartStyle.shadow = core::Shadow{};
                                        components::barChart(
                                            ui, "home.api.chart.bars")
                                            .size(contentW - 32.0f, chartWrapH)
                                            .title("")
                                            .values(norm)
                                            .labels(labels)
                                            .theme(chartTokens)
                                            .style(chartStyle)
                                            .build();
                                    }
                                })
                                .build();
                        })
                        .build();
                }

                // ========== 快捷操作按钮组（Router payload 跨模块指令）==========
                // 目标模块关闭 → 按钮隐藏，宽度按剩余数量均分；全关整行隐藏
                //（THS-08：actCount 从按钮表行数按注册态过滤派生）
                int actCount = 0;
                for (const auto& btn : kActionButtons) {
                    if (moduleRegistered(btn.module)) ++actCount;
                }
                if (actCount > 0) {
                const float actGap = t.sizes.spacingLarge; // 16
                const float actW =
                    (contentW - actGap * static_cast<float>(actCount - 1)) /
                    static_cast<float>(actCount);
                ui.row("home.actions")
                    .size(contentW, 44.0f)
                    .gap(actGap)
                    .content([&] {
                        // THS-08：逐表行渲染（文案/图标/点击负载全在表内；
                        // 样式按行身份取——「+ 新建笔记」主色按钮，其余
                        // 浅底描边）。目标模块关闭 → 本行不渲染
                        for (const auto& btn : kActionButtons) {
                            if (!moduleRegistered(btn.module)) continue;
                            auto builder = components::button(
                                ui, actionButtonElementId(btn.kind));
                            builder.size(actW, 44.0f)
                                .text(btn.label)
                                .fontSize(t.sizes.fontSizeBody)
                                .radius(t.sizes.radius);
                            if (btn.kind == ActionKind::NewNote) {
                                builder.colors(t.colors.primary,
                                               t.colors.primaryHover,
                                               t.colors.primaryPressed)
                                    .textColor(t.colors.onPrimary);
                            } else {
                                builder.colors(t.colors.surfaceElevated,
                                               t.colors.hover,
                                               t.colors.active)
                                    .textColor(t.colors.textPrimary)
                                    .border(1.0f, t.colors.borderDefault);
                            }
                            if (btn.iconCodepoint != 0) {
                                builder.icon(btn.iconCodepoint)  // 0xF46D fa-clipboard-list
                                    .iconColor(t.colors.textPrimary);
                            }
                            // 点击负载（btn.navPayload 空串 = 无负载）按值捕获，
                            // 闭包只活在 compose 调用期（表本身无 lambda 列）
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
            })
            .build();
    }

private:
    static constexpr int kRecentNoteLimit = 5;

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
};

} // namespace module::home

extern "C" ::core::Module* create_home_module() {
    return new module::home::HomeModule();
}
