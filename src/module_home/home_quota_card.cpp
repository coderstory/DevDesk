// src/module_home/home_quota_card.cpp
// G7：首页余量卡片绘制（契约见 home_quota_card.h）。
//
// 三条纪律：
//   1. **画不画条只有 showsQuotaBar 一个判据**（假 0% 红线）。绘制层里没有
//      任何「顺手画一条」的分支：条只出现在 `if (showBar) {}` 里，而 showBar
//      来自那个纯函数。
//   2. **状态文案按 kind 选，不按「有没有数据」选**——错误文案由桥算好
//      （row.lastError，恒非空），这里原样渲染，不做二次判断。
//   3. 几何全部来自常量 + 纯函数（computeQuotaCardMetrics），probe 能直接量。
#include "home_quota_card.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "core/component_theme.h"

#include <components/button.h>
#include <components/card.h>
#include <components/progress.h>
#include <components/scrollview.h>
#include <components/theme.h>

namespace module::home::quota_card {

namespace {

using module::claude::quota::QuotaHomeKind;
using module::claude::quota::QuotaHomeWindow;

// ---- 布局常量（16px 网格；改这里 = 改观感，probe 会量到）----
constexpr float kPad = 16.0f;
constexpr float kHeadH = 24.0f;
constexpr float kTitleH = 28.0f;
constexpr float kTitleGap = 10.0f;
constexpr float kRowGap = 10.0f;
constexpr float kWinRowH = 24.0f;
constexpr float kBalRowH = 26.0f;
constexpr float kErrRowH = 18.0f;
constexpr float kBarH = 8.0f;
constexpr float kInnerGap = 8.0f;

constexpr float kNameW = 168.0f;
constexpr float kProviderW = 112.0f;
constexpr float kStaleW = 68.0f;
constexpr float kConfigBtnW = 88.0f;
constexpr float kWinLabelW = 52.0f;
constexpr float kPctW = 52.0f;
constexpr float kResetW = 108.0f;

eui::Color tintColor(Tint tint, const core::theme::Tokens& t) {
    switch (tint) {
        case Tint::Red: return t.colors.error;
        case Tint::Orange: return t.colors.warning;
        case Tint::Green: break;
    }
    return t.colors.success;
}

// 「这一行有没有错误文案要显示」。Unconfigured / Normal / Pending 无错。
bool showsErrorLine(QuotaCardState state) {
    return state == QuotaCardState::Incomplete ||
           state == QuotaCardState::FailedNever ||
           state == QuotaCardState::FailedStale;
}

// 未配置 / 配置不完整两态才给「去配置」按钮——这两态的可执行动作就是打开
// G6 的弹窗补配置。失败态（401 / 套餐过期）给按钮是误导：换配置解决不了。
bool showsConfigButton(QuotaCardState state) {
    return state == QuotaCardState::Unconfigured ||
           state == QuotaCardState::Incomplete;
}

const char* stateFallbackText(QuotaCardState state) {
    switch (state) {
        case QuotaCardState::Unconfigured: return "未配置余量查询";
        case QuotaCardState::Incomplete: return "配置不完整，去补全";
        case QuotaCardState::Pending: return "等待首次查询";
        case QuotaCardState::Normal: return "";
        case QuotaCardState::FailedStale: return "";
        case QuotaCardState::FailedNever: return "";
    }
    return "";
}

// 状态行文字：**有错时给空串**，无错用状态标签。
//
// 为什么有错给空：错误文案由**专属错误行**（渲染层的 `showErr` 块）渲染
// **一次**。head 行再渲染一遍就是同一张卡上同一句话出现两次，而且 head 的
// .status 是固定宽的单行盒、既没 clip 也没 wrap（maxWidth 只在 wrap=true
// 时才生效，见 EUI-NEO text.cpp），长错误文案会把字直接画到卡片右缘之外
// 被截断（真机落图 /tmp/af-home-quota-live.png 的 MiniMax 行：行首一遍被
// 截断、下一行再来一遍）。
//
// 仍然**不按「有没有数据」选** —— 那是把 401 显示成「可能过期」的根源；
// 按 kind 选文案这件事发生在桥里（row.lastError），这里只决定「这一格
// 该不该有字」。
std::string statusTextFor(const QuotaHomeRow& row) {
    if (showsErrorLine(row.state)) return std::string();
    return stateFallbackText(row.state);
}

// ---- ISO8601 → epoch 秒（纯算术，无平台 API，两平台同形）----
// Howard Hinnant days_from_civil。不用 mktime/timegm 是为了不碰平台时区库
// （上游给的是 UTC，mktime 按本地时区解释 → 倒计时偏差一个时区）。
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                              // [0, 399]
    const int64_t doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      // [0, 146096]
    return era * 146097 + doe - 719468;
}

// 解析 "YYYY-MM-DDTHH:MM:SS"（或空格分隔；尾部 Z / 毫秒 / 偏移一律忽略）。
// 逐字符校验，不匹配返回 false ——**不猜、不容错**（猜出来的时间会让倒计时
// 显示一个错误但看起来很真的数字）。
bool parseIsoUtcSeconds(const std::string& s, int64_t* out) {
    if (s.size() < 19) return false;
    const auto digitsAt = [&](std::size_t pos, std::size_t n, int64_t* v) {
        int64_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            acc = acc * 10 + (c - '0');
        }
        *v = acc;
        return true;
    };
    int64_t y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    if (!digitsAt(0, 4, &y) || s[4] != '-' || !digitsAt(5, 2, &mo) ||
        s[7] != '-' || !digitsAt(8, 2, &d)) {
        return false;
    }
    if (s[10] != 'T' && s[10] != ' ') return false;
    if (!digitsAt(11, 2, &hh) || s[13] != ':' || !digitsAt(14, 2, &mi) ||
        s[16] != ':' || !digitsAt(17, 2, &ss)) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (hh > 23 || mi > 59 || ss > 60) return false;
    *out = daysFromCivil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
    return true;
}

}  // namespace

