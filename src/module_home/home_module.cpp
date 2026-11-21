// Home 仪表盘模块：只读聚合 markdown_index / todo_tasks / claude_call_logs，
// 经 core::appDatabase() 拿共享连接。导航经 core::appRouter()，module_* 互不依赖；
// 跨模块动作走 Router payload（"action:*" / "open:*"），由目标页消费。
#include "core/app_config.h"
#include "core/call_rate.h"
#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/theme.h"
#include "home_service.h"
#include "toast_notice_bridge.h"  // need270：点已关停模块卡片时投递应用级提示
#include <ui_kit/card.h>
#include <ui_kit/jelly.h>  // need301③ 统计卡果冻时间线（手写缝）

#include <eui/dsl.h>

#include <components/linechart.h>
#include <components/button.h>
#include <components/card.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace module::home {

namespace {

// C-2（need380 wave5-c）：今日 token 图表卡（kHomeChartH/kHomeChartCollapsedH/
// kChartCollapsedKey/kHomeStatGrowRatio 及其几何派生）随卡片一起移除。

// 快捷按钮行距窗口底边框的最小留白（2026-10-27 真机反馈贴边）：计进 baseContentH，
// 否则恰好装满的窗口会因这 16px 被滚动条吞掉——两处必须同改。
constexpr float kHomeActionsBottomMargin = 16.0f;

// 卡片网格（need268，2026-11-16 拍板「整页卡片网格 + 响应式列数」）：
// dashboard 惯例是卡片宽恒等、列数随窗宽变——模块关停后幸存卡不跟着拉成巨卡。
//   · minCardW = 单卡可读下限：低于它文字挤成一团（96 高卡里 30px 数字要放下）；
//   · cols 由可用宽反推，上限为元数据表行数（5）；
//   · cardW 用「按列数均分」的精确值，保证一行正好排满不留参差。
// 卡片自身宽度固定，行内换行交给 EUI flow（core/layout.h layoutFlow）。
constexpr float kHomeCardMinW = 180.0f;
constexpr int kHomeCardMaxCols = 5;

int homeGridColumns(float availableW, float gap) {
    for (int cols = kHomeCardMaxCols; cols > 1; --cols) {
        const float cardW =
            (availableW - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        if (cardW >= kHomeCardMinW) return cols;
    }
    return 1;  // 窄窗兜底：单列占满
}

float homeGridCardWidth(float availableW, float gap, int cols) {
    if (cols <= 1) return availableW;
    return (availableW - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
}

// need301③：统计卡 hover 集（todo plate.hoverCards / schedule 同款集成员语义）。
// drawSolidStatCard 为自由函数、home 页进程单实例 → 文件级静态承载，键=卡元素 id。
std::unordered_set<std::string>& statCardHoverSet() {
    static std::unordered_set<std::string> set;
    return set;
}

// 玻璃统计卡（2026-11-01 重构）：玻璃白半透底 + 16px 圆角 + 20px 内边距 +
// 指标名 14px（说明并入「笔记 · 今日」）/ 数据 30px 700 一级 两行排版。
// 五色实底退役（S6.6）：那 5 色不承载状态语义，五个彩色焦点互相抢，反而不聚焦。
// numberColor/badgeColor 非空覆盖默认配色（badge 并入 label 行后 badgeColor
// 不再参与绘制，参数保留 = 五处调用点免改；传了也不生效）。timerSeconds > 0 挂
// 0 尺寸 onTimer 定期重绘；chart 非空叠加迷你图表；onTick 为该 timer 回调。
// 点击由 .press 透明层接收（hover/press 态）。
void drawSolidStatCard(eui::Ui& ui,
                       const std::string& id,
                       float width,
                       float height,
                       const core::theme::Tokens& t,
                       const eui::Color& bgColor,
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
    const float pad = 20.0f;  // 设计稿 §6.1：卡片内边距 20px
    const float cw = width - pad * 2.0f;
    const float corner = t.sizes.radiusOverlay;  // 16px
    // 两行排版：指标名 16 行高 + 4 间隙 + 数字 34 行高 = 54，卡内内容高 96−40=56 装得下；
    // 说明并入指标名（「笔记 · 今日」），不单独占行。
    // B6/F-69-2：行盒/间隙随字号派生（基准字面量走 scaled，token 值不可再乘），
    // 否则放大档「字变大行盒不变 → 数字被裁」。
    const float labelH = core::theme::scaled(16.0f);
    const float numGap = core::theme::scaled(4.0f);
    const float numberH = core::theme::scaled(34.0f);  // 30px 字号行高
    // 文本块在「内边距以内」垂直居中（不是按卡高算——那样会多吃掉上下 pad，
    // 96 高卡里数字底边越出卡缘，2026-11-01 重构把卡从 120 压到 96 时踩过）。
    // spark 垫在数字行同一带先画，不占纵向空间：五张卡文字位置完全一致。
    const float innerH = height - pad * 2.0f;
    const float textTop =
        std::max(0.0f, (innerH - (labelH + numGap + numberH)) * 0.5f);
    const float numberY = textTop + labelH + numGap;

    // need301③ 统计卡果冻（need297 E1 先例 todo_ui_board.cpp:786-810 手写缝同款）：
    // CardBuilder 不外露 scale/transform 缝 → 按 components::card::build 等价展开
    //（bg 的 border/shadow 沿用 CardStyle 默认档，视觉零漂移），果冻只追加
    // radius/scale 形变目标；命中面 .press 与 onClick 零改动，hover 写集下帧读
    //（todo plate.hoverCards 同款集成员语义）。
    const components::CardStyle cardStyle;  // 与 components::card 默认档一致
    const bool cardHovered = statCardHoverSet().count(id) > 0;
    const ui_kit::JellyPose pose = ui_kit::jellyFrame(id, cardHovered);
    // need338 迁 need334 范式：transform/zIndex 施加在**整卡 stack**（本函数根
    // stack 即整卡）。曾挂 .bg 时 .content（图表/文字/命中层）是 .bg 兄弟，dsl.h
    // 兄弟排序按 subtreeMaxZIndex 把玻璃面排到内容之上（hover 白罩），且 bg 独缩
    // 内容不缩（错位露灰底）。bg 只留 radius 形变（仍随 radiusOverlay 平移）。
    ui.stack(id)
        .size(width, height)
        .transformOrigin(0.5f, 0.5f)   // 果冻坑③：缩放中心=卡中心
        .scale(pose.sx, pose.sy)       // 坑②：逐轴目标每帧重给
        .zIndex(pose.active ? 20 : 0)  // 动画期盖邻卡；走完回落 0
        .transition(pose.active
                        ? core::Transition::make(0.035f, core::Ease::Linear)
                        : core::Transition::make(0.18f, core::Ease::OutBack))
        .animate(core::AnimProperty::Transform)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .color(bgColor)
                .border(cardStyle.border)
                .shadow(cardStyle.shadow)
                .radius(pose.radius +
                        (corner - ui_kit::JellyOptions::kRestRadius))
                .transition(pose.active
                                ? core::Transition::make(0.035f,
                                                         core::Ease::Linear)
                                : core::Transition::make(0.18f,
                                                         core::Ease::OutBack))
                .animate(core::AnimProperty::Radius)
                .build();
            ui.stack(id + ".content")
                .position(pad, pad)
                .size(cw, innerH)
                .margin(0.0f, 0.0f, pad, pad)
                .content([&] {
            // 迷你图表（sparkline）：垫在数字行同一带，先于文字 build → 数字压其上。
            // 五张卡的文字位置完全一致，不因有无 spark 而位移（home_probes 组 A 守）。
            // 底留 4px：全零/平线序列是贴着带底画的，贴到卡内底缘会被吃掉看不见。
            // 半透明：读作背景纹理，不与 44px 数字抢注意力。
            if (chart) {
                const float sparkH = numberH - 4.0f;
                ui.stack(id + ".sparkwrap")
                    .position(0.0f, numberY)
                    .size(cw, sparkH)
                    .opacity(0.55f)
                    .content([&] { chart(ui, cw, sparkH); })
                    .build();
            }
            // 指标名：三级字，居中；说明（badge）并入同行「笔记 · 今日」。
            // B7 D1（已拍板 5 张统一）：调用方正常态传空 badge → 只显示指标名；
            // 错误态传「加载失败」→ 照旧拼出来，失败必须显形。
            // 合并文本量宽放不下卡宽时退化为只留 badge（不放宽回指标名）：badge 现在
            // 只剩错误态「加载失败」，丢 badge 等于把失败藏起来——这正是池117 在
            // 1000~1394 窗口宽下暴露的缺陷（合并文案 14px≈180 > cw，旧逻辑回退成指标名，
            // 速率卡「加载失败」不显形）。单 badge 文本在最小窗 cw≈87 仍放得下。
            // 溢出仍由 maxWidth+Ellipsis+clip 兜底，量宽不准也绝不会画到卡外。
            // 量宽留 12px 余量：measureTextSize 与最终排版有 1~2% 的差（字距/取整）。
            std::string labelText = badge.empty() ? label : label + " · " + badge;
            if (!badge.empty()) {
                core::TextStyle merged;
                merged.text = labelText;
                merged.fontSize = core::theme::scaled(14.0f);
                if (core::TextPrimitive::measureTextSize(merged).x + 12.0f > cw) {
                    labelText = badge;
                }
            }
            ui.text(id + ".label")
                .position(0.0f, textTop)
                .size(cw, labelH)
                .text(labelText)
                .fontSize(core::theme::scaled(14.0f))
                .lineHeight(labelH)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .maxWidth(cw)
                .overflow(core::TextOverflow::Ellipsis)
                .clip()
                .build();

            // 数据：30px 600 一级字（2026-11-01 重构 44→30：44 在 96 高卡里压得标签无处立足）
            ui.text(id + ".number")
                .position(0.0f, numberY)
                .size(cw, numberH)
                .text(number)
                .fontSize(t.sizes.fontSizeStat)
                .lineHeight(numberH)
                .fontWeight(600)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(numberColor ? *numberColor
                                   : (accent ? t.colors.errorText
                                             : t.colors.textPrimary))
                .build();

            // 可选计时器（0 尺寸 onTimer）
            if (timerSeconds > 0.0f) {
                ui.stack(id + ".timer")
                    .size(0.0f, 0.0f)
                    .onTimer(timerSeconds,
                             onTick ? std::move(onTick)
                                    : std::function<void()>([] {}))
                    .build();
            }
            // 整卡点击层：透明 rect 覆盖全卡，挂 hover/press/onClick
            if (onClick) {
                ui.rect(id + ".press")
                    .position(-pad, -pad)
                    .size(width, height)
                    .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            t.isDark
                                ? eui::Color{t.colors.surface.r, t.colors.surface.g,
                                             t.colors.surface.b, 0.08f}
                                : eui::Color{t.colors.textPrimary.r, t.colors.textPrimary.g,
                                             t.colors.textPrimary.b, 0.06f},
                            t.isDark
                                ? eui::Color{t.colors.surface.r, t.colors.surface.g,
                                             t.colors.surface.b, 0.12f}
                                : eui::Color{t.colors.textPrimary.r, t.colors.textPrimary.g,
                                             t.colors.textPrimary.b, 0.10f})
                    .radius(corner)
                    .onClick(std::move(onClick))
                    // need301③：hover 边沿喂果冻时间线（写集，下帧 drawSolidStatCard 读）。
                    // need338：必须按值捕获 id——回调在 compose 返回后才会被事件层调用，
                    // 引用捕获 = 悬垂（need338 单测确定必现：二帧 compose 复用内存致插队串号）。
                    .onHover([id](bool h) {
                        if (h) {
                            statCardHoverSet().insert(id);
                        } else {
                            statCardHoverSet().erase(id);
                        }
                    })
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
// 底色统一玻璃白（2026-11-01 重构去五彩色：那 5 色不承载状态语义，纯装饰，5 个焦点
// 抢同一层级反而都不聚焦）。指标身份靠 label 文案与数字着色，不靠底色。
enum class StatCardKind { Tokens, Notes, TodoPending, TodoOverdue, ApiCalls };
struct HomeStatCard {
    StatCardKind kind;
    ::core::PageId module;
    const char* title;
    ::core::PageId target;
};
constexpr HomeStatCard kStatCards[] = {
    {StatCardKind::Tokens,      ::core::PageId::ClaudeProxy, "Token 速率", ::core::PageId::ClaudeProxy},
    {StatCardKind::Notes,       ::core::PageId::Markdown,    "今日笔记",           ::core::PageId::Markdown},
    {StatCardKind::TodoPending, ::core::PageId::Todo,        "待办任务",           ::core::PageId::Todo},
    {StatCardKind::TodoOverdue, ::core::PageId::Todo,        "逾期任务",           ::core::PageId::Todo},
    {StatCardKind::ApiCalls,    ::core::PageId::ClaudeProxy, "API 调用",           ::core::PageId::ClaudeProxy},
};

// 快捷按钮：kind = 行身份，module = 归属模块，target + navPayload = 点击负载。
// C-2（need380）：「切换 Claude Profile」快捷按钮已砍——与 Claude 页右键
// 「设为当前」重复，入口收敛到 Claude 页。
enum class ActionKind { NewNote, NewTodo };
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
        const float pad = t.sizes.spacingLarge * 2.0f;  // 32 —— S4.2 弹性内边距（spacingLarge=16×2=32）
        const float gap = t.sizes.spacingLarge * 1.25f; // 20
        // ⚠ contentW 不在这里定义：整页挂在 home.scroll 下，内容宽只有 scrollView 的 content
        // 回调拿到的 innerW 说了算（见下方同名遮蔽定义）。kHeaderActionsW + 标题列 = contentW。
        constexpr float kHeaderActionsW = 100.0f;  // W9 仅保留刷新按钮（设置按钮已移除）
        const float cardGap = 8.0f;  // 全站卡间距统一档 8px（列表统一规范 §4.3）
        // 统计行高在下方 scrollView 的 content 回调内派生（需要视口高 innerH），见那里。

        // 统计卡恒为全表行数（need270）：统计是**事实**，与模块开关无关——
        // 数据由 home_service 直读库（claude_call_logs / markdown_index / todo_tasks），
        // 关掉 Claude 模块不代表「今天没调过 API」。旧实现按注册态过滤会让关停模块
        // 的指标连同历史数据一起消失，用户看到「卡片数变少」以为出了 bug。
        constexpr int statCount =
            static_cast<int>(sizeof(kStatCards) / sizeof(kStatCards[0]));

        // 页面不自铺底色，用主窗口 clearColor 透出（否则半透/磨砂档页面 alpha 恒 1 看不见效果）。

        // 首页外层滚动容器：整页包一层 scrollView，矮窗超出由滚动条吸收，装得下时几何不变。
        // ⚠ 上游坑：layoutColumn() 算子元素只看流式游标，position() 挂在 column 的流式
        // 子元素上会被静默忽略——home.root 的直接子块一律只用 flow + size()。形参 ui 刻意与外层同名。
        components::scrollView(ui, "home.scroll")
            .size(screen.width, screen.height)
            .theme(core::theme::componentTokensWeChat(t))  // W9 微信页面底 #F7F7F7
            .content([&](eui::Ui& ui, float innerW, float innerH) {
            // ⚠ contentW 遮蔽外层：出滚动条时内容列收窄，必须跟 innerW 走，否则横向溢出。
            const float contentW = innerW - pad * 2.0f;
            // 弹性基准 baseContentH = 内容刚好填满页高的高度，全部从令牌派生，
            // 与平台标题条无关：上下内边距 + 页头 40 + 统计块 + 间隙 + 图表卡 + 操作行。
            // 窗口高于基准才伸展；低于基准由外层滚动吸收（不压缩）。
            const bool hasStats = statCount > 0;
            // C-2（need380 wave5-c）：今日 token 图表卡已砍——小时分布与 Claude 页
            // 调用记录同源重复，入口收敛到 Claude 页。
            int actCount = 0;
            for (const auto& btn : kActionButtons) {
                if (moduleRegistered(btn.module)) ++actCount;
            }
            const bool hasActions = actCount > 0;
            const float statBaseH = 96.0f * core::theme::fontScale();
            // 卡片网格列数（need268）：按可用宽反推，窄窗自动减列、超宽屏最多 5 列。
            // ⚠ 列宽必须按**网格容量**算，不能按可见卡数算——后者会让少卡时每卡
            // 分到更大份额，正是「3 张巨卡」的成因（旧行为）。可见卡少就在行尾留白。
            const int gridCols = homeGridColumns(contentW, cardGap);
            const int statCols = gridCols;
            const int statRows = hasStats ? ((statCount + statCols - 1) / statCols) : 0;
            // 全关时统计区是 32 高空态行（见下方 statCount == 0 分支）。
            const float statsBlockH =
                hasStats ? (statBaseH * static_cast<float>(statRows) +
                            cardGap * static_cast<float>(statRows - 1))
                         : 32.0f;
            const int childCount = 2 + (hasActions ? 1 : 0);
            const float baseContentH =
                40.0f + statsBlockH +   // 上下内边距 0（批L 顶格+贴底），左右 pad 不占高
                gap * static_cast<float>(childCount - 1) +
                (hasActions ? 44.0f + kHomeActionsBottomMargin : 0.0f);
            // 顶距 32→0（批L）：高度向内边距为 0，基准里旧 32 顶距已不存在
            // 0.5px 余量：防浮点求和越过视口触发本不该出现的滚动条。
            const float extraH = std::max(0.0f, innerH - baseContentH - 0.5f);
            // 余高去向：图表卡已砍（C-2 need380），统计行全吃。
            // 网格化后统计区可能多行，增量按行均摊——单卡高度不随窗宽飘。
            const float statsGrowTotalH = hasStats ? extraH : 0.0f;
            const float statsGrowH =
                (hasStats && statRows > 0)
                    ? statsGrowTotalH / static_cast<float>(statRows)
                    : 0.0f;
            const float statH = statBaseH + statsGrowH;
        ui.column("home.root")
            // 高度用 wrapContent：固定高度会让 scrollView 的测量 pass 永远量到「正好等于视口」→ 永不判可滚
            .width(innerW)
            .height(core::SizeValue::wrapContent())
            .padding(pad, 0.0f, pad, 0.0f)  // EUI 序=(left,top,right,bottom)：顶 0 标题顶格、底 0 弹性贴底（2026-10-25 拍板）
            .gap(gap)
            .content([&] {
                // UAT5 全局顶部收紧：页头行 72→40（与其他页 kHeaderH=40 同档）——
                // 标题 24px 加粗（与 todo/markdown 页题同字号）。UAT6 副题横扫：
                // 标题下日期行删除，页头只剩标题行（同 todo/md 档）；行高 40 不动——
                // 行内刷新钮 40 高，且标题 y 与各页 40 档页头对齐，预算 588/600 复核不变（见文件头）。
                ui.row("home.header")
                    .size(contentW, 40.0f)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        // 标题列宽 = contentW − 右侧按钮组宽（两段和 = contentW，header 行 gap=0）。
                        ui.column("home.greeting")
                            .size(std::max(280.0f, contentW - kHeaderActionsW), 40.0f)
                            .gap(2.0f)
                            .content([&] {
                                // B6：页标题行盒随字号派生（同 settings.h1 口径）
                                ui.text("home.h1")
                                    .size(std::max(280.0f, contentW - kHeaderActionsW),
                                          core::theme::scaled(24.0f))
                                    .text("工作台")
                                    .fontSize(t.sizes.fontSizeTitle)
                                    .lineHeight(core::theme::scaled(24.0f))
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();
                            })
                            .build();

                        ui.row("home.header.actions")
                            .size(kHeaderActionsW, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                // S3.1#4: 刷新按钮 Standard Outline 40px——
                                // 白底 + 绿边框 + 绿文字 + 绿图标
                                components::button(ui, "home.btn.refresh")
                                    .size(100.0f, 40.0f)
                                    .text("  刷新")
                                    .icon(0xF021)  // fa-arrows-rotate
                                    .fontSize(t.sizes.fontSizeBody)
                                    .colors(t.colors.surface,
                                            t.colors.primarySoftBg,
                                            components::theme::withOpacity(t.colors.primaryPressed, 0.15f))
                                    .border(1.0f, t.colors.primary)
                                    .textColor(t.colors.primary)
                                    .iconColor(t.colors.primary)
                                    .radius(t.sizes.radius)
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
                // B7 D1（已拍板 5 张统一）：正常态只留指标名，badge 传空；
                // 唯一例外是「加载失败」——失败不能藏（流式路数由数字主色体现）。
                const std::string tokenBadge =
                    rateFailed ? "加载失败" : std::string{};

                if (statCount == 0) {
                    // 三模块全关的空态兜底：标题区保留 + 一句极简引导。
                    ui.text("home.stats.alloff")
                        .size(contentW, core::theme::scaled(32.0f))
                        .text("所有工具模块已关闭")
                        .fontSize(t.sizes.fontSizeBody)
                        .lineHeight(core::theme::scaled(32.0f))
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                // 卡片网格（need268）：列数随窗宽响应，卡宽按列数均分后固定——模块
                // 关停后幸存卡保持原尺寸，不再被拉成「三张巨卡」。换行由 EUI flow
                // 自动完成（core/layout.h layoutFlow），行间距走 lineGap。
                const float statW = homeGridCardWidth(contentW, cardGap, statCols);
                // sparkline 序列只取一次（live 判定与图表绘制共用；空序列兜底 {0.0f}）。
                const std::vector<float> sparkSeries =
                    core::proxy_stats::rateSeries(24);
                ui.flow("home.stats")
                    .size(contentW, statsBlockH + statsGrowTotalH)
                    .gap(cardGap)
                    .lineGap(cardGap)
                    .content([&] {
                        // 逐表行渲染：行序 = 渲染序，元素 id 由 kind 决定；数值/配色/图表按行身份在表外取。
                        // need270：不过滤注册态——5 张卡恒在场（数据是事实，不随模块开关消失）。
                        for (const auto& card : kStatCards) {
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
                                        streaming ? &t.colors.primary
                                        : rateOk  ? &t.colors.textPrimary
                                                  : &t.colors.textTertiary;
                                    drawSolidStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, ui_kit::cardSurface(t), card.title,
                                                 tokenNumber,
                                                 tokenBadge, false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 },
                                                 tokenNumberColor,
                                                 streaming ? &t.colors.primary
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
                                    drawSolidStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, ui_kit::cardSurface(t), card.title,
                                                 num(stats_.todayNotes),
                                                 ok ? "" : "加载失败",
                                                 false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                                case StatCardKind::TodoPending:
                                    drawSolidStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, ui_kit::cardSurface(t), card.title,
                                                 num(stats_.pendingTodos),
                                                 ok ? "" : "加载失败",
                                                 false,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                                case StatCardKind::TodoOverdue:
                                    drawSolidStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, ui_kit::cardSurface(t), card.title,
                                                 num(stats_.overdueTodos),
                                                 ok ? "" : "加载失败",
                                                 stats_.overdueTodos > 0,
                                                 [this, target = card.target] {
                                                     navigateTo(target);
                                                 });
                                    break;
                                case StatCardKind::ApiCalls:
                                    drawSolidStatCard(ui, statCardElementId(card.kind),
                                                 statW, statH, t, ui_kit::cardSurface(t), card.title,
                                                 num(stats_.todayApiCalls),
                                                 ok ? "" : "加载失败",
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

                // 快捷操作按钮组（S3.1#3/#6）：新建笔记=Filled 44px / 新建任务=Outline 44px。
                if (actCount > 0) {
                const float actGap = t.sizes.spacingLarge; // 16
                ui.row("home.actions")
                    .size(contentW, 44.0f)
                    // 底部留白防贴边（8px 网格）；EUI flow 布局下末子块 margin 计入列内容高。
                    .margin(0.0f, 0.0f, 0.0f, kHomeActionsBottomMargin)
                    .gap(actGap)
                    .content([&] {
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
                                .radius(t.sizes.radius);
                            if (btn.kind == ActionKind::NewNote) {
                                // Filled: primary bg + white text
                                builder.colors(t.colors.primary,
                                               t.colors.primaryHover,
                                               t.colors.primaryPressed)
                                    .textColor(t.colors.onPrimary);
                            } else if (btn.kind == ActionKind::NewTodo) {
                                // Outline: white bg + green border + green text
                                builder.colors(t.colors.surface,
                                               t.colors.primarySoftBg,
                                               components::theme::withOpacity(t.colors.primaryPressed, 0.15f))
                                    .border(1.0f, t.colors.primary)
                                    .textColor(t.colors.primary);
                            }
                            if (btn.iconCodepoint != 0) {
                                builder.icon(btn.iconCodepoint)
                                    .iconColor(t.colors.primary);
                            }
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
        // need270：统计卡恒显示后，归属于「已关停模块」的卡也仍在场上。此时点它
        // 会命中 Router 的未注册页兜底（静默回 Home）——用户看到的是「点了没反应」。
        // 这里先给一句可归因的提示，再交给 Router 走既有兜底。
        if (!router->isRegistered(page)) {
            app::toast_notice::show(
                "该模块已关闭",
                std::string("「") + ::core::pageIdToString(page) +
                    "」未启用，可在设置页开启后查看完整页面。");
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
