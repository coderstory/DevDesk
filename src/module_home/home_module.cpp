// src/module_home/home_module.cpp
// Home 仪表盘模块 - 真实数据（HomeService，spec §8.4）+ 快捷按钮接线
//
// 数据：markdown_index / todo_tasks / claude_call_logs 只读聚合，
// 经 core::appDatabase() 拿共享连接（与 Markdown/Todo 模块同一装配模式）。
// 导航：core::appRouter()（main.cpp 注册），module_* 之间互不依赖；
// 跨模块动作经 Router payload 下发（"action:*" / "open:*"），由目标页消费。
#include "core/call_rate.h"
#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/theme.h"
#include "home_quota_card.h"
#include "home_service.h"
#include "quota_home_bridge.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/card.h>
#include <components/linechart.h>
#include <components/scrollview.h>
#include <components/theme.h>

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
// onTick 非空时作为该 timer 的回调（默认空实现——只有需要「按拍做事」的卡
// 才传，见 Token 卡的流末重查）
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
                      nullptr,
                  std::function<void()> onTick = nullptr) {
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
                        .radius(t.sizes.radiusLarge)
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
                        // §2.6——compose 不是每帧重跑，靠 onTimer 触发）。
                        // onTick 非空时用它（Token 卡靠它感知「流式刚结束」）
                        ui.stack(id + ".timer")
                            .size(0.0f, 0.0f)
                            .onTimer(timerSeconds,
                                     onTick ? std::move(onTick)
                                            : std::function<void()>([] {}))
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
        refreshLastRate();
    }

    // ① 最近一次成功调用的速率（独立于 getStats：它的失败不该把整页统计
    // 拖成「加载失败」，但也不能静默——查询失败单独记 lastRateError_，
    // Token 卡据此显示「加载失败」而不是假装空态「暂无成功调用」）
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

    // ==================== 渲染 ====================

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;       // 40
        const float gap = t.sizes.spacingLarge * 1.25f; // 20
        // ⚠ contentW **不在这里定义**：整页已挂进 home.scroll，内容宽取决于
        // scrollView 是否出了滚动条（出了就收窄 scrollbarWidth + gap），只有
        // scrollView 的 content 回调拿到的 innerW 说了算——见下方 home.scroll
        // 段的同名遮蔽定义。写死 screen.width 版会横向溢出。
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

        // ---- 首页外层滚动容器（2026-10-03 final 批，G7 遗留 1 / G8 遗留 1）----
        //
        // 为什么是「整页包一层」而不是「只把余量卡塞进 scrollView」：矮窗下
        // 真正装不下的是**整页**（不含余量卡就已超客户区），只给卡片一个滚动
        // 容器解决不了这个问题——那会让卡片占满视口，把欢迎区/统计/图表/快捷
        // 按钮全挤走，观感更差。整页包一层后，超出的部分由滚动条吸收，各块
        // 几何在「页面装得下」的屏高区间**逐字不变**（scrollView 内容不溢出
        // 时连滚动条都不出现）。
        //
        // ⚠ 上游坑（markdown_module.cpp 同款，已核 EUI-NEO core/layout.h）：
        //   `layoutColumn()` 算子元素位置时**只看流式游标**（layout.h:605-611），
        //   只有 `layoutStack()` 才认 `hasX_/hasY_`（layout.h:622-623）——即
        //   **position() 挂在 column 的流式子元素上会被静默忽略**。所以下面
        //   home.root 的直接子块（header / stats / quota 卡 / 图表卡 / actions）
        //   一律不用 position()，只用 flow + size()；用 position() 的只有卡
        //   **内部**的元素（它们是 card 自己的 stack 子节点，不受影响）。
        //
        // 形参 `ui` **刻意与外层同名**：一旦整页被 scrollView 的 content 回调
        // 包住，下面这 600 行整页代码一行都不用改就换了个容器（遮蔽外层 ui）。
        // `innerW` 才是内容宽——出滚动条时 scrollView 会把内容列收窄
        // （scrollbarWidth + gap），用 screen.width 会让 home.root 横向溢出。
        components::scrollView(ui, "home.scroll")
            .size(screen.width, screen.height)
            .theme(core::theme::componentTokens(t))
            .content([&](eui::Ui& ui, float innerW, float) {
            // ⚠ 遮蔽外层的 contentW（screen.width 版）：出滚动条时内容列变窄，
            // 必须跟着 innerW 走，否则横向溢出（markdown 的 scrollView 同款口径）。
            const float contentW = innerW - pad * 2.0f;
        ui.column("home.root")
            // 高度用 wrapContent：**固定高度会让 scrollView 的测量 pass 永远
            // 量到「正好等于视口」→ contentHeight 恒等于视口 → 永远不判可滚**
            // （core/layout.h measureColumnContent 按子元素实测高求和，固定高
            // 会把 wrapContent 那层测量吃掉）。
            .width(innerW)
            .height(core::SizeValue::wrapContent())
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
                                    .text("欢迎回来～")
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

                // Token 速率卡（真机反馈 ①，2026-09-29 改口径）：
                //   数字 = **最近一次成功调用**的速率 = output_tokens /
                //   (latency_ms / 1000)（口径单源 core/call_rate.h，与 Claude
                //   详情页「最近调用」的速率列同一判定）；没有成功调用 →
                //   空态「--」+ 副标题「暂无成功调用」。
                //   流式实时值（2s 窗口 rateTps / 实时序列）仍在，但只喂卡内
                //   sparkline —— 曲线语义本来就是「刚才几秒的速率」，与卡片
                //   数字（单次调用口径）不是一回事，故保留。
                //   配色沿用旧三态：流式中 primary 数字 + success 绿 badge。
                const core::proxy_stats::Snapshot snap =
                    core::proxy_stats::snapshot();
                const bool streaming = snap.activeStreams > 0;
                // 「加载失败」= 整页统计没加载成功（未 onStart / DB 不可用 /
                // getStats 失败）**或**最近速率查询自己失败——不把「未加载」
                // 冒充成「暂无成功调用」（同 notes 卡的 ok 口径）
                const bool rateFailed =
                    !statsLoaded_ || !lastRateError_.empty();
                const bool rateOk = !rateFailed && lastRate_.hasValue;
                const std::string tokenNumber =
                    rateOk ? core::call_rate::formatTps(lastRate_.tps)
                           : std::string("--");
                // W9：badge 文案不带「会话」二字——em 修复后 11px 全角文案
                // 在最小窗口（statW 208）下会溢出卡片（实测 154.85 > 152 可用宽）
                // 2026-09-28 审计 #2：并发措辞「路并发」→「路」
                // 2026-09-29 ①：**不再写「累计」**——累计 token 与新口径无关。
                // 副标题分档（数字是「最近一次」时必须说出来）：
                //   流式中 → 「● N 路生成中」+（数字有值时）「 · 最近一次」；
                //            数字为空态（整页没加载 / 没有成功调用）时不缀
                //            「最近一次」，免得承诺一个并不存在的值
                //   空闲   → 「最近一次成功调用」/「暂无成功调用」/「加载失败」
                const std::string tokenBadge =
                    streaming
                        ? "● " + std::to_string(snap.activeStreams) +
                              " 路生成中" + (rateOk ? " · 最近一次" : "")
                        : (rateFailed
                               ? "加载失败"
                               : (rateOk ? "最近一次成功调用"
                                         : "暂无成功调用"));

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
                                    const eui::Color* tokenNumberColor =
                                        streaming ? &t.colors.primaryText
                                        : rateOk  ? &t.colors.textPrimary
                                                  : &t.colors.textTertiary;
                                    drawStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, card.title,
                                                 tokenNumber,
                                                 tokenBadge, false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 },
                                                 tokenNumberColor,
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
                                                 },
                                                 // 流末重查（① 的数字来自
                                                 // 落库行，用量在流末才回填）：
                                                 // 只在 1→0 的那一拍查一次，
                                                 // 不给流式期间加查询
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

                // ========== 供应商余量（G7）============
                // 位置：统计网格**下方**的独立全宽区块（spec §5.1「不占第 6 张
                // 统计格」——审计实测 1200 宽下 statW ≈ 135px × 固定 144px 高，
                // 放不下「每 profile 一行 + 多窗口进度条」）。
                // 数据来源：只读库（中立桥 quota_home_bridge.h）。**首页不发
                // HTTP** —— 后台刷新由 claude_module 起的 QuotaRefresher 负责，
                // 两模块生命周期解耦；`[刷新]` 按钮只是往刷新侧写一个请求信号。
                // 门控：claude_proxy 模块关闭 → 整卡隐藏（没有 profile 可显示，
                // 且刷新器根本没在跑，显示一张点不动的卡是骗人）。
                // 高度预算先算一次：余量卡与下方图表卡**共用**这一份结论
                // （矮窗下图表要给余量卡让位），所以声明在门控 if 之外。
                //
                // 算式在 quota_card::computeHomeQuotaBudget（**纯函数**，可单测
                // / probe 直接量）。原先这段算术内联在 compose lambda 里，谁也测不到，
                // 于是「矮窗下余量卡画不画」只能靠人肉心算——而心算与代码漂移过
                // 一次（G7 遗留 1 里记的「既有内容 580 / 可用 640」与代码实算对不上）。
                //
                // 首页整页**没有外层滚动容器**，所以规则是：余量卡行内容至少
                // kQuotaCardMinBodyH，否则整张不画；不够就压缩下方图表卡
                // （最多压到 kHomeChartMinH）给它让位——但**只在压缩真能换到
                // 一张画得出来的余量卡时**才压。压不出就不压：宁可这窗看不见
                // 余量卡，也不把用户本来看得懂的图表压成一条缝换零收益。
                // 仍装不下时 pageStillOverflows 为真——那是「本页装不下」的实话，
                // 不是余量卡的 bug（真要在设计最小窗也画出，得给首页加外层
                // scrollView，那是另一件事，见 report-G8 遗留）。
                const module::home::quota_card::HomeQuotaBudget quotaBudget =
                    module::home::quota_card::computeHomeQuotaBudget(
                        screen.height, statH, pad, gap);
                if (moduleRegistered(::core::PageId::ClaudeProxy)) {
                    if (quotaBudget.drawsQuotaCard) {
                    std::vector<module::claude::quota::QuotaHomeRow> quotaRows;
                    std::string quotaErr;
                    if (auto* qdb = ::core::appDatabase()) {
                        auto qr =
                            module::claude::quota::listQuotaHomeRows(*qdb);
                        if (qr.isOk()) {
                            quotaRows = std::move(qr.value());
                        } else {
                            // 不静默：查询失败会在卡上显形（不是「暂无 profile」）
                            quotaErr = qr.errorMessage();
                            LOG_ERROR("Home 余量卡读取失败: " + quotaErr);
                        }
                    } else {
                        quotaErr = "数据库未初始化（appDatabase 为空）";
                    }
                    module::home::quota_card::Hooks hooks;
                    hooks.refreshAll = [] {
                        module::claude::quota::
                            requestQuotaImmediateRefresh();
                    };
                    hooks.openConfig = [this](int64_t profileId) {
                        navigateTo(::core::PageId::ClaudeProxy,
                                   "action:quota_config:" +
                                       std::to_string(profileId));
                    };
                    hooks.loadError = quotaErr;
                    module::home::quota_card::drawQuotaCard(
                        ui, t, contentW, quotaRows, hooks,
                        quotaBudget.quotaBodyHeight);
                    }  // 窗口够高才画（见上）
                }

                // ========== 今日 Token 用量分布（每小时输出 token 合计，去头去尾） ==========
                // 真机反馈 ③（2026-09-29 改口径）：旧的「按当日峰值归一」百分比
                // 口径作废 —— 纵轴 = 该小时 output_tokens 合计；x 轴只画「第一个
                // 有数据的小时」..「最后一个有数据的小时」闭区间（区间外的空小时
                // 不画）；整天没有数据 → 空态（不画空轴）。数据 =
                // HomeService::getTodayHourlyTokenTotals（恒 24 点 → 绝不空）。
                //
                // 为什么不再用 components::barChart（本卡 M3-1 的原实现）：
                // barchart.h 把纵值硬编码成 [0,1] 百分比语义（build 内
                // clamp(v,0,1)、tooltip 恒拼 percent(v)），喂 token 合计（整数、
                // 动辄上万）会被整体夹成满格、悬停仍显示「N%」——与本需求
                // 「显示每小时 token 总和」直接冲突。上游改造（spec
                // 2026-09-28-ui-modernization-design §6 的「缺的能力走上游贡献」）
                // 本轮被排除（禁止改 EUI-NEO），故本卡在模块内自绘柱体：
                //   · 几何沿用 barchart.h 的常量（plotX 32 / plotY 70 /
                //     plotH = wrapH−112 / slot 均分 / 柱宽 min(32,max(18,slot*0.54))
                //     / 4 条网格线），chromeless 语义不变（无第二底、无边框阴影）；
                //   · 元素 id 沿用 home.api.chart.bars.*（grid.N / bar.N / label.N）
                //     —— 既有 probe（home_probes 类 4(a) 量 grid.0..grid.3 的
                //     plot 高）与单测定位不失效；
                //   · 代价：没有悬停 tooltip。改由柱顶数值标签直接标出 token
                //     合计（数字可见比悬停更直接）。
                // 2026-09-28 审计 #3/#4/#5 的口径保留：卡高 260、wrap 196、
                // plot 84、全零走空态；title/wrap 不二次偏移 16。
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
                    // 去头去尾（纯函数，见 home_service.h）：闭区间两端都是
                    // 「有 token 的小时」；全零 → hasData=false → 空态
                    const module::home::HourRange range =
                        module::home::hourlyDataRange(hourly);
                    const size_t barCount =
                        range.hasData ? range.last - range.first + 1 : 0;
                    int64_t peak = 0;
                    for (size_t i = 0; i < barCount; ++i) {
                        peak = std::max(peak, hourly[range.first + i]);
                    }
                    // 图表卡高度：窗口宽裕时是标称的 kHomeChartH；矮窗下按
                    // computeHomeQuotaBudget 的结论压缩（给余量卡让位），最矮
                    // 到 kHomeChartMinH。两处取自同一套常量，不会各写一个 260。
                    const float chartCardH =
                        quotaBudget.chartHeight > 0.0f
                            ? quotaBudget.chartHeight
                            : module::home::quota_card::kHomeChartH;
                    const float chartWrapH = chartCardH - 48.0f - 16.0f;  // 196
                    components::card(ui, "home.api.chart")
                        .size(contentW, chartCardH)
                        .theme(core::theme::componentTokens(t))
                        .padding(t.sizes.spacingLarge)  // 16 统一内边距（替代默认 20）
                        .content([&] {
                            ui.text("home.api.chart.title")
                                .position(0.0f, 16.0f)
                                .size(contentW - 32.0f, 22.0f)
                                .text("今日 Token 用量分布（每小时输出 token 合计）")
                                .fontSize(t.sizes.fontSizeLarge - 1.0f)
                                .lineHeight(22.0f)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            // 图表区：自绘柱体包一层定位 stack（沿用 wrap id，
                            // probe 类 5(a) 按它量卡片内左右对称）
                            ui.stack("home.api.chart.wrap")
                                .position(0.0f, 48.0f)
                                .size(contentW - 32.0f, chartWrapH)
                                .content([&] {
                                    const float wrapW = contentW - 32.0f;
                                    if (!range.hasData) {
                                        // 空态（审计 #4）：整天没数据不画空轴
                                        // 也不画假条，居中一句引导
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
                                        // 柱体容器（id 沿用组件根 id：probe 与
                                        // 单测按 home.api.chart.bars.* 定位，
                                        // 坐标原点与 wrap 重合）
                                        ui.stack("home.api.chart.bars")
                                            .position(0.0f, 0.0f)
                                            .size(wrapW, chartWrapH)
                                            .content([&] {
                                        // 自绘几何：常量逐项取自 barchart.h
                                        // （改这里 = 改图表观感，probe 类 4(a)
                                        // 量 grid.0→grid.3 的 plot 高）
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
                                        // chromeless（审计 #3 口径）：网格线用
                                        // 主题描边色的低透明版，无第二底、无边框
                                        components::theme::ThemeColorTokens
                                            chartTokens =
                                                core::theme::componentTokens(t);
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
                                            // 柱高按区间峰值归一（相对高度）；
                                            // 0 token 的小时只剩 1px 底线，不画
                                            // 假柱（审计 #4 口径）
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
                                                // ——与改口径前的柱子同色，本次
                                                // 只改数不改观感
                                                .color(components::theme::color(
                                                    0.22f, 0.50f, 0.88f))
                                                .radius(std::min(
                                                    t.sizes.radius,
                                                    barW * 0.34f))
                                                .build();
                                            // 柱顶数值 = 该小时 token 合计
                                            //（替代旧 barChart 的悬停百分比）
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
                                                .color(t.colors.textSecondary)
                                                .build();
                                            // x 轴小时标签：去头去尾后第 i 根
                                            // 对应真实小时 range.first + i
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
                                                .color(t.colors.textTertiary)
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
            })   // home.root 的 content
            .build();  // home.root
    })  // home.scroll 的 content（收尾多这一层）
        .build();  // home.scroll
    }

private:
    static constexpr int kRecentNoteLimit = 5;

    // ① 的取数结果（compose 只读；空态/失败态靠 hasValue + lastRateError_ 区分）
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
    // 上一拍看到的活跃流数（只有 Token 卡的 timer 回调写它，compose 不写）：
    // 1→0 的那一拍 = 流式刚结束 → 重查最近一次成功调用（① 的数字来自落库行，
    // 流末才回填用量；不重查的话用户停在首页时看到的永远是进页那一刻的数字）
    int prevActiveStreams_ = 0;
};

} // namespace module::home

extern "C" ::core::Module* create_home_module() {
    return new module::home::HomeModule();
}