bool showsQuotaBar(QuotaCardState state) {
    // 「画条」= 有过成功值 = Normal / FailedStale。其余四态恒 false：
    //   Unconfigured —— 没配供应商，任何条都是编造
    //   Incomplete   —— 配置不完整（spec §5.1 明写「不画 0% 条」）
    //   Pending      —— 从未成功过（画出来必是 0%）
    //   FailedNever  —— 从未成功过（spec §5.1 明写「绝不画 0% 条」）
    return state == QuotaCardState::Normal ||
           state == QuotaCardState::FailedStale;
}

Tint tintForUsedPercent(double usedPercent) {
    if (usedPercent >= 90.0) return Tint::Red;
    if (usedPercent >= 70.0) return Tint::Orange;
    return Tint::Green;
}

std::string formatQuotaPercent(double usedPercent) {
    char buf[32];
    if (usedPercent >= 10.0) {
        std::snprintf(buf, sizeof(buf), "%.0f%%", usedPercent);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f%%", usedPercent);
    }
    return std::string(buf);
}

std::string formatQuotaBalance(double remaining, const std::string& currency) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "余额 %.2f", remaining);
    std::string out(buf);
    if (!currency.empty()) out += " " + currency;
    return out;
}

std::string formatQuotaResetCountdown(const std::string& resetsAtIso,
                                     int64_t nowSec) {
    int64_t target = 0;
    if (!parseIsoUtcSeconds(resetsAtIso, &target)) return std::string();
    const int64_t delta = target - nowSec;
    if (delta <= 0) return "已过期";
    if (delta < 60) return "即将重置";
    if (delta < 3600) {
        return std::to_string(delta / 60) + " 分钟后重置";
    }
    if (delta < 86400) {
        return std::to_string(delta / 3600) + " 小时后重置";
    }
    return std::to_string(delta / 86400) + " 天后重置";
}

float quotaRowHeight(const QuotaHomeRow& row) {
    float h = kHeadH;
    if (row.kind == QuotaHomeKind::Balance) {
        h += kBalRowH;
    } else {
        h += static_cast<float>(row.windows.size()) * kWinRowH;
    }
    if (showsErrorLine(row.state)) h += kErrRowH;
    return h;
}

QuotaCardMetrics computeQuotaCardMetrics(const std::vector<QuotaHomeRow>& rows,
                                         float contentWidth,
                                         float maxBodyHeight) {
    (void)contentWidth;   // 当前布局不随宽度变高度（行内元素全部按宽自适应）
    QuotaCardMetrics m;
    if (rows.empty()) {
        m.bodyHeight = kQuotaCardEmptyH;
        m.cardHeight = kPad * 2.0f + kTitleH + kTitleGap + m.bodyHeight;
        return m;
    }
    float contentH = 0.0f;
    for (const auto& row : rows) contentH += quotaRowHeight(row);
    contentH += kRowGap * static_cast<float>(rows.size() - 1);
    // maxBodyHeight 由调用方按窗口剩余空间给；负值（窗口太矮）兜成 0
    const float cap = std::max(0.0f, maxBodyHeight);
    m.bodyHeight = std::min(contentH, cap);
    m.overflows = contentH > m.bodyHeight + 0.5f;
    m.cardHeight = kPad * 2.0f + kTitleH + kTitleGap + m.bodyHeight;
    return m;
}

HomeQuotaBudget computeHomeQuotaBudget(float screenHeight, float statCardHeight,
                                       float pad, float gap) {
    HomeQuotaBudget out;
    // 除「余量卡行内容 + 图表卡」以外，本页其余各块的固定占用。
    // 每一项都对应 home_module 里真实绘制的那一块（kHomeWelcomeH / statH /
    // kQuotaCardChromeH / kHomeActionsH），改任一块高度这里自动跟着走。
    const float fixed = pad * 2.0f                    // 上下内边距
                        + kHomeWelcomeH + gap          // 欢迎区
                        + statCardHeight + gap        // 统计网格
                        + kQuotaCardChromeH + gap     // 余量卡外壳（不含行内容）
                        + gap                          // 图表卡与快捷按钮行之间
                        + kHomeActionsH;               // 快捷按钮行

    // ⓪ **页面本来就装不下时 → 滚动模式**（2026-10-03 final 批，G7 遗留 1 +
    //    G8 遗留 1 的正解）。判据：不含余量卡行的整页高度已经超过客户区。
    //
    //    为什么这一档必须单列、且不能沿用下面的压缩逻辑：页面一旦要滚，
    //    「压缩图表给余量卡让位」就是在为一个**不存在的问题**买单——少压那
    //    100~160px 只是把滚动距离缩短这么多，图表滚一下照样看得见，却先把
    //    用户最下面那块图表压成一条缝。G8 把阈值降到 746 仍漏掉设计最小窗
    //    720，正是因为 720 落在 `compressionCanHelp == false` 那一档：
    //    既压不出余量卡、余量卡又整张不画 = **两面落空**。
    //
    //    有了这一档之后，矮窗的行为从「牺牲图表换零收益」变成
    //    「余量卡照常画满（内容多的部分自己滚），图表保持完整高度」。
    //    `pageStillOverflows` 在这一档恒 false：页面确实超出视口，但那是
    //    scrollView 的本职工作（home_module 已把整页挂进 home.scroll），
    //    不是「余量卡把页面撑爆了」——后者才是这个字段要说的。
    const float pageWithoutQuotaBody = fixed + kHomeChartH;
    if (screenHeight < pageWithoutQuotaBody) {
        out.chartHeight = kHomeChartH;            // 图表一分不压
        out.quotaBodyHeight = kQuotaCardMaxBodyH; // 行内容给到上限（内容少时
                                                  // computeQuotaCardMetrics
                                                  // 自己取 min，不会撑高）
        out.drawsQuotaCard = true;
        out.pageStillOverflows = false;
        return out;
    }

    // ① 先按「图表不压缩」算一遍：窗口宽裕时余量卡能拿到多少
    float chart = kHomeChartH;
    float quotaBody = screenHeight - fixed - chart;
    // ② **只有「压缩图表」真的换得到一张能画的余量卡时**才压缩。
    //    这条判断不能省：矮到连 kHomeChartMinH 都腾不出 kQuotaCardMinBodyH 的
    //    窗口（本机设计最小窗 720 正是这种），压缩图表换不来任何余量卡，只会把
    //    用户本来看得懂的图表压成一条缝——那是净亏。
    //    实测教训：先前写成无条件压缩，test_ui_modern 的「Home Token 分布」
    //    几何用例（跑在 1200×720、峰值柱高钉死 84）当场红成 1。
    const bool compressionCanHelp =
        screenHeight - fixed - kHomeChartMinH >= kQuotaCardMinBodyH;
    if (quotaBody < kQuotaCardMinBodyH && compressionCanHelp) {
        const float need = kQuotaCardMinBodyH - quotaBody;   // 还差多少
        chart = std::max(kHomeChartMinH, chart - need);
        quotaBody = screenHeight - fixed - chart;
    }
    out.chartHeight = chart;
    out.quotaBodyHeight = std::max(0.0f, quotaBody);
    out.drawsQuotaCard = quotaBody >= kQuotaCardMinBodyH;
    // 压到底仍装不下 = 本页装不下（余量卡整张不画，**图表保持完整高度**）。
    // 不静默：pageStillOverflows 让调用方 / 用例能一眼看出「这不是余量卡的锅」。
    out.pageStillOverflows = !out.drawsQuotaCard;
    return out;
}

namespace {

// 一行窗口（进度条 + 百分比 [+ 倒计时]）。
// ⚠ **pct 列永远按「reset 列在场」预留**（2026-10-03 布局错位修复）：旧代码
// 按有没有 reset 各算一套 gaps/barW（`hasReset ? 3·gap : 2·gap`），无 reset
// 的行 pct 右移 116px——同一 profile 的百分比数字上下不齐（真机 OpenCode Go
// 行「0.0%」顶到最右、97%/82% 缩在中间）。固定四列（label | bar | pct |
// [reset 槽]）后 pct 的 x 与 reset 有无无关；无 reset 的行 reset 槽留空，
// bar 相应收窄。行内元素按 `.gap` 左排、children 宽度和 < cw 时右侧自然留白，
// 故「不渲染 reset 元素」也能保持 pct 落位（head 行 [刷新] 的 8px 留白同理）。
void drawWindowRow(eui::Ui& ui, const core::theme::Tokens& t, float cw,
                   const std::string& rowId, const QuotaHomeWindow& w,
                   int64_t nowSec) {
    const std::string countdown = formatQuotaResetCountdown(w.resetsAt, nowSec);
    const bool hasReset = !countdown.empty();
    const std::string barId = rowId + ".bar." + w.key;
    const float gaps = kInnerGap * 3.0f;
    const float barW = std::max(
        24.0f, cw - kWinLabelW - kPctW - kResetW - gaps);

    ui.row(barId + ".row").size(cw, kWinRowH).alignItems(eui::Align::CENTER)
        .gap(kInnerGap).content([&] {
            ui.text(barId + ".label")
                .size(kWinLabelW, 20.0f)
                .text(w.label)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(20.0f)
                .color(t.colors.textSecondary)
                .build();

            components::ProgressStyle ps;
            const auto ct = core::theme::componentTokens(t);
            ps.track = ct.surfaceHover;
            ps.fill = tintColor(tintForUsedPercent(w.usedPercent), t);
            components::progress(ui, barId)
                .size(barW, kBarH)
                .value(static_cast<float>(w.usedPercent / 100.0))
                .style(ps)
                .build();

            ui.text(barId + ".pct")
                .size(kPctW, 20.0f)
                .text(formatQuotaPercent(w.usedPercent))
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(tintColor(tintForUsedPercent(w.usedPercent), t))
                .build();

            if (hasReset) {
                ui.text(barId + ".reset")
                    .size(kResetW, 20.0f)
                    .text(countdown)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textTertiary)
                    .build();
            }
        })
        .build();
}

}  // namespace

void drawQuotaCard(eui::Ui& ui, const core::theme::Tokens& t, float contentWidth,
                   const std::vector<QuotaHomeRow>& rows, const Hooks& hooks,
                   float maxBodyHeight) {
    const QuotaCardMetrics m =
        computeQuotaCardMetrics(rows, contentWidth, maxBodyHeight);
    const int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    // 组件 token 单一来源（2026-10-03 两按钮对齐修复）：卡壳 / 卡头行滚动
    // 缩进 / scrollView 三处共用一份——「卡头行按滚动缩进」与「scrollView
    // 缩内容列」必须读同一对常量，分开各取一份就是漂移入口。
    const auto ct = core::theme::componentTokens(t);

    components::card(ui, "home.quota")
        .size(contentWidth, m.cardHeight)
        .theme(ct)
        .padding(kPad)
        .content([&] {
            const float cw = contentWidth - kPad * 2.0f;
            // ---- 标题行 + [刷新] ----
            // ⚠ `.gap(kInnerGap)` 必须在（2026-10-03 按钮不贴右修复）：title
            // 宽按 cw−104 预留（= 按钮 96 + 间距 8），行不挂 gap 时 title 与
            // 按钮零间距、[刷新] 右缘恒比内容列右缘短 8px（探针组 J(b) 实测
            // 1136 vs 1144）。
            // ⚠ 卡头行宽必须与滚动内容列**同宽**（2026-10-03「一个前一个后」
            // 修复）：scrollView 在内容溢出时把内容列缩成
            // width−(scrollbar+gap)（EUI-NEO scrollview.h:74-76，默认 metrics
            // 8+16=24px），行 head 连同 [去配置] 右缘跟着缩到 cw−24；[刷新]
            // 不在 scrollView 里，卡头行仍按 cw 排的话 [刷新] 恒比 [去配置]
            // 突出 24px（真机点验报告原话「一个前一个后」；探针组 L 滚动档
            // 实测 1144 vs 1120）。m.overflows 与 scrollView 的 scrollable
            // 同判据（内容高 > 视口高，组 H(a) 钉两者一致）；inset 取自与
            // scrollView 同一份 ct.metrics，两侧永不漂移。
            const float sbInset =
                m.overflows
                    ? ct.metrics.control.scrollbar + ct.metrics.spacing.section
                    : 0.0f;
            const float headW = cw - sbInset;
            ui.row("home.quota.head").size(headW, kTitleH)
                .alignItems(eui::Align::CENTER).gap(kInnerGap).content([&] {
                    ui.text("home.quota.title")
                        .size(headW - 104.0f, 24.0f)
                        .text("供应商余量")
                        .fontSize(t.sizes.fontSizeLarge - 1.0f)
                        .lineHeight(24.0f)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    // ⚠ **按值捕获**（与下面的 openConfig 完全同款：compose 栈上的临时
                    // `hooks` 不会被延活到 onClick 触发那一刻）。
                    // 这一处的危险性不是「std::function 拷贝其实也安全」——
                    // 真正的问题是：`Hooks` 一旦换成捕获 `this` 的 lambda
                    // 变体，按引用捕获就是**真悬垂**、按下即崩，而这一行
                    // 看上去与「传引用」毫无区别。写成 `auto refreshAll =
                    // hooks.refreshAll` 是**显式意图**：此处要的是快照。
                    auto refreshAll = hooks.refreshAll;
                    components::button(ui, "home.quota.refresh")
                        .size(96.0f, 28.0f)
                        .text("刷新")
                        .icon(0xF021)   // fa-arrows-rotate
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.surfaceElevated, t.colors.hover,
                                t.colors.active)
                        .textColor(t.colors.textSecondary)
                        .iconColor(t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .onClick(refreshAll)
                        .build();
                })
                .build();

            // ---- 空态 / 读库失败 ----
            // 两者**分开**：查询失败说成「没配」是静默失败面（用户会去检查
            // 配置，而问题在库）。
            if (rows.empty()) {
                ui.text("home.quota.empty")
                    .size(cw, 40.0f)
                    .text(hooks.loadError.empty()
                              ? std::string("暂无 Claude Profile")
                              : ("余量加载失败：" + hooks.loadError))
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(40.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(hooks.loadError.empty() ? t.colors.textTertiary
                                                    : t.colors.error)
                    .build();
                return;
            }

            components::scrollView(ui, "home.quota.scroll")
                .position(0.0f, kTitleH + kTitleGap)
                .size(cw, m.bodyHeight)
                .gap(kRowGap)
                .theme(ct)
                .content([&](eui::Ui& inner, float innerW, float) {
                    for (const QuotaHomeRow& row : rows) {
                        const std::string rowId =
                            "home.quota.row." + std::to_string(row.profileId);
                        const bool showBar = showsQuotaBar(row.state);
                        const bool showErr = showsErrorLine(row.state);
                        const bool showCfg = showsConfigButton(row.state);
                        const bool showStale =
                            row.state == QuotaCardState::FailedStale;

                        inner.column(rowId)
                            .size(innerW, quotaRowHeight(row))
                            .content([&] {
                                // ---- 行首：名称 + 供应商 + 状态 + 去配置 ----
                                const float nameW =
                                    std::min(kNameW, innerW * 0.24f);
                                const float staleW = showStale ? kStaleW : 0.0f;
                                const float cfgW = showCfg ? kConfigBtnW : 0.0f;
                                // gap 插在**每两个相邻子元素之间**，而 head 的子
                                // 元素恒为 name / provider / [stale] / status /
                                // [cfg] —— 间隙数 = 子元素数 - 1，**不是**「有几
                                // 个可选徽标」。旧算法只数 stale/cfg 两处，恒少算
                                // name|provider 与 provider|stale 两处，statusW
                                // 每次多出 2×kInnerGap = 16px，于是 .status 的
                                // 右缘**恒**越出内容列右缘 16px（探针组 I(b) 实测
                                // 1136 vs 1120）。这里按子元素数算，钉死。
                                const int headChildCount =
                                    3 + (staleW > 0.0f ? 1 : 0) +
                                    (cfgW > 0.0f ? 1 : 0);
                                const float gaps =
                                    kInnerGap * static_cast<float>(headChildCount - 1);
                                const float statusW = std::max(
                                    40.0f,
                                    innerW - nameW - kProviderW - staleW - cfgW -
                                        gaps);

                                inner.row(rowId + ".head")
                                    .size(innerW, kHeadH)
                                    .alignItems(eui::Align::CENTER)
                                    .gap(kInnerGap)
                                    .content([&] {
                                        // ⚠ .name/.provider 与 .status 同款
                                        // 防溢护栏（2026-10-03 文本重叠修复）
                                        // + 省略号升级（同日 Ellipsis 批次，
                                        // 4e76052 的 .error 先例同款）：
                                        // 两格都是固定宽单行盒且不能 wrap
                                        // （换行会撑破 quotaRowHeight 的高度
                                        // 预算），而 profile 名（cc-switch 条目
                                        // 名）与供应商显示名的长度都不受控——
                                        // 长 profile 名的字形曾直接画出 168px
                                        // 盒、压进 provider 列。护栏分层：
                                        // **ellipsis 管排版宽**（截断有「…」
                                        // 提示，用户看得出名字被截——显式
                                        // maxWidth 是生效前提，漏了框架侧静默
                                        // no-op），**clip 管绘制范围**（scissor
                                        // 兜底，ellipsis 压不住的极窄容器情形
                                        // 由它收口），两层不冲突。
                                        inner.text(rowId + ".name")
                                            .size(nameW, kHeadH)
                                            .text(row.profileName)
                                            .fontSize(t.sizes.fontSizeBody)
                                            .lineHeight(kHeadH)
                                            .fontWeight(600)
                                            .color(t.colors.textPrimary)
                                            .maxWidth(nameW)
                                            .overflow(
                                                core::TextOverflow::Ellipsis)
                                            .clip()
                                            .build();
                                        inner.text(rowId + ".provider")
                                            .size(kProviderW, kHeadH)
                                            .text(row.providerDisplayName)
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .lineHeight(kHeadH)
                                            .color(t.colors.textSecondary)
                                            .maxWidth(kProviderW)
                                            .overflow(
                                                core::TextOverflow::Ellipsis)
                                            .clip()
                                            .build();
                                        if (showStale) {
                                            inner.text(rowId + ".stale")
                                                .size(staleW, 18.0f)
                                                .text("可能过期")
                                                .fontSize(
                                                    t.sizes.fontSizeSmall)
                                                .lineHeight(18.0f)
                                                .horizontalAlign(
                                                    eui::HorizontalAlign::
                                                        Center)
                                                .color(t.colors.warning)
                                                .build();
                                        }
                                        inner.text(rowId + ".status")
                                            .size(statusW, kHeadH)
                                            .text(statusTextFor(row))
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .lineHeight(kHeadH)
                                            // **单行盒不许把字画出盒**：EUI-NEO
                                            // 现语义（dsl.h:150-152）是 Ellipsis
                                            // 需 wrap==false 且 maxWidth>0——
                                            // maxWidth 并非只在 wrap=true 生效
                                            // （旧注释失实）。本格是固定高单行、
                                            // 不能换行（换行会把 kHeadH 撑破、
                                            // 与 quotaRowHeight 的高度预算脱钩），
                                            // 且状态标签是有界枚举文案，故
                                            // Ellipsis 批（8df78dc）按「内容受控」
                                            // 判据保留本格 clip 剪裁。
                                            .clip()
                                            .color(showErr ? t.colors.error
                                                           : t.colors
                                                                 .textTertiary)
                                            .build();
                                        if (showCfg) {
                                            // ⚠ **按值捕获**：onClick 在
                                            // compose 返回之后才被触发，而
                                            // `hooks` 是 compose 栈上的临时
                                            // 对象——按引用捕获就是悬垂引用，
                                            // 点一下直接崩（真实踩过：
                                            // test_home_quota_config_button_
                                            // opens_g6_dialog 段错误）。
                                            auto openConfig = hooks.openConfig;
                                            const int64_t profileId =
                                                row.profileId;
                                            // ⚠ 按钮高 = kHeadH（2026-10-03
                                            // 按钮错位修复）：旧 26 高装在 24
                                            // 的 head 行里，alignItems CENTER
                                            // 后上下各溢出 head frame 1px
                                            // （探针组 J(b) 实测 button y=349
                                            // < head y=350）。
                                            components::button(
                                                inner, rowId + ".config")
                                                .size(cfgW, kHeadH)
                                                .text("去配置")
                                                .fontSize(
                                                    t.sizes.fontSizeSmall)
                                                .colors(
                                                    t.colors.surfaceElevated,
                                                    t.colors.hover,
                                                    t.colors.active)
                                                .textColor(
                                                    t.colors.primaryText)
                                                .radius(t.sizes.radius)
                                                .border(
                                                    1.0f,
                                                    t.colors.borderDefault)
                                                .onClick([openConfig,
                                                          profileId] {
                                                    if (openConfig) {
                                                        openConfig(profileId);
                                                    }
                                                })
                                                .build();
                                        }
                                    })
                                    .build();

                                // ---- 数据区（**唯一**画条的地方）----
                                if (showBar) {
                                    if (row.kind == QuotaHomeKind::Balance) {
                                        inner.row(rowId + ".balance")
                                            .size(innerW, kBalRowH)
                                            .alignItems(eui::Align::CENTER)
                                            .content([&] {
                                                inner.text(
                                                    rowId + ".balance.amount")
                                                    .size(innerW * 0.6f, 24.0f)
                                                    .text(formatQuotaBalance(
                                                        row.balanceRemaining,
                                                        row.currency))
                                                    .fontSize(
                                                        t.sizes.fontSizeBody)
                                                    .lineHeight(24.0f)
                                                    .fontWeight(600)
                                                    .color(t.colors
                                                               .textPrimary)
                                                    .build();
                                                if (!row.planName.empty()) {
                                                    inner.text(rowId +
                                                               ".balance.plan")
                                                        .size(
                                                            innerW * 0.4f,
                                                            20.0f)
                                                        .text(row.planName)
                                                        .fontSize(
                                                            t.sizes
                                                                .fontSizeSmall)
                                                        .lineHeight(20.0f)
                                                        .horizontalAlign(
                                                            eui::HorizontalAlign::
                                                                Right)
                                                        .color(t.colors
                                                                   .textTertiary)
                                                        .build();
                                                }
                                            })
                                            .build();
                                    } else {
                                        for (const QuotaHomeWindow& w :
                                             row.windows) {
                                            drawWindowRow(inner, t, innerW,
                                                          rowId, w, nowSec);
                                        }
                                    }
                                }

                                // ---- 错误行（错误文案的**唯一**出处）----
                                if (showErr) {
                                    inner.text(rowId + ".error")
                                        .size(innerW, kErrRowH)
                                        .text(row.lastError)
                                        .fontSize(t.sizes.fontSizeSmall)
                                        .lineHeight(kErrRowH)
                                        // **省略号是单行语义**：只有
                                        // wrap==false 且 maxWidth>0 才生效，而
                                        // 这一格正是固定高单行（换行会把
                                        // kErrRowH 撑破、与 quotaRowHeight 的高度
                                        // 预算脱钩）→ 显式给 maxWidth 是**生效
                                        // 前提**，漏了它 ellipsis 静默不生效
                                        // （runtime 在 element.maxWidth==0 时回退到
                                        // frame.width，但 DSL 布局走的是
                                        // element.maxWidth，两条路会分叉）。
                                        // 口径 = innerW = 内容列宽 = 该行右缘，
                                        // 与探针组 I(b) 的「内容列右缘」同一口径。
                                        .maxWidth(innerW)
                                        .overflow(core::TextOverflow::Ellipsis)
                                        // 兜底 scissor：ellipsis 把**排版宽**压到
                                        // maxWidth 内，但框架文档化了它压不住的
                                        // 那种情形——省略号自身 advance > maxWidth
                                        // 的极窄容器（本卡 innerW ≈ 1064px，
                                        // 不可能触发）。留着它，「绝不画出内容列」
                                        // 这条不变式由**两层**（排版 + 裁剪）保证，
                                        // 而不是单押在 ellipsis 上。
                                        // 探针组 I(c) 断言的是「… 是 ellipsis 产生的」
                                        // ——它与 clip 无关，所以 clip 不是遮羞布。
                                        .clip()
                                        .color(t.colors.error)
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

}  // namespace module::home::quota_card